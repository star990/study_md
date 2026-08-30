# Android / SoC 调度工程：从 CFS (Completely Fair Scheduler)/EAS (Energy Aware Scheduling) 到厂商 Hint

> **系列**：`01-kernel`  
> **前置**：[`01-scheduling.md`](01-scheduling.md)  
> **相关**：[`../05-bus-rtl/01-spinlock-to-bus.md`](../05-bus-rtl/01-spinlock-to-bus.md)

手机与 SoC 产品侧如何把调度跑起来：EAS、WALT (Window-Assisted Load Tracking)、cpuset/uclamp、PerfLock/Hint、Loading 优化与现场抓数；Part B 再把同步原语接到一致性与互联（细节总线机制请看 spinlock 专文）。

**读完应能**：
- 说明 EAS/WALT 在选核中的角色
- 设计一套 Loading 窗口的 hint 组合并带约束
- 从 systrace 判断是调度问题还是别的瓶颈

---
## 1. 总览：手机上的调度栈

```
┌─────────────────────────────────────────────────────────────┐
│  App / Game / Unity·Unreal 线程                              │
├─────────────────────────────────────────────────────────────┤
│  Android Framework：AMS、ProcessList、adj、AppStandby        │
├─────────────────────────────────────────────────────────────┤
│  厂商 HAL：PerfLock / GameMode / FPSGo / Hint 引擎           │
├─────────────────────────────────────────────────────────────┤
│  cgroup v1/v2：cpuset、cpu、schedtune/uclamp、blkio          │
├─────────────────────────────────────────────────────────────┤
│  Kernel：CFS + EAS 选核 + SCHED_FIFO(少量)                   │
├─────────────────────────────────────────────────────────────┤
│  CPUFreq：schedutil + OPP；CPUIdle；thermal throttle         │
├─────────────────────────────────────────────────────────────┤
│  互联：DDR/LLC/GPU DVFS、互联带宽 vote（常与 Perf 一起抬）    │
└─────────────────────────────────────────────────────────────┘
```

**工程结论**：手机卡顿/耗电很少是「CFS 算错 vruntime」 alone，而是 **选核 + 升频滞后 + cpuset 关错笼子 + 热墙 + IO** 叠加。优化也多在这些层动手，而不是改调度器算法本身。

---

## 2. Kernel 底座：CFS / RT / EAS

### 2.1 CFS（日常 App 的默认策略）

- 按 **vruntime** 公平分享 CPU；`nice` 调权重。
- 手机上几乎所有游戏逻辑/渲染线程仍是 **SCHED_NORMAL（CFS）**。
- 工程上更常动的是：**nice、cgroup、uclamp、绑核**，而不是改成 FIFO。

```bash
# 看线程调度策略与 RT 优先级
ps -eLo pid,tid,class,rtprio,ni,pri,psr,comm | head
chrt -p <tid>          # 查看
renice -n -5 -p <pid>  # 慎用；系统权限/厂商策略可能覆盖
```

### 2.2 SCHED_FIFO / RR（少量关键路径）

| 用途 | 典型 |
|------|------|
| 音频 | `audioserver`、DSP 相关线程 |
| 显示/合成 | 部分 `surfaceflinger`、HWC 路径（视平台） |
| 传感器/低延迟 | 厂商自定义 |

```bash
# 设 FIFO 90（需权限；量产一般由系统组件设置）
chrt -f -p 90 <tid>
```

**工程注意**：游戏主线程乱加 FIFO 容易饿死系统线程 → 卡死/看门狗；手机上优先用 **boost + cpuset**，少用 FIFO。

### 2.3 EAS（Energy Aware Scheduling）—— 手机选核核心

异构核（Little / Mid / Big / Prime）下，wake/select 时综合考虑：

1. 任务 utilization  
2. CPU capacity（常归一化到 1024）  
3. Energy Model（DT 里的功耗表）  
4. 选「能跑完且更省」或「boost 时更偏性能」的 CPU  

```
轻任务 → Little
突发重载 → Mid/Big
已饱和的大核 → 避免继续硬塞（视策略）
```

**工程相关文件/接口（因内核树而异）**：

```bash
# Energy Model / capacity（示意路径）
cat /sys/devices/system/cpu/cpu*/cpu_capacity
# 部分树：
ls /sys/kernel/debug/sched*
# Android 常见：sched_features、walt 相关 debug
```

### 2.4 迁移代价（工程直觉）

| 现象 | 原因 | 手段 |
|------|------|------|
| 滑动偶发卡一下 | 任务在 little 睡醒，升频/迁核慢 | boost、prefer big、抬 minfreq |
| 多线程互相抢大核 | 无差别上大核 | 关键线程白名单绑核 |
| 耗电飙 | 轻活长期占 big | 后台进 background cpuset |

