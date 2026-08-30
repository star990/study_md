# AArch64 异常模型（对照 RISC-V）

> **系列**：`02-arch-boot`  
> **前置**：无特别前置  
> **相关**：[`02-gic.md`](02-gic.md) · [`03-boot-firmware.md`](03-boot-firmware.md) · [`../04-riscv-core/02-csr-trap.md`](../04-riscv-core/02-csr-trap.md)

把 EL、向量表、同步/IRQ/FIQ/SError、系统寄存器和 TrustZone 世界切换理顺，并在每个关键点挂上 RISC-V 对照，方便两边经验互相迁移。

> **和造核系列的分工**：本文是 **ARM 主线 + RV 对照**（原面试/迁移笔记）。若要按「实现一颗 RV core」读特权级、`mtvec`/`mcause`、trap 时序，请看 [`../04-riscv-core/02-csr-trap.md`](../04-riscv-core/02-csr-trap.md)。

**读完应能**：
- 画出 EL0–EL3 与典型软件栈位置
- 说明四类异常差异
- 用对照表完成 ARM↔RV 关键词翻译

---
## ARM ↔ RISC-V 总对照表


> 读法建议：先建立「同概念不同寄存器名」的映射，再往下钻每一节的细节。

| 概念 | ARM (AArch64) | RISC-V（MCU 常见） | 备注 |
|------|---------------|----------------------|------|
| **特权级** | EL0/EL1/EL2/EL3（4 级） | U/S/M-mode（3 级） | ARM 多 EL2 虚拟化、EL3 Monitor |
| **异常向量基址** | `VBAR_ELx` | `mtvec`（M-mode）/ `stvec`（S-mode） | RV 直接写 trap handler 地址 |
| **异常返回地址** | `ELR_ELx` | `mepc` / `sepc` | |
| **异常原因** | `ESR_ELx`（EC 字段） | `mcause` / `scause` | RV 最高位 1=interrupt, 0=exception |
| **Fault 地址** | `FAR_ELx` | `mtval` / `stval` | PMP 出错常看 `mtval` |
| **程序状态/中断屏蔽** | `PSTATE` / `DAIF`（I/F 位） | `mstatus.MIE/SIE` | RV 关中断：`csrc mstatus, MIE` |
| **系统调用** | `SVC #0` | `ECALL`（U→S）/ `ECALL`（S→M） | |
| **Secure 调用** | `SMC #0` → EL3 | 无标准 TZ；`ECALL` → M-mode | ARM 有 TrustZone，RV 靠 PMP |
| **中断控制器** | GICv2/v3 | **PLIC**（平台级）/ **CLIC**（核心本地） | GIC≈PLIC+部分 CLIC 功能 |
| **核间中断 IPI** | GIC SGI (ID 0–15) | `MSIP`（Machine Soft Interrupt） | PLIC 本身无 SGI |
| **外设中断** | GIC SPI (ID 32+) | PLIC 外部 IRQ 0–1023 | |
| **本地定时器** | GIC PPI (Timer) | `MTIP`/`STIP`（mtime 比较） | |
| **快速/安全中断** | FIQ（Group0） | RV 无 FIQ；CLIC 有中断级别 | TrustZone 用 FIQ 分 Secure |
| **内存保护** | MMU 页表 + TrustZone | MMU（`satp`）+ **PMP** + **PMA** | 常见：PMP NAPOT、PMA 属性 |
| **内存属性** | `MAIR_EL1`（Device/Normal） | **PMA**（Cacheable/IO/Main Memory） | device region 对应 PMA I/O |
| **内存屏障** | DMB / DSB / ISB | `fence` / `fence.i` | |
| **Boot Monitor** | BL31 (ATF, EL3) | **OpenSBI**（M-mode） | 都提供 PSCI↔SBI |
| **电源管理 API** | PSCI（SMC 调用） | **SBI**（ECALL 调用） | CPU_ON/OFF/RESET |
| **Secure Boot** | BL1→BL2→BL31→TEE | BootROM(M)→2nd Loader→OpenSBI | RV 通常更简单，无 TZ 链 |
| **栈指针** | SP_EL0 / SP_ELx | `sp`（各 mode 独立或共用） | M100 stack on TCM |
| **断点** | Breakpoint Exception | `EBREAK`（编码示例 `0x00100073`） | |

