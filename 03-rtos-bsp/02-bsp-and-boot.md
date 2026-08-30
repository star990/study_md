# 芯片 BSP：Zynq 启动、Device Tree 与驱动模型

> **系列**：`03-rtos-bsp`  
> **前置**：[`01-freertos-realtime.md`](01-freertos-realtime.md) 可并行阅读  
> **相关**：[`../02-arch-boot/03-boot-firmware.md`](../02-arch-boot/03-boot-firmware.md)

从「拿到一颗芯片」到「外设能跑」：启动链、设备树、platform 驱动框架，以及 GPIO (General-Purpose Input/Output)/UART (Universal Asynchronous Receiver/Transmitter)/SPI (Serial Peripheral Interface)/I2C (Inter-Integrated Circuit) 与 AXI (Advanced eXtensible Interface) 家族速记，并对照 FPGA 原型阶段的工作内容。

**读完应能**：
- 口述 Zynq/FSBL 与 ATF (Arm Trusted Firmware) 的对应
- 写清 DT 匹配到 probe 的路径
- 区分 AXI-Lite 与 Full 的直觉差异

---
## 芯片 BSP 与 Zynq 启动

## 2.1 BSP 是什么

**Board Support Package（板级支持包）**：

```
BSP = Bootloader + Kernel + Device Tree + RootFS + 驱动

职责：
  1. 让硬件跑起来（boot chain）
  2. 让 OS 认识硬件（Device Tree）
  3. 让外设能用（驱动）
  4. 让系统稳定（debug/测试）
```

### 芯片公司 BSP 工作内容

| 阶段 | 工作 |
|------|------|
| **FPGA 原型** | Zynq PS + PL 联调，验证 boot/DDR/外设 |
| **硅前** | 基于 FPGA 优化 boot 时间、驱动框架 |
| **回片** | Bring-up：第一笔代码跑通 |
| **量产** | 稳定性、性能、Security sign-off |

---

## 2.2 Zynq 启动流程

```
Power On
  │
  ▼
BootROM (OCM, On-Chip Memory)
  ├── 读 BOOT_MODE 引脚 (QSPI/SD/NOR/JTAG)
  ├── 加载 FSBL (First Stage Boot Loader)
  └── 跳转 FSBL
  │
  ▼
FSBL (OCM/SRAM)
  ├── Init PS 子系统 (clock/DDR/MIO)
  ├── 加载 bitstream 到 PL (可选)
  ├── 加载 SSBL (U-Boot/ATF)
  └── 跳转 SSBL
  │
  ▼
ATF BL31 (若 Secure Boot)
  ├── 加载 OP-TEE (BL32)
  └── 跳转 U-Boot (BL33)
  │
  ▼
U-Boot (DDR)
  ├── 初始化外设
  ├── 加载 kernel Image + DTB
  ├── 可选：加载 initramfs
  └── bootm → 跳转 kernel
  │
  ▼
Linux Kernel
  ├── early init
  ├── 解析 DTB
  ├── 初始化 GIC/UART/eMMC...
  └── 挂载 rootfs → init
```

### 与 ATF 标准链的对应

| Zynq 传统 | ATF 标准 | 说明 |
|-----------|----------|------|
| BootROM | BL1 | 芯片固化 |
| FSBL | BL2 | Xilinx 提供 |
| ATF BL31 | BL31 | Secure Monitor |
| OP-TEE | BL32 | TEE（若需要） |
| U-Boot | BL33 | Non-trusted FW |
| Linux | Linux | NS EL1 |

---

## 2.3 Device Tree（设备树）

### 为什么需要 Device Tree

```
ARM 平台硬件差异大 → 不能编译进 kernel
→ 用 DTB（Device Tree Blob）描述硬件
→ Bootloader 加载 DTB 传给 kernel
→ Kernel 解析 DTB，匹配驱动
```

### 基本结构

