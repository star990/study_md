# AXI / AHB 协议速查：属性、保护、原子与桥接

> **系列**：`05-bus-rtl`  
> **定位**：协议分解备忘——信号含义、编码表、AXI↔AHB 映射；场景长文见 [`01-spinlock-to-bus.md`](01-spinlock-to-bus.md)、[`02-cache.md`](02-cache.md)  
> **相关**：[`../04-riscv-core/03-memory-bus.md`](../04-riscv-core/03-memory-bus.md) · [`../07-interview/01-chip-validation.md`](../07-interview/01-chip-validation.md)（验 DMA outstanding）· [`../07-interview/02-core-bus.md`](../07-interview/02-core-bus.md)

把附图与 01/02 里**和总线协议直接相关**的核心抽成一页。细节、波形、Linux 路径仍回原文。

**读完应能**：
- 读懂 `AxCACHE` / `AxPROT` / `HPROT` 各位含义与常见编码
- 对照 Exclusive vs Locked vs `HMASTLOCK`/`HLOCK`
- 做 AXI↔AHB 桥时知道保护位极性与 cache 位怎么对
- 用一句话串起通道、ID、1K/4K、属性从哪来
- 说清 Outstanding 是谁的上限、AxID 谁生成、满了有无溢出信号

---
## 目录

