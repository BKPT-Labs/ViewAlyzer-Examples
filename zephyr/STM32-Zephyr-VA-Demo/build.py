#!/usr/bin/env python3
"""
Build / flash / debug helper for the ViewAlyzer Zephyr demo.

Wraps `west` and sets the required environment (Zephyr base, toolchain,
probe tools) so builds work the same from a terminal, an IDE task, or a
script, on Windows, macOS, and Linux.

Usage:
    python3 build.py                    # build the default board (g4)
    python3 build.py build g4
    python3 build.py build h5
    python3 build.py clean f4           # pristine rebuild
    python3 build.py flash g4           # build + flash (board's default runner)
    python3 build.py flash f4 jlink     # build + flash with an explicit runner
    python3 build.py debug g4
    python3 build.py menuconfig g4

Boards: g4 (nucleo_g474re, default), f4 (nucleo_f446re),
        h5 (nucleo_h503rb), h7 (stm32h750b_dk)

Transport selection (default: the board's own transport from boards/<board>.conf):
    python3 build.py build g4 --swo         # ITM / SWO
    python3 build.py build g4 --rtt         # SEGGER RTT
    python3 build.py build g4 --varambuf    # ViewAlyzer RAM buffer (probe memory reads)
    python3 build.py build g4 --snapshot    # RAM buffer in post-mortem snapshot mode
    python3 build.py flash g4 --varambuf

Each transport builds into its own directory (e.g. build-varambuf/) so the
per-board default build is never disturbed and switching never hits a stale
Kconfig cache.

Machine-specific paths (Zephyr, toolchain, OpenOCD, J-Link) are resolved in
this order — first match wins:
    1. build.local.{win|linux|mac}.json, then build.local.json, next to this
       script (per-project overrides)
    2. tools.local.{win|linux|mac}.json, then tools.local.json, at the repo
       root (one master file shared by every example project)
    3. environment variables (ZEPHYR_BASE, STM32CUBECLT_ROOT,
       GNUARMEMB_TOOLCHAIN_PATH, OPENOCD_BIN, OPENOCD_SCRIPTS, JLINK_COMMANDER)
    4. auto-detection (standard install locations / PATH)

All the .local. files are gitignored — put machine-specific paths there, never
in committed files. See tools.local.example.json at the repo root for the keys
(the per-OS variants exist so a dual-boot machine can keep one checkout).
"""

import argparse
from datetime import datetime
import glob
import json
import os
import sys
import shutil
import subprocess
from dataclasses import dataclass, replace
from pathlib import Path

# ── Paths ────────────────────────────────────────────────────────────
PROJECT_DIR = Path(__file__).resolve().parent

IS_WINDOWS = os.name == "nt"


def _load_local_config() -> dict:
    """Merge the optional, gitignored per-machine config files.

    Project-level build.local[.os].json wins over the repo-root master
    tools.local[.os].json; the per-OS variant wins over the plain one.
    """
    os_suffix = "win" if IS_WINDOWS else ("mac" if sys.platform == "darwin" else "linux")
    merged: dict = {}
    layers = [
        PROJECT_DIR.parent.parent / "tools.local.json",
        PROJECT_DIR.parent.parent / f"tools.local.{os_suffix}.json",
        PROJECT_DIR / "build.local.json",
        PROJECT_DIR / f"build.local.{os_suffix}.json",
    ]
    for candidate in layers:   # later layers override earlier ones
        if candidate.is_file():
            try:
                merged.update(json.loads(candidate.read_text(encoding="utf-8")))
            except (OSError, ValueError) as exc:
                print(f"ERROR: cannot read {candidate}: {exc}", file=sys.stderr)
                sys.exit(1)
    return merged


_LOCAL_CONFIG = _load_local_config()


def _local_path(key: str) -> Path | None:
    value = _LOCAL_CONFIG.get(key)
    if not value:
        return None
    path = Path(os.path.expandvars(os.path.expanduser(str(value))))
    if not path.exists():
        print(f"WARNING: {key} in a .local. config points at '{path}', which does not exist; ignoring.")
        return None
    return path

_zephyr_local = _local_path("zephyr_base")
_zephyr_env = os.environ.get("ZEPHYR_BASE")
if _zephyr_local:
    ZEPHYR_BASE = _zephyr_local
