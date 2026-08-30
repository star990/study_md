# Linux 进程调度：CFS (Completely Fair Scheduler) / EAS (Energy Aware Scheduling) / RT / PREEMPT_RT (Preemptible Real-Time)

> **系列**：`01-kernel`  
> **前置**：对进程与中断有基本概念即可  
> **相关**：[`02-memory.md`](02-memory.md) · [`03-crash-debug.md`](03-crash-debug.md) · [`04-android-soc-scheduling.md`](04-android-soc-scheduling.md) · [`../05-bus-rtl/01-spinlock-to-bus.md`](../05-bus-rtl/01-spinlock-to-bus.md)

从调度 class 讲到机器人场景里该用谁：CFS 公平、EAS 能效、FIFO/RR、以及 PREEMPT_RT 如何把延迟压下去。文中一并整理 softirq / tasklet / workqueue / spinlock 与 RTOS (Real-Time Operating System)+PMP 隔离思路。

**读完应能**：
- 画出调度器与实时路径的关系
- 说明 PREEMPT_RT 相对主线改了什么
- 为底半部机制做选型

---
## 1.1 调度器概览

Linux 内核调度器负责决定**哪个 task 在哪个 CPU 上运行**。不同调度 class 服务不同场景：

```
                    ┌─────────────────┐
                    │  schedule()      │
                    │  (kernel/sched/) │
                    └────────┬────────┘
                             │
         ┌───────────────────┼───────────────────┐
         │                   │                   │
    ┌────▼────┐        ┌────▼────┐        ┌────▼────┐
    │  CFS    │        │   RT    │        │  DL     │
    │ SCHED_  │        │ SCHED_  │        │ SCHED_  │
    │ NORMAL  │        │ FIFO/RR │        │ DEADLINE│
    └─────────┘        └─────────┘        └─────────┘
    普通任务              实时任务              _deadline
    (vruntime)           (优先级)              (EDF算法)
```

| 调度 class | 策略 | 典型用途 |
|------------|------|----------|
| **CFS** | SCHED_NORMAL / SCHED_BATCH / SCHED_IDLE | 普通应用、后台任务 |
| **RT** | SCHED_FIFO / SCHED_RR | 音频、控制环路、低延迟 |
| **DL** | SCHED_DEADLINE | 有明确 deadline 的任务 |
| **EAS** | CFS 的 ARM 扩展 | big.LITTLE 能效调度 |
| **Stop/Idle** | 内核专用 | stop machine、idle 线程 |

---

## 1.2 CFS（Completely Fair Scheduler）

### 核心思想

CFS 不给 task 固定时间片，而是追踪每个 task **已消耗的 CPU 时间**，总是选**最"亏"的**（vruntime 最小）来运行，实现"完全公平"。

### 关键概念

| 概念 | 说明 |
|------|------|
| **vruntime** | Virtual Runtime，task 累计消耗的"虚拟 CPU 时间" |
| **min_vruntime** | runqueue 中所有 task 的最小 vruntime，作为基准 |
| **weight (权重)** | 由 nice 值决定，nice 越小权重越大 |
| **period** | 调度周期，默认约 6ms（`sysctl_sched_latency`） |
| **granularity** | 最小调度粒度，约 0.75ms |

### vruntime 计算

```
实际运行时间 delta_exec
vruntime 增量 = delta_exec × (NICE_0_LOAD / task_weight)

nice=0  → weight=1024 → vruntime 增量 = delta_exec × 1
nice=10 → weight=110  → vruntime 增量 = delta_exec × 9.3（跑得"慢"，更容易被调度）
nice=-10→ weight=8876 → vruntime 增量 = delta_exec × 0.12（跑得"快"，不容易被抢占）
```

**记忆**：nice 值越大 → 权重越小 → vruntime 涨得越快 → 更容易让出 CPU。

### 数据结构

```c
/* kernel/sched/sched.h */
struct sched_entity {
    struct load_weight  load;       /* 权重 */
    struct rb_node      run_node;   /* 红黑树节点 */
    u64                 vruntime;   /* 虚拟运行时间 */
    u64                 sum_exec_runtime;
    ...
};

struct cfs_rq {
    struct rb_root      tasks_timeline;  /* 红黑树，按 vruntime 排序 */
    struct rb_node      *rb_leftmost;    /* vruntime 最小的 task */
    ...
};
```

### CFS 调度流程

```
schedule() 被调用（tick 中断 / 主动 yield / 阻塞）
  │
  ▼
pick_next_task_fair()
  │
  ├── 从 cfs_rq 红黑树取 vruntime 最小的 task
  ├── 检查是否需要 preempt（当前 task vruntime 比最小的大太多）
  └── 返回 next task
  │
  ▼
context_switch(prev, next)
  │
  ├── 切换地址空间（若 next 不同进程）
  ├── 切换寄存器
  └── 切换栈
```

### nice 值

| nice | 权重 | 相对 CPU 份额 |
|------|------|---------------|
| -20 | 88761 | ~87× |
| -10 | 9548 | ~9× |
| 0 | 1024 | 1×（基准） |
| 10 | 110 | ~1/9 |
| 19 | 15 | ~1/68 |

```bash
# 用户态调整
nice -n 10 ./my_app        # 启动时设 nice
renice -n -5 -p 1234       # 运行时调整
```

### 新 task 的 vruntime 初始化

新 fork 的 task 不能 vruntime=0（会饿死老 task），而是：

```c
se->vruntime = max_vruntime(curr->vruntime, min_vruntime);
```

保证新 task 不会获得不公平优势。

---

## 1.3 EAS（Energy Aware Scheduling）

### 背景

ARM big.LITTLE 架构：大核（big）性能高功耗大，小核（LITTLE）性能低功耗小。EAS 让调度器**同时考虑性能和能效**。

### 工作原理

```
传统 CFS：只看 vruntime，选最"亏"的 task
EAS：     看 vruntime + 能效模型，选"性价比最高"的 CPU

能效模型输入：
  - CPU 容量（capacity）：big=1024, LITTLE=512
  - 任务利用率（utilization）
  - 能耗曲线（per-CPU energy model）
```

### EAS 决策示例

| 场景 | EAS 选择 |
|------|----------|
| 轻负载后台任务 | 小核（LITTLE） |
| 重负载计算任务 | 大核（big） |
| 中等负载 | 当前核（避免 migration 开销） |
| 唤醒 idle task | 选能最快完成且能效好的核 |