---

## AArch64 异常架构

## 1.1 四个 Exception Level（EL0–EL3）

ARMv8-A AArch64 用 **Exception Level** 划分软件特权，共 4 级：

```
        ┌─────────────────────────────────────┐
  EL3   │  Secure Monitor (BL1/BL31)          │  最高特权，管 Secure/Non-secure 切换
        ├─────────────────────────────────────┤
  EL2   │  Hypervisor (KVM/虚拟化)             │  可选，虚拟化才用
        ├─────────────────────────────────────┤
  EL1   │  OS Kernel (Linux/OP-TEE)           │  操作系统内核
        ├─────────────────────────────────────┤
  EL0   │  User Application                   │  用户态应用
        └─────────────────────────────────────┘
```

### ARM ↔ RISC-V：特权级对照

```
ARM                          RISC-V (标准 3 级)
─────────────────────────────────────────────────
EL0  User App                U-mode  用户态
EL1  OS Kernel               S-mode  监督态（Supervisor）
EL2  Hypervisor (可选)        HS-mode 虚拟化扩展（H extension，可选）
EL3  Secure Monitor          M-mode  机器态（最高特权，BootROM 在这）
```

| ARM EL | RISC-V Mode | 典型软件（ARM） | 典型软件（RISC-V MCU） |
|--------|-------------|----------------|----------------------|
| EL0 | U-mode | 用户 App | 无 OS 时通常不用 |
| EL1 | S-mode | Linux / OP-TEE | 无（M100 纯 M-mode 裸机） |
| EL2 | HS-mode | KVM | 一般不用 |
| EL3 | **M-mode** | BL1/BL31 | **BootROM、全部裸机代码** |

**关键差异**：
- RISC-V 标准只有 **3 级**（U/S/M），没有 ARM 的 TrustZone 双 world
- 你 M100/M200 **全部跑 M-mode**，没有 EL1/EL0 切换
- ARM EL3 ≈ RISC-V M-mode，但 EL3 还管 Secure/Non-secure 切换，M-mode 不管

### 各 EL 典型用途

| EL | 典型软件 | Secure Boot 阶段 |
|----|----------|------------------|
| **EL3** | Monitor、BootROM、BL1、BL31 | BootROM 启动在这里 |
| **EL2** | Hypervisor（KVM/Xen） | 一般 boot 阶段不涉及 |
| **EL1** | Linux Kernel、OP-TEE OS | BL32(OP-TEE) 在 Secure EL1；Linux 在 Non-Secure EL1 |
| **EL0** | 用户态 App | 系统跑起来后 |

### 关键记忆点

- **数字越大，特权越高**：EL3 > EL2 > EL1 > EL0（同 RISC-V M > S > U）
- **同一 EL 分 Secure 和 Non-secure 两个 world**（TrustZone）→ RISC-V **无此概念**
- Boot 阶段：ROM → EL3/M-mode；Linux → NS EL1/S-mode

---

## 1.2 AArch64 vs AArch32

| 特性 | AArch64 | AArch32 | RISC-V (RV32/64) |
|------|---------|---------|------------------|
| 通用寄存器 | X0–X30（64 位） | R0–R15（32 位） | x0–x31（32/64 位） |
| 程序计数器 | PC（独立） | PC（独立） | **无独立 PC**，用 x1/x5 或 jal |
| 栈指针 | SP_EL0 / SP_EL1... | SP（随模式变） | **sp**（x2） |
| 异常级别 | EL0–EL3 | PL0–PL2 + Monitor | U/S/M-mode |
| 异常入口 | **VBAR_ELx** | VBAR | **mtvec / stvec** |
| 返回地址 | LR (X30) | LR (R14) | **ra**（x1，jal 保存） |

### 运行时如何决定 AArch32 还是 AArch64？

- **ARM**：由 `HCR_EL2` 或 `SCR_EL3` 配置；ERET 时看 `SPSR_ELx.M[4]`
- **RISC-V**：由 `mstatus.MXL/SXL` 或编译/启动配置决定 XLEN（32/64），无 ARM 那种运行时切换

---

## 1.3 关键系统寄存器（必背）

### SCR_EL3（Secure Configuration Register，EL3 专用）

