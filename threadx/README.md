# ThreadX recorder examples

These projects run the same recorder workload on STM32 Nucleo boards. The
capture acceptance profile uses F767 with J-Link and G474 with ST-Link:

| Project | Clock / port | Capture transports |
| --- | --- | --- |
| `Nucleo_F767ZI` | 216 MHz, Cortex-M7 GNU | ITM/SWO, ViewAlyzer RAM buffer, SEGGER RTT through an external J-Link |
| `Nucleo_G474RE` | 170 MHz, Cortex-M4 GNU | ITM/SWO and ViewAlyzer RAM buffer through the onboard ST-Link |

`Nucleo_U575ZI` is an optional experimental build at 160 MHz, Cortex-M33 GNU,
with `TX_SINGLE_MODE_NON_SECURE=1`. Its SWO and RAM-buffer captures are not part
of the primary acceptance profile.

The build selects either Eclipse ThreadX **6.5.2** or ST's ThreadX **6.4.0**
(`v6.4.0_260327`). It does not substitute an older kernel supplied by a board
example. No TrustZone option bytes, probe firmware, or security configuration
are changed by these projects. U575 supports a flat image for `TZEN=0` and a
paired secure boot / NonSecure ThreadX image for an already provisioned `TZEN=1`
board. Select the mode matching the board before programming it.

Install Python 3.10+, Git, CMake, Ninja and Arm GNU tools. Use a current
ViewAlyzerRecorder checkout containing `threadx/VA_Adapter_ThreadX.c`.
The dependency cache and build directories must be outside this repository.

```sh
python threadx/build.py --board f767 --kernel eclipse --transport rambuf \
  --cache /path/to/dependency-cache --build-dir /path/to/build-f767-eclipse-rambuf \
  --recorder /path/to/ViewAlyzer/ViewAlyzerRecorder
```

Use `--board g474` for the ST-Link board and `--kernel st` for ST's kernel.
Both boards support `--transport swo` and `rambuf`; `rtt` is only available
for the F767 example.
Pass `--toolchain-bin /path/to/arm-gnu/bin` if Arm GNU tools are not on PATH.
Windows PowerShell accepts the same arguments on one line, using native paths.
Build directories should be separate for each board, kernel, transport and host OS.

For the experimental U575 build, select `--board u575`. For an existing
TrustZone layout matching the following settings, add
`--u575-security trustzone_nonsecure`. This builds ST's GTZC secure boot partner
at `0x0C000000` and ThreadX at `0x08100000`, with its RAM at `0x20040000`.
The matching option bytes are `RDP=0xAA`, `TZEN=1`, `SECBOOTADD0=0x180000`,
`SECWM1_PSTRT=0`, `SECWM1_PEND=0x7F`, `SECWM2_PSTRT=1`, and `SECWM2_PEND=0`;
bank swapping and `BOOT_LOCK` must be disabled. Check these before flashing. Programming this
pair does not provision or change option bytes. The default `--u575-security flat`
uses the original donor's `0x08000000` image and requires `TZEN=0`.

`build.py` only builds. It produces `threadx_recorder.elf`, a matching `.hex`,
a linker map and `build-manifest.json`. The manifest records exact source
revisions, input and image hashes, compiler identity and ELF symbols. Existing
dependency checkouts must be clean and match their pins; the helper never resets
or updates an existing checkout to another revision.
The paired U575 build additionally produces `threadx_secure_boot.elf`, its HEX,
and `threadx_paired.hex`. Flash the combined HEX and verify both images;
use `threadx_recorder.elf` for recorder capture symbols. The manifest includes
both image hashes and the secure partner's source provenance. An existing sparse
CubeU5 checkout must include the GTZC donor and the Nucleo BSP dependencies.

For capture, use the matching ELF/image pair, explicitly select the probe serial,
and set the target and clock from the table. ITM uses stimulus port **1** and
a **2 MHz** SWO rate. RAM capture uses `_VA_RAMBUF` from that ELF; RTT uses
`_SEGGER_RTT`, channel **0**. Those addresses are in the build manifest and change
between builds. The CLI's default reset-on-capture behavior starts the workload
after capture setup. A five-second capture covers several complete rounds.
For RAM-buffer and RTT captures, pass the matching image with
`viewalyzer-cli capture ... --elf /path/to/threadx_recorder.elf` so the host can
resolve the firmware's RAM metadata.
The reusable HIL ThreadX runner verifies the manifest and firmware counters,
records through the CLI, and releases its own probe/rack claims.