---

## 3. 调频与负载信号：schedutil / PELT / WALT

### 3.1 schedutil

调度器利用率 ↑ → schedutil 请求更高 OPP → CPUFreq 升频。

```bash
# 当前 governor
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
# 常为 schedutil

# 频率与限幅（路径因 SoC 略不同）
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
```

**工程实践**：

- Loading 时抬 **min_freq**（地板），避免从最低频爬坡。  
- 稳态游戏靠 util 自然升降；不要长期 `performance` governor（烫、掉帧二次崩）。  
- 看 **升频延迟**：用 ftrace/perf 或厂商 systrace 看 `cpu_frequency` 与 runnable 的时间差。

### 3.2 PELT vs WALT

| | PELT | WALT |
|--|------|------|
| 含义 | Per-Entity Load Tracking，指数衰减 | Window Assisted，窗口统计 |
| 手感 | 更平滑 | 更跟手、偏激进 |
| 常见 | 主线/部分 OEM | 高通等 Android 树常见 |

**工程含义**：同样「突然重载」，WALT 往往更快推高频/上大核；调参时要和 thermal 一起看，否则容易「跟手但易烫」。

### 3.3 利用率怎么看（工程）

```bash
# 粗看各核忙碌（用户态）
top -H -p <game_pid>
# 或
cat /proc/sched_debug          # 视内核是否开启
# systrace / perfetto：CPU 轨道 + freq 轨道最直观
```

Perfetto/systrace 工程清单：

- `sched`：wakeup、switch、blocked  
- `cpu_frequency`、`cpu_idle`  
- `thermal`  
- 应用帧：`Choreographer`、GFX、游戏自己的 trace point  

---

## 4. Android 框架与 cgroup 实践

### 4.1 cpuset：把进程关进核集合

典型（名字因版本/厂商而异）：

```text
/dev/cpuset/
  top-app/          当前顶层（游戏）
  foreground/
  background/       常限 little
  system-background/
  restricted/
```

```bash
# adb shell（需 root 或 debug 机）
cat /dev/cpuset/top-app/cpus
cat /dev/cpuset/background/cpus
# 看进程在哪个 cpuset
cat /proc/<pid>/cpuset
```

**工程实践**：

| 场景 | 做法 |
|------|------|
| 省电 | 后台服务、同步进 `background`，cpus 仅 little |
| 前台跟手 | 进 `top-app`，cpus 含 mid+big |
| Loading | 确保游戏进程在 top-app；必要时临时扩大 cpus |
| 防干扰 | 下载/扫毒线程强制 background |

### 4.2 cpu cgroup / shares（老）与 uclamp（新）

**目标**：给调度器「最低也要这么猛 / 最高别超过」的暗示。

```bash
# uclamp（若内核支持，路径示意）
cat /proc/<pid>/status | grep -i clamp
# 或 cgroup v2 cpu.uclamp.min / max
```

| 旋钮 | Loading | 大厅挂机 |
|------|---------|----------|
| uclamp.min / boost | 拉高 | 降回 0 |
| uclamp.max | 放开 | 可压低控温 |

老树 **schedtune**：

```bash
# 示意（高通等老接口）
ls /dev/stune/
# top-app、foreground、background 等组的 schedtune.boost
cat /dev/stune/top-app/schedtune.boost
# 0~100；Loading 时厂商常动态写成高值
```

### 4.3 进程状态与 adj（框架侧）

AMS 根据 Activity 可见性等调整进程重要性 → 影响 cgroup 归属与杀进程优先级。

工程相关：

```bash
adb shell dumpsys activity processes | grep -A2 <pkg>
adb shell cat /proc/<pid>/oom_score_adj
```

游戏进后台：可能被降 cpuset → 回来时若 boost 没跟上，会「切回前台卡一下」。厂商常对游戏白名单做 **快速恢复 boost**。

### 4.4 绑核（affinity）工程用法

```bash
# 看线程在哪个 CPU
ps -eLo pid,tid,psr,comm | grep -i <name>
# 绑核（示例：tid 绑到 cpu 4-7）
taskset -p <mask> <tid>
```

**推荐模式（游戏）**：

| 线程类型 | 建议 |
|----------|------|
| 主逻辑 / RenderThread | mid/big，相对固定，减少迁移 |
| Job/Worker 池 | 多核并行；Loading 时提高并发 |
| 音频 | 高优先级，避免和大负载挤同一大核到饿死 |
| 后台下载 | little only |

**反模式**：所有线程 `taskset` 到同一颗 Prime → 单核过热、其它核空转。

---

## 5. 厂商加速层：PerfLock / GameMode / Hint

Kernel 不知道「正在 Loading」。工程上由 **用户态发 hint**：

