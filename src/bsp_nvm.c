/* NVM region 0: the upper half of the Pico 2's 4 MB QSPI flash (Winbond W25Q32RV), which the image
 * never occupies (linker/default.ld checks it).
 *
 * Through pico-sdk's hardware_flash, which runs from RAM, takes the flash out of XIP mode for each
 * operation and flushes the XIP cache after it. Core 1 is never started, so nothing else runs from
 * flash meanwhile, and interrupts are masked during each operation only (USB, whose handler runs
 * from flash, is serviced in between). No erase or program suspend: the operations block.
 *  - erase: sector by sector (4 KiB; 30 ms typical, 240 ms at most), then each sector must read
 *    all 0xFF: the flash reports only that it is busy, never that an operation succeeded.
 *  - write: every destination byte must read erased first, or nothing is programmed and the write
 *    returns BSP_NVM_REFUSED: the W25Q32RV specifies programming for erased locations only, hence
 *    no zero_overwrite. Then one program per write unit, in ascending order: a page program puts
 *    all its bytes in flight at once, so a power cut during a program of several units could leave
 *    all of them weak, where lean-ftl's analysis assumes the units of a write programmed one after
 *    the other. pico-sdk programs whole 256-byte pages: each program is the unit's page with 0xFF
 *    around the unit. That relies on NOR physics, not on a datasheet sentence: a 1 bit programs
 *    nothing, so the bytes already programmed in the page stay as they are. Never pad with the
 *    page's current content. Then read back.
 *  - read: memcpy through XIP.
 * Status: BSP_NVM_OK; 1, 2 outside the region; 3 misaligned address; 4 size not a multiple of the
 * program unit; 5 a sector or a write read back different after the operation; BSP_NVM_REFUSED a
 * destination byte was not erased, nothing programmed.
 * The flash ignores program and erase instructions for up to 5 ms after power-up (tPUW); the
 * read back catches one that was lost. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bsp_api.h"
#include "bsp_nvm.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#define NVM0_OFFSET     (2u * 1024u * 1024u) /* from the start of the flash */
/* The bounds can be given at build time, BSP_NVM0_BASE and BSP_NVM0_SIZE, by a build that takes
 * these accessors without the BSP around them (a prebuilt library for this part that offers the
 * whole flash, for instance). A lean-bsp build defines neither: the region is the upper half of
 * the flash, as bsp.toml and the linker script say. */
#if defined(BSP_NVM0_BASE) != defined(BSP_NVM0_SIZE)
#error "define both BSP_NVM0_BASE and BSP_NVM0_SIZE, or neither"
#endif
#ifdef BSP_NVM0_BASE
#define NVM0_BASE  BSP_NVM0_BASE
#define NVM0_SIZE  BSP_NVM0_SIZE
#define NVM0_NAME  "build-time-bounds"
#else
#define NVM0_BASE  (XIP_BASE + NVM0_OFFSET)
#define NVM0_SIZE  (PICO_FLASH_SIZE_BYTES - NVM0_OFFSET)
#define NVM0_NAME  "flash-upper-half"
#endif
#define NVM0_WRITE_SIZE BSP_NVM0_WRITE_SIZE /* bsp_nvm.h */

_Static_assert(FLASH_PAGE_SIZE % NVM0_WRITE_SIZE == 0, "a write unit must not straddle two flash pages");

static inline bool in_region(uintptr_t addr, uintptr_t size) {
    return addr >= NVM0_BASE && size <= NVM0_SIZE && addr - NVM0_BASE <= NVM0_SIZE - size;
}

static bool erased(const uint8_t *p, uintptr_t size) {
    for (uintptr_t i = 0; i < size; i++) {
        if (p[i] != 0xFFu) return false;
    }
    return true;
}

static uint8_t nvm0_erase(void *base_address, unsigned n_pages) {
    if (n_pages == 0) return BSP_NVM_OK;
    const uintptr_t base = (uintptr_t)base_address;
    if ((uintptr_t)n_pages > NVM0_SIZE / FLASH_SECTOR_SIZE) return 2;
    if (!in_region(base, (uintptr_t)n_pages * FLASH_SECTOR_SIZE)) return base < NVM0_BASE ? 1 : 2;
    if (base % FLASH_SECTOR_SIZE) return 3;
    for (unsigned i = 0; i < n_pages; i++) {
        const uintptr_t sector = base + (uintptr_t)i * FLASH_SECTOR_SIZE;
        const uint32_t saved = save_and_disable_interrupts();
        flash_range_erase((uint32_t)(sector - XIP_BASE), FLASH_SECTOR_SIZE);
        restore_interrupts(saved);
        if (!erased((const uint8_t *)sector, FLASH_SECTOR_SIZE)) return 5;
    }
    return BSP_NVM_OK;
}

static uint8_t nvm0_write(void *dst, const void *src, uintptr_t size) {
    if (size == 0) return BSP_NVM_OK;
    const uintptr_t addr = (uintptr_t)dst;
    if (!in_region(addr, size)) return addr < NVM0_BASE ? 1 : 2;
    if (addr % NVM0_WRITE_SIZE) return 3;
    if (size % NVM0_WRITE_SIZE) return 4;
    if (!erased(dst, size)) return BSP_NVM_REFUSED;
    const uint8_t *from = src;
    for (uintptr_t at = addr; at < addr + size; at += NVM0_WRITE_SIZE, from += NVM0_WRITE_SIZE) {
        const uintptr_t page = at & ~(uintptr_t)(FLASH_PAGE_SIZE - 1u);
        uint8_t buf[FLASH_PAGE_SIZE];
        memset(buf, 0xFF, sizeof buf);
        memcpy(buf + (at - page), from, NVM0_WRITE_SIZE); /* while XIP works: the source may be in flash */
        const uint32_t saved = save_and_disable_interrupts();
        flash_range_program((uint32_t)(page - XIP_BASE), buf, FLASH_PAGE_SIZE);
        restore_interrupts(saved);
    }
    return memcmp(dst, src, size) ? 5 : BSP_NVM_OK;
}

static uint8_t nvm0_read(void *dst, const void *src, uintptr_t size) {
    if (!in_region((uintptr_t)src, size)) return (uintptr_t)src < NVM0_BASE ? 1 : 2;
    memcpy(dst, src, size);
    return BSP_NVM_OK;
}

const bsp_nvm_region_t bsp_nvm_regions[BSP_N_NVM_REGIONS] = {
    {
        .name = NVM0_NAME,
        .base_address = (void *)NVM0_BASE,
        .size = NVM0_SIZE,
        .page_size = FLASH_SECTOR_SIZE,
        .write_size = NVM0_WRITE_SIZE,
        .zero_overwrite = false, /* the W25Q32RV specifies programming for erased locations only */
        .erase = nvm0_erase,
        .write = nvm0_write,
        .read = nvm0_read,
    },
};
