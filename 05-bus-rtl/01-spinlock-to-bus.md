# 从 spin_lock 到总线：AXI (Advanced eXtensible Interface) Exclusive 与 AHB (Advanced High-performance Bus) HLOCK

> **系列**：`05-bus-rtl`  
> **前置**：[`../01-kernel/01-scheduling.md`](../01-kernel/01-scheduling.md) 中的锁与底半部概念  
> **相关**：[`../01-kernel/04-android-soc-scheduling.md`](../01-kernel/04-android-soc-scheduling.md) · [`../04-riscv-core/03-memory-bus.md`](../04-riscv-core/03-memory-bus.md)（普通 Load 路径；本文专攻原子/Exclusive）

很多人以为自旋锁会「锁住总线」。在 ARM + **AXI** 上，Linux spinlock 走的是 LDXR/STXR (Load-Exclusive / Store-Exclusive) → Exclusive 事务 → Local/Global Monitor，并不锁死整条总线。  
经典 **AHB** 则另有一套：`HLOCK` 通过占住 grant 串行化多 master 的 RMW。本文把两条路径都讲清，并对照「各挡的是什么」。

**读完应能**：
- 解释为何 AXI4 不靠锁总线做 spinlock
- 画出双核抢锁时 Monitor 与 BRESP 的关系
- 对比 HLOCK 与 Exclusive 挡的是什么

---
## 目录