```
Game / SDK / 厂商 GameSpace
    → Perf HAL / Hint 引擎
        → 写 sysfs / ioctl / vendor node
            → min_freq、uclamp、ddr_vote、gpu_min、sched_boost…
```

### 5.1 常见 hint 类型（概念名，各家不同）

| Hint | 意图 |
|------|------|
| `INTERACTION` / 触控 | 短时跟手（100–300ms） |
| `LAUNCH` | 冷热启动，几秒内性能优先 |
| `LOADING` / `GAME_LOADING` | 读包+解压，CPU/DDR 拉高 |
| `GAME` / `FPS` | 稳态帧率，平衡热 |
| `SUSTAINED` | 长时间性能，偏热管理 |

### 5.2 Loading Hint 窗口内「一套组合拳」（工程模板）

```text
进入 Loading（示例窗口 2–5s，可续期）:
  [CPU]  min_freq 抬地板；boost/uclamp.min↑；cpus 放开到 big
  [DDR]  带宽 vote↑（解压、贴图、DMA）
  [GPU]  若在建 PSO/上传资源，抬 gpu min；纯 CPU 解压可不动 GPU
  [IO]   UFS 性能模式；提高读写进程 ionice；减少后台写
  [干扰] background 限 little；暂停非必要 sync/GC（应用侧）
  [热]   设温度阈值；超阈值提前撤 boost

退出 Loading / 超时 / 过热:
  撤销全部 vote，平滑回 FPS 策略（避免断崖降频）
```

### 5.3 必须带的工程约束

1. **超时**：防止 hint 泄漏导致一直大火。  
2. **可续期**：Loading 超过 5s 可续，但累计有上限。  
3. **热熔断**：皮肤温度/结温到线立即回落。  
4. **成对 acquire/release**：类似锁；泄漏是耗电第一杀手。  
5. **与显示刷新率策略解耦**：Loading 黑屏时不必强行 120Hz。

---

## 6. 游戏与重 Loading：工程优化清单

### 6.1 先定性瓶颈（否则调度白调）

| 症状 | 更可能瓶颈 | 先查 |
|------|------------|------|
| Loading 条慢，CPU 低 | IO/存储 | UFS util、`iowait` |
| CPU 接近 100%，小核满大核空 | 选核/cpuset | cpuset、boost |
| 大核满、频上不去 | 热墙/上限 | thermal、max_freq |
| GPU 忙、CPU 闲 | 渲染/驱动 | GPU freq、带宽 |
| 多核都忙但进度慢 | 锁竞争/单线程关键路径 | Systrace 找阻塞 |

```bash
# iowait 粗看
adb shell top
# 或 dumpsys，厂商 storage 节点
```

### 6.2 Loading 优化分层

**A. 系统侧（OEM）**

1. Loading 场景识别（SDK 回调优先于启发式）。  
2. 组合 vote：CPU min + DDR +（可选）GPU。  
3. top-app 保证；限制后台抢核抢 IO。  
4. 关键线程白名单（主线程/Render）优先大核，worker 用剩余核。  
5. Loading 结束 **延迟 100–200ms 再撤** boost，避免进场景第一帧卡顿。

**B. 应用侧（游戏）**

1. 异步加载 + 线程池；避免主线程同步读盘。  
2. 压缩格式与解压算法选型（CPU vs 体积）。  
3. 预编译/缓存 PSO、shader；减少首次卡顿。  
4. Loading 期间暂停非必要系统调用、日志、分析 SDK。  
5. 与厂商合作：调用官方 Game Loading API（若有）。

**C. 内核/驱动侧**

1. 减少升频滞后（WALT 窗口、schedutil rate limit）。  
2. 中断亲和：重负载时勿让无关 IRQ (Interrupt Request) 打满大核。  
3. 热策略：Loading 允许短时更高功耗曲线（power budget）。

### 6.3 稳态对局（对比 Loading）

| | Loading | 对局中 |
|--|---------|--------|
| 目标 | 尽快结束黑屏 | 稳帧 + 控温 |
| CPU | 短时拉满 | 跟帧，避免过冲 |
| GPU | 按需 | 常为主瓶颈 |
| Boost | 强、短 | 中等、可 sustained |
| 成功指标 | Loading 时长 P95 | 掉帧率、卡顿次数、壳温 |

### 6.4 性能–功耗平衡：用状态机，不用固定档

```text
        功耗
         ▲
         │    ★ Loading（短时允许）
         │         \
         │          ★ 激战（热墙裁剪）
         │               \
         │                ★ 大厅
         └────────────────────▶ 时间

实现：enum { IDLE, LOADING, BATTLE, THERMAL_CAP }
各状态一张「vote 表」，切换时 apply/rollback。
```

工程上把策略做成 **表驱动**，比散落 if-else 好维护，也方便 A/B。

