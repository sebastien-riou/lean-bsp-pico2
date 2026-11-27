/* bsp_api.h - lean-bsp contract 1.5
 *
 * Shared by every lean-bsp BSP and every lean-bsp application. Each BSP keeps
 * a byte-identical copy at include/bsp_api.h; `lean-bsp check` verifies it.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEAN_BSP_CONTRACT_VERSION "1.5"

/* ---- BSP -> app hooks ----
 * app_main is required and defined by the application. The BSP provides weak,
 * do-nothing defaults for the others, and an application overrides one simply
 * by defining it. These declarations are deliberately not weak: a weak
 * declaration would make the application's own definition weak as well, and
 * the winner between two weak definitions depends on link order. */
int  app_main(void);
void app_init(void);                              /* one-time, IRQs still masked */
void app_tick(uint32_t tick_ms);                  /* called from the BSP tick ISR, every millisecond */
void app_on_fault(uint32_t reason, uintptr_t pc); /* last chance before the BSP's terminal action */
void app_on_exit(int code);                       /* app_main returned code */

enum {
    APP_FAULT_HARD = 0, APP_FAULT_ASSERT, APP_FAULT_STACK_OVERFLOW,
    APP_FAULT_WATCHDOG, APP_FAULT_UNKNOWN
};

/* ---- app -> BSP primitives: strong, BSP-owned, mandatory ----
 * Standard output and input are UART 0 on every BSP. */
void      bsp_uart_tx(unsigned port, const void *buf, size_t len);   /* blocking */
size_t    bsp_uart_rx(unsigned port, void *buf, size_t len);         /* non-blocking, returns n */
void      bsp_led_set(unsigned id, bool on);
bool      bsp_button_get(unsigned id);
uint32_t  bsp_cycle_count(void);
uint32_t  bsp_cpu_freq_hz(void);                                     /* rate of bsp_cycle_count() */
void      bsp_delay_ms(uint32_t ms);
void      bsp_reset(void);                                           /* restart the firmware, does not return */
void      bsp_clear_caches(void);        /* invalidate I/D caches and flash prefetch; no-op without caches */
void      bsp_heap_usage_reset(void);    /* the bytes live now become the baseline (see below) */
size_t    bsp_heap_usage(void);          /* the most live bytes since the reset, minus the baseline */

/* Heap use (since contract 1.4; up to 1.3, how far the heap end had moved). Live bytes are the
 * sizes requested from malloc, calloc and realloc for the blocks not freed yet, so the figure does
 * not depend on the allocator. bsp_heap_usage() is the most they have been since the last
 * bsp_heap_usage_reset(), minus what they were at the reset; SIZE_MAX when a block could not be
 * recorded since then. Every BSP counts them with the same file, lean-bsp's bsp_heap.c, which it
 * keeps a byte-identical copy of at src/bsp_heap.c (`lean-bsp check` verifies it); its head says
 * what is counted and how. Nothing may allocate or free from an interrupt handler or from another
 * core while the count matters. */

/* ---- cycle timer: optional, bsp.toml primitive cycle_timer (since contract 1.1) ----
 * A one-shot timer counting CPU cycles. Its interrupt preempts everything else the BSP runs and
 * enters `handler` as the interrupt's own exception handler: on Cortex-M, with LR holding
 * EXC_RETURN and the interrupted context stacked, so the handler may inspect or rewrite that
 * context. The interrupt is taken `cycles` CPU cycles after the start plus a latency fixed for a
 * given BSP: one more in `cycles`, exactly one cycle later. While the timer runs, the BSP holds off
 * its other interrupts (its tick), so they add no cycles to what is being timed. */
typedef void (*bsp_cycle_timer_handler_t)(void);
void      bsp_cycle_timer_init(bsp_cycle_timer_handler_t handler);  /* once, before the first start */
void      bsp_cycle_timer_start(uint32_t cycles);  /* from 0; the interrupt follows `cycles` cycles later */
void      bsp_cycle_timer_stop(void);
uint32_t  bsp_cycle_timer_read(void);              /* CPU cycles since the last start */
void      bsp_cycle_timer_ack(void);               /* from the handler: clear the interrupt, stop the timer */

