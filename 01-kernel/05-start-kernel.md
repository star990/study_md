# Linux 启动：U-Boot 装载与 `start_kernel`

> **系列**：`01-kernel`  
> **前置**：[`../02-arch-boot/03-boot-firmware.md`](../02-arch-boot/03-boot-firmware.md)（ATF (Arm Trusted Firmware)/OpenSBI 链）可先扫一眼  
> **相关**：[`01-scheduling.md`](01-scheduling.md) · [`02-memory.md`](02-memory.md) · [`../03-rtos-bsp/02-bsp-and-boot.md`](../03-rtos-bsp/02-bsp-and-boot.md)

从「U-Boot 把 Image + DTB (Device Tree Blob) 放进内存并跳转」讲到「`start_kernel()` 里各段 init 各干什么」。非 SMP (Symmetric Multi-Processing) 的 init 只勾轮廓；多核 bring-up（`smp_init` / secondary CPU / idle）单独展开。

**读完应能**：
- 说出 U-Boot `booti`/`bootm` 传给内核的关键参数（尤其 DTB 与 bootargs）
- 指出 ARM64 上「关中断 / 建早期页表 / 开 MMU」落在哪些文件与符号
- 按阶段口述 `start_kernel()` → `rest_init()` → pid 1 切到用户态
- 讲清 BSP (Board Support Package) 上 secondary CPU 是怎么被拉起来的（PSCI (Power State Coordination Interface) / SBI (Supervisor Binary Interface) HSM (Hart State Management)）

---

## 1. 总览：从 U-Boot 到能调度

```text
U-Boot
  ├── 读存储：kernel Image (+ 可选 initramfs) + DTB
  ├── 填 bootargs / 修 FDT (Flattened Device Tree)
  └── booti / bootm → 跳到内核入口
        │
        ▼
arch 入口（AArch64: head.S → start_kernel）
  ├── 早期：关中断、建 idmap、开 MMU (Memory Management Unit)（§3 落点）
  ├── setup_arch / 解析 DTB
  ├── start_kernel()：一长串 init（单核上下文）
  └── rest_init() → kernel_init(pid 1) → exec → 用户态 /sbin/init
        │
        └── 途中 smp_init()：把其余 CPU 拉起来
```

嵌入式板子上常见路径是：**BL31/OpenSBI → U-Boot → Linux**。本篇从 U-Boot 装载讲起；更上游的 ATF/OpenSBI 见 `02-arch-boot/03-boot-firmware`。

---

## 2. U-Boot 如何 load kernel 与 device tree

### 2.1 要装进内存的东西

| 镜像 | 常见文件 | 作用 |
|------|----------|------|
| Kernel | `Image` / `Image.gz`（ARM64）；`Image`/`vmlinux`（视平台） | 内核本体 |
| DTB | `*.dtb` | 描述板级硬件，内核靠它 probe 设备 |
| 可选 initramfs | `initrd` / `initramfs.cpio` | 早期用户态根，用于找真正的 rootfs |
| 可选 FIT | `*.itb`（FIT (Flattened Image Tree)） | 把 kernel/DTB/ramdisk 打成一包，带 hash/签名 |

典型嵌入式流程：从 eMMC/SD/网络把 **Image + DTB** 拷到 DRAM 固定地址，再 `booti`。

### 2.2 常用命令：`booti` / `bootm`

**ARM64 裸 Image** 多用 `booti`：

```text
# 伪命令：地址因板而异
load mmc 0:1 ${kernel_addr_r} Image
load mmc 0:1 ${fdt_addr_r} board.dtb
# 可选：load mmc 0:1 ${ramdisk_addr_r} initramfs.cpio.gz

setenv bootargs "console=ttyAMA0,115200 root=/dev/mmcblk0p2 rw"
booti ${kernel_addr_r} - ${fdt_addr_r}
#          Image         无 initrd   DTB
# booti <kernel> [initrd] <fdt>
```

