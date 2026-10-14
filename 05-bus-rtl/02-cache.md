# Cache：属性、总线边界与一致性（专题）

> **系列**：`05-bus-rtl`  
> **前置**：[`03-axi-ahb.md`](03-axi-ahb.md)（`AxCACHE`/`HPROT` 编码表）· [`01-spinlock-to-bus.md`](01-spinlock-to-bus.md)（Exclusive 与 cacheable 见该文 §11.1）  
> **相关**：[`../04-riscv-core/03-memory-bus.md`](../04-riscv-core/03-memory-bus.md)（Load → D-Cache → 总线）；[`../01-kernel/02-memory.md`](../01-kernel/02-memory.md)（软件 DMA / 一致性）

本篇是 **Cache 相关内容的收纳文**：属性如何上总线、1K/4K 边界谱系已写；**一致性（MESI/MOESI、ACE/CHI、DMA 与 cache 维护等）后续追加同一文件**，不另拆「attrs」专名。

**当前可读部分应能**：
- 分清 Outstanding / Cacheable / Bufferable
- 说出 MMU 属性与 `AxCACHE` 的关系，以及谁给这笔事务打 bit
- 解释 AXI 4KB、AHB 1KB、MMU/文件系统 4KB 如何一脉相承（而非巧合）
- 区分「descriptor 不跨 4K」与「AXI burst 不跨 4K」

---
## 目录

**Part A — 属性与边界（已写）**