### 与机器人场景的关系

**要点**：
- AI 推理、视觉处理 → 大核 CFS 普通任务
- 运动控制、传感器融合 → **SCHED_FIFO + CPU affinity 绑小核/专用核**
- EAS 适合**通用负载能效优化**，不替代 RT 调度

### 相关内核配置

```
CONFIG_ENERGY_MODEL=y
CONFIG_SCHED_energy=y    # EAS 开关
```

---

## 1.4 RT 调度（SCHED_FIFO / SCHED_RR）

### SCHED_FIFO

- **同优先级 FIFO** 队列
- 高优先级 task **永远**抢占低优先级
- 同优先级：先运行的不让出，除非主动 yield 或阻塞
- **无时间片**——这是与 CFS 最大区别

### SCHED_RR

- 同优先级 **Round Robin**，有时间片（默认 100ms）
- 时间片用完 → 排到同优先级队列末尾
- 高优先级仍然抢占低优先级

### 优先级范围

```
RT 优先级：1（最低）~ 99（最高）
普通优先级：100（对应 nice 0）~ 139（对应 nice 19）

数值越小优先级越高（RT）
数值越大优先级越低（CFS）
```

### 代码示例

```c
struct sched_param param;
param.sched_priority = 50;  /* RT 优先级 1-99 */

/* 设为 SCHED_FIFO */
sched_setscheduler(0, SCHED_FIFO, &param);

/* 绑定 CPU */
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(2, &cpuset);  /* 绑到 CPU2 */
sched_setaffinity(0, sizeof(cpuset), &cpuset);
```

### RT 陷阱：优先级反转

```
低优先级 Task L 持有锁
  → 高优先级 Task H 等锁（被 L 阻塞）
    → 中优先级 Task M 抢占 L
      → H 等 M 完成，M 等 L，L 被 M 挡 → H 被间接阻塞

解决：Priority Inheritance（优先级继承）
  L 持有锁期间临时提升到 H 的优先级
```

Linux 的 PI mutex（`CONFIG_RT_MUTEXES`）实现了优先级继承。

---

## 1.5 PREEMPT_RT（机器人场景重点详解）

### 是什么

**PREEMPT_RT**（Real-Time Linux）把主线 Linux 改造成**几乎完全可抢占**的内核，把最坏情况延迟（worst-case latency）从毫秒级压到 **10–50μs**（调优+硬件好的情况下）。主线从 6.x 起已逐步合入大量 RT 代码；生产环境仍常说「打 RT 补丁」或选带 `PREEMPT_RT` 的发行版。

机器人需要：电机 FOC、IMU 融合、急停路径的**确定性**——PREEMPT_RT 是「单 OS 方案」的核心选项。

### 主线抢占等级（对照）

| 配置 | 含义 | 典型延迟量级 |
|------|------|--------------|
| `PREEMPT_NONE` | 不可抢占（服务器默认） | 毫秒级尖峰常见 |
| `PREEMPT_VOLUNTARY` | 自愿抢占点 | 数百 μs～ms |
| `PREEMPT` | 可抢占内核（桌面） | 百 μs 级 |
| **`PREEMPT_RT`** | 几乎处处可抢占 | **10–50μs**（目标） |

### 核心改动（必记）

| 原版 Linux | PREEMPT_RT | 为什么重要 |
|------------|------------|------------|
| 多数 `spinlock` 关抢占 | 变成 **rt_mutex / sleeping spinlock** | 持锁时高优先级可抢占低优先级 |
| 硬中断里跑长 handler | **IRQ (Interrupt Request) 线程化**（`irq_thread`） | 中断可被更高优先级线程抢占 |
| 关中断临界区很长 | 尽量缩短；真正关中断用 `raw_spinlock` | 降低「关中断导致的延迟」 |
| softirq 在硬中断尾部 | 可移到线程上下文（ksoftirqd 等） | softirq 不再拖死实时任务 |
| print_console / 部分 printk | 可延后 | 避免 printk 拖垮实时路径 |
| `local_bh_disable` 等 | 行为更接近「可抢占」语义 | 减少非预期关抢占窗口 |

**保留硬实时语义的原语**：

```
raw_spinlock_t     → 真正关抢占/关中断的短临界区（驱动硬件寄存器瞬间）
local_irq_disable  → 仍存在，但 RT 要求用法极短
```

### IRQ 线程化

```
传统：
  硬件 IRQ → 硬中断 handler（关抢占）→ 可能很长 → 实时任务被挡

PREEMPT_RT：
  硬件 IRQ → 极短硬中断（ACK + wake irq_thread）→ 返回
                │
                ▼
           irq_thread（SCHED_FIFO 可配优先级）
                │
                ▼ 可被更高优先级实时线程抢占
           驱动真正的处理逻辑
```

```bash
# 查看线程化中断
ps -eLo pid,tid,class,rtprio,comm | grep irq
# 调高某个 IRQ 线程优先级（示例）
chrt -f -p 90 $(pgrep -f irq/32-eth0)
```

### 机器人常用调优手段（与 RT 补丁配套）

| 手段 | 作用 |
|------|------|
| **SCHED_FIFO + 绑核** | 控制环固定 CPU，不被 CFS 任务抢 |
| **CPU isolation** (`isolcpus` / `cpusets`) | 隔离核只跑实时任务 |
| **关 NOHZ / 调 tick** | 减少无用唤醒（`nohz_full` 等） |
| **禁 IRQ 亲和到隔离核** | `irqaffinity=` 把中断挪到非实时核 |
| **mlockall** | 避免 page fault 抖动 |
| **cyclictest** | 测延迟分布（p99/max） |

```bash
# 延迟摸底（工程可参考）
cyclictest -m -S -p 90 -i 200 -l 100000
# -m mlockall, -S SMP, -p 90 FIFO, -i 200μs 周期
```

### PREEMPT_RT 的上限（诚实讲）

- **不是硬实时证明**：仍是 best-effort + 调优；功能安全认证常仍要 MCU/RTOS 侧
- 内核路径、驱动写坏（关中断太久、`raw_spin` 滥用）仍会出尖峰
- 延迟通常 **高于** 裸机 FreeRTOS（1–10μs），但生态远强
- GPU/NPU 驱动、DMA (Direct Memory Access)、文件系统仍可能引入抖动

