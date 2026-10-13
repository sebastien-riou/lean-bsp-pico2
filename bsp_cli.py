#!/usr/bin/env python3
"""lean-bsp entry point of the Raspberry Pi Pico 2 BSP, made of lean-bsp's board helpers (leanbsp.backends).

build configures and builds this repository's pico-sdk CMake project (CMakeLists.txt) with the
application in it, for the target's core: cortex-m33 (PICO_PLATFORM rp2350-arm-s, arm-none-eabi)
or rv32imcb (rp2350-riscv, riscv-none-elf). CROSS_COMPILE selects the toolchain and must then be
one for the target's triple; without it, <triple>-gcc is taken from PATH. pico-sdk is
PICO_SDK_PATH, by default ../pico-sdk (./setup-pico-sdk clones it).

load and run go through lean-hwbridge; the board is the usbman nickname in HWBRIDGE_BOARD, default
pico2.
"""

import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

try:
    from leanbsp.backends import board, gcc
except ImportError:
    print(json.dumps({'status': 'error', 'message': 'this BSP needs a lean-bsp with leanbsp.backends: update lean-bsp'}))
    sys.exit(1)

ROOT = Path(__file__).resolve().parent
PLATFORMS = {'cortex-m33': 'rp2350-arm-s', 'rv32imcb': 'rp2350-riscv'}


def pico_sdk() -> Path:
    path = Path(os.environ.get('PICO_SDK_PATH') or ROOT.parent / 'pico-sdk')
    if not (path / 'pico_sdk_init.cmake').is_file():
        how = 'PICO_SDK_PATH does not name a pico-sdk' if os.environ.get('PICO_SDK_PATH') else 'run ./setup-pico-sdk'
        raise board.BuildError(f'no pico-sdk in {path}: {how}')
    return path


def cmake_list(name: str, values: list[str]) -> str:
    """set(name ...) with each value as a bracket argument: taken literally by CMake."""
    for value in values:
        if ';' in value or ']==]' in value:
            raise board.BuildError(f'{value!r} cannot be passed to CMake ({name})')
    return f'set({name}' + ''.join(f' [==[{v}]==]' for v in values) + ')\n'


def write_if_changed(path: Path, text: str) -> None:
    if not path.is_file() or path.read_text() != text:  # unchanged: no needless CMake reconfigure
        path.write_text(text)


def build(app: dict, target, bdir: Path) -> dict:
    if target.name not in PLATFORMS:
        raise board.BuildError(f'no target {target.name!r}: this BSP has {", ".join(PLATFORMS)}')
    prefix = gcc.cross_prefix(target)
    toolchain_dir = Path(os.path.abspath(shutil.which(f'{prefix}gcc'))).parent  # not resolved: xpack's bin/ links into bin-org/
    cdir = bdir / 'cmake'
    app_cmake = bdir / 'app.cmake'
    write_if_changed(app_cmake, ''.join([
        '# written by lean-bsp-pico2/bsp_cli.py from the application\'s app.json\n',
        cmake_list('LEAN_BSP_APP_NAME', [app['name']]),
        cmake_list('LEAN_BSP_APP_SOURCES', app['sources']),
        cmake_list('LEAN_BSP_APP_INCLUDE_DIRS', app['include_dirs']),
        cmake_list('LEAN_BSP_APP_DEFINES', app['defines']),
        cmake_list('LEAN_BSP_APP_LINKER_SCRIPT', [app['linker_script']] if app.get('linker_script') else []),
    ]))
    generator = ['-G', 'Ninja'] if shutil.which('ninja') else []
    configure = [
        'cmake', '-S', str(ROOT), '-B', str(cdir), *generator, '-DCMAKE_BUILD_TYPE=Debug',
        f'-DPICO_SDK_PATH={pico_sdk()}', f'-DPICO_PLATFORM={PLATFORMS[target.name]}', '-DPICO_BOARD=pico2',
        f'-DPICO_TOOLCHAIN_PATH={toolchain_dir}', f'-DPICO_GCC_TRIPLE={target.toolchain}',
        '-DPICO_NO_PICOTOOL=1', f'-DLEAN_BSP_APP_CMAKE={app_cmake}', f'-DLEAN_BSP_OUTPUT_DIR={bdir}',
    ]
    if board.read_state(bdir).get('configure') != configure or not (cdir / 'CMakeCache.txt').is_file():
        shutil.rmtree(cdir, ignore_errors=True)  # another toolchain or generator: start over
        print(shlex.join(configure), flush=True)
        if subprocess.run(configure).returncode != 0:
            raise board.BuildError('configuring the pico-sdk build failed')
        board.write_state(bdir, configure=configure)
    compile_ = ['cmake', '--build', str(cdir), '--parallel', str(os.cpu_count() or 4)]
    print(shlex.join(compile_), flush=True)
    if subprocess.run(compile_).returncode != 0:
        raise board.BuildError('building failed')
    elf = bdir / f'{app["name"]}.elf'
    subprocess.run([f'{prefix}size', str(elf)])
    return {'elf': str(elf), 'bin': str(elf.with_suffix('.bin')), 'hex': str(elf.with_suffix('.hex'))}


BOARD = board.Board(
    root=ROOT,
    title='Raspberry Pi Pico 2',
    flash_by_hand='picotool load -f -x {elf} (or hold BOOTSEL while plugging the board in, then picotool load -x {elf})',
    run_by_hand="Run picotool reboot -a -f, then open the board's USB serial port: the application starts only once "
                'a host opened it',
)

if __name__ == '__main__':
    sys.exit(board.main(BOARD, build))