---

## 7. 现场排查与调参手册

### 7.1 标准抓数包（建议固定脚本）

```bash
# 1) 基本信息
uname -a
getprop ro.build.fingerprint
cat /sys/devices/system/cpu/present
cat /sys/devices/system/cpu/cpu*/cpu_capacity

# 2) 进程与 cpuset
pid=$(pidof <game>)
cat /proc/$pid/cpuset
ps -eLo pid,tid,class,rtprio,ni,psr,comm -p $pid

# 3) 频率与热
for c in /sys/devices/system/cpu/cpu*/cpufreq; do
  echo -n "$c "; cat $c/scaling_cur_freq 2>/dev/null
done
cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | head

# 4) Perfetto / systrace（主机侧）
# 录 10–20s，覆盖「点进 Loading → 结束」完整过程
```

### 7.2 看 Systrace/Perfetto 的顺序（工程）

1. **CPU 轨道**：Loading 时是否长期趴在 little？  
2. **freq 轨道**：util 起来后多久才升频？  
3. **主线程**：是 runnable 等 CPU，还是 sleep 等 IO？  
4. **热**：是否中途 throttle？  
5. **其它包**：是否有后台线程抢大核？

### 7.3 常用调参旋钮清单（改前先备份）

| 旋钮 | 作用 | 风险 |
|------|------|------|
| cpuset.cpus | 能用哪些核 | 关太死 → 卡；放太开 → 耗电 |
| schedtune.boost / uclamp.min | 性能偏向 | 泄漏 → 烫 |
| scaling_min_freq | 升频地板 | 过高 → 耗电发热 |
| scaling_max_freq | 上限 | 热墙已限时无效 |
| 线程 affinity | 降迁移 | 绑死 → 负载不均 |
| ionice / blkio | IO 优先级 | 可能饿后台写 |
| DDR/GPU vote | 带宽与 GPU | 功耗、热 |

### 7.4 对比实验方法（避免玄学）

1. 同一关卡、同一温度起点（关机冷却或固定壳温）。  
2. 只改一个旋钮。  
3. 指标：Loading 时长、平均功耗（若可测）、壳温、进场景后 30s 掉帧。  
4. 重复 ≥3 次取 P50/P95。

### 7.5 延迟类工具（Linux/RT 侧，板子上）

```bash
# 若有 cyclictest（偏 RT 内核；手机一般不用）
cyclictest -m -S -p 90 -i 200 -l 100000

# ftrace 调度延迟
echo 1 > /sys/kernel/tracing/events/sched/sched_switch/enable
# ... 抓取后分析 wakeup latency
```

手机更常用 **Perfetto + 厂商自己的 FPS/卡顿指标**。

---

## 8. 常见坑与反模式

| 坑 | 后果 | 正确做法 |
|----|------|----------|
| Hint acquire 不 release | 一直大火、耗电投诉 | 成对 API + 超时守护线程 |
| Loading 永久 performance | 进游戏就过热降频 | 短窗口 + 热回落 |
| 全线程绑同一大核 | 单核烫、并行变差 | 主线程大核，worker 分散 |
| 只调 CPU 不调 DDR | 解压仍慢 | CPU+DDR（+存储）组合 |
| 只看平均帧率 | 掩盖尖峰卡顿 | 看 jank、P95/P99 帧时 |
| 后台杀光导致回来冷启动 | 体验更差 | 游戏进程保活策略单独做 |
| 用 FIFO 提游戏主线程 | 系统线程饿死 | CFS + boost |
| 忽视 iowait | 调度调半天无用 | 先分清 CPU vs IO |

---

## 9. 嵌入式 / 机器人对照

| 维度 | Android 手机 | 机器人 / 车规 MCU·SoC |
|------|--------------|------------------------|
| 默认调度 | CFS + EAS | RTOS (Real-Time Operating System) 或 Linux PREEMPT_RT (Preemptible Real-Time) |
| 「加速」手段 | Hint、uclamp、cpuset | SCHED_FIFO、isolcpus、IRQ 线程优先级 |
| 突发负载 | Loading boost 2–5s | 控制周期硬期限，不靠短时赌运 |
| 能效 | EAS + 热墙 | 功耗+实时+功能安全 |
| 隔离 | cgroup；App 沙箱 | PMP (Physical Memory Protection)/MMU (Memory Management Unit)；AMP (Asymmetric Multi-Processing) 核间隔离 |

**迁移要点**：

> 手机用 EAS 做常态能效，用场景 Hint 做 Loading 突发；机器人控制环用 RT 优先级和隔离核做确定性。都是「调度器 + 约束/暗示 + DVFS」，约束来源不同。

---

---

# Part B · SoC 原厂深度：同步 / Monitor / 总线 / Cache

