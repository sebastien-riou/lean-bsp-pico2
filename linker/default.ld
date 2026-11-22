/* The default linker script, under the name lean-bsp's other boards give theirs, so that an application's
 * own script can end with `INCLUDE default.ld` on every board (for example one that adds INPUT(<archive>)
 * before it). Here it is pico-sdk's: CMakeLists.txt puts this directory and pico-sdk's linker script
 * directory (pico_crt0/<chip>) on the linker's search path. */
INCLUDE memmap_default.ld
