# Coding / 算法面试题

> **系列**：`07-interview`  
> **相关**：嵌入式现场常考的「手写小模块」；与 `01`–`06` 机制文互补，本篇偏 **API 设计 + 正确性边界**

收录 coding / 数据结构向真题；现场往往要求在白板上或 IDE 里写出可编译骨架。每题保留接口约定与易踩坑点。

---

## 一、真题记录

### Q1. 实现 Ring Buffer，封装读 / 写接口

- **出处**：嵌入式 / BSP / 驱动向 coding；常作「10～20 分钟手写」
- **原题**：实现一个环形缓冲区（ring buffer / circular queue），对外封装 **写（put）** 与 **读（get）** 接口；说明空 / 满如何判定，并写出关键路径代码。
- **常见追问**：
  1. 满了再写怎么办？（丢弃 / 覆盖最旧 / 返回错误）
  2. 单生产者单消费者（SPSC）能否无锁？要什么内存序？
  3. 容量为什么常取 2 的幂？`head`/`tail` 会不会溢出？
  4. `read`/`write` 一次搬 N 字节时，如何处理「绕回」折成两段 `memcpy`？
- **当场要点**（口述骨架）：
  1. 固定长度数组 + 读指针 `r` + 写指针 `w`（或 `head`/`tail`）。
  2. **空**：`r == w`；**满**：常见两种——浪费一格（`(w+1)%N == r`）或另存 `count`/`size`。
  3. 写成功前进 `w`，读成功前进 `r`；对外只暴露 `put`/`get`（或 `write`/`read`），内部细节不泄漏。
  4. 返回值约定清楚：成功字节数 / `bool` / 错误码，**满写失败与空读失败**要可区分。
- **参考答 / 自评**：见第二块；面试加分在 **契约说清**（满策略、线程模型、是否可重入），而不只是 `% N` 转圈。

---

## 二、盲区延伸

### 由 Q1 → 接口契约先钉死，再写实现

口述开场建议先问清 / 主动声明这四条（30 秒），再动笔：

| 契约 | 常见默认（嵌入式手写） | 为何要说清 |
|------|------------------------|------------|
| 元素类型 | `uint8_t` 字节流，或 `T` 模板/宏 | 决定 API 是按字节还是按元素 |
| 满策略 | **写失败返回错误**（不覆盖） | 覆盖适合 trace/log；队列语义多半不能静默丢 |
| 并发 | **单线程**，或 **SPSC 无锁** | 多写多读要另谈锁 / 原子 |
| 容量 | 编译期固定 `N`，优先 **2 的幂** | 用位运算代替 `%`，见下节 |

推荐对外 API（字节流版，契约：满则写失败、空则读失败）：

```c
typedef struct {
    uint8_t *buf;
    size_t   cap;   /* usable slots = cap - 1 if waste-one-slot */
    size_t   r;     /* read index  */
    size_t   w;     /* write index */
} ring_t;

void     ring_init(ring_t *rb, uint8_t *storage, size_t cap);
size_t   ring_used(const ring_t *rb);
size_t   ring_free(const ring_t *rb);
/* return bytes written/read; 0 means full/empty (or n==0) */
size_t   ring_write(ring_t *rb, const uint8_t *data, size_t n);
size_t   ring_read(ring_t *rb, uint8_t *data, size_t n);
```

单字节也可再包一层薄接口（面试常先写这个，再扩展批量）：

```c
int ring_put(ring_t *rb, uint8_t byte);  /* 0 ok, -1 full */
int ring_get(ring_t *rb, uint8_t *out);  /* 0 ok, -1 empty */
```

### 由 Q1 → 空 / 满判定：浪费一格 vs 计数

指针都落在 `[0, N)` 时，`r == w` 只能表示一种状态，必须二选一：

```text
方案 A：浪费一格（推荐手写默认）
  empty: r == w
  full : (w + 1) % N == r
  可用容量 = N - 1

方案 B：另存 count（或 size）
  empty: count == 0
  full : count == N
  可用容量 = N
  代价：count 与 r/w 三者要保持一致；并发时多一个共享变量
```

