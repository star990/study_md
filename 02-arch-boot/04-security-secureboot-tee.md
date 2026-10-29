# Secure Boot 全链路：RoT、eFuse、HSM (Hardware Security Module) 与 OP-TEE

> **系列**：`02-arch-boot`  
> **前置**：[`03-boot-firmware.md`](03-boot-firmware.md)  
> **相关**：[`01-exception.md`](01-exception.md) · [`02-gic.md`](02-gic.md)（§2.4 Secure 中断 / 1020·1021）

从信任根到应用的安全启动与运行时隔离：证书链、eFuse 生命周期、HSM、Secure Debug，再用项目案例串起来，并补 OP-TEE / SMC 接口层与 Trusted UI（安全键盘）路径。

**读完应能**：
- 白板讲清 Secure Boot 主路径
- 说明 eFuse 生命周期与密钥角色
- 画出 REE/TEE 与 SMC 调用关系（含 CA→TA、tee-supplicant）
- 说明支付安全键盘为何不在 Linux input 里、PIN 如何不出 TEE

---
## Secure Boot 全链路

## 1.1 Security 整体框架

### Security 包含哪些方面？

```
┌─────────────────────────────────────────────────────────────┐
│                    Security 整体架构                          │
├─────────────────────────────────────────────────────────────┤
│  硬件隔离 + 保护敏感信息                                       │
│                                                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       │
│  │  处理器       │  │  总线         │  │  内存         │       │
│  │  TrustZone   │  │  AWPROT/     │  │  TZC/TZASC  │       │
│  │  EL3 Monitor │  │  ARPROT      │  │  DDR/SRAM    │       │
│  │  MMU/Cache   │  │  ID Check    │  │  Secure/NS   │       │
│  └──────────────┘  └──────────────┘  └──────────────┘       │
│                                                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       │
│  │  外设         │  │  密钥存储     │  │  加密引擎     │       │
│  │  Secure UART │  │  eFuse       │  │  HSM/SPAcc   │       │
│  │  Mailbox     │  │  OTP         │  │  PKA/TRNG    │       │
│  │  Timer       │  │  Key Storage │  │  Crypto Acc  │       │
│  └──────────────┘  └──────────────┘  └──────────────┘       │
└─────────────────────────────────────────────────────────────┘
```

### Security 应用场景

| 场景 | 技术 | 典型实现 |
|------|------|----------|
| **Secure Boot** | 签名验证链 | eFuse root key + 证书链 |
| **生物识别** | TEE 保护模板 | OP-TEE TA |
| **移动支付 / PIN** | Trusted UI + 密钥不出 TEE | Secure Touch/Display + 支付 TA |
| **DRM** | 密钥隔离 | Widevine/FairPlay/PlayReady |
| **可信计算** | TPM/TEE | Windows 11 必选 |
| **Secure Debug** | Token 授权 | Signed debug token |
| **Anti-rollback** | 版本计数器 | eFuse rollback counter |

---

## 1.2 Root of Trust（信任根）

### 什么是 Root of Trust

整个 Secure Boot 链的**第一个信任锚点**——如果它不可信，后面全部不可信。

```
Root of Trust (eFuse 中的 Root Key Hash)
  │
  │ "我信任这个 hash 对应的公钥"
  │
  ├── 验签 BL2 证书
  │     └── 得到 BL2 公钥
  │           └── 验签 BL2 image
  │                 └── 得到 BL2 code（可信）
  │                       └── BL2 验签 BL31/BL32/BL33
  │                             └── ... 链式信任
```

### 信任链（Chain of Trust）

完整描述：

```
Root CA（根证书，私钥离线保存）
  │
  ├── 用 Root 私钥签 Sub-CA 证书
  │     │
  │     ├── 用 Sub-CA 公钥验 Sub-CA 证书 ✓
  │     │     │
  │     │     └── 用 Sub-CA 私钥签 Image 证书
  │     │           │
  │     │           └── 用 Sub-CA 公钥验 Image 证书 ✓
  │     │                 │
  │     │                 └── 用 Image 证书公钥验 Image 签名 ✓
  │     │
  │     └── A 信任 B，B 信任 C → A 信任 C
```

### X.509 证书结构

```
Certificate {
    Version
    Serial Number
    Signature Algorithm        (SHA256-RSA 等)
    Issuer                     (签发者 DN)
    Validity                   (Not Before / Not After)
    Subject                    (持有者 DN)
    Subject Public Key Info    (公钥 + 算法)
    Extensions                 (Key Usage, Basic Constraints...)
    Signature                  (Issuer 私钥对以上内容的签名)
}
```