### 口述要点（机器人芯片场景）

> 「PREEMPT_RT 把 Linux 从『软实时』推到『工程可用的准硬实时』：IRQ 线程化 + sleeping lock 是关键。电机级微秒环若必须证明 WCET，仍倾向 RTOS/MCU；视觉+规划+网络同 SoC 时，PREEMPT_RT + isolcpus 或 AMP (Asymmetric Multi-Processing)（Linux+RTOS）更常见。」

> **详见下一节 §1.6**：softirq / tasklet / workqueue / spinlock 在主线与 PREEMPT_RT 上的完整对照。

---

## 1.6 中断底半部与延迟执行：softirq / tasklet / workqueue / spinlock

> 讨论调度时常从「中断里能不能睡觉」挖到机制选型；SoC/机器人岗还要能说清 **PREEMPT_RT 下这些机制怎么变**。

### 1.6.1 为何要有「底半部」

硬中断（top half）上下文：

- **不能睡眠**（无进程可调度）
- 应尽量短：ACK、清状态、搬关键数据、标记「有活要干」
- 关抢占（甚至关中断），拖太久 → 延迟、丢中断、realtime 崩

因此把「可以稍晚做的工作」推到 **底半部（bottom half）** 或进程上下文：

```
硬件 IRQ
  │
  ▼
硬中断 handler（top half）—— 极短
  │  raise_softirq / tasklet_schedule / queue_work / wake irq_thread
  ▼
底半部或线程（可相对较长；能否睡取决于机制）
```

---

### 1.6.2 硬中断（HardIRQ）路径（主线）

```
GIC 投递 → arch entry.S
  → handle_domain_irq / generic_handle_irq
    → 驱动 handler（request_irq 注册的）
      → 可能 local_bh_enable 末尾跑 softirq
    → 返回前检查 softirq pending → __do_softirq()
```

```c
/* 典型「上半部极短」写法 */
irqreturn_t my_irq(int irq, void *dev)
{
    u32 st = readl(dev->base + STATUS);
    writel(st, dev->base + STATUS);   /* ACK */
    if (st & RX_READY)
        tasklet_schedule(&dev->rx_tasklet);  /* 或 queue_work / softirq */
    return IRQ_HANDLED;
}
```

`request_threaded_irq()`：硬中断返回 `IRQ_WAKE_THREAD` → 内核唤醒对应 **irq 线程**（已是进程上下文，可睡）。

---

### 1.6.3 softirq：实现要点

**是什么**：内核编译期固定的一类「软件中断」向量（网络、block、RCU、tasklet 载体、HRTimer…），在 **关底半部** 的上下文执行，**仍不可睡眠**。

**注册与触发**：

```c
/* 启动时 */
open_softirq(NET_RX_SOFTIRQ, net_rx_action);

/* 硬中断或其它路径 */
raise_softirq(NET_RX_SOFTIRQ);   /* 置 pending 位 */
```

**何时真正跑**：

1. 硬中断返回前：`irq_exit` → `invoke_softirq` → `__do_softirq`
2. `local_bh_enable()` 时若 pending
3. 太多/太久 → 唤醒 **`ksoftirqd/N`** 在进程上下文继续跑（仍标为 softirq 语义，但可被调度）

```
__do_softirq():
  关 BH（防重入本 CPU softirq）
  while pending:
    清 bit → 调 action()
    若超时/次数过多 → 打破循环，wakeup ksoftirqd
  开 BH
```

**特点**：

| | softirq |
|--|---------|
| 上下文 | 通常硬中断尾 / BH-disable；可落到 ksoftirqd |
| 能睡？ | **否** |
| 并发 | **同一 softirq 可在多 CPU 同时跑**（网络收包靠此） |
| 同步 | 共享数据常用 `spinlock` / per-CPU |

**常见类型**：`HI_SOFTIRQ`、`TIMER`、`NET_TX/RX`、`BLOCK`、`IRQ_POLL`、`TASKLET`、`SCHED`、`HRTIMER`、`RCU` 等。

---

### 1.6.4 tasklet：基于 softirq 的串行化

**是什么**：跑在 **TASKLET_SOFTIRQ / HI_SOFTIRQ** 上的回调；**同一 tasklet 实例不会并行**（多 CPU 上串行），不同 tasklet 可并行。

```c
void my_tasklet_fn(unsigned long data)
{
    struct my_dev *d = (void *)data;
    /* 不可 sleep；可用 spin_lock（注意与硬中断的 irqsave） */
}

DECLARE_TASKLET_OLD(my_tasklet, my_tasklet_fn);
/* 或 tasklet_setup() 新 API */

/* 硬中断里 */
tasklet_schedule(&my_tasklet);
```

| | tasklet |
|--|---------|
| 能睡？ | **否** |
| 相对 softirq | 更易用；自动「同实例串行」 |
| 现状 | **逐渐不推荐新代码**；主线倾向 threaded IRQ / workqueue |
| 适用 | 短、不可睡、要串行的收尾（老驱动常见） |

---

### 1.6.5 workqueue：进程上下文延迟执行

**是什么**：把活交给 **worker 内核线程**（`kworker`），**可以睡眠**（mutex、kmalloc(GFP_KERNEL)、等待完成等）。

```c
void my_work_fn(struct work_struct *w)
{
    struct my_dev *d = container_of(w, struct my_dev, work);
    mutex_lock(&d->mlock);
    /* 可 sleep：等硬件、拷用户缓冲、调监管接口 */
    mutex_unlock(&d->mlock);
}

INIT_WORK(&d->work, my_work_fn);

/* 原子上下文可调 */
queue_work(system_wq, &d->work);
/* 或自己的 alloc_workqueue("mywq", WQ_UNBOUND | WQ_HIGHPRI, 0) */
```

| 类型 | 说明 |
|------|------|
| `system_wq` | 共享；短活 |
| `system_highpri_wq` | 更高 nice |
| `system_unbound_wq` | 不绑 CPU，吞吐型 |
| `system_long_wq` | 较长活 |
| 私有 `alloc_workqueue` | 要隔离/优先级/rescuer 时自建 |
| `delayed_work` | 延迟 jiffies 再跑 |
| `WQ_MEM_RECLAIM` | 内存回收路径安全相关 |

