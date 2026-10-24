# GICv3 中断控制器（对照 PLIC (Platform-Level Interrupt Controller) / CLIC (Core-Local Interrupt Controller)）

> **系列**：`02-arch-boot`  
> **前置**：[`01-exception.md`](01-exception.md)  
> **相关**：[`03-boot-firmware.md`](03-boot-firmware.md) · [`04-security-secureboot-tee.md`](04-security-secureboot-tee.md)

从外设到 CPU 的中断旅程：Distributor / Redistributor、Group、SPI (Shared Peripheral Interrupt)/PPI (Private Peripheral Interrupt)/SGI (Software Generated Interrupt)，以及和 RISC-V PLIC/CLIC、Linux 中断框架的对应关系。重点补齐 **TrustZone 下 Secure 中断**（Group0 / Secure Group1、FIQ→EL3、IAR 伪 ID 1020/1021）。

**读完应能**：
- 描述一次 SPI 从触发到 ISR 的路径
- 说明 GICv3 相对 v2 的要点
- 对照 PLIC 的 claim/complete
- 口述「Linux 跑着来了 Secure IRQ」的完整路径（含上下文保存与回程）
- 说明 1020 / 1021 / 1023 各自含义

---
## GICv3 中断控制器

## 2.1 GIC (Generic Interrupt Controller) 在系统中的位置 — 对照 PLIC/CLIC

```
ARM GIC:                           RISC-V PLIC:
  外设 → SPI → GICD → GICR → CPU     外设 → IRQ线 → PLIC → MEIP/SEIP → CPU
                                              ↑
  GICv3 新增 ITS (MSI)               CLIC: 核心本地，带中断级别（类似优先级）
```

| ARM GIC 组件 | RISC-V 对应 | 作用 |
|--------------|-------------|------|
| **GICD** (Distributor) | **PLIC** 全局寄存器 | 管理所有外部中断 enable/priority |
| **GICR** (Redistributor) | PLIC per-hart (hardware thread) 窗口 | 每个 hart 的 enable/threshold/claim |
| **GIC CPU Interface** | PLIC **Claim/Complete** | CPU 侧 ACK/EOI |
| **ITS** (MSI) | 无标准（vendor DMA (Direct Memory Access)/MSI） | PCIe MSI 中断 |
| **SGI** (软件中断) | **MSIP** (Machine Software Interrupt-Pending)（Machine Soft Interrupt） | 核间 IPI (Inter-Processor Interrupt) |
| **PPI** (私有外设) | **MTIP**（Timer）、**MEIP**（本地） | 每核私有 |
| **SPI** (共享外设) | PLIC **IRQ (Interrupt Request) 0–1023** | UART (Universal Asynchronous Receiver/Transmitter)/DMA/HSM (Hardware Security Module) 等 |

---

## 2.2 GICv2 vs GICv3 — 及与 PLIC 对比

| 特性 | GICv2 | GICv3 | RISC-V PLIC |
|------|-------|-------|-------------|
| CPU 数量 | 最多 8 核 | 大量 CPU | 1–158 hart（标准） |
| CPU 访问 | MMIO GICC_* | System Reg ICC_* | MMIO Claim/Complete |
| 中断 ID | 0–1019 | 0–1019 + LPI | **0–1023**（外部） |
| 优先级 | 8 级（256 级 v3） | 256 级 | **0–7**（标准 PLIC） |
| 安全扩展 | Group0/1 | 更完善 | **无** Secure 分组 |
| MSI | 无 | ITS | 无标准 |
| 软件 IPI | SGI 0–15 | SGI 0–15 | **MSIP**（每 hart 一位） |

**CLIC**（Core Local Interrupt Controller，RISC-V 新标准）额外提供：
- 中断**级别**（level）—— 类似 ARM 优先级
- 向量中断 —— 类似 GICv3 Vectored
- 硬件中断抢占 —— ARM GIC 也支持

---

## 2.3 中断类型 — GIC vs RISC-V

| ARM GIC | ID 范围 | RISC-V | 说明 |
|---------|---------|--------|------|
| **SGI** | 0–15 | **MSIP/SSIP** | 软件触发，核间 IPI |
| **PPI** | 16–31 | **MTIP**（Timer）、本地 | 每 CPU 私有 |
| **SPI** | 32–1019 | **PLIC IRQ 0+** | 共享外设中断 |
| **LPI** | 8192+ | 无标准 | GICv3 ITS 分配 |

