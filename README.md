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
| Heap usage | growth of the heap end since the last reset, tracked in `_sbrk` |
| Reset | watchdog reboot, once the host took the pending output |
| `app_tick` | from a 1 ms repeating timer, run only when the application defines `app_tick` |
| `app_on_fault` | from the HardFault handler (`pc` is 0) on Cortex-M33, from the machine exception handler (`pc` is `mepc`) on Hazard3; the core then stops |
| `app_on_exit` | default does nothing: `main()` then parks the CPU, USB still serviced |
| Flash/NVM | not yet |

### USB is the console, and the way in

UART 0 and the reset interface picotool uses to reach the board both exist only while the firmware
services USB. So:

- **The application starts only once a host opened the serial port** (DTR raised): its whole
  output reaches whoever reads, however late the host comes. A board on a plain USB charger never
  gets past that point.
- UART 0 lets interrupts in while it moves bytes, since only the USB interrupt does; `printf` and
  `bsp_uart_tx` are therefore usable with interrupts masked, but briefly unmask them.
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
- pico-sdk's own linker script: an application needing its own sets `linker_script` in `app.toml`.
- `bsp.toml`, `bsp_cli.py`, `setup-pico-sdk`: what lean-bsp reads and runs.
