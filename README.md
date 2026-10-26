# Embedded Stack Notes

按主题撰写的嵌入式学习库：一篇 Markdown ≈ 一个 topic（长文博客体例）。  
路径覆盖 **Linux Kernel → 架构/启动 → RTOS (Real-Time Operating System)/BSP → RISC-V Core → 总线/RTL →（计划）电机 IP → 面试专题 → AI 大 SoC（`08-ai-soc`）**。

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
04-riscv-core      RV Core（Trap / Load→SRAM 成文；流水线等草稿）
        ↓
05-bus-rtl         spinlock → AXI Exclusive / AHB HLOCK
        ↓
06-gtm-ip          RTL 基础 ·（计划）core + SRAM + PWM 控电机 IP
        ↓
07-interview       面试题（01-chip-validation · 02-core-bus · 03-bsp-kernel · 04-secure-boot · 05-coding-algo）
        ↓
08-ai-soc          AI 大 SoC（01-soc-common · 02-tpu · 03-gpu）
```

| 状态 | 说明 |
|------|------|
| 成文 | `01` `02` `03` `05`；`04` 中 `02-csr-trap`、`03-memory-bus`；`06` 中 `01-rtl-basics` |
| 草稿 | `04` 中流水线 / 验证 |
| 计划 | `06-gtm-ip` 其余（GTM 架构 / PWM / 集成）；`07-interview` 其余真题；`08-ai-soc` 中 `01`/`03` |
| 成文初版 | `08-ai-soc` 中 [`02-tpu`](08-ai-soc/02-tpu.md)（架构 vs GPU + 数据流性能） |

---

## 怎么读

1. 按路径从上往下，或只挑当前缺口的一篇。  
2. 每篇开头有 **系列 / 前置 / 相关 / 读完应能**——先看导语再进正文。  
3. 造 core 时以 `04` 为主；总线细节看 `05`；对照表在 `02`。  
4. 新增文章：放入对应目录，文件名 `01-` `02-`…，并更新本索引。  
5. `07-interview` 另有体例：见 [`07-interview/README.md`](07-interview/README.md)（真题记录 + 盲区延伸）。

**体例**（`01`–`06` 长文）：

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
| [01-scheduling.md](01-kernel/01-scheduling.md) | CFS (Completely Fair Scheduler) / EAS (Energy Aware Scheduling) / RT / PREEMPT_RT (Preemptible Real-Time)、底半部、RTOS+PMP (Physical Memory Protection) |
| [02-memory.md](01-kernel/02-memory.md) | 伙伴/slab、页表、DMA (Direct Memory Access) 与 cache 一致性 |
| [03-crash-debug.md](01-kernel/03-crash-debug.md) | oops / panic / hung、crash 与 tracing |
| [04-android-soc-scheduling.md](01-kernel/04-android-soc-scheduling.md) | Android/SoC 调度工程；Part B 链到总线专文 |

### `02-arch-boot/`

| 文章 | 主题 |
|------|------|
| [01-exception.md](02-arch-boot/01-exception.md) | **原 Day1 对照主文**：AArch64 异常/寄存器 ↔ RISC-V（迁移用） |
| [02-gic.md](02-arch-boot/02-gic.md) | GICv3 ↔ PLIC (Platform-Level Interrupt Controller)/CLIC (Core-Local Interrupt Controller) |
| [03-boot-firmware.md](02-arch-boot/03-boot-firmware.md) | ATF (Arm Trusted Firmware) / OpenSBI (Supervisor Binary Interface) / 启动与安全衔接 |
| [04-security-secureboot-tee.md](02-arch-boot/04-security-secureboot-tee.md) | Secure Boot、HSM (Hardware Security Module)、OP-TEE (Trusted Execution Environment) |

### `04-riscv-core/`

| 文章 | 主题 | 状态 |
|------|------|------|
| [01-pipeline.md](04-riscv-core/01-pipeline.md) | 流水线 | 草稿 |
| [02-csr-trap.md](04-riscv-core/02-csr-trap.md) | **RV 特权级 / CSR (Control and Status Register) / Trap（造核用）** | 成文初版 |
| [03-memory-bus.md](04-riscv-core/03-memory-bus.md) | **Load：LSU (Load/Store Unit)→TLB (Translation Lookaside Buffer)/PMP/PMA (Physical Memory Attributes)→D$→总线→SRAM** (Static Random-Access Memory)；单核闭环 + SMP (Symmetric Multi-Processing) | 成文初版 |
| [04-verification.md](04-riscv-core/04-verification.md) | 验证 | 草稿 |

> **分工**：`02-arch-boot` = ARM 主线 + RV 对照；`04-riscv-core` = 以 RISC-V 为本写造核行为。对照表在 `01-exception`；trap 在 `02-csr-trap`；访存闭环在 `03-memory-bus`。

### `03-rtos-bsp/`

| 文章 | 主题 |
|------|------|
| [01-freertos-realtime.md](03-rtos-bsp/01-freertos-realtime.md) | FreeRTOS 与实时选型 |
| [02-bsp-and-boot.md](03-rtos-bsp/02-bsp-and-boot.md) | Zynq/DT/驱动与外设速记 |

### `05-bus-rtl/`

| 文章 | 主题 |
|------|------|
| [01-spinlock-to-bus.md](05-bus-rtl/01-spinlock-to-bus.md) | spinlock → AXI (Advanced eXtensible Interface) Exclusive / Monitor；对照 AHB (Advanced High-performance Bus) `HLOCK` |
| [02-cache.md](05-bus-rtl/02-cache.md) | Cache 专题：属性/`AxCACHE`/4KB（后续补一致性等） |
| [03-axi-ahb.md](05-bus-rtl/03-axi-ahb.md) | AXI/AHB 协议速查：`AxCACHE`/`AxPROT`/`HPROT`、原子、桥接、Outstanding/AxID |

### `06-gtm-ip/`

| 文章 | 主题 | 状态 |
|------|------|------|
| [01-rtl-basics.md](06-gtm-ip/01-rtl-basics.md) | **RTL 基础**：软件 vs 硬件思维、FF/组合/时序、一拍与流水线、抓波读法 | 成文初版 |

> 后续计划：简易 GTM（core + SRAM + PWM）作为控电机 IP。

### `07-interview/` · [系列说明](07-interview/README.md)

**宗旨**：总结面试题，温故而知新。每篇两大块——**真题记录**（真实遇到的有价值题）+ **盲区延伸**（由题牵出的薄弱点补强）；深挖链回 `01`–`06`。

| 文章 | 主题 | 状态 |
|------|------|------|
| [`01-chip-validation.md`](07-interview/01-chip-validation.md) | 芯片验证（Q1：帕拉丁偶现；Q2：DMA outstanding=64；Q3：CWF / Fill Buffer） | 收录中 |
| [`02-core-bus.md`](07-interview/02-core-bus.md) | 核 / 总线（Q1：AXI `rwrwrw` vs `rrrwww`） | 收录中 |
| [`03-bsp-kernel.md`](07-interview/03-bsp-kernel.md) | BSP / Kernel | 待收录 |
| [`04-secure-boot.md`](07-interview/04-secure-boot.md) | Secure Boot | 待收录 |
| [`05-coding-algo.md`](07-interview/05-coding-algo.md) | Coding / 算法（Q1：Ring Buffer；Q2：字符串倒转；Q3：圆环长度；Q4：冒泡） | 收录中 |

### `08-ai-soc/` · [系列说明](08-ai-soc/README.md)

**宗旨**：AI 相关大 SoC——先 `soc-common` 打通片上骨架，再分 `tpu` / `gpu` 两条加速路径；总线/核/启动细节链回 `01`–`06`。扁平一主题一篇 md（同 `07-interview`）。

| 文章 | 主题 | 状态 |
|------|------|------|
| [`01-soc-common.md`](08-ai-soc/01-soc-common.md) | SoC 通用（互联、内存层次、功耗热、软件栈分界） | 计划 |
| [`02-tpu.md`](08-ai-soc/02-tpu.md) | TPU / NPU：架构 vs GPU、数据流与传输性能 | 成文初版 |
| [`03-gpu.md`](08-ai-soc/03-gpu.md) | GPU 与 AI 算力 | 计划 |

---

## 推荐顺序

1. `01-scheduling` → `02-memory` → `03-crash-debug`  
2. `04-android-soc-scheduling`（产品侧，按需）  
3. `01-exception` → `02-gic` → `03-boot-firmware` → `04-security-…`  
4. `01-freertos-realtime` → `02-bsp-and-boot`  
5. `04-riscv-core/02-csr-trap`（RV 异常/CSR；对照表仍看 `01-exception`）  
6. `04-riscv-core/03-memory-bus`（Load 路径闭环）→ `05-bus-rtl/03-axi-ahb`（协议速查）→ `01-spinlock-to-bus`（原子）→ `02-cache`（属性与一致性）  
7. `06-gtm-ip/01-rtl-basics`（RTL 思维打底；写 GTM 前建议先读）  
8. `04` 其余草稿 → 之后 `06-gtm-ip` 其余  
9. `08-ai-soc`：`01-soc-common` → `02-tpu` / `03-gpu`（按兴趣；两路可对照）

---

## 仓库说明

- 中文为主，术语保留英文惯用名。  
- 内容来自 ARM/Linux BSP、RISC-V MCU、FPGA 原型、Secure Boot/HSM 等实战整理。  
- 欢迎纠错；新文请挂索引。
