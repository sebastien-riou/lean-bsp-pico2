/* bsp_api.h - lean-bsp contract 1.1
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

#define LEAN_BSP_CONTRACT_VERSION "1.1"

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
void      bsp_heap_usage_reset(void);    /* restart heap high-water-mark tracking */
size_t    bsp_heap_usage(void);          /* growth of the heap high-water mark since the last reset, in bytes */

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

/* ---- flash/NVM: function-pointer struct, lean-ftl-compatible shape ---- */
typedef uint8_t (*bsp_nvm_erase_t)(void *base_address, unsigned n_pages);
typedef uint8_t (*bsp_nvm_write_t)(void *dst, const void *src, uintptr_t size);
typedef uint8_t (*bsp_nvm_read_t )(void *dst, const void *src, uintptr_t size);

typedef struct {
    const char       *name;             /* e.g. "internal-flash" */
    void             *base_address;
    uintptr_t         size;
    uintptr_t         page_size;
    bsp_nvm_erase_t   erase;
    bsp_nvm_write_t   write;
    bsp_nvm_read_t    read;
} bsp_nvm_region_t;

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
