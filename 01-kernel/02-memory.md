# Linux 内存管理：伙伴系统、页表与 DMA 一致性

> **系列**：`01-kernel`  
> **前置**：[`01-scheduling.md`](01-scheduling.md) 可先读（非强制）  
> **相关**：[`03-crash-debug.md`](03-crash-debug.md) · [`../02-arch-boot/01-exception.md`](../02-arch-boot/01-exception.md)

围绕嵌入式/驱动最常踩的坑：分配器、页表属性、DMA zone / CMA，以及 cache 与 Device 内存配错时怎么爆。

**读完应能**：
- 说清 buddy / slab 各自解决什么
- 解释 DMA 与 cache 维护为何必须成对
- 对照 Normal vs Device 属性

---
## 内存管理

## 2.1 虚拟内存概览

```
用户态视角                    内核视角
┌──────────────┐             ┌──────────────┐
│  用户空间     │  EL0       │              │
│  (每个进程独立)│             │              │
├──────────────┤             │  内核空间     │
│              │  EL1       │  (所有进程共享)│
│  内核空间     │             │              │
│  (不可直接访问)│             │              │
└──────────────┘             └──────────────┘
       │                            │
       ▼                            ▼
   MMU 页表翻译                  直接映射区
       │                       (linear mapping)
       ▼                            │
   物理内存 (PA) ◀──────────────────┘
```

### AArch64 地址空间布局

| 区域 | 地址范围 | 用途 |
|------|----------|------|
| 用户空间 | 0x0000_0000_0000 – 0x0000_FFFF_FFFF_FFFF | 每个进程独立 |
| 内核空间 | 0xFFFF_0000_0000_0000 – 0xFFFF_FFFF_FFFF_FFFF | 共享 |
| TTBR0_EL1 | 用户页表基址 | EL0/EL1 用户态 |
| TTBR1_EL1 | 内核页表基址 | EL1 内核态 |

---

## 2.2 页表与 TLB

### 四级页表（AArch64，4KB page）

```
VA (Virtual Address)
  │
  ├── PGD (Page Global Directory)     ← TTBR 指向
  │     ├── PUD (Page Upper Directory)
  │     │     ├── PMD (Page Middle Directory)
  │     │     │     ├── PTE (Page Table Entry)
  │     │     │     │     └── PA (Physical Address)
```

### TLB 匹配条件（见 `02-arch-boot` 异常/MMU 笔记）

A72 中 TLB entry 匹配需满足：

1. VA page number == TLB entry 中的 VA page number
2. **Memory space identifier** 匹配（Secure/NS、EL3/EL2/EL1/EL0）
3. 若 non-Global：ASID == TLB entry 中的 ASID
4. VMID == TLB entry 中的 VMID（虚拟化）

**TrustZone 切换**：Secure/Non-secure 是不同的 memory space，TLB entry 带 NS 标签。

### 页表属性（PTE flags）

| 位 | 含义 |
|----|------|
| Valid | 有效 |
| AP | Access Permission（读/写/执行） |
| NS | Non-secure |
| nG | not Global（ASID 相关） |
| AF | Access Flag |
| SH | Shareability（Inner/Outer/Non） |
| MT[2:0] | Memory Type（Normal/Device） |

---

## 2.3 伙伴系统（Buddy System）

### 作用

管理**物理页帧**（page frame），内核 kmalloc 大内存、进程 mmap 都从这里分配。

### 原理

```
物理内存按 2^n 页大小分 free list：

Order 0:  1 页 (4KB)    free list
Order 1:  2 页 (8KB)    free list
Order 2:  4 页 (16KB)   free list
...
Order 10: 1024 页 (4MB) free list

分配 3 页：
  → 从 Order 2 (4页) 拆
  → 用 3 页，剩 1 页回 Order 0

释放时：
  → 尝试与 buddy 合并
  → 合并成功则升到更高 order
```

### 外部碎片 vs 内部碎片

| 类型 | 原因 | 缓解 |
|------|------|------|
| **外部碎片** | 空闲页不连续 | 伙伴合并、compaction |
| **内部碎片** | 分配大于请求 | slab 分配器 |

---

## 2.4 Slab 分配器

### 作用