### 验签流程（通用）

```
1. 提取证书中的 Signature
2. 用 Issuer 公钥解密 Signature → 得到 hash A
3. 对证书内容（除 Signature 外）用相同算法算 hash → 得到 hash B
4. hash A == hash B → 证书可信
5. 提取 Subject 公钥 → 用于验签下一级
```

---

## 1.3 eFuse 详解

### eFuse 是什么

**一次性可编程存储**（Electrically Programmable Fuse），芯片出厂后烧录密钥和配置，**不可逆**。

### 技术特性

| 特性 | 说明 |
|------|------|
| **写操作** | 需要高压；写前供电准备；写完 IRQ (Interrupt Request) 触发后立即关高压 |
| **读操作** | 普通电压，但寿命有限（读 10s+ 次级别 vs 写 ~1s） |
| **Double-bit** | 防止反转：两个 bit 中任一位为 1 即为 1 |
| **Block Lock** | 某个 block 写完后可 lock，永久不可改 |
| **供电隔离** | 读写各一路电源，写时才上高压 |

### eFuse 内容分类（Genesys V100）

| 类型 | 内容 | 说明 |
|------|------|------|
| **non-key** | secure state, chip ID, DIE ID | 状态/标识，非密钥 |
| **non-key** | rom patch en, xip en, rom patch | 功能开关 |
| **soc key** | jtag key | 芯片厂密钥 |
| **customer key** | hash root key × 4 | 客户根密钥 hash |
| **customer non-key** | users cust key, rollback count | 客户配置 |
| **customer non-key** | private key × 8 | 客户私钥相关 |

### eFuse 生命周期

```
┌────────┐    ┌────────┐    ┌────────┐    ┌────────┐
│   CM   │───▶│   DM   │───▶│   SE   │───▶│  回厂  │
│  裸片   │    │ 给客户的 │    │ 最终用户 │    │ 隐私清零│
└────────┘    └────────┘    └────────┘    └────────┘
 权限最高       关 bootrom     debug 受限     读出为 0
 可 debug       后门/冗余
 可写 fuse     禁 XPI
               禁 rom patch
```

| 阶段 | 权限 | 典型操作 |
|------|------|----------|
| **CM** (Chip Manufacturing) | 最高 | 写 root key、debug 全开 |
| **DM** (Device Manufacturing) | 中 | 关 bootrom 打印、禁后门、禁 XPI |
| **SE** (Secure End-user) | 低 | 仅用户级 debug token |
| **回厂** | 无 | 隐私信息清零，读出为 0 |

**要点**：lifecycle 控制 debug 能力和 fuse 读写权限，是硬件级安全边界。

---

## 1.4 Secure Boot 完整流程

### 5 分钟 Whiteboard 版

```
┌─────────────────────────────────────────────────────────────┐
│                    Secure Boot 全流程                         │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  [SoC Reset]                                                 │
│       │                                                      │
│       ▼                                                      │
│  ┌─────────┐                                                │
│  │ BootROM │  EL3                                           │
│  │  (BL1)  │                                                │
│  └────┬────┘                                                │
│       │ 1. 读 eFuse: root key hash, lifecycle, rollback      │
│       │ 2. Init: PLL, cache, uart, pinmux                   │
│       │ 3. 选择 boot 介质 (eMMC/UFS/SPI/UART/XIP)           │
│       │ 4. 加载 BL2 image + certificate                     │
│       │ 5. 验签 BL2:                                        │
│       │    a. 证书公钥 hash vs eFuse root key hash           │
│       │    b. 公钥验证书签名                                  │
│       │    c. 公钥验 BL2 image 签名                          │
│       │    d. rollback counter 检查                          │
│       │ 6. 跳转 BL2                                          │
│       ▼                                                      │
│  ┌─────────┐                                                │
│  │   BL2   │  EL3                                           │
│  └────┬────┘                                                │
│       │ 1. Init DDR                                          │
│       │ 2. 加载 BL31 + cert → 验签                          │
│       │ 3. 加载 BL32(TEE) + cert → 验签                     │
│       │ 4. 加载 BL33(U-Boot) + cert → 验签                │
│       │ 5. 跳转 BL31                                         │
│       ▼                                                      │
│  ┌─────────┐                                                │
│  │  BL31   │  EL3, 常驻 Monitor                              │
│  └────┬────┘                                                │
│       │ 1. 启动 BL32 (OP-TEE) → Secure EL1                  │
│       │ 2. ERET 到 BL33 → NS EL1                            │
│       ▼                                                      │
│  ┌─────────┐       ┌─────────┐                              │
│  │ BL32    │       │  BL33   │  NS EL1                      │
│  │ OP-TEE  │       │ U-Boot  │                              │
│  └─────────┘       └────┬────┘                              │
│                         │ 加载 kernel + DTB                  │
│                         ▼                                    │
│                    ┌─────────┐                              │
│                    │  Linux  │  NS EL1                      │
│                    └─────────┘                              │
└─────────────────────────────────────────────────────────────┘
```

