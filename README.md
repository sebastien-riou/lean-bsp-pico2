# lean-bsp-pico2

The [lean-bsp](https://github.com/sebastien-riou/lean-bsp) BSP for the Raspberry Pi Pico 2 board
(RP2350 at 150 MHz). Any lean-bsp application builds and runs on either of its two kinds of cores:

````
lbsp build-load-run --bsp pico2 --target cortex-m33 --until 'hello-world done' --timeout 60
lbsp build-load-run --bsp pico2 --target rv32imcb --until 'hello-world done' --timeout 60
````

The RP2350 runs either its two Cortex-M33 or its two Hazard3 RISC-V cores; the firmware uses
core 0 of the pair and never starts core 1.

## What it needs

- [pico-sdk](https://github.com/raspberrypi/pico-sdk) 2.2.0 next to this repository: `./setup-pico-sdk`
  clones it there (`lbsp fetch pico2` runs it). `PICO_SDK_PATH` names another one.
- CMake (and Ninja, used when found).
- The toolchain of the target: arm-none-eabi for `cortex-m33`, riscv-none-elf for `rv32imcb`.
  `CROSS_COMPILE` selects it, and must then be one for that target, e.g.
  `CROSS_COMPILE=/opt/toolchains/riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf-`;
  without it, `arm-none-eabi-gcc` or `riscv-none-elf-gcc` is taken from `PATH`.
- For `load` and `run`, [lean-hwbridge](https://github.com/sebastien-riou/lean-hwbridge) serving the
  board with its `picotool` profile: `hwbridge` on `PATH`, or the command in `HWBRIDGE`. The board is
  its usbman nickname, `HWBRIDGE_BOARD`, by default `pico2`. Without a bridge, `load` prints the
  picotool command to flash the board by hand.

No picotool is needed to build: the build makes no UF2 (`PICO_NO_PICOTOOL`), lean-hwbridge loads
the ELF itself.

## How the contract maps to the board

| Primitive | On Pico 2 |
|---|---|
| UART 0 (and `printf`, `stdin`) | The USB serial port the RP2350 serves itself (pico_stdio_usb), bytes as they are: no `\r` added before `\n`. |
| LEDs | 0 is the green LED (GP25) |
| Button | 0 is BOOTSEL, read through the flash chip select |
| Cycle counter, CPU frequency | DWT `CYCCNT` on Cortex-M33, `mcycle` on Hazard3, at `clk_sys` |
| Delay | busy wait on the 64-bit microsecond timer: works with interrupts masked |
| Cache clear | the XIP cache in front of the flash |
| Cycle timer | the SIO's RISC-V platform timer, which both kinds of core reach, counting `clk_sys`: a one-shot for any 32-bit count (28.6 s at 150 MHz), `read()` from the same counter. One more cycle delays the interrupt by exactly one instruction (checked on both cores). While it runs, every other interrupt is held off, USB included: BASEPRI on Cortex-M33 (the timer's interrupt is at priority 0), `mie` on Hazard3. The handler is a plain function on both (below) |
| Heap usage | lean-bsp's heap counter (contract 1.4), `src/bsp_heap.c`: the bytes requested from `malloc`, `calloc` and `realloc` and not freed yet, at most since the last reset. pico-sdk's own malloc wrappers are left out (`SKIP_PICO_MALLOC`); the heap is newlib's, grown by pico-sdk's `_sbrk` up to the end of main RAM |
| Reset | watchdog reboot, once the host took the pending output |
| `app_tick` | from a 1 ms repeating timer, run only when the application defines `app_tick` |
| `app_on_fault` | from the HardFault handler (`pc` is 0) on Cortex-M33, from the machine exception handler (`pc` is `mepc`) on Hazard3; the core then stops |
| `app_on_exit` | default does nothing: `main()` then parks the CPU, USB still serviced |
| Flash/NVM | region 0, `flash-upper-half`, on both targets: the upper 2 MB of the 4 MB QSPI flash (0x10200000, a Winbond W25Q32RV), 4 KiB sectors, 8-byte program unit, no zero overwrite (the W25Q32RV specifies programming for erased locations only). The image must fit the lower 2 MB (the link checks it). A write refuses a destination that does not read erased, then programs one unit at a time (its 256-byte page, 0xFF around it) and reads back; an erase checks its sectors read erased. Interrupts are masked for each sector erase (30 ms typical, 240 ms at most) and each program, not between them. A load through lean-hwbridge (picotool) rewrites only the image's sectors: the region keeps its content |

### USB is the console, and the way in

UART 0 and the reset interface picotool uses to reach the board both exist only while the firmware
services USB. So:

- **The application starts only once a host opened the serial port** (DTR raised): its whole
  output reaches whoever reads, however late the host comes. A board on a plain USB charger never
  gets past that point.
- UART 0 lets interrupts in while it moves bytes, since only the USB interrupt does; `printf` and
  `bsp_uart_tx` are therefore usable with interrupts masked, but briefly unmask them. That does not
  reach past what holds the cycle timer's competitors off (BASEPRI, `mie`) or out of a running
  exception handler: output from the cycle timer's handler, or while the timer runs, must wait
  (buffer it, send it afterwards from thread mode).

### The cycle timer's handler

A plain C function on both cores. On Cortex-M33 it is the handler of the timer's interrupt
(`SIO_IRQ_MTIMECMP`) itself, in pico-sdk's RAM vector table: the hardware stacks the interrupted
context, LR holds EXC_RETURN. On Hazard3 the machine timer's slot of
pico-sdk's vector table enters the BSP's `isr_riscv_machine_timer`, which saves the registers a call
may change and calls the handler; the interrupted pc is `mepc`, which the handler may read and
rewrite (the return goes there). The interrupted registers themselves are out of its reach.
- After `app_main` returns, `main()` parks with interrupts enabled, whatever the application left
  masked, so picotool can still reboot the board into BOOTSEL and load the next firmware.
- After a fault the core stops and USB with it: only a power cycle (the hub's, through lean-hwbridge)
  makes the board reachable again.

A firmware that does not service USB at all (or a fresh board) is loaded once by hand: hold BOOTSEL
while plugging the board in, then `picotool load -x <elf>`.

## Layout

- `CMakeLists.txt`, `pico_sdk_import.cmake`: the pico-sdk project, the firmware made of the board
  support and the application. `bsp_cli.py` configures it in `<build dir>/cmake`, with the
  application written into `<build dir>/app.cmake`; it is not configured by hand.
- `src/bsp_pico2.c`: `main()`, the primitives, the default application hooks, `_sbrk` and the fault
  handlers.
- `include/bsp_api.h`: the contract header, identical to lean-bsp's canonical copy (`lbsp check` verifies it).
- `src/bsp_nvm.c`: NVM region 0, through pico-sdk's `hardware_flash`.
- `linker/default.ld`: pico-sdk's linker script (`memmap_default.ld`), plus NVM region 0 (`NVM0`, where the
  sections named `.bsp_nvm0` go, loading nothing there) and the check that the image stays below it.
  An application needing its own script sets `linker_script` in `app.toml` and ends it with
  `INCLUDE default.ld`, as on every board.
- `bsp.toml`, `bsp_cli.py`, `setup-pico-sdk`: what lean-bsp reads and runs. `bsp_cli.py` is this board's
  CMake step on top of lean-bsp's board helpers (`leanbsp.backends`), which lean-bsp runs it with.