> 目标：能从 **调度里的一把 spinlock** 讲到 **LDXR/STXR → Local/Global Monitor → AXI (Advanced eXtensible Interface) Exclusive → ACE/CHI Snoop → RTL**。  
> 这是芯片公司 BSP/体系结构与 bring-up 的分水岭。

---


> **阅读提示（Part B）**：Exclusive / Monitor / `HLOCK` 专文见 [`../05-bus-rtl/01-spinlock-to-bus.md`](../05-bus-rtl/01-spinlock-to-bus.md)。下文侧重调度语境下的同步/一致性地图与 ACE/CHI、屏障、AMP，与专文互补。

## 12. 为什么谈调度必须谈 spinlock

调度器热路径里到处是同步：

| 场景 | 典型锁 | 为何不能睡 |
|------|--------|------------|
| `rq->lock`（每 CPU runqueue） | `raw_spinlock` | 持锁时可能关抢占/关中断，不能 schedule |
| 跨 CPU 操作 runqueue | 双持两把 rq lock | SMP (Symmetric Multi-Processing) 原子性靠硬件 exclusive |
| 中断关上下文 | `raw_spin_lock_irqsave` | hardirq / softirq 路径 |
| PREEMPT_RT | 多数 spin → sleeping lock | 仍保留 raw_spin 碰硬件瞬间 |

**原厂视角**：软件「原子」最终落在：

1. **单指令原子**（`LDADD` / `AMO` 等），或  
2. **Load-Exclusive / Store-Exclusive 对** + **Monitor 状态机** + **一致性互联的窥探/失效**。

不懂 Monitor/总线，就无法回答：「多核 spinlock 重试时总线上发生了什么？」「锁变量放 Device 行不行？」「DMA (Direct Memory Access) 会不会破坏 exclusive？」

---

## 13. 全链路地图：App → Kernel → ISA → 总线 → RTL

```
[App]
  pthread_mutex / 无锁算法
       │（多数不进内核；争用严重才 futex）
       ▼
[Kernel 调度 / 驱动]
  spin_lock(&rq->lock)
  arch_spin_lock() / qspinlock
       │
       ▼
[ISA]  ARM64 例
  PRFM + LDXR + 比较 + STXR + CBNZ 重试 + DMB
  失败可选 WFE；解锁 STLR/STR + DSB + SEV
       │
       ▼
[CPU / L1]
  Exclusive Load → Local Monitor 标记 (地址/粒度)
  Store Exclusive → 查 Local → 发一致性/总线事务
       │
       ▼
[ACE / CHI / CCI / DSU / CMN]
  Exclusive 属性事务
  Global Monitor（或互联内嵌）仲裁多 master
  Snoop：保证其它核 cache line 与锁语义一致
       │
       ▼
[AXI 通道]
  ARLOCK/AWLOCK = Exclusive（AXI4 概念）
  成功：EXOKAY；独占失败：OKAY（且不得提交写）
       │
       ▼
[RTL]
  Monitor CAM：{MasterID, Addr, Size, Valid}
  其它 master 写同地址 → 清 Valid → 下次 STXR 失败
  与 DCache / Snoop Filter / Directory 状态机交互
```

**一句话**：软件看到的「STXR 失败重试」= 硬件 Local/Global Monitor 认为独占标记已丢。

---

## 14. Spinlock：C 到汇编（ARM64 / RISC-V）

### 14.1 软件层（Linux）

现代 ARM64 常用 **qspinlock**（排队），底层仍是 `atomic` / `ldxr+stxr` 或 **LSE** 原子指令。

教学用 test-and-set：

```c
typedef struct { uint32_t locked; } arch_spinlock_t;

void arch_spin_lock(arch_spinlock_t *lock)
{
    while (cmpxchg_acquire(&lock->locked, 0, 1) != 0)
        cpu_relax();   /* yield / wfe：降总线风暴 */
}

void arch_spin_unlock(arch_spinlock_t *lock)
{
    smp_store_release(&lock->locked, 0);
}
```

### 14.2 ARM64：无 LSE 时的 LDXR/STXR

```asm
; W0=1, X1=&lock->locked
1:  ldxr    w2, [x1]        ; Load Exclusive → 置 Local Monitor
    cbnz    w2, 2f          ; 已锁
    stxr    w3, w0, [x1]    ; 成功 W3=0；失败 W3≠0
    cbnz    w3, 1b          ; monitor 丢或被抢 → 重试
    dmb     ish             ; acquire：锁内访问不重排到获锁前
    ret
2:  wfe                     ; 可选，等 SEV
    b       1b

; unlock
    stlr    wzr, [x1]       ; store-release；或 dmb+str
    dsb     ish
    sev                     ; 踢醒其它核 WFE
    ret
```

