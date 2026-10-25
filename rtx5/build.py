#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Build RTX5 firmware from verified external source checkouts; never touches hardware."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

PROJECT = Path(__file__).resolve().parent
REPO = PROJECT.parent
KERNEL_COMMIT = "b566de42a5bf77d8b080f521cefc38ec7c45a3cc"
CMSIS_COMMIT = "af1b110e46a1720349f6f125e386e126767699bd"
BOARDS = {
    "g474": ("b79cbdcc0e88f6f0db9689f98c99595e7d2769fa", "STM32G474RE", 0xC24),
    "f767": ("5ce63d50c4ffeb9e6c7e8c5ec567b424cffcd223", "STM32F767ZI", 0xC27),
}
TRANSPORTS = {"swo": 1, "rtt": 2, "rambuf": 4}


def run(argv, **kwargs) -> str:
    return subprocess.check_output([str(x) for x in argv], text=True,
                                   stderr=subprocess.STDOUT, **kwargs)


def verify(directory: Path, commit: str) -> Path:
    directory = directory.resolve()
    actual = run(["git", "-C", directory, "rev-parse", "HEAD"]).strip()
    dirty = run(["git", "-C", directory, "status", "--porcelain", "--untracked-files=all", "--ignored"]).strip()
    if actual != commit or dirty:
        raise ValueError(f"Expected clean dependency {directory} at {commit}; got {actual}, dirty={bool(dirty)}")
    return directory


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", required=True, choices=BOARDS)
    parser.add_argument("--transport", choices=TRANSPORTS, default="rambuf")
    parser.add_argument("--kernel", type=Path, required=True, help="Clean CMSIS-RTX v5.9.1 checkout")
    parser.add_argument("--cmsis", type=Path, required=True, help="Clean CMSIS_6 v6.4.0 checkout")
    parser.add_argument("--cube", type=Path, required=True, help="Pinned x-cube-azrtos-g4 or -f7 checkout")
    parser.add_argument("--recorder", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--toolchain-bin", type=Path)
    parser.add_argument("--recorder-off", action="store_true", help="Build an uninstrumented control image")
    args = parser.parse_args()
    if args.board == "g474" and args.transport == "rtt":
        parser.error("The G474 uses ST-Link: select swo or rambuf")
    recorder = args.recorder.resolve()
    build = args.build_dir.resolve()
    for source in [REPO, recorder.parent, args.kernel.resolve(), args.cmsis.resolve(), args.cube.resolve()]:
        if build == source or source in build.parents:
            parser.error(f"Build outputs must be outside source repositories: {build}")
    if not (recorder / "rtx5/VA_Adapter_RTX5.c").is_file():
        parser.error("--recorder must contain the RTX5 adapter")
    kernel = verify(args.kernel, KERNEL_COMMIT)
    cmsis = verify(args.cmsis, CMSIS_COMMIT)
    cube_commit, board, cpuid = BOARDS[args.board]
    cube = verify(args.cube, cube_commit)
    env = os.environ.copy()
    if args.toolchain_bin:
        env["PATH"] = str(args.toolchain_bin.resolve()) + os.pathsep + env.get("PATH", "")
    tools = {name: shutil.which(name, path=env["PATH"]) for name in
             ["cmake", "ninja", "arm-none-eabi-gcc", "arm-none-eabi-nm"]}
    if any(value is None for value in tools.values()):
        parser.error(f"Missing required tools: {tools}")
    extensions = {".c", ".h", ".s", ".S", ".ld", ".cmake", ".txt", ".py"}
    roots = [PROJECT, recorder / "core", recorder / "rtx5", kernel / "Source",
             kernel / "Include", kernel / "Config", cmsis / "CMSIS/Core/Include",
             cmsis / "CMSIS/RTOS2", cube / "Drivers/CMSIS", cube / "Projects"]
    if args.transport == "rtt":
        roots.append(REPO / "freertos/Nucleo_F446RE/SEGGER_RTT")
    def inputs():
        return {str(p): sha256(p) for root in roots for p in sorted(root.rglob("*"))
                if p.is_file() and p.suffix in extensions}
    before = inputs()
    build.mkdir(parents=True, exist_ok=True)
    manifest = build / "build-manifest.json"
    manifest.unlink(missing_ok=True)
    configure = [tools["cmake"], "-S", PROJECT, "-B", build, "-G", "Ninja",
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
        raise
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


if __name__ == "__main__":
    main()
