/* bsp_heap.c - lean-bsp heap use (bsp_heap_usage_reset, bsp_heap_usage), since contract 1.4
 *
 * Shipped by lean-bsp. Every BSP keeps a byte-identical copy at src/bsp_heap.c, which `lean-bsp
 * check` verifies, compiles it, links with
 *     -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
 * and wraps these four functions no other way. It follows crypto-benchmark's common/lbmk_heap.c,
 * so that a benchmark counts its heap the same on a lean-bsp board as it did before.
 *
 * Heap use is how much more the code had allocated at its worst moment than when the count was
 * reset, counted on the sizes requested from malloc, calloc and realloc. It depends neither on the
 * allocator nor on the order of the requests.
 * - live: the sum of the sizes requested for the blocks recorded and not freed yet, all the blocks
 *   since the start of the firmware, not only those since the reset.
 * - bsp_heap_usage_reset() takes live as the baseline; it does not zero live.
 * - bsp_heap_usage() is the most live has been since the reset, minus the baseline: never negative.
 *   A block allocated before the reset and freed after it lowers live, possibly below the
 *   baseline, so allocating that much again does not raise the figure.
 * - malloc(n) adds n, calloc(count, n) adds count*n, when they return a block, malloc(0) included
 *   (a block of size 0). A failed allocation adds nothing; count*n cannot overflow when calloc
 *   succeeds, as C requires calloc to fail then.
 * - realloc(p, n) that returns a block replaces the size of p by n; the moment both blocks exist
 *   during a copy is not counted. A failed realloc(p, n) with n > 0 leaves p as it was.
 *   realloc(NULL, n) is malloc(n). realloc(p, 0) that returns NULL freed p (newlib, glibc); if it
 *   returns a block, that block counts as a block of size 0.
 * - free(NULL) changes nothing, nor does free(p) of a block that was never recorded.
 * - Only these four functions are counted, under these names, and only where the code linked with
 *   the wrap options calls them: memalign, aligned_alloc and the entry points the C library uses
 *   for itself (newlib's _malloc_r for its stdio buffers, glibc's internal calls) are not.
 * - The table of live blocks has LEAN_BSP_HEAP_MAX_BLOCKS entries (256 unless the BSP defines
 *   it), so that free knows the size it gives back and every call takes a time bounded by that
 *   number. When it is full, the allocation still succeeds: the block is returned but never
 *   recorded, and bsp_heap_usage() returns SIZE_MAX, the figure being unknown, until the next
 *   reset.
 * - No lock, and interrupts are left as they are: nothing may allocate or free from an interrupt
 *   handler, another core or another thread while the count matters.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp_api.h"

#ifndef LEAN_BSP_HEAP_MAX_BLOCKS
#define LEAN_BSP_HEAP_MAX_BLOCKS 256
#endif

/* noipa: one copy of each function below, that the optimizer neither inlines nor specializes, as
 * in crypto-benchmark's file; plain noinline where the compiler does not know noipa */
#if defined(__GNUC__) && !defined(__clang__)
#define BSP_HEAP_NOIPA __attribute__((noipa))
#else
#define BSP_HEAP_NOIPA __attribute__((noinline))
#endif

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *mem, size_t size);
void __real_free(void *mem);

static void *block_ptr[LEAN_BSP_HEAP_MAX_BLOCKS];
static size_t block_size[LEAN_BSP_HEAP_MAX_BLOCKS];
static unsigned int nblocks;
static size_t live;     /* bytes recorded and not freed yet */
static size_t baseline; /* live at the last reset */
static size_t peak;     /* the most live since then */
static bool overflow;   /* a block could not be recorded since then */

static void add_block(void *mem, size_t size) {
    if (!mem) return;
    if (nblocks == LEAN_BSP_HEAP_MAX_BLOCKS) {
        overflow = true;
        return;
    }
    block_ptr[nblocks] = mem;
    block_size[nblocks] = size;
    nblocks++;
    live += size;
    if (live > peak) peak = live;
}

static void remove_block(void *mem) {
    if (!mem) return;
    for (unsigned int i = 0; i < nblocks; i++) {
        if (block_ptr[i] == mem) {
            live -= block_size[i];
            nblocks--;
            block_ptr[i] = block_ptr[nblocks];
            block_size[i] = block_size[nblocks];
            return;
        }
    }
}

void bsp_heap_usage_reset(void) {
    baseline = live;
    peak = live;
    overflow = false;
}

size_t bsp_heap_usage(void) {
    if (overflow) return SIZE_MAX;
    return peak - baseline;
}

/* The count, called by the wrappers. */
BSP_HEAP_NOIPA void *bsp_heap_allocated(void *mem, size_t size) {
    add_block(mem, size);
    return mem;
}

BSP_HEAP_NOIPA void *bsp_heap_reallocated(void *old_mem, void *new_mem, size_t size) {
    if (new_mem || 0 == size) remove_block(old_mem);
    add_block(new_mem, size);
    return new_mem;
}

BSP_HEAP_NOIPA void *bsp_heap_freed(void *mem) {
    remove_block(mem);
    return mem;
}

/* The wrappers, linked in only for the functions the firmware calls. */
BSP_HEAP_NOIPA void *__wrap_malloc(size_t size) {
    return bsp_heap_allocated(__real_malloc(size), size);
}

BSP_HEAP_NOIPA void *__wrap_calloc(size_t count, size_t size) {
    return bsp_heap_allocated(__real_calloc(count, size), count * size);
}

BSP_HEAP_NOIPA void *__wrap_realloc(void *mem, size_t size) {
    return bsp_heap_reallocated(mem, __real_realloc(mem, size), size);
}

BSP_HEAP_NOIPA void __wrap_free(void *mem) {
    __real_free(bsp_heap_freed(mem));
}