| 指令 | 作用 |
|------|------|
| `LDXR` / `LDAXR` | 读 + Local Monitor 标记 |
| `STXR` / `STLXR` | monitor 有效才写；返回成功/失败 |
| `CLREX` | 软件清 exclusive（切换等路径） |
| `WFE` / `SEV` | 降自旋功耗与互联压力 |
| `DMB ISH` | Inner Shareable 屏障 |

**LSE** 时可用 `swpal` / `cas` / `ldadd` 等，少了软件重试环，但仍依赖一致性域内的原子语义。

### 14.3 RISC-V：LR/SC 与 AMO (Atomic Memory Operation)

```asm
1:  lr.w    t0, (a0)
    bnez    t0, 1b
    li      t1, 1
    sc.w    t2, t1, (a0)    ; t2=0 成功
    bnez    t2, 1b
    fence   rw, rw
    ret
```

| ARM64 | RISC-V |
|-------|--------|
| LDXR/STXR | **LR/SC** |
| LSE AMO | **AMO**（amoswap/amoadd…） |
| Local/Global Monitor | Reservation + 互连实现 |
| DMB/DSB | fence |

单核 MCU：常靠核内 reservation；若 CPU+DMA 多 master 共享 SRAM (Static Random-Access Memory)，仍需总线互斥或禁止 DMA 写锁地址。

### 14.4 `cpu_relax` 与总线风暴

无 pause/WFE 的紧自旋：多核同时锤同一行 → **Snoop/Exclusive 流量打满** → 全体变慢。原厂性能分析常看：锁地址 EXFAIL 比例、snoop 命中、互联带宽。

---

## 15. Exclusive Monitor：Local / Global

ARM 把「独占」拆成两级（微架构可合并实现）。

### 15.1 Local Monitor（每 PE）

```
状态：Open / Exclusive Access
LDXR 成功 → Exclusive，记下地址（及粒度）
本 PE 普通 STR 同地址 / CLREX / 部分异常路径 → 清除
STXR：仍 Exclusive 且地址匹配 → 尝试系统级提交；否则失败
```

**粒度常为 cache line（如 64B）**。同 line 无关写也会踢掉 exclusive → **false sharing** → STXR 狂失败。

### 15.2 Global Monitor（一致性域）

多 PE 对同一 **PA (Physical Address)** 做 exclusive 时，需要系统级观察：

```
记录哪些 master 对哪些地址持有 exclusive 兴趣
任一破坏性写 → 作废其它 master 的 exclusive
Exclusive store 提交点：标记仍在 → EXOKAY；否则 OKAY（失败）
```

**是的，就是常说的 global monitor（或 CCI/CMN/DSU 内等价逻辑）**：保证「同一地址同时只有一个 STXR 成功」。

### 15.3 锁变量落点

| 落点 | 行为 |
|------|------|
| **Normal Inner Shareable + Cacheable** | 正确默认 |
| Non-cacheable Normal | 可走总线 exclusive，慢，依赖互连 |
| **Device / Strongly-ordered** | **不要**做自旋锁 |
| 其它核不可见的私有内存 | 无法跨核互斥 |

### 15.4 上下文切换与 CLREX

切换时若不清理 exclusive，可能误成功/怪行为。内核进入/退出路径会 `clrex` 或等价处理。

---

## 16. AXI / ACE / CHI：独占访问如何在总线上成立

### 16.1 纯 AXI（无一致性）

| 概念 | 含义 |
|------|------|
| `ARLOCK` / `AWLOCK` | Normal vs **Exclusive** |
| 写响应 `EXOKAY` | 独占写成功 |
| 写响应 `OKAY` | 普通成功，或 **独占失败** |

Monitor：Exclusive Read 记账 → 中间若有它人写同地址则清账 → Exclusive Write 账仍在则提交+EXOKAY，否则失败。

**关键**：经典 AXI **不管 cache 一致**。CPU SMP 上的锁必须走 **ACE / CHI**（或软件刷 cache，Linux SMP 不这么干）。

### 16.2 ACE（Cortex-A 常见）

```
ACE = AXI + 一致性通道（snoop）
Exclusive 嵌在一致性事务中
CCI / DSU 等含 snoop filter，并配合 global exclusive
```

### 16.3 CHI（Neoverse / CMN）

ReadShared / ReadUnique / Atomic / Exclusive 类报文在 RN-F↔HN-F 完成；大规模 SoC 原厂必问 CMN。

### 16.4 双核抢锁时序（可画板）

```
CPU0 LDXR [lock] → 标记 local(+global 兴趣)
CPU1 LDXR [lock] → 两端都可读到 unlocked=0
CPU0 STXR 1      → EXOKAY；作废 CPU1 exclusive / 刷其线
CPU1 STXR 1      → 失败 → 重回 LDXR
```