### 验签 BL2 详细 5 步

```
Step 1: 从 BL2 image 头部提取证书（Certificate）

Step 2: 从证书中提取公钥，计算公钥的 hash
        与 eFuse 中预烧的 root key hash 对比
        ├─ 匹配 → 继续
        └─ 不匹配 → 验签失败，停止 boot

Step 3: 用该公钥验证证书本身的签名
        （根证书 = 用自己的私钥自签）

Step 4: 用已验证的公钥验证 BL2 image 的签名
        得到 BL2 内容的 hash

Step 5: 对比 BL2 hash 与证书中记录的 hash
        ├─ 匹配 → BL2 可信，加载执行
        └─ 不匹配 → 验签失败
```

### Anti-Rollback（防降级攻击）

```
eFuse 中烧 rollback counter = N

BL2 image 中版本号 = M

Boot 时检查：
  M >= N → 允许启动，可选更新 counter
  M < N  → 拒绝启动（防止刷旧版本漏洞固件）
```

---

## 1.5 密钥体系

### 密钥分类

| 密钥 | 存储 | 用途 |
|------|------|------|
| **RTL Key** | 对称，熔入硬件 | 产线烧录 RSA key 时验签，产线不能知道 |
| **HUK** (Hardware Unique Key) | eFuse/OTP | 每颗芯片唯一，派生其他密钥 |
| **Root Key** (Public) | eFuse 存 hash | Secure Boot 信任根 |
| **Private Key** | 离线/HSM | 签名 firmware image |
| **Device Key** | 运行时派生 | 设备级加密/认证 |

### KDF 密钥派生（HSM 工作）

 K1-K10 密钥体系：

```
Master ECU Key (flash 中存储)
  │
  ├── KDF 派生
  │     ├── K1 → AES-CBC 加密
  │     ├── K2 → ...
  │     └── K10 → ...
  │
  └── HSM 硬件执行 KDF + 加解密
        输入明文 → HSM → 输出密文
        与 expected 对比验证
```

**要点**：HSM K1-K10 验证就是确认 KDF 派生和 AES-CBC/CMAC 结果与标准向量 match。

---

## 1.6 HSM（Hardware Security Module）

### HSM 在系统中的位置

```
┌──────────────────────────────────────────────┐
│  Linux (NS)                                   │
│  ┌──────────┐                                │
│  │ Crypto   │ ← /dev/hwrng, /dev/crypto       │
│  │ Driver   │                                │
│  └────┬─────┘                                │
│       │ SMC / MMIO                            │
├───────┼──────────────────────────────────────┤
│  OP-TEE (Secure EL1)                          │
│  ┌────▼─────┐                                │
│  │ HSM TA   │ ← Secure 侧密钥操作             │
│  └────┬─────┘                                │
│       │                                       │
├───────┼──────────────────────────────────────┤
│  HSM IP (Hardware)                           │
│  ┌────▼─────┐  ┌─────────┐  ┌─────────┐     │
│  │  SPAcc   │  │   PKA   │  │  TRNG   │     │
│  │ AES/SM4  │  │RSA/ECC  │  │Random   │     │
│  │ SHA/SM3  │  │SM2      │  │Number   │     │
│  └──────────┘  └─────────┘  └─────────┘     │
└──────────────────────────────────────────────┘
```

### HSM 三大模块

| 模块 | 算法 | 用途 |
|------|------|------|
| **SPAcc** (Symmetric Processing Accelerator) | AES(多模式), SM3, SM4, SHA-512 | 对称加密/哈希 |
| **PKA** (Public Key Accelerator) | RSA-4096, ECC, SM2 | 非对称加密/签名 |
| **TRNG** (True Random Number Generator) | 硬件随机 | 密钥生成、nonce |

### HSM 验证方法

```
1. 从 GitHub/标准测试集获取输入向量
2. 配置 HSM 寄存器，输入测试数据
3. 触发运算
4. 读取输出
5. 与 expected output 对比
6. 覆盖：ECB/CBC/CMAC/KDF 等模式
7. Performance 测试（optional）
```

### TRNG 在 Linux 中的对接

