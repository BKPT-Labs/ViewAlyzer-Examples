#!/usr/bin/env python3
"""
Build / flash / debug helper for the NUCLEO-L031K6 ViewAlyzer sample.

Cortex-M0+ target: no DWT, no ITM. Timestamps come from TIM2 through the
recorder's CUSTOM_TIMER source and the transport is the RAM buffer drained
by the on-board ST-LINK, so flashing uses STM32CubeProgrammer (shipped with
STM32CubeCLT), not a J-Link.

Toolchain resolution order:
  1. ZEPHYR_TOOLCHAIN_VARIANT / matching *_TOOLCHAIN_PATH already in the env
     (e.g. a Zephyr SDK, or gnuarmemb via GNUARMEMB_TOOLCHAIN_PATH).
  2. The arm-none-eabi GCC shipped with STM32CubeCLT, looked up in the
     per-OS default install locations (Linux: ~/st, /opt/st, /usr/local/st;
     Windows: C:/ST, Program Files).

Works on both Linux and Windows (this repo lives on a drive shared by both);
each OS gets its own build directory (build/ on Linux, build-win/ on
Windows) - CMake caches are not portable across OSes, and west's pristine
step chokes on a cache written by the other one.

Usage:
    python3 build.py                 # build
    python3 build.py build
    python3 build.py clean           # pristine rebuild
    python3 build.py flash           # west flash --runner stm32cubeprogrammer
    python3 build.py debug           # west debug (openocd)
    python3 build.py menuconfig
"""

import glob
import os
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent
BUILD_DIR = PROJECT_DIR / ("build-win" if os.name == "nt" else "build")

BOARD = "nucleo_l031k6"


def _local_config() -> dict:
    """Merge gitignored per-machine config: repo-root tools.local[.os].json,
    overridden by a build.local[.os].json next to this script. See
    tools.local.example.json at the repo root for the keys."""
    import json
    suffix = "win" if os.name == "nt" else ("mac" if sys.platform == "darwin" else "linux")
    merged = {}
    for c in (PROJECT_DIR.parent.parent / "tools.local.json",
              PROJECT_DIR.parent.parent / f"tools.local.{suffix}.json",
              PROJECT_DIR / "build.local.json",
              PROJECT_DIR / f"build.local.{suffix}.json"):
        if c.is_file():
            merged.update(json.loads(c.read_text(encoding="utf-8")))
    return merged


_LOCAL = _local_config()


def _local_path(key: str) -> "Path | None":
    v = _LOCAL.get(key)
    if not v:
        return None
    p = Path(os.path.expandvars(os.path.expanduser(str(v))))
    return p if p.exists() else None


def resolve_zephyr_base() -> Path:
    local = _local_path("zephyr_base")
    if local is not None and (local / "kernel").exists():
        return local
    env = os.environ.get("ZEPHYR_BASE")
    if env and (Path(env) / "kernel").exists():
        return Path(env)
    # Walk up looking for a sibling zephyr/ checkout, then fall back to the
    # conventional ~/zephyrproject/zephyr.
    candidates = [PROJECT_DIR.parents[i] / "zephyr" for i in range(6)]
    candidates.append(Path.home() / "zephyrproject" / "zephyr")
    for c in candidates:
        if (c / "west.yml").exists() or (c / "kernel").exists():
            return c
    sys.exit("ERROR: cannot locate a Zephyr checkout. Set ZEPHYR_BASE.")


def cubeclt_root() -> Path | None:
    local = _local_path("stm32cubeclt_root")
    if local is not None:
        return local
    if os.name == "nt":
        patterns = (
            "C:/ST/STM32CubeCLT_*",
            "C:/Program Files/STMicroelectronics/STM32CubeCLT_*",
        )
    else:
        patterns = (
            str(Path.home() / "st" / "stm32cubeclt_*"),
            "/opt/st/stm32cubeclt_*",
            "/usr/local/st/stm32cubeclt_*",
        )
    for pat in patterns:
        matches = sorted(glob.glob(pat), reverse=True)
        if matches:
            return Path(matches[0])
    return None


