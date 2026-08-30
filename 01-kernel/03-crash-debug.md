# Linux 死机分析：oops / panic / hung 与现场取证

> **系列**：`01-kernel`  
> **前置**：对调度与内存有基本印象更佳  
> **相关**：[`01-scheduling.md`](01-scheduling.md) · [`02-memory.md`](02-memory.md)

把死机从「板子挂了」变成可复盘的路径：识别故障类型、读栈、用 crash/ftrace/perf 取证，形成可重复的排查习惯。

**读完应能**：
- 区分 oops / panic / hung / soft lockup
- 按栈与日志定位到驱动或内核路径
- 列出一组常备调试命令

---
## 死机分析与 Debug

## 3.1 内核崩溃类型

| 类型 | 特征 | 严重程度 | 系统状态 |
|------|------|----------|----------|
| **Oops** | 内核错误，打印 backtrace | 中 | 可能继续运行（oops_kill_process） |
| **Panic** | 致命错误，内核停止 | 高 | 完全停止或 reboot |
| **Hung Task** | D 状态 TASK_UNINTERRUPTIBLE 超时 | 中 | 系统卡住但不 panic |
| **Soft Lockup** | 关中断跑 > 20s（默认） | 中 | CPU 不响应 |
| **Hard Lockup** | NMI 检测到 CPU 完全不动 | 高 | CPU 死 |
| **RCU Stall** | RCU grace period 超时 | 中 | 可能后续 panic |
| **Stack Overflow** | 栈底 canary 被踩 | 高 | panic |

---

## 3.2 Oops 分析

### 典型 Oops 日志

```
Unable to handle kernel NULL pointer dereference at virtual address 0000000000000010
Internal error: Oops: 96000005 [#1] PREEMPT SMP
Modules linked in: my_driver(O)
CPU: 2 PID: 1234 Comm: my_app Tainted: G        O
Hardware name: My SoC Platform
pstate: 80400005 (Nzcv daif +PAN -UAO)
pc : my_driver_open+0x24/0x100 [my_driver]
lr : do_open+0x200/0x900
sp : ffffffc012345678
x29: ffffffc0123456a0 x28: ...
```

### 关键字段解读

| 字段 | 含义 |
|------|------|
| **Unable to handle ...** | 错误类型（null deref / paging request / undefined instruction） |
| **Internal error: Oops: 96000005** | ESR 值，查 ARM 手册得具体原因 |
| **CPU: 2 PID: 1234** | 出错的 CPU 和进程 |
| **pc** | Program Counter，出错指令位置 |
| **lr** | Link Register，调用者地址 |
| **sp** | Stack Pointer |
| **Comm** | 进程名 |

### 常见 Oops 原因

| 原因 | 典型 pc/lr 特征 |
|------|-----------------|
| NULL 指针解引用 | virtual address 0x0 或很小 |
| 访问已释放内存 | use-after-free，slab 相关 |
| 栈溢出 | sp 接近栈边界 |
| 未初始化指针 | 随机地址 |
| 驱动缺少 __user 检查 | copy_from/to_user 相关 |

---

## 3.3 Panic 分析

### 典型 Panic 日志

```
Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b
---[ end Kernel panic - not syncing ]---
```

### 常见 Panic 原因

| 原因 | 日志关键词 |
|------|-----------|
| init 进程被杀 | Attempted to kill init |
| BUG_ON / WARN_ON | kernel BUG at xxx |
| 栈溢出 | Stack overflow detected |
| OOM 杀关键进程 | Out of memory: Kill process |
| Watchdog 超时 | Watchdog detected hard LOCKUP |
| 内核数据结构损坏 | list corruption / bad magic |
| 双 free | kfree() detected corrupted memory |

### 栈溢出检测

```c
/* kernel/sched/core.c */
#define STACK_END_MAGIC 0x57AC6E9D

void set_task_stack_end_magic(struct task_struct *tsk)
{
    unsigned long *stackend = end_of_stack(tsk);
    *stackend = STACK_END_MAGIC;  /* canary */
}

/* 调度时检查 */
if (*stack_end != STACK_END_MAGIC)
    panic("Stack overflow detected");
```

**crash 工具中定位**：
```
SP_EL0 = 0xffffffc019013440 → task_struct
查看 stack 底部是否 0x57AC6E9D
```

---

## 3.4 Hung Task

### 特征

- 进程处于 **D 状态**（TASK_UNINTERRUPTIBLE）
- 超过 `hung_task_timeout_secs`（默认 120 秒）
- 内核打印 warning 但不 panic（除非 `panic_on_warn`）

### 典型日志

```
INFO: task my_app:1234 blocked for more than 120 seconds.
      Not tainted 5.10.0 #1
"echo 0 > /proc/sys/kernel/hung_task_panic" to disable this message.
my_app          D 0   1234   1233 0x00004000
Call trace:
 __switch_to+0x...
 do_block_ioctl+0x...
 vfs_ioctl+0x...
```

### 常见原因

| 原因 | 场景 |
|------|------|
| 驱动 ioctl 等硬件响应 | 硬件 hang、寄存器无 ack |
| 等 I/O 完成 | eMMC/SD 超时 |
| 等锁 | mutex/rwsem 持有者 hang |
| 等内存回收 | 等 direct reclaim |

### 与你经验的关联

