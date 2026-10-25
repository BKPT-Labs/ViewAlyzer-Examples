# RTX5 recorder examples

Source-built CMSIS-RTX 5.9.1 examples for NUCLEO-G474RE (ST-Link, SWO or
ViewAlyzer RAM buffer) and NUCLEO-F767ZI (J-Link, SWO, RAM buffer or RTT).
The build script never flashes hardware.

Provide clean source checkouts at these commits:

| Input | Revision |
| --- | --- |
| ARM-software/CMSIS-RTX | `b566de42a5bf77d8b080f521cefc38ec7c45a3cc` (5.9.1) |
| ARM-software/CMSIS_6 | `af1b110e46a1720349f6f125e386e126767699bd` (6.4.0) |
| STMicroelectronics/x-cube-azrtos-g4 | `b79cbdcc0e88f6f0db9689f98c99595e7d2769fa` |
| STMicroelectronics/x-cube-azrtos-f7 | `5ce63d50c4ffeb9e6c7e8c5ec567b424cffcd223` |

The ST repositories supply device headers, startup, system initialization and
linker scripts only. The resulting firmware runs RTX, not ThreadX. Install
CMake, Ninja and Arm GNU Toolchain, then run:

```sh
python rtx5/build.py --board g474 --transport swo \
  --kernel /sources/CMSIS-RTX --cmsis /sources/CMSIS_6 \
  --cube /sources/x-cube-azrtos-g4 \
  --recorder /sources/ViewAlyzer/ViewAlyzerRecorder \
  --build-dir /builds/rtx5-g474-swo
```

Use `--board f767 --cube /sources/x-cube-azrtos-f7` for the F767.
`--transport` accepts `swo`, `rambuf`, and (F767 only) `rtt`.
`--toolchain-bin` selects an explicit compiler directory.
`--recorder-off` builds a control image with the upstream exception port.
Build directories must be outside all source repositories. Each build writes an
ELF, HEX, map, log and manifest containing dependency revisions, tool identity,
source/image hashes and ELF symbols.

The G474 runs at 80 MHz and F767 at 216 MHz. SWO is on PB3. The examples reserve
8 KiB of MSP and use 32 KiB transport buffers. Use the manifest's clock and
buffer symbols when constructing a capture configuration.
Use 4 MHz SWO for the complete hook workload. Lower baud rates can stall the
blocking ITM writer long enough to delay kernel ticks. The controller waits
80 ticks between rounds and the FP workers wait 37/41 ticks, leaving headroom
for probes forwarded over USB networks. These examples validate hook semantics;
their event rate is not a recorder throughput benchmark.

The workload exercises priority inheritance, recursive mutexes, direct queue/
semaphore/flag handoffs, limits and timeouts, pool exhaustion, object deletion,
thread suspend/resume/join/termination, timer callbacks, interrupt-originated
operations and floating-point context preservation. Thread flags exercise
task/IRQ sends, all/any waits, no-clear and clear behavior, polling and timed
failures, and termination of a blocked detached waiter. Firmware counters expose
progress and failures; a successful build alone does not establish capture
correctness. Long captures should cross multiple 32-bit DWT wraps (about 53.7 s
on G474 and 19.9 s on F767).