elif _zephyr_env:
    ZEPHYR_BASE = Path(_zephyr_env)
else:
    # Walk up from the project looking for a sibling 'zephyr' checkout.
    for _candidate in (parent / "zephyr" for parent in (PROJECT_DIR, *PROJECT_DIR.parents)):
        if (_candidate / "west.yml").exists():
            ZEPHYR_BASE = _candidate
            break
    else:
        print("ERROR: Cannot locate Zephyr.  Set ZEPHYR_BASE in the environment,", file=sys.stderr)
        print('       or create build.local.json here with {"zephyr_base": "<path>"}.', file=sys.stderr)
        sys.exit(1)

# A very common mistake is pointing ZEPHYR_BASE at the *west workspace*
# (the directory holding .west/, zephyr/, modules/) instead of the Zephyr
# repo itself.  CMake then fails deep inside ZephyrConfig.cmake with an
# unhelpful "include could not find requested file: zephyr_default".
# Silently correct it, and tell the user what we did.
if not (ZEPHYR_BASE / "share" / "zephyr-package").is_dir():
    _nested = ZEPHYR_BASE / "zephyr"
    if (_nested / "share" / "zephyr-package").is_dir():
        print(f"NOTE: '{ZEPHYR_BASE}' is a west workspace, not a Zephyr checkout;")
        print(f"      using '{_nested}' instead.")
        ZEPHYR_BASE = _nested
    else:
        print(f"ERROR: '{ZEPHYR_BASE}' does not look like a Zephyr checkout", file=sys.stderr)
        print("       (no share/zephyr-package/ inside it).  Point ZEPHYR_BASE at the", file=sys.stderr)
        print("       'zephyr' directory of your west workspace, e.g. <workspace>/zephyr.", file=sys.stderr)
        sys.exit(1)

DEFAULT_BUILD_DIR = PROJECT_DIR / "build"


@dataclass(frozen=True)
class HostTools:
    toolchain_path: Path
    openocd_bin: str
    openocd_scripts: Path
    jlink_commander: str


def _existing_path_from_candidates(*candidates: str) -> Path | None:
    for candidate in candidates:
        expanded = os.path.expandvars(os.path.expanduser(candidate))
        matches = sorted(glob.glob(expanded), reverse=True)
        for match in matches:
            path = Path(match)
            if path.exists():
                return path
    return None


def _resolve_cubeclt_root() -> Path | None:
    local = _local_path("stm32cubeclt_root")
    if local is not None:
        return local

    env_root = os.environ.get("STM32CUBECLT_ROOT")
    if env_root:
        root = Path(os.path.expandvars(os.path.expanduser(env_root)))
        if root.exists():
            return root

    if IS_WINDOWS:
        return _existing_path_from_candidates(
            "C:/ST/STM32CubeCLT_*",
            "C:/Program Files/STMicroelectronics/STM32CubeCLT_*",
        )

    return _existing_path_from_candidates(
        "~/st/stm32cubeclt_*",
        "/opt/st/stm32cubeclt_*",
        "/usr/local/st/stm32cubeclt_*",
    )


def _resolve_toolchain_path(cubeclt_root: Path | None) -> Path | None:
    local = _local_path("gnuarmemb_toolchain_path")
    if local is not None:
        return local

    env_toolchain = os.environ.get("GNUARMEMB_TOOLCHAIN_PATH")
    if env_toolchain:
        path = Path(os.path.expandvars(os.path.expanduser(env_toolchain)))
        if path.exists():
            return path

    if cubeclt_root is not None:
        candidate = cubeclt_root / "GNU-tools-for-STM32"
        if candidate.exists():
            return candidate

    if IS_WINDOWS:
        return _existing_path_from_candidates(
            "C:/ST/STM32CubeCLT_*/GNU-tools-for-STM32",
            "C:/Program Files/STMicroelectronics/STM32CubeCLT_*/GNU-tools-for-STM32",
        )

    return _existing_path_from_candidates(
        "~/st/stm32cubeclt_*/GNU-tools-for-STM32",
        "/opt/st/stm32cubeclt_*/GNU-tools-for-STM32",
        "/usr/local/st/stm32cubeclt_*/GNU-tools-for-STM32",
    )