```
Hardware TRNG
  │
  ├── BL31 SMC handler (你改过)
  │     ARM_TRNG_RND32 / ARM_TRNG_RND64
  │
  └── Linux hw_random 框架
        ├── 内核驱动注册 HWRNG
        ├── /dev/hwrng 字符设备
        └── 用户态 /dev/random 间接使用
```

---

## 1.7 Secure Debug

### 为什么需要 Secure Debug

- 量产后不能随意 JTAG/debug（攻击面）
- 但开发/售后需要有限的 debug 能力
- 解决方案：**Signed Debug Token**

### 机制

```
1. eFuse lifecycle 决定最大 debug 权限
   CM: 全开
   DM: 关 bootrom log, 禁 JTAG
   SE: 仅 token 授权

2. Debug Token 生成：
   - 用私钥签名 XML 配置
   - XML 指定：UID, debug 能力(modem log/TZ log/...)
   - 有效期

3. 设备端验证：
   - 用 eFuse 中公钥验 token 签名
   - 检查 UID 匹配
   - 检查未过期
   - 临时开启指定 debug 能力
```

---

## 1.8 BootROM 主要工作

| # | 工作 | 说明 |
|---|------|------|
| 1 | SoC 初始化 | debug/backdoor/冗余检查 |
| 2 | PLL | 时钟配置 |
| 3 | Boot reason | 读复位原因 |
| 4 | eFuse | 读 key/lifecycle/feature |
| 5 | ICache/DCache | 使能 cache |
| 6 | Pinmux | 引脚复用 |
| 7 | Stack/Vector | 栈和异常向量 |
| 8 | Rom patch | 若 eFuse 使能 |
| 9 | Console | UART (Universal Asynchronous Receiver/Transmitter) 初始化 |
| 10 | Boot 介质 | eMMC/UFS/UART/SPI (Serial Peripheral Interface)/XIP |
| 11 | Verify & Load BL2 | 验签加载 |
| 12 | DFU | 升级模式 |
| 13 | Boot 脚印 | 日志记录 |
| 14 | JTAG | 状态检查 |

---

## 1.9 IP 验证方法论（Genesys V100，你的核心方法论）

### 通用验证流程

```
Phase 1: 基础验证
  ├── 上电时序
  ├── Clock / Reset
  ├── 寄存器读写（default value）
  └── Bus 访问（AXI/AHB 协议）

Phase 2: 功能验证
  ├── IRQ 触发与响应
  ├── DMA 传输（single/burst）
  ├── 各功能模块独立 case
  └── 边界条件 / 错误注入

Phase 3: 场景验证
  ├── 多模块串联
  ├── Secure Boot 全链路
  ├── 性能 / 压力
  └── Corner case / 随机测试
```

### eFuse IP 验证特殊流程

```
1. Memory model 模拟真实 eFuse 颗粒
2. Read → 产生 IRQ → 验证数据
3. Write → 产生 IRQ → 验证 bit 状态
4. 重复写 → 产生 IRQ → 验证 double-bit 逻辑
5. Write to SRAM → Sync to Fuse → 验证
6. Reset → 验证之前写的 bit 仍在
7. Block lock → 验证不可再写
8. Lifecycle 切换 → 验证权限变化
```

### HSM IP 验证

```
1. 标准测试向量（GitHub NIST/国密）
2. SPAcc: AES-ECB/CBC/CMAC, SM4, SHA, SM3
3. PKA: RSA-4096 sign/verify, ECC, SM2
4. TRNG: 统计测试（NIST SP 800-22）
5. DMA 模式：HSM 通过 DMA 读写数据
6. Performance：吞吐量、延迟
7. Cache 一致性：MMU 属性验证
```

---

## 1.10 OP-TEE 运行时与 Trusted UI

> 启动验签保证「进来的固件可信」；本节讲 Linux 起来之后，密钥/PIN 如何仍不落在 REE。硬件动机与 CA/TA 分层可对照公众号文《从 TrustZone 到 OP-TEE：安全世界是怎样实现的？》（本地：`~/5_temp/TrustZone 到 OP-TEE：安全世界是怎样实现的？.html`）。

### 为何 MMU 不够：缩小 TCB

- Linux 用 MMU 隔离进程，但**页表由内核维护** → 内核被攻破（root / 恶意模块）即可映射任意物理地址。
- 若密钥只存在 Linux，整个内核 + 驱动都进 **TCB (Trusted Computing Base)**，无法逐行审查。
- TEE 用**内核改不了的硬件属性**划出 Secure 内存/外设，再由小得多的可信 OS + TA 使用密钥；REE 只能走受限接口拿「结果」，拿不到密钥本身。

### 两世界：差在 Secure state，不在 EL 高低

