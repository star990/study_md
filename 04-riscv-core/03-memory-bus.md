# RISC-V：一次 Load 从 Core 到 SRAM（单核 → SMP）

> **系列**：`04-riscv-core`  
> **前置**：[`02-csr-trap.md`](02-csr-trap.md)（特权级与 trap CSR）；流水线形态见 [`01-pipeline.md`](01-pipeline.md)  
> **相关**：[`../05-bus-rtl/01-spinlock-to-bus.md`](../05-bus-rtl/01-spinlock-to-bus.md)（原子/Exclusive）；软件侧页表/DMA 见 [`../01-kernel/02-memory.md`](../01-kernel/02-memory.md)

上一篇总线专文把 **spinlock** 讲到 AXI Exclusive / AHB `HLOCK`。本文换一条更日常的路径：  
**`lw` 从片上 SRAM 读一个字**——从 LSU 算地址，经 TLB/MMU、PMP、一级 D-cache、总线主口、Bus Matrix，再到 SRAM slave。  
§1–12 先把 **单核** 每一跳端口讲闭环；§13–18 再把 **双 hart SMP** 的复制/共享、无 snoop 失败场景、三条工程出路，以及 `lr`/`sc`、fence、TLB 射齐补齐。全文以 **RISC-V** 为本，不展开 ARM。

**读完应能**：
- 画出单核 `lw → SRAM` 上每一级模块与端口
- 分清 Hit / Miss 两条时序，以及 page walk 为何也占总线
- 说明 VA→PA、PMP、cacheable 属性各自挡在哪一层
- 画出双 hart 框图：哪些模块复制、哪些共享，并解释无 snoop 时为何会读到旧值
- 对照软件一致 / 硬件 snoop / AMP，以及 `lr`/`sc`、`fence`、`sfence.vma` 落在哪一层

---

## 目录

