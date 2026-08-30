# 固件启动链：ATF 与 OpenSBI / Secure Boot 衔接

> **系列**：`02-arch-boot`  
> **前置**：[`01-exception.md`](01-exception.md)  
> **相关**：[`04-security-secureboot-tee.md`](04-security-secureboot-tee.md) · [`../03-rtos-bsp/02-bsp-and-boot.md`](../03-rtos-bsp/02-bsp-and-boot.md)

BL1→BL31→TEE→OS 的职责切分，SMC/PSCI 与 RISC-V SBI 的对应，以及证书链在启动中的位置——为下一篇 Security 全文打底。

**读完应能**：
- 按 stage 口述 ATF 链
- 对照 OpenSBI / SBI 扩展
- 指出 Secure Boot 验签落在哪一级

---
## ATF 启动链

## 3.1 ATF 是什么 — 对照 OpenSBI / RISC-V Boot

| | ARM ATF | RISC-V (M100/M200) |
|--|---------|---------------------|
| 项目 | ARM Trusted Firmware-A | **OpenSBI**（开源 M-mode 固件） |
| 运行级别 | EL3 (Monitor) | **M-mode** |
| Boot 阶段 | BL1→BL2→BL31→BL32→BL33 | BootROM→2nd Loader→**OpenSBI**→U-Boot/App |
| Secure 服务 | SMC → OP-TEE | ECALL → **SBI** |
| 电源管理 | **PSCI** | **SBI HSM**（Hart Start/Stop） |
| 多核启动 | BL31 PSCI CPU_ON | OpenSBI **HSM** + **sbi_hart_start** |

---

## 3.2 Boot Stage 总览 — ARM vs RISC-V

```
ARM ATF:                              RISC-V (Linux 平台):
─────────                             ─────────────────────
Reset                                 Reset
  │                                     │
BL1 (ROM, EL3)                        BootROM (M-mode, 片上 ROM)
  │ 验签 BL2                            │ 加载 2nd stage
  ▼                                     ▼
BL2 (EL3)                             2nd Stage Loader (M-mode)
  │ 加载 BL31/BL32/BL33                 │ 加载 OpenSBI + U-Boot/Kernel
  ▼                                     ▼
BL31 (EL3, 常驻 Monitor)              OpenSBI (M-mode, 常驻)
  │ PSCI/SMC                            │ SBI 服务 (ECALL)
  ├─ BL32 OP-TEE (Secure EL1)          │ (可选 vendor TEE)
  └─ BL33 U-Boot (NS EL1)              └─ U-Boot (S-mode) 或裸机 App
       │                                     │
Linux (NS EL1)                        Linux (S-mode) 或 M-mode 裸机
```

**你 M100 实际 Boot（更简单）**：
```
Reset → BootROM (M-mode) → 验签 → 加载 Flash App → M-mode 裸机运行
         │                        │
         HSM Secure Boot            无 BL31/OpenSBI（纯 MCU）
         eFuse key                  PMP 保护
```

| ARM Stage | RISC-V Linux 平台 | RISC-V MCU (M100) |
|-----------|-------------------|-------------------|
| BL1 | BootROM | **BootROM** |
| BL2 | FSBL / 2nd loader | BootROM 直接加载 |
| BL31 | **OpenSBI** | 无（不需要） |
| BL32 | OP-TEE（可选） | 无 |
| BL33 | U-Boot | 无（直接 App） |
| Linux | Linux (S-mode) | 无 OS |

---

## 3.3 各 Stage 详细职责 — 带 RISC-V 对照

### BL1 ↔ BootROM (M-mode)

| 工作 | ARM BL1 | RISC-V BootROM (M100) |
|------|---------|----------------------|
| 特权级 | EL3 | **M-mode** |
| PLL/clock | ✓ | ✓ |
| eFuse | ✓ 读 key/lifecycle | ✓ HSM key |
| Cache | ICache/DCache | ICache/DCache |
| 验签 | BL2 证书链 | App image 签名（Secure Boot） |
| Boot 介质 | eMMC/UFS/SPI/XIP | **Flash XIP**（M100 boot from flash） |
| Console | UART | UART |
| 跳转 | → BL2 | → Flash App |

### BL31 ↔ OpenSBI

| 功能 | ARM BL31 | RISC-V OpenSBI |
|------|----------|----------------|
| 常驻 | EL3 不退出 | M-mode 不退出 |
| 跨特权调用 | **SMC** 分发 | **ECALL → SBI** 分发 |
| CPU 上下电 | **PSCI** CPU_ON/OFF | **SBI HSM** hart start/stop |
| 定时器 | ARM Arch Timer | **SBI TIME** set_timer |
| IPI | GIC SGI | **SBI IPI** send |
| 系统复位 | PSCI SYSTEM_RESET | **SBI SRST** system_reset |
| TEE | 转发 SMC 给 OP-TEE | vendor 扩展 SBI |

### SMC ↔ ECALL / SBI

| ARM | RISC-V |
|-----|--------|
| `SMC #0` 指令 | **`ECALL`** 指令 |
| SMC Function ID (x0) | SBI Extension ID (a7) + Function ID (a6) |
| PSCI 0x84000000+ | SBI Base + HSM 0x48534D |
| OP-TEE 0x32000000+ | vendor SBI 扩展 |
| BL31 处理 | **OpenSBI** 处理 |
| 返回：`ERET` | 返回：`mret` |