| 位 | 名称 | 作用 |
|----|------|------|
| 0 | NS | 0=Secure world，1=Non-secure world |
| 1 | IRQ | IRQ 路由：0→EL3，1→当前 EL 的 NS 侧 |
| 2 | FIQ | FIQ 路由：0→EL3，1→当前 EL 的 NS 侧 |
| 4 | EA | External Abort 路由 |
| 10 | RW | 0=低级别跑 AArch32，1=跑 AArch64 |

> **RISC-V 对照**：**无 SCR_EL3 等价物**。RISC-V 没有 TrustZone，安全隔离靠 **PMP**（物理地址权限）和 vendor 扩展。中断路由无 NS/S 之分，全部进 M-mode 或委托给 S-mode（`mideleg`）。

| ARM SCR_EL3 | RISC-V 近似 |
|-------------|-------------|
| NS (Secure/Non-secure) | 无；PMP 条目限制地址范围权限 |
| IRQ/FIQ routing | `mideleg` / `medeleg`（异常/中断委托给 S-mode） |
| RW (AArch32/64) | `mstatus.MXL`（位宽） |

### HCR_EL2（Hypervisor Configuration Register）

虚拟化用。**RISC-V 对照**：**H extension** 的 `hstatus`、`hedeleg`、`hideleg` 等，M100 无虚拟化不涉及。

### DAIF（中断 mask 位，在 PSTATE 里）

| 位 | 含义 | 作用 |
|----|------|------|
| D | Debug | Debug 异常 mask |
| A | SError | 异步 Abort mask |
| I | **IRQ** | 普通中断 mask |
| F | **FIQ** | 快速中断 mask |

- `MSR DAIFSet, #4` → 关 IRQ
- `MSR DAIFClr, #4` → 开 IRQ

> **RISC-V 对照**：

| ARM DAIF | RISC-V mstatus | 作用 |
|----------|----------------|------|
| I (IRQ) | **MIE**（bit 3） | M-mode 全局中断使能 |
| I (IRQ, S-mode) | **SIE**（bit 1） | S-mode 全局中断使能 |
| F (FIQ) | **无** | RV 无 FIQ 概念 |
| A (SError) | 平台 NMI / 总线错误 | 非标准，vendor 定义 |

```c
/* ARM 关中断 */
MSR DAIFSet, #4          // 关 IRQ

/* RISC-V 关中断（你 M100 常用） */
csrc mstatus, MIE        // 清 MIE 位
csrc mstatus, 0x8        // MIE = bit 3
```

### 其他必知寄存器 — ARM ↔ RISC-V 完整对照

| ARM 寄存器 | 作用 | RISC-V 对应 | 备注 |
|------------|------|-------------|------|
| **VBAR_ELx** | 异常向量表基址 | **mtvec**（M-mode）/ **stvec**（S-mode） | RV 写入 handler 地址；mode=0 直接跳转，mode=1 vectored |
| **SPSR_ELx** | 异常发生时保存 PSTATE | **mstatus**（部分）/ 硬件压栈 | RV 硬件保存 pc 到 mepc，status 到 mstatus |
| **ELR_ELx** | 异常返回地址 | **mepc** / **sepc** | |
| **ESR_ELx** | 异常综合寄存器（原因） | **mcause** / **scause** | RV：bit63=1 中断，0 异常；低位=原因码 |
| **FAR_ELx** | Fault 地址 | **mtval** / **stval** | PMP 出错看 **mtval** |
| **TTBR0/1_EL1** | 页表基址 | **satp** | RV MODE+ASID+PPN |
| **TCR_EL1** | 页表配置 | satp.MODE + PG 位 | |
| **MAIR_EL1** | Normal/Device 属性 | **PMA** 配置 | PMA：Cacheable / Non-cacheable / IO |

### ESR vs mcause 原因码对照

| 异常类型 | ARM ESR.EC | RISC-V mcause (exception) |
|----------|------------|---------------------------|
| 系统调用 | 0x15 (SVC) | **8** (Environment call from U-mode) |
| Secure 调用 | 0x17 (SMC) | **11** (Environment call from M-mode) 或 9/10 |
| 取指页 fault | 0x20/0x21 | **12** (Instruction page fault) |
| 数据页 fault | 0x24/0x25 | **13/15** (Load/Store page fault) |
| 断点 | 0x22/0x23 | **3** (Breakpoint = EBREAK) |
| 非法指令 | 0x00 | **2** (Illegal instruction) |
| 外部中断 | — (走 IRQ 向量) | mcause **bit63=1** + 中断号 | 

