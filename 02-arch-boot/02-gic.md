# GICv3 中断控制器（对照 PLIC (Platform-Level Interrupt Controller) / CLIC (Core-Local Interrupt Controller)）

> **系列**：`02-arch-boot`  
> **前置**：[`01-exception.md`](01-exception.md)  
> **相关**：[`03-boot-firmware.md`](03-boot-firmware.md)

从外设到 CPU 的中断旅程：Distributor / Redistributor、Group、SPI (Shared Peripheral Interrupt)/PPI (Private Peripheral Interrupt)/SGI (Software Generated Interrupt)，以及和 RISC-V PLIC/CLIC、Linux 中断框架的对应关系。

**读完应能**：
- 描述一次 SPI 从触发到 ISR 的路径
- 说明 GICv3 相对 v2 的要点
- 对照 PLIC 的 claim/complete

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

| ARM GIC Group | 安全属性 | RISC-V |
|---------------|----------|--------|
| Group 0 (Secure) | FIQ → EL3 | **无**；所有 IRQ 进 M-mode 或委托 S-mode |
| Group 1 NS | IRQ → Linux | PLIC → `mideleg` → S-mode → Linux |

> **遗留 Q1 对照**：ARM Secure/NS 中断由 RTL+GIC+SCR_EL3 三层决定。RISC-V 无 Secure 中断，HSM 安全靠 **PMP (Physical Memory Protection) 禁止 S-mode 访问** + 仅 M-mode 驱动 HSM。

> **遗留 Q2 对照**：ARM FIQ 给 EL3 处理，Linux 不处理 FIQ。RISC-V 无 FIQ；若 M-mode 处理某 IRQ，S-mode Linux 同样看不到——需 `mideleg` 把外设 IRQ 委托给 S-mode。

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
- 安全分组是 ARM 特色；RV 常用 PMP 等保护 Secure 资源。

## 自测

1. GICv3 相对 v2 你认为最重要的两点？
2. SGI / PPI / SPI 各举一例用途，并对照 RV。
3. Group0/Group1 在安全模型里大致意味着什么？
4. 口述一次 SPI 从外设到 ISR 的步骤。
5. IAR/EOIR 与 PLIC Claim/Complete 如何对应？

---

*`02-arch-boot` · GICv3*
