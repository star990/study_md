# Embedded Stack Notes

按主题撰写的嵌入式学习库：一篇 Markdown ≈ 一个 topic（长文博客体例）。  
路径覆盖 **Linux Kernel → 架构/启动 → RTOS/BSP → RISC-V Core → 总线/RTL →（计划）电机 IP**。

面向嵌入式 / BSP / SoC 学习者；可自用，也可直接分享给同伴当复习路径。

---

## 学习路径

```text
01-kernel          调度 · 内存 · 死机 · Android/SoC 工程
        ↓
02-arch-boot       异常·GIC·启动·Security（含 ARM↔RV 对照）
        ↓
03-rtos-bsp        FreeRTOS · 芯片 BSP
        ↓
04-riscv-core      RV Core（特权/Trap 成文；流水线等草稿）
        ↓
05-bus-rtl         spinlock → AXI Exclusive / AHB HLOCK
        ↓
06-gtm-ip          （尚未开始）core + SRAM + PWM 控电机 IP
```

| 状态 | 说明 |
|------|------|
| 成文 | `01` `02` `03` `05`；`04` 中 `02-csr-trap` |
| 草稿 | `04` 其余篇 |
| 计划 | `06-gtm-ip` |

---

## 怎么读

1. 按路径从上往下，或只挑当前缺口的一篇。  
2. 每篇开头有 **系列 / 前置 / 相关 / 读完应能**——先看导语再进正文。  
3. 造 core 时以 `04` 为主；总线细节看 `05`；对照表在 `02`。  
4. 新增文章：放入对应目录，文件名 `01-` `02-`…，并更新本索引。

**体例**（全库统一）：

```text
# 标题
> 系列 / 前置 / 相关
导语
读完应能
---
正文（## 分层；章节内收束用「本节要点」）
---
## 小结     ← 3–5 条要点
## 自测     ← 若干问
---`系列 · 短名`
```

一题一文；语气中性讲授；技术附录（若有）放在小结/自测之前。

---

## 目录索引

### `01-kernel/`

| 文章 | 主题 |
|------|------|
| [01-scheduling.md](01-kernel/01-scheduling.md) | CFS / EAS / RT / PREEMPT_RT、底半部、RTOS+PMP |
| [02-memory.md](01-kernel/02-memory.md) | 伙伴/slab、页表、DMA 与 cache 一致性 |
| [03-crash-debug.md](01-kernel/03-crash-debug.md) | oops / panic / hung、crash 与 tracing |
| [04-android-soc-scheduling.md](01-kernel/04-android-soc-scheduling.md) | Android/SoC 调度工程；Part B 链到总线专文 |

### `02-arch-boot/`

| 文章 | 主题 |
|------|------|
| [01-exception.md](02-arch-boot/01-exception.md) | **原 Day1 对照主文**：AArch64 异常/寄存器 ↔ RISC-V（迁移用） |
| [02-gic.md](02-arch-boot/02-gic.md) | GICv3 ↔ PLIC/CLIC |
| [03-boot-firmware.md](02-arch-boot/03-boot-firmware.md) | ATF / OpenSBI / 启动与安全衔接 |
| [04-security-secureboot-tee.md](02-arch-boot/04-security-secureboot-tee.md) | Secure Boot、HSM、OP-TEE |

### `04-riscv-core/`

| 文章 | 主题 | 状态 |
|------|------|------|
| [01-pipeline.md](04-riscv-core/01-pipeline.md) | 流水线 | 草稿 |
| [02-csr-trap.md](04-riscv-core/02-csr-trap.md) | **RV 特权级 / CSR / Trap（造核用）** | 成文初版 |
| [03-memory-bus.md](04-riscv-core/03-memory-bus.md) | 访存与主口 | 草稿 |
| [04-verification.md](04-riscv-core/04-verification.md) | 验证 | 草稿 |

> **分工**：`02-arch-boot` = ARM 主线 + RV 对照；`04-riscv-core` = 以 RISC-V 为本写造核行为。对照表在 `01-exception`；trap 实现骨架在 `02-csr-trap`。

### `03-rtos-bsp/`

| 文章 | 主题 |
|------|------|
| [01-freertos-realtime.md](03-rtos-bsp/01-freertos-realtime.md) | FreeRTOS 与实时选型 |
| [02-bsp-and-boot.md](03-rtos-bsp/02-bsp-and-boot.md) | Zynq/DT/驱动与外设速记 |

### `05-bus-rtl/`

| 文章 | 主题 |
|------|------|
| [01-spinlock-to-bus.md](05-bus-rtl/01-spinlock-to-bus.md) | spinlock → AXI Exclusive / Monitor；对照 AHB `HLOCK` |

### `06-gtm-ip/`

计划中：简易 GTM（core + SRAM + PWM）作为控电机 IP。

---

## 推荐顺序

1. `01-scheduling` → `02-memory` → `03-crash-debug`  
2. `04-android-soc-scheduling`（产品侧，按需）  
3. `01-exception` → `02-gic` → `03-boot-firmware` → `04-security-…`  
4. `01-freertos-realtime` → `02-bsp-and-boot`  
5. `04-riscv-core/02-csr-trap`（RV 异常/CSR；对照表仍看 `01-exception`）  
6. `01-spinlock-to-bus`  
7. `04` 其余草稿 → 之后 `06-gtm-ip`

---

## 仓库说明

- 中文为主，术语保留英文惯用名。  
- 内容来自 ARM/Linux BSP、RISC-V MCU、FPGA 原型、Secure Boot/HSM 等实战整理。  
- 欢迎纠错；新文请挂索引。