**浪费一格**实现（`put`/`get`）：

```c
int ring_put(ring_t *rb, uint8_t byte)
{
    size_t next = (rb->w + 1) % rb->cap;
    if (next == rb->r)
        return -1;           /* full */
    rb->buf[rb->w] = byte;
    rb->w = next;
    return 0;
}

int ring_get(ring_t *rb, uint8_t *out)
{
    if (rb->r == rb->w)
        return -1;           /* empty */
    *out = rb->buf[rb->r];
    rb->r = (rb->r + 1) % rb->cap;
    return 0;
}
```

`used` / `free`（浪费一格）：

```c
size_t ring_used(const ring_t *rb)
{
    return (rb->w + rb->cap - rb->r) % rb->cap;
}

size_t ring_free(const ring_t *rb)
{
    /* leave one slot empty */
    return rb->cap - 1 - ring_used(rb);
}
```

### 由 Q1 → 批量读写：绕回折成至多两段

`ring_write` / `ring_read` 一次拷 `n` 字节时，线性缓冲会在末尾「折返」。面试口述：**先算第一段到数组尾，再算第二段从 0 起**，不要在循环里逐字节 `%`（慢且易错）。

```text
例：cap=8, r=5, w=5（空），waste-one → 最多写 7
写 5 字节：
  第一段：index 5,6,7 → 3 字节
  第二段：index 0,1   → 2 字节
  之后 w = 2
```

要点：

1. `n` 先 `min(n, ring_free(rb))`，**允许短写**（返回实际字节数），与 POSIX `write` 同类语义，比「全或无」更好测、更好用。
2. 第一段长度 = `min(n, cap - w)`；余下第二段从 `0` 起。
3. 更新 `w`（或 `r`）时用 `% cap`（或 2 的幂时用 `& (cap-1)`）。

### 由 Q1 → 容量取 2 的幂

若 `cap` 为 2 的幂：

```c
idx = (idx + 1) & (cap - 1);   /* 代替 % cap */
```

嵌入式上 `%` 对非常量分母可能很贵；面试提一句即可。注意：**浪费一格时可用容量仍是 `cap-1`**，不要误以为「2 的幂所以满了还能塞 cap 个」。

`r`/`w` 用 `size_t` 单调递增、只在索引时取模也可以（永久计数），满空用 `(w - r)` 与 `cap` 比——这是另一派写法；SPSC 无锁文献里常见。手写题二选一说清即可。

### 由 Q1 → 并发追问怎么收口

| 模型 | 做法 | 一句话 |
|------|------|--------|
| 单线程 / 关中断临界区 | 普通 `r`/`w` | 中断与线程各一端时，短路径关中断或用无锁 SPSC |
| SPSC 无锁 | 一端只改 `w`，一端只改 `r`；对端只读 | 写完数据再发布 `w`；读完再发布 `r`（C11 atomic + 合适 memory order，或平台 barrier） |
| MPMC | 锁，或更重无锁结构 | 手写题一般不要求现场写完 |

不要主动把题做大；追问到并发时：**先画清谁生产、谁消费**，再谈锁或 atomic。

### 由 Q1 → 现场自测清单（写完对着念）

1. 空缓冲 `get` → 失败；满缓冲 `put` → 失败（或不覆盖）。  
2. 写满 `N-1` 个再读光，`r`/`w` 回到相等。  
3. 从尾部绕回：`w` 靠近 `cap-1` 时批量写仍正确。  
4. `n=0` 的 `read`/`write` 返回 0，不破坏状态。  
5. 初始化后 `used==0`，`free==cap-1`（浪费一格）。

---

## 小结

1. Ring buffer 面试分三层：**契约**（满策略 / 线程模型）→ **空满判定**（浪费一格或 count）→ **批量绕回**（两段拷贝）。  
2. 对外只暴露 `put`/`get` 或 `write`/`read`；返回值区分满、空、短读写。  
3. 默认手写：**浪费一格 + 满则失败 + 单线程**；追问再升级 SPSC / 2 的幂。  
4. 加分点：主动声明契约、短写语义、自测绕回与空满边界。