def _resolve_openocd_bin(cubeclt_root: Path | None) -> str:
    local = _local_path("openocd_bin")
    if local is not None:
        return str(local)

    env_bin = os.environ.get("OPENOCD_BIN")
    if env_bin:
        return env_bin

    executable_names = ["openocd.exe", "openocd"] if IS_WINDOWS else ["openocd"]
    for name in executable_names:
        detected = shutil.which(name)
        if detected:
            return detected

    if cubeclt_root is not None:
        exe_name = "openocd.exe" if IS_WINDOWS else "openocd"
        candidate = cubeclt_root / "OpenOCD" / "bin" / exe_name
        if candidate.exists():
            return str(candidate)

    fallback = _existing_path_from_candidates(
        "C:/ST/STM32CubeCLT_*/OpenOCD/bin/openocd.exe",
        "C:/Program Files/STMicroelectronics/STM32CubeCLT_*/OpenOCD/bin/openocd.exe",
        "/usr/local/bin/openocd",
        "/opt/homebrew/bin/openocd",
        "/usr/bin/openocd",
    )
    if fallback is not None:
        return str(fallback)

    return "openocd.exe" if IS_WINDOWS else "openocd"


def _openocd_paired_scripts(openocd_bin: str) -> Path | None:
    """The scripts tree installed alongside `openocd_bin`, if there is one.

    An OpenOCD binary and its scripts tree are a matched set: mixing a new
    binary with an old tree (or vice versa) fails with confusing errors deep
    inside a .cfg file: a missing `source`d helper, or a transport the
    selected adapter driver no longer supports.  <prefix>/bin/openocd pairs
    with <prefix>/share/openocd/scripts on every platform, and the same holds
    for the STM32CubeCLT layout (<root>/OpenOCD/bin + <root>/OpenOCD/share).
    """
    try:
        bin_path = Path(openocd_bin)
        if not bin_path.is_absolute():
            resolved = shutil.which(openocd_bin)
            if resolved is None:
                return None
            bin_path = Path(resolved)
        candidate = bin_path.resolve().parent.parent / "share" / "openocd" / "scripts"
    except OSError:
        return None
    return candidate if candidate.is_dir() else None


def _same_dir(a: Path, b: Path) -> bool:
    try:
        return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))
    except OSError:
        return False


def _resolve_openocd_scripts(cubeclt_root: Path | None, openocd_bin: str | None = None) -> Path:
    local = _local_path("openocd_scripts")
    if local is not None:
        return local

    paired = _openocd_paired_scripts(openocd_bin) if openocd_bin else None

    env_scripts = os.environ.get("OPENOCD_SCRIPTS")
    if env_scripts:
        path = Path(os.path.expandvars(os.path.expanduser(env_scripts)))
        if path.exists():
            if paired is None or _same_dir(path, paired):
                return path
            # The environment points somewhere other than this binary's own
            # scripts.  Say so instead of failing later inside a .cfg file.
            print(f"NOTE: ignoring OPENOCD_SCRIPTS={path}")
            print(f"      ({openocd_bin} ships its own scripts in {paired};")
            print("       mixing trees breaks .cfg files.  Set OPENOCD_BIN too if")
            print("       you meant to use a different OpenOCD build.)")
            return paired

    if paired is not None:
        return paired

    if cubeclt_root is not None:
        candidate = cubeclt_root / "OpenOCD" / "share" / "openocd" / "scripts"
        if candidate.exists():
            return candidate

    fallback = _existing_path_from_candidates(
        "C:/ST/STM32CubeCLT_*/OpenOCD/share/openocd/scripts",
        "C:/Program Files/STMicroelectronics/STM32CubeCLT_*/OpenOCD/share/openocd/scripts",
        "/usr/local/share/openocd/scripts",
        "/opt/homebrew/share/openocd/scripts",
        "/usr/share/openocd/scripts",
    )
    if fallback is not None:
        return fallback

    return Path("C:/ST/STM32CubeCLT/OpenOCD/share/openocd/scripts") if IS_WINDOWS else Path("/usr/local/share/openocd/scripts")