分配**小于一页**的小对象（task_struct、inode、dentry 等），减少内部碎片。

```
                    ┌─────────────┐
                    │  Slab Cache  │  "task_struct" cache
                    │  (对象类型)   │
                    └──────┬──────┘
                           │
              ┌────────────┼────────────┐
              ▼            ▼            ▼
         ┌────────┐  ┌────────┐  ┌────────┐
         │  Slab  │  │  Slab  │  │  Slab  │  每个 slab = 若干页
         │(活跃)   │  │(半满)   │  │(空闲)   │
         └────────┘  └────────┘  └────────┘
              │
              ▼
         [obj][obj][obj]...  每个 obj = 一个 task_struct
```

### 常用 API

```c
/* 内核小对象分配 */
struct task_struct *tsk = kmem_cache_alloc(task_struct_cachep, GFP_KERNEL);

/* 通用 kmalloc */
void *buf = kmalloc(256, GFP_KERNEL);

/* 释放 */
kmem_cache_free(task_struct_cachep, tsk);
kfree(buf);
```

### GFP 标志

| 标志 | 含义 |
|------|------|
| GFP_KERNEL | 正常分配，可能睡眠 |
| GFP_ATOMIC | 原子上下文（中断/持锁），不睡眠 |
| GFP_DMA | 从 DMA zone 分配 |
| __GFP_ZERO | 分配后清零 |

---

## 2.5 内存 Zone

物理内存按用途分 zone：

| Zone | 用途 |
|------|------|
| **ZONE_DMA** | DMA 可访问的低地址（< 16MB 或 < 4GB） |
| **ZONE_DMA32** | 32-bit 设备 DMA 可访问 |
| **ZONE_NORMAL** | 正常内核/用户内存 |
| **ZONE_MOVABLE** | 可迁移页（用于 compaction） |
| **ZONE_HIGHMEM** | 32-bit 内核访问高端内存（AArch64 无此问题） |

### CMA（Contiguous Memory Allocator）

- 预留一块**物理连续**内存给 DMA 设备
- 平时当 Normal page 用，设备需要时回收
- 典型：camera、GPU、NPU 驱动

```bash
# 启动参数预留 CMA
cma=256M
```

---

## 2.6 用户态内存分配

```
用户 malloc()
  │
  ▼
glibc ptmalloc/mmap
  │
  ├── brk（小分配，堆扩展）
  └── mmap（大分配，>128KB，直接映射）
        │
        ▼
      系统调用 sys_mmap()
        │
        ▼
      do_mmap() → 建立 VMA（Virtual Memory Area）
        │
        ▼
      缺页异常时 → 伙伴系统分配物理页 → 建立 PTE
```

### VMA（Virtual Memory Area）

```c
struct vm_area_struct {
    unsigned long vm_start;   /* 起始 VA */
    unsigned long vm_end;     /* 结束 VA */
    pgprot_t vm_page_prot;    /* 页属性（cacheable/device） */
    unsigned long vm_flags;   /* VM_READ/WRITE/EXEC/IO */
    struct file *vm_file;     /* 映射的文件（若有） */
    ...
};
```

---

## 2.7 DMA 与 Cache 一致性（实战重点）

### 问题本质

```
CPU 有 Cache，DMA 直接访问内存（不经过 Cache）

场景 1：CPU 写 buffer → 数据在 Cache → DMA 读内存 → 读到旧数据
场景 2：DMA 写内存 → CPU 读 buffer → Cache hit → 读到旧数据
```

### 三种解决方案

| 方案 | 做法 | 适用 |
|------|------|------|
| **1. Non-cacheable 映射** | MMU 配 Device 或 Normal Non-cacheable | 简单，性能略差 |
| **2. 软件 flush/invalidate** | DMA 前 flush，DMA 后 invalidate | cacheable buffer，最常用 |
| **3. 硬件 coherent** | CCI/ACE 总线 snoop | 硬件自动保持一致 |

### 软件方案详细流程

