# RTX5 recorder examples

Source-built CMSIS-RTX 5.9.1 examples for NUCLEO-G474RE (ST-Link, SWO or
ViewAlyzer RAM buffer) and NUCLEO-F767ZI (J-Link, SWO, RAM buffer or RTT).

`build.py` takes the same flags as the FreeRTOS and baremetal examples:

```sh
python rtx5/build.py                                # G474, RAM buffer
python rtx5/build.py --board f767 --transport rtt   # F767 over J-Link RTT
python rtx5/build.py --transport swo --flash --serial <probe-serial>
python rtx5/build.py --clean                        # full rebuild
```

Output goes to `rtx5/build/<os>-<board>-<transport>/`. Tools are found the same
way as the other examples (STM32CubeCLT, PATH, or `tools.local[.os].json`).
The recorder defaults to `../ViewAlyzer/ViewAlyzerRecorder` next to this repo
or `VIEWALYZER_RECORDER_DIR`. The pinned sources below are fetched once into a
per-user cache shared with the ThreadX examples (`--cache` or `deps_cache`
overrides it). Existing checkouts are verified and never reset.
`--flash` uses the onboard ST-Link for G474 and a J-Link for F767.

The pinned source revisions are:

| Input | Revision |
| --- | --- |
| ARM-software/CMSIS-RTX | `b566de42a5bf77d8b080f521cefc38ec7c45a3cc` (5.9.1) |
| ARM-software/CMSIS_6 | `af1b110e46a1720349f6f125e386e126767699bd` (6.4.0) |
| STMicroelectronics/x-cube-azrtos-g4 | `b79cbdcc0e88f6f0db9689f98c99595e7d2769fa` |
| STMicroelectronics/x-cube-azrtos-f7 | `5ce63d50c4ffeb9e6c7e8c5ec567b424cffcd223` |

The ST repositories supply device headers, startup, system initialization and
linker scripts only. The resulting firmware runs RTX, not ThreadX.

`--kernel-dir`, `--cmsis-dir` and `--cube-dir` use your own clean checkouts
instead of the cache. `--build-dir` selects an exact output directory; it must
be under `rtx5/build/` or outside all source repositories.
`--toolchain-bin` selects an explicit compiler directory.
`--recorder-off` builds a control image with the upstream exception port.
Each build writes an
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