**uImage / FIT** 多用 `bootm`（会解析头部、校验、解压）。FIT 时一条 `bootm` 可带上 kernel+fdt+ramdisk。

### 2.3 传给内核的约定（ARM64）

U-Boot 跳转前按 **ARM64 Linux boot protocol** 准备寄存器（概念上）：

| 寄存器 | 内容 |
|--------|------|
| `x0` | DTB 物理地址（最重要） |
| `x1`–`x3` | 保留，须为 0 |

同时：
- **bootargs** 写进 FDT 的 `/chosen/bootargs`（或等价路径），内核 `setup_arch` 时读出变成 `command_line`
- 内存布局、stdout-path、initrd-start/end 等也可由 U-Boot 改 FDT 再交给内核
- 内核入口处仍是 **物理地址执行**；开 MMU、切到虚拟地址在 `head.S` / early setup 里完成

RISC-V 平台常见是 OpenSBI → U-Boot（S-mode）→ Linux；DTB 同样经 bootloader 传入，多核后续用 **SBI HSM** 而不是 PSCI——对照见固件篇。

### 2.4 U-Boot 侧还顺手做的几件事

- 选 boot 介质、验签（若 Secure Boot）
- 解压 `Image.gz`（若未用内核自解压路径）
- `fdt addr` / `fdt resize` / `fdt set`：改内存节点、bootargs、status
- 关闭或移交自己占用的外设，避免和内核抢同一块硬件

**本节要点**：U-Boot 的核心交付物是「**可执行的内核映像 + 一份描述本板的 DTB（含 bootargs）**」；`booti`/`bootm` 只是按协议跳进去。

---

## 3. 进入内核：到 `start_kernel` 之前（ARM64 落点）

下面以 **主线 arm64**（`arch/arm64/kernel/head.S` + `arch/arm64/kernel/pi/`）为准；版本演进后符号名可能微调，但阶段不变。RISC-V 对应入口在 `arch/riscv/kernel/head.S`，语义同类。

### 3.1 调用链（boot CPU）

```text
U-Boot booti
  → Image 头部：b primary_entry          arch/arm64/kernel/head.S
       │
       ├─ record_mmu_state / preserve_boot_args   保存 x0=FDT 等
       ├─ 设 early_init_stack
       ├─ __pi_create_init_idmap                  建初始 identity map
       │     └─ create_init_idmap()               arch/arm64/kernel/pi/map_range.c
       ├─ init_kernel_el                          落到内核运行的 EL，MMU 仍关
       ├─ __cpu_setup                             arch/arm64/mm/proc.S（TCR (Translation Control Register)/MAIR (Memory Attribute Indirection Register) 等）
       └─ __primary_switch
             ├─ __enable_mmu                      head.S：TTBR + SCTLR.M=1
             ├─ __pi_early_map_kernel             映射内核 VA / 清 BSS / 可读 FDT
             │     └─ early_map_kernel()          arch/arm64/kernel/pi/map_kernel.c
             └─ __primary_switched
                   ├─ 挂上 init_task 栈、VBAR_EL1=vectors
                   └─ bl start_kernel             init/main.c
```

### 3.2 「关中断 / 建页表 / 开 MMU」具体在哪