**你 M100 验证对照**：
- Genesys GIC SPI 验证 → M100 外设 IRQ 接 PLIC 某 IRQ 号
- 使能：`GICD_ISENABLER` → PLIC `enable` 寄存器
- 优先级：`GICD_IPRIORITYR` → PLIC `priority` 寄存器
- 触发：`GICD_ISPENDR` → 外设拉 IRQ 线 → PLIC pending

---

## 2.4 中断 Group 与安全 — RISC-V 无此概念

GICv3 在启用 Security 时，每个 INTID 属于三类之一（由固件配 `GICD_IGROUPR` / `GICD_IGRPMODR`，**不是**「某一个固定 Secure 中断号」）：

| ARM GIC Group | 谁处理 | 在 NS 世界信号 | RISC-V |
|---------------|--------|----------------|--------|
| **Group 0** | **EL3**（BL31 / Monitor） | 始终 **FIQ (Fast Interrupt Request)** | **无**；所有 IRQ 进 M-mode 或委托 S-mode |
| **Secure Group 1** | **S-EL1**（OP-TEE / Trusted OS） | 在 NS 侧表现为 **FIQ**（先到 EL3 再转交） | 无 |
| **NS Group 1** | **NS-EL1**（Linux） | **IRQ** | PLIC → `mideleg` → S-mode → Linux |

> **怎么决定配 0 还是 1**：看 ISR 写在哪一层。BL31/平台固件 → Group 0；OP-TEE Secure 驱动 → Secure Group 1；Linux → NS Group 1。平台在 ATF (Arm Trusted Firmware) 启动时按 SoC 手册配好，Linux 一般改不了 Secure 分组。

> **遗留 Q1 对照**：ARM Secure/NS 中断由 RTL+GIC+`SCR_EL3` 三层决定。RISC-V 无 Secure 中断，HSM 安全靠 **PMP (Physical Memory Protection) 禁止 S-mode 访问** + 仅 M-mode 驱动 HSM。

> **遗留 Q2 对照**：ARM 下 Secure 相关中断以 FIQ 进 EL3，Linux 不处理 FIQ。RISC-V 无 FIQ；若 M-mode 处理某 IRQ，S-mode Linux 同样看不到——需 `mideleg` 把外设 IRQ 委托给 S-mode。

### 2.4.1 典型路由配置（面试常用）

BL31 在进入 NS 前通常配：

- `SCR_EL3.FIQ=1` → FIQ 进 **EL3**
- `SCR_EL3.IRQ=0` → 普通 IRQ 仍给当前 NS 侧（Linux）

效果：跑 Linux 时，**Group 0 / Secure Group 1** 都以 FIQ 形式打进 BL31；**NS Group 1** 走 Linux IRQ 向量。Linux 的 `DAIF.I` mask 不住这条 FIQ 路由。

### 2.4.2 场景：Linux 跑着来了 Secure 中断

**关键纠正**：不是 Linux 发 `SMC` 切过去。Secure 中断是**异步 FIQ→EL3**；`SMC` 是同步主动调用（CA→TA / PSCI）。

```
Secure 外设 pending（Group0 或 Secure Group1）
    → GICD 仲裁、路由到目标 CPU
    → CPU Interface 以 FIQ 通知
    → SCR_EL3.FIQ=1 → 异常进 EL3（BL31 VBAR FIQ 入口）
    → Linux IRQ 向量不会走到
```

**上下文必须保存**（否则回不了 Linux）：

| 谁 | 保存什么 |
|----|----------|
| **硬件** | `ELR_EL3`←被打断 PC；`SPSR_EL3`←PSTATE；切到 `SP_EL3` |
| **BL31 软件** | 通用寄存器等写入该核的 **NS `cpu_context`**（与主动 SMC 进 Monitor 同一套 Context lib） |

EL3 之后按 Group 分流：

```
读 ICC_IAR0_EL1
  ├─ 真实 INTID     → Group 0：BL31 本地处理 → EOI → restore NS → ERET 回 Linux
  ├─ 1020           → Secure Group1：切 Secure → ERET 进 OP-TEE
  │                    （真正 INTID 由 TEE 再读 ICC_IAR1_EL1 ACK）
  └─ 1023           → spurious：不 EOI，直接返回
```