**适用**：耗时、要睡、要拿 mutex、文件系统/用户缓冲交互。

---

### 1.6.6 Threaded IRQ（现代驱动首选之一）

```c
request_threaded_irq(irq, my_hard_irq, my_thread_fn,
                     IRQF_ONESHOT, "mydev", dev);

irqreturn_t my_hard_irq(int irq, void *dev)
{
    /* 极短：mask/ack；返回 WAKE_THREAD */
    return IRQ_WAKE_THREAD;
}

irqreturn_t my_thread_fn(int irq, void *dev)
{
    /* 进程上下文：可 sleep；IRQF_ONESHOT 下硬中断保持屏蔽直到线程跑完 */
    return IRQ_HANDLED;
}
```

相对手写 tasklet：优先级可调、可睡、RT 下天然友好。

---

### 1.6.7 spinlock / mutex / raw_spinlock：和底半部怎么配

| 原语 | 能睡？ | 关什么 | 典型场景 |
|------|--------|--------|----------|
| **`spinlock_t`** | 否 | 关抢占（SMP (Symmetric Multi-Processing) 上自旋） | 短临界区；与 softirq 共享用 `spin_lock_bh` |
| **`spinlock_irq` / `_irqsave`** | 否 | 关抢占+关（本 CPU）硬中断 | 与硬中断 handler 共享数据 |
| **`raw_spinlock_t`** | 否 | 真·硬自旋 | 极短；**PREEMPT_RT 上几乎唯一还能硬关抢占的锁** |
| **`mutex`** | **能** | 无 | 进程上下文；workqueue / 线程化 IRQ / ioctl |
| **`rt_mutex`** | 能 | 带优先级继承 | RT；主线 mutex 在 RT 上行为靠拢 |

**选锁口诀**：

```
硬中断 ↔ 进程共享     → spin_lock_irqsave
硬中断 ↔ softirq/tasklet → 常 irqsave 或设计成只在一侧碰
softirq ↔ 进程         → spin_lock_bh
仅进程 ↔ 进程且可睡   → mutex
RT 上「必须不睡」的瞬间 → raw_spinlock（越短越好）
```

与 [`04-android-soc-scheduling.md`](04-android-soc-scheduling.md) Part B 及 [`../05-bus-rtl/01-spinlock-to-bus.md`](../05-bus-rtl/01-spinlock-to-bus.md) 衔接：spinlock 底层是 **LDXR/STXR (Load-Exclusive / Store-Exclusive) + Monitor + 一致性**；在 softirq/硬中断里自旋过久 = 延迟尖峰。

---

### 1.6.8 对照总表（选型）

| 机制 | 上下文 | 可睡 | 同实例并行 | 延迟量级 | 适用 |
|------|--------|------|------------|----------|------|
| **HardIRQ** | 硬中断 | 否 | 同 IRQ 一般串行（有 affinity） | 最短 | ACK、清状态 |
| **softirq** | BH / ksoftirqd | 否 | **多 CPU 可并行** | 很短～短 | 网络等高频 |
| **tasklet** | softirq 上 | 否 | **同实例不并行** | 短 | 老驱动短收尾 |
| **Threaded IRQ** | 内核线程 | **是** | 按线程 | 短～中 | **新驱动首选** |
| **workqueue** | kworker | **是** | 可并发（视 wq） | 中 | 耗时/可睡 |
| **独立 kthread** | 自建线程 | 是 | 自管 | 中 | 长期服务、策略循环 |
| **HRTimer** | 常 softirq / hard | 否* | — | 很短 | 高精度定时 |

\* HRTIMER 模式不同；回调仍通常不可睡。

**决策树（工程）**：

```
中断里要做事？
  ├─ 必须 < 几 μs 且不可睡 → 硬中断内做完，或 raise softirq/tasklet
  ├─ 要可睡 / 拿 mutex     → threaded IRQ 或 queue_work
  ├─ 高频网络类并行       → softirq（现成 NET_RX）
  └─ 周期性后台           → delayed_work / kthread
```

---

### 1.6.9 PREEMPT_RT 下这些机制如何变化（重点）

PREEMPT_RT 的目标：压缩「不可抢占窗口」。底半部/锁是重灾区。

| 机制 | 主线 | PREEMPT_RT |
|------|------|------------|
| **HardIRQ** | 关抢占跑 handler | **强制线程化**（除少数 `IRQF_NO_THREAD`）；primary 极短，活在 `irq/N-name` |
| **softirq** | 常在中断返回路径关 BH 跑 | 更多在 **线程上下文**（ksoftirqd / 强制线程化路径），减少「关 BH 拖死 RT 任务」 |
| **tasklet** | softirq 上，不可抢占窗口 | 随 softirq 线程化；**新代码更不建议用** |
| **workqueue** | 已是进程上下文 | 变化相对小；注意 WQ 优先级与 RT 任务别互相挤 |
| **`spinlock_t`** | 关抢占自旋 | 变成 **可睡的 RT 锁**（类似 rt_mutex），持锁可被更高优抢占 |
| **`raw_spinlock_t`** | 硬自旋 | **仍硬自旋**；仅留给真正要关抢占的瞬间（硬件、调度器极热路径） |
| **`local_bh_disable`** | 禁 softirq | 语义保留但实现为「可睡眠的底半部锁」一类，避免长窗口 |
| **`mutex`** | 可睡 | 加强 **优先级继承**，减优先级反转 |
| **中断下半部里的锁** | `spin_lock_bh` | 要理解：BH 锁与 RT 睡眠锁组合后的延迟模型变了 |

**IRQ 线程化（与 §1.5 呼应）**：

```
主线：硬中断里可能直接跑很长 → RT 任务被挡

PREEMPT_RT：
  硬中断：ACK + wake_thread
  irq_thread（可 SCHED_FIFO）：跑原 thread_fn / 强制线程化后的 handler
  → 更高优先级实时任务可抢占 irq_thread（除非你把 IRQ 线程提得过高）
```

**工程后果**：

1. 驱动若在「以为是原子上下文」里调用可能睡眠的锁——主线碰巧能过，**RT 上会警告/死锁检测**（`CONFIG_DEBUG_ATOMIC_SLEEP`）。  
2. 用 `spinlock_t` 保护的「短」临界区若实际很长 → RT 上变成睡眠，可能引入 **优先级继承链**，延迟模型与主线不同。  
3. 必须与硬件寄存器「关抢占一致」的瞬间 → 改用 **`raw_spinlock_irqsave`**，并保证 **极短**。  
4. 新驱动：**threaded IRQ + mutex/workqueue**，少用 tasklet，少在硬中断里玩花活——主线与 RT 都更友好。