| 步骤 | 主要文件 | 符号 / 机制 | 说明 |
|------|----------|-------------|------|
| 关 IRQ (Interrupt Request)（约定 + 状态） | boot protocol + `head.S` | 进入时 DAIF (Debug, SError, IRQ, FIQ mask) 应已屏蔽；`init_kernel_el` 用 `INIT_PSTATE_EL1` / `INIT_SCTLR_EL*_MMU_OFF` 把 CPU 收到可控状态 | 不是单独一个叫 `disable_irq` 的早期函数；C 里 `start_kernel()` 开头仍会 `local_irq_disable()` 再保险 |
| 建临时 / 早期页表 | `arch/arm64/kernel/pi/map_range.c` | `__pi_create_init_idmap` → `create_init_idmap()` | 填 `init_idmap_pg_dir`：把内核镜像做 **identity map**（VA=PA），供开 MMU 瞬间取指 |
| 处理器翻译控件 | `arch/arm64/mm/proc.S` | `__cpu_setup` | 配好 TCR、MAIR 等，为开 MMU 做准备 |
| 打开 MMU | `arch/arm64/kernel/head.S` | `__primary_switch` → `__enable_mmu` | 写 `TTBR0_EL1`（idmap）、`TTBR1_EL1`（先用 `reserved_pg_dir`），再 `SCTLR_EL1.M=1` |
| 建内核虚拟映射 | `arch/arm64/kernel/pi/map_kernel.c` | `__pi_early_map_kernel` → `early_map_kernel()` | MMU 已开、仍跑在 idmap 上时，映射 `KIMAGE_VADDR` 等，并可映射 FDT、清 BSS |
| 切到「正经」内核上下文 | `head.S` | `__primary_switched` | `init_cpu_task(init_task)`、`msr vbar_el1, vectors`、保存 `__fdt_pointer`，再 `bl start_kernel` |

记忆口诀：**`create_init_idmap` 铺路 → `__enable_mmu` 开门 → `early_map_kernel` 把家搬到高位 VA → `__primary_switched` 进 C**。

Secondary CPU 不重跑整条 primary 链：`secondary_entry` / `secondary_startup`（同 `head.S`）里 `__enable_mmu` 后进 `__secondary_switched` → `secondary_start_kernel()`（`arch/arm64/kernel/smp.c`）。

随后进入通用路径 `start_kernel()`（`init/main.c`）。下面按**阶段**扫一遍；记职责顺序即可。

---

## 4. `start_kernel()`：init 地图（点到为止）

可以把 `start_kernel` 想成一条流水线：**先让「能打印、能分配、能认硬件」成立，再拉多核，最后把 CPU0 变成 idle 并拉起 init 进程**。

```text
start_kernel()
  │
  ├─ 极早期：set_task_stack_end_magic、smp_setup_processor_id、debug/early boot
  ├─ boot_cpu_init / page_address_init
  ├─ setup_arch()          ← 解析 DTB、memblock、早期控制台、arch 中断控制器线索
  ├─ mm_init_cpumask / build_all_zonelists / page_alloc_init
  ├─ 解析 command_line、setup_nr_cpu_ids、smp_prepare_boot_cpu
  ├─ 调度器早期：sched_init()
  ├─ 软中断 / RCU (Read-Copy-Update) / 定时器：softirq、rcu、init_timers、hrtimers、timekeeping
  ├─ 控制台：console_init()（此后 printk 更「正经」）
  ├─ 内存后半：mem_init、kmem_cache、vma/vmalloc 等
  ├─ VFS (Virtual File System) / 信号 / 进程相关：vfs_caches_init、signals、proc 等
  ├─ 驱动模型骨架：driver_init、（稍后才大量 probe）
  ├─ 延迟初始化：initcalls 队列准备
  ├─ 多核：smp_init() / sched_init_smp()     ← 下文重点
  ├─ 其余子系统：acpi/of、irq、cgroup、…（平台相关）
  └─ rest_init()
        ├─ kernel_clone(kernel_init)    → pid 1：initcalls → 挂 root → exec /sbin/init
        ├─ kernel_thread(kthreadd)      → pid 2
        └─ cpu_startup_entry()          → CPU0 进入 idle（init_task）
```

### 4.1 各段在干什么（一句话版）