```
CPU 写 DMA TX buffer:
  1. 写数据到 buffer（在 cache 中）
  2. dma_map_single(dev, buf, len, DMA_TO_DEVICE)
     → 内部调用 dma_flush_range() 或 arch_sync_dma_for_device()
     → Cache Clean（写回内存）
  3. 启动 DMA
  4. DMA 完成中断
  5. dma_unmap_single()

CPU 读 DMA RX buffer:
  1. 启动 DMA
  2. DMA 完成中断
  3. dma_map_single(dev, buf, len, DMA_FROM_DEVICE)
     → Cache Invalidate（丢弃 cache，从内存重新读）
  4. CPU 读 buffer
  5. dma_unmap_single()
```

### 三个实战案例

#### 案例 1：HSM MMU 配错

```
问题：HSM 寄存器 MMU 配成 Normal（cacheable）而非 Device
现象：写寄存器后硬件没反应，或读到旧值
原因：CPU 写到 D-Cache，没到达硬件
解决：改为 Device-nGnRnE，或写后 dsb + flush
```

#### 案例 2：dFlash program + dcache

```
问题：dFlash 通过 APB cmd 编程，但 dcache 已 hit 旧数据
现象：program 后再读，仍是旧内容
原因：CPU 从 cache 读，没从 flash 重新读
解决：program 前/后对相关区域 cache invalidate
```

#### 案例 3：DMA channel enable 前 ctl 读为 0

```
问题：dma_channel_config 设 ctl 正确，enable 时读 ctl 变 0
原因：写 ctl 在 cache 中，enable 读从 cache/错误路径
解决：Device 映射 or 写后 dsb + flush
```

### Linux DMA API

```c
/* 分配 coherent（一致）内存，硬件 snoop 或 uncached */
void *dma_alloc_coherent(struct device *dev, size_t size,
                         dma_addr_t *dma_handle, gfp_t flag);

/* 映射已有 buffer */
dma_addr_t dma_map_single(struct device *dev, void *cpu_addr,
                          size_t size, enum dma_data_direction dir);
/* dir: DMA_TO_DEVICE / DMA_FROM_DEVICE / DMA_BIDIRECTIONAL */

/* 解除映射 */
void dma_unmap_single(struct device *dev, dma_addr_t dma_addr,
                      size_t size, enum dma_data_direction dir);

/* 同步（cache flush/invalidate） */
void dma_sync_single_for_cpu(struct device *dev, dma_addr_t dma_handle,
                             size_t size, enum dma_data_direction dir);
void dma_sync_single_for_device(struct device *dev, dma_addr_t dma_handle,
                                size_t size, enum dma_data_direction dir);
```

### 与 RISC-V 对照

| ARM Linux | RISC-V MCU |
|-----------|------------|
| dma_map_single + flush/invalidate | 手动 flush/invalidate 或 PMA Non-cacheable |
| Device-nGnRnE 页属性 | PMA I/O 类型 |
| CCI hardware coherent | 通常无，纯软件 |

---

## 2.8 OOM（Out Of Memory）

```
内存不足
  │
  ▼
kswapd 唤醒，尝试回收
  ├── 回收 page cache（文件缓存）
  ├── 回收 slab
  └── swap out（若有 swap）
  │
  ▼
仍不够 → 触发 OOM Killer
  │
  ▼
oom_score 最高的进程被 SIGKILL
```

### oom_score 计算因素

- 内存占用量
- 运行时间（短命进程优先杀）
- 是否 root
- 是否硬件 bound
- `/proc/<pid>/oom_score_adj` 用户可调（-1000 到 1000）

---

---

## 小结

- buddy 解决大块连续页；slab/slub 解决小对象高频分配。
- DMA 前后的 cache 维护必须成对：设备读内存前 flush，设备写内存后 invalidate。
- MMU/PMA 上 Normal 与 Device 属性不同，配错会出现「软件看着对、总线上行为怪」。
- CMA / DMA zone 是给连续/低地址缓冲留的工程手段，和驱动申请方式绑定。

## 自测

1. 伙伴系统如何分配与释放？它主要解决什么碎片问题？
2. slab 与 buddy 如何分工？
3. DMA 前为什么要 flush？DMA 后为什么要 invalidate？
4. Device memory 与 Normal memory 在属性上差在哪里？
5. 用一两个案例说明 cache 维护失败时的现场表现。

---

*`01-kernel` · 内存管理*