1. [破除误解：x86 的 LOCK ≠ ARM 的 spinlock](#1-破除误解x86-的-lock--arm-的-spinlock)
2. [第一层：Linux spinlock 的软件栈](#2-第一层linux-spinlock-的软件栈)
3. [第二层：LDXR/STXR 如何变成 AXI 事务](#3-第二层ldxrstxr-如何变成-axi-事务)
4. [第三层：Exclusive Monitor——真正的"锁"在哪里](#4-第三层exclusive-monitor真正的锁在哪里)
5. [第四层：完整的多核竞争时序](#5-第四层完整的多核竞争时序)
6. [认知纠偏：三个常见误区](#6-认知纠偏三个常见误区)
7. [AXI 五个通道速览](#7-axi-五个通道速览)
8. [关键场景推演：没有 Global Monitor 会怎样](#8-关键场景推演没有-global-monitor-会怎样)
9. [波形：从指令到引脚](#9-波形从指令到引脚)
10. [对照：AHB HLOCK vs AXI Exclusive](#10-对照ahb-hlock-vs-axi-exclusive)
11. [附录 A：AxPROT](#附录-aaxprot-保护位含义)
12. [附录 B：抓波与断言清单](#附录-b抓波与断言清单)
13. [小结](#小结)
14. [自测](#自测)

---

## 1. 破除误解：x86 的 LOCK ≠ ARM 的 spinlock

在 x86 上，`spin_lock()` 底层确实会 emit 一个带 `LOCK` 前缀的指令（如 `LOCK BTS`），这个前缀在早期的 x86 多核系统上确实会**锁住前端总线（FSB）或 Intel 的 UPI**，阻止其他 CPU 访问内存。

但 ARM/Linux 走的**完全是另一条路**：

| 架构 | 机制 | 实现思路 |
|------|------|----------|
| **x86** | `LOCK` 前缀 → 锁总线 | 原子地完成 read-modify-write |
| **ARM** | `LDXR/STXR`（或老的 `SWP`）→ 发 AXI **exclusive 事务** | 由 exclusive monitor 判定成败 → 失败重试 |

> **关键差异**：AXI 协议**不支持**"锁住整个总线"这种粗暴做法。AXI 是 out-of-order、pipeline 的高性能总线，把总线锁死等于自废武功。所以 ARM 设计了 **exclusive 访问 + 本地 monitor** 的优雅机制。

---

## 2. 第一层：Linux spinlock 的软件栈

以 ARM64 为例，`spin_lock()` 的调用链大致是：

```
spin_lock()
  └─ raw_spin_lock()
       └─ arch_spin_lock()         // 架构相关
            └─ 核心循环（汇编）：
                1:  LDXR  w, [lock]      // 独占加载锁变量
                    CBNZ  w, 2f          // 非 0 表示已被占用，跳到 2
                    STXR  w1, #1, [lock] // 独占存储，尝试置 1
                    CBNZ  w1, 1b         // 若 STXR 失败（w1≠0），重试
                    DMB                    // 内存屏障
                    ... 进入临界区 ...
                2:  WFE                  // 等待事件，省电
                    B    1b
```

**精髓在这两句**：

- **`LDXR`**：带"独占"语义的加载，同时**在当前 CPU 的 exclusive monitor 里标记这块地址**
- **`STXR`**：带"独占"语义的存储，硬件会检查：从上次 `LDXR` 到现在，这块地址**有没有被别人动过**？
  - 没人动过 → 存储成功，`w1=0`，拿到锁
  - 被人动过 → 存储失败，`w1≠0`，回到 `1:` 重试

这就是所谓的 **LL/SC（Load-Link/Store-Conditional）** 机制在 ARMv8 上的实现：`LDXR`=Load Exclusive，`STXR`=Store Exclusive。

---

## 3. 第二层：LDXR/STXR 如何变成 AXI 事务

CPU 核心执行 `LDXR/STXR` 时，AXI 总线上的表现如下：

| 指令 | AXI 通道 | 关键信号 | 值 | 含义 |
|------|----------|----------|----|------|
| `LDXR` | AR | `ARLOCK[1:0]` | `2'b01` (Exclusive) | 发起**独占读** |
| `LDXR` | R | `RRESP[1:0]` | `2'b00` (OKAY) | 读成功，monitor 记录地址 |
| `STXR` | AW | `AWLOCK[1:0]` | `2'b01` (Exclusive) | 发起**独占写** |
| `STXR` | W | `WDATA` | `0x1` | 写入锁值 |
| `STXR` | B | `BRESP[1:0]` | `EXOKAY` / `OKAY` | 写成功 = 拿到锁 |

**重点**：AXI4 的 `ARLOCK[1:0]` / `AWLOCK[1:0]` 是 **2 位宽**，支持三种模式：

| `AxLOCK[1:0]` | 模式 | 说明 |
|---------------|------|------|
| `2'b00` | Normal access | 普通访问 |
| `2'b01` | **Exclusive access** | **独占访问，Linux spinlock 用这个** |
| `2'b10` | Locked access | **锁访问，AXI4 已废弃，仅 AXI3 支持** |
| `2'b11` | 保留 | — |

> **AXI4 规范明确移除了 Locked access**，因为锁总线会严重破坏 AXI 的流水线性能。现代 ARM 多核系统里的 spinlock，**100% 依赖 Exclusive 模式**，不是 Locked 模式。

---

## 4. 第三层：Exclusive Monitor——真正的"锁"在哪里

**"锁"不在总线上，而在每个 CPU 核心里的 Exclusive Monitor。** ARM 架构定义了两级 monitor：

### 4.1 Local Monitor（每个 CPU 核心私有）

- 位于 CPU 核心内部，监听该核心的 `LDXR`/`STXR`
- 记录：上一次 `LDXR` 访问的地址范围
- 当发生以下任一情况时，**monitor 状态从 Exclusive 变为 Open**：
  - 该 CPU 执行了 `STXR` 且成功
  - 该 CPU 执行了 `STR`/`STXR` 且成功
  - 其他 CPU 对该地址发起了**任何写事务**（包括非独占写）
  - 该 CPU 执行了上下文切换相关操作
- `STXR` 执行时，硬件检查 monitor 状态：
  - 仍是 Exclusive → 写放行，`STXR` 返回 0（成功）
  - 已变为 Open → 写被丢弃，`STXR` 返回 1（失败）

### 4.2 Global Monitor（系统级，可选）

- 位于总线互联（Interconnect）里
- 保证**不同 CPU 对同一地址**的 exclusive 访问互斥
- 当一个 CPU 成功完成 exclusive 写，global monitor 会让其他 CPU 的 local monitor 失效

### 4.3 总线从设备（如 SRAM (Static Random-Access Memory) 控制器）的角色

- **对于 exclusive 读**：正常返回数据，并记录这个 master 的 exclusive 访问
- **对于 exclusive 写**：检查从上次 exclusive 读到现在，这块地址有没有被其他 master 写过
  - 没有被碰过 → 接受写，`BRESP = EXOKAY`
  - 被碰过 → 拒绝写，`BRESP = OKAY`（但不是 EXOKAY），CPU 看到 `STXR` 失败

---

## 5. 第四层：完整的多核竞争时序

假设 **CPU0 和 CPU1 竞争同一把 spinlock（地址 0x8000）**，且系统**有** Global Monitor：

```
CPU0                                      CPU1
────                                      ────
LDXR x0, [0x8000]  ──ARLOCK=Excl──▶  Global Monitor 记录 CPU0@0x8000
x0 = 0 (锁空闲)
                                     LDXR x0, [0x8000]  ──ARLOCK=Excl──▶ Global Monitor 记录 CPU1@0x8000
                                     x0 = 0 (锁空闲)
STXR x1, #1, [0x8000] ──AWLOCK=Excl──▶ 
                                       Global Monitor 检查：CPU0 是第一个独占者 → 写成功
                                       BRESP = EXOKAY
                                       x1 = 0  ✓ 拿到锁！
                                     STXR x1, #1, [0x8000] ──AWLOCK=Excl──▶
                                       Global Monitor 检查：CPU0 已经改过这块地址 → 写失败
                                       BRESP = OKAY (非 EXOKAY)
                                       x1 = 1  ✗ 未拿到锁，回到 LDXR 重试
```

**关键点**：

1. 总线**全程没有"锁死"**——CPU1 的 `LDXR` 和 `STXR` 都能正常发到总线上
2. "锁"的语义由 **Global Monitor + 每个 CPU 的 Local Monitor** 共同保证
3. CPU1 的 `STXR` 失败不是因为总线不让它写，而是因为 **monitor 判定它失去了独占权**
4. CPU1 失败后走 `WFE` 进入低功耗等待，等 CPU0 释放锁时发 `SEV` 唤醒

---

## 6. 认知纠偏：三个常见误区

### 误区 1："spinlock 会锁 AXI 总线" —— ❌ 错

AXI4 根本不支持 Locked access。就算在 AXI3 设备上发了 `ARLOCK=2'b10`，也只是让互联在 locked 事务期间不调度其他 master，**这不是 Linux spinlock 用的机制**。Linux 用的是 Exclusive。

### 误区 2："AXI 上有个 HLOCK 类似的信号" —— ❌ 错

AHB 确实有 `HLOCKx`（**占住 grant、串行化 RMW**），但 AXI 的 `ARLOCK/AWLOCK` 在 AXI4 里**只剩 Exclusive 一种有意义的值**。  
AXI 设计哲学：**不在总线层面提供互斥原语，互斥交给 Exclusive Monitor**。  
两种机制对照与 AHB 波形见 [§10](#10-对照ahb-hlock-vs-axi-exclusive)。

### 误区 3："独占访问会一直占用总线" —— ❌ 错

`LDXR` 和 `STXR` 是两个独立 AXI 事务，中间可以插入其他 master 的访问。正是这种"非锁定"特性，让 AXI 的 out-of-order 能力得以发挥。代价是：如果竞争激烈，`STXR` 可能反复失败（starvation），但硬件保证最终会成功。

### 补充："Exclusive 访问一定要从设备支持" —— ✅ 对，但有 fallback

如果从设备不支持 exclusive（返回不了 EXOKAY），Linux 会退化到 **`SWP` 指令**（ARMv7 老办法）或 **`LDAXR/STLXR` 加循环**。现代 AXI 从设备基本都支持 exclusive，因为这是 ARM 生态的标配。

---

## 7. AXI 五个通道速览

AXI 采用**通道分离、并行流水线**设计，共 **5 个独立通道**，分两组：

- **读组**：读地址（AR） + 读数据（R）
- **写组**：写地址（AW） + 写数据（W） + 写响应（B）

每组都使用独立的 VALID/READY 握手协议，支持 out-of-order 和多 outstanding 事务。

### 7.1 读地址通道（AR）

master → slave，携带一次读传输的所有控制信息。

| 信号 | 说明 |
|------|------|
| `ARID` | 读事务 ID，用于标识返回数据的归属 |
| `ARADDR` | 起始地址 |
| `ARLEN` | 突发长度（`ARLEN+1` = 拍数） |
| `ARSIZE` | 每拍数据宽度（0=8bit, 1=16bit, 2=32bit, …） |
| `ARBURST` | 突发类型：00=FIXED, 01=INCR, 10=WRAP |
| `ARLOCK` | 访问类型：00=Normal, 01=Exclusive, 10=Locked(废弃) |
| `ARCACHE` | 缓存属性（bufferable/cacheable/modifiable/allocate） |
| `ARPROT` | 保护属性（特权级、安全态、数据/指令） |
| `ARQOS` | 服务质量等级（用于 QoS 仲裁） |
| `ARVALID` / `ARREADY` | 握手信号 |

### 7.2 读数据通道（R）

slave → master，返回读数据和响应。

| 信号 | 说明 |
|------|------|
| `RID` | 事务 ID，与 `ARID` 匹配，用于 out-of-order 重排序 |
| `RDATA` | 读数据 |
| `RRESP` | 响应：00=OKAY, 01=EXOKAY, 10=SLVERR, 11=DECERR |
| `RLAST` | 最后一拍标志 |
| `RVALID` / `RREADY` | 握手信号 |

> 读数据通道支持**乱序返回**：不同 ID 的事务可以交错返回；同一 ID 的事务必须按顺序返回。

### 7.3 写地址通道（AW）

master → slave，信号集与 AR 通道一一对应（前缀 `AR` → `AW`）：

`AWID`, `AWADDR`, `AWLEN`, `AWSIZE`, `AWBURST`, `AWLOCK`, `AWCACHE`, `AWPROT`, `AWQOS`, `AWVALID`, `AWREADY`

### 7.4 写数据通道（W）

master → slave，发送写数据。**写数据可以和写地址并行发送**（AXI 流水线优势）。

| 信号 | 说明 |
|------|------|
| `WDATA` | 写数据 |
| `WSTRB` | 字节选通信号，每位对应一个字节 |
| `WLAST` | 最后一拍标志 |
| `WVALID` / `WREADY` | 握手信号 |

> **AXI4 关键变化**：写数据通道不再有 `WID`，写地址和写数据必须严格按顺序对齐——master 必须在最后一个写数据拍（`WLAST`）之后才能发送下一个写地址。

### 7.5 写响应通道（B）

slave → master，返回写传输的完成状态。

| 信号 | 说明 |
|------|------|
| `BID` | 事务 ID，与 `AWID` 匹配 |
| `BRESP` | 响应：00=OKAY, 01=EXOKAY, 10=SLVERR, 11=DECERR |
| `BVALID` / `BREADY` | 握手信号 |

> 写响应总是在**整个突发传输结束后**才返回。

### 7.6 响应编码速查

| `xRESP` | 含义 |
|---------|------|
| `00` OKAY | 正常完成 |
| `01` EXOKAY | **Exclusive 访问成功**（spinlock 判定成败的关键！） |
| `10` SLVERR | 从设备错误 |
| `11` DECERR | 译码错误（地址无对应 slave） |

> **记住**：`STXR` 是否成功，CPU 看的就是返回的 `BRESP` 是否为 `EXOKAY`。这是 spinlock 与 AXI 协议的交汇点。

---

## 8. 关键场景推演：没有 Global Monitor 会怎样

这是理解 Global Monitor 必要性的**核心场景**。假设系统中**没有** Global Monitor（互联是简单转发器，不实现 exclusive 监视逻辑）。

### 8.1 前提：只有 Local Monitor 时

每个 CPU 核心只有 **Local Monitor**，它**只能感知本 CPU 内部的 store**，**无法感知其他 CPU 的写入**。

Local Monitor 规则：
- 执行 `LDXR` 后 → 进入 **Exclusive** 状态，记录地址
- 本 CPU 对该地址执行任意 store → 变为 **Open**
- `STXR` 时检查状态：Exclusive → 成功(0)；Open → 失败(1)

**关键缺陷**：Local Monitor 完全不知道**其他 CPU 对该地址的写入**。

### 8.2 灾难推演

```
Cycle N-1:  CPU0 LDXR [0x8000] → LocalMonitor0 = Exclusive(0x8000)
            CPU1 LDXR [0x8000] → LocalMonitor1 = Exclusive(0x8000)

Cycle N:    CPU0 发出 STXR [0x8000], data=1
            CPU1 同时发出 STXR [0x8000], data=1
```

**两个 CPU 的 Local Monitor 各自判断**：

- **CPU0**：从 N-1 到 N，CPU0 自身没执行过 store，Local Monitor 仍是 Exclusive → `STXR` 检查通过 → **返回成功（x1=0）**
- **CPU1**：同理，Local Monitor 也是 Exclusive → `STXR` 检查通过 → **返回成功（x1=0）**

**结果**：两个 CPU 都认为拿到了锁，同时进入临界区 → **spinlock 互斥性崩溃**。

### 8.3 Global Monitor 如何拯救

- CPU0 的 `LDXR` → Global Monitor 记录：CPU0 对 0x8000 有独占权
- CPU1 的 `LDXR` → Global Monitor 记录：CPU1 也对 0x8000 有独占权（允许多个 master 同时持有 exclusive 权限）
- CPU0 的 `STXR` 到达 → 检查通过 → **接受写，并清除其他 master 的独占权限** → 返回 `EXOKAY`
- CPU1 的 `STXR` 到达 → 权限已被清除 → **拒绝写** → 返回 `OKAY`（非 EXOKAY）→ CPU1 重试

> 即使两个 `STXR` 同一拍到达互联，Global Monitor 也会按仲裁顺序串行化处理，**保证只有一个得到 `EXOKAY`**。

### 8.4 结论对照

| 有无 Global Monitor | 同时 STXR 的结果 | 原因 |
|---------------------|------------------|------|
| ✅ 有 | 只有一个成功，另一个失败 | Global Monitor 串行化，清除其他 master 权限 |
| ❌ 无 | **两个都成功，锁被同时获取** | Local Monitor 互相不可见 |

**所以，Global Monitor 是 AXI exclusive 访问机制的基石。** 任何支持多核 spinlock 的 AXI 系统，其互联或内存控制器**必须实现 Global Monitor**。

## 9. 波形：从指令到引脚

### 9.1 读事务（无等待，单拍）

```
CLK      ██░░██░░██░░██░░██░░
ARVALID  ____████████________
ARREADY  ________████________
ARADDR   ______X Addr X______
RVALID   ____________████____
RREADY   ________________████
RDATA    ____________X Data X
RLAST    ____________████____ (单拍时 RLAST=1)
```

### 9.2 写事务（4 拍突发，无等待）

```
CLK      ██░░██░░██░░██░░██░░██░░██░░
AWVALID  ____████____________________
AWREADY  ________████________________
AWADDR   ______X Addr X______________
WVALID   ____________████████████████
WREADY   ________________████████████
WDATA    ____________X D0 X D1 X D2 X D3
WLAST    ____________________________████
BVALID   ________________________________████
BREADY   ________________________________████
BRESP    ________________________________X OK X
```

### 9.3 双核抢锁（Exclusive + Global Monitor）

对应软件：两核都对同一 `lock` 做 `LDXR`→见 0→`STXR` 写 1。总线**全程不锁死**；成败看 Monitor + `BRESP`。

设定：`lock=0x80001000`，初值 0；CPU0 `AxID=0`，CPU1 `AxID=1`；单 beat、裸 AXI（无 cache）。

```
CLK     1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16

════ CPU0：ExRd 成功 + ExWr 成功 ═══════════════════════════
ARVALID0 / ARLOCK=1 / ARID=0 / ADDR=1000
         ▃███
RDATA=0  RRESP=EXOKAY(或 OKAY，视实现)
              ▃███
AWLOCK=1 / WDATA=1
                    ▃███
BRESP=EXOKAY ★ 提交写，lock→1
                         ▃███

════ CPU1：也可 ExRd 到 0；ExWr 失败 ═══════════════════════
ARVALID1 / ARLOCK=1 / ARID=1          （AXI 不挡别人读）
           ▃███
RDATA=0
                ▃███
AWLOCK=1 / WDATA=1（晚于 CPU0）
                              ▃███
BRESP=OKAY ★ 独占失败，内存仍为 1
                                   ▃███

════ Global Monitor（同拍概念）═════════════════════════════
~3   {M0,addr} valid
~5   {M1,addr} valid（可同时「感兴趣」）
~11  M0 ExWr 成功 → 提交；清其它 master 对该 addr 的 valid
~15  M1 ExWr → 自己 valid 已无 → OKAY，不改数据
```

软件结果：CPU0 `STXR` 成功持锁；CPU1 `STXR` 失败后重试 `LDXR`（会读到 1）。

压缩成时间轴对照（与上表同一故事）：

```
时间 →  t0      t1      t2      t3      t4      t5
CPU0 AR: LDXR Excl ████
CPU0 R:           ████  (data=0)
CPU1 AR:        LDXR Excl ████
CPU1 R:                 ████  (data=0)
CPU0 AW/W/B:      STXR ████ → BRESP=EXOKAY ✓
CPU1 AW/W/B:                STXR ████ → BRESP=OKAY ✗
```

### 9.4 普通写踢掉 Monitor（独占失败经典因）

```
场景：CPU0 已 LDXR，尚未 STXR；DMA/CPU1 对该地址发 Normal Write（AWLOCK=0）
      → Monitor 清除 CPU0 的 exclusive
      → CPU0 再 STXR → BRESP=OKAY（失败，不更新）

CPU0 ExRd  [AR LOCK=1]──[R DATA=0]
Monitor    {M0,addr}✓

DMA Normal Write
           ........[AW LOCK=0][W][B OKAY]
Monitor    {M0,addr} 被清 ✗

CPU0 ExWr  ................[AW LOCK=1][W][B OKAY]  ← 失败
```

抓波：在 `ARLOCK=1` 与随后 `AWLOCK=1` 之间，看有无**同址任意写**（含 `AWLOCK=0`）。有则随后 Exclusive Write 必须失败。

### 9.5 通道交织与 Master 身份（补一句）

- AXI 读写通道独立，允许与其它 master 的 AR/AW **时间重叠**；Monitor 按 **{master/端口, addr}** 记账，不靠霸占整总线。  
- 多 outstanding 用 `AxID`/`RID`/`BID` 配对；「是谁」通常是 **端口或 AxID 高位**（看 TRM (Technical Reference Manual)），不是软件 pid。  
- 与 AHB：`HMASTER` 多表示「当前 grant 给了谁」；AXI Exclusive 则是 Monitor 表项，**不阻止对方发 AR/AW**。

### 9.6 AXI→AHB5 桥：保护位映射（备查）

```verilog
assign hnonsec  = axprot[1];      // Secure/Non-secure
assign hprot[0] = ~axprot[2];     // data/instr
assign hprot[1] = axprot[0];     // privilege
// cache/domain → HPROT[2+] 依桥实现，略
```

---

## 10. 对照：AHB `HLOCK` vs AXI Exclusive

> 问题：AXI 用 ARLOCK/AWLOCK + Monitor + EXOKAY；**AHB 靠什么做多 master 临界 RMW？**  
> 答：经典 AHB 用 **`HLOCK` 占住 grant**，保证读-改-写中间不被其它 master 插入。这与 Linux spinlock 在 AXI 上的路径**不是同一条路**。

### 10.1 一张表先定调

| | AHB `HLOCK` | AXI Exclusive |
|--|-------------|---------------|
| 手段 | **占住 `HGRANT`** | **Monitor 记账** |
| 其它 master | 等总线 | 仍可访问；同址写会踢 monitor |
| 成功指示 | 无 EXOKAY | **`EXOKAY` / `OKAY`** |
| 性能 | 锁总线，易堵 | 更细粒度 |
| 典型用途 | MCU/DMA (Direct Memory Access) 的总线级 RMW | `LDXR/STXR`、Linux spinlock |

一句话：**HLOCK 挡的是「换主人」；Exclusive 挡的是「同址独占写还算不算成功」。**

### 10.2 `HLOCK` 是什么

| 项 | 说明 |
|----|------|
| 方向 | Master → **Arbiter**（不是写进 slave 的锁变量） |
| 语义 | `HLOCK=1`：当前获 grant 后，**不要把总线再授给别人**，直到本序列结束 |
| 与数据 | Slave 仍按普通传输响应；**没有 EXOKAY** |

```
错误（无 HLOCK 的 cnt++）：
  A 读到 5 → B 读到 5 → A 写 6 → B 写 6  → 丢一次 +1
正确：
  A: HLOCK=1 → 读 5 → 写 6 → HLOCK=0；其间 B 拿不到 GRANT
```

注意：`HLOCK` ≠ 软件 spinlock，≠ `LDXR`；单核任务互斥仍常靠关中断。锁范围只在**该仲裁域**；跨桥必须正确传递 lock/exclusive 属性。

### 10.3 挡人挡在哪一层

```
HBUSREQ0/HLOCK0/HGRANT0 ─┐
HBUSREQ1/HLOCK1/HGRANT1 ─┼─▶ Arbiter ─▶ HMASTER + 共享总线 ─▶ Slave
         ▲
         └ 「锁住不换人」= 持有 GRANT 且 HLOCK=1 时不切换 grant
           Slave 不必「拒绝 DMA」—— DMA 往往根本还没上来
```

AHB-Lite（单 master）常无多路 REQ/GRANT，也常无 `HMASTER`。

### 10.4 波形：CPU 与 DMA 对同一地址 RMW

M0=CPU，M1=DMA；对 `0x20000100` 做 `cnt++`；CPU 优先；DMA 早请求但被挡住。

```
CLK        1  2  3  4  5  6  7  8  9 10 11 12
HBUSREQ0   ▁▁████████████▁▁▁▁▁▁▁▁▁▁
HLOCK0     ▁▁▁█████████▁▁▁▁▁▁▁▁▁▁▁
HGRANT0    ▁▁███████████▁▁▁▁▁▁▁▁▁▁
HTRANS0    I  I  N(读5) N(写6) I ...
HBUSREQ1   ▁▁▁████████████████████▁
HGRANT1    ▁▁▁▁▁▁▁▁▁▁▁▁█████████▁   ← LOCK 期间必须为 0
HTRANS1    .......... N(读6) N(写7)
HMASTER    -  -  0  0  0  -  -  1  1  1
```

读波盯三点：① `HLOCK0=1` 时 `HGRANT1=0`；② CPU 读与写之间无 M1 有效传输；③ 锁定序列中 `HMASTER` 保持 0。

### 10.5 和 MCU / 有 Cache 系统

- **单核**：日常临界区优先关中断；HLOCK 不是 RTOS (Real-Time Operating System) 互斥主手段。  
- **CPU+DMA 改同一控制字**：查 DMA locked 传输 / 硬件互斥 / 协议禁止双边 RMW。  
- **有 cache 的 A 系列**：软件仍是 LDXR/STXR；总线上可能是 ACE/CHI 一致性报文，不一定裸露 `ARLOCK` 名字；对软件可见的仍是 STXR 成败。

---


## 附录 A：AxPROT 保护位含义

| Bit | 含义 |
|-----|------|
| `AxPROT[0]` | 0=Unprivileged, 1=Privileged |
| `AxPROT[1]` | **0=Secure, 1=Non-secure（TrustZone）** |
| `AxPROT[2]` | 0=Data, 1=Instruction |

> AXI 把 secure/non-secure 做进 `AxPROT[1]`；AHB5 用 `HNONSEC`，桥上常直接映射。

## 附录 B：抓波与断言清单

**AXI Exclusive**

| # | 检查 |
|---|------|
| 1 | ExWr 成功：`BRESP=EXOKAY` **且** 该地址数据已更新 |
| 2 | ExWr 失败：`BRESP=OKAY` **且** 数据未变 |
| 3 | 两 master 同时 ExWr：至多一个 EXOKAY |
| 4 | ExRd 后、ExWr 前插入同址 Normal Write → 随后 ExWr 必须失败 |
| 5 | `BID` 与 `AWID` 匹配；多 outstanding 不串响应 |
| 6 | Device 内存上发 Exclusive：行为符合 TRM（通常应避免） |

**AHB HLOCK**

| # | 检查 |
|---|------|
| 1 | `HLOCK=1` 区间内其它 master 的 `HGRANT` 保持 0 |
| 2 | 同一 locked RMW 的读与写之间，共享总线无其它 master 有效 `HTRANS` |
| 3 | 锁定序列中 `HMASTER`（若有）保持当前 master |
| 4 | 跨 AXI↔AHB 桥时，lock/exclusive 属性未被桥丢弃 |

---

## 小结

- ARM Linux spinlock 靠 LDXR/STXR + Exclusive，而不是锁住整条 AXI。
- Local/Global Monitor 决定独占写成败；`BRESP=EXOKAY` 才是拿到锁。
- 普通写可踢掉 Monitor；双核可同时 ExRd，但 ExWr 至多一个成功。
- AHB `HLOCK` 挡的是换 grant；与 Exclusive 不是同一条故事。

## 自测

1. 为什么 AXI4 不宜用「锁总线」实现 Linux spinlock？
2. LDXR/STXR 分别反映到哪些通道、什么属性？
3. 没有 Global Monitor 时双核同时 STXR 可能怎样？
4. Exclusive 读之后被同址 Normal Write 插入，再 STXR 会怎样？
5. AHB `HLOCK` 与 AXI Exclusive 各挡什么？抓波看哪些信号？

---

*`05-bus-rtl` · spinlock → 总线（AXI / AHB）*