def _resolve_jlink_commander() -> str:
    local = _local_path("jlink_commander")
    if local is not None:
        return str(local)

    env_commander = os.environ.get("JLINK_COMMANDER")
    if env_commander:
        return env_commander

    candidates = ["JLink.exe", "JLinkExe"] if IS_WINDOWS else ["JLinkExe", "JLink.exe"]
    for name in candidates:
        detected = shutil.which(name)
        if detected:
            return detected

    fallback = _existing_path_from_candidates(
        "C:/Program Files/SEGGER/JLink/JLink.exe",
        "C:/Program Files (x86)/SEGGER/JLink/JLink.exe",
        "/usr/bin/JLinkExe",
        "/usr/local/bin/JLinkExe",
    )
    if fallback is not None:
        return str(fallback)

    return "JLink.exe" if IS_WINDOWS else "JLinkExe"


def resolve_host_tools() -> HostTools:
    cubeclt_root = _resolve_cubeclt_root()
    toolchain_path = _resolve_toolchain_path(cubeclt_root)
    if toolchain_path is None:
        print("ERROR: GNU Arm Embedded toolchain not found.", file=sys.stderr)
        print("  Set GNUARMEMB_TOOLCHAIN_PATH or STM32CUBECLT_ROOT to your STM32CubeCLT install.", file=sys.stderr)
        sys.exit(1)

    openocd_bin = _resolve_openocd_bin(cubeclt_root)

    return HostTools(
        toolchain_path=toolchain_path,
        openocd_bin=openocd_bin,
        openocd_scripts=_resolve_openocd_scripts(cubeclt_root, openocd_bin),
        jlink_commander=_resolve_jlink_commander(),
    )


@dataclass(frozen=True)
class BoardConfig:
    alias: str
    board: str
    build_dir: Path
    jlink_device: str
    default_flash_runner: str
    default_debug_runner: str = "openocd"
    openocd_config: Path | None = None


BOARD_CONFIGS = {
    "g4": BoardConfig(
        alias="g4",
        board="nucleo_g474re",
        build_dir=DEFAULT_BUILD_DIR,
        jlink_device="STM32G474RE",
        default_flash_runner="openocd",
        # Zephyr's own nucleo_g474re/support/openocd.cfg selects the removed
        # 'hla_swd' transport, which fails on any recent OpenOCD and with an
        # ST-LINK/V3.  Use the project-local config instead (see the comment
        # at the top of that file).
        openocd_config=PROJECT_DIR / "openocd" / "nucleo_g474re.cfg",
    ),
    "f4": BoardConfig(
        alias="f4",
        board="nucleo_f446re",
        build_dir=PROJECT_DIR / "build-f4",
        jlink_device="STM32F446RE",
        default_flash_runner="jlink",
    ),
    "h5": BoardConfig(
        alias="h5",
        board="nucleo_h503rb",
        build_dir=PROJECT_DIR / "build-h5",
        jlink_device="STM32H503RB",
        default_flash_runner="openocd",
        openocd_config=ZEPHYR_BASE / "boards" / "st" / "nucleo_h503rb" / "support" / "openocd.cfg",
    ),
    "h7": BoardConfig(
        alias="h7",
        board="stm32h750b_dk",
        build_dir=PROJECT_DIR / "build-h7",
        jlink_device="STM32H735IG",
        default_flash_runner="openocd",
        openocd_config=ZEPHYR_BASE / "boards" / "st" / "stm32h750b_dk" / "support" / "openocd.cfg",
    ),
}

BOARD_ALIASES = {
    "g4": "g4",
    "g474": "g4",
    "nucleo_g474re": "g4",
    "f4": "f4",
    "f446": "f4",
    "nucleo_f446re": "f4",
    "h5": "h5",
    "h503": "h5",
    "nucleo_h503rb": "h5",
    "h7": "h7",
    "h750": "h7",
    "stm32h750b_dk": "h7",
}

# Transport overrides (--swo / --rtt / --varambuf / --snapshot).  Each maps to
# a Kconfig fragment merged after the board conf, plus a build-dir suffix so
# every transport keeps its own CMake/Kconfig cache and ELF.
def apply_transport(board_cfg: BoardConfig, transport: str | None) -> tuple[BoardConfig, Path | None]:
    """Return (possibly rebuilt) board config and the overlay conf to merge."""
    if transport is None:
        return board_cfg, None

    overlay = PROJECT_DIR / "transports" / f"{transport}.conf"
    if not overlay.is_file():
        raise ValueError(f"Transport overlay not found: {overlay}")

    build_dir = board_cfg.build_dir.parent / f"{board_cfg.build_dir.name}-{transport}"
    return replace(board_cfg, build_dir=build_dir), overlay


