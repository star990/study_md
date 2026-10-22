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
  5. 能不能用**双向链表**做？和数组环形比什么优劣？
- **当场要点**（口述骨架）：
  1. 固定长度数组 + 读指针 `r` + 写指针 `w`（或 `head`/`tail`）。
  2. **空**：`r == w`；**满**：常见两种——浪费一格（`(w+1)%N == r`）或另存 `count`/`size`。
  3. 写成功前进 `w`，读成功前进 `r`；对外只暴露 `put`/`get`（或 `write`/`read`），内部细节不泄漏。
  4. 返回值约定清楚：成功字节数 / `bool` / 错误码，**满写失败与空读失败**要可区分。
- **参考答 / 自评**：见第二块；面试加分在 **契约说清**（满策略、线程模型、是否可重入），而不只是 `% N` 转圈。双向链表版见「由 Q1 → 双向链表实现」。

### Q2. 字符串原地倒转（C / RV32 汇编）

- **出处**：嵌入式 coding；常追问「标准库有没有现成的」「汇编怎么写」「`lb` 还是 `lbu`」
- **原题**：给定以 `'\0'` 结尾的字符串，原地倒转；可用 C，也可要求 RV32 汇编。
- **常见追问**：
  1. C 标准库有没有 `strrev`？
  2. 找尾指针那段汇编在干什么？空串会不会踩坑？
  3. 为何用 `lbu` 而不是 `lb`？和 C 的 `char *` 是否一致？
- **当场要点**：
  1. 双指针：左端与右端交换，直到相遇；不碰结尾 `'\0'`。
  2. 缓冲区必须可写（栈/数组），不能倒转字符串字面量。
  3. **ISO C / POSIX 没有**标准 `strrev`；MSVC 等非标准扩展有，glibc/newlib 一般没有——自己写。
- **参考答 / 自评**：见「由 Q2」；汇编重点是找尾、`bgeu` 结束条件、以及 `lb`/`lbu` 对纯 swap 等价。

### Q3. 纯圆环链表，求长度（快慢指针）

- **出处**：链表 coding
- **原题**：给定**首尾相接的圆环**链表（无环外结点）和 `head`，用一步 / 两步两个指针求长度。
- **当场要点**：`slow` 每次 `next`，`fast` 每次两步；从 `head` 同步出发，每轮 `n++`，**第一次** `slow == fast` 时 `n` 即为环长。
- **参考答**：见「由 Q3」。

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

**浪费一格**实现（`init` / `put` / `get` / `used` / `free`）：

```c
void ring_init(ring_t *rb, uint8_t *storage, size_t cap)
{
    rb->buf = storage;
    rb->cap = cap;           /* usable slots = cap - 1 */
    rb->r = 0;
    rb->w = 0;
}

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

用法示意：

```c
uint8_t storage[8];
ring_t  rb;

ring_init(&rb, storage, sizeof(storage));  /* 最多存 7 字节 */
ring_put(&rb, 0x11);
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

完整批量接口（`ring_init` / `used` / `free` 见上一节）：

```c
size_t ring_write(ring_t *rb, const uint8_t *data, size_t n)
{
    size_t free_n = ring_free(rb);
    size_t n1, n2;

    if (n > free_n)
        n = free_n;
    if (n == 0)
        return 0;

    n1 = rb->cap - rb->w;
    if (n1 > n)
        n1 = n;
    memcpy(rb->buf + rb->w, data, n1);

    n2 = n - n1;
    if (n2)
        memcpy(rb->buf, data + n1, n2);

    rb->w = (rb->w + n) % rb->cap;
    return n;
}

size_t ring_read(ring_t *rb, uint8_t *data, size_t n)
{
    size_t used_n = ring_used(rb);
    size_t n1, n2;

    if (n > used_n)
        n = used_n;
    if (n == 0)
        return 0;

    n1 = rb->cap - rb->r;
    if (n1 > n)
        n1 = n;
    memcpy(data, rb->buf + rb->r, n1);

    n2 = n - n1;
    if (n2)
        memcpy(data + n1, rb->buf, n2);

    rb->r = (rb->r + n) % rb->cap;
    return n;
}
```

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

### 由 Q1 → 双向链表实现（简化 FIFO）

数组 ring = 固定槽 + 下标取模；链表版 = 同一套 `put`/`get`，里面换成节点。空：`head == NULL`；满：`count >= LQ_MAX`（或池空）。

