# RISC-V Debug：Program Buffer 如何把 JTAG 指令交给 Core

> **系列**：`05-bus-rtl`  
> **前置**：[`03-axi-ahb.md`](03-axi-ahb.md)（总线形态）；对 RISC-V 特权级 / Trap 有印象更佳（[`../04-riscv-core/02-csr-trap.md`](../04-riscv-core/02-csr-trap.md)）  
> **相关**：[`../06-gtm-ip/01-rtl-basics.md`](../06-gtm-ip/01-rtl-basics.md)（抓波读法）· [`../02-arch-boot/01-exception.md`](../02-arch-boot/01-exception.md)（`EBREAK` 对照）  
> **平台对照**：Andes D25（`ncepldm200` / AE350 `d25f-se`）↔ OpenC906（ITR 注入）

RISC-V Debug Spec 里常见两条微架构路径：**execution-based**（hart 自己去 Debug Module 窗口取指执行）与 **指令注入**（DM 经专用总线把指令塞进 IFU）。  
本文以 Andes D25 的 Program Buffer 路径为主线，对照 OpenC906 的 ITR，讲清「JTAG 侧写指令字」到「流水线真正执行」之间发生了什么。

**读完应能**：
- 用一张对照表区分 Andes ProgBuf 与 C906 ITR
- 画出 Host → DTM → DM → hart 取指窗口的端到端通路
- 解释 halt 后 PC、`dpc`、Debug ROM / ABSPROG / PROGBUF 各自管什么
- 按 flags 分支说清 park → abstract/progbuf → `ebreak` / `dret` 的控制流
- 避开仿真里 `NDSROM.dat` ≠ Debug ROM、以及 Verdi FSDB 开关的常见坑

---
## 目录