### 2.4.3 TEE 处理完如何回 Linux

S-EL1 **不能**直接 `ERET` 到 NS；必须再经 EL3：

```
OP-TEE 处理完（IAR1 ACK → ISR → EOI）
    → SMC（yield / return-to-NW，如 OPTEE_SMC_RETURN_ON_IRQ 一类 FID）
    → BL31 SPD (Secure Payload Dispatcher) 识别「Secure IRQ 做完」
    → 保存 Secure context；restore 当初 FIQ 时存的 NS context
    → SCR_EL3.NS=1；ERET
    → Linux 从被打断处继续
```

| 路径 | 进 Secure | 回 Linux 恢复的是 |
|------|-----------|-------------------|
| Secure IRQ 打断 Linux | FIQ→EL3 | **FIQ 时**那份 NS context |
| Linux 主动 `SMC` 调 TA | Sync→EL3 | **该次 SMC 时**那份 NS context |

### 2.4.4 伪中断 ID：1020 / 1021 / 1022 / 1023

读 IAR (Interrupt Acknowledge Register) 时 1020–1023 是**保留特殊码**，不是外设 SPI 连线号。在 `SCR_EL3.FIQ=1` 下，EL3 读 **`ICC_IAR0_EL1`** 常见：

| ID | 含义 | EL3 该做什么 |
|----|------|----------------|
| **1020** | 最高优先级 pending = **Secure Group 1** | **切到 Secure**（Trusted OS）；真正 INTID 留给 S-EL1 的 IAR1 |
| **1021** | 最高优先级 pending = **NS Group 1** | **切到 NS**（Linux）；真正 INTID 留给 NS 的 IAR1 |
| **1022** | Legacy | 现代路径基本不碰 |
| **1023** | Spurious（真伪中断） | 无可 ACK 中断；**不要 EOI** |

一句话：**1020/1021 ≈ 通过 IAR 告诉 EL3「该切 S/NS world」的信令**（EL3 不能错误 ACK 属于另一 world 的外设中断）；**1023** 才是狭义 spurious。

对称场景（TEE 跑着、Linux 中断来了）：

```
OP-TEE (Secure) → NS Group1 pending → FIQ→EL3
  → IAR0 得 1021 → 保存 Secure context → 切 NS → ERET 回 Linux
  → Linux 读 IAR1，ACK 真正 INTID
```

这与「TEE 主动 SMC 回 Linux」不同：这是 **NS 中断抢占 Secure**，由 **1021** 驱动切回。

### 2.4.5 面试口述模板

> Linux 跑在 Non-secure 时，Secure 中断被配成 Group0 或 Secure Group1，以 FIQ 经 `SCR_EL3.FIQ` 进 BL31；硬件记 PC/PSTATE，BL31 再存 NS `cpu_context`。读 `ICC_IAR0_EL1`：真 INTID 则 EL3 自处理；**1020** 则切 OP-TEE，由 TEE 用 IAR1 ACK。TEE 做完再 SMC 回 BL31，restore NS 后 `ERET`。Linux 只处理 NS Group1 的 IRQ，一般碰不到这条路径。**不是** Linux 发 SMC 去接 Secure IRQ。

---

## 2.5 GIC 中断生命周期 — 对照 PLIC

```
步骤  ARM GIC                           RISC-V PLIC
────────────────────────────────────────────────────────────
 1    外设拉高中断线                     外设拉高中断线
 2    GICD/GICR 检查 Enable             PLIC 检查 enable[irq]
 3    优先级仲裁 vs PMR                  优先级 vs threshold
 4    Affinity Routing 到目标 CPU        固定 per-hart 或 shared
 5    CPU Interface 通知 (nIRQ)          MEIP 位置 1
 6    读 ICC_IAR1_EL1 (ACK, 得 INTID)   读 Claim 寄存器 (得 IRQ 号)
 7    跳 VBAR IRQ handler                跳 mtvec trap_handler
 8    执行 ISR                           执行 ISR
 9    写 ICC_EOIR1_EL1 (EOI)             写 Complete 寄存器 (同 IRQ 号)
10    允许更低优先级中断                  threshold 恢复
```