对比一句话：数组好缓存、无分配；链表好变长/动态，但散落内存、有分配成本。纯 FIFO 其实单链就够，双向多一个 `prev`，方便以后从尾删。容量两边都定死 `LQ_MAX`，结构体不再带 `max` 成员。

下面两版**分开写**：A 用 `malloc`（手写快）；B 预开节点数组（嵌入式常用，无堆）。

#### 方案 A：`malloc` / `free`

```c
#include <stdlib.h>
#include <stdint.h>

#define LQ_MAX 64

typedef struct node {
    struct node *prev;
    struct node *next;
    uint8_t      data;
} node_t;

typedef struct {
    node_t *head;   /* get 从这出 */
    node_t *tail;   /* put 往这进 */
    size_t  count;
} list_queue_t;

void lq_init(list_queue_t *q)
{
    q->head = q->tail = NULL;
    q->count = 0;
}

int lq_put(list_queue_t *q, uint8_t byte)
{
    node_t *n;

    if (q->count >= LQ_MAX)
        return -1;                 /* full */
    n = malloc(sizeof(*n));
    if (!n)
        return -1;
    n->data = byte;
    n->next = NULL;
    n->prev = q->tail;
    if (q->tail)
        q->tail->next = n;
    else
        q->head = n;               /* was empty */
    q->tail = n;
    q->count++;
    return 0;
}

int lq_get(list_queue_t *q, uint8_t *out)
{
    node_t *n = q->head;

    if (!n)
        return -1;                 /* empty */
    *out = n->data;
    q->head = n->next;
    if (q->head)
        q->head->prev = NULL;
    else
        q->tail = NULL;            /* now empty */
    q->count--;
    free(n);
    return 0;
}
```

#### 方案 B：预先开节点数组（无 `malloc`，无 `free_head`）

`free_head` **不是必须的**：它只是把空闲节点串成链，取还都是 O(1)。去掉后用 `used` 标记，`put` 时扫一遍 `pool` 找空槽即可（`LQ_MAX` 不大时完全够用）。

满：`count >= LQ_MAX`（或扫不到空槽）。

```c
#include <stdint.h>
#include <stddef.h>

#define LQ_MAX 64

typedef struct node {
    struct node *prev;
    struct node *next;
    uint8_t      data;
    uint8_t      used;   /* 0 free, 1 in queue */
} node_t;

typedef struct {
    node_t *head;        /* get 从这出 */
    node_t *tail;        /* put 往这进 */
    size_t  count;
    node_t  pool[LQ_MAX];
} list_queue_t;

void lq_init(list_queue_t *q)
{
    size_t i;

    q->head = q->tail = NULL;
    q->count = 0;
    for (i = 0; i < LQ_MAX; i++) {
        q->pool[i].prev = q->pool[i].next = NULL;
        q->pool[i].used = 0;
    }
}

static node_t *lq_find_free(list_queue_t *q)
{
    size_t i;

    for (i = 0; i < LQ_MAX; i++) {
        if (!q->pool[i].used)
            return &q->pool[i];
    }
    return NULL;
}

int lq_put(list_queue_t *q, uint8_t byte)
{
    node_t *n = lq_find_free(q);

    if (!n)
        return -1;                 /* full */
    n->used = 1;
    n->data = byte;
    n->next = NULL;
    n->prev = q->tail;
    if (q->tail)
        q->tail->next = n;
    else
        q->head = n;
    q->tail = n;
    q->count++;
    return 0;
}

int lq_get(list_queue_t *q, uint8_t *out)
{
    node_t *n = q->head;

    if (!n)
        return -1;                 /* empty */
    *out = n->data;
    q->head = n->next;
    if (q->head)
        q->head->prev = NULL;
    else
        q->tail = NULL;
    q->count--;

    n->prev = n->next = NULL;
    n->used = 0;
    return 0;
}
```

**口述**：对外契约不变；字节流选数组 ring，变长消息才考虑链表；嵌入式优先方案 B。

### 由 Q2 → C 原地倒转

```c
#include <string.h>

void str_reverse(char *s)
{
    char *l = s;
    char *r;
    char  t;

    if (!s || !*s)
        return;

    r = s + strlen(s) - 1;
    while (l < r) {
        t = *l;
        *l = *r;
        *r = t;
        l++;
        r--;
    }
}
```

