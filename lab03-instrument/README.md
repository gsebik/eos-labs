# L3 spectrum instrument

This is the complete student project for the spectrum-instrument lab. Run
`picolab` from this directory; do not assemble the project from manual snippets.
The manual explains the investigation and refers to numbered markers in `main.c`.

## Files and roles

- `CMakeLists.txt`, `FreeRTOSConfig.h`: target configuration.
- `main.c`: processing, display, settings and reporting tasks; experiment choices.
- `capture.c/.h`, `pool.c/.h`: ADC/DMA acquisition and three-buffer ownership.
- `workload.c/.h`: supplied frequency calculation and adjustable extra demand.
- `panel.c/.h`: supplied EP-0172 display renderer.
- `trace.c/.h`, `sig_trace.py`: bounded target trace and host monitor.
- `rta.py`, `model-example.json`: restricted response calculator and illustrative inputs.
- `explorer.py`: one-job, discrete-time release-phase model.
- `analyzer.py`: developer capture work, not a validated student requirement.

These files are the editable source. The course manual contains only focused
excerpts; keep them synchronized with this project rather than maintaining
another complete source tree in the documentation repository.

## Reference, faulty comparison and student repair

Prerequisites: Pico SDK, FreeRTOS **V11.3.1** with the RP2350 Arm port,
`picolab`, and Python 3. The intended board is `pico2_w` on the EP-0172 station.
Run the separately supplied `reference/l3-reference.elf` first, using
`picolab flash reference/l3-reference.elf`. Its manifest must identify the
matching build and acceptance observations. The reference ELF is an observable
behavioural reference, not editable solution source; it is not built by this
project. A candidate with no hardware acceptance record is not a known-good
reference.

Then build this student's path with all settings explicit:

```sh
picolab build lab03_instrument_rtos --debug-build \
    EOS_LONG_RESULT_LOCK=OFF EOS_SIG_SETTINGS_MUTEX=ON EOS_SETTINGS_ITERS=2000 \
    EOS_DSP_REPEATS=4 EOS_DISPLAY_ROWS=40 EOS_DISPLAY_PERIOD_MS=100 \
    EOS_UI_PERIOD_MS=20 EOS_DEADLINE_US=10240
```

Building needs no board and is not hardware verification. Follow the manual's
version/configuration check before flashing. Do not substitute a target-USB
console: the course station uses UART0 through PicoGremlin.

`EOS_LONG_RESULT_LOCK=ON` selects the frozen display fault for repeatable
comparisons. `OFF` selects **TODO (1)** in `main.c`, initially with the same
fault. Merely switching OFF is not a repair. At `[5]`, preserve consistent
display data and the renderer while removing unnecessary publication delay.
Task 3's **TODO (2)** concerns the work a settings reader must wait for in
`write_settings()`. Retain that workload while redesigning its protected scope.
The manual supplies the measurement and acceptance contracts for both TODOs.

The panel draws bottom-anchored, height-encoded spectrum bars. It erases the
unused part of each band on every draw, writing the same number of pixels at
each signal level. Plot height is a workload control, not a cosmetic adjustment.

The second target, `lab03_instrument_foreground`, uses the same components in a
foreground loop. The manual introduces it after the RTOS comparisons. Host
model outputs are not measurements of either target.

## Content status

The lab remains under review. Physical acceptance, pinned-kernel timing and
analyzer validation are separate from compilation and host tests. Private
solutions and answer keys do not belong in this repository.
