# RISC-V Core：特权级、CSR 与 Trap

> **系列**：`04-riscv-core`  
> **前置**：可先扫一眼 [`../02-arch-boot/01-exception.md`](../02-arch-boot/01-exception.md) 里的 ARM↔RV 总对照表（迁移用）；本文以 **RISC-V 为本**，面向「这颗 core 要真正做出什么」。  
> **相关**：[`01-pipeline.md`](01-pipeline.md) · [`03-memory-bus.md`](03-memory-bus.md) · [`../02-arch-boot/02-gic.md`](../02-arch-boot/02-gic.md)

造一颗 RV core，异常与特权模型是骨架：谁在什么 mode 跑、trap 怎么进、CSR 记什么、中断如何挂上来。ARM 侧对照见架构系列；这里只写 **M-mode 最小可实现集**，并标明以后加 U/PMP 时要扩什么。

**读完应能**：
- 画出 U/S/M 与典型裸机/OS 落点
- 列出 trap 进出时硬件改写的 CSR
- 说明 `mtvec` Direct/Vectored 与 `mcause` 分发

---

## 1. 特权级：U / S / M

标准用户可见三级（虚拟化 H 扩展另说）：

```text
M-mode   机器态：BootROM、OpenSBI、多数 MCU 裸机全程
S-mode   监督态：Linux / 类 Unix（需委托 trap）
U-mode   用户态：应用（靠 PMP/MMU 隔离）
```

| Mode | 典型软件 | V0 简易 GTM / MCU |
|------|----------|-------------------|
| M | Boot、固件、RTOS 内核 | **建议 V0 只实现 M** |
| S | Linux | 以后再开 |
| U | App | 做进程隔离时再开 + PMP |

和 ARM 的粗对照（细节表见架构篇）：

| RISC-V | 约略对应 ARM |
|--------|----------------|
| U | EL0 |
| S | EL1 |
| M | EL3 一侧的「最高特权 / 固件」，但 **没有 TrustZone 双 world** |
| — | EL2 / FIQ / Secure 中断分组：RV 无同款标准物 |

安全隔离在 MCU 上常见靠 **PMP**（物理地址 R/W/X），而不是 TZASC。

---

## 2. Trap 入口：`mtvec`

RV **没有** ARM 那种 16 格向量表；M-mode 只有一个 trap 入口寄存器 `mtvec`：

```text
mtvec[XLEN-1:2] = base（对齐）
mtvec[1:0]      = mode
  00 Direct   → 所有 trap 跳到 base
  01 Vectored → 异常仍跳 base；中断跳 base + 4×cause
```

| 项 | Direct | Vectored |
|----|--------|----------|
| 实现复杂度 | 低：一个 handler 里读 `mcause` 分发 | 稍高：中断可直跳 |
| V0 建议 | **Direct 足够** | 中断延迟敏感再开 |

设置：`csrw mtvec, handler_addr`（注意对齐与 mode 位）。

S-mode 对应 `stvec`，规则同构；V0 可不实现。

---

## 3. Trap 时硬件改写哪些 CSR

发生 trap（同步异常或中断）进入 M-mode 时，硬件大致做：

1. `mepc ←` 当前 PC（精确到哪条指令取决于异常类型）
2. `mcause ←` 原因（见下节）
3. `mtval ←` 附加信息（fault 地址、坏指令等；有的原因可为 0）
4. `mstatus.MPIE ← mstatus.MIE`，然后 **`MIE ← 0`**（关全局中断）
5. `mstatus.MPP ←` 来源特权级
6. PC ← `mtvec`（按 mode）

返回：`mret`

1. PC ← `mepc`
2. `MIE ← MPIE`
3. 特权级 ← `MPP`
4. `MPP` 常被置成实现定义的默认（如 U 或 M，看规范/实现）

**流水线含义**（留给 `01-pipeline`）：trap 时要冲刷错误路径上的指令，保证 `mepc`/`mcause` 与可见状态一致。

---

## 4. `mcause`：异常 vs 中断

```text
mcause[XLEN-1] = Interrupt（1）/ Exception（0）
mcause[XLEN-2:0] = 原因码
```

### 4.1 常见同步异常（Interrupt=0）

| 码 | 含义 | 备注 |
|----|------|------|
| 0 | Instruction address misaligned | 依扩展可 trap 或拆 |
| 1 | Instruction access fault | PMP/总线错 |
| 2 | Illegal instruction | |
| 3 | Breakpoint（`EBREAK`） | 调试 |
| 4/5/6/7 | load/store/AMO misaligned / access fault | |
| 8/9/11 | ECALL from U/S/M | 系统调用 / 固件调用 |
| 12/13/15 | page fault 类 | 有 MMU/SATP 时 |