| 阶段 | 代表调用 | 一句话 |
|------|----------|--------|
| Arch / DT | `setup_arch` | 吃掉 DTB：内存、chosen、机器 ID、早期 IRQ 芯片信息 |
| 内存 | `mm_init` / buddy·slab 相关 | 从 memblock 走到伙伴系统与 slab，之后才能正常 `kmalloc` |
| 调度 | `sched_init` | 建 runqueue、CFS (Completely Fair Scheduler) 等 class；此时仍基本是单核语义 |
| 时间 | `timekeeping` / timer / hrtimer | 节拍与时钟源；没有它就没有 `jiffies`、超时、调度 tick |
| 同步 | RCU、irq、softirq | 内核并发原语可用；中断子系统可注册 |
| 可见性 | `console_init` | 串口 console 挂上，bring-up 日志可读 |
| 文件系统骨架 | VFS caches、proc 等 | 为后面挂根、创建设备节点打底 |
| 设备模型 | `driver_init` 等 | bus/class/device 框架就位；大量设备要等 initcall / 晚些 probe |
| 多核 | `smp_init` | 枚举并 online 其余 CPU（§6） |
| 收尾 | `rest_init` | 创建 `kernel_init`/`kthreadd`，boot CPU 变 idle（§5） |

**不必背每个函数名**。板级 bring-up 时更常卡在：DTB 错、earlycon/console 不对、内存节点不对、然后才是驱动 probe——那些属于后续 initcall / 驱动阶段，不在本篇展开。

`start_kernel` 本身多半在 **只有 boot CPU 跑 C 代码** 的环境下走完大半；`smp_init` 把其它核 online 后，调度与中断才变成真正的多核系统。首进程与切用户态见下一节。

---

## 5. 第一个进程：从 `init_task` 到用户态 `/sbin/init`

这里有三层容易混在一起的「第一」：

| 角色 | 是谁 | pid | 文件 / 创建方式 |
|------|------|-----|----------------|
| 最早的 `task_struct` | `init_task`（常叫 swapper / idle） | **0** | 静态：`init/init_task.c`；`__primary_switched` 里挂上栈 |
| 第一个「真正会被当成 init 的任务」 | `kernel_init` | **1** | `rest_init()` → `kernel_clone({.fn = kernel_init})`（`init/main.c`） |
| 内核线程工厂 | `kthreadd` | **2** | `rest_init()` → `kernel_thread(kthreadd, …)` |

Boot CPU 跑完 `start_kernel()` 末尾进入 `rest_init()` 后：**自己变成 idle（继续用 `init_task`）**，并拉起 pid 1 / pid 2。

```text
rest_init()                          init/main.c
  ├─ kernel_clone(kernel_init)  →  pid 1（此时仍是内核线程）
  ├─ kernel_thread(kthreadd)    →  pid 2
  └─ cpu_startup_entry()        →  CPU0 进 idle 循环（init_task）
```

### 5.1 pid 1 在内核里干什么（`kernel_init`）

`kernel_init()` 大致顺序：

1. 等 `kthreadd` 就绪（`wait_for_completion(&kthreadd_done)`）
2. `kernel_init_freeable()`：跑完剩余 bring-up（含 `do_basic_setup()` / initcalls、挂根等）
3. 释放 `__init` 内存、`system_state = SYSTEM_RUNNING`
4. `run_init_process(...)` → **`kernel_execve()`**，把「内核线程 pid 1」变成「用户态 init」

候选路径（谁先成功用谁）：`rdinit=` / `init=` 参数 → `CONFIG_DEFAULT_INIT` → `/sbin/init` → `/etc/init` → `/bin/init` → `/bin/sh`。

### 5.2 怎么切到用户态？

关键点：**不是另起一个 pid，而是对当前 pid 1 做 exec**。

```text
kernel_init()  （内核线程，EL1，mm 多为内核视角）
  │
  └─ run_init_process("/sbin/init")
        └─ kernel_execve(filename, argv, envp)     fs/exec.c 路径上
              ├─ 打开可执行文件，解析 ELF (Executable and Linkable Format)
              ├─ 建立用户地址空间（mm_struct / 页表）
              ├─ 映射代码/数据/堆栈，准备 argv/envp、辅助向量
              ├─ 把当前任务的 pt_regs 改成「返回用户态」的样子：
              │     PC  = ELF 入口（或动态链接器入口）
              │     SP  = 用户栈
              │     PSTATE/SPSR = EL0t（用户态）
              └─ 返回后并不「再跑 kernel_init 后半段」
                    │
                    ▼
              调度路径上的 ret_to_user / 异常返回
                    │  ARM64: ERET → EL0
                    │  RISC-V: sret → U-mode
                    ▼
              用户态 /sbin/init 开始执行（仍是 pid 1）
```