**和机器人场景**：

| 需求 | 建议 |
|------|------|
| 电机周期 100μs 内必须响应 | 优先 **隔离核 + SCHED_FIFO**；关键 IRQ 线程提优先级；或 AMP 到 RTOS |
| 传感器融合可睡 | workqueue / 用户 RT 线程 |
| 网络收包 | softirq/NAPI (New API)；RT 上接受 ksoftirqd，用 cpuset 与控制核隔离 |

---

### 1.6.10 最小代码对比（同一驱动三种底半部）

```c
/* A: tasklet（不可睡） */
static void rx_tasklet(unsigned long arg) { /* 解析描述符 */ }
static irqreturn_t irq_a(int i, void *d) {
    ack(); tasklet_schedule(&t); return IRQ_HANDLED;
}

/* B: workqueue（可睡） */
static void rx_work(struct work_struct *w) { /* 可 copy_to_user 路径、mutex */ }
static irqreturn_t irq_b(int i, void *d) {
    ack(); queue_work(system_wq, &work); return IRQ_HANDLED;
}

/* C: threaded IRQ（推荐） */
static irqreturn_t hard(int i, void *d) { ack(); return IRQ_WAKE_THREAD; }
static irqreturn_t thread(int i, void *d) { /* 可睡 */ return IRQ_HANDLED; }
/* request_threaded_irq(..., hard, thread, IRQF_ONESHOT, ...) */
```

---

### 1.6.11 高频追问（底半部）

1. **软中断里能调用 `mutex_lock` 吗？** → 不能（不可睡）。  
2. **tasklet 和 softirq 谁更并行？** → softirq 同类型可多 CPU；tasklet 同实例串行。  
3. **为何 PREEMPT_RT 还要 `raw_spinlock`？** → 调度器/中断入口等不能睡的硬原子区。  
4. **`spin_lock_bh` 禁的是什么？** → 本 CPU softirq（底半部），防与 softirq 死锁。  
5. **workqueue 一定比 softirq 慢吗？** → 通常是（线程唤醒）；但可睡、更安全；RT 上 softirq 线程化后差距可能缩小。

---

## 1.7 Linux 实时方案 vs RTOS：优劣对比（机器人）


### 总表

| 维度 | 主线 Linux | Linux + PREEMPT_RT | FreeRTOS / 类 RTOS |
|------|------------|--------------------|--------------------|
| 典型延迟 | ms 尖峰 | 10–50μs | **1–10μs** |
| 确定性可证明性 | 弱 | 中（测出来） | **强**（简单路径） |
| 内存占用 | 大 | 大 | **极小（KB）** |
| 网络/文件系统/驱动生态 | **最强** | 同左 | 弱（LwIP/FatFS） |
| 进程隔离 / MMU (Memory Management Unit) / 安全 | 完整 | 完整 | 默认弱（可加 PMP） |
| 开发效率 | 高 | 高 | 中（裸机感） |
| 功能安全（ISO）举证 | 难 | 难 | **相对容易** |
| 适用 | AI/视觉/云边 | 同 SoC 软实时控制 | 电机/急停/传感器环 |

### 优劣势拆开说

**PREEMPT_RT 优势**

1. 一套 Linux 跑视觉、ROS2、网络、控制（调优后）
2. 工具链成熟：ftrace、perf、cyclictest、cgroup
3. 驱动/中间件复用成本低

**PREEMPT_RT 劣势**

1. WCET 难严格证明；认证友好度不如 MCU RTOS
2. 调优复杂（亲和、隔离、驱动质量）
3. 延迟仍可能被脏驱动、锁、page fault 打穿

**RTOS 优势**

1. 路径短、延迟低、行为可预期
2. 适合功能安全、急停、FOC 内环
3. 可在小核/MCU 跑，和 Linux AMP 分工

**RTOS 劣势**

1. 无完整进程隔离时，任务互相踩内存（**你们想加 PMP 正是补这点**）
2. 生态弱：复杂协议栈、文件系统、AI 框架难
3. 若强行做成「类 Linux」（U-mode + syscall + 驱动封装），复杂度迅速逼近「迷你内核」

### 机器人推荐架构（可画图）

```
方案 A：单 Linux + PREEMPT_RT
  A78 × N：视觉/规划/ROS2 + 软实时控制（isolcpus）

方案 B：AMP（更常见于物理 AI 芯片）
  Cortex-A + Linux/PREEMPT_RT  ← 视觉/AI
  Cortex-R / RISC-V MCU + RTOS ← 电机/急停
  通信：rpmsg / 共享内存 + doorbell

方案 C：纯 RTOS MCU（小机器人/关节）
  单核 FreeRTOS +（可选）PMP 任务隔离
```

---

## 1.8 RTOS + PMP 进程隔离完整方案（U/M 模式）

> 背景：你们准备在 RTOS 上「切进程时切 PMP」，隔离任务地址空间。下面按「可落地的完整方案」写清边界，并正面回答：**做得完美是否必须大量 syscall 封装、内核栈不能给上层看**——答案是 **是，而且应该这么做**。

### 1.7.1 目标与非目标

| 目标 | 非目标 |
|------|--------|
| 任务 A 不能读写任务 B 的栈/数据 | 完整 Linux 级虚拟内存（页表/COW） |
| 用户态不能直接摸外设寄存器 | 完整 TEE (Trusted Execution Environment)/TrustZone |
| 非法访问触发 trap → kill/重启任务 | 多核复杂 NUMA |
| M-mode 内核可信计算基（TCB）尽量小 | 把所有驱动塞进 U-mode |

### 1.7.2 特权模型（RISC-V）

```
┌─────────────────────────────────────────────────────────┐
│  M-mode（RTOS Kernel / 可信计算基）                        │
│  - 调度器、PMP 切换、syscall、中断入口、驱动、内核栈        │
│  - 唯一能写 PMP CSR、唯一能直接访问外设的代码               │
├─────────────────────────────────────────────────────────┤
│  U-mode（Task / 「进程」）                                 │
│  - 应用逻辑、算法；只能访问自己的 PMP 窗口                  │
│  - 无直接 MMIO；通过 syscall / IPC 请求服务                 │
└─────────────────────────────────────────────────────────┘
```