1. [三个正交概念](#1-三个正交概念)
2. [Bufferable：缓冲许可落在哪](#2-bufferable缓冲许可落在哪)
3. [MMU 属性与 AXI AxCACHE：是一回事吗](#3-mmu-属性与-axi-axcache是一回事吗)
4. [谁决定这笔事务的 AxCACHE](#4-谁决定这笔事务的-axcache)
5. [1KB / 4KB 边界从哪来](#5-1kb--4kb-边界从哪来)
6. [和 Exclusive / DMA 描述符的交界](#6-和-exclusive--dma-描述符的交界)
7. [小结（Part A）](#7-小结part-a)
8. [自测（Part A）](#8-自测part-a)

**Part B — 一致性（待写）**

9. ~~多核 cache 一致性（MESI/MOESI 等）~~  
10. ~~ACE/CHI 与软件维护（clean/invalidate、DMA）~~  

---

---

## 1. 三个正交概念

| 概念 | 层级 | 含义 | 不是什么 |
|------|------|------|----------|
| **Outstanding** | 总线/事务 | 已发出、尚未完整完成（读等 `RLAST`，写等 `B`）的笔数 | 不是「进了写缓冲」 |
| **Cacheable** | 内存属性 | 允许进 **cache**（可 allocate） | 不是 outstanding |
| **Bufferable** | 内存属性 | **允许**写 posted / 进写缓冲再往后推 | 不是「缓冲只在 bus 上」 |

```
Outstanding：时间轴上「已发未完」
Cacheable：  数据能不能放进 Cache
Bufferable： 写允不允许先缓冲、晚一点再到终点
```

可组合举例：

- Device、non-bufferable 的写 → 仍可与其它交易形成多笔 outstanding。  
- Cacheable + hit → 很多 load/store **根本不出 AXI**，谈不上这趟 outstanding。  
- Non-cacheable + bufferable → 不进 D$，但仍可能进 core/桥的写缓冲。

---

## 2. Bufferable：缓冲许可落在哪

`bufferable` 是**许可**，不规定缓冲只画在某一块 RTL。路径上谁实现了写缓冲、谁看到属性允许，谁就可以 posted。

```
CPU store
  → [Core store buffer]      ← 最常见；「指令跑完 ≠ 数据到设备」
  → [可选 Cache]             ← 叠 cacheable / write-back 时
  → [互联 / Bridge 写队列]   ← SoC 常见
  → Slave / DRAM 控制器写队列
```

| 抓现象 | 先查哪里 |
|--------|----------|
| 软件刚 store，外设没反应 | **core store buffer** → 屏障/`DSB`/属性是否该 Device |
| 总线上已 `BRESP=OKAY`，设备仍未更新 | bufferable 时 **不保证**已到最终 slave；继续查桥与控制器 |
| 波形上 `AWCACHE` bufferable 位 | 确认 master 意图；再看下游是否真缓冲 |

`non-bufferable`（及更严的 Device 档）：要求路径上别用「提前完成、数据还在半路」破坏程序员可见序。

---

## 3. MMU 属性与 AXI AxCACHE：是一回事吗？

**同一语义族，不是同一个 bit。**

| 位置 | 形态 | 作用 |
|------|------|------|
| **MMU / 页表** | 旧式 PTE 的 B/C/TEX；ARMv8 为 **MAIR + AttrIndx**（Device 四档 / Normal + Inner·Outer） | 按 **VA** 定策略（源） |
| **AXI** | `ARCACHE[3:0]` / `AWCACHE[3:0]`（Bufferable、Modifiable、RA/WA Allocate 等） | 按**这一笔事务**告诉互联/下游（编码副本） |

```
软件/页表:  VA → 属性（含能否 cache / 缓冲 / 序）
              ↓
CPU 微架构:  store buffer / cache 按属性行事
              ↓
总线:        AxCACHE = 该笔事务属性摘要
```

注意：

- ARMv7 的 B/C/TEX → `AxCACHE` 映射较直接；**ARMv8 更绕**，不是简单 `MMU.B == AWCACHE[0]`。  
- 互联对 **modifiable** 事务可能改 cache 属性，但不得破坏架构可见性。  
- 无 MMU 的 master **没有**「页表 → AxCACHE」这条链（见下一节）。

---

## 4. 谁决定这笔事务的 AxCACHE

属性是**事务规则**；配置点在 **发起这笔访问的 Master**，不是 DDR 控制器上的「全局一个 bufferable 开关」。

| Master | AxCACHE 怎么来 | 软件/硬件落点 |
|--------|----------------|----------------|
| **CPU（有 MMU/MPU）** | 访存时查页表/MAIR，**硬件译成** `AxCACHE` | 页属性；Linux：`ioremap` / `dma_*` / 内存类型；DT 描述内存与设备区 |
| **DMA / 无 MMU Master** | **寄存器可编程**或通道默认值 | IP 配置寄存器（如部分 SoC 的 `GPR_ARCACHE_*`、CoreSight `ARCACHE` 域）；驱动 init 写 |
| **从不改的简单口** | RTL **tie-off** 常量 | 综合/IP 集成绑死（如常打 Non-cacheable Bufferable） |

OS/驱动还会间接影响：

- `dma_alloc_coherent` 等 → 主要改 **CPU 侧**页属性（及一致性策略），从而影响 CPU 访存时的 `AxCACHE`。  
- DT `dma-coherent`、内核 DMA 默认策略 → 决定要不要给 DMA 口另配 cache 属性、是否依赖硬件一致性。  
- **CPU 页表对了、DMA 口 `AxCACHE` 乱配** → 仍可能错；两边策略要对齐。

---

## 5. 1KB / 4KB 边界从哪来

### 5.1 总线在防什么

一笔 burst 一旦发出，地址按 INCR/WRAP 递增。中途若跨「硬边界」：

- 可能换到**另一个 slave**（译码变了）  
- 可能换到**另一页**（属性/物理映射/权限变了）  

协议因此要求：**单笔 burst/事务不得跨界**；要跨就拆成多笔。

| 协议 | 单笔不得跨越 | 主要对齐什么 |
|------|--------------|--------------|
| **AHB** | **1KB** | 简单矩阵里 slave 译码窗口折中；跨界要新 burst，第一拍 `NONSEQ` |
| **AXI** | **4KB** | 常见 **MMU 小页**；一笔内假定页属性不变 |

AHB 的 1KB **不是**「slave 最大 1KB」——设备可以很大，只是 master 的连续 SEQ 不能跨中间的 1KB 线。  
AXI 的 4KB 是「**不许越过** 4K 线」，不是「每笔必须打满 4K」。

### 5.2 为何和 ext4 / Linux 页都是 4KB——相关联，非巧合反因果

```
MMU 小页收敛为 ≈4KB（页表数学 + 碎片/TLB 折中；386 等定调）
        │
        ├─→ OS PAGE_SIZE、page cache
        │         └─→ 文件系统块常 4KB（ext4 可配，4K 最省事）
        │
        └─→ AXI：单笔事务不跨 4KB（对齐页属性边界）
```

- **不是**「因为 ext4 是 4K，所以 AXI 定 4K」。  
- **是**大家顺着 **同一套 4KB 页粒度**：文件系统吃 OS 页；AXI 吃 MMU 页。

### 5.3 MMU 的 4KB 又从哪来（极简）

32 位两级页表的自洽解：页 = **2¹²** → 偏移 12 bit；PTE 4B × 1024 = 正好一页 → 索引 10+10 bit。再叠加「页太碎页表爆炸 / 页太大碎片与保护变粗」的折中，工业界锁在 **4KB 小页**。大页（2MB/1GB）是后来补 TLB 的；小页默认仍多是 4KB。

---

## 6. 和 Exclusive / DMA 描述符的交界

### 6.1 Exclusive 与 cacheable

- Local Monitor 记的是 **地址预约范围**，不等于「必须是 MESI line」。  
- **不必** cacheable 才能做 Exclusive；non-cacheable 靠 **Global Monitor**。  
- 多核 spinlock 实务上仍常用 **Normal Shareable Cacheable**，跨核靠一致性清预约。  

细节见 [`01-spinlock-to-bus.md` §11.1](01-spinlock-to-bus.md#111-local-monitor-必须-cacheable--等于-mesi-吗)。

### 6.2 DMA：descriptor 不跨 4K ≠ 数据 buffer 条款

以 Andes DMAC110 一类表述为例：

> Descriptor 存放地址 8 字节对齐；**每个 descriptor 不得跨 4KB 边界。**

| 对象 | 这句管不管 |
|------|------------|
| Chain 里每一颗 **descriptor 结构体** | ✅ |
| Descriptor 指向的 **payload buffer** | ❌（另查 transfer/buffer 章节） |
| AXI「单笔 burst 不跨 4K」 | 根因同类，条文不是同一句 |

Chain 模式：链表上**每一颗** descriptor 都要满足；不是「整条 chain 合成一个 4K 限制」。硬件一次（或少数次）把整颗描述符读回时，天然不想跨 4K——与 AXI 规则同族。

---

## 7. 小结（Part A）

- **Outstanding / Cacheable / Bufferable** 正交：在飞笔数 ≠ 进 cache ≠ 写可缓冲。  
- **Bufferable** = 许可；缓冲可在 core / bridge / 控制器多层。  
- **MMU 属性 →（CPU）→ AxCACHE**：语义同源，位不同源；DMA 用寄存器或 tie-off。  
- **AHB 1KB / AXI 4KB**：防 burst 中途换译码或换页；4KB 与 MMU/OS/文件系统同祖，非 ext4 规定 AXI。  
- **Descriptor 4K** 管描述符摆放；勿与数据 buffer、与协议 burst 边界混为一谈。

## 8. 自测（Part A）

1. 一笔 Device non-bufferable 写，能否仍有多个 outstanding？  
2. `BRESP=OKAY` 是否表示数据已写入最终外设寄存器？  
3. CPU 与 DMA 访问同一 Normal 缓冲时，`AxCACHE` 分别可能从哪配置？配错一侧会怎样？  
4. 为何 AXI 选 4KB、AHB 选 1KB？和 MMU 页、ext4 块大小如何串联？  
5. DMAC 手册写「descriptor 不跨 4K」时，payload 跨 4K 是否一定非法？

---

## Part B — 一致性（待写）

> 占位：多核 MESI/MOESI、ACE/CHI、与 Exclusive Monitor 的关系、DMA 下 clean/invalidate / `dma-coherent` 等，写完后挂到上方目录 §9 / §10。

---

*`05-bus-rtl` · `02-cache` · Cache 专题*