---

## 1.4 异常向量表（VBAR_ELx）

### ARM 向量表结构

AArch64 每个 EL 有自己的向量表，由 `VBAR_ELx` 指向，**每条 entry 128 字节**：

```
偏移        类型                    触发条件
────────────────────────────────────────────────────
0x000      Synchronous           当前 EL，使用 SP_EL0
0x080      IRQ                   当前 EL，使用 SP_EL0
0x100      FIQ                   当前 EL，使用 SP_EL0
0x180      SError                当前 EL，使用 SP_EL0
0x200      Synchronous           当前 EL，使用 SP_ELx
...（共 16 个 entry）
```

### RISC-V 对照：mtvec / stvec

RISC-V **没有** ARM 那种 16-entry 向量表，只有 **一个** trap 入口：

```
mtvec 寄存器：
  [1:0] MODE：
    00 = Direct   → 所有 trap 跳到 mtvec 同一地址
    01 = Vectored → 中断跳 mtvec + 4×cause，异常仍跳 mtvec

stvec：S-mode 等价物（若启用 S-mode）
```

| 对比项 | ARM VBAR_ELx | RISC-V mtvec |
|--------|--------------|--------------|
| 入口数量 | 16 个（Sync/IRQ/FIQ/SError × 4 种 SP/EL） | **1 个**（Direct）或 1+N（Vectored 仅中断） |
| Sync/IRQ 分开？ | **是**，不同 offset | Direct 模式：**否**，同一 handler 查 mcause |
| FIQ 独立入口？ | **是**（0x100/0x300...） | **无 FIQ** |
| 设置方式 | `MSR VBAR_EL1, x0` | `csrw mtvec, addr` |

```assembly
/* ARM: 16 个独立 entry */
VBAR_EL1 + 0x080  → el1_irq_handler
VBAR_EL1 + 0x000  → el1_sync_handler

/* RISC-V: 一个 trap_handler，内部分发 */
trap_handler:
    csrr t0, mcause
    bgez t0, handle_exception   /* bit63=0 异常 */
    handle_interrupt:           /* bit63=1 中断 */
    /* 再按 cause 分支 */
```

###  Linux entry.S 分工

| 向量 | 处理 | RISC-V 等价 |
|------|------|-------------|
| sp0 sync | invalid | N/A（无 SP 选择） |
| spx fiq | fiq except | **无 FIQ** |
| low level sync | sync handler | trap_handler 查 mcause |
| low level irq | irq handler | trap_handler → PLIC claim |

---

## 1.5 四类异常

### 1. Synchronous Exception（同步异常）

| 类型 | ARM 例子 | ARM ESR.EC | RISC-V mcause |
|------|----------|------------|---------------|
| 系统调用 | `SVC #0` | 0x15 | 8 (ECALL from U) / 9 (from S) |
| Secure 调用 | `SMC #0` | 0x17 | 11 (ECALL from M) — 无 TZ |
| 取指 fault | Page fault | 0x20/0x21 | 12 (Inst page fault) / 1 (Inst access fault) |
| 数据 fault | Data abort | 0x24/0x25 | 13/15 (Load/Store page fault) / 5/7 |
| 断点 | Breakpoint | 0x22/0x23 | **3 (Breakpoint)** — `EBREAK` |
| 非法指令 | Undefined | 0x00 | 2 (Illegal instruction) |

**ARM 处理流程**：
```
指令触发 → PC→ELR, PSTATE→SPSR → VBAR+offset → 读 ESR/FAR → ERET
```

**RISC-V 处理流程**（你 M100 熟悉）：
```
指令触发 → pc→mepc, 改 mstatus → mtvec → 读 mcause/mtval → mret
```

> **EBREAK 对照**：GDB/OpenOCD 软件断点替换为 `EBREAK`（0x00100073），CPU 直接读到的就是这条指令；ARM 对应 `BRK #imm`。

### 2. IRQ（Interrupt Request）

