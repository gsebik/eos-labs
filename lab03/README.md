# L3 spectrum instrument

This is the complete student project for the spectrum-instrument lab. Run
`picolab` from this directory; do not assemble the project from manual snippets.
The lab manual explains the investigation. The markers `[0]`–`[6]` in `main.c`
follow the path a block takes through the firmware, which is not their order in
the file.

## Read the source in this order

1. **`main.c`, top to bottom.** The four tasks at the bottom of the file say who
   runs; the functions above them say what each task does. Follow one call:
   `processing_task` → `process_block` → `publish_spectrum`, then
   `display_task` → `draw_spectrum`.
2. **`capture.h` and `capture.c`.** Where a block comes from and how its buffer
   is delivered. Read `dma_done()` once; it is short and it explains `LOST`.
3. **`pool.h`.** The four buffer states. Every ownership question in the lab is
   one of those transitions.
4. **`trace.h`.** The events you will read back with the debugger.
5. **`panel.c`.** Only if a question is about the screen: what is drawn, and why
   the picture is not evidence of timing.

## Files and roles

- `CMakeLists.txt`, `FreeRTOSConfig.h`: target configuration and experiment
  options.
- `main.c`: the tasks, the two locks, the status line, the spectrum export. This
  is the file the lab asks you to change.
- `capture.c/.h`: ADC/DMA acquisition, descriptor delivery, the generated tone
  and its sweep.
- `pool.c/.h`: sample-buffer ownership.
- `workload.c/.h`: FFT and the adjustable processing load.
- `panel.c/.h`: the EP-0172 renderer: bars, frequency scale, counters.
- `trace.c/.h`, `sig_trace.py`: bounded event trace; decode to JSONL or CSV, and
  check a retention window.
- `status_log.py`: turn a captured console log into one CSV row per status group.
- `rta.py`, `model-example.json`: response-time calculator and one input file.
- `analyzer.py`: PicoGremlin marker capture, for the optional marker work.

## Build and run

Prerequisites: Pico SDK, FreeRTOS **V11.3.1** with the RP2350 Arm port,
`picolab`, and Python 3. The intended board is `pico2_w` on the EP-0172 station.

`FREERTOS_KERNEL_PATH` must name the V11.3.1 checkout; `picolab doctor` reports
the revision and `picolab doctor --fix` can fetch it.

```sh
PICOLAB_BUILD_TYPE=Release picolab build lab03_instrument_rtos \
    EOS_DSP_REPEATS=4 EOS_DISPLAY_ROWS=140 EOS_DISPLAY_PERIOD_MS=50 \
    EOS_UI_PERIOD_MS=20 EOS_DEADLINE_US=10240
picolab flash build/lab03_instrument_rtos.elf
picolab monitor
```

Building needs no board and is not hardware verification. `--debug-build` selects
different optimisation; reserve it for source stepping and re-establish the
Release baseline before comparing timings.

`lab03_instrument_foreground` runs the same components in one loop, for the
comparison in Task 3. `lab03_priority_inheritance` is the separate H/M/L
demonstration; `EOS_INHERITANCE_MUTEX=ON` is the default.

## Report images

The lab issues prebuilt firmware for the reported faults. They are release
downloads, not part of this project and not built here. Each sheet records the
SHA-256 and the source commit; check both before programming one.

## Content status

The project builds and has been accepted on the EP-0172 station for the baseline
and both report images. That acceptance does not make this directory a measured
timing reference for your own board, toolchain or kernel build. Private solutions
and answer keys do not belong in this repository.
