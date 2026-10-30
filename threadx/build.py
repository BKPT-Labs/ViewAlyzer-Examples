#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Build and flash helper for the ThreadX recorder examples.

Usage:
    python3 build.py                          # F767, Eclipse ThreadX, RAM buffer
    python3 build.py --board g474 --transport swo
    python3 build.py --kernel st              # ST's ThreadX 6.4.0 instead of Eclipse 6.5.2
    python3 build.py --flash                  # build, then flash the board
    python3 build.py --flash --serial <probe-serial>
    python3 build.py --clean                  # drop the build dir first (full rebuild)

Builds go into build/<os>-<board>-<kernel>-<transport>[-tz]/ unless
--build-dir is given. Pinned kernel and ST donor checkouts are fetched by exact
commit into a per-user dependency cache (shared with the RTX5 examples).
Existing checkouts are verified, never reset or modified.

--serial pins the flash to one debug probe when several are connected
(`STM32_Programmer_CLI --list` / `JLinkExe -CommanderScript` show serials;
the ViewAlyzer CLI prints them too: `ViewAlyzer --headless --list-probes`).
The F767 flashes through a J-Link, the G474 and U575 through their onboard
ST-Link. A paired U575 TrustZone build flashes threadx_paired.hex.

Requirements: Git, CMake, Ninja, and the Arm GNU toolchain. Everything is
found automatically when STM32CubeCLT is installed. Machine-specific paths
(including deps_cache) can be set in tools.local[.win|.linux|.mac].json at the
repo root (shared by all examples) or build.local[.os].json next to this
script — see tools.local.example.json at the repo root. Committed files never
contain machine paths.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import glob
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parent
REPO = PROJECT.parent
IS_WINDOWS = os.name == "nt"
OS_NAME = "win" if IS_WINDOWS else ("mac" if sys.platform == "darwin" else "linux")
DONOR = ("https://github.com/STMicroelectronics/x-cube-azrtos-f7.git",
         "5ce63d50c4ffeb9e6c7e8c5ec567b424cffcd223")
U5_DONOR = ("https://github.com/STMicroelectronics/STM32CubeU5.git",
            "448c03b3ef00759626cb69e68a6c84eba7c7a5dc")
G4_DONOR = ("https://github.com/STMicroelectronics/x-cube-azrtos-g4.git",
            "b79cbdcc0e88f6f0db9689f98c99595e7d2769fa")
KERNELS = {
    "st": ("https://github.com/STMicroelectronics/stm32-mw-threadx.git",
           "b44bdbbbb5a9e4fb56728d2932236564c89db29a", "6.4.0"),
    "eclipse": ("https://github.com/eclipse-threadx/threadx.git",
                "93387b0a60382a29579bbad5045c68328e3751df", "6.5.2"),
}
TRANSPORTS = {"swo": 1, "rtt": 2, "rambuf": 4}
GTZC_EXAMPLE = "Projects/NUCLEO-U575ZI-Q/Examples/GTZC/GTZC_TZSC_MPCBB_TrustZone"


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

def run(argv: list[str | Path], **kwargs) -> str:
    return subprocess.check_output([str(x) for x in argv], text=True,
                                   stderr=subprocess.STDOUT, **kwargs)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_inputs(recorder: Path, transport: str, dependencies: list[Path],
                  build: Path) -> dict[str, str]:
    extensions = {".c", ".h", ".s", ".S", ".ld", ".cmake", ".py", ".txt"}
    roots = [PROJECT, recorder / "core", recorder / "threadx", *dependencies]
    outputs = [PROJECT / "build", build]
    inputs = {str(path): sha256(path) for root in roots for path in sorted(root.rglob("*"))
              if path.is_file() and path.suffix in extensions
              and not any(out in path.parents for out in outputs)}
    if transport == "rtt":
        inputs.update({str(path): sha256(path) for path in
                       (REPO / "freertos/Nucleo_F446RE/SEGGER_RTT").glob("*.[ch]")})
    return inputs