> boot 随机死机，大概率 DDR 问题  
> migration/0 线程跑 19+ 秒 → runq 异常

---

## 3.5 Soft Lockup / Hard Lockup

### Soft Lockup

- 某个 CPU **关中断**运行超过 `watchdog_thresh`（默认 20s）
- 其他 CPU 的 watchdog 检测到并打印

```
watchdog: BUG: soft lockup - CPU#2 stuck for 22s! [migration/2:45]
```

### Hard Lockup

- CPU **完全不动**（连 perf counter 都不增）
- NMI watchdog 检测

### 常见原因

| 类型 | 原因 |
|------|------|
| Soft | 长临界区、spinlock 死等、关中断太久 |
| Hard | 硬件问题、固件 bug、极端 interrupt storm |

---

## 3.6 Crash 工具分析

### 基本流程

```
1. sys          → 全局信息
2. kmem -i      → 内存状态
3. irq -s       → 中断状态
4. runq -m      → 各 CPU 运行队列
5. log          → dmesg
6. bt <pid>     → 调用栈
7. trace        → ftrace（需插件）
```

### sys 命令输出解读

```
sys
  cpu_count: 8
  uptime: 20:44:41
  kernel_version: 5.10.0
  memory: 4GB
  panic_string: "Oops: 96000005 [#1] PREEMPT SMP"
```

### runq -m 关键

```
CPU 3-7 idle 线程运行时间 = 20:44:41 → 系统 uptime
migration/0 运行 19.867 秒 → 异常！正常调度 ms 级

分析：migration 线程负责 CPU 间负载均衡
      连续跑 19s 说明某个 CPU 的 runqueue 有问题
      或某个 task 无法被迁移（affinity 限制/pin 住）
```

### bt 命令

```
bt 1234          → 查看 PID 1234 的调用栈
bt 1234 5678     → 同时查看多个

限制：
  - 阻塞/睡眠/runnable 任务：直接 bt
  - 正在 running 的任务：需要从 cpu_context 或栈帧手动恢复
```

### Running Task 栈恢复

```
方法 1：从 cpu_context 获取 sp
方法 2：从解析好的 cmm 获取 sp_el1
方法 3（推荐，成功率最高）：
  → 在任务栈或中断栈中找最顶层栈帧
  → 按 ATPCS 规则手动恢复各层调用栈

ATPCS 规则：
  - x29 (FP) 链式指向上一帧
  - x30 (LR) 是返回地址
  - SP 指向当前栈帧
  - 每帧结构：[x29 prev FP][x30 LR][局部变量...]
```

---

## 3.7 其他 Debug 工具

### ftrace

```bash
# 函数追踪
echo function > /sys/kernel/tracing/current_tracer
echo my_func > /sys/kernel/tracing/set_ftrace_filter
echo 1 > /sys/kernel/tracing/tracing_on
cat /sys/kernel/tracing/trace

# 延迟追踪
echo function_graph > /sys/kernel/tracing/current_tracer
```

### perf

```bash
perf record -a -g sleep 10    # 采样
perf report                     # 看热点
perf top                        # 实时

# 火焰图
perf record -F 99 -a -g -- sleep 30
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

### kmemleak / KASAN

```bash
# 启动参数
kmemleak=on          # 内存泄漏检测
kasan=on             # 地址 sanitizer（检测 UAF/overflow）
```

---

## 3.8 Boot 死机分析

### 随机 boot 死机 → 大概率 DDR

```
排查步骤：
1. 是否 temperature/voltage 相关？→ 拷机不同环境
2. DDR training 是否 pass？→ 看 boot log 中 DDR init
3. 是否特定 bin/频率？→ DVFS 相关
4. 是否 memtest 可复现？→ 硬件问题
5. 是否 U-Boot 阶段就 hang？→ 排除 kernel 问题
```

### DVFS 相关

- 升频时电压不足 → 随机 bit flip → 各种奇怪 crash
- 排查：固定频率跑、降频测试、看 PMIC 日志

---

## 3.9 死机分析通用方法论

```
1. 复现
   ├── 固定条件？随机？特定负载？
   └── 保留 crash 现场（ramdump/pstore）

2. 分类
   ├── Oops → 看 pc/lr/backtrace
   ├── Panic → 看 panic string
   ├── Hang → 看 runq/hung task
   └── Reboot → 看 last_kmsg/pstore

3. 定位
   ├── 哪个 CPU/PID/模块
   ├── 什么操作触发
   └── 有无规律（时间/负载/温度）

4. 深入
   ├── crash 工具分析 ramdump
   ├── ftrace/perf 复现
   └── 代码 review 可疑路径

5. 验证
   └── 修复 → 回归 → 压力测试
```

---

---

## 小结

- 先分类：oops、panic、hung task、soft/hard lockup，再选工具。
- 栈与日志是主线索；magic number / 栈溢出要会辨认。
- crash / ftrace / perf 解决不同问题：事后镜像、时序、热点。
- 可重复的「抓数清单」比临场猜因更重要。

## 自测

1. Oops 与 Panic 的差别与常见诱因？
2. Hung Task 通常处于什么状态？如何往下追？
3. crash 里 sys / runq / bt 各自看什么？
4. 如何发现栈溢出？常见检测手段有哪些？
5. boot 阶段偶发死机，你的排查顺序是什么？

---

*`01-kernel` · 死机分析*