```
Normal World (REE)          Secure World (TEE)
  EL0  App / CA               S-EL0  TA
  EL1  Linux                  S-EL1  OP-TEE
  EL2  Hypervisor (可选)
              │
              ▼
         EL3 Secure Monitor (BL31，始终 Secure)
              SCR_EL3.NS：1 = 下面跑 NS；0 = 下面跑 Secure
```

- **S-EL1 并不比 Linux EL1「更特权」**；隔离靠 CPU Secure state + 总线安全属性，不是靠异常级别更高。
- `SCR_EL3.NS` 只能在 EL3 改 → Linux **不能**自己切进安全世界；唯一主动入口是 **SMC (Secure Monitor Call)**。
- 独立安全核（Apple SE、独立 SE 芯片）是另一条路；TrustZone 是**同一核分时** Secure/NS。

### 硬件隔离三件套：属性 → 控制器 → 中断

**1）每次访问带安全属性**

- AXI：`AxPROT` 中一位标 Secure / Non-secure。
- CPU 在 **NS** 时发出的事务**一律 NS**（页表里的 NS 位在 NS 状态下被忽略）→ 恶意改页表也发不出 Secure 访问。
- CPU 在 **Secure** 时：按页表 NS 位可选访问 Secure 或 NS 内存（TEE 可写共享缓冲）。
- **Cache / TLB 条目也带 NS**：同物理地址的 Secure 与 NS 视为不同行，NS 不会命中 Secure 留下的密钥缓存行。

**2）控制器按属性放行（总线本身不裁决）**

| 控制器 | 保护对象 | 作用 |
|--------|----------|------|
| **TZASC** (TrustZone Address Space Controller，如 TZC-400) | DDR | 按区域配 Secure/NS 读写权限 |
| **TZPC** (TrustZone Protection Controller) | 外设 | 加密引擎、OTP、安全定时器等可锁成仅 Secure |
| **TZMA** (TrustZone Memory Adapter) | 片上 SRAM/ROM | 划一部分给安全世界 |

配置寄存器本身通常也只接受 Secure 访问；启动阶段由 BL2/BL31/OP-TEE 划区，Linux 改不了。  
**DMA** 作为 bus master 同样带安全属性，一般配成 NS master → 也不能靠 DMA 偷读 Secure DDR。

**3）中断分组（GIC）**

- Group0 / Secure Group1 / NS Group1；分组配置仅 Secure 可改。
- Linux 跑着时来了 Secure 中断 → 进 EL3 再交 TEE；NS 侧屏蔽挡不住。细节见 [`02-gic.md`](02-gic.md) §2.4（伪 ID **1020 / 1021**）。

**Linux 读 Secure 内存被拒的路径（口述）**：页表映射了 Secure PA → NS CPU 发 NS 读 → TZASC 拒（读 0 或 bus error，可报中断）。内核能选「地址」，选不了「安全属性」。

### 启动先后：TEE 必须先于 Linux 就位

与 §1.4 一致，强调运行时含义：

```
BL31 初始化
  → 先以 Secure 进入 BL32 (OP-TEE)：建安全映射、线程、TA 环境
  → OP-TEE SMC 回 BL31，登记各入口
  → 再 NS=1 进 BL33 (U-Boot) → Linux
```

- TZASC 等在 Linux 跑前配好；划给 TEE 的内存在 DT 里常标 **reserved** → 开启 TEE 后「可用内存少一块」，Linux 一般收不回。
- 若 TEE 在 Linux 之后才装，Linux 有窗口改安全代码/控制器 → **不可接受**。

### CA → TA：一次调用穿过哪些层

OP-TEE 在安全世界内再隔离：**每个服务一个 TA（S-EL0，独立地址空间，UUID + 签名校验）**。接口跟 GlobalPlatform：Client API（CA）/ Internal Core API（TA）。

```
CA (REE EL0)
  └─ libteec：TEEC_OpenSession / TEEC_InvokeCommand
       └─ ioctl → Linux TEE 子系统 (drivers/tee/) + OP-TEE 驱动
            · /dev/tee0     ← CA
            · /dev/teepriv0 ← tee-supplicant
            └─ 参数写入 NS↔Secure 共享内存，物理地址进寄存器
                 └─ SMC → BL31 (opteed) 存 Linux 上下文（含 EL1 系统寄存器）
                      └─ NS=0，ERET → OP-TEE → 目标 TA
                           · 私钥只在 Secure 内存；共享内存里只有明文入参 + 结果（如签名）
                      ← SMC 回 EL3，恢复 Linux，ioctl 返回 CA
```

要点：