/* ---- flash/NVM: function-pointer struct, lean-ftl-compatible shape ----
 * optional: bsp.toml primitives flash_read, flash_erase, flash_write; regions declared in
 * [[bsp.nvm_regions]] (since contract 1.2).
 * A region is flash the firmware image never occupies: the application may erase and program
 * all of it. The BSP's linker script gathers the sections named .bsp_nvm<N> (region N, from 0)
 * at the region's start, without loading anything there, so that an application can lay data
 * out in the region at link time. lean-bsp passes the regions' geometry to the application's
 * compilation: LEAN_BSP_NVM_REGIONS, their number (not defined when there are none), and for
 * each LEAN_BSP_NVM<N>_BASE, _SIZE, _PAGE_SIZE, _WRITE_SIZE and _ZERO_OVERWRITE (0 or 1).
 *  - erase: erases n_pages pages from base_address, which is page-aligned; 0 pages is a no-op.
 *  - write: dst is aligned to write_size and size a multiple of it; src may be anywhere,
 *    misaligned or in the region itself (never overlapping dst). It programs only erased units,
 *    or, where zero_overwrite is true, units it then overwrites with zeros.
 *  - read: copies size bytes from the region to dst.
 * Each returns BSP_NVM_OK on success. A write returns BSP_NVM_REFUSED when the flash refused to
 * program a unit that was not erased (zero_overwrite says when it may accept zeros there, not that
 * it always does). It has then programmed nothing at or above the refused unit; the units of the
 * call below it may be programmed, as a power cut at the refused unit would have left them (since
 * 1.3; 1.2 said nothing programmed). Only a write returns BSP_NVM_REFUSED: an erase that fails
 * returns a failure (since 1.3). Any other value is a failure the BSP numbers as it likes. */
#define BSP_NVM_OK      0u
#define BSP_NVM_REFUSED 6u              /* lean-ftl's LFTL_NVM_STATUS_REFUSED */

typedef uint8_t (*bsp_nvm_erase_t)(void *base_address, unsigned n_pages);
typedef uint8_t (*bsp_nvm_write_t)(void *dst, const void *src, uintptr_t size);
typedef uint8_t (*bsp_nvm_read_t )(void *dst, const void *src, uintptr_t size);

typedef struct {
    const char       *name;             /* e.g. "internal-flash" */
    void             *base_address;
    uintptr_t         size;
    uintptr_t         page_size;        /* the erase unit */
    uint32_t          write_size;       /* the program unit, a power of two (since 1.2) */
    bool              zero_overwrite;   /* zeros may be programmed over a programmed unit (since 1.2) */
    bsp_nvm_erase_t   erase;
    bsp_nvm_write_t   write;
    bsp_nvm_read_t    read;
} bsp_nvm_region_t;

/* ---- what the board is made of (since contract 1.5) ----
 * A BSP that drives a board declares in bsp.toml its MCU ([bsp.mcu]), the parts outside it that
 * hold the application's code or data and any other it chooses to list ([[bsp.parts]]), and, for
 * a target on an Armv8-M CPU with TrustZone, the security state the application runs in. lean-bsp
 * passes them to the application's compilation:
 *  - LEAN_BSP_MCU_VENDOR and LEAN_BSP_MCU_PART, strings, e.g. "ST" and "STM32U5A5ZJT6Q" (the
 *    ordering code);
 *  - LEAN_BSP_MCU_IS_<NAME>, defined to 1 for the part and for each family the vendor puts it
 *    in, e.g. LEAN_BSP_MCU_IS_STM32U5: an application matches on a family rather than on a part;
 *  - for each outside part, of role <ROLE> (e.g. FLASH): LEAN_BSP_PART_<ROLE> and
 *    LEAN_BSP_PART_<ROLE>_VENDOR, strings, and LEAN_BSP_PART_<ROLE>_IS_<NAME>, 1, for the part
 *    and its families;
 *  - one of LEAN_BSP_TRUSTZONE_OFF, LEAN_BSP_TRUSTZONE_SECURE and LEAN_BSP_TRUSTZONE_NON_SECURE,
 *    defined to 1, when the target says it: it decides which of a peripheral's two addresses works.
 * <NAME> and <ROLE> are bsp.toml's values in upper case, with '_' for every character outside
 * A-Z and 0-9. Where bsp.toml says `checked = true` for the MCU or a part, the BSP reads at
 * startup what the part says of itself and stops before app_init when it is not the declared one.
 * The application runs in the CPU's most privileged mode. An application that drives hardware the
 * contract does not cover leaves alone the peripherals the BSP reserves ([bsp.mcu] reserved,
 * listed by `lean-bsp show-bsp`); the rest is its own once bsp_init has returned. */

/* ---- capability introspection: runtime, const, BSP-populated ---- */
typedef struct {
    const char             *board_name;
    unsigned                n_uarts, n_leds, n_buttons;
    bool                    has_cycle_counter;
    unsigned                n_nvm_regions;
    const bsp_nvm_region_t *nvm_regions;    /* NULL if n_nvm_regions == 0 */
} bsp_capabilities_t;

const bsp_capabilities_t *bsp_get_capabilities(void);

#ifdef __cplusplus
}
#endif