1. [形态对比：AXI vs AHB](#1-形态对比axi-vs-ahb)
2. [AxCACHE：Bufferable / Modifiable / Allocate](#2-axcachebufferable--modifiable--allocate)
3. [保护：AxPROT vs HPROT](#3-保护axprot-vs-hprot)
4. [原子：Exclusive / Locked / HMASTLOCK](#4-原子exclusive--locked--hmastlock)
5. [桥接映射要点](#5-桥接映射要点)
6. [从 01/02 抽的总线核心（精简）](#6-从-0102-抽的总线核心精简)
7. [小结](#7-小结)
8. [自测](#8-自测)

---

## 1. 形态对比：AXI vs AHB

| | **AXI** | **AHB** |
|--|---------|---------|
| 结构 | 5 通道分离（AR/R/AW/W/B），可流水、多 outstanding | 共享地址/数据相 + 仲裁 grant |
| 乱序 | 按 **AxID**：同 ID 保序，不同 ID 可乱序 | 仲裁域内基本按 grant 串行 |
| Burst 边界 | 单笔不跨 **4KB** | 单笔不跨 **1KB** |
| 原子主流 | **Exclusive** + Monitor（`EXOKAY`） | **`HLOCK`/`HMASTLOCK` 占 grant** |
| 属性口 | `AxCACHE`、`AxPROT`、`AxLOCK`、`AxDOMAIN`… | `HPROT`、`HMASTLOCK`；AHB5 更接近 AXI |

---

## 2. AxCACHE：Bufferable / Modifiable / Allocate

`ARCACHE[3:0]` / `AWCACHE[3:0]`（经典 AXI4 读法）：

| Bit | 名 | 含义 |
|-----|-----|------|
| `[0]` | **Bufferable (B)** | 中间级可否缓冲写 / posted |
| `[1]` | **Modifiable (M)** | 互联可否改地址/长度/拆并等（常与「可 cache 路径」相关） |
| `[2]` | **Read Allocate (RA)** | 读 miss 建议 allocate |
| `[3]` | **Write Allocate (WA)** | 写 miss 建议 allocate |

常见取值（记忆表，以所用 IP TRM 为准）：

| `AxCACHE` | 含义（常见说法） |
|-----------|------------------|
| `0000` | Device, Non-bufferable |
| `0001` | Device, Bufferable |
| `0010` | Normal, Non-cacheable, Non-bufferable |
| `0011` | Normal, Non-cacheable, Bufferable |
| `0110` | Write-through, Read-allocate |
| `0111` | Write-through, Read/Write-allocate |
| `1110` | Write-back, Read-allocate |
| `1111` | Write-back, Read/Write-allocate |

> Bufferable ≠ Cacheable：前者管「写可否晚到」；后者管「进不进 cache」。展开见 [`02-cache.md`](02-cache.md)。

---

## 3. 保护：AxPROT vs HPROT

### 3.1 AXI `AxPROT[2:0]`

| Bit | 含义 |
|-----|------|
| `[0]` | 0=Unprivileged，1=Privileged |
| `[1]` | **0=Secure，1=Non-secure**（TrustZone） |
| `[2]` | 0=Data，1=Instruction |

### 3.2 AHB `HPROT`（经典 4 bit；AHB5 可扩到更多）

| Bit | 含义 | 注意 |
|-----|------|------|
| `[0]` | 0=Opcode（指令），1=Data | 与 AXI「Instr=1」**极性相反** |
| `[1]` | 0=User，1=Privileged | |
| `[2]` | Bufferable | |
| `[3]` | Cacheable / Modifiable | 常对 AXI `AxCACHE[1]` |

AHB5 可再带 lookup / allocate / shareable 等，更贴 AXI；具体位定义看桥与 AHB5 规范。

---

## 4. 原子：Exclusive / Locked / HMASTLOCK

### 4.1 AXI `AxLOCK[1:0]`

| 值 | 模式 | 要点 |
|----|------|------|
| `00` | Normal | 普通 |
| `01` | **Exclusive** | LL/SC；Monitor 记账；成功看 **`EXOKAY`**；**不锁死总线** |
| `10` | Locked | 占互联；**AXI4 废弃**，勿当 Linux spinlock 路径 |
| `11` | 保留 | — |

成败在 Monitor + `BRESP`，不在「别人发不出发 AR/AW」。详见 [`01-spinlock-to-bus.md`](01-spinlock-to-bus.md)。

### 4.2 AHB `HMASTLOCK` / `HLOCK`

- 单 bit：锁定序列期间 **仲裁器不换 grant**。  
- Slave 无 EXOKAY；挡的是「换主人」，不是「同址写是否算独占成功」。  
- AHB-Lite 单 master 时常无多路 REQ/GRANT。

| | AXI Exclusive | AHB Locked |
|--|---------------|------------|
| 手段 | Monitor | 占 `HGRANT` |
| 其它 master | 仍可上总线 | 等 grant |
| 成功指示 | `EXOKAY` / `OKAY` | 无对应码 |
| 典型 | `LDXR`/`STXR` | MCU/DMA 总线级 RMW |

---

## 5. 桥接映射要点

AXI→AHB（示意，以所用桥 TRM 为准）：

```text
AxCACHE[0] (Bufferable)  →  HPROT[2]
AxCACHE[1] (Modifiable)  →  HPROT[3]   // 常称 Cacheable
AxPROT[0]  (Priv)        →  HPROT[1]
AxPROT[2]  (Data/Instr)  →  HPROT[0] = ~AxPROT[2]   // 极性反
AxPROT[1]  (NS)          →  HNONSEC（AHB5）或桥扩展脚
```

其它必查：

| 项 | 注意 |
|----|------|
| Burst 边界 | AXI 可到 4K 内一笔；AHB 须在 **1K** 拆（桥常在 1K 处 `NONSEQ`） |
| Exclusive / Lock | 跨桥必须**传递** exclusive 或 lock 语义，丢属性会破原子 |
| ID | AXI 多 outstanding 靠 ID；AHB 侧常被串成单流 |

示例（与 01 §9.6 同类）：

```verilog
assign hnonsec  = axprot[1];
assign hprot[0] = ~axprot[2];   // data/instr
assign hprot[1] = axprot[0];    // privilege
// AxCACHE → HPROT[2+] 依桥实现
```

写回 / 多级 cache 场景下，基本 AXI4 **不够**表达硬件一致性；需 **ACE / CHI**（snoop、DVM 等）——一致性正文放 [`02-cache.md`](02-cache.md) Part B。

---

## 6. 从 01/02 抽的总线核心（精简）

### 6.1 来自 01：通道、ID、Exclusive

- **五通道**：读 AR+R；写 AW+W+B；各 VALID/READY 独立。  
- **AxID**：标签池租还，非每笔唯一号；同 ID 保序，不同 ID 可乱序；有依赖 → 同 ID / 等完成 / 屏障。  
- **AXI4 写**：无 `WID`；W 顺序跟 AW；B 可对不同 ID 乱序回来。  
- **Exclusive**：Local +（跨核）Global/一致性；`STXR` 看 `EXOKAY`。  
- **HLOCK vs Exclusive**：挡 grant ≠ 挡同址独占语义。

### 6.2 来自 02：属性、边界、谁打 AxCACHE

| 概念 | 一句话 |
|------|--------|
| Outstanding | 已发未完成的事务数（展开见 §6.3） |
| Cacheable | 能否进 cache |
| Bufferable | 写能否 posted / 进缓冲（core/桥/控制器都可能） |
| MMU ↔ AxCACHE | 语义同源；CPU 由页表译出；**不是同一物理 bit** |
| DMA / 无 MMU | 寄存器或 RTL tie-off 打 `AxCACHE` |
| AHB 1KB / AXI 4KB | 防 burst 中途换 slave 或换页；4K 与 MMU 小页同祖 |
| Descriptor 不跨 4K | 管描述符摆放；≠ payload 条款；根因与 AXI 4K 同族 |

### 6.3 Outstanding 与 AxID（精炼）

#### 定义与归属

- **Outstanding** = 已发出、尚未完成的事务个数（读常计到 `RLAST`，写常计到 `B`）；**不必等 response 即可再发**下一笔 AR/AW。  
- **不是**「整条 AXI bus 的一个全局数」。规格写「DMA outstanding=64」→ 多半指 **该 master 发单能力/配置上限**。  
- 路径上各级各自有限额，墙上有效深度：

```text
有效 outstanding ≈ min( Master 能发, 互联 issuing/acceptance, Slave acceptance, ID 策略允许的深度 )
```

- Core 与 DMA 各挂一个 master 口时：**各自 outstanding，互不共用一个「总线 outstanding」**；下游 slave acceptance 可能被多 master 分食。  
- 读/写 outstanding 在 IP 里常 **分开配**（`max_rd` / `max_wr`）。

#### AxID：谁生成？

| 角色 | 做什么 |
|------|--------|
| **Master（CPU LSU / DMA）** | **选择**本次 `ARID`/`AWID`（发地址相位时带上） |
| **互联** | 多 master 汇合时 **拼端口高位 / 映射**，回程按 ID 路由；不替业务决定「保序还是并行」 |
| **Slave** | 响应用同号 `RID`/`BID` 对回 |

常见误解：「ID 是 bus 自动生成的流水号」→ **错**。ID 是有限位宽的**标签池**（租用/归还）；软件配 DMA 时通常**不填** AxID，由 DMA RTL 按策略分配（固定单 ID、按 outstanding 槽位租还、按通道分 ID 等）。有依赖 → 同 ID 或等完成；无依赖才用不同 ID 拉吞吐。

| 限制 | 后果 |
|------|------|
| **同 ID only** / 同 ID 深度=1 | 再大配置也难把在飞堆高 |
| **窄 ID 空间**（如 2bit → 4 个标签） | 再叠加每 ID 一笔 → 理论上限远小于 64 |

规则复习：同 ID 保序；不同 ID 可乱序/交错。细节见 [`01-spinlock-to-bus` §11.2](01-spinlock-to-bus.md#112-axid-怎么分配有依赖怎么办)。

#### 满了之后：有无「溢出」信号？

- **标准 AXI 没有** outstanding 溢出专用信号；`RRESP`/`BRESP` 也不表达「队列满」。  
- 健康行为：满则 **停发下一笔 AR/AW**（或表现为长时间等 `READY`——更偏互联/slave acceptance 满）。  
- `cnt > MAX` 仍完成新地址握手 → **设计 bug**（验证用断言抓），不是靠某个 `OVERFLOW` 脚上报的正常路径。  
- 部分 DMA 有 `queue_full` / 状态计数等 —— **厂商自定义，看 TRM**。

#### 和验证的接口（一句话）

要证「能力=64」：慢 slave/背压把在飞**顶到 64**，monitor/SVA 证 **能到且不超过**，满时再加压应 stall；可配则同条件比带宽。Case 表见面试 [`01-chip-validation` Q2](../07-interview/01-chip-validation.md)。

---

## 7. 小结

- **属性看 `AxCACHE`/`HPROT`，保护看 `AxPROT`/`HPROT`（Instr 位极性相反）。**  
- **原子：AXI 用 Exclusive+Monitor；AHB 用锁 grant；AXI4 勿再依赖 Locked。**  
- **桥：对 cache/prot/lock/1K 拆笔；丢属性 = 隐性 bug。**  
- **一致性扩展在 ACE/CHI，不在裸 AXI4 的 `AxCACHE` 四位里讲完。**  
- **Outstanding：master 能力上限为主，路径取 min；AxID 由 master 选，互联可改写；满了 stall，无标准溢出脚。**

## 8. 自测

1. `AxCACHE=0011` 与 `1111` 差在哪？Bufferable 和 Allocate 各管什么？  
2. AXI `AxPROT[2]=1` 落到 AHB `HPROT[0]` 应是 0 还是 1？为什么？  
3. AXI4 上 Linux spinlock 该用 Exclusive 还是 Locked？AHB 多 master RMW 常见靠什么？  
4. AXI→AHB 桥遇到跨 1KB 的 INCR 通常怎么做？  
5. CPU 与 DMA 的 `AxCACHE` 配置点分别在哪？  
6. DMA 规格 outstanding=64，总线上却打不满，可能卡在哪几层？AxID 是谁生成的？满了有没有标准溢出信号？

---

*`05-bus-rtl` · `03-axi-ahb` · 协议速查*
