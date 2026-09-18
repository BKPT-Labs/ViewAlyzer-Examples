# Trace Domain catalog

This is the canonical, publicly contributable home of BKPT's `.vadomain` JSON
definitions. Browse the [engineering references and downloads](https://bkptlabs.com/docs/trace-domains/catalog.html),
or use [catalog.json](catalog.json) to discover the files and prerequisites.
A descriptor contains no firmware, ELF, SVD, instrument driver or recorder binary.

## Domain catalog

[`catalog/`](catalog/) contains 13 definitions for the listed firmware or recorded
channels. Match each domain to its required producer, ELF, SVD and host capabilities.
The nine BLE files share one [engineering reference](https://bkptlabs.com/docs/trace-domains/ble-stm32wb-suite.html).

| Definition | Purpose | Requirements |
|---|---|---|
| [STM32U575 USBX / ThreadX](catalog/stm32u575-usbx-threadx.vadomain) | USB controller, CDC transfers, ThreadX waits, error reports, control queue and allocator pressure. | STM32U575 SVD and matching USBX CDC ACM / ThreadX Debug ELF |
| [STM32N6 Ethernet / lwIP](catalog/stm32-ethernet.vadomain) | Follow frames from the MAC through DMA descriptors and lwIP; distinguish receive pressure, wire errors and stack drops. | STM32N657 ETH_S / RCC_S SVD and exact N6 demo ELF / lwIP layout |
| [STM32WB six-axis IMU](catalog/imu-6dof.vadomain) | Sensor samples, configuration and consumption rate reveal a stalled reader, bus errors or a reader falling behind the sensor. | STM32WB5MM-DK ISM330DHCX firmware mirrors, matching ELF and STM32WB55_CM4 SVD |
| [Joulescope power channels](catalog/joulescope.vadomain) | Group imported MCU current and power traces under Power. | Existing external MCU current / MCU power channels; recorder or import pipeline supplied separately |
| [BLE overview](catalog/ble-overview.vadomain) | Connection, command outcomes, retained errors, service registrations and observer capacity. | STM32WB BLE observation ABI 1 and matching Debug ELF |
| [BLE GATT slots 0 and 1](catalog/ble-gatt-1.vadomain) | Registered characteristic identity, properties, retained values, subscriptions and write/update outcomes. | BLE observation ABI 1; characteristic slots 0 and 1 must be populated to show values |
| [BLE GATT slots 2 and 3](catalog/ble-gatt-2.vadomain) | Registered characteristic identity, properties, retained values, subscriptions and write/update outcomes. | BLE observation ABI 1; characteristic slots 2 and 3 must be populated to show values |
| [BLE GATT slots 4 and 5](catalog/ble-gatt-3.vadomain) | Registered characteristic identity, properties, retained values, subscriptions and write/update outcomes. | BLE observation ABI 1; characteristic slots 4 and 5 must be populated to show values |
| [BLE GATT slots 6 and 7](catalog/ble-gatt-4.vadomain) | Registered characteristic identity, properties, retained values, subscriptions and write/update outcomes. | BLE observation ABI 1; characteristic slots 6 and 7 must be populated to show values |
| [BLE ST peer-to-peer profile](catalog/ble-profile-st-p2p.vadomain) | LED/button state, skipped notifications and an explicit application-mediated LED command. | ST P2P symbols; command mailbox ABI 1 and typed-actions-v1 for Send in BKPT Debug |
| [BLE STM32WB shared transport](catalog/ble-stm32wb-transport.vadomain) | Application snapshots of IPCC channel flags and the M4 interrupt mask. | STM32WB BLE observation ABI 1; firmware must refresh IPCC snapshots |
| [BLE activity slots 0-7](catalog/ble-journal-1.vadomain) | Retained command outcomes and significant asynchronous events in eight physical journal slots. | BLE observation ABI 1; combine both journal pages for the 16-slot retained window |
| [BLE activity slots 8-15](catalog/ble-journal-2.vadomain) | Retained command outcomes and significant asynchronous events in eight physical journal slots. | BLE observation ABI 1; combine both journal pages for the 16-slot retained window |

## Authoring examples

[`authoring-examples/`](authoring-examples/) contains six templates for creating
your own definitions. Adapt their placeholder symbols, registers or recorder before
use. The IMU styling template has no supplied firmware producer; the STM32WB
six-axis IMU domain in the catalog is the sensor demo.
See the [authoring guide](https://bkptlabs.com/docs/trace-domains/authoring.html).

| Definition | Purpose | Requirements |
|---|---|---|
| [IMU channel styling example](authoring-examples/imu-demo.vadomain) | A presentation-only template for user_trace channels; no matching firmware producer is supplied. | Adapt to actual recorder channel names and units; not STM32WB sensor support |
| [Packet counter example](authoring-examples/counter.vadomain) | Wrapping counters, a derived rate and an error-count finding. | Firmware must supply packets_completed and packet_errors |
| [Ethernet authoring example](authoring-examples/ethernet.vadomain) | Guarded registers, descriptor ownership and backlog checks for a placeholder MAC. | Replace ExampleMCU registers and ring layout with verified target definitions |
| [Middleware queue example](authoring-examples/middleware.vadomain) | Structured pointers, occupancy/capacity cards and consumer-progress checks. | Implement or map active_queue, queue_slots, queue_enqueued and queue_drained |
| [Typed actions example](authoring-examples/actions.vadomain) | RAM writes, an application mailbox and a register-field action. | Implement matching firmware/mailbox/SVD; actions require explicit debugger Send |
| [Instrument sidecar example](authoring-examples/instrument.vadomain) | External current samples and a high-current threshold. | Supply sensor-recorder executable and synchronization channel; no sidecar included |

`catalog.json` indexes both sections using paths relative to this directory.
Shared metadata, this README and `sync.py` remain here at the `domains/` root.

## Compatibility

These descriptors use **domain API 1**. Use current Trace Domain v1 builds of
BKPT Debug and ViewAlyzer RS, with structured reads and presentation support.
A product version number alone does not establish these capabilities.
The STM32WB firmware's observation ABI 1 and command ABI 1 are separate contracts.

The BLE adapter requires firmware hooks and observation mirrors. It is not a
radio sniffer or a generic view into CPU2 private memory. The U575 and Ethernet
domains inspect existing firmware/controller state. The IMU uses firmware mirrors.
Styling descriptors merely annotate channels already present in a recording.

## Contribute a correction or a new domain

1. Fork [ViewAlyzer-Examples](https://github.com/BKPT-Labs/ViewAlyzer-Examples)
   and edit the canonical JSON in `domains/catalog/` or
   `domains/authoring-examples/`. Keep IDs stable for revisions. Use a new ID
   for a distinct domain that users may enable alongside an existing one.
2. Describe the supported target, firmware/ELF/SVD requirements, acquisition
   source, units, status mappings, read side effects, unavailable inputs, and
   limits of each finding. Do not infer device support from a filename.
3. Add a `catalog.json` entry with a path relative to `domains/`. Put usable
   integrations in `catalog/`. Put format templates in `authoring-examples/`
   with family `Authoring examples` and kind `Authoring example`; describe what
   the user must supply before use.
   Supply an engineering explanation for every input, derived field and card,
   including any action and its completion semantics, for the website reference.
4. Validate with the native offline parser (no probe access):

   ```sh
   viewalyzer-cli --parse-trace-domain domains/catalog/your-domain.vadomain --pretty
   ```

   For a template, use `domains/authoring-examples/your-example.vadomain`.

5. Report exactly what was checked: parsing, ELF resolution, replay, actual
   hardware and supported operating systems are different kinds of evidence.
   Submit a pull request. Preserve existing notices; do not include firmware
   binaries, local paths, probe serials, credentials or personal capture data.

## Offline consumer copies

Edit this directory first. Required copies remain in ViewAlyzer-RS for standalone
builds, release packaging and compile-time tests, and in demo/HIL projects so
those projects remain usable offline. They are snapshots, not independent sources.
The website generator reads this catalog and copies these exact bytes to its
download directory. It no longer owns a separate U575 descriptor.

With consumer repositories beside this checkout:

```sh
python domains/sync.py                     # report drift; never writes
python domains/sync.py --apply             # refresh known copies
python domains/sync.py --consumer ViewAlyzer-RS --apply
```

`--workspace PATH` selects another parent directory. Missing consumer repositories
are skipped. Review every changed repository. Regenerate the website with
`python tools/guide_pages.py --only trace-domains` from its checkout, then run its
link and navigation checks. Publish the Examples changes before website GitHub
source links that point to them; no command above publishes anything.

Historical regression fixtures intentionally retain their original data and are
not synchronized: the RS tests compare old acquisition against current presentation.
Archived temporary captures, old installed test profiles and the Suite bulk-read
prototype are superseded snapshots, not additional supported domains. They remain
untouched. Live demo copies in STM32N6, STM32WB and the HIL N6/U575 projects are
included in synchronization. No user-installed global registry is modified.

## Definition provenance

The initial collection comes from the ViewAlyzer-RS domain bundle and authoring
examples, STM32N6/STM32WB demos, the STM32WB BLE observation ABI 1 adapter and the
U575 USBX/ThreadX example. Duplicate definitions were consolidated; IMU recording
styling is classified as an authoring example, not a second supported sensor domain. BLE GATT and journal pages remain
separate because they address different slots within bounded input limits.
BLE status/opcode vocabulary came from the demo's pinned STM32CubeWB interface
definitions (CubeWB `e7385e64d05a6c143e60b3890668d45f611f872e`, STM32_WPAN
`1a66948ee871e7665712aa53dd3cbb9a71c4985a`). Retain that provenance when updating it.