RUNNER_ALIASES = {
    "jlink": "jlink",
    "jl": "jlink",
    "stlink": "openocd",
    "st-link": "openocd",
    "st": "openocd",
    "openocd": "openocd",
    "ocd": "openocd",
}

# ── Environment ──────────────────────────────────────────────────────
def setup_env():
    """Return a copy of os.environ with every Zephyr variable set."""
    env = os.environ.copy()
    host_tools = resolve_host_tools()

    env["ZEPHYR_BASE"]              = str(ZEPHYR_BASE)
    env["ZEPHYR_TOOLCHAIN_VARIANT"] = "gnuarmemb"
    env["GNUARMEMB_TOOLCHAIN_PATH"] = str(host_tools.toolchain_path)

    # Prevent CMake from finding an incompatible Zephyr SDK
    env.pop("ZEPHYR_SDK_INSTALL_DIR", None)

    # Make sure west is on PATH
    home = Path.home()
    local_bin = str(home / ".local" / "bin")
    if local_bin not in env.get("PATH", ""):
        env["PATH"] = local_bin + os.pathsep + env.get("PATH", "")

    # If the host has no system CMake/Ninja, fall back to the copies that
    # ship inside STM32CubeCLT so a stock CubeCLT install is enough to build.
    cubeclt_root = _resolve_cubeclt_root()
    if cubeclt_root is not None:
        for tool, subdir in (("cmake", "CMake"), ("ninja", "Ninja")):
            if shutil.which(tool, path=env["PATH"]) is None:
                tool_bin = cubeclt_root / subdir / "bin"
                if tool_bin.exists():
                    env["PATH"] = str(tool_bin) + os.pathsep + env["PATH"]

    return env, host_tools


def find_west(env):
    """Return the absolute path to west, or exit with a clear message."""
    west = shutil.which("west", path=env.get("PATH"))
    if west is None:
        print("ERROR: 'west' not found on PATH.", file=sys.stderr)
        print("  Install with:  pip3 install --user west", file=sys.stderr)
        sys.exit(1)
    return west


def resolve_board(name: str | None) -> BoardConfig:
    key = BOARD_ALIASES.get((name or "g4").lower())
    if key is None:
        choices = ", ".join(sorted(BOARD_ALIASES))
        raise ValueError(f"Unknown board '{name}'. Use one of: {choices}")
    return BOARD_CONFIGS[key]


def resolve_runner(name: str | None, *, default: str) -> str:
    key = RUNNER_ALIASES.get((name or default).lower())
    if key is None:
        choices = ", ".join(sorted(RUNNER_ALIASES))
        raise ValueError(f"Unknown runner '{name}'. Use one of: {choices}")
    return key


def west_build_args(board_cfg: BoardConfig, west: str, pristine: bool, host_tools: HostTools,
                    transport_overlay: Path | None = None) -> list[str]:
    args = [
        west, "build",
        "-p", "always" if pristine else "auto",
        "-b", board_cfg.board,
        "-d", str(board_cfg.build_dir),
        str(PROJECT_DIR),
        "--",
        "-DZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb",
        f"-DGNUARMEMB_TOOLCHAIN_PATH={host_tools.toolchain_path}",
    ]
    if transport_overlay is not None:
        # Relative to the app dir; forward slashes keep it valid on Windows.
        args.append(f"-DEXTRA_CONF_FILE={transport_overlay.relative_to(PROJECT_DIR).as_posix()}")
    if project_dir_looks_like_build_dir():
        args.insert(2, "--force")
    return args


def project_dir_looks_like_build_dir() -> bool:
    return (PROJECT_DIR / "CMakeCache.txt").exists() or (PROJECT_DIR / "CMakeFiles").exists()


def generated_root_kconfig_dir() -> Path | None:
    candidate = PROJECT_DIR / "Kconfig"
    if not candidate.is_dir():
        return None

    generated_markers = ("Kconfig.modules", "Kconfig.sysbuild.modules", "Kconfig.dts")
    if any((candidate / marker).exists() for marker in generated_markers):
        return candidate

    # An EMPTY Kconfig/ is the leftover of a previous quarantine (the marker
    # files went with it, the directory did not). Zephyr still tries to parse
    # the app Kconfig and dies with "Is a directory", so sweep it too.
    if not any(candidate.iterdir()):
        return candidate

    return None