- SMC 像「门铃」；大数据走**共享内存**（类比 mailbox + 共享缓冲）。
- AArch64 下 EL1 的 TTBR/VBAR 等**两世界共用一套寄存器**，故 Monitor 切换时必须保存/恢复 EL1 上下文。
- 发起 SMC 的 Linux 线程会停在调用上，这段 CPU 时间跑的是 TEE；OP-TEE 线程用尽则在驱动侧排队。若 TA 执行中来了 Linux 中断，OP-TEE 可先退回 NS 让 Linux 处理，再 SMC 续跑。
- **没有 `/dev/tee0`**：多半固件没起 OP-TEE、DT 无 `optee` 节点、或内核未开驱动。

### RPC：tee-supplicant 反向代办

OP-TEE 自己没有完整 FS/eMMC 驱动。处理请求中途可通过 SMC 返回 **RPC**，请 REE 代办：

| RPC 典型用途 | 行为 | 安全边界 |
|--------------|------|----------|
| **加载 TA** | tee-supplicant 从 FS 读 TA 镜像 → 共享内存；OP-TEE 拷进 Secure 内存并**验签** | REE 可删文件，无法伪造合法签名镜像 |
| **安全存储** | TEE 用 HUK 等派生密钥加密后，请 REE 写密文到 FS | REE 只有密文；篡改验不过 |

嵌入式集成：启动脚本需先拉起 **tee-supplicant**，否则依赖文件系统的 TA 会 **OpenSession 失败**。

### 支付安全键盘：Trusted UI（不是搬 Linux input）

App 自绘「随机布局软键盘」多半仍在 **REE**，root/恶意内核仍可能截获。真要防 NS 侧偷看 PIN，走 **Trusted UI / Secure Touch**：

```
支付 CA ① Invoke「请输入 PIN」
    → SMC → 支付 TA 申请 TUI 会话
    → 硬件（厂商）：Display 切 Secure framebuffer；Touch IRQ 改 Secure（TZPC）
    → 用户点按：坐标只进 TEE；屏幕常只显示 *；PIN 缓冲仅 Secure 内存
    → TA/HSM 内做 PIN block / MAC / 签名
    → ③ 仅密文或 token 回 CA；清 PIN；解除 TUI，恢复 NS 显示/触摸
```

| 组件 | 位置 | 职责 |
|------|------|------|
| 支付 App / CA | REE | 发起会话，展示普通业务 UI |
| 支付 TA | TEE | 会话、组 PIN、密码运算；**明文不出 TEE** |
| Secure Touch/Display 路径 | TEE 或厂商安全固件 | TUI 会话期内读坐标、画安全帧缓冲 |
| 触摸/显示控制器 | TZPC 配 Secure | 会话内 NS 读不到 |
| 普通 `input` / IME | REE | 平时使用；进 TUI 后旁路 |

一句话：**不是把整棵 Linux 输入栈搬进 TEE，而是会话期把输入/显示收归 Secure；回 REE 的只有密文/结果。**  
更高要求时 PIN/密钥再下沉到 **独立 SE**；TrustZone 主要防 NS 软件攻击，对物理/侧信道有限（与文章结论一致）。

### 其它常见 TEE OS（可选印象）

| TEE OS | 开源 | 常见平台 |
|--------|------|----------|
| OP-TEE | 是 | 嵌入式 Linux 参考实现 |
| Trusty | 是（AOSP） | 部分 Android |
| QSEE/QTEE | 否 | 高通 |
| TEEGRIS / iTrustee / Kinibi | 否 | 三星 / 华为 / Trustonic 等 |

硬件隔离与 EL3 切换模型相同；多数跟 GP，Trusty 用自有接口。

### 排障三句

1. **可用内存少一块** → 查安全内存 / DT reserved。  
2. **没有 `/dev/tee0`** → 查 BL32 是否起来、DT `optee`、内核 TEE 驱动。  
3. **Session 打不开** → 查 tee-supplicant、TA 文件路径与签名。

---

### 本节要点

**建议能白板讲清的内容**：

1. Secure Boot 全链路（BL1→Linux，含验签 5 步）
2. eFuse 生命周期（CM→DM→SE→回厂）
3. IP 验证方法论（三阶段 + eFuse 特殊流程）
4. CA→共享内存→SMC→TA；tee-supplicant 只碰密文/已签名镜像
5. 安全键盘：TUI 收归 Touch/Display，PIN 不出 TEE

---

## 项目案例深挖

## 2.1 项目 1：Genesys V100 Security IP 验证

### 案例完整版（STAR）