RTL 断言：`EXOKAY` 与数据提交绑定；失败路径 **不得写入** lock 字。

> **波形级例子**见 [`../05-bus-rtl/01-spinlock-to-bus.md`](../05-bus-rtl/01-spinlock-to-bus.md) **§9**（含普通写踢 Monitor）。

---

## 17. Cache 一致性与锁的关系（MESI / Snoop）

### 17.1 为何绑死

- 持锁核希望 lock 行在自己 cache 里 **Unique/Modified**，改得快。  
- 其它核自旋读 → 行 **Shared**；释放写 0 → 必须 **Invalidate/Update** 他人。  
- STXR 成功 ≈ 协议上取得写权限 + monitor 允许。

无一致性的「假共享内存」→ 双进临界区 → `rq` 破坏 → 随机 panic。

### 17.2 MESI 与锁行

| 状态 | 锁场景 |
|------|--------|
| I | 被别人写后失效 |
| S | 多核 LDXR 见 unlocked |
| E/M | STXR 成功后持有写权限 |

**False sharing**：`lock` 与热计数器同 64B → 不断踢行/monitor → 空转+互联拥塞。性能 bug 高发区。

### 17.3 DMA

- IO-coherent master：走正确 snoop/ATS。  
- 非一致 DMA：**禁止**乱写内核自旋锁所在行；或整页 Non-cacheable（非 Linux 锁常规做法）。

### 17.4 对照 MCU

| 手机 SMP | 车规 MCU |
|----------|----------|
| ACE/CHI + Global Monitor | 常单核关中断即可 |
| 锁在 DDR cacheable | SRAM；DMA 要手动一致性 |
| snoop 自动 | fence + flush/invalidate |

---

## 18. 单核 / SMP / AMP / 异构：差异与坑

### 18.1 单核（UP）

关抢占（+中断上下文关中断）即可防同核并发。spinlock API 可保留但硬件可退化。  
M100：无 SMP，同步首选关 `MIE`；多 master（CPU+DMA）仍要总线/约定互斥。

### 18.2 SMP（Android 手机）

同 Linux、同一致性域；`rq` 每 CPU 一把锁；迁移靠锁序；IPI (Inter-Processor Interrupt)（SGI）踢 resched。

### 18.3 AMP（Linux + RTOS）

不能假设共用一把 `LDXR` 锁，除非共享区在**同一一致性域**并有协议。  
常用：non-cacheable SRAM、双方 flush+屏障、硬件邮箱/自旋锁 IP、rpmsg。

### 18.4 big.LITTLE

锁语义仍成立；坑是持锁者趴在小核低频跑长临界区 → 大核狂自旋（耗电+延迟，与 EAS 叠加）。对策：缩短临界区。

### 18.5 多 cluster

CHI/CMN 延时大 → 自旋更贵 → 更依赖排队锁、减少跨核乒乓。

---

## 19. 内存屏障与锁：DMB/DSB 何时必须出现

```
正确性 = 原子性(exclusive) + 顺序可见性(barrier) + 一致性(cache 协议)
```

| 位置 | ARM64 | 作用 |
|------|-------|------|
| 获锁后 | `dmb ish` / acquire | 临界区读不重排到获锁前 |
| 解锁前 | `stlr` / `dmb` | 临界区写在解锁前可见 |
| 只有原子无 barrier | — | **payload 可穿帮**（常被追问） |

RISC-V：`fence` 与 LR/SC、AMO 配对。

---

## 20. Kernel 源码锚点与 SoC 验证清单

### 20.1 精读路径（ARM64）

```text
include/asm-generic/qspinlock.h
arch/arm64/include/asm/atomic_llsc.h      # LDXR/STXR
arch/arm64/include/asm/atomic_lse.h       # LSE
kernel/sched/core.c                       # rq lock
arch/arm64/kernel/smp.c                   # IPI
Documentation/atomic_t.txt
ARM ARM：Exclusive monitors
AMBA AXI / ACE / CHI overview
本 SoC TRM：CCI/DSU/CMN、line size、IO-coherent master
```

### 20.2 Bring-up / 验证清单

| # | 项 | 方法 |
|---|-----|------|
| 1 | 双核同时 STXR，仅一核成功 | 专测 + 计数 |
| 2 | EXOKAY 与写提交一致 | 总线 VIP / assert |
| 3 | False sharing 失败率 | 同 line vs align 64 |
| 4 | Device 内存自旋 | 应拒绝或不可用 |
| 5 | DMA 写锁行 | 禁止或走一致路径 |
| 6 | WFE/SEV、CPU idle | 解锁能唤醒 |
| 7 | Cluster 间锁风暴 | 调度迁移压测 |
| 8 | clrex / 切换路径 | 随机切换 + atomic 压测 |
| 9 | TZ Secure/NS 同 PA | 规格是否分 monitor |
| 10 | RV reservation 粒度 | 文档 + 实验 |

