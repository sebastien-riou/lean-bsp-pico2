#!/usr/bin/env python3
"""lean-bsp entry point of the Raspberry Pi Pico 2 BSP: capabilities, build, load and run.

build configures and builds this repository's pico-sdk CMake project (CMakeLists.txt) with the
application in it, for the target's core: cortex-m33 (PICO_PLATFORM rp2350-arm-s, arm-none-eabi)
or rv32imcb (rp2350-riscv, riscv-none-elf). CROSS_COMPILE selects the toolchain and must then be
one for the target's triple; without it, <triple>-gcc is taken from PATH. pico-sdk is
PICO_SDK_PATH, by default ../pico-sdk (./setup-pico-sdk clones it).

load and run go through lean-hwbridge (`hwbridge`, or the command in HWBRIDGE), so they work from
a sandboxed session as well as from a terminal. The board is the usbman nickname in HWBRIDGE_BOARD,
default pico2. Without a bridge, load reports what to do by hand.

Every verb ends its output with one JSON line, the result lean-bsp parses.
"""

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent
STATE = 'pico2.json'  # this BSP's record of what it built and where the boot output starts
PLATFORMS = {'cortex-m33': 'rp2350-arm-s', 'rv32imcb': 'rp2350-riscv'}


def result(status: str, **fields) -> int:
    print(json.dumps({'status': status, **fields}), flush=True)
    return 1 if status == 'error' else 0


class BuildError(Exception):
    pass


def target_toolchain(name: str) -> str:
    with open(ROOT / 'bsp.toml', 'rb') as f:
        manifest = tomllib.load(f)
    for target in manifest['bsp']['targets']:
        if target['name'] == name:
            return target['toolchain']
    raise BuildError(f'no target {name!r} in bsp.toml')


def read_state(bdir: Path) -> dict:
    path = bdir / STATE
    return json.loads(path.read_text()) if path.is_file() else {}


def write_state(bdir: Path, **fields) -> None:
    (bdir / STATE).write_text(json.dumps({**read_state(bdir), **fields}) + '\n')


# ---- build ----


def cmd_capabilities(args) -> int:
    return result('ok')


def toolchain_bin(target: str, triple: str) -> Path:
    """The directory of the target's gcc: what pico-sdk calls PICO_TOOLCHAIN_PATH."""
    prefix = os.environ.get('CROSS_COMPILE')
    if prefix and not prefix.endswith(f'{triple}-'):
        raise BuildError(f'target {target} needs a {triple} toolchain, but CROSS_COMPILE is {prefix}: set it to '
                         f'that toolchain\'s prefix, or unset it to take {triple}-gcc from PATH')
    gcc = shutil.which(f'{prefix or triple + "-"}gcc')
    if gcc is None:
        raise BuildError(f'no {prefix or triple + "-"}gcc: install a {triple} toolchain, and name it with CROSS_COMPILE '
                         f'or put it in PATH')
    return Path(os.path.abspath(gcc)).parent  # not resolved: xpack's bin/ links into bin-org/


def pico_sdk() -> Path:
    path = Path(os.environ.get('PICO_SDK_PATH') or ROOT.parent / 'pico-sdk')
    if not (path / 'pico_sdk_init.cmake').is_file():
        how = 'PICO_SDK_PATH does not name a pico-sdk' if os.environ.get('PICO_SDK_PATH') else 'run ./setup-pico-sdk'
        raise BuildError(f'no pico-sdk in {path}: {how}')
    return path


def cmake_list(name: str, values: list[str]) -> str:
    """set(name ...) with each value as a bracket argument: taken literally by CMake."""
    for value in values:
        if ';' in value or ']==]' in value:
            raise BuildError(f'{value!r} cannot be passed to CMake ({name})')
    return f'set({name}' + ''.join(f' [==[{v}]==]' for v in values) + ')\n'


def write_if_changed(path: Path, text: str) -> None:
    if not path.is_file() or path.read_text() != text:  # unchanged: no needless CMake reconfigure
        path.write_text(text)


def cmd_build(args) -> int:
    try:
        return build(args)
    except BuildError as e:
        return result('error', message=str(e))


def build(args) -> int:
    app = json.loads(Path(args.app_json).read_text())
    if app.get('lib'):
        return result('error', message='prebuilt application libraries ([app.build]) are not supported yet')
    if args.target not in PLATFORMS:
        raise BuildError(f'no target {args.target!r}: this BSP has {", ".join(PLATFORMS)}')
    triple = target_toolchain(args.target)
    bdir = Path(args.bdir).resolve()
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
        f'-DPICO_SDK_PATH={pico_sdk()}', f'-DPICO_PLATFORM={PLATFORMS[args.target]}', '-DPICO_BOARD=pico2',
        f'-DPICO_TOOLCHAIN_PATH={toolchain_bin(args.target, triple)}', f'-DPICO_GCC_TRIPLE={triple}',
        '-DPICO_NO_PICOTOOL=1', f'-DLEAN_BSP_APP_CMAKE={app_cmake}', f'-DLEAN_BSP_OUTPUT_DIR={bdir}',
    ]
    if read_state(bdir).get('configure') != configure or not (cdir / 'CMakeCache.txt').is_file():
        shutil.rmtree(cdir, ignore_errors=True)  # another toolchain or generator: start over
        print(shlex.join(configure), flush=True)
        if subprocess.run(configure).returncode != 0:
            return result('error', message='configuring the pico-sdk build failed')
        write_state(bdir, configure=configure)
    compile_ = ['cmake', '--build', str(cdir), '--parallel', str(os.cpu_count() or 4)]
    print(shlex.join(compile_), flush=True)
    if subprocess.run(compile_).returncode != 0:
        return result('error', message='building failed')
    elf = bdir / f'{app["name"]}.elf'
    subprocess.run([str(toolchain_bin(args.target, triple) / f'{triple}-size'), str(elf)])
    write_state(bdir, elf=str(elf))
    return result('ok', artifact={'elf': str(elf), 'bin': str(elf.with_suffix('.bin')),
                                  'hex': str(elf.with_suffix('.hex'))})