**S（Situation）**：
Genesys 自研 SoC V100，面向 IoT/AIoT 市场，芯片包含完整 Security 架构：TrustZone + eFuse + HSM + GIC (Generic Interrupt Controller) + Secure Boot。芯片 tape-out 前需要完成 Security 相关 IP 的验证 sign-off。

**T（Task）**：
负责 Security IP 模块级验证，包括 eFuse、HSM、GIC 的验证方案制定、case 开发、问题定位，以及 Secure Boot 场景串联验证。

**A（Action）**：
1. 制定 IP 验证方案：上电→clock→IRQ→DMA (Direct Memory Access)→功能→场景串联
2. eFuse 验证：用 memory model 模拟真实颗粒，覆盖 read/write/重复写/reset/sync SRAM (Static Random-Access Memory)/block lock
3. HSM 验证：SPAcc/PKA/TRNG 标准测试向量对比，覆盖 AES/SM4/RSA/ECC/SM2
4. GIC 验证：Secure/Non-secure 中断分组、优先级、SPI 路由
5. Secure Boot 场景：全链路 boot + 验签 + rollback + lifecycle 切换
6. 定位并推动修复多个 RTL/验证 bug

**R（Result）**：
- Security 相关 IP 全部通过 sign-off，支撑 V100 成功 tape-out
- 建立 IP 验证方法论，后续 IP 复用
- 发现并修复 eFuse double-bit、HSM cache 一致性等关键 bug

### 常见追问 & 回答

| 追问 | 回答要点 |
|------|----------|
| eFuse 验证最难的点？ | Memory model 模拟真实颗粒行为；lifecycle 切换权限验证 |
| HSM 遇到什么问题？ | Cache 一致性问题——MMU (Memory Management Unit) 配成 Normal 而非 Device，必须 flush |
| GIC Secure 中断怎么验？ | 配 Group0/Group1，Secure 外设触发，确认 routing 到 EL3 |
| 验签流程你怎么测？ | 分别测：正确签名通过、篡改 image 拒绝、过期证书拒绝、rollback 拒绝 |

---

## 2.2 项目 2：Motorola HAB Secure Boot

### 案例完整版（STAR）

**S（Situation）**：
Motorola Android 手机项目（kane/payton/andy 等），使用 Qualcomm 平台 + HAB（Hardware Assisted Boot）安全启动方案，需要确保 retail 版本完整 secure boot 闭环，包括 AP、Modem、TrustZone 等所有 firmware 签名。

**T（Task）**：
负责 secure boot 签名流程集成、efuse key 管理、OTA 升级签名验证、以及 secure boot 相关 bug 定位。

**A（Action）**：
1. HAB CST 签名流程：配置 CST client、product key、证书链
2. efuse key 管理：SMEM 传递 boot SRK、Prov service 烧录
3. OTA 签名：trustzone.bin 签名/验签、verity 集成
4. Keymaster 集成：Boot State 验证、Key Attestation
5. 推动修复多个 secure boot 量产出问题

**R（Result）**：
- retail 版本 secure boot 全流程闭环
- OTA 升级安全签名集成
- 积累 Android + Security 量产经验

### 常见追问 & 回答

| 追问 | 回答要点 |
|------|----------|
| HAB 是什么？ | NXP/Qualcomm 的 Hardware Assisted Boot，ROM 验签 bootloader |
| efuse key 怎么管理？ | Prov service 通过 TrustZone app 烧录；SMEM 传递 SRK |
| OTA 时 trustzone 怎么处理？ | 需签名 tz.bin；部分项目 deprecate signed tz for OTA |
| 遇到过什么 secure boot bug？ | Keymaster 验证 Boot State 与 flashed image 版本不匹配 |

---

## 2.3 项目 3：车规 MCU FPGA 原型验证（M100/M200）

### 案例完整版（STAR）

**S（Situation）**：
自研车规级 MCU（M100/M200），基于 RISC-V 核心，目标 ISO 26262 ASIL-B。芯片流片前在 FPGA 上做原型验证，需要验证 Core 功能、性能、Security（HSM Secure Boot）、以及各 IP 的正确性。

**T（Task）**：
负责 Core 性能优化、Cache/DMA 验证、HSM Secure Boot 验证、JTAG debug 环境搭建、以及 silicon bug 提前发现。