### 20.3 和调度联查

```
soft lockup / runq 异常
  → 是否自旋 rq->lock？
  → 持锁核是否卡在关中断长临界区 / 外设 hang？
  → exclusive 风暴？false sharing？
```

### 20.4 RTL / IP（验证习惯）

```
Monitor：表项深度、line 粒度、MasterID 宽、与 snoop 清标顺序、
         reset/低功耗唤醒后必须清空
验证：单功能成功/失败 → 多 master 并发 → cache on/off 矩阵 → 随机压测
```

---

## 21. 综合口述稿（含原厂深度）

### 系统产品向（90 秒）

> Android 内核是 CFS，手机关键是 EAS 选核和 schedutil 调频；框架用 cpuset/uclamp，厂商用 Hint 做 Loading 短时 boost。优化看场景识别、vote 和热回落。

### Loading（90 秒）

> 先分 CPU/IO/热。系统侧 Loading hint：抬 min_freq 和 boost、放开 cpuset、DDR vote、压后台；应用侧异步加载。Hint 必须超时释放；结束后延迟撤 boost。Perfetto 看是否趴小核、升频滞后、iowait。

### SoC 原厂：spinlock 全链路（2 分钟）

> 调度里的 spinlock，C 层是原子或 qspinlock，ARM64 落到 LDXR/STXR 或 LSE。LDXR 置 Local Monitor；STXR 成败还取决于 Global Monitor/一致性互联是否仍承认该 master 独占。总线上是带 Exclusive 的一致性事务，成功 EXOKAY。锁必须在 Normal cacheable Inner Shareable，靠 ACE/CHI snoop 与 MESI 状态保证多核顺序，再用 DMB 做 acquire/release。单核可关抢占退化；SMP 必须硬件一致；AMP 跨 OS 不能共用假 LDXR，除非真共享一致域。False sharing、DMA 乱写、Device 上自旋是典型坑。裸 AXI 有 LOCK/EXOKAY，但 CPU SMP 实际走 ACE/CHI，monitor 常和 snoop filter 做在 CCI/DSU/CMN 里。

---

## 附录 A：建议实验

1. Loading Perfetto 基线（核分布 + 时长）。  
2. 只抬 min_freq / 只改 cpuset 做 A/B。  
3. 故意泄漏 boost 看热与掉帧。  
4. **双线程 STXR 计数**：统计失败次数。  
5. **False sharing**：锁与计数器同 line vs `alignas(64)`。  
6. **AMP 反例**：两 OS 无协议写「锁」演示双进临界区。

---

## 附录 B：路径速记

```text
/sys/devices/system/cpu/cpu*/cpufreq/
/sys/devices/system/cpu/cpu*/cpu_capacity
/dev/cpuset/
/dev/stune/
/sys/fs/cgroup/
/sys/class/thermal/
/proc/<pid>/cpuset
```

---

## 附录 C：原厂文档索引

| 文档 | 读什么 |
|------|--------|
| ARM ARM (DDI 0487) | Exclusive monitors；LDXR/STXR；Shareability |
| ARM Barrier Litmus | DMB/DSB 与锁 |
| AMBA AXI4 | LOCK；EXOKAY；见 [`01-spinlock-to-bus.md`](../05-bus-rtl/01-spinlock-to-bus.md) §9 |
| AMBA ACE / CHI | 一致性与 exclusive |
| AMBA AHB (Advanced High-performance Bus) | **HLOCK**；见 [`01-spinlock-to-bus.md`](../05-bus-rtl/01-spinlock-to-bus.md) §10 |
| 本 SoC TRM (Technical Reference Manual) | CCI/DSU/CMN；line size；IO-coherent masters |
| RISC-V Spec | LR/SC；AMO；RVWMO |


---

---

## 小结

- 产品调度 = Kernel 策略（CFS/EAS）+ 负载信号（PELT/WALT）+ cgroup/hint 约束。
- Loading 优化要用窗口化组合拳，并带热/功耗回退，避免永久抬频。
- 先定性瓶颈（CPU/IO/锁/渲染）再调调度，否则只是在噪声上拧旋钮。
- Part B 的同步/一致性地图与 `01-spinlock-to-bus` 互补：专文讲机制，本文讲调度语境。

## 自测

1. EAS 选核时主要看什么信号？
2. PELT 与 WALT 的工程直觉差异？
3. 举一个 Loading Hint 组合，并说明必须带哪些约束。
4. systrace/perfetto 上你按什么顺序排除「不是调度问题」？
5. 为何谈调度仍要理解 spinlock/一致性？专文与本文如何分工？

---

*`01-kernel` · Android/SoC 调度工程*
