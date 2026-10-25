# L3 spectrum instrument

This is the complete student project for the spectrum-instrument lab. Run
`picolab` from this directory; do not assemble the project from manual snippets.
The lab manual explains the investigation and refers to the numbered markers
`[0]`–`[6]` in `main.c`.

## Files and roles

- `CMakeLists.txt`, `FreeRTOSConfig.h`: target configuration and experiment
  options.
- `main.c`: acquisition handoff, processing, publication, display, settings and
  status reporting. This is the file the lab asks you to change.
- `capture.c/.h`: ADC/DMA acquisition, descriptor delivery and the generated test
  tone.
- `pool.c/.h`: sample-buffer ownership states.
- `workload.c/.h`: supplied frequency calculation and its adjustable extra
  arithmetic.
- `panel.c/.h`: supplied EP-0172 display renderer.
- `trace.c/.h`, `sig_trace.py`: bounded on-target event trace and its host
  decoder/checker. `decode --csv` writes one row per event.
- `status_log.py`: turns a captured console log into one CSV row per status
  group, with the deltas and rates already computed.
- `rta.py`, `model-example.json`: restricted response-time calculator and one
  input file to copy and edit.
- `analyzer.py`: developer work on the PicoGremlin analyzer capture path; not
  required for the lab tasks.

These files are the editable source. The course manual quotes short excerpts;
keep them consistent with this project rather than maintaining a second copy of
the source in the documentation repository.

## Build and run

Prerequisites: Pico SDK, FreeRTOS **V11.3.1** with the RP2350 Arm port,
`picolab`, and Python 3. The intended board is `pico2_w` on the EP-0172 station.

`FREERTOS_KERNEL_PATH` must name the V11.3.1 kernel checkout. It is read from
the environment when CMake configures; configuration stops with an error if the
path is unset or does not contain the RP2350 port.

Build the normal configuration with Release optimisation, matching the issued
timing images, and keep every option explicit:

```sh
PICOLAB_BUILD_TYPE=Release picolab build lab03_instrument_rtos \
    EOS_DSP_REPEATS=4 EOS_DISPLAY_ROWS=40 EOS_DISPLAY_PERIOD_MS=100 \
    EOS_UI_PERIOD_MS=20 EOS_DEADLINE_US=10240
picolab flash build/lab03_instrument_rtos.elf
picolab monitor
```

Building needs no board and is not hardware verification. `--debug-build` selects
different optimisation; reserve it for separate source-stepping work and
re-establish the Release baseline before comparing timings. Do not substitute a
target USB console: the course station uses UART0 through PicoGremlin.

The option names above are the whole experiment interface. A command that names
any other `EOS_` setting belongs to an earlier revision of this project and does
not apply to this source.

## Targets

`lab03_instrument_rtos` runs the instrument as FreeRTOS tasks.
`lab03_instrument_foreground` runs the same acquisition, processing and display
components in one foreground loop, for the comparison the manual introduces
after the RTOS measurements.

`lab03_priority_inheritance` is a separate demonstration program for the
priority-inheritance task. It shares no acquisition, FFT or display code.
`EOS_INHERITANCE_MUTEX=ON` (the default) uses an inheritance mutex;
`EOS_INHERITANCE_MUTEX=OFF` uses a binary semaphore.

## Report images

The lab issues prebuilt firmware images for the reported faults. They are release
downloads, not part of this project and not built from this directory. Each image
is recorded by its SHA-256 in the sheet that accompanies it; check the hash before
programming it. The report images retain symbols and debug information, so the
manual's debugger and trace-dump procedure works with them.

## Content status

The project builds and has been accepted on the EP-0172 station. That acceptance
covers the baseline and the issued report images; it does not make this directory
a measured timing reference for your own board, toolchain or kernel build.
Private solutions and answer keys do not belong in this repository.