| | ARM | RISC-V |
|--|-----|--------|
| 来源 | **GIC** 触发 | **PLIC** 或 **CLIC** |
| 信号 | nIRQ 引脚 | PLIC 汇总 → `MEIP`/`SEIP` 位 |
| 入口 | VBAR + 0x080/0x280 | mtvec（Vectored: mtvec+4×cause） |
| ACK | 读 `ICC_IAR1_EL1` | 读 PLIC **Claim** 寄存器 |
| EOI | 写 `ICC_EOIR1_EL1` | 写 PLIC **Complete** 寄存器 |
| Linux | `el1_irq_handler` | `handle_riscv_irq` → PLIC handler |

### 3. FIQ（Fast Interrupt Request）

| | ARM | RISC-V |
|--|-----|--------|
| 存在？ | **有**，独立 nFIQ 引脚 | **无** FIQ 概念 |
| 用途 | TrustZone Secure 中断（Group0） | 无 Secure 中断分级 |
| 独立向量？ | VBAR + 0x100/0x300 | 无 |
| 更多 banked 寄存器？ | ARMv7 时代有，v8 缩小差距 | N/A |
| 近似替代 | — | **CLIC** 中断级别（level）可设高优先级快速响应 |

> **要点**：FIQ 是 ARM 特有，TrustZone 用它路由 Secure 中断到 EL3。RISC-V 没有 FIQ/IRQ 二分，所有外部中断走 PLIC，M-mode 统一处理或通过 `mideleg` 委托 S-mode。

### 4. SError（System Error）

| | ARM | RISC-V |
|--|-----|--------|
| 类型 | 异步 bus error、parity error | 平台相关 NMI / Bus Error |
| 屏蔽 | DAIF.A | 通常不可屏蔽（NMI） |
| 严重性 | 常 kernel panic | 常触发 mcause=**bus error** 或复位 |

---

## 1.6 异常处理完整流程（手绘用）

```
                    ARM                          RISC-V
                    ───                          ──────
              ┌──────────────┐            ┌──────────────┐
              │  CPU 正常运行 │            │  CPU 正常运行 │
              │  EL1/EL0     │            │  M-mode      │
              └──────┬───────┘            └──────┬───────┘
                     │                           │
        ┌────────────┼────────────┐    ┌─────────┼─────────┐
        │            │            │    │         │         │
   Sync Exception   IRQ/FIQ    SError  Sync    IRQ(PLIC)   NMI
        │            │            │    │         │         │
        ▼            ▼            ▼    ▼         ▼         ▼
   查 ESR_ELx    GIC 仲裁     查 ESR  mcause   PLIC claim  vendor
   查 FAR_ELx    读 IAR                 mtval   mcause
        │            │            │    │         │         │
        └────────────┼────────────┘    └─────────┼─────────┘
                     │                           │
              保存上下文到栈                 保存上下文（或硬件）
              (ELR/SPSR/通用寄存器)          (mepc/mstatus/寄存器)
                     │                           │
              调用 C handler                 调用 C handler
                     │                           │
              ERET 返回                      mret 返回
```

---

## 1.7 栈指针 SP 的选择

| | ARM | RISC-V (M100/M200) |
|--|-----|---------------------|
| 用户栈 | SP_EL0 | sp（U-mode，若启用） |
| 内核/Monitor 栈 | SP_EL1/2/3 | sp（M-mode） |
| 每 task 独立栈？ | 是（Linux） | 裸机通常一个栈 |
| 栈溢出检测 | STACK_END_MAGIC | 可软件 canary |

**你 M100 经验对照**：
- ARM ATF：`platform_up_stack.S` / `platform_mp_stack.S`
- RISC-V M100：**stack on TCM**、ILM/DLM 放代码数据，sp 在 SRAM
- 原理相同：Boot 阶段设 sp → 调 C 函数

---

## 1.8 TrustZone 与两个 World

ARM 独有，RISC-V **无标准等价物**：

| | ARM TrustZone | RISC-V |
|--|---------------|--------|
| Secure/Non-secure | SCR_EL3.NS 位切换 | **无** |
| Secure Monitor | BL31 (EL3) | M-mode 即最高级，无 world 切换 |
| Secure OS | OP-TEE (Secure EL1) | vendor TEE 或纯 M-mode |
| 隔离机制 | TZASC/TZC + MMU NS bit | **PMP**（物理地址范围权限） |
| 跨 world 调用 | **SMC** 指令 | **ECALL** 到 M-mode（OpenSBI SBI） |
| 内存保护 | Secure PA 不可 NS 访问 | PMP 条目设 R/W/X 权限 |

