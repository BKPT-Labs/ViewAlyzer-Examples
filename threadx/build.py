#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Build pinned STM32 ThreadX examples; no hardware access.

Dependencies and build outputs must be outside this repository. Existing
dependency checkouts are verified, never reset or modified. Missing checkouts
are fetched by exact commit into the supplied cache. Pass --toolchain-bin
when Arm GNU tools are not on PATH (for example STM32CubeCLT's GNU-tools bin).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

PROJECT = Path(__file__).resolve().parent
REPO = PROJECT.parent
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


def run(argv: list[str | Path], **kwargs) -> str:
    return subprocess.check_output([str(x) for x in argv], text=True,
                                   stderr=subprocess.STDOUT, **kwargs)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_inputs(recorder: Path, transport: str, dependencies: list[Path]) -> dict[str, str]:
    extensions = {".c", ".h", ".s", ".S", ".ld", ".cmake", ".py", ".txt"}
    roots = [PROJECT, recorder / "core", recorder / "threadx", *dependencies]
    inputs = {str(path): sha256(path) for root in roots for path in sorted(root.rglob("*"))
              if path.is_file() and path.suffix in extensions}
    if transport == "rtt":
        inputs.update({str(path): sha256(path) for path in
                       (REPO / "freertos/Nucleo_F446RE/SEGGER_RTT").glob("*.[ch]")})
    return inputs


def external(path: Path) -> Path:
    path = path.resolve()
    if path == REPO or REPO in path.parents:
        raise ValueError(f"Build/cache outputs must be outside the repository: {path}")
    return path


def dependency(directory: Path, url: str, commit: str, sparse: list[str] | None = None) -> Path:
    if not directory.exists():
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", choices=("f767", "g474", "u575"), default="f767")
    parser.add_argument("--kernel", choices=KERNELS, default="eclipse")
    parser.add_argument("--transport", choices=TRANSPORTS, default="rambuf")
    parser.add_argument("--u575-security", choices=("flat", "trustzone_nonsecure"), default="flat",
                        help="U575 only: flat TZEN=0 image, or a secure boot / NonSecure ThreadX pair")
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--recorder", type=Path, required=True)
    parser.add_argument("--toolchain-bin", type=Path)
    args = parser.parse_args()
    paired = args.u575_security == "trustzone_nonsecure"
    if paired and args.board != "u575":
        parser.error("--u575-security trustzone_nonsecure requires --board u575")
    cache, build = external(args.cache), external(args.build_dir)
    recorder = args.recorder.resolve()
    if not (recorder / "threadx/VA_Adapter_ThreadX.c").is_file():
        parser.error("--recorder must contain the ThreadX adapter")
    env = os.environ.copy()
    if args.toolchain_bin:
        env["PATH"] = str(args.toolchain_bin.resolve()) + os.pathsep + env.get("PATH", "")
    tools = {name: shutil.which(name, path=env["PATH"]) for name in
             ("cmake", "ninja", "arm-none-eabi-gcc", "arm-none-eabi-nm")}
    if any(value is None for value in tools.values()):
        parser.error(f"Missing required tools: {tools}")
    if args.board == "f767":
        donor = dependency(cache / "x-cube-azrtos-f7", *DONOR, sparse=["Drivers",
            "Projects/STM32F767ZI-Nucleo/Applications/ThreadX/Tx_Thread_Creation"])
        project, donor_var, donor_sha = PROJECT / "Nucleo_F767ZI", "CUBE_F7_DIR", DONOR[1]
        board, cpu = "STM32F767ZI", 216000000
    elif args.board == "g474":
        if args.transport == "rtt":
            parser.error("The G474 example uses ST-Link: select swo or rambuf")
        donor = dependency(cache / "x-cube-azrtos-g4", *G4_DONOR, sparse=["Drivers",
            "Projects/NUCLEO-G474RE/Applications/ThreadX/Tx_Thread_Creation"])
        project, donor_var, donor_sha = PROJECT / "Nucleo_G474RE", "CUBE_G4_DIR", G4_DONOR[1]
        board, cpu = "STM32G474RE", 170000000
    else:
        if args.transport == "rtt":
            parser.error("The U575 example uses ST-Link: select swo or rambuf")
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
    build.mkdir(parents=True, exist_ok=True)
    (build / "build-manifest.json").unlink(missing_ok=True)
    inputs = source_inputs(recorder, args.transport, dependency_sources)
    configure = [tools["cmake"], "-S", project, "-B", build, "-G", "Ninja",
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
        raise
    (build / "build.log").write_text(log, encoding="utf-8")
    elf = build / "threadx_recorder.elf"
    symbols = {}
    for line in run([tools["arm-none-eabi-nm"], "--defined-only", elf], env=env).splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    if source_inputs(recorder, args.transport, dependency_sources) != inputs:
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
    print(f"Built ThreadX {version} / {args.transport}: {elf}")


if __name__ == "__main__":
    main()