def external(path: Path) -> Path:
    path = path.resolve()
    if path == REPO or REPO in path.parents:
        raise ValueError(f"The dependency cache must be outside the repository: {path}")
    return path


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
    actual = run(["git", "-C", directory, "rev-parse", "HEAD"]).strip()
    dirty = run(["git", "-C", directory, "status", "--porcelain", "--untracked-files=all", "--ignored"]).strip()
    if actual != commit or dirty:
        raise ValueError(f"Expected clean dependency {directory} at {commit}; got {actual}, dirty={bool(dirty)}")
    return directory


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
    parser.add_argument("--board", choices=("f767", "g474", "u575"), default="f767",
                        help="target board (default: f767)")
    parser.add_argument("--kernel", choices=KERNELS, default="eclipse",
                        help="eclipse = Eclipse ThreadX 6.5.2, st = ST ThreadX 6.4.0 (default: eclipse)")
    parser.add_argument("--transport", choices=TRANSPORTS, default="rambuf",
                        help="transport variant to build (default: rambuf; rtt is F767 only)")
    parser.add_argument("--flash", action="store_true",
                        help="flash the board after building")
    parser.add_argument("--serial", metavar="SN",
                        help="debug probe serial number to flash through "
                             "(needed when several probes are connected)")
    parser.add_argument("--clean", action="store_true",
                        help="remove the variant's build directory first")
    parser.add_argument("--u575-security", choices=("flat", "trustzone_nonsecure"), default="flat",
                        help="U575 only: flat TZEN=0 image, or a secure boot / NonSecure ThreadX pair")
    parser.add_argument("--build-dir", type=Path,
                        help="exact output directory (default: build/<os>-<board>-<kernel>-<transport>)")
    parser.add_argument("--recorder", type=Path, default=_recorder_default(),
                        help="ViewAlyzerRecorder checkout (default: $VIEWALYZER_RECORDER_DIR "
                             "or ../ViewAlyzer/ViewAlyzerRecorder next to this repo)")
    parser.add_argument("--cache", type=Path, default=_deps_cache(),
                        help="dependency cache for the pinned checkouts "
                             "(default: per-user cache, or deps_cache in tools.local.json)")
    parser.add_argument("--toolchain-bin", type=Path,
                        help="Arm GNU tools bin directory when not on PATH or in STM32CubeCLT")
    args = parser.parse_args()
    paired = args.u575_security == "trustzone_nonsecure"
    if paired and args.board != "u575":
        parser.error("--u575-security trustzone_nonsecure requires --board u575")
    if args.board != "f767" and args.transport == "rtt":
        parser.error(f"The {args.board.upper()} example uses ST-Link: select swo or rambuf")
    try:
        cache = external(args.cache.expanduser())
    except ValueError as exc:
        parser.error(str(exc))
    variant = f"{OS_NAME}-{args.board}-{args.kernel}-{args.transport}" + ("-tz" if paired else "")
    build = (args.build_dir or PROJECT / "build" / variant).expanduser().resolve()
    if ((build == REPO or REPO in build.parents)
            and not (build == PROJECT / "build" or PROJECT / "build" in build.parents)):
        parser.error(f"Build outputs must be under {PROJECT / 'build'} or outside the repository: {build}")
    recorder = args.recorder.resolve()
    if build == recorder.parent or recorder.parent in build.parents:
        parser.error(f"Build outputs must be outside the recorder repository: {build}")
    if not (recorder / "threadx/VA_Adapter_ThreadX.c").is_file():
        parser.error(f"No ThreadX adapter in {recorder}: pass --recorder or set VIEWALYZER_RECORDER_DIR")
    env = _toolchain_env(args.toolchain_bin)
    tools = {name: shutil.which(name, path=env["PATH"]) for name in
             ("cmake", "ninja", "arm-none-eabi-gcc", "arm-none-eabi-nm")}
    missing = [name for name, value in tools.items() if value is None]
    if missing:
        parser.error(f"Missing required tools: {', '.join(missing)}. Install STM32CubeCLT, set "
                     "stm32cubeclt_root in tools.local.json, or pass --toolchain-bin")
    if args.board == "f767":
        donor = dependency(cache / "x-cube-azrtos-f7", *DONOR, sparse=["Drivers",
            "Projects/STM32F767ZI-Nucleo/Applications/ThreadX/Tx_Thread_Creation"])
        project, donor_var, donor_sha = PROJECT / "Nucleo_F767ZI", "CUBE_F7_DIR", DONOR[1]
        board, cpu = "STM32F767ZI", 216000000
    elif args.board == "g474":
        donor = dependency(cache / "x-cube-azrtos-g4", *G4_DONOR, sparse=["Drivers",
            "Projects/NUCLEO-G474RE/Applications/ThreadX/Tx_Thread_Creation"])
        project, donor_var, donor_sha = PROJECT / "Nucleo_G474RE", "CUBE_G4_DIR", G4_DONOR[1]
        board, cpu = "STM32G474RE", 170000000
    else:
        sparse = ["Drivers/CMSIS/Include",
            "Drivers/CMSIS/Device/ST/STM32U5xx", "Drivers/STM32U5xx_HAL_Driver",
            "Projects/NUCLEO-U575ZI-Q/Applications/ThreadX/Tx_Thread_Creation"]
        submodules = ["Drivers/CMSIS/Device/ST/STM32U5xx", "Drivers/STM32U5xx_HAL_Driver"]
        if paired:
            submodules.extend(["Drivers/BSP/STM32U5xx_Nucleo", "Drivers/BSP/Components/Common"])
            sparse.extend([GTZC_EXAMPLE, *submodules[2:]])
        donor = dependency(cache / "STM32CubeU5", *U5_DONOR, sparse=sparse)
        if paired and not (donor / GTZC_EXAMPLE / "Secure/Src/main.c").is_file():
            raise ValueError(f"Existing sparse CubeU5 checkout needs {GTZC_EXAMPLE}; "
                             "expand its sparse checkout or use a fresh external cache")
        for submodule in submodules:
            expected = run(["git", "-C", donor, "rev-parse", f"HEAD:{submodule}"]).strip()
            path = donor / submodule
            if not (path / ".git").exists():
                run(["git", "-C", donor, "submodule", "update", "--init", "--depth=1", "--", submodule])
            if run(["git", "-C", path, "rev-parse", "HEAD"]).strip() != expected or run(
                    ["git", "-C", path, "status", "--porcelain", "--untracked-files=all", "--ignored"]).strip():
                raise ValueError(f"Submodule differs from pinned CubeU5: {path}")
        project, donor_var, donor_sha = PROJECT / "Nucleo_U575ZI", "CUBE_U5_DIR", U5_DONOR[1]
        board, cpu = "STM32U575ZITx", 160000000
    kernel_url, kernel_sha, version = KERNELS[args.kernel]
    kernel = dependency(cache / f"threadx-{args.kernel}", kernel_url, kernel_sha)
    family, nucleo, port = {"f767": ("F7", "STM32F767ZI-Nucleo", "cortex_m7"),
                           "g474": ("G4", "NUCLEO-G474RE", "cortex_m4"),
                           "u575": ("U5", "NUCLEO-U575ZI-Q", "cortex_m33")}[args.board]
    dependency_sources = [donor / f"Projects/{nucleo}/Applications/ThreadX/Tx_Thread_Creation",
                          donor / "Drivers/CMSIS/Include",
                          donor / f"Drivers/CMSIS/Device/ST/STM32{family}xx/Include",
                          kernel / "common", kernel / f"ports/{port}/gnu"]
    if args.board == "u575":
        dependency_sources.extend([donor / "Drivers/STM32U5xx_HAL_Driver/Inc",
                                   donor / "Drivers/STM32U5xx_HAL_Driver/Src"])
        if paired:
            dependency_sources.extend([donor / GTZC_EXAMPLE,
                                       donor / "Drivers/BSP/STM32U5xx_Nucleo",
                                       donor / "Drivers/BSP/Components/Common"])
    elif args.board == "g474":
        dependency_sources.extend([donor / "Drivers/STM32G4xx_HAL_Driver/Inc",
                                   donor / "Drivers/STM32G4xx_HAL_Driver/Src"])
    if args.clean and build.exists():
        if not (build / "CMakeCache.txt").is_file() and any(build.iterdir()):
            parser.error(f"--clean refuses to delete {build}: it is not a CMake build directory")
        shutil.rmtree(build)
    build.mkdir(parents=True, exist_ok=True)
    (build / "build-manifest.json").unlink(missing_ok=True)
    inputs = source_inputs(recorder, args.transport, dependency_sources, build)
    configure = [tools["cmake"], "-S", project, "-B", build, "-G", "Ninja",
                 f"-DCMAKE_MAKE_PROGRAM={tools['ninja']}",
                 f"-D{donor_var}={donor.as_posix()}", f"-DTHREADX_DIR={kernel.as_posix()}",
                 f"-DVIEWALYZER_RECORDER_DIR={recorder.as_posix()}",
                 f"-DVA_TRANSPORT={TRANSPORTS[args.transport]}"]
    if args.board == "u575":
        configure.append(f"-DVA_U575_TRUSTZONE={'ON' if paired else 'OFF'}")
    log = ""
    try:
        log = run(configure, env=env)
        log += run([tools["cmake"], "--build", build, "--parallel", "8"], env=env)
    except subprocess.CalledProcessError as exc:
        (build / "build.log").write_text(log + exc.output, encoding="utf-8")
        sys.exit(f"{exc.output}\nERROR: build failed; full log in {build / 'build.log'}")
    (build / "build.log").write_text(log, encoding="utf-8")
    elf = build / "threadx_recorder.elf"
    symbols = {}
    for line in run([tools["arm-none-eabi-nm"], "--defined-only", elf], env=env).splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    if source_inputs(recorder, args.transport, dependency_sources, build) != inputs:
        raise RuntimeError("Sources changed during the build; rebuild after edits finish")
    manifest = {"schema_version": 1, "coverage_version": 2, "board": board, "cpu_clock_hz": cpu,
                "cpuid_part": {"f767": 0xC27, "g474": 0xC24, "u575": 0xD21}[args.board],
                "software_irq_exception": 27 if args.board == "u575" else 22,
                "kernel": args.kernel, "kernel_version": version, "kernel_commit": kernel_sha,
                "donor_commit": donor_sha, "transport": args.transport,
                "elf": str(elf), "elf_sha256": sha256(elf),
                "hex": str(elf.with_suffix(".hex")), "hex_sha256": sha256(elf.with_suffix(".hex")),
                "compiler": run([tools["arm-none-eabi-gcc"], "--version"], env=env).splitlines()[0],
                "symbols": symbols, "inputs": inputs}
    if paired:
        partner = build / "threadx_secure_boot.elf"
        flash_image = build / "threadx_paired.hex"
        # Both objcopy outputs carry explicit extended-address records. As in
        # the existing U575 CI recipe, join them with one final EOF record.
        records = [line for image in (partner.with_suffix(".hex"), elf.with_suffix(".hex"))
                   for line in image.read_text(encoding="ascii").splitlines()
                   if line != ":00000001FF"]
        flash_image.write_text("\n".join([*records, ":00000001FF"]) + "\n", encoding="ascii")
        manifest.update(security_mode="trustzone_nonsecure", flash_image=str(flash_image),
                        flash_image_sha256=sha256(flash_image), boot_partner={
                            "elf": str(partner), "elf_sha256": sha256(partner),
                            "hex": str(partner.with_suffix(".hex")),
                            "hex_sha256": sha256(partner.with_suffix(".hex")),
                            "flash_origin": 0x0C000000,
                            "provenance": {"repository": U5_DONOR[0], "commit": donor_sha,
                                           "example": GTZC_EXAMPLE}})
    elif args.board == "u575":
        manifest["security_mode"] = "flat"
    (build / "build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Built ThreadX {version} / {board} / {args.transport}: {elf}")
    if args.flash:
        probe = "jlink" if args.board == "f767" else "stlink"
        flash(build / "threadx_paired.hex" if paired else elf, probe, board, args.serial)
    elif args.serial:
        print("NOTE: --serial has no effect without --flash.")


if __name__ == "__main__":
    main()
