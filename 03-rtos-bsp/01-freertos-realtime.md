# FreeRTOS (Real-Time Operating System) 与实时性：任务、同步与机器人选型

> **系列**：`03-rtos-bsp`  
> **前置**：无特别前置  
> **相关**：[`02-bsp-and-boot.md`](02-bsp-and-boot.md) · [`../01-kernel/01-scheduling.md`](../01-kernel/01-scheduling.md)

面向机器人/控制场景整理 FreeRTOS 核心：任务与优先级、信号量/互斥/队列、FromISR 规则、tickless，以及何时该上 RTOS、何时 PREEMPT_RT (Preemptible Real-Time) / AMP (Asymmetric Multi-Processing)。

**读完应能**：
- 对比 Mutex 与 Semaphore
- 说明中断里能调哪些 API
- 给出机器人控制的 OS 选型理由

---
## FreeRTOS 与实时性

## 1.1 为什么机器人芯片需要 RTOS

```
机器人 SoC 典型负载：

┌─────────────────────────────────────────────────────┐
│  视觉/AI 推理     → 高算力，延迟不敏感    → Linux   │
│  网络/通信        → 协议栈复杂           → Linux   │
│  运动控制/电机    → μs 级确定性延迟      → RTOS    │
│  传感器融合       → ms 级确定性          → RTOS    │
│  安全监控         → 功能安全             → RTOS/MCU │
└─────────────────────────────────────────────────────┘

单 OS 搞不定 → AMP（Asymmetric Multi-Processing）架构
  大核算力（Linux）+ 小核/MCU 做实时（FreeRTOS）
```

---

## 1.2 FreeRTOS 核心概念

### 任务（Task）

```c
/* 创建任务 */
xTaskCreate(
    vTaskFunction,    /* 任务函数 */
    "TaskName",       /* 任务名（调试用） */
    1024,             /* 栈大小（word 数） */
    NULL,             /* 参数 */
    tskIDLE_PRIORITY + 2,  /* 优先级（数字越大越高） */
    &xTaskHandle      /* 任务句柄 */
);

void vTaskFunction(void *pvParameters)
{
    for (;;) {
        /* 任务逻辑 */
        vTaskDelay(pdMS_TO_TICKS(100));  /* 延时 100ms，让出 CPU */
    }
}
```

### 任务状态

```
         ┌──────────┐
    ┌───▶│ Running  │◀───┐
    │    └────┬─────┘    │
    │         │          │
  就绪/恢复   │  时间片到/阻塞  │  唤醒
    │         │          │
    │    ┌────▼─────┐    │
    └───▶│ Ready    │────┘
         │ (就绪)    │
         └────┬─────┘
              │ 等信号量/队列/延时
         ┌────▼─────┐
         │ Blocked  │
         │ (阻塞)    │
         └──────────┘
              │
         ┌────▼─────┐
         │ Suspended│  ← vTaskSuspend()
         │ (挂起)    │
         └──────────┘
```

| 状态 | 说明 |
|------|------|
| **Running** | 正在 CPU 上执行 |
| **Ready** | 就绪，等调度器选中 |
| **Blocked** | 等信号量/队列/事件/延时 |
| **Suspended** | 被显式挂起 |
| **Deleted** | 已删除，等清理 |

### 调度策略

- **Preemptive（抢占式）**：高优先级 task 立即抢占低优先级
- **Cooperative（协作式）**：task 主动让出 CPU
- FreeRTOS 默认 **Preemptive**
- **同优先级**：时间片轮转（可配置 `configUSE_TIME_SLICING`）

---

## 1.3 同步机制

### 信号量（Semaphore）

```c
/* 二值信号量：同步 */
SemaphoreHandle_t xSemaphore = xSemaphoreCreateBinary();
xSemaphoreGive(xSemaphore);              /* 释放 */
xSemaphoreTake(xSemaphore, portMAX_DELAY); /* 等待 */

/* 计数信号量：资源计数 */
SemaphoreHandle_t xSem = xSemaphoreCreateCounting(10, 10);
```

### 互斥量（Mutex）—— 解决优先级反转

```c
SemaphoreHandle_t xMutex = xSemaphoreCreateMutex();

/* 使用 */
if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    /* 访问共享资源 */
    xSemaphoreGive(xMutex);
}
```

**优先级继承（Priority Inheritance）**：

```
问题：Task H(高) 等 Task L(低) 持有的 Mutex
      Task M(中) 抢占 Task L → H 被 M 间接阻塞

FreeRTOS Mutex 解决：
  Task L 持有 Mutex 期间，临时提升到 H 的优先级
  → M 无法抢占 L → H 不会被 M 阻塞
  → L 释放 Mutex 后恢复原始优先级
```

**要点**：FreeRTOS 的 Mutex 自带 Priority Inheritance，Semaphore 不带。保护共享资源用 Mutex，不要用 Binary Semaphore。

### 队列（Queue）

```c
QueueHandle_t xQueue = xQueueCreate(10, sizeof(int));

/* 发送 */
int data = 42;
xQueueSend(xQueue, &data, portMAX_DELAY);

/* 接收 */
int received;
xQueueReceive(xQueue, &received, portMAX_DELAY);
```

- 线程安全的 FIFO
- 可阻塞发送/接收
- 常用于 task 间通信

### 事件组（Event Group）

