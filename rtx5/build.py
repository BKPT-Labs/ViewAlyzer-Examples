#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Build and flash helper for the RTX5 recorder examples.

Usage:
    python3 build.py                          # G474, RAM buffer transport
    python3 build.py --board f767 --transport rtt
    python3 build.py --flash                  # build, then flash the board
    python3 build.py --flash --serial <probe-serial>
    python3 build.py --clean                  # drop the build dir first (full rebuild)
    python3 build.py --recorder-off           # uninstrumented control image

Builds go into build/<os>-<board>-<transport>[-off]/ unless --build-dir is
given. Pinned CMSIS-RTX, CMSIS_6 and ST device checkouts are fetched by exact
commit into a per-user dependency cache (shared with the ThreadX examples).
Existing checkouts are verified, never reset or modified; --kernel-dir,
--cmsis-dir and --cube-dir select your own clean checkouts instead.

--serial pins the flash to one debug probe when several are connected
(`STM32_Programmer_CLI --list` / `JLinkExe -CommanderScript` show serials;
the ViewAlyzer CLI prints them too: `ViewAlyzer --headless --list-probes`).
The G474 flashes through its onboard ST-Link, the F767 through a J-Link.

Requirements: Git, CMake, Ninja, and the Arm GNU toolchain. Everything is
found automatically when STM32CubeCLT is installed. Machine-specific paths
(including deps_cache) can be set in tools.local[.win|.linux|.mac].json at the
repo root (shared by all examples) or build.local[.os].json next to this
script — see tools.local.example.json at the repo root. Committed files never
contain machine paths.
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

PROJECT = Path(__file__).resolve().parent
REPO = PROJECT.parent
IS_WINDOWS = os.name == "nt"
OS_NAME = "win" if IS_WINDOWS else ("mac" if sys.platform == "darwin" else "linux")
KERNEL = ("https://github.com/ARM-software/CMSIS-RTX.git",
          "b566de42a5bf77d8b080f521cefc38ec7c45a3cc")
CMSIS = ("https://github.com/ARM-software/CMSIS_6.git",
         "af1b110e46a1720349f6f125e386e126767699bd")
KERNEL_COMMIT, CMSIS_COMMIT = KERNEL[1], CMSIS[1]
# board -> (donor url, donor commit, cache name, Nucleo project dir, device, CPUID part, probe)
BOARDS = {
    "g474": ("https://github.com/STMicroelectronics/x-cube-azrtos-g4.git",
             "b79cbdcc0e88f6f0db9689f98c99595e7d2769fa", "x-cube-azrtos-g4",
             "NUCLEO-G474RE", "STM32G474RE", 0xC24, "stlink"),
    "f767": ("https://github.com/STMicroelectronics/x-cube-azrtos-f7.git",
             "5ce63d50c4ffeb9e6c7e8c5ec567b424cffcd223", "x-cube-azrtos-f7",
             "STM32F767ZI-Nucleo", "STM32F767ZI", 0xC27, "jlink"),
}
TRANSPORTS = {"swo": 1, "rtt": 2, "rambuf": 4}


# ── Machine-local tool configuration ─────────────────────────────────

def _load_local_config() -> dict:
    """Merge the optional, gitignored per-machine config files. Project-level
    build.local[.os].json wins over the repo-root tools.local[.os].json; the
    per-OS variant wins over the plain one."""
    merged: dict = {}
    for candidate in (
        REPO / "tools.local.json",
        REPO / f"tools.local.{OS_NAME}.json",
        PROJECT / "build.local.json",
        PROJECT / f"build.local.{OS_NAME}.json",
    ):
        if candidate.is_file():
            try:
                merged.update(json.loads(candidate.read_text(encoding="utf-8")))
            except (OSError, ValueError) as exc:
                sys.exit(f"ERROR: cannot read {candidate}: {exc}")
    return merged


_LOCAL = _load_local_config()


def _local_value(key: str) -> Path | None:
    value = _LOCAL.get(key)
    return Path(os.path.expandvars(os.path.expanduser(str(value)))) if value else None