```
ARM TrustZone:                    RISC-V 安全模型:
┌──────────┬──────────┐          ┌──────────────────┐
│ Secure   │ Non-Sec  │          │  M-mode (最高)    │
│ OP-TEE   │ Linux    │          │  PMP 保护区域     │
│ (S-EL1)  │ (NS-EL1) │          │  ├─ BootROM      │
├──────────┴──────────┤          │  ├─ HSM 寄存器    │
│  EL3 Monitor (BL31) │          │  └─ 密钥存储      │
└─────────────────────┘          └──────────────────┘
```

> **你 M100 HSM Secure Boot**：无 TrustZone，HSM 寄存器靠 **PMP** 或地址 decode 限制仅 M-mode 访问，Secure Boot 在 M-mode BootROM 完成验签——原理同 ARM BL1，但没有 world 切换。

### World 切换 vs RISC-V ECALL

| ARM | RISC-V |
|-----|--------|
| Linux CA → OP-TEE TA | 用户 App → ECALL → M-mode handler |
| 路径：CA→Driver→**SMC**→BL31→OP-TEE | 路径：App→**ECALL**→OpenSBI/M-mode |
| SCR_EL3.NS 位切换 | 无 world，始终同一地址空间 |

---

## 1.9 内存屏障（DMB / DSB / ISB）

| ARM | RISC-V | 作用 |
|-----|--------|------|
| **DMB** | `fence rw, rw` | 读写保序，不等待完成 |
| **DSB** | `fence rw, rw` + 更强语义 | 等待 memory access 完成 |
| **ISB** | **`fence.i`** | 刷新取指流水线 |

> **你 M100 笔记**：`fence_i` 用于 icache invalidate 后；ARM 对应 `ISB`。

```c
/* ARM */
writel(val, DMA_CTRL);
dsb();
writel(1, DMA_START);

/* RISC-V */
*(volatile uint32_t*)DMA_CTRL = val;
fence iorw, ow;   /* 或 __asm__ volatile ("fence w,w") */
*(volatile uint32_t*)DMA_START = 1;
```

---

## 1.10 MMU 与内存属性

### ARM MAIR vs RISC-V PMA

| ARM MAIR 类型 | RISC-V PMA 类型 | MCU 笔记对照 |
|---------------|-----------------|----------------|
| Normal, Write-Back | Main Memory, **Cacheable, Write-Back** | 默认 cacheable region |
| Normal, Non-cacheable | Main Memory, **Non-cacheable** | — |
| Device-nGnRnE | **I/O**, Non-cacheable | **device region** |
| Device-nGnRE | I/O（稍宽松） | 外设寄存器 |

**笔记对照**：
- ARM：MMU 配 Device → RISC-V：**PMA 配 I/O 类型**
- ARM：MAIR Normal 配错 → RISC-V：**PMA Cacheable 配错**
- 结果一样：外设访问走 cache → 必须 flush/invalidate

| 保护机制 | ARM | RISC-V (M100) |
|----------|-----|---------------|
| 虚拟地址翻译 | MMU 页表 (TTBR) | MMU satp（若启用）或物理地址 |
| 物理地址权限 | MMU AP + TrustZone | **PMP**（R/W/X，NAPOT 格式） |
| 物理地址属性 | MAIR | **PMA** |
| 案例 | HSM MMU Device 配错 | **PMP 出错 mtval**、PMA device region |

---

---

## 小结

- EL0–EL3 决定「谁在跑、能碰什么」；Secure Boot 与 OS 落在不同 EL。
- 同步异常与 IRQ/FIQ/SError 原因与返回路径不同；RV 侧用 mcause + 单入口分发。
- 关键寄存器要能对照：VBAR↔mtvec，ESR↔mcause，FAR↔mtval。
- TrustZone 世界切换与 RV 的 PMP/ECALL 是不同隔离模型，不要硬一一对应。

## 自测

1. EL0–EL3 分别典型跑什么？对照 U/S/M。
2. Sync 与 IRQ 的本质差别？RV 如何表达？
3. SCR_EL3 管什么？RV 侧近似手段？
4. VBAR 与 mtvec 的结构差异？
5. TrustZone 切换时 TLB/缓存直觉上要注意什么？

---

*`02-arch-boot` · 异常模型*