def setup_env() -> dict:
    env = os.environ.copy()
    env["ZEPHYR_BASE"] = str(resolve_zephyr_base())
    env["PATH"] = str(Path.home() / ".local" / "bin") + os.pathsep + env.get("PATH", "")

    # A CC/CXX that names a directory (seen: a stale machine-wide
    # CC=...\MinGW\bin on one Windows host) derails CMake's compiler checks.
    for var in ("CC", "CXX"):
        if env.get(var) and Path(env[var]).is_dir():
            env.pop(var)

    clt = cubeclt_root()

    # Honour an already-selected toolchain (e.g. a good Zephyr SDK); otherwise
    # fall back to CubeCLT's gnuarmemb and drop the incompatible SDK 1.0.0.
    if not env.get("ZEPHYR_TOOLCHAIN_VARIANT"):
        local_tc = _local_path("gnuarmemb_toolchain_path")
        gnuarmemb = str(local_tc) if local_tc else env.get("GNUARMEMB_TOOLCHAIN_PATH")
        if not (gnuarmemb and Path(gnuarmemb).exists()):
            gnuarmemb = str(clt / "GNU-tools-for-STM32") if clt else None
        if not (gnuarmemb and Path(gnuarmemb).exists()):
            sys.exit("ERROR: no Arm toolchain found. Set ZEPHYR_TOOLCHAIN_VARIANT "
                     "(Zephyr SDK) or GNUARMEMB_TOOLCHAIN_PATH.")
        env["ZEPHYR_TOOLCHAIN_VARIANT"] = "gnuarmemb"
        env["GNUARMEMB_TOOLCHAIN_PATH"] = gnuarmemb
        env.pop("ZEPHYR_SDK_INSTALL_DIR", None)

    # west's stm32cubeprogrammer runner finds STM32_Programmer_CLI on PATH;
    # CubeCLT does not put it there by default.
    if clt:
        prog_bin = clt / "STM32CubeProgrammer" / "bin"
        if prog_bin.exists():
            env["PATH"] = str(prog_bin) + os.pathsep + env["PATH"]
    return env


def west(env: dict) -> str:
    w = shutil.which("west", path=env["PATH"])
    if w is None:
        sys.exit("ERROR: 'west' not found on PATH (pip3 install --user west).")
    return w


def cmd_build(env, pristine: bool) -> int:
    args = [west(env), "build", "-p", "always" if pristine else "auto",
            "-b", BOARD, "-d", str(BUILD_DIR), str(PROJECT_DIR)]
    print(f"Building {BOARD}  (toolchain={env['ZEPHYR_TOOLCHAIN_VARIANT']}, "
          f"ZEPHYR_BASE={env['ZEPHYR_BASE']})")
    return subprocess.call(args, env=env, cwd=str(PROJECT_DIR))


def cmd_runner(env, verb: str) -> int:
    rc = cmd_build(env, pristine=False)
    if rc:
        return rc
    runner = "stm32cubeprogrammer" if verb == "flash" else "openocd"
    args = [west(env), verb, "-d", str(BUILD_DIR), "--skip-rebuild",
            "--runner", runner]
    print(f"\n{verb} via {runner} (on-board ST-LINK) ...")
    return subprocess.call(args, env=env, cwd=str(PROJECT_DIR))


def cmd_menuconfig(env) -> int:
    args = [west(env), "build", "-b", BOARD, "-d", str(BUILD_DIR),
            str(PROJECT_DIR), "-t", "menuconfig"]
    return subprocess.call(args, env=env, cwd=str(PROJECT_DIR))


def main() -> int:
    action = sys.argv[1] if len(sys.argv) > 1 else "build"
    env = setup_env()
    if action == "build":
        return cmd_build(env, pristine=False)
    if action == "clean":
        return cmd_build(env, pristine=True)
    if action in ("flash", "debug"):
        return cmd_runner(env, action)
    if action == "menuconfig":
        return cmd_menuconfig(env)
    sys.exit(f"Unknown action '{action}'. Use build|clean|flash|debug|menuconfig.")


if __name__ == "__main__":
    sys.exit(main())