```dts
/dts-v1/;

/ {
    compatible = "xlnx,zynqmp-zcu102", "xlnx,zynqmp";
    #address-cells = <2>;
    #size-cells = <2>;

    cpus {
        cpu@0 {
            compatible = "arm,cortex-a53";
            device_type = "cpu";
            reg = <0x0 0x0>;
            enable-method = "psci";
        };
    };

    memory@0 {
        device_type = "memory";
        reg = <0x0 0x0 0x0 0x80000000>;  /* 2GB DDR */
    };

    gic: interrupt-controller@f9010000 {
        compatible = "arm,gic-v3";
        #interrupt-cells = <3>;
        reg = <0x0 0xf9010000 0x0 0x10000>,
              <0x0 0xf9020000 0x0 0x80000>;
        interrupt-controller;
    };

    uart0: serial@ff000000 {
        compatible = "xlnx,xuartps", "cdns,uart-r1p12";
        reg = <0x0 0xff000000 0x0 0x1000>;
        interrupts = <GIC_SPI 21 IRQ_TYPE_LEVEL_HIGH>;
        clock-frequency = <99999000>;
    };
};
```

### 驱动匹配

```c
/* 驱动中 */
static const struct of_device_id my_uart_of_match[] = {
    { .compatible = "xlnx,xuartps" },
    { .compatible = "cdns,uart-r1p12" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, my_uart_of_match);

static struct platform_driver my_uart_driver = {
    .probe  = my_uart_probe,
    .remove = my_uart_remove,
    .driver = {
        .name = "my_uart",
        .of_match_table = my_uart_of_match,
    },
};

/* probe 中获取资源 */
static int my_uart_probe(struct platform_device *pdev)
{
    struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    int irq = platform_get_irq(pdev, 0);
    void __iomem *base = devm_ioremap_resource(&pdev->dev, res);
    ...
}
```

---

## 2.4 Linux 驱动模型

### 驱动分类

| 类型 | 框架 | 例子 |
|------|------|------|
| **Platform** | platform_driver | UART, SPI, I2C controller |
| **Char** | cdev/file_operations | /dev/mydev |
| **Block** | gendisk/request_queue | eMMC, SD |
| **Network** | net_device | Ethernet |
| **Misc** | miscdevice | 简单字符设备 |

### Platform Driver 框架

```c
/* 1. 定义 file_operations（若需要用户态接口） */
static struct file_operations my_fops = {
    .owner          = THIS_MODULE,
    .open           = my_open,
    .release        = my_release,
    .read           = my_read,
    .write          = my_write,
    .unlocked_ioctl = my_ioctl,
};

/* 2. probe：初始化硬件 */
static int my_driver_probe(struct platform_device *pdev)
{
    /* 获取 DT 资源 */
    /* 映射寄存器 ioremap */
    /* 注册中断 request_irq */
    /* 初始化硬件 */
    /* 注册 char device */
    return 0;
}

/* 3. remove：释放资源 */
static int my_driver_remove(struct platform_device *pdev)
{
    free_irq(...);
    iounmap(...);
    return 0;
}

/* 4. 注册 platform_driver */
module_platform_driver(my_driver);
```

### 中断处理

```c
static irqreturn_t my_irq_handler(int irq, void *dev_id)
{
    struct my_device *dev = dev_id;
    u32 status = readl(dev->base + STATUS_REG);

    if (status & RX_READY) {
        /* 处理接收 */
        return IRQ_HANDLED;
    }
    return IRQ_NONE;
}

/* 注册 */
request_irq(irq, my_irq_handler, IRQF_TRIGGER_RISING,
            "my_device", dev);
```

---

## 2.5 常见硬件接口

### GPIO

```c
/* 内核 GPIO 框架 */
gpio_request(gpio_num, "my_gpio");
gpio_direction_output(gpio_num, 1);  /* 输出高 */
gpio_set_value(gpio_num, 0);         /* 拉低 */
gpio_direction_input(gpio_num);      /* 输入 */
int val = gpio_get_value(gpio_num);  /* 读值 */
gpio_free(gpio_num);
```