```c
char buf[] = "hello";   /* 可写数组；勿用 char *p = "hello" */
str_reverse(buf);       /* → "olleh" */
```

**标准库**：`<string.h>` **没有** `strrev` / `strreverse`（ISO C、POSIX 皆无）。部分旧编译器 / MSVC 有非标准 `strrev`；Linux glibc、常见嵌入式 newlib 通常没有。面试答「自己写双指针」即可。

### 由 Q2 → RV32 汇编（`a0` = `s`）

```asm
# RV32I: reverse NUL-terminated string in place
    .globl  str_reverse
    .text
str_reverse:
    beqz    a0, .Ldone
    lbu     t0, 0(a0)
    beqz    t0, .Ldone          # empty ""

    mv      t1, a0              # left
    mv      t2, a0
.Lfind_end:
    lbu     t0, 0(t2)
    beqz    t0, .Lgot_end
    addi    t2, t2, 1
    j       .Lfind_end
.Lgot_end:
    addi    t2, t2, -1          # right = last char

.Lswap:
    bgeu    t1, t2, .Ldone      # left >= right → stop
    lbu     t3, 0(t1)
    lbu     t4, 0(t2)
    sb      t4, 0(t1)
    sb      t3, 0(t2)
    addi    t1, t1, 1
    addi    t2, t2, -1
    j       .Lswap

.Ldone:
    ret
```

**找尾循环（对应 `while (*r) r++; r--;`）**：`t2` 走到 `'\0'` 后必须 `-1`，才指最后一个有效字符；`'\0'` 本身不参与交换。空串若未在函数开头返回，`t2--` 会退到 `s` 前面——故先判 `!*s`。

**交换**：与 C 双指针相同；地址比较用无符号 `bgeu`/`bltu`。

### 由 Q2 → `lbu` 还是 `lb`？

| | `lb` | `lbu` |
|--|------|-------|
| 扩展 | 符号扩展到 32 位 | 零扩展到 32 位 |
| 内存 `0xFF` | `0xFFFFFFFF` | `0x000000FF` |
| 再 `sb` 写回 | 只写低 8 位，**两种一样** | 同左 |

对本题「只 swap、不当整数运算」：**`lb` / `lbu` 结果等价**。

- C 接口若是 `char *`（多数工具链里 `char` 有符号），语义上更贴 `lb`。
- 写 `lbu` 多半是「当原始字节零扩展」的习惯；**不是**因为标准库 reverse 规定如此（库里根本没有标准 reverse）。
- 库里其它按字节扫描的优化实现（如 `strlen`）常零扩展，那是找 `'\0'` / 模式匹配的实现习惯，不能反推「必须 `lbu`」。

口述收口：swap 用哪个都行；若强调与 `char *` 一致就说 `lb`，若强调无符号字节就说 `lbu`，并点明对本题无功能差别。

### 由 Q3 → 纯圆环：第一次相遇的步数即长度

快指针每轮多走 1 步，相对慢指针「多走的距离」第一次凑满一整圈时相遇，轮数 = 节点数。

```c
typedef struct node {
    struct node *next;
} node_t;

int circ_len(node_t *head)
{
    node_t *slow, *fast;
    int n;

    if (!head)
        return 0;
    if (head->next == head)
        return 1;

    slow = head;
    fast = head;
    n = 0;
    do {
        slow = slow->next;
        fast = fast->next->next;
        n++;
    } while (slow != fast);

    return n;
}
```

---

## 小结

1. Ring buffer 面试分三层：**契约**（满策略 / 线程模型）→ **空满判定**（浪费一格或 count）→ **批量绕回**（两段拷贝）。  
2. 对外只暴露 `put`/`get` 或 `write`/`read`；返回值区分满、空、短读写。  
3. 默认手写：**浪费一格 + 满则失败 + 单线程**；追问再升级 SPSC / 2 的幂。  
4. **双向链表**简化为尾进头出 FIFO：方案 A（`malloc`）与方案 B（节点池）分开展示；字节流仍优先数组 ring。  
5. **字符串倒转**：标准库无 `strrev`；C / RV32 都是双指针；找尾后 `-1`；`lb`/`lbu` 对纯 swap 等价。  
6. **纯圆环长度**：快慢指针从 `head` 同出，第一次相遇时的步数即长度。  
7. 加分点：契约、短写、空满自测、数组 vs 链表、以及 `lb`/`lbu` 讲清楚。
