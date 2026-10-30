/*
 * SoC L1 (local) + L2 (hashed) dual-view memory allocator (interview skeleton)
 *
 * Physical: 4 x 1MB banks
 * L1: contiguous slices in one bank (128B granule)
 * L2: contiguous SoC stripes; each 512B stripe = 128B x 4 banks at same index
 *
 * Build:  cc -O2 -Wall -Wextra -o soc_alloc soc_l1_l2_allocator.c
 * Demo:   ./soc_alloc
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_BANKS           4
#define BANK_SIZE           (1u << 20)          /* 1 MB */
#define SLICE_SIZE          128u
#define STRIPE_SIZE         512u                /* 4 * SLICE_SIZE */
#define SLICES_PER_BANK     (BANK_SIZE / SLICE_SIZE)   /* 8192 */
#define NUM_STRIPES         SLICES_PER_BANK

#define MAX_ALLOCS          256

typedef enum {
    ALLOC_NONE = 0,
    ALLOC_L1,
    ALLOC_L2
} alloc_type_t;

typedef struct {
    alloc_type_t type;
    int          in_use;
    int          bank;          /* L1 only */
    uint32_t     base_stripe;   /* first stripe index */
    uint32_t     n_stripes;     /* L1: #slices; L2: #stripes */
    uint32_t     local_off;     /* L1: byte offset in bank */
    uint32_t     soc_addr;      /* L2: SoC byte address */
    uint32_t     bytes;
} alloc_rec_t;

typedef struct {
    /* bit m set => Memory_m slice at this stripe is occupied */
    uint8_t used_mask[NUM_STRIPES];
    alloc_rec_t table[MAX_ALLOCS];
} allocator_t;

static allocator_t g_alloc;

static uint32_t align_up_u32(uint32_t v, uint32_t a)
{
    return (v + a - 1u) / a * a;
}

static int stripe_fully_free(const allocator_t *A, uint32_t s)
{
    return A->used_mask[s] == 0;
}

static int stripe_bank_free(const allocator_t *A, uint32_t s, int bank)
{
    return (A->used_mask[s] & (1u << bank)) == 0;
}

static int stripe_is_dirty(const allocator_t *A, uint32_t s)
{
    return A->used_mask[s] != 0;
}

static void mark_l1(allocator_t *A, int bank, uint32_t base, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        A->used_mask[base + i] |= (uint8_t)(1u << bank);
}

static void unmark_l1(allocator_t *A, int bank, uint32_t base, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        A->used_mask[base + i] &= (uint8_t)~(1u << bank);
}

static void mark_l2(allocator_t *A, uint32_t base, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        A->used_mask[base + i] = 0x0F; /* all 4 banks */
}

static void unmark_l2(allocator_t *A, uint32_t base, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        A->used_mask[base + i] = 0;
}

/*
 * L1 placement policy (utilization):
 *  1) Prefer already-dirty stripes (L2 already poisoned).
 *  2) Else take clean stripes from the low end.
 */
static int find_l1(const allocator_t *A, int bank, uint32_t n, uint32_t *out_base)
{
    uint32_t s, run, start;

    /* Pass 1: windows entirely in dirty stripes with target bank free. */
    run = 0;
    start = 0;
    for (s = 0; s < NUM_STRIPES; s++) {
        if (stripe_is_dirty(A, s) && stripe_bank_free(A, s, bank)) {
            if (run == 0)
                start = s;
            run++;
            if (run >= n) {
                *out_base = start;
                return 1;
            }
        } else {
            run = 0;
        }
    }

    /* Pass 2: any free bank slices, search from low address. */
    run = 0;
    for (s = 0; s < NUM_STRIPES; s++) {
        if (stripe_bank_free(A, s, bank)) {
            if (run == 0)
                start = s;
            run++;
            if (run >= n) {
                *out_base = start;
                return 1;
            }
        } else {
            run = 0;
        }
    }

    return 0;
}

/* L2: search fully-free stripe runs from the high end (opposite of L1). */
static int find_l2(const allocator_t *A, uint32_t n, uint32_t *out_base)
{
    uint32_t s;
    uint32_t run = 0;
    uint32_t end = 0; /* exclusive end of current run while scanning downward */

    if (n == 0 || n > NUM_STRIPES)
        return 0;

    for (s = NUM_STRIPES; s-- > 0; ) {
        if (stripe_fully_free(A, s)) {
            if (run == 0)
                end = s + 1;
            run++;
            if (run >= n) {
                *out_base = end - n;
                return 1;
            }
        } else {
            run = 0;
        }
        if (s == 0)
            break;
    }
    return 0;
}

static int alloc_slot(allocator_t *A)
{
    int i;
    for (i = 0; i < MAX_ALLOCS; i++) {
        if (!A->table[i].in_use)
            return i;
    }
    return -1;
}

void allocator_init(allocator_t *A)
{
    memset(A, 0, sizeof(*A));
}

/*
 * alloc_l1: contiguous bytes in one bank.
 * On success returns handle >= 0; fills *local_off_out with bank byte offset.
 */