# ---- load and run, through lean-hwbridge ----


def board() -> str:
    return os.environ.get('HWBRIDGE_BOARD', 'pico2')


def hwbridge(*args: str, capture: bool = True) -> subprocess.CompletedProcess:
    cmd = [*shlex.split(os.environ.get('HWBRIDGE', 'hwbridge')), *args]
    print(shlex.join(cmd), flush=True)
    return subprocess.run(cmd, capture_output=capture, text=True)


class NoBridge(Exception):
    pass


def bridge_json(*args: str) -> dict:
    try:
        done = hwbridge(*args, '--json')
    except FileNotFoundError:
        raise NoBridge('hwbridge is not installed') from None
    if done.returncode == 3:
        raise NoBridge(done.stderr.strip())
    if done.returncode != 0:
        raise RuntimeError(done.stderr.strip() or f'hwbridge {args[0]} failed ({done.returncode})')
    return json.loads(done.stdout) if done.stdout.strip() else {}


def cmd_load(args) -> int:
    bdir = Path(args.bdir)
    state = read_state(bdir)
    if 'elf' not in state:
        return result('error', message=f'nothing built in {bdir}')
    try:
        bridge_json('serial', 'open', board())  # first: the firmware waits for it before starting
        flashed = bridge_json('flash', board(), state['elf'])
    except NoBridge as e:
        return result('manual', message=f'{e}. Flash it yourself, e.g. picotool load -f -x {state["elf"]} (or hold '
                                         f'BOOTSEL while plugging the board in, then picotool load -x); or start '
                                         f'`hwbridge serve`')
    except RuntimeError as e:
        return result('error', message=str(e))
    write_state(bdir, boot=[flashed['generation'], flashed['offset']])
    return result('ok', boot=f'{flashed["generation"]}:{flashed["offset"]}')


def cmd_run(args) -> int:
    com_device = f'hwbridge:{board()}'
    try:
        bridge_json('serial', 'open', board())
        reset = bridge_json('reset', board())
    except NoBridge as e:
        return result('manual', message=f'{e}. Run picotool reboot -a -f, then open the board\'s USB serial port: the '
                                         f'application starts only once a host opened it')
    except RuntimeError as e:
        return result('error', message=str(e))
    boot = f'{reset["generation"]}:{reset["offset"]}'
    if args.detach:
        return result('ok', com_device=com_device, exit_code=None, boot=boot)
    # follow the console: a board has no exit code, so this lasts until --until, --timeout or Ctrl-C
    cat = ['serial', 'cat', board(), '--from', boot]
    if args.until:
        cat += ['--until', args.until]
    if args.timeout is not None and args.timeout >= 0:
        cat += ['--timeout', str(args.timeout)]
    done = hwbridge(*cat, capture=False)
    print(flush=True)
    if done.returncode == 124 and args.until:
        return result('error', message=f'{args.until!r} did not show up within {args.timeout:g} s',
                      com_device=com_device, exit_code=None)
    if done.returncode not in (0, 124):
        return result('error', message=f'following the console failed ({done.returncode})',
                      com_device=com_device, exit_code=None)
    return result('ok', com_device=com_device, exit_code=None, boot=boot)


def main() -> int:
    parser = argparse.ArgumentParser(prog='bsp_cli.py', description='Raspberry Pi Pico 2 BSP entry point for lean-bsp')
    sub = parser.add_subparsers(dest='verb', required=True)
    p = sub.add_parser('capabilities')
    p.add_argument('--json', action='store_true')
    p.set_defaults(func=cmd_capabilities)
    p = sub.add_parser('build')
    p.add_argument('--app-json', required=True)
    p.add_argument('--target', required=True)
    p.add_argument('--bdir', required=True)
    p.set_defaults(func=cmd_build)
    p = sub.add_parser('load')
    p.add_argument('--bdir', required=True)
    p.add_argument('--target')
    p.set_defaults(func=cmd_load)
    p = sub.add_parser('run')
    p.add_argument('--bdir', required=True)
    p.add_argument('--target')
    p.add_argument('--detach', action='store_true')
    p.add_argument('--timeout', type=float, help='seconds to follow the console; -1 or absent: until Ctrl-C')
    p.add_argument('--until', help='stop following once this text was printed')
    p.set_defaults(func=cmd_run)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
