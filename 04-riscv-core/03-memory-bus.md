# RISC-V：一次 Load 从 Core 到 SRAM（Cache / MMU / PMP / 总线）

> **系列**：`04-riscv-core`  
> **前置**：[`02-csr-trap.md`](02-csr-trap.md)（特权级与 trap CSR）；流水线形态见 [`01-pipeline.md`](01-pipeline.md)  
> **相关**：[`../05-bus-rtl/01-spinlock-to-bus.md`](../05-bus-rtl/01-spinlock-to-bus.md)（原子/Exclusive）；软件侧页表/DMA 见 [`../01-kernel/02-memory.md`](../01-kernel/02-memory.md)

上一篇总线专文把 **spinlock** 讲到 AXI Exclusive / AHB `HLOCK`。本文换一条更日常的路径：  
**`lw` 从片上 SRAM 读一个字**——从 LSU 算地址，经 TLB/MMU、PMP、一级 D-cache、总线主口、Bus Matrix，再到 SRAM slave。先 **单核闭环**，再扩到 **SMP** 多出了什么。全文以 **RISC-V** 为本，不展开 ARM。

**读完应能**：
- 画出单核 `lw → SRAM` 上每一级模块与端口
- 分清 Hit / Miss 两条时序，以及 page walk 为何也占总线
- 说明 VA→PA、PMP、cacheable 属性各自挡在哪一层
- 指出 SMP 相对单核新增的一致性与原子路径落点

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
13. [从单核到 SMP](#13-从单核到-smp)
14. [抓波与断言清单](#14-抓波与断言清单)
15. [附录 A：裸机无 MMU 的最短路径](#附录-a裸机无-mmu-的最短路径)
16. [附录 B：异常对照表（Load 相关）](#附录-b异常对照表load-相关)
17. [小结](#小结)
18. [自测](#自测)

---

## 1. 场景假设与总框图

### 1.1 本文固定模型（先钉死，避免混谈）

| 项 | 假设 |
|----|------|
| ISA | RV32/RV64；指令 `lw rd, imm(rs1)` |
| 核 | **单 hart**（§13 再扩 SMP） |
| Cache | **仅一级 D-cache**（无 L2）；I-cache 存在但不走本路径 |
| 目标 | 片上 **SRAM**（Normal、可 cache；非 Device MMIO） |
| 地址翻译 | 开 **Sv32 / Sv39**（§附录 A 给无 MMU 捷径） |
| 保护 | **PMP** 开（至少覆盖 SRAM / 外设分区） |
| 总线 | 数据侧主口用 **AHB-Lite** 或 **AXI4**（两套都写清信号对应） |
| 一致性 | 单核：无 snoop；SMP：先讲需求，细节链到总线专文 |

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

## 13. 从单核到 SMP

单核路径闭环后，第二个 hart 加入时，**同一套 Port-A…F 仍然存在**，但多了三类问题。

### 13.1 多一套「对称」主口

```text
Hart0: LSU→…→D$0→Master0 ─┐
                           ├─► Matrix ─► SRAM
Hart1: LSU→…→D$1→Master1 ─┘
```

两套 D$ **默认不共享**。若无一致性硬件：

| 现象 | 原因 |
|------|------|
| Hart0 写了，Hart1 仍读旧值 | Hart1 D$ 仍持有旧 line |
| 两核写同一行 | 无所有权协议时数据损坏 |

### 13.2 一致性方案（选型级）

| 方案 | 适用 |
|------|------|
| **软件管理** | 划核私有区；共享区 uncached 或显式 flush |
| **硬件 snoop / directory** | 应用级 SMP Linux；总线侧 ACE/CHI 等 |
| **AMP + 邮箱/共享 uncached** | Linux + RTOS 分核（机器人常见） |

本文不展开 snoop 状态机；原子操作与 Monitor 见 spinlock 专文。RV 上多核自旋常落在 **`lr`/`sc` + 保留集**，语义类似 exclusive。

### 13.3 TLB / PMP 在 SMP 上

| 结构 | 注意 |
|------|------|
| TLB | **每核一份**；改页表后要 IPI + `sfence.vma` |
| PMP | 每核 CSR；常镜像同一配置，开机各自写 |
| `satp` | 每核可不同（跑不同进程时） |

### 13.4 Page walk / DMA 再抢矩阵

SMP + DMA 时，Port-D 竞争明显上升：line fill、PTE walk、DMA、外设同时上场。定带宽与优先级（固定优先 / round-robin / QoS）是芯片集成题，不是 ISA 题。

**本节要点**：SMP 不是「把单核框图复制粘贴」就完——**私有 D$ 强制你回答一致性**；总线从「偶然安静」变成「常态仲裁」。

---

## 14. 抓波与断言清单

### 14.1 建议探针

| 探针 | 看什么 |
|------|--------|
| LSU req/resp | VA、stall 原因编码（tlb/pmp/cache/bus） |
| DTLB | hit/miss、walk 次数 |
| PMP | fail 与区域号 |
| D$ | hit/miss、fill 起止、dirty WB |
| Master AR/R 或 HADDR 流 | 是否行对齐、beat 数是否=line/总线宽 |
| Matrix grant | 谁在打 SRAM |
| SRAM HSEL/rdata | 最终数据是否回到 fill buffer |

### 14.2 断言（单核）

1. PMP fail ⇒ 当拍/下一拍 **无** 对该 PA 的 Master 请求。  
2. D$ hit 的 Load ⇒ 无对应 fill 的 bus 读。  
3. D$ miss fill ⇒ `ARADDR`/`HADDR` 行对齐；beat 数匹配 line size。  
4. Walker 读 PTE 的地址落在页表页，且 PMP 允许。  
5. Slave `ERROR` ⇒ 核侧进入可观测的 fault 路径（与 `mcause` 一致）。

### 14.3 断言（SMP 起步）

1. 同一 cacheable 行，两核同时当 owner 写 —— 无协议则必须在验证计划里标为 **禁止场景** 或期望 fail。  
2. `sfence.vma` 后，本核不得继续用旧 VPN→PPN 映射（除非再 walk）。

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
- SMP 复制主口与 D$ 后，核心新增题是 **一致性与 TLB 射齐**，不是再讲一遍 AHB 信号。

## 自测

1. 画出单核 `lw` 的模块图，并标出 Port-A…F 各传什么。  
2. D$ hit 与 miss 时，哪些端口有活动？  
3. DTLB miss 时总线在干什么？PTE 访问还经 MMU 吗？  
4. PMP 失败会不会在 SRAM 口看到一次 ERROR？为什么？  
5. 为何软件 Load 4B，miss 时总线可能读 32B？  
6. Write-back 下 `sw` 成功后，另一 master 读 SRAM 一定看到新值吗？  
7. 从单核到双核，框图最少要加什么？一致性有哪几类做法？  
8. 无 MMU 的 MCU 路径去掉了哪一段？TCM 与经 Matrix 的 SRAM 差别？

---

*`04-riscv-core` · Load 路径：Core → SRAM*