def quarantine_root_build_artifacts() -> list[str]:
    artifact_paths: list[Path] = []

    for name in ("CMakeCache.txt", "CMakeFiles", "sysbuild_modules.txt", "zephyr_modules.txt", "zephyr_settings.txt"):
        candidate = PROJECT_DIR / name
        if candidate.exists():
            artifact_paths.append(candidate)

    generated_kconfig = generated_root_kconfig_dir()
    if generated_kconfig is not None:
        artifact_paths.append(generated_kconfig)

    if not artifact_paths:
        return []

    backup_dir = PROJECT_DIR / ".root-artifact-backup" / datetime.now().strftime("auto-%Y%m%d-%H%M%S-%f")
    backup_dir.mkdir(parents=True, exist_ok=True)

    moved_names: list[str] = []
    for artifact_path in artifact_paths:
        shutil.move(str(artifact_path), str(backup_dir / artifact_path.name))
        moved_names.append(artifact_path.name)

    return moved_names


def build_dir_needs_reset(build_dir: Path) -> bool:
    cache_file = build_dir / "CMakeCache.txt"
    if not cache_file.exists():
        return False

    try:
        cache_text = cache_file.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return False

    normalized_project_dir = PROJECT_DIR.as_posix()
    normalized_zephyr_base = ZEPHYR_BASE.as_posix()
    expected_markers = (
        f"APPLICATION_SOURCE_DIR:PATH={normalized_project_dir}",
        f"ZEPHYR_BASE:PATH={normalized_zephyr_base}",
    )
    return any(marker not in cache_text for marker in expected_markers)


def reset_build_dir_if_needed(board_cfg: BoardConfig) -> bool:
    if not build_dir_needs_reset(board_cfg.build_dir):
        return False

    shutil.rmtree(board_cfg.build_dir, ignore_errors=False)
    return True


def west_runner_args(board_cfg: BoardConfig, runner: str, host_tools: HostTools) -> list[str]:
    if runner == "openocd":
        args = [
            "--runner", "openocd",
            "--openocd", host_tools.openocd_bin,
            "--openocd-search", str(host_tools.openocd_scripts),
        ]
        if board_cfg.openocd_config is not None:
            args.extend(["--config", str(board_cfg.openocd_config)])
        return args

    if runner == "jlink":
        return [
            "--runner", "jlink",
            "--",
            "--device", board_cfg.jlink_device,
            "--commander", host_tools.jlink_commander,
            "--tool-opt=-autoconnect 1",
        ]

    raise ValueError(f"Unsupported runner '{runner}'")


# ── Commands ─────────────────────────────────────────────────────────
def cmd_build(board_cfg: BoardConfig, pristine: bool = False, transport_overlay: Path | None = None):
    env, host_tools = setup_env()
    west = find_west(env)

    moved_root_artifacts = quarantine_root_build_artifacts()
    build_dir_reset = reset_build_dir_if_needed(board_cfg)

    args = west_build_args(board_cfg, west, pristine, host_tools, transport_overlay)

    print(f"{'[CLEAN] ' if pristine else ''}Building {board_cfg.board} ({board_cfg.alias}) …")
    print(f"  west     = {west}")
    print(f"  build dir   = {board_cfg.build_dir}")
    print(f"  ZEPHYR_BASE = {env['ZEPHYR_BASE']}")
    print(f"  toolchain   = gnuarmemb ({host_tools.toolchain_path})")
    if transport_overlay is not None:
        print(f"  transport   = {transport_overlay.stem} (overlay {transport_overlay.relative_to(PROJECT_DIR).as_posix()})")
    if moved_root_artifacts:
        print(f"  note        = quarantined generated root artifacts: {', '.join(moved_root_artifacts)}")
    elif project_dir_looks_like_build_dir():
        print("  note        = root contains CMake build markers, forcing west source-dir check")
    if build_dir_reset:
        print("  note        = removed stale build directory generated on a different host/path")
    print()

    return subprocess.call(args, env=env, cwd=str(PROJECT_DIR))


