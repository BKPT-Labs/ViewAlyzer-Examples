#!/usr/bin/env python3
"""Build and flash helper for Nucleo_H723_Throughput.

Usage:
    python3 build.py                     # build the default transport
    python3 build.py --transport swo        # build another transport variant
    python3 build.py --flash             # build, then flash the board
    python3 build.py --flash --serial <probe-serial>
    python3 build.py --clean             # drop the build dir first (full rebuild)

Transports (each builds into its own build/<transport>/ directory, so
switching never hits a stale CMake cache):
    rambuf   RAM ring drained through the on-board ST-LINK (default)
    swo      ITM / SWO trace pin

--serial pins the flash to one debug probe when several are connected
(`STM32_Programmer_CLI --list` / `JLinkExe -CommanderScript` show serials;
the ViewAlyzer CLI prints them too: `ViewAlyzer --headless --list-probes`).

Requirements: CMake, Ninja, and the Arm GNU toolchain. Everything is found
automatically when STM32CubeCLT is installed. Machine-specific paths can be
set in tools.local[.win|.linux|.mac].json at the repo root (shared by all
examples) or build.local[.os].json next to this script — see
tools.local.example.json at the repo root. Committed files never contain
machine paths.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PROJECT = "Nucleo_H723_Throughput"
ELF_NAME = "Nucleo_H723_Throughput"
PROBE = "stlink"                      # "stlink" or "jlink"
TARGET_DEVICE = "STM32H723ZG"      # device name for J-Link flashing
TRANSPORTS = {"rambuf": "RAM_BUFFER", "swo": "ARM_ITM"}              # cli name -> VA_TRANSPORT value (None = project default)
DEFAULT_TRANSPORT = "rambuf"

PROJECT_DIR = Path(__file__).resolve().parent
REPO_ROOT = PROJECT_DIR.parent.parent
IS_WINDOWS = os.name == "nt"


# ── Machine-local tool configuration ─────────────────────────────────

def _load_local_config() -> dict:
    """Merge the optional, gitignored per-machine config files. Project-level
    build.local[.os].json wins over the repo-root tools.local[.os].json; the
    per-OS variant wins over the plain one."""
    os_suffix = "win" if IS_WINDOWS else ("mac" if sys.platform == "darwin" else "linux")
    merged: dict = {}
    for candidate in (
        REPO_ROOT / "tools.local.json",
        REPO_ROOT / f"tools.local.{os_suffix}.json",
        PROJECT_DIR / "build.local.json",
        PROJECT_DIR / f"build.local.{os_suffix}.json",
    ):
        if candidate.is_file():
            try:
                merged.update(json.loads(candidate.read_text(encoding="utf-8")))
            except (OSError, ValueError) as exc:
                sys.exit(f"ERROR: cannot read {candidate}: {exc}")
    return merged


_LOCAL = _load_local_config()


def _local_path(key: str) -> Path | None:
    value = _LOCAL.get(key)
    if not value:
        return None
    path = Path(os.path.expandvars(os.path.expanduser(str(value))))
    if not path.exists():
        print(f"WARNING: {key} in a .local. config points at '{path}', "
              "which does not exist; ignoring.")
        return None
    return path


def _first_existing(*patterns: str) -> Path | None:
    for pattern in patterns:
        expanded = os.path.expandvars(os.path.expanduser(pattern))
        matches = sorted(glob.glob(expanded), reverse=True)
        if matches:
            return Path(matches[0])
    return None


def _cubeclt_root() -> Path | None:
    root = _local_path("stm32cubeclt_root")
    if root:
        return root
    env = os.environ.get("STM32CUBECLT_ROOT")
    if env and Path(os.path.expanduser(env)).exists():
        return Path(os.path.expanduser(env))
    if IS_WINDOWS:
        return _first_existing("C:/ST/STM32CubeCLT_*",
                               "C:/Program Files/STMicroelectronics/STM32CubeCLT_*")
    return _first_existing("/opt/ST/STM32CubeCLT_*", "/opt/st/stm32cubeclt_*",
                           "~/st/stm32cubeclt_*", "/usr/local/st/stm32cubeclt_*")


CUBECLT = _cubeclt_root()


def _tool(name: str, *cubeclt_rel: str, local_key: str | None = None) -> str:
    """Resolve a tool: .local. config -> PATH -> STM32CubeCLT install."""
    if local_key:
        local = _local_path(local_key)
        if local:
            return str(local)
    found = shutil.which(name)
    if found:
        return found
    if CUBECLT:
        for rel in cubeclt_rel:
            exe = CUBECLT / rel / (name + (".exe" if IS_WINDOWS else ""))
            if exe.exists():
                return str(exe)
    sys.exit(f"ERROR: '{name}' not found. Install STM32CubeCLT or set "
             "stm32cubeclt_root in tools.local.json at the repo root "
             "(see tools.local.example.json).")


def _jlink_exe() -> str:
    local = _local_path("jlink_commander")
    if local:
        return str(local)
    name = "JLink" if IS_WINDOWS else "JLinkExe"
    found = shutil.which(name)
    if found:
        return found
    fallback = _first_existing("C:/Program Files/SEGGER/JLink*/JLink.exe",
                               "/Applications/SEGGER/JLink*/JLinkExe",
                               "/opt/SEGGER/JLink*/JLinkExe")
    if fallback:
        return str(fallback)
    sys.exit("ERROR: J-Link Commander not found. Install the SEGGER J-Link "
             "tools or set jlink_commander in tools.local.json at the repo "
             "root (see tools.local.example.json).")


def _run(cmd: list[str], **kwargs) -> None:
    print("+", " ".join(str(c) for c in cmd))
    result = subprocess.run([str(c) for c in cmd], **kwargs)
    if result.returncode != 0:
        sys.exit(result.returncode)


# ── Build ────────────────────────────────────────────────────────────

def build(transport: str, clean: bool) -> Path:
    build_dir = PROJECT_DIR / "build" / transport
    if clean and build_dir.exists():
        shutil.rmtree(build_dir)

    env = os.environ.copy()
    if CUBECLT:
        # Put the CubeCLT Arm GNU toolchain on PATH for the toolchain file.
        env["PATH"] = os.pathsep.join(
            [str(CUBECLT / "GNU-tools-for-STM32" / "bin"), env.get("PATH", "")])

    cmake = _tool("cmake", "CMake/bin")
    if not (build_dir / "CMakeCache.txt").exists():
        configure = [cmake, "-S", PROJECT_DIR, "-B", build_dir, "-G", "Ninja",
                     "--toolchain", PROJECT_DIR / "cmake" / "gcc-arm-none-eabi.cmake",
                     "-DCMAKE_BUILD_TYPE=Release",
                     f"-DCMAKE_MAKE_PROGRAM={_tool('ninja', 'Ninja/bin')}"]
        if TRANSPORTS.get(transport):
            configure.append(f"-DVA_TRANSPORT={TRANSPORTS[transport]}")
        _run(configure, env=env)
    _run([cmake, "--build", build_dir], env=env)

    elf = build_dir / f"{ELF_NAME}.elf"
    if not elf.exists():
        sys.exit(f"ERROR: build produced no ELF at {elf}")
    print(f"Built: {elf}")
    return elf


# ── Flash ────────────────────────────────────────────────────────────

def flash(elf: Path, serial: str | None) -> None:
    if PROBE == "jlink":
        script = Path(tempfile.mkstemp(suffix=".jlink")[1])
        script.write_text(f"loadfile {elf}\nr\ng\nqc\n", encoding="utf-8")
        cmd = [_jlink_exe(), "-NoGui", "1", "-device", TARGET_DEVICE,
               "-if", "SWD", "-speed", "4000", "-autoconnect", "1"]
        if serial:
            cmd += ["-SelectEmuBySN", serial]
        cmd += ["-CommandFile", script]
        try:
            _run(cmd)
        finally:
            script.unlink(missing_ok=True)
    else:
        cli = _tool("STM32_Programmer_CLI", "STM32CubeProgrammer/bin")
        connect = ["-c", "port=SWD"] + ([f"sn={serial}"] if serial else [])
        _run([cli, *connect, "-w", elf, "-v", "-rst"])
    print("Flashed and reset.")


def main() -> None:
    parser = argparse.ArgumentParser(
        description=f"Build and flash {PROJECT}.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--transport", choices=sorted(TRANSPORTS),
                        default=DEFAULT_TRANSPORT,
                        help=f"transport variant to build (default: {DEFAULT_TRANSPORT})")
    parser.add_argument("--flash", action="store_true",
                        help="flash the board after building")
    parser.add_argument("--serial", metavar="SN",
                        help="debug probe serial number to flash through "
                             "(needed when several probes are connected)")
    parser.add_argument("--clean", action="store_true",
                        help="remove the transport's build directory first")
    args = parser.parse_args()

    elf = build(args.transport, args.clean)
    if args.flash:
        flash(elf, args.serial)
    elif args.serial:
        print("NOTE: --serial has no effect without --flash.")


if __name__ == "__main__":
    main()