### UART

```
硬件：TX/RX 线，波特率/数据位/停止位/校验
驱动：platform_driver → tty 层 → /dev/ttyS0
调试：console=ttyS0,115200 内核参数
```

### SPI

```c
/* SPI 框架 */
struct spi_device *spi;
struct spi_transfer xfer = {
    .tx_buf = tx_data,
    .rx_buf = rx_data,
    .len = len,
};
spi_sync(spi, &msg);
```

### I2C

```c
/* I2C 框架 */
struct i2c_client *client;
i2c_smbus_read_byte_data(client, reg_addr);
i2c_smbus_write_byte_data(client, reg_addr, value);
```

### USB

```
Host 模式：USB 子系统 → xHCI/EHCI 驱动 → 设备枚举
Device 模式：USB Gadget 框架 → ConfigFS
```

---

## 2.6 总线协议速记

### AXI 家族

| 协议 | 特点 | 用途 |
|------|------|------|
| **AXI-Lite** | 简单，无 burst | 寄存器访问、IP 初始化 |
| **AXI-Full** | 完整 burst，memory map | DDR 访问、DMA (Direct Memory Access) |
| **AXI-Stream** | 无地址，数据流 | 视频/信号处理 pipeline |

```
一次传输大小 = (AWLEN + 1) × 2^AXSIZE
一拍大小 = 2^AXSIZE
```

### AHB (Advanced High-performance Bus)

```
Burst 类型：SINGLE, INCR, WRAP
Burst 长度：1, 4, 8, 16
笔记：M200 DMA 仅支持 INCR 1/4/8/16
  burst 16 传 1KB ≈ 20μs
  burst 4  传 1KB ≈ 30μs
```

### CHI（了解）

- ARM 新一代一致性互联协议
- Neoverse V1/N2 + CMN-700
- 支持更多 core、更高性能、更灵活 cache 一致性

---

## 2.7 FPGA 原型阶段工作

### 工程经验映射

| FPGA 原型工作 | M100/M200 经验 | 机器人芯片 Zynq |
|---------------|---------------------|-----------------|
| Boot 验证 | Flash boot + HSM (Hardware Security Module) secure boot | FSBL → U-Boot → Linux |
| Core 性能 | CoreMark 优化 | A53 性能摸底 |
| Cache/DMA | I/D Cache + DMA burst | CCI cache 一致性 |
| Debug | OpenOCD + GDB + VCS | JTAG + Xilinx hw_server |
| IP 验证 | HSM/eFuse/Timer | PS 外设 + PL 逻辑 |
| 联调 | RTL + Software | PS + PL bitstream |

### 口述要点

> 车规 MCU 的 FPGA 原型验证与芯片公司 Zynq 硅前阶段同构：boot 打通 → IP 逐个验证 → 性能摸底 → 与硬件联调 → 提前发现 silicon bug；方法论可迁移。

---

---

## 小结

- BSP 把芯片启动、时钟复位、外设与 OS 对接成可产品化的板级支持。
- Device Tree 描述硬件；驱动用兼容串匹配后在 probe 里申领资源。
- AXI-Lite 与 Full 的差别首先是能力与复杂度，不是「谁更高级」口号。
- FPGA 原型阶段的工作流与硅前 bring-up 同构：打通→验证→摸底→联调。

## 自测

1. Zynq 启动大致经过哪些阶段？FSBL 做什么？
2. Device Tree 解决什么问题？驱动如何匹配节点？
3. Platform driver 的 probe 通常完成哪些事？
4. AXI-Lite 与 AXI-Full 的直觉差异？
5. FPGA 原型阶段 BSP 侧你如何安排优先级？

---

*`03-rtos-bsp` · 芯片 BSP*
