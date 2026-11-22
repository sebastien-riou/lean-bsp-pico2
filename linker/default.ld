/* The default linker script, under the name lean-bsp's other boards give theirs, so that an application's
 * own script can end with `INCLUDE default.ld` on every board (for example one that adds INPUT(<archive>)
 * before it). Here it is pico-sdk's, plus NVM region 0: CMakeLists.txt puts this directory and pico-sdk's
 * linker script directory (pico_crt0/<chip>) on the linker's search path. */
INCLUDE memmap_default.ld

/* NVM region 0: the upper half of the 4 MB flash (src/bsp_nvm.c). The application's data there is laid
 * out at link time, in the sections named .bsp_nvm0; nothing is loaded there, so a load keeps it. */
MEMORY
{
  NVM0 (rw) : ORIGIN = 0x10200000, LENGTH = 2048k
}

SECTIONS
{
  .bsp_nvm0 (NOLOAD) :
  {
    KEEP(*(.bsp_nvm0))
    KEEP(*(.bsp_nvm0.*))
  } > NVM0
}

ASSERT(__flash_binary_end <= ORIGIN(NVM0), "the image reaches into NVM region 0: it must fit the lower 2 MB of flash")