可选：中间加 **S-mode**（更大系统）；MCU RTOS 通常 **M + U 两级足够**。

对照 ARM：M≈EL1/EL3 内核侧，U≈EL0；你们之前 ARM TrustZone 是另一维度，这里是 **任务隔离** 不是 Secure World。

### 1.7.3 内存布局与 PMP 槽位规划

假设 16 条 PMP（RV32 常见）：

| 槽位 | 用途 | 权限 | 谁配置 |
|------|------|------|--------|
| 0–1 | BootROM / Flash 代码（RX） | R X | 启动一次 |
| 2 | 外设 MMIO 窗口 | — 对 U **关**；M 全开 | 固定 |
| 3 | 内核 .text/.rodata | R X（U 不可进） | 固定 |
| 4 | 内核 .data/.bss + **内核栈** | R W（仅 M） | 固定 |
| 5–6 | **当前任务** code | R X | **每次调度切换** |
| 7–8 | **当前任务** data/bss/heap | R W | **每次调度切换** |
| 9 | **当前任务** 用户栈 | R W | **每次调度切换** |
| 10 | 共享只读区（配置表） | R | 固定或按任务 |
| 11 | 共享 IPC 环形缓冲（可选） | R W | 固定小窗 |
| 12–15 | 预留 / 锁死 TOR 哨兵 | L 位 lock | 启动后锁定关键槽 |

**关键原则**：

1. **内核栈永不映射给 U**：PMP 不给 U 看内核栈区间（或整段内核 RAM 对 U 关闭）。
2. **切换任务 = 只改「当前任务」相关槽**，固定槽不动。
3. 用 **NAPOT** (Naturally Aligned Power-of-Two) 对齐区域（常用 NAPOT）；不对齐就多条 TOR。
4. 切完 PMP 后必须 **`fence` / SFENCE.VMA 类屏障**（无 MMU 时至少 `fence` + 确认 CSR (Control and Status Register) 写完成）。

### 1.7.4 上下文里要存什么

```c
/* 每个任务一份「用户上下文 + PMP 镜像」 */
struct pmp_entry {
    uint32_t addr;   /* pmpaddr 编码后的值，或裸基址+编码辅助 */
    uint8_t  cfg;    /* R/W/X/A/L */
};

#define PMP_SLOTS_PER_TASK  5   /* code×2 + data×2 + stack×1，示例 */

struct task_pmp {
    struct pmp_entry e[PMP_SLOTS_PER_TASK];
};

struct tcb {
    uint32_t     *sp_user;      /* U-mode 栈指针 */
    uint32_t     *sp_kernel;    /* 本任务陷入时用的内核栈顶（可选：全局内核栈） */
    uint32_t      mepc;         /* 返回 U 的 PC */
    uint32_t      mstatus;      /* MPP=U 等 */
    struct task_pmp pmp;        /* 该任务可见窗口 */
    uint8_t       prio;
    /* ... 队列、名字等 */
};
```

**两种内核栈策略**：

| 策略 | 做法 | 优劣 |
|------|------|------|
| **A. 全局一张内核栈** | 任意时刻只有一个任务在 M 态跑，共用 `g_kernel_stack` | 省 RAM；不支持「M 态可抢占嵌套」要小心 |
| **B. 每任务一张内核栈**（类 Linux） | 陷入时切到 `tcb->sp_kernel` | 更像真正 OS；占 RAM |

MCU 上常见：**起步用 A，要做可抢占内核再上 B**。无论 A/B，**内核栈地址都不进该任务的 PMP 用户窗口**。

### 1.7.5 调度切换流程（核心）

```
schedule():
  1. 关中断（MIE=0）或进入调度临界区
  2. 选 next TCB
  3. 保存 prev：通用寄存器 → prev 用户上下文区（或内核栈帧）
  4. pmp_load(&next->pmp)     ← 只改任务相关槽
  5. 恢复 next：mepc/mstatus/sp
  6. mret → 进入 U-mode 跑 next
```

非法访问：U 踩到别的任务或内核区 → **Load/Store/Instruction access fault** → `mcause` + `mtval` → 杀任务或 panic。

### 1.7.6 Syscall 边界：为什么必须封装

做得「完美」时，**上层几乎不能直接碰硬件**，必须：

```
U-mode App
   │  ecall (a7=syscall_nr, a0..a5=args)
   ▼
M-mode trap_handler
   │  校验参数、拷贝（若跨窗）
   ▼
kernel service：驱动 / IPC / 延时 / 创建任务 ...
   │
   ▼
mret 回 U
```

这和 Linux 一样：

| Linux | 你们的 RTOS+PMP |
|-------|-----------------|
| `syscall` / SVC | `ecall` |
| 内核栈（每线程） | 内核栈（全局或每 TCB） |
| `copy_from_user` | 校验指针 ∈ 当前任务 PMP 窗 |
| 驱动只在内核 | 驱动只在 M-mode |
| 用户看不到内核栈 | **PMP 禁止 U 访问内核栈** |

**必须 syscall 化的典型接口**：

- 延时 / yield / 睡眠
- 队列、信号量、互斥（内部碰共享内核对象）
- 读写 UART (Universal Asynchronous Receiver/Transmitter)/SPI (Serial Peripheral Interface)/I2C (Inter-Integrated Circuit)/电机寄存器
- 申请堆（从内核堆划一块并改 PMP 或从任务私有 heap 分配）
- 创建/删除任务、改优先级
- 映射共享 IPC 窗（谨慎）

**可以留在 U 的**：纯计算、任务私有数据、只读共享表。

> **工程结论**：PMP 隔离一旦认真做，RTOS 会从「库式调度器」演进成「微内核雏形」——syscall 表、参数校验、内核栈、驱动下沉是代价；收益是任务内存互踩、外设乱写被硬件拦住。不要幻想「又隔离又让应用直接写寄存器」。

### 1.7.7 中断与驱动放哪

```
推荐：
  硬中断入口永远在 M-mode（mtvec）
  中断里：清硬件 → 投递到内核队列 → 可选唤醒 U 任务
  长逻辑：U 任务通过阻塞 syscall 取事件（类似 read）

不推荐：
  让 U 直接注册 ISR 写寄存器（破坏隔离）
```