直觉：

- **`kernel_clone`**：新建一个内核上下文里的任务（还没有用户页表上的「程序」）。
- **`kernel_execve`**：用用户 ELF **覆盖**这个任务的执行映像，并布置好「下一次返回时落到 EL0」。
- **真正降特权**发生在该任务从内核返回用户时的 **`ERET`/`sret`**（和普通进程 syscall 返回是同一条「回用户」管道），不是 `start_kernel` 里某条特殊指令。

若 exec 全失败，内核会 `panic("No working init found...")`——板级上常见原因是 rootfs/initramfs 没挂上或路径不对，而不是 `head.S` 有问题。

**本节要点**：静态的 `init_task` 是 idle；**pid 1 先是内核线程 `kernel_init`，再经 `kernel_execve` + 异常返回进入用户态**，pid 不变。

---

## 6. 重点：SMP 启动

### 6.1 谁是 boot CPU，谁是 secondary

| 角色 | 含义 |
|------|------|
| **Boot CPU**（通常 CPU0） | 从 U-Boot 跳进来，跑完 `start_kernel` 绝大部分 |
| **Secondary CPU** | 上电后往往在 WFI (Wait For Interrupt)/固件停车场睡觉，等 OS 发「开机」 |

设备树里常见 `cpu` 节点 + `enable-method`（如 `psci`），告诉内核「怎么唤醒这颗核」。

### 6.2 软件主路径

概念顺序（名称随版本略有出入）：

```text
setup_arch / of_parse …
  → 发现 cpu 节点，nr_cpu_ids、possible/present mask

smp_prepare_cpus()
  → 为每个可能 online 的 CPU 准备栈、idle task、per-cpu 数据

smp_init()
  → 对每个 secondary：cpu_up() / bringup
       → arch: boot_secondary()
            → ARM64: 常走 PSCI CPU_ON（进 EL3/ATF）
            → RISC-V: 常走 SBI HSM hart_start（进 OpenSBI）
       → secondary 从 arch 入口跑 secondary_start_kernel
            → 开 MMU、建异常栈、通知 boot CPU「我好了」
            → 进入 cpu_startup_entry() → idle

sched_init_smp()
  → 调度域、负载均衡拓扑（把多核真正纳入调度器）
```

Secondary 入口**不会**再跑一遍完整的 `start_kernel()`；只做「本核私有」初始化，然后进 idle，等调度器派任务过来。

### 6.3 硬件/固件怎么「叫醒」一颗核（ARM64）

```text
Linux (EL1) ──SMC──► ATF BL31 (EL3)
                        │ PSCI CPU_ON(target_cpu, entry_point, context)
                        ▼
                   把 secondary 从停车场释放
                   令其跳到内核准备好的 secondary entry
                        │
                        ▼
                   secondary_start_kernel → idle
```

要点：
- **入口地址、栈** 必须由主核事先写好；固件只负责按调用约定把核送到该地址
- GIC (Generic Interrupt Controller) 对 secondary 的分发、PPI/SGI 等要在该核 online 路径里就绪，否则「核起来了但中断不对」
- big.LITTLE / 多 cluster 时，还要关心 cache 一致性域、电源域是否已被 ATF/板级固件点亮

### 6.4 RISC-V 对照

| | ARM64 | RISC-V Linux |
|--|-------|----------------|
| 唤醒接口 | PSCI `CPU_ON` | SBI HSM `hart_start` |
| 固件 | ATF BL31 | OpenSBI（M-mode） |
| 调用方式 | `SMC` | `ECALL` |
| 目标 | 某 CPU ID + entry | 某 hartid + addr |