### 4.2 标准中断（Interrupt=1）

| 码 | 名 | 典型来源 |
|----|-----|----------|
| 3 | Machine software interrupt | `MSIP`（IPI） |
| 7 | Machine timer interrupt | `mtime`/`mtimecmp` → `MTIP` |
| 11 | Machine external interrupt | PLIC/CLIC → `MEIP` |

V0：至少能接 **external**（或一根 IRQ 线进 `mip.MEIP`）+ 可选 timer。

---

## 5. 中断使能：`mstatus` / `mie` / `mip`

三层同时满足才进中断 trap：

1. **全局**：`mstatus.MIE = 1`
2. **类型**：`mie` 里对应位（`MSIE`/`MTIE`/`MEIE`）为 1
3. **挂起**：`mip` 里对应位为 1（外设/定时器置位；软件可写的位看规范）

关临界区常用：

```asm
csrc mstatus, 8      /* 清 MIE；MIE = bit3 */
/* ... 临界区 ... */
csrs mstatus, 8
```

委托给 S-mode 时用 `mideleg`/`medeleg`（Linux 平台）；**纯 M 裸机 V0 可不实现委托**。

与 PLIC：外设 → PLIC → 拉高到 hart 的 MEIP；handler 里 **claim**，处理完 **complete**（见架构系列 GIC↔PLIC 文）。

---

## 6. 最小 CSR 清单（M-mode V0）

| CSR | 作用 | V0 |
|-----|------|----|
| `mstatus` | 全局中断、MPP/MPIE 等 | 必做子集 |
| `mtvec` | trap 入口 | 必做 |
| `mepc` | 返回地址 | 必做 |
| `mcause` | 原因 | 必做 |
| `mtval` | 附加信息 | 建议做 |
| `mie` / `mip` | 中断使能/挂起 | 要中断则必做 |
| `mscratch` | 临时/内核指针 | 强烈建议 |
| `mhartid` | hart 编号 | 多核前可读常量 0 |
| `mvendorid`/`marchid`/`mimpid` | 标识 | 可常量 |
| PMP `pmpcfg*`/`pmpaddr*` | 物理保护 | U-mode 隔离时再做 |

读写：`csrr` / `csrw` / `csrs` / `csrc`；非法 CSR 访问应 trap（非法指令或相应 fault，按实现与规范）。

---

## 7. 软件侧最小 trap 框架（Direct）

```c
void trap_handler(void) {
    unsigned long cause = read_csr(mcause);
    if (cause & (1UL << (XLEN - 1))) {
        /* 中断：清源或 complete PLIC，再处理 */
    } else {
        /* 同步异常：看 mtval，修 / panic */
    }
}
```

汇编入口需保存通用寄存器到栈（或换 `mscratch` 指向的 trap 栈），再调 C；`mret` 前恢复。

---

## 8. 和 ARM 对照时怎么用两篇文章

| 需求 | 读哪篇 |
|------|--------|
| 「ARM 的 XX 寄存器 RV 叫什么？」 | [`../02-arch-boot/01-exception.md`](../02-arch-boot/01-exception.md) 总表 + 分节对照 |
| 「我这颗 core 的 trap 硬件行为 / CSR 复位值 / 波形」 | **本文** + [`01-pipeline.md`](01-pipeline.md) |
| 「PLIC 和 GIC 生命周期」 | [`../02-arch-boot/02-gic.md`](../02-arch-boot/02-gic.md) |
| 「OpenSBI / ATF」 | [`../02-arch-boot/03-boot-firmware.md`](../02-arch-boot/03-boot-firmware.md) |

一句话：**架构系列解决迁移对照；本系列解决造核实现。**

---

## 小结

- V0 可只做 **M-mode**：`mtvec` + `mepc/mcause/mtval` + `mstatus/mie/mip`。
- Trap 入口简单，分发靠 `mcause`；Direct 模式足够起步。
- 中断 = 全局使能 ∧ `mie` ∧ `mip`；外设常经 PLIC 反映到 MEIP。
- 无 TrustZone：隔离靠 PMP（以后加 U-mode 时再展开）。
- ARM 寄存器名对照放在 `02-arch-boot`，不在这里重复堆表。

## 自测

1. Direct 与 Vectored 下，同步异常与外部中断各跳到哪里？
2. Trap 进入时 `mstatus.MIE/MPIE/MPP` 如何变化？`mret` 又如何？
3. 为何说「只有 Local Monitor」不够——且这与 trap CSR 是不是同一层问题？
4. 最小要哪些 CSR 才能跑「一条非法指令进 handler 再 mret」？
5. MCU 无 TZ 时，用什么手段保护 HSM 一类外设寄存器？

---

*`04-riscv-core` · 特权级 / CSR / Trap*