若必须「用户驱动」：做成 **能力型 syscall**（`dev_read(fd,...)`），fd 由内核在 open 时绑定到允许的 MMIO，仍由 M 代写。

### 1.7.8 最简关键代码示例

#### （1）PMP 加载（切换时调用）

```c
/* 极简：用 pmpcfg0 + pmpaddr0..；真实芯片按槽位偏移改写 */
static inline void pmp_write_slot(int i, uint32_t addr_napot, uint8_t cfg)
{
    /* 示意：真实实现用 csr 读写宏按 i 选择 pmpaddrN / pmpcfg 字节 */
    extern void arch_pmp_set(int idx, uint32_t addr, uint8_t cfg);
    arch_pmp_set(i, addr_napot, cfg);
}

void pmp_load_task(const struct task_pmp *p)
{
    /* 假设任务窗占用硬件槽 5..9 */
    for (int i = 0; i < PMP_SLOTS_PER_TASK; i++)
        pmp_write_slot(5 + i, p->e[i].addr, p->e[i].cfg);

    __asm__ volatile ("fence" ::: "memory");
}

/* NAPOT 编码：size 必须 2^n，基址对齐 size */
static uint32_t pmp_napot_encode(uintptr_t base, uintptr_t size)
{
    /* addr = (base >> 2) | ((size >> 3) - 1)  —— 标准 NAPOT 公式，实现时对照手册 */
    return (uint32_t)((base >> 2) | ((size >> 3) - 1));
}

#define PMP_R  (1u << 0)
#define PMP_W  (1u << 1)
#define PMP_X  (1u << 2)
#define PMP_A_NAPOT (3u << 3)
```

#### （2）创建任务时登记窗口

```c
int task_create_isolated(struct tcb *t,
                         void (*entry)(void *), void *arg,
                         void *code, size_t code_sz,
                         void *data, size_t data_sz,
                         void *ustack, size_t ustack_sz)
{
    t->pmp.e[0] = (struct pmp_entry){
        .addr = pmp_napot_encode((uintptr_t)code, code_sz),
        .cfg  = PMP_R | PMP_X | PMP_A_NAPOT
    };
    t->pmp.e[1] = (struct pmp_entry){
        .addr = pmp_napot_encode((uintptr_t)data, data_sz),
        .cfg  = PMP_R | PMP_W | PMP_A_NAPOT
    };
    t->pmp.e[2] = (struct pmp_entry){
        .addr = pmp_napot_encode((uintptr_t)ustack, ustack_sz),
        .cfg  = PMP_R | PMP_W | PMP_A_NAPOT
    };
    /* e[3], e[4] 可拆更大 code/data */

    t->mepc = (uint32_t)entry;
    t->mstatus = /* MPP=U, MPIE=1 ... */ 0; /* 填 arch 宏 */
    t->sp_user = (uint32_t *)((uintptr_t)ustack + ustack_sz);
    /* 参数约定：a0=arg，由第一次 mret 前写入 */
    return 0;
}
```

#### （3）调度切换片段

```c
extern struct tcb *g_curr;

void schedule(void)
{
    struct tcb *prev = g_curr;
    struct tcb *next = pick_next_task();
    if (next == prev)
        return;

    csr_clear_mie();                 /* 临界区 */
    context_save_user(prev);         /* 保存 U 寄存器到 prev */
    pmp_load_task(&next->pmp);       /* ★ 切 PMP */
    g_curr = next;
    context_restore_user(next);      /* 恢复 mepc/sp 等并 mret */
    /* context_restore_user 不返回 */
}
```

#### （4）Trap / Syscall 入口（内核栈 + 参数校验）

```c
#define SYS_YIELD   1
#define SYS_DELAY   2
#define SYS_UART_WR 3
#define SYS_QUEUE_SEND 4

/* mtvec → 这里；硬件已切到 M-mode */
void trap_handler(void)
{
    uint32_t cause = csr_read_mcause();
    uint32_t tval  = csr_read_mtval();

    if ((int32_t)cause < 0) {
        /* 中断：PLIC claim → 驱动 → complete；必要时 ready 某任务 */
        handle_interrupt(cause);
        return;
    }

    if (cause == CAUSE_ECALL_U) {          /* Environment call from U */
        uint32_t nr = /* a7 */ g_curr_regs.a7;
        uintptr_t a0 = g_curr_regs.a0;
        /* 返回时 mepc += 4 跳过 ecall */
        g_curr_regs.a0 = syscall_dispatch(nr, a0, g_curr_regs.a1,
                                          g_curr_regs.a2, g_curr_regs.a3);
        csr_write_mepc(csr_read_mepc() + 4);
        return;
    }

    if (cause == CAUSE_LOAD_ACCESS ||
        cause == CAUSE_STORE_ACCESS ||
        cause == CAUSE_FETCH_ACCESS) {
        /* PMP 违规：隔离生效 */
        on_task_fault(g_curr, cause, tval);  /* kill / restart */
        schedule();
        return;
    }

    panic("unexpected trap");
}

static int user_ptr_ok(uintptr_t p, size_t n)
{
    /* 检查 [p,p+n) 落在 g_curr->pmp 的可读写窗内 */
    return pmp_range_belongs_to_task(g_curr, p, n);
}

long syscall_dispatch(uint32_t nr, uintptr_t a0, uintptr_t a1,
                      uintptr_t a2, uintptr_t a3)
{
    switch (nr) {
    case SYS_YIELD:
        schedule();
        return 0;
    case SYS_DELAY:
        return sys_delay_ms((uint32_t)a0);
    case SYS_UART_WR:
        if (!user_ptr_ok(a0, a1))
            return -1;                 /* 类似 -EFAULT */
        return uart_write_kernel((void *)a0, (size_t)a1); /* M 里写 MMIO */
    case SYS_QUEUE_SEND:
        return sys_queue_send((int)a0, a1, a2);
    default:
        return -1;
    }
}
```

#### （5）用户态侧（只能 ecall）

