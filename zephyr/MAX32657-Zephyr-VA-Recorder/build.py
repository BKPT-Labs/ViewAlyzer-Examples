#!/usr/bin/env python3
"""
Build / flash / debug helper for the MAX32657EVKIT ViewAlyzer sample.

This board is a Cortex-M33 from Analog Devices, so it does not need any
STM32-specific tooling. The script just makes sure west can find a Zephyr
checkout and an Arm toolchain, then drives west.

Toolchain resolution order:
  1. ZEPHYR_TOOLCHAIN_VARIANT / matching *_TOOLCHAIN_PATH already in the env
     (e.g. a Zephyr SDK, or gnuarmemb via GNUARMEMB_TOOLCHAIN_PATH).
  2. A generic arm-none-eabi GCC shipped with STM32CubeCLT (works fine for
     the M33 core) — used here because this bench's Zephyr SDK 1.0.0 is
     flagged incompatible. Looked up in the per-OS default install locations
     (Linux: ~/st, /opt/st, /usr/local/st; Windows: C:/ST, Program Files).

Works on both Linux and Windows (this repo lives on a drive shared by both);
the Zephyr workspace is expected at ~/zephyrproject on either OS unless
ZEPHYR_BASE points elsewhere. Each OS gets its own build directory (build/ on
Linux, build-win/ on Windows) — CMake caches are not portable across OSes, and
west's pristine step chokes on a cache written by the other one.

Usage:
    python3 build.py                 # build (pristine auto)
    python3 build.py build
    python3 build.py clean
    python3 build.py flash            # west flash --runner jlink
    python3 build.py debug            # west debug  --runner jlink
    python3 build.py menuconfig

The J-Link runner needs a SEGGER J-Link software version new enough to know
the MAX32657 (added in current releases). The flash/debug commands pin the
newest local JLinkExe explicitly (see resolve_jlink_commander) — relying on
PATH order silently picks up older installs, whose "unknown device" dialog
pops on every flash.
"""

import glob
import os
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent
BUILD_DIR = PROJECT_DIR / ("build-win" if os.name == "nt" else "build")

BOARD = "max32657evkit/max32657"
JLINK_DEVICE = "MAX32657"


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


def resolve_gnuarmemb() -> str | None:
    local = _local_path("gnuarmemb_toolchain_path")
    if local is not None:
        return str(local)
    env = os.environ.get("GNUARMEMB_TOOLCHAIN_PATH")
    if env and Path(env).exists():
        return env
    if os.name == "nt":
        patterns = (
            "C:/ST/STM32CubeCLT_*/GNU-tools-for-STM32",
            "C:/Program Files/STMicroelectronics/STM32CubeCLT_*/GNU-tools-for-STM32",
        )
    else:
        patterns = (
            "/home/*/st/stm32cubeclt_*/GNU-tools-for-STM32",
            "/opt/st/stm32cubeclt_*/GNU-tools-for-STM32",
            "/usr/local/st/stm32cubeclt_*/GNU-tools-for-STM32",
        )
    for pat in patterns:
        matches = sorted(glob.glob(pat), reverse=True)
        if matches:
            return matches[0]
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

    # Honour an already-selected toolchain (e.g. a good Zephyr SDK); otherwise
    # fall back to gnuarmemb and drop the incompatible SDK 1.0.0 on this host.
    if not env.get("ZEPHYR_TOOLCHAIN_VARIANT"):
        gnuarmemb = resolve_gnuarmemb()
        if gnuarmemb is None:
            sys.exit("ERROR: no Arm toolchain found. Set ZEPHYR_TOOLCHAIN_VARIANT "
                     "(Zephyr SDK) or GNUARMEMB_TOOLCHAIN_PATH.")
        env["ZEPHYR_TOOLCHAIN_VARIANT"] = "gnuarmemb"
        env["GNUARMEMB_TOOLCHAIN_PATH"] = gnuarmemb
        env.pop("ZEPHYR_SDK_INSTALL_DIR", None)
    return env


def resolve_jlink_commander() -> str | None:
    """Newest SEGGER J-Link commander on this host, preferring versioned
    installs (~/JLink on Linux, Program Files/SEGGER on Windows) over whatever
    PATH resolves first (an older install found first on PATH doesn't know the
    MAX32657 and pops the core-selection dialog on every flash)."""
    if os.name == "nt":
        roots = [Path(p) / "SEGGER" / "JLink_V*"
                 for p in (os.environ.get("ProgramFiles", r"C:\Program Files"),
                           os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))]
        exe_name = "JLink.exe"
    else:
        roots = [Path.home() / "JLink" / "JLink_Linux_V*"]
        exe_name = "JLinkExe"
    installs = sorted((d for root in roots for d in glob.glob(str(root))),
                      reverse=True)
    for d in installs:
        exe = Path(d) / exe_name
        if exe.exists():
            return str(exe)
    return shutil.which("JLink" if os.name == "nt" else "JLinkExe")


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
    args = [west(env), verb, "-d", str(BUILD_DIR), "--skip-rebuild",
            "--runner", "jlink", "--", "--device", JLINK_DEVICE]
    commander = resolve_jlink_commander()
    if commander:
        args += ["--commander", commander]
    print(f"\n{verb} via J-Link (device {JLINK_DEVICE}, "
          f"commander {commander or 'from PATH'}) …")
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
