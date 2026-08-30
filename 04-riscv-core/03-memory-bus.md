# RISC-V：一次 Load 从 Core 到 SRAM（单核 → SMP）

> **系列**：`04-riscv-core`  
> **前置**：[`02-csr-trap.md`](02-csr-trap.md)（特权级与 trap **CSR** (Control and Status Register)）；流水线形态见 [`01-pipeline.md`](01-pipeline.md)  
> **相关**：[`../05-bus-rtl/01-spinlock-to-bus.md`](../05-bus-rtl/01-spinlock-to-bus.md)（原子/Exclusive）；软件侧页表/**DMA** (Direct Memory Access) 见 [`../01-kernel/02-memory.md`](../01-kernel/02-memory.md)

上一篇总线专文把 **spinlock** 讲到 **AXI** (Advanced eXtensible Interface) Exclusive / **AHB** (Advanced High-performance Bus) `HLOCK`。本文换一条更日常的路径：  
**`lw` 从片上 SRAM (Static Random-Access Memory) 读一个字**——从 **LSU** (Load/Store Unit) 算地址，经 **TLB** (Translation Lookaside Buffer) / **MMU** (Memory Management Unit)、**PMP** (Physical Memory Protection)、**PMA** (Physical Memory Attributes)、一级 D-cache、总线主口、Bus Matrix，再到 SRAM slave。  
§1–13 先把 **单核** 每一跳端口讲闭环；§14–19 再把 **双 hart** (hardware thread) **SMP** (Symmetric Multi-Processing) 的复制/共享、无 snoop 失败场景、三条工程出路，以及 `lr`/`sc` (Load-Reserved / Store-Conditional)、fence、TLB 射齐补齐。全文以 **RISC-V** 为本，不展开 ARM。

**读完应能**：
- 画出单核 `lw → SRAM` 上每一级模块与端口
- 分清 Hit / Miss 两条时序，以及 page walk 为何也占总线
- 说明 **VA** (Virtual Address) → **PA** (Physical Address)、PMP 权限、**PMA 属性**、D$ 各自挡在哪一层
- 画出双 hart 框图：哪些模块复制、哪些共享，并解释无 snoop 时为何会读到旧值
- 对照软件一致 / 硬件 snoop / **AMP** (Asymmetric Multi-Processing)，以及 `lr`/`sc`、`fence`、`sfence.vma` 落在哪一层

---

## 目录

1. [场景假设与总框图](#1-场景假设与总框图)
2. [端口清单：每一跳叫什么](#2-端口清单每一跳叫什么)
3. [第一层：LSU——算出 VA，发起访存](#3-第一层lsu算出-va发起访存)
4. [第二层：MMU / TLB——VA → PA](#4-第二层mmu--tlbva--pa)
5. [第三层：PMP——物理地址权限](#5-第三层pmp物理地址权限)
6. [第四层：PMA——物理内存属性](#6-第四层pma物理内存属性)
7. [第五层：D-Cache——Hit / Miss](#7-第五层d-cachehit--miss)
8. [第六层：总线主口——AHB-Lite / AXI](#8-第六层总线主口ahb-lite--axi)
9. [第七层：Bus Matrix——仲裁与译码](#9-第七层bus-matrix仲裁与译码)
10. [第八层：SRAM Controller / Slave](#10-第八层sram-controller--slave)
11. [闭环时序 A：D-Cache Hit](#11-闭环时序-ad-cache-hit)
12. [闭环时序 B：D-Cache Miss + Line Fill](#12-闭环时序-bd-cache-miss--line-fill)
13. [对照：Store 与 Fence](#13-对照store-与-fence)
14. [从单核到 SMP：总框图与端口增量](#14-从单核到-smp总框图与端口增量)
15. [SMP 逐层对照：哪些复制、哪些共享](#15-smp-逐层对照哪些复制哪些共享)
16. [无一致性硬件：三条必挂场景](#16-无一致性硬件三条必挂场景)
17. [三条工程出路：软件 / snoop / AMP](#17-三条工程出路软件--snoop--amp)
18. [原子、Fence、TLB 射齐（RV）](#18-原子fencetlb-射齐rv)
19. [SMP 闭环时序与抓波](#19-smp-闭环时序与抓波)
20. [附录 A：裸机无 MMU 的最短路径](#附录-a裸机无-mmu-的最短路径)
21. [附录 B：异常对照表（Load 相关）](#附录-b异常对照表load-相关)
22. [小结](#小结)
23. [自测](#自测)

---

## 1. 场景假设与总框图

### 1.1 本文固定模型（先钉死，避免混谈）

| 项 | 假设 |
|----|------|
| ISA | RV32/RV64；指令 `lw rd, imm(rs1)` |
| 核 | §1–13：**单 hart**；§14 起：**双 hart SMP**（可推到 N） |
| Cache | **每 hart 私有一级 D-cache**（无 L2）；I-cache 对称存在但不走本路径 |
| 目标 | 片上 **SRAM**（Normal、可 cache；非 Device MMIO） |
| 地址翻译 | 开 **Sv32 / Sv39**（RISC-V 分页模式；§附录 A 给无 MMU 捷径） |
| 保护 | **PMP** 开（每 hart 一份 CSR，配置常镜像） |
| 属性 | **PMA**（固定地址图或可配表；SRAM=cacheable Normal；MMIO=Device） |
| 总线 | 数据侧主口用 **AHB-Lite** 或 **AXI4**；多 master 进同一 Matrix |
| 一致性 | 先讲 **无硬件一致** 的失败，再给软件 / snoop / AMP 三条出路 |

> **为何只谈一级 D$**：把「核内 → 总线 → SRAM」每一跳端口讲透；加 L2/CHI 会再多一层，留给后续专题。

### 1.2 软件一眼看到的

```text
  // rs1 = 某 VA，指向已映射到片上 SRAM 的页
  lw  x5, 0(x10)     // 读 4 字节 → x5
```

硬件要做的事可以压缩成一句：

> **把 VA 变成 PA，查权限与属性，尽量用 D$ 命中；没命中就按 cache line 从 SRAM 拉回来，再把请求的字交给流水线。**

### 1.3 总框图（单核）

```text
                    ┌──────────────────────────────────────────────┐
                    │                  Hart / Core                   │
                    │  ┌─────┐   ┌─────┐   ┌─────────────────────┐ │
  指令流 ──────────►│  │ IF  │──►│ …  │──►│ LSU (MEM stage)     │ │
                    │  └─────┘   └─────┘   │  VA, size, Load     │ │
                    │                      └──────────┬──────────┘ │
                    │                                 │ Port-A     │
                    │                      ┌──────────▼──────────┐ │
                    │                      │ DTLB (Data TLB; + walker) │ │
                    │                      │  miss → page walk   │──┼──► 也会走总线！
                    │                      └──────────┬──────────┘ │
                    │                                 │ PA + attr  │
                    │                      ┌──────────▼──────────┐ │
                    │                      │ PMP check（权限）    │ │
                    │                      └──────────┬──────────┘ │
                    │                      ┌──────────▼──────────┐ │
                    │                      │ PMA lookup（属性）   │ │
                    │                      │ cacheable? Device?  │ │
                    │                      └──────────┬──────────┘ │
                    │                                 │ Port-B     │
                    │                      ┌──────────▼──────────┐ │
                    │                      │ L1 D-Cache          │ │
                    │                      │  Hit → data 回 LSU  │ │
                    │                      │  Miss→ fill FSM (Finite State Machine) │ │
                    │                      └──────────┬──────────┘ │
                    │                                 │ Port-C     │
                    │                      ┌──────────▼──────────┐ │
                    │                      │ Bus Master IF       │ │
                    │                      │ (AHB / AXI)         │ │
                    └──────────────────────┴──────────┬──────────┘
                                                      │ Port-D
                                           ┌──────────▼──────────┐
                                           │ Bus Matrix / Xbar   │
                                           │  decode + arbiter   │
                                           └──────────┬──────────┘
                                                      │ Port-E
                                           ┌──────────▼──────────┐
                                           │ SRAM Controller     │
                                           │  (AHB/AXI Slave)    │
                                           └──────────┬──────────┘
                                                      │ Port-F
                                                   ┌──▼──┐
                                                   │SRAM │
                                                   └─────┘
```

**本节要点**：一条 Load 不是「LSU 直接捅 SRAM」，中间至少有 **翻译、PMP 权限、PMA 属性、缓存、主口、矩阵、从口** 七段；page walk 还会 **再占用一次总线**。

---

## 2. 端口清单：每一跳叫什么

造核 / 抓波时，先把「接口契约」说清楚。下表是本文统一命名。

| 代号 | 谁 → 谁 | 典型载荷 | 单核备注 |
|------|---------|----------|----------|
| **Port-A** | LSU → DTLB | VA、**ASID** (Address Space Identifier；若有)、访问类型 `Load`、特权级、size | 未命中则 walker 用 PA 读页表 |
| **Port-B** | PMP/PMA → D$（或 uncached 旁路） | **PA**、size、意图、**PMA 属性**（cacheable/Device…） | PMP 失败则 trap；PMA=**NC** (Non-Cacheable) 则 **绕过 D$** |
| **Port-C** | D$ → Bus Master | 行填充：对齐到 **cache line** 的 burst 读 | Hit 时 Port-C **静默** |
| **Port-D** | Master → Matrix | AHB：`HADDR/HTRANS/HSIZE…`；AXI：`AR*` / `R*` | Master ID 供仲裁 |
| **Port-E** | Matrix → SRAM Slave | 译码后的从口时序（可与 Port-D 同协议，或经桥） | 多 slave 时按地址窗口切 |
| **Port-F** | Controller → SRAM 阵列 | `addr / cen / wen / wdata / rdata`、字节使能 | 常 1 拍读延迟 |

另有两条「旁路主口」，初学者易漏：

| 旁路 | 何时出现 | 走哪 |
|------|----------|------|
| **I-Fetch 主口** | 取指 | 通常另一套 I$ / **ITCM** (Instruction Tightly-Coupled Memory)，**不经 D$** |
| **Page-walk 主口** | DTLB miss | 读 **PTE** (Page Table Entry)；可与 D$ **共享**同一 Bus Master，也可独立 walker 口 |
| **Uncached / Device** | PMA（或页属性）标 non-cacheable / Device | **绕过 D$**，LSU→PMP→PMA→Master 直打从设备 |

> **设计选择题**：walker 与 D$ miss 是否共享同一 AXI port？共享省口、要排队；独立口可并行，矩阵多一个 master。验证时两边都要能抓到。

---

## 3. 第一层：LSU——算出 VA，发起访存

### 3.1 有效地址

```text
VA = rs1 + sext(imm)     // I-type load
```

随后 LSU 提交一次 **内存事务描述符**（概念上）：

```text
{
  va,                 // 有效地址
  size,               // 1/2/4/8
  op = LOAD,
  priv,               // 当前 privilege（影响 PMP / 页表 U 位）
  unsigned?,          // lbu/lhu vs lb/lh
}
```

### 3.2 对齐

| 情况 | 行为（常见实现） |
|------|------------------|
| 自然对齐 | 一条总线/缓存事务 |
| 非对齐且硬件不支持 | **Load address misaligned** → trap（`mcause`） |
| 非对齐且硬件拆分 | 拆成多次访问再拼接（MCU 少见；应用核更多） |

本文默认：**要求对齐**；不对齐直接异常，方便 RTL 断言。

### 3.3 和流水线的握手（概念）

```text
EX/AGU (Address Generation Unit) 算出 VA
   → MEM：等 DTLB + PMP + D$（或 bus）返回
   → WB (Write-Back)：写回 rd
```

D$ miss 或 page walk 时，MEM 级 **stall**（或记分牌挂起该指令）；单发射五级流水最直观。

**本节要点**：LSU 只认 **VA + 访问意图**；PA、能不能 cache、走不走总线，都是后面几层的事。

---

## 4. 第二层：MMU / TLB——VA → PA

> RISC-V 分页由 **`satp`** 控制。M-mode 通常可关分页（`satp.MODE=Bare`）；S/U 下访存才走页表。MCU 很多产品 **只做 PMP、不做 MMU**——见附录 A。本节按「开了 Sv\*」写全。

### 4.1 先分清三个词

| 词 | 是什么 | 在路径上干什么 |
|----|--------|----------------|
| **MMU** | Memory Management Unit：整套「按页表做 VA→PA」的机制（含硬件+页表约定） | 定义翻译规则 |
| **DTLB** | Data TLB：核内一张小表，缓存「最近用过的」VA→PA | **加速**；命中则本拍出 PA |
| **Page Walker** | TLB miss 时按页表层级去 **读 PTE** 的状态机（硬件 walk 或 trap 后软件 walk） | **填 TLB**；本身也是访存 |

框图里写 `DTLB (+ walker)`，意思是：LSU 先问 DTLB；问不到再让 walker 去内存里把答案找回来。

取指侧通常还有 **ITLB** (Instruction TLB)，结构和 DTLB 类似，本文 Load 路径只谈 DTLB。

### 4.2 `satp`：翻译开不开、根在哪

| 字段 | 含义 |
|------|------|
| MODE | `Bare`=不做翻译（VA≈PA）；`Sv32`/`Sv39`/…=开分页 |
| ASID | 地址空间 ID；详解见 §4.3 |
| PPN | Physical Page Number：**根页表**所在物理页号 |

```text
satp.MODE == Bare  →  根本不进 DTLB/walker（附录 A）
satp.MODE == Sv39  →  每次 Load 的 VA 都要翻译（先 DTLB，miss 再 walk）
```

### 4.3 ASID：为何 TLB 要带「进程标签」

#### （1）问题：VA 会「撞车」

多进程下，进程 A 与进程 B 经常都用同一套用户 VA（例如都从 `0x10000` 起）：

```text
进程 A: VA 0x10000 → PA 0x2000_0000
进程 B: VA 0x10000 → PA 0x2100_0000   // 同 VA，不同物理页
```

若 DTLB 项只存 `{VPN (Virtual Page Number) → PPN}`、**不带进程标签**，则：

1. 调度到 B 时必须 **整表刷掉**（否则会用到 A 的翻译 → 静默踩内存）；或  
2. 每条项都绑定「属于谁」，切进程只换当前标签，旧项可留着备用。

**ASID (Address Space Identifier)** 就是这个标签：写在 `satp.ASID`，填进每条 TLB entry，查找时与当前 `satp.ASID` 匹配才算命中。

#### （2）和 `satp` 其它字段的关系

```text
切进程（典型 OS）时硬件/软件常一起换：
  satp.PPN  ← 新进程根页表
  satp.ASID ← 新进程的 ASID（或从 ASID 池分配）
  再按需 sfence.vma
```

| 做法 | 行为 | 代价 |
|------|------|------|
| **不用 ASID**（或宽度为 0） | 每次换地址空间往往全局 `sfence.vma` 洗 TLB | 简单，切进程 TLB 冷启动 |
| **用 ASID** | TLB 项带 ASID；切到曾跑过的进程可能 **直接 hit 旧项** | 少刷、更快；要管理 ASID 复用 |

ASID 位宽由实现/MODE 决定（如 Sv39 常见最多 16 bit，以规范与 **TRM** (Technical Reference Manual) 为准）；池子用尽时 OS 回收 ASID → 必须射齐仍标旧 ASID 的 TLB 项。

#### （3）Global 映射

内核镜像等希望「所有 ASID 都能用」的映射，PTE 可标 **G（Global）**：TLB 命中时 **不比对 ASID**（仍比对 VPN 等）。  
改全局映射后，射齐规则与非 G 不同——实现/OS 要用对 `sfence.vma` 参数，避免留下幽灵全局项。

#### （4）`sfence.vma` 与 ASID

```text
sfence.vma rd, rs1     // 语义随 rd/rs1 是否为 x0 变化（规范精确定义）
```

直观用法（概念）：

| 意图 | 效果（示意） |
|------|----------------|
| 全局洗本核 TLB | 不分 ASID 全无效 |
| 只洗某个 ASID | 该进程旧翻译作废，其它进程项可留 |
| 只洗某 VA（+可选 ASID） | 改了一页 PTE 后的精确射齐 |

多核还要 **IPI** (Inter-Processor Interrupt) 让其它 hart 也做（§18.3）——ASID 不能跨 hart 自动广播。

#### （5）虚拟化时还有 VMID（不是 HID）

若实现 **H 扩展（Hypervisor）**，翻译变成两级世界，标签也多一层：

```text
Guest VA  ──(vsatp / 客户 OS 页表，带 ASID)──►  Guest PA
Guest PA  ──(hgatp，二级页表)──────────────►  Host PA（真物理）
```

| 标签 | 在哪 | 区分什么 |
|------|------|----------|
| **ASID** | `satp` / `vsatp` | 同一套「客户/主机 OS」里的不同进程 |
| **VMID** (Virtual Machine Identifier) | **`hgatp.VMID`** | 不同 **虚拟机 / Guest** 的 **GPA** (Guest Physical Address)→**HPA** (Host Physical Address) 缓存（如 TLB/G-stage 缓存） |

有人把虚机标签口误成「HID」——RISC-V 规范里标准名字是 **VMID**，写在 **`hgatp`**，不是 `mhartid`，也不是另造一个 HID CSR。  
ARM 同理是 ASID + **VMID**；RV 对齐的是这套概念。

TLB/缓存项在带虚拟化的核上概念上变成：

```text
{ VMID?, ASID?, VPN, … → PPN, attrs }
```

- 只跑裸机 / 单 OS、无 H 扩展：只有 **ASID**（或连 ASID 都不用），本文主路径到此为止。  
- Hypervisor 场景：切 VM 换 `hgatp`（含 VMID）+ 客户自己的 `vsatp`（含 ASID）；射齐用 `HFENCE.GVMA` / `HFENCE.VVMA` 等（细节留给虚拟化专题）。

**本节要点**：ASID 让 TLB 知道「这条翻译属于哪个地址空间」，避免同 VA 换进程必须整表开洗；虚拟化再叠加 **VMID**（`hgatp`）区分 Guest。MCU 无 MMU 则可整节忽略。

### 4.4 VA 怎么切成「页号 + 页内偏移」（以 Sv39、4KB 页为例）

RV64 Sv39 的 VA 低 39 位参与翻译（高位符号扩展）：

```text
VA[38:0] =
  VPN[2] | VPN[1] | VPN[0] | page_offset
  9 bit    9 bit    9 bit     12 bit
```

| 字段 | 位数 | 用途 |
|------|------|------|
| `page_offset` | 12 | **不翻译**，直接拼到 PA 末尾（页内字节） |
| `VPN[0]` | 9 | 最末级页表索引 |
| `VPN[1]` | 9 | 中间级 |
| `VPN[2]` | 9 | 根页表索引 |

最终：

```text
PA = (leaf_PTE.PPN << 12) | page_offset
```

> Sv32（RV32）是两级、PTE 4 字节；Sv39 是三级、PTE 8 字节。道理一样：用 VPN 当「目录下标」层层找。

### 4.5 DTLB：到底存了什么、怎么查

把 DTLB 想成 **全相联或组相联的小 cache**，每一项大致是：

```text
{ ASID, VPN（或 VPN 的 tag 部分）, PPN, 属性位, valid }
```

**查找（Load 时）**：

```text
LSU 交出 { ASID(当前 satp), VPN=VA>>12, 访问类型=Load, 特权级 }
    │
    ▼
DTLB 比较：有没有 valid 且 ASID/VPN 匹配的项？
    │
    ├─ Hit：取出 PPN + 属性
    │         ├─ 属性允许本次 Load？ → PA 交给下游（PMP/D$）
    │         └─ 不允许（如无 R）  → page fault（不必上总线读数据）
    │
    └─ Miss：卡住/暂停本条 Load，启动 Page Walker
```

属性里和 Load 直接相关：

| PTE / TLB 属性 | Load 时 |
|----------------|---------|
| V | 无效 → page fault |
| R | 不可读 → page fault |
| U | U-mode 访问需 U=1；S-mode 还受 `sstatus.SUM` 等约束 |
| A | Access；硬件可置位或靠软件补 |
| （实现扩展）**PBMT** (Page-Based Memory Types) | 页级内存类型；与 **PMA** 合并后才决定是否进 D$（§6） |

**ASID 在查找里的角色**：见 §4.3；项里带 ASID（Global 页除外），与当前 `satp.ASID` 一致才 Hit。

**DTLB 不会**：

- 自己去「懂」总线协议（那是 walker / D$ 的事）  
- 在 Bare 模式下参与翻译  
- 替代 PMP（PMP 看的是译完后的 **PA**）

### 4.6 Page Walk：Miss 之后硬件在干什么

以 **Sv39、硬件 page walker、4KB leaf** 为例。一次 DTLB miss 最多读 **3 次 PTE**（中间若遇大页可提前结束，此处按满三级写）。

```text
root_pa = satp.PPN << 12

① 读 PTE_L2 @  root_pa + VPN[2]*8
     检查 V；若是指针项 → next = PTE_L2.PPN << 12
② 读 PTE_L1 @  next     + VPN[1]*8
     同上
③ 读 PTE_L0 @  next     + VPN[0]*8
     必须是 leaf（带 R/W/X 等），且允许本次 Load
     → 填入 DTLB：{ ASID, VPN, PPN=leaf.PPN, attrs }
     → 用新 TLB 项重试原 Load（通常立即 Hit）
```

示意（数字仅帮助建立数量级）：

```text
satp.PPN = 0x80000        → root @ 0x8000_0000
VA       = 0x0000_0000_0040_1234
           VPN[2]=0, VPN[1]=0, VPN[0]=0x40, offset=0x234

walk:
  mem[0x8000_0000 + 0*8]     → PTE_L2（指向 L1 表）
  mem[L1_base + 0*8]         → PTE_L1（指向 L0 表）
  mem[L0_base + 0x40*8]      → leaf PPN=0x20000, R=1, …
  PA = (0x20000<<12) | 0x234 = 0x2000_0234
```

任一级走不下去 → **page fault**，walker 停，**不会**再发对数据地址的 line fill。具体见下一小节。

### 4.7 Walk 失败：Page Fault 到底是什么

**Page fault** 不是总线报错，而是核在做 **地址翻译 / 页权限检查** 时主动触发的 **同步异常（exception）**：当前这条访存指令「按页表规则不能完成」，硬件转入 trap，把原因留给软件（OS/**RTOS** (Real-Time Operating System)/裸机 handler）。

#### （1）和「总线挂了」先分清

| | Page fault | Access fault（对照） |
|--|------------|----------------------|
| 典型原因 | 页表说「这页无效 / 不准这样访问」 | 翻译已过（或 Bare），但 **PMP / PMA 空穴 / 总线 ERROR** |
| 发生层 | MMU / TLB / walker | PMP、PMA、Slave |
| Load 的 `mcause` | **13** Load page fault | **5** Load access fault |
| `mtval` | 通常是出问题的 **VA** | 常为 fault 地址（VA 或实现定义） |
| 总线上 | 可能已有 PTE 读；**没有**对该 VA 对应数据的合法 fill | 可能已发出数据事务并收到 ERROR |

> 口误纠偏：有人把任何访存 trap 都叫 page fault——在 RV 上应看 `mcause`。无 MMU（`satp=Bare`）时 **根本不会有 page fault**，只有 misaligned / access fault 等。

#### （2）Load 路径上哪些情况会打出 page fault

硬件 walker（或 DTLB 命中后的权限检查）遇到例如：

| 条件 | 说明 |
|------|------|
| 某级 PTE **V=0** | 未映射；最常见「缺页」 |
| 中间级被标成 leaf / leaf 被标成指针 | 页表格式非法 → fault |
| leaf 无 **R**（Load） | 页存在但不可读 |
| **U** 位与当前特权不符 | 如 U-mode 访 S-only 页；或 S-mode 受 `sstatus.SUM` 约束 |
| 保留位 / 约定非法编码 | 实现按规范 fault |
| （若硬件查 A 位）需要置 A 但策略为 trap | 少见，看实现 |

注意：**不一定发生在 walk 中途**。若 DTLB **已经命中**旧项，但权限不够，同样直接 **Load page fault**，不会上数据总线。

Store/**AMO** (Atomic Memory Operation)、取指各有对应码（见下表）；本专文 Load 主线记 **13**。

#### （3）trap 时硬件改了什么（与 [`02-csr-trap`](02-csr-trap.md) 对齐）

以 M-mode 收 trap、一条失败的 `lw` 为例：

```text
mcause = 13          // Load page fault；Interrupt 位 = 0（同步异常）
mepc   = 这条 lw 的 PC
mtval  = 触发故障的 VA（通常是有效地址；规范允许部分实现细节）
mstatus: 按 trap 规则更新 MPIE/MIE/MPP 等
→ 跳转到 mtvec 入口
```

RV 特权规范里与页相关的同步异常码：

| `mcause`（Exception Code） | 名称 | 典型触发指令 |
|----------------------------|------|----------------|
| **12** | Instruction page fault | 取指翻译失败 |
| **13** | Load page fault | `lb/lh/lw/ld/…` 翻译或页权限失败 |
| **15** | Store/AMO page fault | `sb/sw/…`、`amoswap` 等 |

（14 为保留。）委托给 S-mode 时，同类原因进 **S** 的 `scause`/`stval`/`sepc`（`medeleg` 置位时）；MCU 常只跑 M，则都进 M。

#### （4）软件 handler 通常干什么

```text
trap_handler:
  读 mcause → 13
  读 mtval  → 出问题的 VA
  读 mepc   → 哪条指令
      │
      ├─ OS：缺页？→ 分配物理页、填 PTE、sfence.vma，再 mret 重试 lw
      ├─ OS：非法访问 / 权限？→ 杀进程或送信号
      └─ 裸机 / 简单 RTOS：日志 + panic，或重启任务（PMP 隔离场景更常见 access fault）
```

**关键**：page fault 后 **默认不会自动「修好再继续」**——必须软件改页表（或决定终止）。`mret` 回到 `mepc` 时，若页表已修好，同一条 `lw` 会重新走 DTLB/walk。

#### （5）和 walk 波形的关系

```text
lw 触发 DTLB miss
  → 总线读 PTE（可能 1～2 次已成功）
  → 某一级 V=0 或 leaf 无 R
  → walker 停止；Port-C/D 上不应再出现「数据 PA 的 line fill」
  → 核内产生 exception 13，进 mtvec
```

若 PTE 所在物理页本身 PMP 不可读或总线 ERROR：有的实现报 **access fault**（读页表失败），有的仍归入 walk 失败路径——**以 TRM/`mcause` 为准**；验证时两种都要能区分。

**本节要点**：Walk/权限失败 = **Load page fault（mcause=13）**，是翻译层的同步异常；`mtval` 带 VA，`mepc` 指向过错指令。它和 PMP/总线的 **access fault（5）** 不是一类；无 MMU 则不会出现 page fault。

### 4.8 Walker 读 PTE：还算不算「又一次 MMU」？

| 问题 | 答案 |
|------|------|
| PTE 地址是 VA 还是 PA？ | **PA**（由上一层 PPN 算出） |
| 读 PTE 还走 DTLB 翻译吗？ | **通常不**——避免「翻译需要翻译」递归 |
| 还过 PMP 吗？ | **要**——页表页必须允许当前权限读 |
| 会进 D$ 吗？ | 可以：PTE 在 cacheable 内存时，walker 可能命中 D$ 或专用 PT cache |
| 失败是总线 ERROR 吗？ | 权限/无效页 → **page fault**；真总线挂了才可能 access/bus fault |

所以波形上常见两种「读」搅在一起：

```text
时间 →
  [PTE walk 读 ×1~3]   ← 地址落在页表物理页
  [可选：数据行 fill]   ← 地址落在数据 PA 的 cache line
  [LSU 拿到字]
```

软件只看见一条 `lw`；总线上可能先有 walk、再有 fill。

### 4.9 硬件 walk vs 软件 walk

| 方式 | 谁做 | 常见于 |
|------|------|--------|
| **硬件 walker** | 核内 FSM 自动读 PTE、填 TLB | 应用核、多数带 MMU 的 RV |
| **软件 walk** | TLB miss → trap，内核用软件遍历页表再 `sfence`/填 TLB | 教学核、极简实现 |

本文默认 **硬件 walker**（和框图一致）。软件 walk 时，Port-A 上看到的是「trap 进内核」，总线读 PTE 发生在内核 `lw` 路径上，道理仍相同。

### 4.10 Miss 时端口与流水线

```text
Port-A: LSU 等翻译（指令在 MEM 级 stall 或记分牌挂起）
Walker: 1~3 次读 PTE
        ├─ 与 D$ 共用 Port-C/D（排队）
        └─ 或独立 walker master（Matrix 多一个口）
填 DTLB → Port-A 重试 → 得 PA → 再进 PMP / D$
```

和 D$ miss 的关系：

```text
DTLB miss 且 D$ 也 miss：
  先 walk（读 PTE）→ 再可能对数据 PA 做 line fill
DTLB hit 且 D$ miss：
  只有数据 fill，没有 PTE 读
DTLB hit 且 D$ hit：
  总线完全安静（单核）
```

### 4.11 和后面几层的接口一句话

```text
DTLB/walker 的产品：PA + 页属性（含可选 PBMT）
         │
         ▼
PMP：这份 PA 当前特权能不能碰？
         │
         ▼
PMA：这份 PA 是 Normal 还是 Device？默不默认 cacheable？
         │
         ▼
D$ 或 uncached 旁路：hit / fill / 直打总线
```

**本节要点**：DTLB 是 VA→PA 的 **缓存**；page walk 是 miss 时按 `satp` 根页表 **用物理地址读 PTE** 的过程。Walk/页权限失败是 **page fault（Load 为 mcause=13）**；成功只是让原 Load 重新获得 PA，真正的数据还要再过 PMP / **PMA** / D$（或旁路）/ 总线。

---

## 5. 第三层：PMP——物理地址权限

PMP 在 **物理地址** 上做区域检查，对 M/S/U 都可生效（具体锁定与权限编码见 priv spec）。典型 SoC：

```text
[0x0000_0000, ROM)
[0x2000_0000, SRAM)   R/W，按 mode 配
[0x4000_0000, MMIO)   仅 M 可访问外设窗口
…
```

### 5.1 检查时机

```text
VA ──MMU──► PA ──PMP──► 允许？
                 │否
                 └─► Access fault（Load access fault）
```

顺序必须是 **先翻译后 PMP**（对启用分页的访问）。Bare 模式则 VA≈PA，直接 PMP。

### 5.2 和 MMU 的分工

| 层 | 挡什么 |
|----|--------|
| 页表 | 「这页给不给这个进程用、能否读」 |
| PMP | 「这块物理内存允不允许当前特权碰」 |
| PMA | 「这块物理内存 **是什么类型**」（能否 cache、是否 Device…）——见 §6 |

常见故事：用户页表误映射到外设物理窗——**PMP 仍可拒绝**，这是 MCU 安全底线。  
另一类故事：外设被标成 cacheable——**PMP 放行，PMA/属性配错**，读到的是 cache 里的幻象（见 §6）。

### 5.3 检查顺序（与 PMA）

```text
VA ──MMU──► PA ──PMP（权限）──► PMA（属性）──► D$ 或 uncached 旁路
```

PMP 与 PMA 都看 **PA**，但问题不同：一个问「能不能碰」，一个问「怎么碰」。

**本节要点**：PMP 失败 **不会** 发出「非法写 SRAM」的总线波；在核内就变成 trap。抓 bus 前先确认没卡在 PMP。

---

## 6. 第四层：PMA——物理内存属性

> **PMP = Physical Memory Protection**  
> **PMA = Physical Memory Attributes**  
> 名字只差一个字母，工程上最容易混。权限过了仍可能因属性走错路径（该 uncached 却进了 D$）。

### 6.1 标准里 PMA 处在什么位置

RISC-V Privileged Spec 要求实现为每个物理地址区间提供 **PMA**，但 **不强制** 用哪套 CSR 暴露给软件——多数 MCU/应用核用下面一种或组合：

| 实现方式 | 说明 |
|----------|------|
| **固定地址图（硬件写死）** | 译码：`0x2000_0000` SRAM=cacheable；`0x4000_0000` MMIO=Device |
| **可配 PMA 寄存器/表** | 厂商 CSR：按基址+大小配 cacheable、idempotent、ordering… |
| **页表扩展（如 Svpbmt）** | leaf PTE 带 PBMT，软件按页覆盖/细化物理属性 |
| **与 PMP 表耦合一张「区域表」** | 同一槽既有 R/W/X 又有 cache 位（有的 IP 这么做） |

本文教学模型：**芯片有一张 PA→属性的查表（固定或可配）**；开了 Svpbmt 时再与 PTE 属性 **合并**（见 §6.5）。

### 6.2 PMA 通常包含哪些属性

不同核命名不一，Load 路径上要关心的核心集合：

| 属性（概念名） | 对 `lw` 的影响 |
|----------------|----------------|
| **cacheable / non-cacheable** | 能否分配进 D$；否 → **旁路 D$**，直打 Master |
| **Normal vs Device**（或 idempotent） | Device：通常不可 cache、不可推测合并、访问必须真正落到从设备 |
| **读写顺序 / 缓冲** | 是否允许写合并、读投机；Device 往往更严 |
| **可原子 / 支持 LR/SC 粒度** | 该区能否做 `lr`/`sc`、AMO（有的 MMIO 窗禁止） |
| **可执行（X 侧 PMA）** | 主要约束取指；数据 Load 偶发实现也查 |
| **空穴 / 未实现** | 访问 → access fault（有的实现归总线 ERROR） |

> 助记：PMA≈按物理区配的内存类型；PMP≈按物理区卡的特权权限（本文不展开 ARM 对照）。

### 6.3 在 Load 路径上的落点

```text
                    ┌─────────────┐
               PA ─►│ PMP check   │── fail → access fault
                    └──────┬──────┘
                           │ pass
                    ┌──────▼──────┐
                    │ PMA lookup  │  ← 按 PA 查区域属性
                    └──────┬──────┘
                           │
              ┌────────────┴────────────┐
              │ cacheable               │ non-cacheable / Device
              ▼                         ▼
         进 L1 D$                  旁路 D$（uncached 主口）
         Hit / Miss fill           按 size 发总线事务（非整行 fill）
```

**本文场景**：SRAM 窗 PMA = **Normal + cacheable** → 走 §7 D$。  
UART 等 MMIO = **Device + non-cacheable** → 永不进 D$，每次 `lw` 都上总线。

### 6.4 和端口的关系

| 情况 | Port-B 之后 | Port-C/D |
|------|-------------|----------|
| PMA cacheable + D$ hit | 回数据 | 无 |
| PMA cacheable + D$ miss | 进 fill FSM | line burst |
| PMA non-cacheable | 不进 tag 比较 | **按访问 size** 的窄事务（不是整行 fill） |

抓波辨识：对 MMIO 的 `lw` 若看到 **32B INCR8 fill**，多半是 **PMA/页属性被标成 cacheable**——经典 SoC bug。

### 6.5 PMA vs 页表属性 vs PMP（三层怎么叠）

```text
页表（VA 视图）     ：这页给不给进程；R/W/X/U；可选 PBMT
PMP  （PA 权限）    ：当前特权能不能碰这块物理区
PMA  （PA 类型）    ：这块物理区 intrinsic 是 SRAM 还是 Device、默不默认 cache
```

合并规则（实现相关，验证以 TRM 为准；常见直觉）：

| 来源 | 典型规则 |
|------|----------|
| PMP 拒绝 | **一票否决** → fault，不再看 PMA |
| PMA 标 Device | 即使 PTE 想 cacheable，最终仍 **不可 cache**（或 fault） |
| Svpbmt / PTE 类型 | 在 PMA 允许的前提下 **收紧或选择** 子类型（如 NC vs IO） |
| 仅 Bare + 固定 PMA | 无页表时，**整段属性只看 PMA** |

MCU 无 MMU：**只有 PMP + PMA**（或一张合并的区域表）。

### 6.6 典型地址图（举例）

```text
PA 区间              PMA（示例）              软件该怎么用
─────────────────    ────────────────────    ──────────────
0x0000_0000 ROM      RX, cacheable           代码/常量
0x2000_0000 SRAM     RW, Normal cacheable    数据、堆栈（可 D$）
0x3000_0000 共享SRAM RW, Normal **NC**       SMP mailbox（强制上总线）
0x4000_0000 MMIO     RW, Device NC           外设寄存器
0x5000_0000 空洞     nonexistent             访问 → fault
```

§17 软件一致出路里的「共享区 uncached」，本质就是把共享 PA **PMA（或页属性）打成 NC/Device**。

### 6.7 和总线保护位的关系（AHB/AXI）

主口上的 `HPROT` / `AxCACHE` / `AxPROT` **不是** PMA 本身，而是 Master 把「这次事务的记忆类型」告诉互联/从设备：

| 核内判定 | 总线上常见反映 |
|----------|----------------|
| cacheable fill | `AxCACHE` 可缓存类编码；burst |
| Device / NC | `AxCACHE` 非缓存；常单拍；有的桥禁 speculative |
| privilege | `AxPROT` / `HPROT` 特权位 |

从设备若是「真 Device」，即使 Master 误标 cacheable，也可能行为怪异——**根因仍应在 PMA/页表配对面改**。

### 6.8 对照句

> **PMP 决定「准不准进」；PMA 决定「进来之后当内存还是当设备、走不走 cache」。**

**本节要点**：PMA 挂在 **PA** 上，告诉后续 cacheable / Device / 原子是否允许。它与 PMP 并列、决定 D$ 旁路；把 MMIO 配成 cacheable 是嵌入式最高频的外设/一致性 magics 来源之一。

---

## 7. 第五层：D-Cache——Hit / Miss

RISC-V **基础 ISA 不规定 cache**；行为是实现定义。教学/MCU 常见模型：

| 项 | 本文默认 |
|----|----------|
| 结构 | 组相联或直接映射均可 |
| Line size | 16B / 32B / 64B（下文用 **32B** 举例） |
| 写策略 | Write-back + write-allocate（Store 节用） |
| 一致性 | 单核自洽即可 |

### 7.1 Hit

```text
索引/标签比较命中
→ 按 size/offset 抽出字
→ 经对齐/符号扩展
→ 返回 LSU
→ Port-C/D/E/F 全无事务
```

延迟：常 1～2 拍（流水设计而定）。

### 7.2 Miss

```text
1. 选 victim（若 WB 且 dirty → 先 write-back 总线写）
2. 按 line 对齐 PA，发 burst 读（Port-C）
3. 填 line，置 valid（及 tag）
4. 把原 Load 需要的字节回给 LSU
```

注意：**软件只 Load 了 4B，总线上往往是整行 32B**。这是「缓存」的代价与收益来源。  
前提：上游 **PMA 已判定 cacheable**；若 PMA=NC/Device，根本不会进本 miss 路径（见 §6）。

### 7.3 D$ 对上/对下端口

```text
对上（对 LSU / PMP / PMA）：
  req:  PA, size, load/store, (byte enables), cacheable?
  resp: data / fault / retry

对下（对 Bus Master）：
  行填充读、写回写、（可选）uncached 直通旁路
```

**本节要点**：D$ 是「PA 空间上的加速器」，且 **只服务 PMA 允许 cache 的区**；Hit 截断整条总线路径，Miss 把一次字访问放大成 line 事务。

---

## 8. 第六层：总线主口——AHB-Lite / AXI

Cache miss 或 uncached 访问时，Bus Master 把「读多少、从哪」翻译成协议信号。

### 8.1 AHB-Lite（MCU 极常见）

一次 **32B line fill**、假设数据宽 32-bit，需要 **8 beat** `INCR`：

| 信号 | Line fill 时 |
|------|----------------|
| `HADDR` | 行首，然后每拍 +4 |
| `HTRANS` | NONSEQ → SEQ… |
| `HSIZE` | Word |
| `HBURST` | INCR8（或 INCR） |
| `HWRITE` | 0 |
| `HPROT` | data、privilege 等 |
| `HRDATA`/`HREADY` | 从机返回 |

单拍 Load（uncached）则 `HTRANS=NONSEQ`，`HBURST=SINGLE`。

### 8.2 AXI4（应用核常见）

| 通道 | Line fill |
|------|-----------|
| AR | `ARADDR`=行首，`ARLEN`=beats-1，`ARSIZE`，`ARBURST=INCR`，`ARID` |
| R | 连续 `RLAST` 前多拍 `RDATA`；`RRESP=OKAY` |

Load 字访问若走 uncached：`ARLEN=0` 单拍即可。

### 8.3 主口上还要带的「身份」

| 信息 | 用途 |
|------|------|
| Secure / Privilege（`AxPROT` / `HPROT`） | 外设防火、调试 |
| Instruction vs Data | 区分取指/数据（本路径为 Data） |
| Master ID | Matrix 仲裁、错误上报 `HMASTER`/`AxID` |

> Exclusive/`ARLOCK`：**普通 `lw` 不置位**。原子路径见 [`01-spinlock-to-bus`](../05-bus-rtl/01-spinlock-to-bus.md)；RV 侧对应 `lr.w`/`sc.w`。

**本节要点**：主口协议是 D$ miss FSM 的「外语」；同一 PA 的 line fill，AHB 与 AXI 只是拍数与通道拆分不同。

---

## 9. 第七层：Bus Matrix——仲裁与译码

片上通常不是点对点，而是：

```text
 Masters:  D$ / I$ / DMA / (Debug) / (Walker)
      │
      ▼
  Bus Matrix
      │
      ├── SRAM
      ├── APB bridge → UART/…
      ├── ROM
      └── …
```

### 9.1 两件独立的事

| 功能 | 做什么 |
|------|--------|
| **Decode** | 用地址高位选 slave |
| **Arbiter** | 多 master 同时要同一 slave（或共享段）时谁先走 |

### 9.2 和「端口」的关系

- Port-D：核侧看到的「我发起的请求」  
- Port-E：SRAM 侧看到的「矩阵帮我转过来的请求」  
- 中间可能有 **upsizer/downsizer、AHB↔AXI 桥、流水寄存器**——延迟加拍，但地址/数据语义应保持。

### 9.3 单核 Load 时谁在抢

单核无 DMA 时，常见竞争来自：

- I$ miss 与 D$ miss  
- Page walk 与 D$ fill  
- Debug 口

所以：**即便「只有一个 hart」，矩阵仍可能不是旁路。**

**本节要点**：Matrix 决定「请求落到哪块 slave、何时获准」；不负责 cache 一致性语义。

---

## 10. 第八层：SRAM Controller / Slave

### 10.1 Slave 口（以 AHB 为例）

```text
HSEL && HTRANS!=IDLE && HREADY
  → 锁存地址/控制
  → 下一拍或当拍驱动 HRDATA（视时序）
  → HRESP=OKAY
```

错误地址（未实现洞）：`HRESP=ERROR` → 核侧通常打成 **bus error / access fault**（实现相关，需在 TRM 写死）。

### 10.2 Port-F：阵列口

```text
sram_addr  = PA 在窗口内的 offset（常去掉基址、按 word 对齐）
sram_cen   = chip enable
sram_wen   = 写使能（Load 时为读）
sram_ben   = byte enable（读时常全 1 或忽略）
sram_rdata = 同步读出
```

编译器/综合后的单口 SRAM IP：读延迟固定（如 1 cycle），controller 用 `HREADY` 反压对齐。

### 10.3 地址图（举例）

```text
0x2000_0000 – 0x2001_FFFF   128KB SRAM
Matrix 规则:  if (addr[31:17]==…) → SRAM_HSEL
```

软件 VA 经页表映到 `0x2000_xxxx`；总线只看见 PA。

**本节要点**：SRAM「从口」很薄——译码、时序、错误；**不理解** VA、TLB、cache line 语义。

---

## 11. 闭环时序 A：D-Cache Hit

假设：TLB hit、PMP pass、D$ hit。

```text
周期概念序（可合并拍数，示意因果）：

t0  LSU: VA ready
t1  DTLB: hit → PA
t2  PMP: ok
t3  PMA: cacheable
t4  D$: tag hit → data
t5  WB: rd ← data

总线 Port-D/E/F：无活动
```

抓波清单：核内握手有、**AXI/AHB 安静**。若此时总线在动，多半是取指或其它 master，不是这次 `lw`。

---

## 12. 闭环时序 B：D-Cache Miss + Line Fill

再假设：TLB hit、PMP pass、D$ miss、line clean（无写回）、SRAM 窗口正确。

```text
t0   LSU VA
t1   DTLB hit → PA
t2   PMP ok
t3   PMA: cacheable（若 NC 则不会走 fill，见 §6）
t4   D$: miss → 启动 fill，stall 流水
t5   Master: 发起 burst 读（行首 PA）
t6…  Matrix grant → SRAM slave 连续返回 beats
tN   Line 填入 D$，valid=1
tN+1 抽出字 → LSU → WB
```

若 **DTLB 也 miss**：

```text
t0   LSU VA
t1   DTLB miss → walker
t2…  总线读 PTE（可能 2～3 次）
tk   TLB 填好 → 重试
     再进入上面的 Hit 或 Miss 路径
```

示意波形（AXI line fill，32B / 64-bit 数据宽 → 4 beats）：

```text
AR:  ADDR=line_base  LEN=3  SIZE=3(8B)  BURST=INCR  ID=…
R:   DATA0  DATA1  DATA2  DATA3+RLAST  RESP=OKAY
```

AHB 同址则看到 `HADDR` 递增的 `SEQ` 拍。

**本节要点**：Miss 路径的「主人」是 **D$ fill FSM**；LSU 只是在等「字数据就绪」。

---

## 13. 对照：Store 与 Fence

### 13.1 Store（同路径，方向相反）

| 阶段 | 与 Load 的差异 |
|------|----------------|
| TLB | 需要 **W=1**；可能置 Dirty |
| PMP | 查写权限 |
| PMA | Device/NC 则直打总线；cacheable 才进 D$ |
| D$ Hit + WB | 常只改 cache，**暂不上总线** |
| D$ Miss + write-allocate | 先 fill 再改（或 write-no-allocate 直写） |
| 写回 | victim dirty 时 Port-C 变 **写 burst** |

所以：**`sw` 成功 ≠ 总线上立刻看到写**——WB cache 下要等到替换或软件 flush。

### 13.2 `fence` / `fence.i`

| 指令 | 作用（直观） |
|------|----------------|
| `fence` | 约束本 hart 上某些内存操作的可见顺序 |
| `fence.i` | 同步 I$ 与后续取指（自修改代码） |

单核 + 强序实现里，`fence` 可能几乎是 nop；一旦有写缓冲、多口、或 SMP，fence 才「值钱」。验证时不要假设「所有 RV 核 fence 都刷总线」。

### 13.3 显式 cache 维护

若平台提供 `cbo.*` 或自定义 **CMO** (Cache Management Operation)：DMA 前对 buffer 做 clean/invalidate——与 Linux DMA API 同构，细节见内核内存篇。

---

## 14. 从单核到 SMP：总框图与端口增量

单核路径（§1–13）对每个 hart **原样成立**。第二个 hart 并不是「再画一条到 SRAM 的线」这么简单——多数模块要 **复制**，少数 **共享**，再多问一句：**两套 D$ 里同一 PA 的数据如何保持同一真相？**

### 14.1 双 hart 总框图

```text
┌─────────────────────────────┐   ┌─────────────────────────────┐
│ Hart0                       │   │ Hart1                       │
│ LSU0→DTLB0→PMP0→PMA0→D$0 │   │ LSU1→DTLB1→PMP1→PMA1→D$1 │
│                 │ Master0   │   │                 │ Master1   │
└─────────────────┼───────────┘   └─────────────────┼───────────┘
                  │ Port-D0                         │ Port-D1
                  └────────────┬────────────────────┘
                               │
                    ┌──────────▼──────────┐
                    │ Bus Matrix / Xbar   │  ← 共享
                    │ decode + arbiter    │
                    └──────────┬──────────┘
                               │ Port-E
                    ┌──────────▼──────────┐
                    │ SRAM Controller     │  ← 共享（一份物理真相）
                    └──────────┬──────────┘
                               │ Port-F
                            ┌──▼──┐
                            │SRAM │
                            └─────┘

可选：DMA / Debug 再挂 Matrix
可选：Coherence 互联（snoop 口）挂在 D$ 旁 —— §16
```

### 14.2 端口怎么编号

| 代号 | SMP 下的变化 |
|------|----------------|
| Port-A0 / A1 | 每 hart 私有：LSU↔DTLB |
| Port-B0 / B1 | 每 hart 私有：PMP/PMA 后到本核 D$（或 NC 旁路） |
| Port-C0 / C1 | 每 hart 私有：本核 D$↔本核 Master |
| **Port-D0 / D1** | **两条对等主口** 进 Matrix（Master ID 不同） |
| Port-E / F | **仍共享**；仲裁后串行或 interleaved 访问同一 SRAM |

另增（实现可选）：

| 代号 | 含义 |
|------|------|
| **Port-Snoop** | 一致性互联 → 各 D$：Invalidate / Probe / Data 转发 |
| **Port-IPI** | 核间软中断（RV 上常见 **MSIP** (Machine Software Interrupt-Pending) / **ACLINT** (Advanced Core Local Interruptor)）；不经 SRAM，但配合 TLB 射齐 |

### 14.3 一句分水岭

> **单核**：D$ 命中 ⇒ 总线可完全安静。  
> **SMP 无 snoop**：Hart0 的命中 **不能** 代表 Hart1 看到的值；「安静的总线」甚至更危险——旧数据锁在对方 cache 里，SRAM 也被蒙在鼓里。

---

## 15. SMP 逐层对照：哪些复制、哪些共享

| 层 | 复制 or 共享 | Load 路径影响 |
|----|--------------|----------------|
| LSU / 流水线 | **每 hart 一份** | 两核可同时算 VA、同时 stall |
| DTLB + walker | **每 hart 一份** | 同 VA 可能映到同 PA；改页表要射齐 |
| `satp` | 每 hart CSR | 可跑不同地址空间；也可同 OS 同 ASID 策略 |
| PMP | 每 hart CSR | 通常镜像同一区域表；开机各写一遍 |
| PMA | 每 hart 查表或共享固定图 | 属性图应一致；共享区常打成 NC |
| L1 D$ | **每 hart 私有** | **一致性问题的根源** |
| Bus Master | 每 hart 一（或数）口 | Matrix 看到多 master |
| Bus Matrix | **共享** | 仲裁、优先级、outstanding |
| SRAM / 外设从口 | **共享** | 物理只有一份；最终写回落点 |
| **PLIC** (Platform-Level Interrupt Controller) / ACLINT | 共享外设 | 中断路由、IPI；不在 Load 数据通路上 |

**没有复制的「全局 D$」**——除非你显式做共享 L2；本文仍坚持「仅私有 L1」，把问题暴露清楚。

**本节要点**：SMP 相对单核，Load 的「垂直路径」不变；变的是 **横向多了一套对称路径 + 共享底座**，以及私有 D$ 之间要不要说话。

---

## 16. 无一致性硬件：三条必挂场景

假设：双 hart、**私有 write-back D$、无 snoop、共享区标成 cacheable**。这是教学上最有用的「先看会怎么坏」。

### 16.1 场景 A：写后读（对方仍 hit 旧行）

```text
初始：行 @PA 在两核都无效；SRAM 中字 = 0

Hart0: sw 1, (x)     // miss→fill→改 D$0，dirty；SRAM 仍可能是 0
Hart1: lw  y, (x)    // 若也曾 fill 过旧行，或稍后 fill 到旧 SRAM
                     // → 读到 0，软件以为「写没发生」
```

时序示意：

```text
Hart0 D$0:  [line] = 1 (M/Dirty)     SRAM: 0
Hart1 D$1:  [line] = 0 (E/S) 或稍后从 SRAM fill 到 0
总线：Hart0 的 sw 可能全程不上总线（WB）
```

### 16.2 场景 B：双写丢失

```text
Hart0 / Hart1 都对同一字做 read-modify-write（无锁）
两边都在自己的 D$ 里改 → 后写回的覆盖先写回的 → 丢更新
```

即使用 `amoswap`/`amoadd`，若实现把 AMO 拆成「本地 cache 里非原子 RMW」且无保留集/总线锁，同样会坏——见 §17。

### 16.3 场景 C：DMA 与 CPU

```text
Hart0 填好 buffer（数据在 D$0）
DMA 从 SRAM 搬 → 读到旧内容
或 DMA 写入后 Hart0 仍 hit 旧 D$ 行
```

单核也有 DMA 一致性问题；SMP 只是再乘上「哪个 hart 的 cache」。

### 16.4 波形级直觉（无 snoop）

```text
Hart0:  SW 成功，Master0 安静（hit + WB）
Hart1:  LW miss → AR/R 读 SRAM → 得到旧值
SRAM:   仍是旧值，直到 Hart0 换出 dirty 行才 AW/W
```

抓波结论：**不能**用「总线上有没有写」判断「别的 hart 该不该看见新值」——在无协议时，真相分裂在多个 D$ 里。

**本节要点**：无硬件一致时，cacheable 共享 = 未定义行为；验证计划里应标为 **禁止**，或强制走 §17 的出路之一。

---

## 17. 三条工程出路：软件 / snoop / AMP

### 17.1 出路 1：软件管理一致性（MCU / 简单 SMP 常用）

| 手段 | 做法 |
|------|------|
| **私有分区** | 每 hart 堆栈/堆在不同 PA，禁止 cacheable 共享 |
| **共享区 uncached** | 页属性 / PMA：Device 或 Non-cacheable；Load/Store 直打 SRAM |
| **显式 CMO** | 写完 `cbo.clean`/`flush`，读前 `cbo.inval`；或平台 custom CMOs |
| **消息 + 所有权** | 只在「当前 owner hart」上 cache；移交前 flush |

端口影响：共享变量的 Load **经常走到 Port-D**（不再被 D$ 截断），延迟变高、可预测性变好。

### 17.2 出路 2：硬件 Cache Coherence（应用级 SMP）

在私有 L1 旁增加 **一致性代理**（概念上）：

```text
D$0 ↔ Snoop/Coherence IF ↔ Interconnect ↔ Snoop/Coherence IF ↔ D$1
                │
                └── 仍可经 Memory 口访问 SRAM（目录或总线嗅探）
```

常见状态直觉（不必死背某一协议名）：

| 状态感 | 含义 |
|--------|------|
| Invalid | 本核无有效副本 |
| Shared | 可读；他核也可能有只读副本 |
| Exclusive / Modified | 本核可写；他核不得再持有 |

Hart0 `sw` 时，协议保证：先让 Hart1 上同行 **Invalidate**（或拿到数据转发），再完成本地写。  
Hart1 再 `lw`：miss → 可能从 Hart0 cache **转发**，或从 SRAM（若已写回）取新值。

> 总线协议名（ACE / CHI / TileLink / 自定义）留给总线专题；此处只要建立：**多了一个 Port-Snoop，Load 路径在 miss 时可能根本不进 SRAM。**

### 17.3 出路 3：AMP（非对称）——机器人很常见

```text
Hart0: Linux，cacheable 大内存
Hart1: RTOS，紧耦合 TCM / 私有 SRAM 窗
共享：uncached mailbox + 门铃 **IRQ** (Interrupt Request)（MSIP/外部中断）
```

这不是「残缺 SMP」，而是 **刻意不做硬件一致**，用软件契约换确定性。与 [`../03-rtos-bsp/01-freertos-realtime.md`](../03-rtos-bsp/01-freertos-realtime.md) 选型一致。

### 17.4 选型表

| 需求 | 更合适 |
|------|--------|
| 双核跑同一 Linux、共享堆 | 硬件 coherence + `lr`/`sc` |
| 双核 MCU、共享标志位 | uncached 原子区，或关 D$ 的 SRAM 窗 |
| 大核 Linux + 小核电机 | AMP + mailbox |
| 只有「偶发共享只读表」 | 只读副本 + 更新时广播 inval |

**本节要点**：一致性是 **产品选择**，不是 Load 路径上自动长出来的；框图里有没有 Port-Snoop，决定 §15 的场景是否合法。

---

## 18. 原子、Fence、TLB 射齐（RV）

### 18.1 `lr` / `sc` 与保留集（Reservation Set）

RISC-V 原子自旋的典型形态：

```text
1:  lr.w   t0, (a0)       // 保留地址 a0
    bne    t0, zero, …    // 锁被占则等/重试
    sc.w   t1, t2, (a0)   // 条件写；t1=0 成功
    bnez   t1, 1b
```

和单核普通 `lw`/`sw` 的差异：

| | 普通 Load/Store | `lr`/`sc` |
|--|-----------------|-----------|
| 保留集 | 无 | hart 内记下地址（粒度常 ≥ 字，实现定义） |
| 成功条件 | 无 | 自 `lr` 以来保留集未被「干扰」 |
| 干扰来源 | — | 本核其它写、**他核写同保留粒度**、部分实现含缓存驱逐等 |
| 总线 | 普通读写 | 常需 interconnect/monitor 配合（类似 Exclusive） |

细节与「锁不在整条总线」的直觉，见 [`01-spinlock-to-bus`](../05-bus-rtl/01-spinlock-to-bus.md)；RV 把指令叫 `lr`/`sc`，语义同族。

AMO（`amoadd.w` 等）：可在 **本地 cache 在 Modified 时核内完成**，或 **锁住行 / 走总线原子通路**——实现必须保证多核对同址 AMO 的原子性；验证要按 TRM 对波形。

### 18.2 `fence` 在 SMP 上变「真」

```text
Hart0:  sw  data
        fence w, w      // 或更宽
        sw  flag, #1

Hart1:  lw  flag
        // 旋等到 1
        fence r, r
        lw  data        // 期望看到新 data
```

无 fence、仅靠程序顺序时，写缓冲 / 旁路 / 多口重排可能让 Hart1 先看到 flag、后看到旧 data（取决于内存模型与实现）。  
**单核强序核**上 fence 常很轻；**SMP** 上它是发布-订阅的契约点。

### 18.3 TLB 射齐：`sfence.vma` + IPI

页表是 **内存里的共享结构**；TLB 是 **每核私有缓存**。

```text
Hart0（改页表者）:
  1. 改 PTE（注意自身 D$ / 需要时 fence）
  2. sfence.vma          // 刷本核 TLB
  3. 写 mailbox / 发 IPI（MSIP）给其它 hart
Hart1（收到 IPI）:
  4. sfence.vma（指定 ASID/地址或全局）
  5. 应答完成
Hart0:
  6. 等待全部应答后，再假定「旧映射已死」
```

漏射齐的典型故障：Hart1 仍用旧 PPN → 写到已释放物理页（silent corruption）。

### 18.4 PMP / PMA 在 SMP

- 每核自己的 `pmpcfg`/`pmpaddr`；**不会自动广播**。  
- 运行时改 PMP（少见，多在 boot）要对齐所有 hart，否则同 PA 一核能访问、一核 access fault。  
- **PMA 图**（固定或可配）也应一致：同一共享窗若一核当 cacheable、一核当 NC，软件契约直接崩。

**本节要点**：数据一致靠 cache 协议或软件契约；**映射一致**靠 `sfence.vma`+IPI；**互斥**靠 `lr`/`sc`/AMO——三者层次不同，不要混成「一个锁搞定」。

---

## 19. SMP 闭环时序与抓波

### 19.1 双核同时 D$ miss 同一行（无 snoop）

```text
Hart0 miss fill ──AR──┐
                      ├─► Matrix 仲裁 ─► SRAM 返回两趟（或 interleaved beats）
Hart1 miss fill ──AR──┘
两边各自填入 D$0 / D$1，副本相同（若其间无写）
之后任一核 sw → 进入 §16 的分裂世界
```

### 19.2 有 snoop 时：Hart0 写、Hart1 再读（概念序）

```text
Hart0 sw:
  1. 查本地状态；若需升级权限 → Probe Hart1
  2. Hart1：Invalidate 同行（若 dirty 先写回或转发数据）
  3. Hart0：本地行变 Modified，写入新值
Hart1 lw:
  4. miss（已被 inval）
  5. 向互联要数据 → 可能从 Hart0 转发，或从 SRAM
  6. 填 D$1，返回 LSU
```

对比无 snoop：第 2 步不存在 → Hart1 可能一直 hit 旧值。

### 19.3 建议探针（在单核清单上追加）

| 探针 | 看什么 |
|------|--------|
| Master0 vs Master1 | 同 PA 的 AR/AW 交错、ID |
| Matrix grant | 饿死、优先级是否符合实时核需求 |
| D$0/D$1 tag + 状态 | 无协议时是否双 Modified |
| Snoop 通道（若有） | Inval 是否先于对端再 hit |
| MSIP / IPI | TLB 射齐是否成对出现 |
| `sc` 成败 / AMO | 与他核写同址的因果 |

### 19.4 断言清单

**单核（保留）**

1. PMP fail ⇒ 无对应 Master 请求。  
1b. PMA=Device/NC 的 Load ⇒ **不应**出现对该 PA 的 cache line fill（应是按 size 的窄事务）。  
2. D$ hit Load ⇒ 无本次 fill。  
3. Miss fill ⇒ 行对齐，beat 数匹配。  
4. Walker PTE 地址合法且过 PMP。  
5. Slave ERROR ⇒ 可观测 fault。

**SMP**

6. **无 snoop 且共享 cacheable**：验证不得依赖「跨核写后读一致性」；或将该区标禁止。  
7. **有 snoop**：Hart0 写完成后，Hart1 不得再对同行保持有效旧副本（状态机可查）。  
8. `sfence.vma` 完成后，本核旧 VPN→PPN 不得再命中（除非重新 walk）。  
9. 两核对同址 `sc`：至多一个成功（保留集/monitor 语义）。  
10. 实时 hart 的 Matrix 优先级：长 burst（line fill）不得无界挡住其 deadline 路径（产品约束）。

---

## 附录 A：裸机无 MMU 的最短路径

许多 RV MCU：`satp=Bare`，只有 M-mode + PMP（+ PMA）。

```text
LSU:  addr = rs1+imm     （此时即 PA）
  → PMP
  → PMA（决定 cacheable / Device）
  → D$ 或 TCM / uncached 直连
  → Bus / 本地 SRAM 口
```

端口仍在，只是 **砍掉 Port-A 的翻译与 walker**。无 MMU 时 **PMP+PMA 仍在**。教学时可先跑通这条，再加 Sv\*。

**TCM** (Tightly-Coupled Memory) / **ILM** (Instruction Local Memory) / **DLM** (Data Local Memory)：往往 **不经外部 Bus Matrix**，LSU 旁路到紧耦合 RAM——延迟固定，适合实时。可与「Cacheable SRAM 经矩阵」对照理解。

**双核 MCU**：每核私有 TCM + 共享 uncached SRAM 做 mailbox，是 AMP 的缩略版，常比硬上 snoop 更划算。

---

## 附录 B：异常对照表（Load 相关）

| 条件 | 典型 `mcause`（Load） | 卡在哪一层 |
|------|----------------------|------------|
| 非对齐（且不支持） | **4** Load address misaligned | LSU |
| PTE 无效 / 无 R / 特权与 U 位不符等 | **13** Load page fault | MMU / walker / DTLB 权限 |
| PMP 拒绝 | **5** Load access fault | PMP |
| PMA 空穴 / 禁止属性 | **5** Load access fault（实现定义） | PMA |
| 总线 ERROR | **5** Load access fault（实现定义） | Slave / 桥 |
| ECC 不可纠 | 实现定义（NMI / fault） | SRAM / cache |

页故障时常见 CSR 快照：`mcause=13`，`mtval=faulting VA`，`mepc=过错 lw`。取指/Store 对应 **12 / 15**。细节见 §4.7 与 [`02-csr-trap.md`](02-csr-trap.md)。

> **口诀**：翻译/页权限不行 → **page fault**；翻译过了但物理保护或总线拒绝 → **access fault**。

---

## 小结

- 一次 `lw` 到 SRAM：LSU → **TLB/MMU** → **PMP** → **PMA** → **D$**（或 NC 旁路）→ **Bus Master** → **Matrix** → **SRAM Slave** → 阵列；Hit 在 D$ 截断。  
- **PMP ≠ PMA**：前者问「准不准进」，后者问「当内存还是 Device、走不走 cache」。  
- **每个 Port 职责不同**：翻译、权限、属性、缓存、协议、仲裁、从口时序——抓波要对号入座。  
- Page walk 与 line fill **都会占用总线**；前者读 PTE，后者读数据行。Walk/页权限失败是 **Load page fault（`mcause=13`）**，不是总线 ERROR；与 PMP 的 access fault 分层。  
- DTLB 用 **ASID** 区分同 VA 的不同进程；虚拟化再叠加 **VMID**（`hgatp`，不是 HID/`mhartid`）。  
- Store 在 write-back 下可长时间不上总线；顺序靠 `fence` / 平台 CMO。  
- **SMP**：垂直路径复制为 Port-\*0/\*1，Matrix/SRAM 共享；私有 D$ 迫使你在 **软件一致 / snoop / AMP** 里三选一。  
- **`lr`/`sc`、fence、`sfence.vma`+IPI** 分别解决互斥、顺序、映射射齐——与「普通 Load 数据通路」分层。

## 自测

1. 画出单核 `lw` 的模块图，并标出 Port-A…F 各传什么。  
2. D$ hit 与 miss 时，哪些端口有活动？  
3. DTLB miss 时总线在干什么？PTE 访问还经 MMU 吗？  
3b. Walk 失败时 `mcause`/`mtval`/`mepc` 各是什么？和 Load access fault 差在哪？  
3c. ASID 解决什么问题？虚拟化下 VMID 在哪个 CSR？和 `mhartid` 是一回事吗？  
4. PMP 失败会不会在 SRAM 口看到一次 ERROR？为什么？  
5. 为何软件 Load 4B，miss 时总线可能读 32B？  
6. Write-back 下 `sw` 成功后，另一 master 读 SRAM 一定看到新值吗？  
7. 画出双 hart 框图：标出复制模块与共享模块；Port-D0/D1 与 Port-E 各是什么？  
8. 无 snoop 时，Hart0 `sw` 后 Hart1 `lw` 为何可能读到旧值？总线上一定有写吗？  
9. 软件一致 / 硬件 snoop / AMP 各适合什么产品？共享区 uncached 时 Load 还走 D$ 吗？  
10. `lr`/`sc`、`fence`、`sfence.vma`+IPI 各解决哪一类问题？  
11. 无 MMU 的 MCU 路径去掉了哪一段？双核 + 私有 TCM + 共享 mailbox 属于哪条出路？  
12. PMP 与 PMA 各回答什么问题？MMIO 被标成 cacheable 时波形上可能看到什么异常？  
13. PMA=Device 时，Load 还走 D$ line fill 吗？与 `AxCACHE`/`HPROT` 是什么关系？

---

*`04-riscv-core` · Load 路径：Core → SRAM（PMP/PMA · 单核 → SMP）*