```c
/* uapi.h —— 给应用的唯一口径 */
static inline long syscall1(long nr, long a0)
{
    register long a7 __asm__("a7") = nr;
    register long a0r __asm__("a0") = a0;
    __asm__ volatile ("ecall" : "+r"(a0r) : "r"(a7) : "memory");
    return a0r;
}

void uart_print(const char *s, size_t n)
{
    syscall1(SYS_UART_WR, /* 实际要传指针+长度，用 syscall2 */ 0);
    (void)s; (void)n;
}

void task_main(void *arg)
{
    for (;;) {
        /* 纯计算可本地做 */
        syscall1(SYS_DELAY, 10);   /* 绝不能直接摸定时器寄存器 */
    }
}
```

#### （6）启动时锁死内核区（示意）

```c
void pmp_init_kernel_static(void)
{
    /* 槽 0..4：ROM RX、内核 RX、内核 RW、MMIO 对 U 关闭等 */
    /* 对关键槽置 L 位：锁定后即使用户逃逸到 M 也难改（视实现） */
    pmp_write_slot(3, napot_kernel_text, PMP_R | PMP_X | PMP_A_NAPOT | PMP_L);
    pmp_write_slot(4, napot_kernel_ram,  PMP_R | PMP_W | PMP_A_NAPOT | PMP_L);
    /* U 默认匹配不到这些区域 → access fault */
}
```

### 1.7.9 方案分层（避免一口吃成 Linux）

| 阶段 | 内容 | 工作量 |
|------|------|--------|
| **MVP** | 仍 M-mode 跑任务，仅用 PMP **静态**分区（任务区互不重叠）+ 故障检测 | 小 |
| **L1** | 调度时切换 PMP；任务仍 M-mode（弱隔离，防呆） | 中 |
| **L2（推荐目标）** | U-mode 任务 + ecall + 驱动在 M + 内核栈隔离 | 大 |
| **L3** | 每任务内核栈、fd 模型、共享内存能力、看门狗杀任务 | 接近嵌入式微内核 |

> 可表述为：我们计划从 L1→L2；L2 起接受 syscall 化代价，换硬件级任务隔离，服务车规/机器人功能安全叙事。

### 1.7.10 与 Linux / TrustZone 一句话对照

| | Linux | TrustZone | RTOS+PMP(L2) |
|--|-------|-----------|--------------|
| 隔离对象 | 进程 | Secure/NS World | 任务 |
| 硬件 | MMU+页表 | TZASC+EL | **PMP** |
| 进入内核 | syscall | SMC | **ecall** |
| 内核栈 | 每线程 | Monitor/TEE 栈 | 全局或每 TCB |
| 复杂度 | 高 | 高 | 中→高（随 L2/L3） |

---

## 1.9 进程状态与 schedule 触发

### task 状态

```
        ┌──────────┐
   ┌───▶│ RUNNING  │◀───┐
   │    └────┬─────┘    │
   │         │          │
   │    时间片到/被抢占  │ wake_up
   │         │          │
   │    ┌────▼─────┐    │
   │    │ RUNNABLE │────┘
   │    │ (就绪)    │
   │    └────┬─────┘
   │         │ schedule()
   │    ┌────▼─────┐
   └───▶│ BLOCKED  │
        │ (D/S状态) │
        └──────────┘
```

| 状态 | 宏 | 含义 |
|------|-----|------|
| Running | TASK_RUNNING | 正在 CPU 上或就绪 |
| Interruptible Sleep | TASK_INTERRUPTIBLE | 可信号唤醒的阻塞 |
| Uninterruptible Sleep | TASK_UNINTERRUPTIBLE | 不可信号唤醒（D 状态） |
| Stopped | TASK_STOPPED | 被 SIGSTOP |
| Zombie | EXIT_ZOMBIE | 已退出等父进程收尸 |

### schedule() 触发时机

1. **tick 中断**：`scheduler_tick()` → 检查是否需要抢占
2. **主动 yield**：`sched_yield()`
3. **阻塞**：`mutex_lock()`、`wait_event()`、`msleep()`
4. **wake_up**：唤醒高优先级 task
5. **返回用户态**：`TIF_NEED_RESCHED` 标志

---

## 1.10 NUMA 调度（了解即可）

关于 CFS / EAS / NUMA：

- **NUMA**（Non-Uniform Memory Access）：多 socket 系统，每个 CPU 访问本地内存快、远程内存慢
- CFS 的 NUMA 扩展：尽量让 task 在**靠近其内存的 CPU** 上运行
- 机器人芯片一般不会多 socket，了解概念即可

---

## 1.11 调度 debug（ ftrace）

```bash
# 开启 tracing
echo 1 > /sys/kernel/tracing/tracing_on

# 使能调度事件
echo 1 > /sys/kernel/tracing/events/sched/sched_switch/enable
echo 1 > /sys/kernel/tracing/events/sched/sched_wakeup/enable
echo 1 > /sys/kernel/tracing/events/sched/sched_stat_wait/enable

# 查看
cat /sys/kernel/tracing/trace

# 或用 perf
perf sched record -a sleep 5
perf sched latency    # 看调度延迟
perf sched map        # 看 task 在哪些 CPU 迁移

# RT 延迟（PREEMPT_RT 必会）
cyclictest -m -S -p 90 -i 200 -l 100000
```

---

---

## 小结

- 调度 class 按场景分工：CFS 默认公平，RT（FIFO/RR）保截止，EAS 做能效选核。
- PREEMPT_RT 关键改动是 IRQ 线程化与可睡眠锁，把延迟从「不可预期」推向「可测量」。
- 底半部选型看能否睡眠、延迟预算与上下文：softirq / tasklet / workqueue / threaded IRQ。
- 机器人控制常落在 RTOS 或 AMP；Linux PREEMPT_RT 适合「要生态又要较低延迟」的折中。

## 自测

1. CFS 的 vruntime 在表达什么？nice 如何影响它？
2. SCHED_FIFO 与 SCHED_RR 的核心差别是什么？
3. PREEMPT_RT 相对主线最重要的两处改动是什么？量级大概如何描述？
4. 硬中断 / softirq / tasklet / workqueue / threaded IRQ：谁能睡眠？各适用什么？
5. 主线与 PREEMPT_RT 上，spinlock / raw_spinlock / mutex 怎么理解？
6. 何种场景选 PREEMPT_RT，何种场景更该上 RTOS/AMP？
7. RTOS+PMP：切任务要切什么？为何应用不能直接碰内核栈与裸寄存映射？

---

*`01-kernel` · 进程调度*
