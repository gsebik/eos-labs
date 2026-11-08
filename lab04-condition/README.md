# L4 — Multicore RTOS design

Student project for the multicore lab: a condition monitor that samples a tone,
computes a spectrum, draws it on the EP-0172 panel and runs a fast alarm path.
The instrument is supplied; the lab work is the partition, the placement
comparison and the cross-core handoff.

The manual is the course guide, *L4 — Multicore RTOS design: placing work on two
cores*. This file only covers building the project.

## Build

```sh
export PICOLAB_BUILD_TYPE=Release
picolab flash EOS_CORES=1 EOS_PLACEMENT=0 EOS_SCHED_MODE=0 \
    EOS_FFT_SIZE=256 EOS_DSP_REPEATS=4 EOS_FAST_RING=0
```

Every non-default option is a cached CMake variable. Name every option you need
in the command; `picolab` then configures `build/` from scratch for that run, so
a previous task cannot leak into the next measurement.

## Targets

| Target | Purpose |
|---|---|
| `lab04_condition` | the instrument, and the A/B/C scheduler preset with `EOS_SCHED_MODE=1` |
| `lab04_sched` | separate scheduler experiment; not used by the manual |

## Options

| Option | Default | Meaning |
|---|---|---|
| `EOS_CORES` | 1 | 1 = single core, 2 = SMP |
| `EOS_PLACEMENT` | 0 | 0 = no masks, 1 = SMP global, 2 = SMP partitioned |
| `EOS_FFT_SIZE` | 256 | FFT block length: 256, 512 or 1024 samples |
| `EOS_DSP_REPEATS` | 4 | extra FFT arithmetic passes |
| `EOS_DSP_RAM` | 0 | 1 = place the DSP function in SRAM |
| `EOS_FAST_RING` | 0 | 1 = SPSC ring instead of the FreeRTOS fast queue |
| `EOS_RING_DELAY_US` | 0 | consumer busy-wait, for the full-ring experiment |
| `EOS_SCHED_MODE` | 0 | 1 = build the A/B/C scheduling preset |
| `EOS_SCHED_PLACEMENT` | 0 | 0 = one core, 1 = partitioned, 2 = global |
| `EOS_SCHED_LOOP_PER_MS` | 13600 | CPU-loop calibration at 150 MHz |
| `EOS_ALARM_DEADLINE_US` | 2000 | fast-path deadline |

The ring's four `ORDER_*` macros are deliberately `memory_order_relaxed`; Task 3
asks you to replace them and justify each one.

## Hardware

Pico 2 W on the EP-0172 carrier, SWD and UART0. The PWM-to-ADC jumper must be
fitted. The GT911 touch panel is on I2C0 (GP8/GP9), reset GP10, interrupt GP11.