int alloc_l1(allocator_t *A, int memory_id, uint32_t size, uint32_t *local_off_out)
{
    uint32_t nbytes, nslice, base;
    int h;

    if (!A || memory_id < 0 || memory_id >= NUM_BANKS || size == 0)
        return -1;

    nbytes = align_up_u32(size, SLICE_SIZE);
    nslice = nbytes / SLICE_SIZE;

    if (!find_l1(A, memory_id, nslice, &base))
        return -1;

    h = alloc_slot(A);
    if (h < 0)
        return -1;

    mark_l1(A, memory_id, base, nslice);

    A->table[h].in_use = 1;
    A->table[h].type = ALLOC_L1;
    A->table[h].bank = memory_id;
    A->table[h].base_stripe = base;
    A->table[h].n_stripes = nslice;
    A->table[h].local_off = base * SLICE_SIZE;
    A->table[h].soc_addr = 0;
    A->table[h].bytes = nbytes;

    if (local_off_out)
        *local_off_out = A->table[h].local_off;
    return h;
}

/*
 * alloc_l2: contiguous SoC address range (hashed across 4 banks).
 * On success returns handle >= 0; fills *soc_addr_out.
 */
int alloc_l2(allocator_t *A, uint32_t size, uint32_t *soc_addr_out)
{
    uint32_t nbytes, nstripe, base;
    int h;

    if (!A || size == 0)
        return -1;

    nbytes = align_up_u32(size, STRIPE_SIZE);
    nstripe = nbytes / STRIPE_SIZE;

    if (!find_l2(A, nstripe, &base))
        return -1;

    h = alloc_slot(A);
    if (h < 0)
        return -1;

    mark_l2(A, base, nstripe);

    A->table[h].in_use = 1;
    A->table[h].type = ALLOC_L2;
    A->table[h].bank = -1;
    A->table[h].base_stripe = base;
    A->table[h].n_stripes = nstripe;
    A->table[h].local_off = 0;
    A->table[h].soc_addr = base * STRIPE_SIZE;
    A->table[h].bytes = nbytes;

    if (soc_addr_out)
        *soc_addr_out = A->table[h].soc_addr;
    return h;
}

int free_alloc(allocator_t *A, int handle)
{
    alloc_rec_t *r;

    if (!A || handle < 0 || handle >= MAX_ALLOCS)
        return -1;
    r = &A->table[handle];
    if (!r->in_use)
        return -1;

    if (r->type == ALLOC_L1)
        unmark_l1(A, r->bank, r->base_stripe, r->n_stripes);
    else if (r->type == ALLOC_L2)
        unmark_l2(A, r->base_stripe, r->n_stripes);
    else
        return -1;

    memset(r, 0, sizeof(*r));
    return 0;
}

/* --- helpers for demo / self-check --- */

static uint32_t count_fully_free_stripes(const allocator_t *A)
{
    uint32_t s, c = 0;
    for (s = 0; s < NUM_STRIPES; s++) {
        if (stripe_fully_free(A, s))
            c++;
    }
    return c;
}

static void dump_stripe_range(const allocator_t *A, uint32_t from, uint32_t to)
{
    uint32_t s;
    printf("stripes[%u..%u): ", from, to);
    for (s = from; s < to && s < NUM_STRIPES; s++)
        printf("%x", A->used_mask[s] & 0xF);
    printf("\n");
}

int main(void)
{
    uint32_t off0, off1, soc;
    int h0, h1, h2, h3;

    allocator_init(&g_alloc);

    /* L1 on Memory0: 128B -> poisons stripe 0 for L2 */
    h0 = alloc_l1(&g_alloc, 0, 128, &off0);
    printf("L1 mem0 128B  handle=%d off=0x%x  free_stripes=%u\n",
           h0, off0, count_fully_free_stripes(&g_alloc));
    dump_stripe_range(&g_alloc, 0, 8);

    /* L2 512B should NOT take stripe 0; comes from high end */
    h1 = alloc_l2(&g_alloc, 512, &soc);
    printf("L2 512B       handle=%d soc=0x%x  free_stripes=%u\n",
           h1, soc, count_fully_free_stripes(&g_alloc));

    /* Pack another L1 into already-dirty stripe 0 on Memory1 */
    h2 = alloc_l1(&g_alloc, 1, 128, &off1);
    printf("L1 mem1 128B  handle=%d off=0x%x (prefer dirty)\n", h2, off1);
    dump_stripe_range(&g_alloc, 0, 8);

    /* Larger L2 */
    h3 = alloc_l2(&g_alloc, 2048, &soc);
    printf("L2 2048B      handle=%d soc=0x%x\n", h3, soc);

    free_alloc(&g_alloc, h0);
    free_alloc(&g_alloc, h2);
    printf("after free L1s, stripe0 mask=%x free_stripes=%u\n",
           g_alloc.used_mask[0], count_fully_free_stripes(&g_alloc));

    free_alloc(&g_alloc, h1);
    free_alloc(&g_alloc, h3);
    printf("after free all, free_stripes=%u (expect %u)\n",
           count_fully_free_stripes(&g_alloc), NUM_STRIPES);

    return 0;
}