1. [场景假设与总框图](#1-场景假设与总框图)
2. [端口清单：每一跳叫什么](#2-端口清单每一跳叫什么)
3. [第一层：LSU——算出 VA，发起访存](#3-第一层lsu算出-va发起访存)
4. [第二层：MMU / TLB——VA → PA](#4-第二层mmu--tlbva--pa)
5. [第三层：PMP——物理地址权限](#5-第三层pmp物理地址权限)
6. [第四层：D-Cache——Hit / Miss](#6-第四层d-cachehit--miss)
7. [第五层：总线主口——AHB-Lite / AXI](#7-第五层总线主口ahb-lite--axi)
8. [第六层：Bus Matrix——仲裁与译码](#8-第六层bus-matrix仲裁与译码)
9. [第七层：SRAM Controller / Slave](#9-第七层sram-controller--slave)
10. [闭环时序 A：D-Cache Hit](#10-闭环时序-ad-cache-hit)
11. [闭环时序 B：D-Cache Miss + Line Fill](#11-闭环时序-bd-cache-miss--line-fill)
12. [对照：Store 与 Fence](#12-对照store-与-fence)
13. [从单核到 SMP：总框图与端口增量](#13-从单核到-smp总框图与端口增量)
14. [SMP 逐层对照：哪些复制、哪些共享](#14-smp-逐层对照哪些复制哪些共享)
15. [无一致性硬件：三条必挂场景](#15-无一致性硬件三条必挂场景)
16. [三条工程出路：软件 / snoop / AMP](#16-三条工程出路软件--snoop--amp)
17. [原子、Fence、TLB 射齐（RV）](#17-原子fencetlb-射齐rv)
18. [SMP 闭环时序与抓波](#18-smp-闭环时序与抓波)
19. [附录 A：裸机无 MMU 的最短路径](#附录-a裸机无-mmu-的最短路径)
20. [附录 B：异常对照表（Load 相关）](#附录-b异常对照表load-相关)
21. [小结](#小结)
22. [自测](#自测)

---

## 1. 场景假设与总框图

### 1.1 本文固定模型（先钉死，避免混谈）

| 项 | 假设 |
|----|------|
| ISA | RV32/RV64；指令 `lw rd, imm(rs1)` |
| 核 | §1–12：**单 hart**；§13 起：**双 hart SMP**（可推到 N） |
| Cache | **每 hart 私有一级 D-cache**（无 L2）；I-cache 对称存在但不走本路径 |
| 目标 | 片上 **SRAM**（Normal、可 cache；非 Device MMIO） |
| 地址翻译 | 开 **Sv32 / Sv39**（§附录 A 给无 MMU 捷径） |
| 保护 | **PMP** 开（每 hart 一份 CSR，配置常镜像） |
| 总线 | 数据侧主口用 **AHB-Lite** 或 **AXI4**；多 master 进同一 Matrix |
| 一致性 | 先讲 **无硬件一致** 的失败，再给软件 / snoop / AMP 三条出路 |

> **为何只谈一级 D$**：把「核内 → 总线 → SRAM」每一跳端口讲透；加 L2/CHI 会再多一层，留给后续专题。

### 1.2 软件一眼看到的

```text
  // rs1 = 某 VA，指向已映射到片上 SRAM 的页
  lw  x5, 0(x10)     // 读 4 字节 → x5
```

硬件要做的事可以压缩成一句：

> **把 VA 变成 PA，查权限，尽量用 D$ 命中；没命中就按 cache line 从 SRAM 拉回来，再把请求的字交给流水线。**

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
                    │                      │ DTLB (+ walker)     │ │
                    │                      │  miss → page walk   │──┼──► 也会走总线！
                    │                      └──────────┬──────────┘ │
                    │                                 │ PA + attr  │
                    │                      ┌──────────▼──────────┐ │
                    │                      │ PMP check           │ │
                    │                      └──────────┬──────────┘ │
                    │                                 │ Port-B     │
                    │                      ┌──────────▼──────────┐ │
                    │                      │ L1 D-Cache          │ │
                    │                      │  Hit → data 回 LSU  │ │
                    │                      │  Miss→ fill FSM     │ │
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

**本节要点**：一条 Load 不是「LSU 直接捅 SRAM」，中间至少有 **翻译、权限、缓存、主口、矩阵、从口** 六段；page walk 还会 **再占用一次总线**。

---

## 2. 端口清单：每一跳叫什么

造核 / 抓波时，先把「接口契约」说清楚。下表是本文统一命名。

| 代号 | 谁 → 谁 | 典型载荷 | 单核备注 |
|------|---------|----------|----------|
| **Port-A** | LSU → DTLB | VA、ASID（若有）、访问类型 `Load`、特权级、size | 未命中则 walker 用 PA 读页表 |
| **Port-B** | PMP → D$（或 LSU→D$） | **PA**、size、R/W/X 意图、cacheable 等属性 | PMP 失败则 **不上总线**，直接 trap |
| **Port-C** | D$ → Bus Master | 行填充：对齐到 **cache line** 的 burst 读 | Hit 时 Port-C **静默** |
| **Port-D** | Master → Matrix | AHB：`HADDR/HTRANS/HSIZE…`；AXI：`AR*` / `R*` | Master ID 供仲裁 |
| **Port-E** | Matrix → SRAM Slave | 译码后的从口时序（可与 Port-D 同协议，或经桥） | 多 slave 时按地址窗口切 |
| **Port-F** | Controller → SRAM 阵列 | `addr / cen / wen / wdata / rdata`、字节使能 | 常 1 拍读延迟 |

另有两条「旁路主口」，初学者易漏：

| 旁路 | 何时出现 | 走哪 |
|------|----------|------|
| **I-Fetch 主口** | 取指 | 通常另一套 I$ / ITCM，**不经 D$** |
| **Page-walk 主口** | DTLB miss | 读 PTE；可与 D$ **共享**同一 Bus Master，也可独立 walker 口 |
| **Uncached / Device** | MMIO 或属性标 non-cacheable | **绕过 D$**，LSU→PMP→Master 直打从设备 |

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
EX/AGU 算出 VA
   → MEM：等 DTLB + PMP + D$（或 bus）返回
   → WB：写回 rd
```

D$ miss 或 page walk 时，MEM 级 **stall**（或记分牌挂起该指令）；单发射五级流水最直观。

**本节要点**：LSU 只认 **VA + 访问意图**；PA、能不能 cache、走不走总线，都是后面几层的事。

---

## 4. 第二层：MMU / TLB——VA → PA

> RISC-V 分页由 **`satp`** 控制。M-mode 通常可关分页（`satp.MODE=Bare`）；S/U 下访存才走页表。MCU 很多产品 **只做 PMP、不做 MMU**——见附录 A。本节按「开了 Sv\*」写全。

### 4.1 `satp` 一眼

| 字段 | 含义 |
|------|------|
| MODE | Bare / Sv32 / Sv39 / … |
| ASID | 地址空间 ID（TLB 标签） |
| PPN | 根页表物理页号 |

### 4.2 DTLB

```text
查找键 ≈ { ASID, VPN }
命中 → 得到 PPN + PTE 属性（R/W/X/U/A/D、可能的缓存提示）
未命中 → Page Walker
```

属性里和 Load 直接相关：

| PTE 位 | Load 时 |
|--------|---------|
| V | 无效 → page fault |
| R | 不可读 → page fault |
| U | U-mode 访问需 U=1；S-mode 还受 `sstatus.SUM` 等约束 |
| A | Access；硬件可置位或靠软件 |
| （实现扩展）PBMT / PMA | 暗示 cacheable / Device 等 |

### 4.3 Page Walk（仍是「访存」）

以 Sv39 三级为例（概念）：

```text
root = satp.PPN << 12
读 PTE[2] @ root + VPN[2]*8
读 PTE[1] @ ...
读 PTE[0] @ ...   → leaf：PA = PPN<<12 | page_offset
填入 DTLB，再重试原 Load
```

**关键**：walker 读 PTE 用的是 **物理地址**，通常：

1. **不再经 MMU**（避免递归翻译）；  
2. 仍应过 **PMP**（页表页必须可读）；  
3. PTE 所在内存常标为 **cacheable**，则 walker 也可能打 D$（或专用 walker cache）。

所以波形上你会看到：**一次软件 `lw`，前面可能先冒出几次「读页表」的总线事务，再出现 cache line fill。**

### 4.4 TLB miss 时的端口占用

```text
Port-A: LSU 等翻译
Walker: 多次读 PTE ──► 共用 Port-C/D 或独立 walker master
填 TLB 后 Port-A 重试 → 得到 PA
```

**本节要点**：MMU 的产出是 **PA + 页属性**；TLB 只是加速。Walk 失败是 **page fault**，不是总线 DECERR。

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

常见故事：用户页表误映射到外设物理窗——**PMP 仍可拒绝**，这是 MCU 安全底线。

### 5.3 PMA（平台属性，实现定义）

标准未强制 PMA CSR，但实现几乎都有「物理区属性」：

| 属性 | 对 Load 的影响 |
|------|----------------|
| cacheable | 可进 D$ |
| Device / strongly-ordered | **绕过 D$**，直打总线 |
| 可执行 / 不可执行 | 主要约束取指；数据侧偶发实现检查 |

本文 SRAM = **cacheable Normal**；寄存器 = Device，走 uncached 口。

**本节要点**：PMP 失败 **不会** 发出「非法写 SRAM」的总线波；在核内就变成 trap。抓 bus 前先确认没卡在 PMP。

---

## 6. 第四层：D-Cache——Hit / Miss

RISC-V **基础 ISA 不规定 cache**；行为是实现定义。教学/MCU 常见模型：

| 项 | 本文默认 |
|----|----------|
| 结构 | 组相联或直接映射均可 |
| Line size | 16B / 32B / 64B（下文用 **32B** 举例） |
| 写策略 | Write-back + write-allocate（Store 节用） |
| 一致性 | 单核自洽即可 |

### 6.1 Hit

```text
索引/标签比较命中
→ 按 size/offset 抽出字
→ 经对齐/符号扩展
→ 返回 LSU
→ Port-C/D/E/F 全无事务
```

延迟：常 1～2 拍（流水设计而定）。

### 6.2 Miss

```text
1. 选 victim（若 WB 且 dirty → 先 write-back 总线写）
2. 按 line 对齐 PA，发 burst 读（Port-C）
3. 填 line，置 valid（及 tag）
4. 把原 Load 需要的字节回给 LSU
```

注意：**软件只 Load 了 4B，总线上往往是整行 32B**。这是「缓存」的代价与收益来源。

### 6.3 D$ 对上/对下端口

```text
对上（对 LSU/PMP）：
  req:  PA, size, load/store, (byte enables)
  resp: data / fault / retry

对下（对 Bus Master）：
  行填充读、写回写、（可选）uncached 直通旁路
```

**本节要点**：D$ 是「PA 空间上的加速器」；Hit 截断整条总线路径，Miss 把一次字访问放大成 line 事务。

---

## 7. 第五层：总线主口——AHB-Lite / AXI

Cache miss 或 uncached 访问时，Bus Master 把「读多少、从哪」翻译成协议信号。

### 7.1 AHB-Lite（MCU 极常见）

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

### 7.2 AXI4（应用核常见）

| 通道 | Line fill |
|------|-----------|
| AR | `ARADDR`=行首，`ARLEN`=beats-1，`ARSIZE`，`ARBURST=INCR`，`ARID` |
| R | 连续 `RLAST` 前多拍 `RDATA`；`RRESP=OKAY` |

Load 字访问若走 uncached：`ARLEN=0` 单拍即可。

### 7.3 主口上还要带的「身份」

| 信息 | 用途 |
|------|------|
| Secure / Privilege（`AxPROT` / `HPROT`） | 外设防火、调试 |
| Instruction vs Data | 区分取指/数据（本路径为 Data） |
| Master ID | Matrix 仲裁、错误上报 `HMASTER`/`AxID` |

> Exclusive/`ARLOCK`：**普通 `lw` 不置位**。原子路径见 [`01-spinlock-to-bus`](../05-bus-rtl/01-spinlock-to-bus.md)；RV 侧对应 `lr.w`/`sc.w`。

**本节要点**：主口协议是 D$ miss FSM 的「外语」；同一 PA 的 line fill，AHB 与 AXI 只是拍数与通道拆分不同。

---

## 8. 第六层：Bus Matrix——仲裁与译码

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

### 8.1 两件独立的事

| 功能 | 做什么 |
|------|--------|
| **Decode** | 用地址高位选 slave |
| **Arbiter** | 多 master 同时要同一 slave（或共享段）时谁先走 |

### 8.2 和「端口」的关系

- Port-D：核侧看到的「我发起的请求」  
- Port-E：SRAM 侧看到的「矩阵帮我转过来的请求」  
- 中间可能有 **upsizer/downsizer、AHB↔AXI 桥、流水寄存器**——延迟加拍，但地址/数据语义应保持。

### 8.3 单核 Load 时谁在抢

单核无 DMA 时，常见竞争来自：

- I$ miss 与 D$ miss  
- Page walk 与 D$ fill  
- Debug 口

所以：**即便「只有一个 hart」，矩阵仍可能不是旁路。**

**本节要点**：Matrix 决定「请求落到哪块 slave、何时获准」；不负责 cache 一致性语义。

---

## 9. 第七层：SRAM Controller / Slave

### 9.1 Slave 口（以 AHB 为例）

```text
HSEL && HTRANS!=IDLE && HREADY
  → 锁存地址/控制
  → 下一拍或当拍驱动 HRDATA（视时序）
  → HRESP=OKAY
```

错误地址（未实现洞）：`HRESP=ERROR` → 核侧通常打成 **bus error / access fault**（实现相关，需在 TRM 写死）。

### 9.2 Port-F：阵列口

```text
sram_addr  = PA 在窗口内的 offset（常去掉基址、按 word 对齐）
sram_cen   = chip enable
sram_wen   = 写使能（Load 时为读）
sram_ben   = byte enable（读时常全 1 或忽略）
sram_rdata = 同步读出
```

编译器/综合后的单口 SRAM IP：读延迟固定（如 1 cycle），controller 用 `HREADY` 反压对齐。

### 9.3 地址图（举例）

```text
0x2000_0000 – 0x2001_FFFF   128KB SRAM
Matrix 规则:  if (addr[31:17]==…) → SRAM_HSEL
```

软件 VA 经页表映到 `0x2000_xxxx`；总线只看见 PA。

**本节要点**：SRAM「从口」很薄——译码、时序、错误；**不理解** VA、TLB、cache line 语义。

---

## 10. 闭环时序 A：D-Cache Hit

假设：TLB hit、PMP pass、D$ hit。

```text
周期概念序（可合并拍数，示意因果）：

t0  LSU: VA ready
t1  DTLB: hit → PA
t2  PMP: ok
t3  D$: tag hit → data
t4  WB: rd ← data

总线 Port-D/E/F：无活动
```

抓波清单：核内握手有、**AXI/AHB 安静**。若此时总线在动，多半是取指或其它 master，不是这次 `lw`。

---

## 11. 闭环时序 B：D-Cache Miss + Line Fill

再假设：TLB hit、PMP pass、D$ miss、line clean（无写回）、SRAM 窗口正确。

```text
t0   LSU VA
t1   DTLB hit → PA
t2   PMP ok
t3   D$: miss → 启动 fill，stall 流水
t4   Master: 发起 burst 读（行首 PA）
t5…  Matrix grant → SRAM slave 连续返回 beats
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

## 12. 对照：Store 与 Fence

### 12.1 Store（同路径，方向相反）

| 阶段 | 与 Load 的差异 |
|------|----------------|
| TLB | 需要 **W=1**；可能置 Dirty |
| PMP | 查写权限 |
| D$ Hit + WB | 常只改 cache，**暂不上总线** |
| D$ Miss + write-allocate | 先 fill 再改（或 write-no-allocate 直写） |
| 写回 | victim dirty 时 Port-C 变 **写 burst** |

所以：**`sw` 成功 ≠ 总线上立刻看到写**——WB cache 下要等到替换或软件 flush。

### 12.2 `fence` / `fence.i`

| 指令 | 作用（直观） |
|------|----------------|
| `fence` | 约束本 hart 上某些内存操作的可见顺序 |
| `fence.i` | 同步 I$ 与后续取指（自修改代码） |

单核 + 强序实现里，`fence` 可能几乎是 nop；一旦有写缓冲、多口、或 SMP，fence 才「值钱」。验证时不要假设「所有 RV 核 fence 都刷总线」。

### 12.3 显式 cache 维护

若平台提供 `cbo.*` 或自定义 CMOs：DMA 前对 buffer 做 clean/invalidate——与 Linux DMA API 同构，细节见内核内存篇。

---

## 13. 从单核到 SMP：总框图与端口增量

单核路径（§1–12）对每个 hart **原样成立**。第二个 hart 并不是「再画一条到 SRAM 的线」这么简单——多数模块要 **复制**，少数 **共享**，再多问一句：**两套 D$ 里同一 PA 的数据如何保持同一真相？**

### 13.1 双 hart 总框图

```text
┌─────────────────────────────┐   ┌─────────────────────────────┐
│ Hart0                       │   │ Hart1                       │
│ LSU0 → DTLB0 → PMP0 → D$0   │   │ LSU1 → DTLB1 → PMP1 → D$1   │
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

### 13.2 端口怎么编号

| 代号 | SMP 下的变化 |
|------|----------------|
| Port-A0 / A1 | 每 hart 私有：LSU↔DTLB |
| Port-B0 / B1 | 每 hart 私有：PMP 后到本核 D$ |
| Port-C0 / C1 | 每 hart 私有：本核 D$↔本核 Master |
| **Port-D0 / D1** | **两条对等主口** 进 Matrix（Master ID 不同） |
| Port-E / F | **仍共享**；仲裁后串行或 interleaved 访问同一 SRAM |

另增（实现可选）：

| 代号 | 含义 |
|------|------|
| **Port-Snoop** | 一致性互联 → 各 D$：Invalidate / Probe / Data 转发 |
| **Port-IPI** | 核间软中断（RV 上常见 **MSIP** / ACLINT）；不经 SRAM，但配合 TLB 射齐 |

### 13.3 一句分水岭

> **单核**：D$ 命中 ⇒ 总线可完全安静。  
> **SMP 无 snoop**：Hart0 的命中 **不能** 代表 Hart1 看到的值；「安静的总线」甚至更危险——旧数据锁在对方 cache 里，SRAM 也被蒙在鼓里。

---

## 14. SMP 逐层对照：哪些复制、哪些共享

| 层 | 复制 or 共享 | Load 路径影响 |
|----|--------------|----------------|
| LSU / 流水线 | **每 hart 一份** | 两核可同时算 VA、同时 stall |
| DTLB + walker | **每 hart 一份** | 同 VA 可能映到同 PA；改页表要射齐 |
| `satp` | 每 hart CSR | 可跑不同地址空间；也可同 OS 同 ASID 策略 |
| PMP | 每 hart CSR | 通常镜像同一区域表；开机各写一遍 |
| L1 D$ | **每 hart 私有** | **一致性问题的根源** |
| Bus Master | 每 hart 一（或数）口 | Matrix 看到多 master |
| Bus Matrix | **共享** | 仲裁、优先级、outstanding |
| SRAM / 外设从口 | **共享** | 物理只有一份；最终写回落点 |
| PLIC / ACLINT | 共享外设 | 中断路由、IPI；不在 Load 数据通路上 |

**没有复制的「全局 D$」**——除非你显式做共享 L2；本文仍坚持「仅私有 L1」，把问题暴露清楚。

**本节要点**：SMP 相对单核，Load 的「垂直路径」不变；变的是 **横向多了一套对称路径 + 共享底座**，以及私有 D$ 之间要不要说话。

---

## 15. 无一致性硬件：三条必挂场景

假设：双 hart、**私有 write-back D$、无 snoop、共享区标成 cacheable**。这是教学上最有用的「先看会怎么坏」。

### 15.1 场景 A：写后读（对方仍 hit 旧行）

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

### 15.2 场景 B：双写丢失

```text
Hart0 / Hart1 都对同一字做 read-modify-write（无锁）
两边都在自己的 D$ 里改 → 后写回的覆盖先写回的 → 丢更新
```

即使用 `amoswap`/`amoadd`，若实现把 AMO 拆成「本地 cache 里非原子 RMW」且无保留集/总线锁，同样会坏——见 §17。

### 15.3 场景 C：DMA 与 CPU

```text
Hart0 填好 buffer（数据在 D$0）
DMA 从 SRAM 搬 → 读到旧内容
或 DMA 写入后 Hart0 仍 hit 旧 D$ 行
```

单核也有 DMA 一致性问题；SMP 只是再乘上「哪个 hart 的 cache」。

### 15.4 波形级直觉（无 snoop）

```text
Hart0:  SW 成功，Master0 安静（hit + WB）
Hart1:  LW miss → AR/R 读 SRAM → 得到旧值
SRAM:   仍是旧值，直到 Hart0 换出 dirty 行才 AW/W
```

抓波结论：**不能**用「总线上有没有写」判断「别的 hart 该不该看见新值」——在无协议时，真相分裂在多个 D$ 里。

**本节要点**：无硬件一致时，cacheable 共享 = 未定义行为；验证计划里应标为 **禁止**，或强制走 §16 的出路之一。

---

## 16. 三条工程出路：软件 / snoop / AMP

### 16.1 出路 1：软件管理一致性（MCU / 简单 SMP 常用）

| 手段 | 做法 |
|------|------|
| **私有分区** | 每 hart 堆栈/堆在不同 PA，禁止 cacheable 共享 |
| **共享区 uncached** | 页属性 / PMA：Device 或 Non-cacheable；Load/Store 直打 SRAM |
| **显式 CMO** | 写完 `cbo.clean`/`flush`，读前 `cbo.inval`；或平台 custom CMOs |
| **消息 + 所有权** | 只在「当前 owner hart」上 cache；移交前 flush |

端口影响：共享变量的 Load **经常走到 Port-D**（不再被 D$ 截断），延迟变高、可预测性变好。

### 16.2 出路 2：硬件 Cache Coherence（应用级 SMP）

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

### 16.3 出路 3：AMP（非对称）——机器人很常见

```text
Hart0: Linux，cacheable 大内存
Hart1: RTOS，紧耦合 TCM / 私有 SRAM 窗
共享：uncached mailbox + 门铃 IRQ（MSIP/外部中断）
```

这不是「残缺 SMP」，而是 **刻意不做硬件一致**，用软件契约换确定性。与 [`../03-rtos-bsp/01-freertos-realtime.md`](../03-rtos-bsp/01-freertos-realtime.md) 选型一致。

### 16.4 选型表

| 需求 | 更合适 |
|------|--------|
| 双核跑同一 Linux、共享堆 | 硬件 coherence + `lr`/`sc` |
| 双核 MCU、共享标志位 | uncached 原子区，或关 D$ 的 SRAM 窗 |
| 大核 Linux + 小核电机 | AMP + mailbox |
| 只有「偶发共享只读表」 | 只读副本 + 更新时广播 inval |

**本节要点**：一致性是 **产品选择**，不是 Load 路径上自动长出来的；框图里有没有 Port-Snoop，决定 §15 的场景是否合法。

---

## 17. 原子、Fence、TLB 射齐（RV）

### 17.1 `lr` / `sc` 与保留集（Reservation Set）

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

### 17.2 `fence` 在 SMP 上变「真」

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

### 17.3 TLB 射齐：`sfence.vma` + IPI

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

### 17.4 PMP 在 SMP

- 每核自己的 `pmpcfg`/`pmpaddr`；**不会自动广播**。  
- 运行时改 PMP（少见，多在 boot）要对齐所有 hart，否则同 PA 一核能访问、一核 access fault。

**本节要点**：数据一致靠 cache 协议或软件契约；**映射一致**靠 `sfence.vma`+IPI；**互斥**靠 `lr`/`sc`/AMO——三者层次不同，不要混成「一个锁搞定」。

---

## 18. SMP 闭环时序与抓波

### 18.1 双核同时 D$ miss 同一行（无 snoop）

```text
Hart0 miss fill ──AR──┐
                      ├─► Matrix 仲裁 ─► SRAM 返回两趟（或 interleaved beats）
Hart1 miss fill ──AR──┘
两边各自填入 D$0 / D$1，副本相同（若其间无写）
之后任一核 sw → 进入 §15 的分裂世界
```

### 18.2 有 snoop 时：Hart0 写、Hart1 再读（概念序）

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

### 18.3 建议探针（在单核清单上追加）

| 探针 | 看什么 |
|------|--------|
| Master0 vs Master1 | 同 PA 的 AR/AW 交错、ID |
| Matrix grant | 饿死、优先级是否符合实时核需求 |
| D$0/D$1 tag + 状态 | 无协议时是否双 Modified |
| Snoop 通道（若有） | Inval 是否先于对端再 hit |
| MSIP / IPI | TLB 射齐是否成对出现 |
| `sc` 成败 / AMO | 与他核写同址的因果 |

### 18.4 断言清单

**单核（保留）**

1. PMP fail ⇒ 无对应 Master 请求。  
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

许多 RV MCU：`satp=Bare`，只有 M-mode + PMP。

```text
LSU:  addr = rs1+imm     （此时即 PA）
  → PMP
  → D$ 或 TCM 直连
  → Bus / 本地 SRAM 口
```

端口仍在，只是 **砍掉 Port-A 的翻译与 walker**。教学时可先跑通这条，再加 Sv\*。

TCM/ILM/DLM：往往 **不经外部 Bus Matrix**，LSU 旁路到紧耦合 RAM——延迟固定，适合实时。可与「Cacheable SRAM 经矩阵」对照理解。

**双核 MCU**：每核私有 TCM + 共享 uncached SRAM 做 mailbox，是 AMP 的缩略版，常比硬上 snoop 更划算。

---

## 附录 B：异常对照表（Load 相关）

| 条件 | 典型 `mcause`（Load） | 卡在哪一层 |
|------|----------------------|------------|
| 非对齐（且不支持） | Load address misaligned | LSU |
| PTE 无效 / 无 R | Load page fault | MMU |
| PMP 拒绝 | Load access fault | PMP |
| 总线 ERROR | Load access fault（实现定义） | Slave / 桥 |
| ECC 不可纠 | 实现定义（NMI / fault） | SRAM / cache |

Trap 时硬件写 `mepc`/`mtval`/`mcause` 等，见 [`02-csr-trap.md`](02-csr-trap.md)。

---

## 小结

- 一次 `lw` 到 SRAM：LSU → **TLB/MMU** → **PMP** → **D$** → **Bus Master** → **Matrix** → **SRAM Slave** → 阵列；Hit 在 D$ 截断。  
- **每个 Port 职责不同**：翻译、权限、缓存、协议、仲裁、从口时序——抓波要对号入座。  
- Page walk 与 line fill **都会占用总线**；前者读 PTE，后者读数据行。  
- Store 在 write-back 下可长时间不上总线；顺序靠 `fence` / 平台 CMO。  
- **SMP**：垂直路径复制为 Port-\*0/\*1，Matrix/SRAM 共享；私有 D$ 迫使你在 **软件一致 / snoop / AMP** 里三选一。  
- **`lr`/`sc`、fence、`sfence.vma`+IPI** 分别解决互斥、顺序、映射射齐——与「普通 Load 数据通路」分层。

## 自测

1. 画出单核 `lw` 的模块图，并标出 Port-A…F 各传什么。  
2. D$ hit 与 miss 时，哪些端口有活动？  
3. DTLB miss 时总线在干什么？PTE 访问还经 MMU 吗？  
4. PMP 失败会不会在 SRAM 口看到一次 ERROR？为什么？  
5. 为何软件 Load 4B，miss 时总线可能读 32B？  
6. Write-back 下 `sw` 成功后，另一 master 读 SRAM 一定看到新值吗？  
7. 画出双 hart 框图：标出复制模块与共享模块；Port-D0/D1 与 Port-E 各是什么？  
8. 无 snoop 时，Hart0 `sw` 后 Hart1 `lw` 为何可能读到旧值？总线上一定有写吗？  
9. 软件一致 / 硬件 snoop / AMP 各适合什么产品？共享区 uncached 时 Load 还走 D$ 吗？  
10. `lr`/`sc`、`fence`、`sfence.vma`+IPI 各解决哪一类问题？  
11. 无 MMU 的 MCU 路径去掉了哪一段？双核 + 私有 TCM + 共享 mailbox 属于哪条出路？

---

*`04-riscv-core` · Load 路径：Core → SRAM（单核 → SMP）*