def _local_path(key: str) -> Path | None:
    path = _local_value(key)
    if path and not path.exists():
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


def _deps_cache() -> Path:
    """Per-user, per-OS dependency cache unless deps_cache is configured."""
    local = _local_value("deps_cache")
    if local:
        return local
    if IS_WINDOWS:
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local"))
    elif sys.platform == "darwin":
        base = Path.home() / "Library/Caches"
    else:
        base = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    return base / "BKPT" / "ViewAlyzer-Examples" / "deps"


def _recorder_default() -> Path:
    env = os.environ.get("VIEWALYZER_RECORDER_DIR")
    return Path(env) if env else REPO.parent / "ViewAlyzer" / "ViewAlyzerRecorder"


def _toolchain_env(toolchain_bin: Path | None) -> dict[str, str]:
    """--toolchain-bin, then CubeCLT's Arm GNU tools, then PATH; CubeCLT's
    CMake/Ninja only when PATH has none."""
    env = os.environ.copy()
    first = [toolchain_bin.resolve()] if toolchain_bin else []
    last = []
    if CUBECLT:
        first.append(CUBECLT / "GNU-tools-for-STM32" / "bin")
        last += [CUBECLT / "CMake" / "bin", CUBECLT / "Ninja" / "bin"]
    env["PATH"] = os.pathsep.join([*map(str, first), env.get("PATH", ""), *map(str, last)])
    return env


# ── Build ────────────────────────────────────────────────────────────

def run(argv, **kwargs) -> str:
    return subprocess.check_output([str(x) for x in argv], text=True,
                                   stderr=subprocess.STDOUT, **kwargs)


def dependency(directory: Path, url: str, commit: str, sparse: list[str] | None = None) -> Path:
    if not directory.exists():
        print(f"Fetching {url} @ {commit[:12]} into {directory}")
        directory.mkdir(parents=True)
        run(["git", "init", directory])
        run(["git", "-C", directory, "remote", "add", "origin", url])
        run(["git", "-C", directory, "fetch", "--depth=1", "origin", commit])
        if sparse:
            run(["git", "-C", directory, "sparse-checkout", "set", *sparse])
        run(["git", "-C", directory, "checkout", "--detach", commit])
    return verify(directory, commit)