```c
EventGroupHandle_t xEventGroup = xEventGroupCreate();

/* 设置事件位 */
xEventGroupSetBits(xEventGroup, BIT_0 | BIT_1);

/* 等待多个事件 */
xEventGroupWaitBits(xEventGroup, BIT_0 | BIT_1,
                    pdTRUE,   /* 等到后清除 */
                    pdTRUE,   /* 等所有位 */
                    portMAX_DELAY);
```

---

## 1.4 中断与 FreeRTOS

### 中断优先级规则

```
ARM Cortex-M 中断优先级（数值越小越高）：

configMAX_SYSCALL_INTERRUPT_PRIORITY  ← FreeRTOS API 安全线
  │
  │  高于此优先级的中断：
  │    ✗ 不能调用 FreeRTOS API（FromISR 版本除外）
  │    ✓ 执行时间要短
  │
  │  低于此优先级的中断：
  │    ✓ 可以调用 FromISR API
  │    ✓ xSemaphoreGiveFromISR() 等
  │
  0（最高优先级）
```

### FromISR API

```c
void UART_IRQHandler(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    /* 清除中断标志 */
    UART->ICR = UART_INT_RX;

    /* 通知任务 */
    xSemaphoreGiveFromISR(xRxSemaphore, &xHigherPriorityTaskWoken);

    /* 如果需要切换任务 */
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
```

### 与 RISC-V MCU 的对照（工程经验）

| 概念 | ARM Cortex-M + FreeRTOS | RISC-V MCU（如 M100/M200） |
|------|------------------------|------------------------------|
| 关中断同步 | `taskENTER_CRITICAL()` | 直接 `csrc mstatus, MIE` |
| 中断优先级 | NVIC 配置 | CLIC (Core-Local Interrupt Controller)/PLIC (Platform-Level Interrupt Controller) |
| 栈 | FreeRTOS 分配 task 栈 | ILM (Instruction Local Memory)/DLM (Data Local Memory)/TCM (Tightly-Coupled Memory) |
| 调度触发 | PendSV 异常 | 软件中断/trap |

---

## 1.5 Tickless 低功耗

### 原理

```
传统：SysTick 每 1ms 中断 → 检查延时 task → 功耗高

Tickless：
  1. 计算下一个 task 唤醒时间
  2. 停止 SysTick
  3. CPU 进入 WFI（Wait For Interrupt）
  4. 外设中断 or SysTick 到期 → 唤醒
  5. 补偿 tick 计数
```

```c
/* FreeRTOS tickless 配置 */
#define configUSE_TICKLESS_IDLE  1
```

**车规 MCU 关联**：power mode 笔记（sleep/standby/deep sleep）与 tickless 思路一致。

---

## 1.6 FreeRTOS vs Linux PREEMPT_RT

| 维度 | FreeRTOS | Linux PREEMPT_RT |
|------|----------|------------------|
| **典型延迟** | 1–10 μs | 10–50 μs |
| **上下文切换** | ~1–3 μs | ~5–10 μs |
| **内存占用** | KB 级 | MB 级 |
| **生态** | 裸机/简单驱动 | 完整 Linux 生态 |
| **网络/文件系统** | 需 LwIP/FAT 等 | 原生支持 |
| **开发复杂度** | 低 | 高 |
| **适用** | 单核 MCU、电机控制 | 复杂 SoC、视觉+控制 |

### 机器人场景选择

| 场景 | 推荐 | 原因 |
|------|------|------|
| 电机 FOC 控制 | FreeRTOS / 裸机 | μs 级确定性 |
| IMU 数据融合 | FreeRTOS | ms 级，简单 OS 够用 |
| 视觉 SLAM | Linux | 算力需求大 |
| 语音/NLP | Linux + NPU | 模型推理 |
| 全栈在一个 OS | Linux PREEMPT_RT + CPU isolation | 简化架构 |

### AMP 架构（大芯片常见）

```
┌─────────────────────────────────────────────┐
│  ARM Cortex-A78 × 4        ARM Cortex-R52 × 2│
│  ┌──────────────┐          ┌──────────────┐ │
│  │    Linux     │          │  FreeRTOS    │ │
│  │  AI/视觉/网络 │◀─rpmsg──▶│  运动控制     │ │
│  └──────────────┘          └──────────────┘ │
│         │                         │         │
│         └─────── Shared Memory ─────┘         │
│         └─────── Mailbox/Doorbell ──┘       │
└─────────────────────────────────────────────┘
```

**通信方式**：
- **rpmsg**：Linux 与 RTOS 间的 virtio 消息
- **OpenAMP**：开源 AMP 框架
- **Shared Memory + Doorbell**：最简单，一块共享 RAM + 中断通知

---

---

## 小结

- 实时性首先是优先级与可预期延迟，不是「跑得快」。
- Mutex 带优先级继承，保护共享资源；Semaphore 更偏信令。
- 中断上下文只能走 FromISR API，并注意优先级规则。
- 控制环常用 RTOS；需要生态与中等实时时再评估 PREEMPT_RT / AMP。

## 自测

1. FreeRTOS 任务有哪些主要状态？
2. Mutex 与 Binary Semaphore 差别？为何 Mutex 能缓解优先级反转？
3. 中断里能否随意调用 FreeRTOS API？正确做法是什么？
4. 何种机器人负载更适合 RTOS 而不是 Linux？
5. AMP 下 Linux 与 RTOS 常见如何通信？

---

*`03-rtos-bsp` · FreeRTOS 与实时性*