def cmd_menuconfig(board_cfg: BoardConfig, transport_overlay: Path | None = None):
    env, host_tools = setup_env()
    west = find_west(env)

    quarantine_root_build_artifacts()
    build_dir_reset = reset_build_dir_if_needed(board_cfg)

    args = west_build_args(board_cfg, west, False, host_tools, transport_overlay)
    # Insert -t menuconfig before the "--" separator so it's parsed by
    # west, not forwarded to CMake.
    try:
        sep = args.index("--")
        args[sep:sep] = ["-t", "menuconfig"]
    except ValueError:
        args.extend(["-t", "menuconfig"])

    print(f"Opening menuconfig for {board_cfg.board} ({board_cfg.alias}) …")
    print(f"  build dir = {board_cfg.build_dir}")
    if build_dir_reset:
        print("  note        = removed stale build directory generated on a different host/path")
    print()

    return subprocess.call(args, env=env, cwd=str(PROJECT_DIR))


def cmd_flash(board_cfg: BoardConfig, runner: str, transport_overlay: Path | None = None):
    rc = cmd_build(board_cfg, transport_overlay=transport_overlay)
    if rc != 0:
        return rc

    env, host_tools = setup_env()
    west = find_west(env)

    args = [west, "flash", "-d", str(board_cfg.build_dir), "--skip-rebuild"]
    args.extend(west_runner_args(board_cfg, runner, host_tools))

    print(f"\nFlashing {board_cfg.board} via {runner} …")
    return subprocess.call(args, env=env, cwd=str(PROJECT_DIR))


def cmd_debug(board_cfg: BoardConfig, runner: str, transport_overlay: Path | None = None):
    rc = cmd_build(board_cfg, transport_overlay=transport_overlay)
    if rc != 0:
        return rc

    env, host_tools = setup_env()
    west = find_west(env)

    args = [west, "debug", "-d", str(board_cfg.build_dir), "--skip-rebuild"]
    args.extend(west_runner_args(board_cfg, runner, host_tools))

    print(f"\nStarting debugger for {board_cfg.board} via {runner} …")
    return subprocess.call(args, env=env, cwd=str(PROJECT_DIR))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "action",
        nargs="?",
        default="build",
        choices=("build", "clean", "menuconfig", "flash", "debug"),
        help="operation to perform",
    )
    parser.add_argument(
        "board",
        nargs="?",
        default="g4",
        help="board target shorthand: g4|f4|h5|h7 or full Zephyr board name",
    )
    parser.add_argument(
        "runner",
        nargs="?",
        help="runner shorthand for flash/debug: jlink|openocd",
    )
    transport = parser.add_mutually_exclusive_group()
    transport.add_argument(
        "--swo", dest="transport", action="store_const", const="swo",
        help="force the ITM/SWO transport (overrides the board default)",
    )
    transport.add_argument(
        "--rtt", dest="transport", action="store_const", const="rtt",
        help="force the SEGGER RTT transport (overrides the board default)",
    )
    transport.add_argument(
        "--varambuf", "--rambuf", dest="transport", action="store_const", const="varambuf",
        help="force the ViewAlyzer RAM buffer transport (probe memory reads, "
             "works with ST-LINK; overrides the board default)",
    )
    transport.add_argument(
        "--snapshot", dest="transport", action="store_const", const="snapshot",
        help="RAM buffer in post-mortem snapshot mode: run untethered, read the "
             "most recent trace window out afterwards with ViewAlyzer --snapshot",
    )
    return parser.parse_args()


def main():
    args = parse_args()

    try:
        board_cfg = resolve_board(args.board)
        board_cfg, transport_overlay = apply_transport(board_cfg, args.transport)
        if args.action == "build":
            return cmd_build(board_cfg, pristine=False, transport_overlay=transport_overlay)
        if args.action == "clean":
            return cmd_build(board_cfg, pristine=True, transport_overlay=transport_overlay)
        if args.action == "menuconfig":
            return cmd_menuconfig(board_cfg, transport_overlay)
        if args.action == "flash":
            runner = resolve_runner(args.runner, default=board_cfg.default_flash_runner)
            return cmd_flash(board_cfg, runner, transport_overlay)
        if args.action == "debug":
            runner = resolve_runner(args.runner, default=board_cfg.default_debug_runner)
            return cmd_debug(board_cfg, runner, transport_overlay)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    print(f"Unknown command '{args.action}'.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