The workload creates and deletes queues, semaphores, recursive mutexes, event
flags, byte pools, block pools, timers and disposable threads each round. It
exercises successful operations, full/empty and unavailable failures, timed waits,
blocked front/back queue senders, and a higher-priority helper waking from
blocked operations. Timer callbacks use
the explicit `VA_ThreadX_TimerEnter/Exit` hooks. It also covers priority changes,
thread sleep, real and delayed suspend/resume, ready-thread priority changes,
equal-priority relinquish, reset, blocked-thread termination and delete.
A sleeping disposable thread holds an inherited mutex while the helper waits;
terminating that owner verifies the kernel's implicit mutex release and wakeup.
A software-pended EXTI0 interrupt uses one manual ISR hook pair to send to a
separate queue, give a semaphore and set flags; no physical interrupt input is
required. Deleting a semaphore with a blocked helper checks `TX_DELETED` wakeup.
The first round also lowers the blocked helper's priority, deletes and recreates
queue, semaphore and event-flag objects at the same addresses, then lets the
helper return. These failures must retain the deleted object's identity, including
queue receive/front-send/back-send and flag-mask details.
Each round emits `THREADX:coverage:v2`, lifecycle/ISR completion markers,
`THREADX:kernel:<version>` and `THREADX:round-complete`; any unexpected API status
increments `threadx_example_failures` and emits `THREADX:unexpected-status`.

The integration enables the adapter in `common/tx_user.h` for every kernel and
application translation unit. The ST low-level assembly is also preprocessed
with that header so its existing execution-notify ISR calls are enabled.
`VA_Init` runs before `tx_kernel_enter`, including the kernel timer-thread
creation. Do not add another ISR wrapper around the donor SysTick handler.
The recorder replaces ThreadX's native trace insertion; a simultaneous TraceX
buffer or execution-profiling implementation is unsupported. F767 data caching
is disabled so RAM and RTT readers observe firmware writes without cache upkeep.
G474 uses the donor's internal-HSI PLL clock configuration. Its legacy low-level
assembly gates the execution callbacks under the profiling macro name; a shim
enables those calls only in that assembly unit, without enabling kernel profiling.
U575 retains the donor's instruction cache only.
In paired mode, the secure bootstrap reuses ST's GTZC partition and SRAM3 attributes.
The NonSecure ThreadX donor configures the system clocks directly.
The bootstrap routes EXTI0 and the initialization-only TIM6
interrupt to NonSecure state and grants NonSecure use of the PB3 trace pin.
The ThreadX donor initializes ICACHE after the secure bootstrap transfers control.

The board support remains in pinned upstream checkouts, retaining their licenses
and notices without copying vendor trees into this repository:

| Source | Commit |
| --- | --- |
| [ST F7 ThreadX donor](https://github.com/STMicroelectronics/x-cube-azrtos-f7/tree/5ce63d50c4ffeb9e6c7e8c5ec567b424cffcd223/Projects/STM32F767ZI-Nucleo/Applications/ThreadX/Tx_Thread_Creation) | `5ce63d50c4ffeb9e6c7e8c5ec567b424cffcd223` |
| [ST G4 ThreadX donor](https://github.com/STMicroelectronics/x-cube-azrtos-g4/tree/b79cbdcc0e88f6f0db9689f98c99595e7d2769fa/Projects/NUCLEO-G474RE/Applications/ThreadX/Tx_Thread_Creation) | `b79cbdcc0e88f6f0db9689f98c99595e7d2769fa` |
| [ST U5 ThreadX donor](https://github.com/STMicroelectronics/STM32CubeU5/tree/448c03b3ef00759626cb69e68a6c84eba7c7a5dc/Projects/NUCLEO-U575ZI-Q/Applications/ThreadX/Tx_Thread_Creation) | `448c03b3ef00759626cb69e68a6c84eba7c7a5dc` |
| [ST U5 GTZC boot and partition donor](https://github.com/STMicroelectronics/STM32CubeU5/tree/448c03b3ef00759626cb69e68a6c84eba7c7a5dc/Projects/NUCLEO-U575ZI-Q/Examples/GTZC/GTZC_TZSC_MPCBB_TrustZone) | `448c03b3ef00759626cb69e68a6c84eba7c7a5dc` |
| [Eclipse kernel](https://github.com/eclipse-threadx/threadx/tree/93387b0a60382a29579bbad5045c68328e3751df) | `93387b0a60382a29579bbad5045c68328e3751df` |
| [ST kernel](https://github.com/STMicroelectronics/stm32-mw-threadx/tree/b44bdbbbb5a9e4fb56728d2932236564c89db29a) | `b44bdbbbb5a9e4fb56728d2932236564c89db29a` |

U5 CMSIS and HAL submodules use the revisions recorded by its donor commit.
The F767 RTT build reuses the licensed, unmodified SEGGER RTT sources already
in `freertos/Nucleo_F446RE/SEGGER_RTT`.

The upstream F7 and G4 licence tables label their application projects ST proprietary.
The donor low-level ThreadX assembly retains Microsoft's Azure RTOS licence;
these examples target the supported STM32 hardware. The pinned ThreadX kernels
are MIT-licensed, CMSIS is Apache-2.0, and ST HAL sources are BSD-3-Clause.
Consult the upstream files and licence tables for their full terms; the final
firmware links components with these distinct licences. First-party C/header
files copy the Apache-2.0 licence block from this repository's
`Desktop-CPP-UDP/c-udp/viewalyzer_cobs.c`; CMake files also use Apache-2.0.
The Python helper retains `LicenseRef-BKPT-Proprietary`, following the workspace's
explicit Python-header rule and the existing `domains/sync.py` precedent.
