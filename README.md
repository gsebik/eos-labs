# EOS Labs

Student starter code and checks for Embedded and Operating Systems (ES201). The full student-facing lab manuals live in the main course repository under `docs/Embedded and Operating Systems/Labs/`.

The repository contains starter code, instruments, and checks—no reference solutions. The primary station is the EP-0172 + Pico 2 W; PicoBot-specific work is a later extension, except for a network-lab variant where the same stack can be reused.

## Get the source

```sh
git clone https://github.com/gsebik/eos-labs.git
```

```sh
cd eos-labs/lab03-instrument
```

The 50 kS/s instrument samples GP28, fed by the selectable GP22 square-wave
output through a jumper, computes a 256-point FFT and displays frequency bands.
See the lab manual for wiring, configuration, TODO contracts and evidence.

## Pilot status

The current L3 spectrum-instrument project is in
[`lab03-instrument/`](lab03-instrument/). It is supplied as a complete directory:
students work from the checkout rather than copy driver listings from a page.
Its manual, maintained in the course documentation repository, gives the
experiment sequence and the evidence to retain. Code remains authoritative here.

The existing Lab 1 pilot below is retained; the L3 addition does not replace
or overwrite those exercises. Student starter repository: [gsebik/eos-labs](https://github.com/gsebik/eos-labs).
The source is a work in progress; clone a snapshot and record its commit before
collecting evidence. Reference firmware is supplied separately by the lecturer.

Lab 1 is being piloted as short exercise sets. Each exercise follows:

1. predict the behavior or measurement (recorded in the course-docs lab report);
2. complete, fix, or modify a small prepared project;
3. build and run using `picolab`;
4. record the measurement and its conditions in the course-docs logbook;
5. validate and explain the result.

The first checker is deliberately small: firmware emits `@@CHECK` records and `tools/check.py` validates them against per-exercise TOML expectations. No build/flash orchestration or rustlings-style progress tool yet.

## Toolchain

- Pico SDK: set `PICO_SDK_PATH`.
- Raspberry Pi FreeRTOS Kernel fork: set `FREERTOS_KERNEL_PATH`.
- Board: `pico2_w`.
- Build: `cmake -S lab1-kernel/01-task-baseline -B build/01-task-baseline -G Ninja && cmake --build build/01-task-baseline`.

Builds require no probe or hardware. Board measurements require the board and instruments listed in the exercise.

## Repositories

Solutions are maintained separately in a lecturer-only repository. No solutions or answer-key data belong here.
