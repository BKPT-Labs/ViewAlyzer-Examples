# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Check or refresh the explicit offline consumer copies of canonical domains.

Run with --apply to copy, otherwise report drift without writing. Missing
consumer checkouts are skipped. Historical test fixtures are never overwritten.
"""
import argparse
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parent
BUNDLED = ("stm32-ethernet", "imu-6dof", "imu-demo", "joulescope", "stm32u575-usbx-threadx")
AUTHORING = ("counter", "ethernet", "middleware", "actions", "instrument")


def copies(workspace):
    for name in BUNDLED:
        yield "ViewAlyzer-RS", ROOT / (f"authoring-examples/{name}.vadomain" if name == "imu-demo" else f"catalog/{name}.vadomain"), workspace / f"ViewAlyzer-RS/domains/{name}.vadomain"
    for name in AUTHORING:
        yield "ViewAlyzer-RS", ROOT / f"authoring-examples/{name}.vadomain", workspace / f"ViewAlyzer-RS/docs/examples/trace-domains/{name}.vadomain"
    for consumer, folder, pattern in (
        ("stm32n6-demo", "domains", "stm32-ethernet.vadomain"),
        ("stm32wb-demo", "domains", "imu-6dof.vadomain"),
        ("stm32wb-ble-demo", "domains", "ble-*.vadomain"),
        ("hil-regression-tests", "projects/Domains/firmware/usbx-threadx-u575/domains", "stm32u575-usbx-threadx.vadomain"),
        ("hil-regression-tests", "wip/n6/ethernet-n6/domains", "stm32-ethernet.vadomain"),
    ):
        for source in sorted((ROOT / "catalog").glob(pattern)):
            yield consumer, source, workspace / consumer / folder / source.name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=ROOT.parents[1])
    parser.add_argument("--consumer", action="append", choices=["ViewAlyzer-RS", "stm32n6-demo", "stm32wb-demo", "stm32wb-ble-demo", "hil-regression-tests"])
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    changed = checked = 0
    for consumer, source, target in copies(args.workspace.resolve()):
        if args.consumer and consumer not in args.consumer:
            continue
        if not (args.workspace / consumer).is_dir():
            continue
        checked += 1
        if target.is_file() and source.read_bytes() == target.read_bytes():
            continue
        changed += 1
        print(f"{'COPY' if args.apply else 'DRIFT'} {target}")
        if args.apply:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
    print(f"Checked {checked} consumer files; {changed} {'updated' if args.apply else 'different'}.")
    return int(changed > 0 and not args.apply)


if __name__ == "__main__":
    raise SystemExit(main())