### 关键寄存器对照

| ARM GIC | RISC-V PLIC | 作用 |
|---------|-------------|------|
| GICD_CTLR | PLIC 全局 enable | 总开关 |
| GICD_ISENABLER | enable[irq] | 使能某中断 |
| GICD_IPRIORITYR | priority[irq] | 优先级 |
| ICC_IAR1_EL1 | **claim** | 读中断号 ACK |
| ICC_EOIR1_EL1 | **complete** | 完成通知 |
| ICC_PMR_EL1 | **threshold** | 优先级阈值 mask |
| GICD_IGROUPR | **无** | Secure/NS 分组 |

---

## 2.6 Linux 中断处理框架 — ARM vs RISC-V

```
ARM:                                RISC-V:
el1_irq_handler                     handle_riscv_irq
  → handle_arch_irq                   → riscv_intc_interrupt (PLIC)
    → gic_handle_irq                    → plic_handle_irq
      → 读 ICC_IAR1_EL1                   → 读 Claim
        → generic_handle_irq_desc           → generic_handle_irq_desc
          → driver handler                    → driver handler
            → 写 ICC_EOIR1_EL1                  → 写 Complete
```

驱动注册**完全相同**：
```c
request_irq(irq_num, my_handler, IRQF_TRIGGER_RISING, "dev", dev_id);
/* ARM: irq_num = GIC SPI 32 → RISC-V: irq_num = PLIC IRQ 0 等 */
```

---

## 2.7 GIC 与 Device Tree — 对照 PLIC DT

```dts
/* ARM GIC */
interrupt-controller@2c010000 {
    compatible = "arm,gic-v3";
    #interrupt-cells = <3>;  /* type, irq, flags */
    reg = <... GICD ...>, <... GICR ...>;
};

/* RISC-V PLIC */
plic: interrupt-controller@c000000 {
    compatible = "riscv,plic0";
    #interrupt-cells = <1>;  /* 仅 IRQ 号 */
    reg = <0x0 0xc000000 0x0 0x4000000>;
    riscv,ndev = <64>;
    interrupt-extended = <&cpu_intc 11>, <&cpu_intc 9>; /* MEIP/SEIP */
};

/* 外设引用 */
uart0: serial@10000000 {
    /* ARM: */  interrupts = <GIC_SPI 32 IRQ_TYPE_LEVEL_HIGH>;
    /* RISC-V: */ interrupts = <10>;  /* PLIC IRQ 10 */
};
```

---

## 2.8 中断 affinity 与 SMP (Symmetric Multi-Processing)

| ARM | RISC-V |
|-----|--------|
| GIC Affinity Routing 绑 CPU | PLIC 每 hart 独立 Claim，IRQ 线共享或 per-hart |
| SGI IPI → `smp_call_function` | **MSIP** IPI → 写其他 hart 的 MSIP 位 |
| DSU L3 cache | 无标准 L3（vendor cluster） |

---

---

## 小结

- GIC 把「外设事件」变成「可被 CPU 认领的中断」；v3 强化了 redistributor 与 ITS 等扩展。
- SGI/PPI/SPI 与 RV 的软中断/定时器/PLIC IRQ 可对照记忆。
- 生命周期要能走到 Linux 的 handler：ack → 处理 → eoi/complete。
- 安全分组是 ARM 特色：Group0→EL3，Secure Group1→TEE，NS Group1→Linux；RV 常用 PMP 等保护 Secure 资源。
- Secure IRQ 路径是 **FIQ→EL3**（不是 Linux SMC）；1020/1021 是 IAR 上的切 world 信令，1023 是真伪中断。

## 自测

1. GICv3 相对 v2 你认为最重要的两点？
2. SGI / PPI / SPI 各举一例用途，并对照 RV。
3. Group0 / Secure Group1 / NS Group1 分别谁处理？如何决定配哪一类？
4. 口述一次 SPI 从外设到 ISR 的步骤。
5. IAR/EOIR 与 PLIC Claim/Complete 如何对应？
6. Linux 跑着来了 Secure Group1：从 FIQ 到回 Linux 的步骤？为何不是 SMC？
7. 读 `ICC_IAR0_EL1` 得到 1020 / 1021 / 1023 各意味着什么？

---

*`02-arch-boot` · GICv3*
