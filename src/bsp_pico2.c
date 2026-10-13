/* Raspberry Pi Pico 2: the lean-bsp contract on top of pico-sdk, for both RP2350 cores.
 *
 * UART 0 is the USB serial port the RP2350 serves itself (pico_stdio_usb), so it exists only while
 * the firmware services USB, and so does the vendor reset interface picotool reaches the board
 * through. Hence the three rules this file follows:
 *  - main() waits for a host to open the port (DTR) before the application starts: nothing it
 *    prints is lost, whenever the host comes;
 *  - UART 0 enables interrupts while it moves bytes, since only the USB interrupt does;
 *  - once app_main returns, main() parks with interrupts enabled, so the board stays reachable. */
#include <limits.h>

#include "bsp_api.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/io_qspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "hardware/xip_cache.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "tusb.h"

#ifdef __arm__
#include "hardware/structs/m33.h"
#endif

static const bsp_capabilities_t capabilities = {
    .board_name = "pico2",
    .n_uarts = 1,
    .n_leds = 1,
    .n_buttons = 1,
    .has_cycle_counter = true,
    .n_nvm_regions = 0,
    .nvm_regions = NULL,
};

const bsp_capabilities_t *bsp_get_capabilities(void) { return &capabilities; }

/* ---- the application hooks' defaults ---- */

__attribute__((weak)) void app_init(void) {}

/* app_tick's default is an alias, so main() can tell whether the application overrides it and
 * not run a 1 ms timer interrupt for nothing. */
void lean_bsp_default_app_tick(uint32_t tick_ms) { (void)tick_ms; }
void app_tick(uint32_t tick_ms) __attribute__((weak, alias("lean_bsp_default_app_tick")));

__attribute__((weak)) void app_on_fault(uint32_t reason, uintptr_t pc) {
    (void)reason;
    (void)pc;
}

__attribute__((weak)) void app_on_exit(int code) { (void)code; }  /* main() then parks the CPU */

/* ---- UART 0: the RP2350's own USB serial port; stdin and stdout too ---- */

/* Bytes move only from the USB interrupt: with interrupts masked, output would wait in TinyUSB's
 * buffer, or be dropped once it is full. So interrupts are let in for the duration of the call. */
static void usb_out(const char *buf, int len) {
    const uint32_t saved = save_and_disable_interrupts();
    enable_interrupts();
    stdio_usb.out_chars(buf, len);
    restore_interrupts(saved);
}

static int usb_in(char *buf, int len) {
    const uint32_t saved = save_and_disable_interrupts();
    enable_interrupts();
    const int n = stdio_usb.in_chars(buf, len);
    restore_interrupts(saved);
    return n;
}

/* Until the host took everything queued, or went away: bsp_reset() would otherwise cut it off. */
static void usb_drain(void) {
    const uint32_t saved = save_and_disable_interrupts();
    enable_interrupts();
    stdio_usb.out_flush();
    while (stdio_usb_connected() && tud_cdc_write_available() < CFG_TUD_CDC_TX_BUFSIZE) tight_loop_contents();
    restore_interrupts(saved);
}

static void lean_out_flush(void) { stdio_usb.out_flush(); }

/* printf and getchar go through the same path as bsp_uart_tx/rx, bytes as they are: no "\r"
 * added before "\n", as on the other boards. */
static stdio_driver_t stdio_lean = {
    .out_chars = usb_out,
    .out_flush = lean_out_flush,
    .in_chars = usb_in,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = false,
#endif
};

void bsp_uart_tx(unsigned port, const void *buf, size_t len) {
    if (port != 0) return;
    const char *p = buf;
    while (len) {
        const int n = len > INT_MAX ? INT_MAX : (int)len;
        usb_out(p, n);
        p += n;
        len -= (size_t)n;
    }
}

size_t bsp_uart_rx(unsigned port, void *buf, size_t len) {
    if (port != 0 || len == 0) return 0;
    const int n = usb_in(buf, len > INT_MAX ? INT_MAX : (int)len);
    return n > 0 ? (size_t)n : 0;
}

/* ---- the LED and the BOOTSEL button ---- */

void bsp_led_set(unsigned id, bool on) {
    if (id == 0) gpio_put(PICO_DEFAULT_LED_PIN, on);
}

/* BOOTSEL drives the flash chip select low: read it by releasing that pin for a moment, so with
 * flash unreachable, which is why this runs from RAM with interrupts masked (pico-examples,
 * picoboard/button). */