**A（Action）**：
1. **CoreMark 性能优化**：TCM (Tightly-Coupled Memory) 放热点函数、-O3 编译、ICache 调优，CoreMark 达标
2. **Cache 验证**：I/D Cache hit/miss 分析、cache line 对齐、与 flash/TCM 交互时序
3. **DMA 验证**：burst 传输效率、channel config bug 定位、HSM DMA latency 分析
4. **HSM Secure Boot**：K1-K10 密钥 KDF 派生验证、AES-CBC/CMAC 标准向量对比
5. **Debug 环境**：OpenOCD + GDB + VCS 联合仿真 debug；FPGA JTAG 频率调优
6. **Bug 发现**：dFlash program + dcache 一致性、dFlash AHB (Advanced High-performance Bus) write disable、DMA abort

**R（Result）**：
- 提前发现 10+ silicon bug，避免流片后返工
- CoreMark 性能达标，满足车规要求
- HSM Secure Boot 验证通过，支撑功能安全认证
- 建立 FPGA 原型验证流程

### 常见追问 & 回答

| 追问 | 回答要点 |
|------|----------|
| CoreMark 怎么优化？ | TCM 放热点、-O3 -funroll-all-loops、ICache 加大、检查 float lib |
| Cache 一致性问题？ | dFlash program 后 dcache invalidate；DMA 前后 flush/invalidate |
| HSM DMA 为什么慢？ | 17 拍 latency，对比 flash 取数时序，优化 burst/对齐 |
| FPGA debug 遇到什么坑？ | JTAG 频率过高录不到波形；128kHz IRC 时钟未灌导致 JTAG 状态机错误 |
| RISC-V 与 ARM 区别？ | PMP (Physical Memory Protection) vs MMU、PMA (Physical Memory Attributes) vs MAIR、无 TrustZone 但可有 PMP 保护 |

---

## 2.4 项目串联叙事

### 如何串联三个项目

```
时间线叙事：

"我的经历覆盖了 Security 从 IP 验证到量产的全链路：

最早在 Motorola 做 Android 手机 secure boot 量产（HAB），
理解完整的签名链和 efuse key 管理。

然后在 Genesys 做自研 SoC V100 的 Security IP 验证，
eFuse/HSM/GIC 从 RTL 级别验证，建立 IP 验证方法论。

近年做车规 MCU FPGA 原型，
把 Security 和 Core/DMA/Cache 验证推到更底层，
HSM Secure Boot、CoreMark 性能优化、 silicon bug 提前发现。

这三段经历互补：量产经验 + IP 深度 + 硅前验证广度。"
```

### 与典型岗位能力的映射

| 能力要求 | 项目映射 |
|---------|----------|
| 架构设计、规格制定 | Genesys V100 Security 架构验证 |
| 集成设计开发 | Moto HAB 全链路 secure boot |
| 驱动模块设计验证 | Genesys IP 验证 + MCU DMA/Cache |
| 硬件软件联合开发 | FPGA 原型联调、JTAG debug |
| 系统联调测试 | Secure Boot 场景串联、CoreMark |

---

---

## 小结

- 信任链：只信任 RoT，再逐级验签；eFuse 承载生命周期与密钥材料。
- HSM/KDF 等是「密钥与算法落地」的硬件边界，验证要有标准向量。
- 项目案例用来练「现象→根因→手段→结果」的叙述，而不是背型号。
- 运行时：AxPROT + TZASC/TZPC 防被攻破的内核；BL31 先起 OP-TEE 再进 Linux；CA→libteec→共享内存→SMC→TA；tee-supplicant 只代办加载/落盘密文。
- 支付安全键盘：TUI 会话收归 Touch/Display，PIN 只在 TA/HSM 内变成密文再回 REE——不是「input 驱动搬进 TEE」。
- **Secure 中断**不是 Linux 主动 SMC：FIQ→BL31，IAR 伪 ID **1020** 驱动切 TEE、**1021** 驱动切回 Linux；Group0/Secure Group1/回程细节见 [`02-gic.md`](02-gic.md) §2.4。GIC 验证口述可对齐该节。

## 自测

1. 用五分钟结构讲清 Secure Boot 主路径。
2. eFuse 生命周期如何约束 debug 与密钥权限？
3. Anti-rollback 比较的是哪两边？
4. CA 调用到 TA 的路径经过哪些层次？共享内存和 SMC 各干什么？
5. IP 验证常用哪三阶段？各解决什么风险？
6. Linux 跑着来了 Secure Group1 中断：和 CA→SMC→TA 路径差在哪？1020 扮演什么角色？
7. 为何说 S-EL1 并不比 Linux EL1 更高？隔离靠什么？
8. 没有 `/dev/tee0`、Session 打不开、可用内存少一块——分别先查什么？
9. 支付安全键盘若只在 App 里画随机键位，和 Trusted UI 差在哪？PIN 明文应出现在哪一侧？

---

*`02-arch-boot` · Secure Boot / TEE*