def verify(directory: Path, commit: str) -> Path:
    directory = directory.resolve()
    actual = run(["git", "-C", directory, "rev-parse", "HEAD"]).strip()
    dirty = run(["git", "-C", directory, "status", "--porcelain", "--untracked-files=all", "--ignored"]).strip()
    if actual != commit or dirty:
        raise ValueError(f"Expected clean dependency {directory} at {commit}; got {actual}, dirty={bool(dirty)}")
    return directory


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def flash(image: Path, probe: str, device: str, serial: str | None) -> None:
    if probe == "jlink":
        script = Path(tempfile.mkstemp(suffix=".jlink")[1])
        script.write_text(f"loadfile {image}\nr\ng\nqc\n", encoding="utf-8")
        cmd = [_jlink_exe(), "-NoGui", "1", "-device", device,
               "-if", "SWD", "-speed", "4000", "-autoconnect", "1"]
        if serial:
            cmd += ["-SelectEmuBySN", serial]
        cmd += ["-CommandFile", script]
        try:
            print("+", " ".join(map(str, cmd)))
            if subprocess.call([str(c) for c in cmd]) != 0:
                sys.exit("ERROR: J-Link flashing failed")
        finally:
            script.unlink(missing_ok=True)
    else:
        cli = _tool("STM32_Programmer_CLI", "STM32CubeProgrammer/bin",
                    local_key="stm32_programmer_cli")
        connect = ["-c", "port=SWD"] + ([f"sn={serial}"] if serial else [])
        cmd = [cli, *connect, "-w", image, "-v", "-rst"]
        print("+", " ".join(map(str, cmd)))
        if subprocess.call([str(c) for c in cmd]) != 0:
            sys.exit("ERROR: STM32CubeProgrammer flashing failed")
    print("Flashed and reset.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--board", choices=BOARDS, default="g474",
                        help="target board (default: g474)")
    parser.add_argument("--transport", choices=TRANSPORTS, default="rambuf",
                        help="transport variant to build (default: rambuf; rtt is F767 only)")
    parser.add_argument("--flash", action="store_true",
                        help="flash the board after building")
    parser.add_argument("--serial", metavar="SN",
                        help="debug probe serial number to flash through "
                             "(needed when several probes are connected)")
    parser.add_argument("--clean", action="store_true",
                        help="remove the variant's build directory first")
    parser.add_argument("--recorder-off", action="store_true",
                        help="build an uninstrumented control image")
    parser.add_argument("--build-dir", type=Path,
                        help="exact output directory (default: build/<os>-<board>-<transport>)")
    parser.add_argument("--recorder", type=Path, default=_recorder_default(),
                        help="ViewAlyzerRecorder checkout (default: $VIEWALYZER_RECORDER_DIR "
                             "or ../ViewAlyzer/ViewAlyzerRecorder next to this repo)")
    parser.add_argument("--cache", type=Path, default=_deps_cache(),
                        help="dependency cache for the pinned checkouts "
                             "(default: per-user cache, or deps_cache in tools.local.json)")
    parser.add_argument("--kernel-dir", "--kernel", dest="kernel", type=Path,
                        help="existing clean CMSIS-RTX v5.9.1 checkout instead of the cache")
    parser.add_argument("--cmsis-dir", "--cmsis", dest="cmsis", type=Path,
                        help="existing clean CMSIS_6 v6.4.0 checkout instead of the cache")
    parser.add_argument("--cube-dir", "--cube", dest="cube", type=Path,
                        help="existing pinned x-cube-azrtos-g4 or -f7 checkout instead of the cache")
    parser.add_argument("--toolchain-bin", type=Path,
                        help="Arm GNU tools bin directory when not on PATH or in STM32CubeCLT")
    args = parser.parse_args()
    if args.board == "g474" and args.transport == "rtt":
        parser.error("The G474 uses ST-Link: select swo or rambuf")
    donor_url, cube_commit, cube_name, nucleo, board, cpuid, probe = BOARDS[args.board]
    recorder = args.recorder.resolve()
    if not (recorder / "rtx5/VA_Adapter_RTX5.c").is_file():
        parser.error(f"No RTX5 adapter in {recorder}: pass --recorder or set VIEWALYZER_RECORDER_DIR")
    variant = f"{OS_NAME}-{args.board}-{args.transport}" + ("-off" if args.recorder_off else "")
    build = (args.build_dir or PROJECT / "build" / variant).expanduser().resolve()
    cache = args.cache.expanduser().resolve()
    if cache == REPO or REPO in cache.parents:
        parser.error(f"The dependency cache must be outside this repository: {cache}")
    in_build_tree = build == PROJECT / "build" or PROJECT / "build" in build.parents
    for source in [REPO, recorder.parent, *(p.resolve() for p in (args.kernel, args.cmsis, args.cube) if p)]:
        if (build == source or source in build.parents) and not (source == REPO and in_build_tree):
            parser.error(f"Build outputs must be under {PROJECT / 'build'} or outside "
                         f"source repositories: {build}")
    sparse = ["Drivers", f"Projects/{nucleo}/Applications/ThreadX/Tx_Thread_Creation"]
    kernel = verify(args.kernel, KERNEL_COMMIT) if args.kernel else dependency(cache / "CMSIS-RTX", *KERNEL)
    cmsis = verify(args.cmsis, CMSIS_COMMIT) if args.cmsis else dependency(cache / "CMSIS_6", *CMSIS)
    cube = (verify(args.cube, cube_commit) if args.cube
            else dependency(cache / cube_name, donor_url, cube_commit, sparse=sparse))
    env = _toolchain_env(args.toolchain_bin)
    tools = {name: shutil.which(name, path=env["PATH"]) for name in
             ["cmake", "ninja", "arm-none-eabi-gcc", "arm-none-eabi-nm"]}
    missing = [name for name, value in tools.items() if value is None]
    if missing:
        parser.error(f"Missing required tools: {', '.join(missing)}. Install STM32CubeCLT, set "
                     "stm32cubeclt_root in tools.local.json, or pass --toolchain-bin")
    extensions = {".c", ".h", ".s", ".S", ".ld", ".cmake", ".txt", ".py"}
    roots = [PROJECT, recorder / "core", recorder / "rtx5", kernel / "Source",
             kernel / "Include", kernel / "Config", cmsis / "CMSIS/Core/Include",
             cmsis / "CMSIS/RTOS2", cube / "Drivers/CMSIS", cube / "Projects"]
    if args.transport == "rtt":
        roots.append(REPO / "freertos/Nucleo_F446RE/SEGGER_RTT")
    outputs = [PROJECT / "build", build]
    def inputs():
        return {str(p): sha256(p) for root in roots for p in sorted(root.rglob("*"))
                if p.is_file() and p.suffix in extensions
                and not any(out in p.parents for out in outputs)}
    if args.clean and build.exists():
        if not (build / "CMakeCache.txt").is_file() and any(build.iterdir()):
            parser.error(f"--clean refuses to delete {build}: it is not a CMake build directory")
        shutil.rmtree(build)
    before = inputs()
    build.mkdir(parents=True, exist_ok=True)
    manifest = build / "build-manifest.json"
    manifest.unlink(missing_ok=True)
    configure = [tools["cmake"], "-S", PROJECT, "-B", build, "-G", "Ninja",
                 f"-DCMAKE_MAKE_PROGRAM={tools['ninja']}",
                 f"-DBOARD={args.board}", f"-DVA_TRANSPORT={TRANSPORTS[args.transport]}",
                 f"-DVA_ENABLED={'OFF' if args.recorder_off else 'ON'}",
                 f"-DCUBE_DIR={cube.as_posix()}", f"-DRTX5_DIR={kernel.as_posix()}",
                 f"-DCMSIS_DIR={cmsis.as_posix()}", f"-DVIEWALYZER_RECORDER_DIR={recorder.as_posix()}"]
    log = ""
    try:
        log = run(configure, env=env)
        log += run([tools["cmake"], "--build", build, "--parallel", "6"], env=env)
    except subprocess.CalledProcessError as exc:
        (build / "build.log").write_text(log + exc.output, encoding="utf-8")
        sys.exit(f"{exc.output}\nERROR: build failed; full log in {build / 'build.log'}")
    (build / "build.log").write_text(log, encoding="utf-8")
    if inputs() != before:
        raise RuntimeError("Sources changed during build; rebuild after edits finish")
    elf = build / "rtx5_recorder.elf"
    symbols = {}
    for line in run([tools["arm-none-eabi-nm"], "--defined-only", elf], env=env).splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    manifest.write_text(json.dumps({
        "schema_version": 1, "board": board, "cpuid_part": cpuid,
        "cpu_hz": 80000000 if args.board == "g474" else 216000000,
        "kernel": "RTX5", "kernel_version": "5.9.1", "kernel_commit": KERNEL_COMMIT,
        "cmsis_commit": CMSIS_COMMIT, "cube_commit": cube_commit,
        "transport": args.transport, "recorder_enabled": not args.recorder_off,
        "elf": str(elf), "elf_sha256": sha256(elf),
        "hex": str(elf.with_suffix(".hex")), "hex_sha256": sha256(elf.with_suffix(".hex")),
        "compiler": run([tools["arm-none-eabi-gcc"], "--version"], env=env).splitlines()[0],
        "symbols": symbols, "inputs": before,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"Built RTX5 5.9.1 / {board} / {args.transport}: {elf}")
    if args.flash:
        flash(elf, probe, board, args.serial)
    elif args.serial:
        print("NOTE: --serial has no effect without --flash.")


if __name__ == "__main__":
    main()