1. [结论先说：拉指令，不是推指令](#1-结论先说拉指令不是推指令)
2. [端到端数据通路](#2-端到端数据通路)
3. [ProgBuf 模式：JTAG 指令如何到 Core](#3-progbuf-模式jtag-指令如何到-core)
4. [Debug ROM 行为（结合 RTL）](#4-debug-rom-行为结合-rtl)
5. [一次 ProgBuf 执行的时序](#5-一次-progbuf-执行的时序)
6. [仿真与波形注意](#6-仿真与波形注意)
7. [相关代码 / 路径索引](#7-相关代码--路径索引)
8. [小结](#小结)
9. [自测](#自测)

---

## 1. 结论先说：拉指令，不是推指令

Andes D25 走的是 **RISC-V Debug Spec 的 execution-based** 路径，**不是** OpenC906 那种 ITR 注入 IFU：

| 项目 | Andes D25 (`ncepldm200`) | OpenC906 |
|------|--------------------------|----------|
| Program Buffer 位置 | DM 内、hart **可取指** 的地址空间 | DM 寄存器 `progbuf[]`，core 不映射取指 |
| 指令如何进流水线 | halt 后 PC 进 **Debug ROM**，再跳到 **ABSPROG / PROGBUF** 正常取指 | `itr` → DTU → `dtu_ifu_debug_inst` 塞进 ibuf |
| 结束方式 | progbuf/abs 末尾 `ebreak` 回 ROM；恢复用 `dret` | `itr_done` 握手，ebreak 结束 pb_work |

**一句话**：JTAG 只负责经 DMI 把指令字写进 DM 的 PROGBUF/控制寄存器；真正「执行」是 **已 halt 的 hart 自己去 DM 窗口取指跑**。

---

## 2. 端到端数据通路

```text
Host Debugger (OpenOCD / TB ext_debugger)
    │  JTAG (TCK/TMS/TDI/TDO)
    ▼
DTM
    │  DMI (APB-like)
    ▼
ncepldm200  (Debug Module)
    │  ① 写 PROGBUF[] / DATA / COMMAND（postexec 等）
    │  ② 写 flags（0x180 等）通知 hart「可以 GO」
    │
    │  hart 总线（AHB/AXI rv_*）
    ▼
CPU Core（Debug Mode）
    取指 PC → PLDM_BASE + DEBUG_ROM
           →（flags==0）jalr → ABSPROG@0x100
           → 再执行 PROGBUF@0x80 中的指令
           → ebreak / dret
```

顶层连接概念（AE350）：

- `ae350_debug_subsystem` 例化 `ncepldm200`
- hart 的 `rv_haddr/hrdata/...`（或 AXI）接到 DM 从机口 → **core 能像访问普通存储器一样读 ROM/PROGBUF/ABSPROG**
- JTAG 侧走 DMI 写同一套寄存器文件

公开头文件内存图（`ncepldm200.h`，相对 `PLDM_BASE`，AE350 上多为 `0xe6800000`）：

```c
typedef struct {
    __I  uint32_t DEBUG_ROM[23];   // 0x00  ~ 0x58   固化 Debug ROM
    __IO uint32_t RESERVED0[9];    // 0x5c  ~ 0x7c
    __IO uint32_t PROGBUF[16];     // 0x80  ~ 0xbc   Program Buffer
    __IO uint32_t DATAREG[12];     // 0xc0  ~ 0xec
    __I  uint32_t ABSPROG[5];      // 0x100 ~ 0x110  Abstract 合成指令区
    // ...
    __IO uint32_t COMMAND;         // 0x120
    __IO uint32_t NOTIFY;          // 0x124
    __IO uint32_t RESUME;          // 0x128
    __IO uint32_t EXCEPTION;       // 0x12c
    // ...
} NCEPLDM200_RegDef;
```

ROM 内 park 还使用实现向的 flags 字（约 `0x180~0x18c`），与头文件命名寄存器互补。

---

## 3. ProgBuf 模式：JTAG 指令如何到 Core

### 3.1 Debugger / JTAG 侧做什么

1. `haltreq` → hart 进 Debug Mode，PC 被导向 Debug ROM（波形可见 `fetch_pc = 0xe6800000+`）。
2. 经 DMI 把要执行的指令写入 **`PROGBUF[i]`**（例如 `csrw mepc, s0` + `ebreak`）。
3. 配置 abstract command（常带 **`postexec`**），或置位 DM 内部 「GO」相关 flags。
4. DM 可在 **ABSPROG** 中填入跳板指令；ROM 通过 `jalr → 0x100` 进入该区，再执行到 PROGBUF。
5. 等待 busy 清除 / 再读结果（DATA 或再 abstract 读寄存器）。

TB 侧：`test_debugger` 用加密的 `ext_debugger_seq.v` 扫 JTAG；**不需要 download Debug ROM**（ROM 在 `ncepldm200` 内固化）。`NDSROM.dat` 只是用户程序镜像，由 TB `$readmemh` 灌 ILM，与 Debug ROM 无关。

### 3.2 Core 侧做什么（execution-based）

1. **`dpc`**：保存「将来 `dret` 回去」的用户 PC，**不是** ROM 里跑的取指 PC。
2. **取指 PC**：落在 `PLDM_BASE + DEBUG_ROM`，IFU **正常取指、译码、执行**。
3. Park 循环读 flags，按值分支：
   - `flags == 0x400` → 恢复 s0/s1 → **`dret`** 回用户；
   - `flags == 0` → 恢复 s0/s1 → **`jalr` 到 `0x100`（ABSPROG）** → 跑 abstract/progbuf → **`ebreak`** 再回 ROM；
   - 其它 → `ebreak`。

因此：**JTAG 并不把指令「推」进流水线**；只是把指令字放到 hart 能取到的 DM 窗口，再敲一下 flags，让 ROM 里的软件自己跳过去取指执行。

### 3.3 和 C906 ITR 的对比（帮助记忆）

```text
Andes:  JTAG → DMI → PROGBUF 寄存器/RAM ──(hart 取指)──► IFU
C906:   JTAG → DMI → progbuf[] → itr 总线 → DTU → debug_inst ──(注入)──► IFU ibuf
```

同一套 ISA 调试语义，两种微架构实现：**拉 vs 推**。

---

## 4. Debug ROM 行为（结合 RTL）

### 4.1 ROM 在 RTL 中的形态

`ncepldm200` 内用组合逻辑 `case` 给出 64-bit 读数据（两拍指令拼在一起），形如：

```verilog
always @* begin
  case ({rv_rd_addr[8:3], 1'b0})
    7'h00: rv_rd_data = {32'h00c0006f, 32'h0180006f}; // +4 指令, +0 指令
    // ...
    7'h1a: rv_rd_data = {32'h00100073, 32'h10000067};
    RV_ADDR_PROGBUF0: rv_rd_data = {dmi_progbuf1, dmi_progbuf0};
    // ... ABSPROG / COMMAND / RESUME ...
  endcase
end
```

注意 case 键是 `{addr[8:3], 1'b0}`：

- 等价于 `case_label = addr[8:3] << 1`（拼接补 LSB 0，不是别处再写 `<<1`）
- **字节偏移 = `case_label * 4`**
- 故 `RV_ADDR_PROGBUF0 = 7'h20` → 偏移 `0x20 * 4 = 0x80`，与 struct 中 PROGBUF 一致
- 若误用 `0x20 * 8 = 0x100` 会对到 ABSPROG，那是算错了

### 4.2 简化控制流

```text
halt → PC 进 DEBUG_ROM
     → 保存 s0/s1 到 dscratch
     → Park: 读 flags / 写应答
           │
           ├─ flags==0x400 → 恢复 → dret → 用户代码
           ├─ flags==0     → 恢复 → jalr 0x100(ABSPROG)
           │                      → abstract / PROGBUF
           │                      → ebreak → 再回 Park
           └─ 其它         → 恢复 → ebreak → Park
```

### 4.3 反汇编要点（相对 `PLDM_BASE`）

| 偏移 | 指令要点 | 含义 |
|------|----------|------|
| 0x00 等 | `jal` 向量 | 多种 halt 入口进 park |
| 0x18/1c | `csrw dscratch0/1, s0/s1` | 腾出临时寄存器 |
| 0x20 | `lw` flags @0x180 | 轮询 DM 命令 |
| 0x30/38 | `beq` | 区分 exec / resume |
| 0x48–58 | ack + 恢复 + **`dret`** | 恢复执行 |
| 0x5c–68 | ack + 恢复 + **`jalr → 0x100`** | 去 ABSPROG 执行调试程序 |
| 0x6c | `ebreak` | 兜底 |

波形上 `fetch_pc` 在 `0xe6800000+` 可验证 ROM 取指。

---

## 5. 一次 ProgBuf 执行的时序

```text
1. Debugger: halt
2. Hart: 进 ROM park，写 halt 应答，轮询 flags
3. Debugger: 写 PROGBUF[0..], 写 abstract cmd (postexec) / 置 flags=0
4. Hart: 发现 flags==0 → jalr ABSPROG
5. Hart: 执行 ABSPROG（及 PROGBUF 中的用户指令）
6. Hart: ebreak → 回到 ROM park
7. Debugger: 读 DATA / 再 abstract 读寄存器核对结果
8. Debugger: 置 flags=0x400 → Hart dret 回用户
```

---

## 6. 仿真与波形注意

| 坑 | 说明 |
|----|------|
| **`NDSROM.dat` ≠ Debug ROM** | 前者是 case 用户程序（灌 ILM）；Debug ROM 在 DM RTL 内固化 |
| **Verdi W-2024.09 FSDB** | 不要再用 `-P novas.tab/pli.a`；需 `export VERDI_HOME`（安装根目录）+ `-debug_access+all`。否则 deprecated ERROR，有 dump 调用也无 `verilog.fsdb` |
| **`fetch_instr==0`** | 常为气泡，需与稳定拍对齐后再反汇编 |

---

## 7. 相关代码 / 路径索引

| 内容 | 路径 |
|------|------|
| DM 寄存器 map | `andes_vip/patterns/samples/include/ncepldm200.h` |
| DM RTL（含 ROM case，常加密） | `andes_ip/soc/ncepldm200/hdl/ncepldm200.v` |
| Debug 子系统 | `andes_ip/ae350/top/hdl/ae350_debug_subsystem.v` |
| 外部 debugger TB | `andes_vip/models/debug/hdl/ext_debugger.v` + `test_debugger/ext_debugger_seq.v` |
| debugger case | `andes_vip/patterns/samples/test_debugger/` |
| 对照：C906 progbuf→ITR | `C906_RTL_FACTORY/.../tdt_dm.v`，`aq_dtu_cdc.v`，`aq_ifu_ibuf.v` |

---

## 小结

- Andes ProgBuf 是 **execution-based**：JTAG 写字，hart **拉**指；C906 是 ITR **推**进 IFU。
- 取指落在 DM 窗口：`DEBUG_ROM` →（flags）→ `ABSPROG` → `PROGBUF`；`dpc` 只管 `dret` 回去的用户 PC。
- Park 靠 flags 分支：`0` 去执行，`0x400` 恢复；progbuf/abs 末尾 `ebreak` 回 ROM。
- RTL 里 ROM/`PROGBUF` 读口用 `case` 拼双字；地址换算用 `case_label * 4`，勿乘成 8。
- 仿真：`NDSROM.dat` 不是 Debug ROM；Verdi 新版本用 `VERDI_HOME` + `-debug_access+all`。

## 自测

1. Andes 与 C906 在「指令如何进流水线」上差在哪一层？
2. halt 后取指 PC 和 `dpc` 各保存什么？混了会怎样理解错？
3. flags 为 `0` / `0x400` / 其它时，ROM park 分别走哪条路径？
4. `RV_ADDR_PROGBUF0 = 7'h20` 对应字节偏移为何是 `0x80` 而不是 `0x100`？
5. 一次 ProgBuf 从 halt 到 `dret`，Debugger 与 Hart 各做哪些关键步骤？
6. 为何 TB 不必 download Debug ROM？`NDSROM.dat` 实际灌到哪里？

---

*`05-bus-rtl` · `04-debug` · ProgBuf / DM*