```c
/* ARM Linux 调 PSCI */
arm_smccc_smc(PSCI_0_2_FN_CPU_ON, cpuid, entry, context, 0, 0, 0, 0, &res);

/* RISC-V Linux 调 SBI */
sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_START, hartid, addr, 0, 0, 0, 0);
```

### BL32 (OP-TEE) ↔ RISC-V TEE

| ARM | RISC-V |
|-----|--------|
| OP-TEE OS (Secure EL1) | **无标准 TEE**；Keystone/Eyrie 等研究项目 |
| CA → TA via SMC | 无标准 CA/TA 模型 |
| M100 HSM | 直接在 **M-mode** 调 HSM 寄存器，无需 TEE |

---

## 3.4 完整启动时序 — 三平台对照

```
ARM (Secure Boot):          RISC-V Linux:              RISC-V MCU (M100):
BL1→BL2→BL31→TEE→UBoot    BootROM→Loader→OpenSBI     BootROM→验签→Flash App
  →Linux                     →UBoot→Linux               (M-mode 裸机)
```

验签流程**原理相同**（你两个平台都做过）：
```
读 eFuse root key hash → 验证书公钥 → 验 image 签名 → rollback 检查
ARM: BL1 验 BL2              RISC-V M100: BootROM 验 App
```

---

## 3.5 证书链 — 两平台通用

证书链、X.509、Anti-rollback 概念 **ARM 和 RISC-V 完全相同**，只是载体不同：

| | ARM | RISC-V M100 |
|--|-----|-------------|
| Root key | eFuse hash | eFuse / OTP |
| 验签位置 | BL1 (EL3) | BootROM (M-mode) |
| 签名算法 | RSA-4096/ECDSA | RSA/ECDSA/SM2（HSM 硬件） |
| Rollback | eFuse counter | eFuse counter |

---

## 3.6 PSCI ↔ SBI 对照

| 功能 | ARM PSCI (SMC) | RISC-V SBI (ECALL) |
|------|----------------|---------------------|
| 查版本 | PSCI_VERSION 0x84000000 | sbi_get_spec_version |
| CPU 启动 | CPU_ON 0xC4000003 | **HART_START** |
| CPU 关闭 | CPU_OFF 0x84000002 | **HART_STOP** |
| CPU 休眠 | CPU_SUSPEND 0x84000001 | **HART_SUSPEND** |
| 查 CPU 状态 | AFFINITY_INFO 0xC4000004 | **HART_STATUS** |
| 关机 | SYSTEM_OFF 0x84000008 | **SRST_SYSTEM_SHUTDOWN** |
| 重启 | SYSTEM_RESET 0x84000009 | **SRST_SYSTEM_RESET** |
| 定时器 | ARM Arch Timer | **SBI_TIME_SET_TIMER** |
| IPI | GIC SGI | **SBI_IPI_SEND_IPI** |

Linux 内核：
- ARM：`kernel/arm64/kernel/psci.c` → `cpu_psci_ops`
- RISC-V：`kernel/riscv/sbi.c` → `sbi_ecall`

---

## 3.7 SMC Calling Convention ↔ SBI Extension

### ARM SMC ID 编码
```
Bit[31]: SMC32/SMC64
Bit[23:16]: Service (0x80=OP-TEE, 0x84=PSCI)
Bit[15:0]: Function Number
```

### RISC-V SBI Extension ID
```
a7 = Extension ID:
  0x10 = Base
  0x4442434E = DBCN (Debug Console)
  0x48534D = HSM (Hart State Management)
  0x735049 = IPI
  0x54494D45 = TIME
  0x53525354 = SRST (System Reset)
a6 = Function ID within extension
```

---

## 3.8 ATF 与 Zynq / FPGA — 及 M100 FPGA 对照

| 阶段 | ARM Zynq | RISC-V M100 FPGA |
|------|----------|------------------|
| 第一级 | BootROM/FSBL | BootROM |
| 安全固件 | ATF BL31 | 无（M-mode 直接） |
| 加载 OS | U-Boot → Linux | 直接 Flash App |
| Debug | JTAG + hw_server | **OpenOCD + GDB + VCS** |
| PL/FPGA | Zynq PL bitstream | M200 FPGA 逻辑 |
| 验证重点 | PS+PL 联调 | Core/IP/Secure Boot |

---

---

## 小结

- ATF 用 BL 阶段切分 ROM→常驻运行时→OS；RV 常用 BootROM + OpenSBI 扮演相近角色。
- BL31 / OpenSBI 常驻，是 SMC/SBI 服务与 PSCI/HART 控制的落点。
- 验签与证书链跨平台原理相近，落点在早期 BL/ROM。
- 下一篇 Security 在这条链上展开 RoT、eFuse 与 TEE。

## 自测

1. BL1 / BL31 / BL33 各自职责？RV 对照物是什么？
2. 为何 BL31（或 OpenSBI）需要常驻？
3. PSCI CPU_ON 与 SBI HART_START 的关系？
4. SMC 与 ECALL 在特权跳转上如何类比？
5. Secure Boot 验签通常卡在启动链的哪一段？

---

*`02-arch-boot` · 固件启动链*