语义相同：**主 hart 跑完早期 init → 固件把其它 hart 送到内核 secondary 入口**。

### 6.5 和调度、idle 的衔接

- 每个 CPU 有自己的 **idle task**；secondary online 后第一件事往往是跑 idle，直到有 task 被 migrate / 唤醒过来
- `sched_init_smp()` 建好调度域后，CFS/RT 才按多核拓扑做负载均衡（细节见 [`01-scheduling.md`](01-scheduling.md)）
- bring-up 失败时的典型现象：只有 CPU0 在 `/proc/cpuinfo`、某核卡在 `online`、或 early 就 panic 在 `cpu_up`——优先查 **DT `enable-method`、PSCI/SBI 是否通、secondary 入口/栈、电源与 cache 维护**

### 6.6 AMP 旁路（知道即可）

若一颗核交给 RTOS (Real-Time Operating System)/裸机（AMP (Asymmetric Multi-Processing)），Linux **不会**对它做 `cpu_up`；设备树里可能 `status = "disabled"` 或根本不归 Linux 管。那是产品分区问题，不是 `smp_init` 失败。

**本节要点**：SMP 启动 = **主核准备 per-cpu 资源 + 固件协议拉核 + secondary 短路径进 idle + 调度器建多核拓扑**；板级最容易错的是 DT/PSCI（或 SBI HSM）与电源，而不是 `start_kernel` 里某一个普通 init 函数。

---

## 7. 和前后文怎么串

| 你卡在… | 更该看 |
|---------|--------|
| BL31 / OpenSBI / 谁先谁后 | [`../02-arch-boot/03-boot-firmware.md`](../02-arch-boot/03-boot-firmware.md) |
| DTB 节点、platform 驱动 probe | [`../03-rtos-bsp/02-bsp-and-boot.md`](../03-rtos-bsp/02-bsp-and-boot.md) |
| 起来之后 task 怎么选核跑 | [`01-scheduling.md`](01-scheduling.md) |
| memblock → 伙伴/页表/DMA | [`02-memory.md`](02-memory.md) |

---

## 小结

- U-Boot 负责把 **kernel 映像 + DTB（含 bootargs）** 放到约定地址，并用 `booti`/`bootm` 按 arch boot protocol 跳转（ARM64 上 DTB 在 `x0`）。
- ARM64 早期：`create_init_idmap`（`pi/map_range.c`）→ `__enable_mmu`（`head.S`）→ `early_map_kernel`（`pi/map_kernel.c`）→ `__primary_switched` → `start_kernel`。
- `start_kernel()` 是单核主路径上的「子系统总装配」；真正多核从 `smp_init` / `sched_init_smp` 开始。
- **`init_task` = pid 0 idle**；**pid 1 = `kernel_init` 内核线程**，再经 **`kernel_execve` + `ERET`/`sret`** 变成用户态 `/sbin/init`。
- Secondary CPU 由 **PSCI CPU_ON（ARM）或 SBI HSM hart_start（RISC-V）** 唤醒，跑短路径后进 idle。

## 自测

1. `booti` 三个参数分别是什么？没有 initrd 时中间怎么写？
2. ARM64 跳进内核时，DTB 物理地址通常放在哪个寄存器？bootargs 一般从哪读到？
3. 建初始 idmap、打开 MMU、映射内核 VA，分别主要落在哪个文件/符号？
4. `init_task`、`kernel_init`、`/sbin/init` 三者 pid 与关系是什么？谁负责降到 EL0？
5. 为什么说 secondary CPU 不会完整执行 `start_kernel()`？
6. 画出 CPU0 调用 PSCI `CPU_ON` 到 secondary 进入 idle 的步骤（可含 ATF）。
7. RISC-V 上对应的拉核接口叫什么？走哪条指令进固件？

---

*`01-kernel` · U-Boot 与 start_kernel*
