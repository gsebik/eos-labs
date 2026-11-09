# L4 — Multicore RTOS design

Student project for the multicore lab: a condition monitor that samples a tone,
computes a spectrum, draws it on the EP-0172 panel and runs a fast alarm path.
The instrument is supplied; the lab work is the partition, the placement
comparison and the cross-core handoff.

The manual is the course guide, *L4 — Multicore RTOS design: placing work on two
cores*. It gives the experiments and evidence requirements. This file maps the
source and lists build options and UART-capture commands.

## Read the source in this order

The project keeps L3's `capture`, `pool`, `workload`, `panel` and `trace` modules.
Start with the application path, not every conditional branch in `main.c`.
The course route uses FreeRTOS on one core or SMP on two cores; it does not run
a separate bare-metal worker on core 1.

1. **`main.c`: the processing and display path.** Follow
   `processing_task` → `process_block` → `publish_spectrum`, then
   `display_task` → `draw_spectrum`. Processing returns the sample buffer before
   publishing its private result. The display copies that result before drawing.
2. **`capture.h`, then `capture.c`: `dma_done()`.** The completion interrupt
   queues a descriptor; the full sample array stays in the pool. The interrupt
   also calls `fast_hook`, which copies a separate short window for the alarm.
3. **`pool.h`.** Read the ownership transitions that govern sample-buffer reuse.
   The alarm's copied record does not extend the original buffer's lifetime.
4. **`placement.c`, then `sched_mode.c` and `sched_release.c` for Task 1.** Edit the small `partition[]`
   mask table after analysing your alternative. Read `tasks[]`, `sched_task()` and
   `sched_mode_main()` to see how the masks become `xTaskCreateAffinitySet` calls.
   This is the small A/B/C experiment selected by
   `EOS_SCHED_MODE=1`, not a model using the instrument's measured task costs.
   The board also runs reporting and, with two cores, heartbeat services. Their
   overhead is outside the nominal three-task calculation.
5. **Task creation in `main.c`, then `workload.c` for Task 2.** Find the affinity
   masks and `sig_dsp_process()`. Keep workload settings identical when comparing
   instruction placement. L4 defaults to 40 display rows and a 100 ms sleep
   after drawing, unlike L3's 140 rows and 50 ms sleep.
6. **`main.c`: the fast path for Task 3.** Follow `fast_hook` → queue or
   `fast_ring_push` → `alarm_task` / `fast_ring_pop`. The alarm does not wait
   for the FFT. The four `ORDER_*` macros are the student edit for this task.
7. **`main.c`: `comm_task`, then `pc_station.py` for Task 4.** Locate the board
   command bracket before interpreting the PC's round-trip measurements.

No source edits are needed for the starting run. Task 1 changes `placement.c`;
Task 3 changes the four `ORDER_*` macros in `main.c`. Leave the supplied
drivers, recorder, inactive foreground branches and separate `sched.c` demo unchanged.

| Reference file | Read when you need to know |
|---|---|
| `trace.h` | which events mark readiness, processing, release and publication |
| `panel.c` | what the display draws, rather than whether deadlines were met |
| `FreeRTOSConfig.h` | how core count, affinity and scheduling are configured |
| `CMakeLists.txt` | which options select the workload and experiment |
| `sched_trace.c`, `sched_trace.h` | how the optional recorder captures switch/job events without UART printing in its window |
| `sched_release.c`, `sched_release.h` | how the scheduler preset uses a kernel trace macro to record due software releases before ready tasks can run |
| `analyze_run.py` | how source timestamps become intervals and plots; no summary maximum becomes CPU demand |

## Build

```sh
export PICOLAB_BUILD_TYPE=Release
picolab flash EOS_CORES=1 EOS_PLACEMENT=0 EOS_SCHED_MODE=0 \
    EOS_FFT_SIZE=256 EOS_DSP_REPEATS=4 EOS_FAST_RING=0 && \
    picolab monitor | tee l4-baseline.log
```

Every non-default option is a cached CMake variable. Name every option you need
in the flash command; `picolab flash` then configures `build/` from scratch for
that run, so a previous task cannot leak into the next measurement. A separate
`picolab build` can reuse the cache; do not replace the documented measurement
commands with a build followed by `flash --no-build`.

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
| `EOS_SCHED_TRACE` | 0 | 1 = record 150 ms of early scheduler execution, then dump after recording ends |
| `EOS_SEED_SMP_BUG` | 0 | optional unsafe shared-counter demo; leave disabled for this lab's measured comparisons |
| `EOS_ALARM_DEADLINE_US` | 2000 | fast-path deadline |

The ring's four `ORDER_*` macros are deliberately `memory_order_relaxed`; Task 3
asks you to replace them and justify each one.

## Decode a run and plot measured execution

```sh
python3 analyze_run.py l4-baseline.log --out baseline-analysis
```

Open `baseline-analysis/index.html`. Use a new output directory for each run.
Instrument logs yield source CSV, throughput from `Δprocessed/Δt_ms`, and a plot
of the reported elapsed DSP maximum. These maxima accumulate since boot; they
are not a series of independent job execution times.

Normal scheduler logs yield per-report CSV and summary maxima, **not** an
invented Gantt chart. For measured execution lanes, make a separate build with
`EOS_SCHED_MODE=1 EOS_SCHED_TRACE=1` and retain output through
`sched_trace_end`. The script rejects missing events and recorder overflow.
Trace-build UART dumping disturbs later report windows, so use trace=0 captures
for summary comparisons and trace=1 only for its bounded RAM-recorded interval.

Trace analysis writes `timeline.svg`, `job.svg`, `segments.csv`, `jobs.csv` and
`analysis.json`. All event timestamps use the shared board timer and carry core
and task identity. Running segments give **scheduled residency**, including IRQ
time charged to an interrupted task. Unattributed time is not assumed idle.
Neither this residency nor `max_exec_us` establishes WCET or a CPU-demand bound.

The review source archive includes `SOURCE-REVISION.txt`; it records base
revision and file hashes, including the local PREV/NEXT edit. Public revision
9400c2a does not include this candidate's placement/analysis/trace additions.

## PC capture

Close any UART monitor before running the PC tool. Use the probe's stable
`-if01` port, not the analyzer's `-if03` interface:

```sh
python3 pc_station.py --port /dev/serial/by-id/your-probe-if01 \
    --mode query --count 5 --gap-ms 300 --status \
    --raw-log l4-query.jsonl --json l4-query.json
```

`--status` retains the periodic board command counters and waits for a report
after the replies before sending again. The JSON result attaches these status
snapshots to the last reply of each request. The JSONL trace records all received
bytes and transmitted requests with host timestamps, including startup and gaps.

`cmd_n` and `cmd_max_us` reset every report. They describe a report window, not
a reply identifier; a ten-reply burst is one command. Do not treat an aggregate
maximum as a matched per-command measurement. See Task 4 for the timing boundaries.

If a reply or command-status report is unavailable, the tool exits nonzero and
keeps the captured evidence. It stops sending after a timeout, so a late reply
cannot be mistaken for a new request. Use distinct capture filenames per run.
`--response-timeout` and `--status-timeout` set the respective waits in seconds.

## Hardware

Pico 2 W on the EP-0172 carrier, SWD and UART0. The PWM-to-ADC jumper must be
fitted. The GT911 touch panel is on I2C0 (GP8/GP9), reset GP10, interrupt GP11.