static bool __no_inline_not_in_flash_func(bootsel_pressed)(void) {
    const unsigned cs = 1;
    const uint32_t saved = save_and_disable_interrupts();
    hw_write_masked(&io_qspi_hw->io[cs].ctrl, GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 1000; ++i) {
    }
    const bool pressed = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);
    hw_write_masked(&io_qspi_hw->io[cs].ctrl, GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(saved);
    return pressed;
}

bool bsp_button_get(unsigned id) { return id == 0 && bootsel_pressed(); }

/* ---- time: the core's cycle counter, at clk_sys ---- */

#ifdef __arm__
static void cycle_counter_start(void) {
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
    m33_hw->dwt_cyccnt = 0;
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
}

uint32_t bsp_cycle_count(void) { return m33_hw->dwt_cyccnt; }
#else
static void cycle_counter_start(void) {
    __asm volatile("csrw mcountinhibit, zero");  /* Hazard3 boots with its counters stopped */
}

uint32_t bsp_cycle_count(void) {
    uint32_t cycles;
    __asm volatile("csrr %0, mcycle" : "=r"(cycles));
    return cycles;
}
#endif

uint32_t bsp_cpu_freq_hz(void) { return clock_get_hz(clk_sys); }

/* On the 64-bit microsecond timer: works with interrupts masked, and is bounded. */
void bsp_delay_ms(uint32_t ms) { busy_wait_ms(ms); }

void bsp_reset(void) {
    usb_drain();
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

/* The XIP cache in front of the flash; the cores have no cache of their own. */
void bsp_clear_caches(void) {
    xip_cache_invalidate_all();
    __dmb();
    __dsb();
    __isb();
}

/* ---- heap usage: how far the heap end moved since the last reset ---- */

extern char end;           /* the heap's start, from the linker script */
extern char __StackLimit;  /* its limit */
static char *heap_end;
static char *heap_mark;
static char *heap_peak;

/* pico-sdk's own _sbrk (pico_clib_interface), plus the high-water mark. */
void *_sbrk(int incr) {
    if (heap_end == NULL) heap_end = &end;
    char *const prev = heap_end;
    char *next = heap_end + incr;
    if (next > &__StackLimit) {
#if PICO_USE_OPTIMISTIC_SBRK
        if (heap_end == &__StackLimit) return (void *)-1;
        next = &__StackLimit;
#else
        return (void *)-1;
#endif
    }
    heap_end = next;
    if (heap_end > heap_peak) heap_peak = heap_end;
    return prev;
}

void bsp_heap_usage_reset(void) {
    heap_mark = heap_end == NULL ? &end : heap_end;
    heap_peak = heap_mark;
}

size_t bsp_heap_usage(void) { return heap_peak > heap_mark ? (size_t)(heap_peak - heap_mark) : 0; }

/* ---- faults: tell the application, then stop the core ----
 * The board is then unreachable over USB until a power cycle (the hub's, through lean-hwbridge):
 * the firmware then services USB again while it waits for a host, and picotool can reach it. */

#ifdef __arm__
void isr_hardfault(void) {
    app_on_fault(APP_FAULT_HARD, 0);
    for (;;) {
    }
}
#else
/* Every synchronous exception lands here, straight from the vector table: this never returns, so
 * it needs no trap frame of its own. In RAM, as the vector table is: its `j` cannot reach flash. */
void __not_in_flash_func(isr_riscv_machine_exception)(void) {
    uintptr_t pc;
    __asm volatile("csrr %0, mepc" : "=r"(pc));
    app_on_fault(APP_FAULT_HARD, pc);
    for (;;) {
    }
}
#endif

/* ---- the firmware's entry point ---- */

static bool tick(repeating_timer_t *timer) {
    (void)timer;
    app_tick(to_ms_since_boot(get_absolute_time()));
    return true;
}

int main(void) {
    stdio_init_all();  /* USB: the serial port, and the reset interface picotool uses */
    stdio_set_driver_enabled(&stdio_lean, true);
    stdio_filter_driver(&stdio_lean);
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    cycle_counter_start();
    bsp_heap_usage_reset();
    /* No timeout on purpose: the application's whole output goes to whoever opens the port. */
    while (!stdio_usb_connected()) tight_loop_contents();

    const uint32_t saved = save_and_disable_interrupts();
    app_init();
    restore_interrupts(saved);
    static repeating_timer_t tick_timer;
    if (app_tick != lean_bsp_default_app_tick) add_repeating_timer_ms(-1, tick, NULL, &tick_timer);

    app_on_exit(app_main());

    /* Park, with USB still serviced whatever the application left masked: picotool can then
     * reboot the board into BOOTSEL and load the next firmware. */
    usb_drain();
    enable_interrupts();
    for (;;) tight_loop_contents();
}
