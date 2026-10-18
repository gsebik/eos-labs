#!/usr/bin/env python3
"""Discrete-time scheduling explorer for the L3 instrument (Stage 5).

A small model, not the firmware and not the board. One core, fixed-priority
pre-emptive scheduling, three tasks and two locks, in discrete microseconds:

  * DSP     (priority 3) reads the settings, computes, publishes the result;
  * display (priority 2) redraws over SPI;
  * UI      (priority 1) writes the settings.

A mutex gives priority inheritance; a binary semaphore does not. The result
lock is always a mutex. It sweeps the release phase of the display and UI jobs
relative to one DSP block release and reports the worst ready-to-release
response and the number of misses. It is exhaustive over the sampled phases,
within this model only: no caches, bus contention, real interrupt variation,
kernel overhead or instruction timing. Every duration is an assumed input.
"""
from dataclasses import dataclass
import argparse
import json

PRIORITY = {'dsp': 3, 'display': 2, 'ui': 1}
# Calibrated on the EP-0172 station (Pico 2 W + PicoGremlin), 2026-10:
# SPI0 was granted 15 MHz (not the 16 MHz requested); the settings spin loop
# costs about ten CPU cycles per iteration; one DSP block takes about 1.4 ms.
SPI_BITS_PER_ROW = 640 * 8
SPI_HZ = 15_000_000
SETTINGS_CYCLES_PER_ITER = 10
CPU_HZ = 150_000_000


@dataclass(frozen=True)
class Model:
    deadline_us: int = 10240
    block_period_us: int = 5120
    compute_us: int = 1400
    settings_read_us: int = 20
    publish_us: int = 20
    copy_us: int = 20
    redraw_us: int = 13653
    settings_write_us: int = 3333
    long_result_lock: bool = True
    settings_mutex: bool = True


def from_options(display_rows=40, settings_iters=50000, **overrides):
    """Derive display and settings durations from the firmware options.

    The constants are bench-calibrated, not guesses: SPI0 granted 15 MHz on the
    station, and the settings spin loop costs about ten CPU cycles per iteration
    at 150 MHz. Re-measure on a different board before trusting them.
    """
    redraw = SPI_BITS_PER_ROW * display_rows * 1_000_000 // SPI_HZ
    write = settings_iters * SETTINGS_CYCLES_PER_ITER * 1_000_000 // CPU_HZ
    return Model(redraw_us=redraw, settings_write_us=write, **overrides)


def _inherits(model, lock):
    return lock == 'result' or model.settings_mutex


def jobs(model):
    dsp = [('acquire', 'settings'), ('work', model.settings_read_us),
           ('release', 'settings'), ('work', model.compute_us),
           ('acquire', 'result'), ('work', model.publish_us),
           ('release', 'result')]
    if model.long_result_lock:
        display = [('acquire', 'result'), ('work', model.redraw_us),
                   ('release', 'result')]
    else:
        display = [('acquire', 'result'), ('work', model.copy_us),
                   ('release', 'result'), ('work', model.redraw_us)]
    ui = [('acquire', 'settings'), ('work', model.settings_write_us),
          ('release', 'settings')]
    return {'dsp': dsp, 'display': display, 'ui': ui}


def _job(steps):
    return [[kind, value, value if kind == 'work' else 0] for kind, value in steps]


def simulate(model, display_phase=None, ui_phase=None, step_us=100):
    plan = jobs(model)
    release_at = {'dsp': 0, 'display': display_phase, 'ui': ui_phase}
    known = [value for value in release_at.values() if value is not None]
    start = min([0] + known)
    tasks = {name: dict(prio=PRIORITY[name], job=None, released=None,
                        blocked=False, block_lock=None) for name in plan}
    locks = {'settings': None, 'result': None}
    t = start
    horizon = (start + model.redraw_us + model.settings_write_us
               + model.deadline_us + model.compute_us + 50 * step_us)
    response = None
    settings_wait = publish_wait = 0

    while t <= horizon:
        for name, task in tasks.items():
            when = release_at[name]
            if task['job'] is None and when is not None and t >= when:
                task['job'] = _job(plan[name])
                task['released'] = when
                release_at[name] = None

        for name, task in tasks.items():
            task['blocked'] = False
            task['block_lock'] = None
            if task['job'] and task['job'][0][0] == 'acquire':
                lock = task['job'][0][1]
                owner = locks[lock]
                if owner is not None and owner != name:
                    task['blocked'] = True
                    task['block_lock'] = lock

        effective = {name: task['prio'] for name, task in tasks.items()}
        changed = True
        while changed:
            changed = False
            for name, task in tasks.items():
                if not task['blocked']:
                    continue
                lock = task['block_lock']
                owner = locks[lock]
                if owner is None or not _inherits(model, lock):
                    continue
                if effective[owner] < effective[name]:
                    effective[owner] = effective[name]
                    changed = True

        advance = True
        runnable = [n for n, task in tasks.items()
                    if task['job'] and not task['blocked']]
        if runnable:
            name = max(runnable,
                       key=lambda n: (effective[n], tasks[n]['prio'], n == 'dsp'))
            task = tasks[name]
            kind, value, remaining = task['job'][0]
            finished_at = None
            if kind == 'acquire':
                locks[value] = name
                task['job'].pop(0)
                finished_at = t
                advance = False
            elif kind == 'release':
                locks[value] = None
                task['job'].pop(0)
                finished_at = t
                advance = False
            else:
                remaining -= step_us
                if remaining <= 0:
                    task['job'].pop(0)
                    finished_at = t + step_us
                else:
                    task['job'][0][2] = remaining
            if not task['job'] and finished_at is not None:
                if name == 'dsp':
                    response = finished_at - task['released']
                task['job'] = None

        dsp = tasks['dsp']
        if dsp['blocked']:
            if dsp['block_lock'] == 'settings':
                settings_wait += step_us
            elif dsp['block_lock'] == 'result':
                publish_wait += step_us

        if response is not None:
            break
        if advance:
            t += step_us

    return dict(response_us=response, settings_wait_us=settings_wait,
                publish_wait_us=publish_wait)


def worst_case(model, grid_us=500, step_us=100):
    dphases = list(range(-model.redraw_us, model.deadline_us + 1, grid_us))
    uphases = list(range(-model.settings_write_us, model.deadline_us + 1, grid_us))
    summary = dict(deadline_us=model.deadline_us, response_us=0, misses=0,
                   samples=0, settings_wait_us=0, publish_wait_us=0,
                   worst_phases=None)
    for dp in dphases:
        for up in uphases:
            result = simulate(model, dp, up, step_us)
            if result['response_us'] is None:
                continue
            summary['samples'] += 1
            if result['response_us'] > summary['response_us']:
                summary['response_us'] = result['response_us']
                summary['worst_phases'] = [dp, up]
            if result['response_us'] > model.deadline_us:
                summary['misses'] += 1
            summary['settings_wait_us'] = max(summary['settings_wait_us'],
                                              result['settings_wait_us'])
            summary['publish_wait_us'] = max(summary['publish_wait_us'],
                                             result['publish_wait_us'])
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--long-lock', choices=['on', 'off'], default='on')
    parser.add_argument('--settings-mutex', choices=['on', 'off'], default='on')
    parser.add_argument('--display-rows', type=int, default=40)
    parser.add_argument('--settings-iters', type=int, default=50000)
    parser.add_argument('--compute-us', type=int, default=1000)
    parser.add_argument('--deadline-us', type=int, default=10240)
    parser.add_argument('--grid-us', type=int, default=250)
    parser.add_argument('--step-us', type=int, default=50)
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    model = from_options(args.display_rows, args.settings_iters,
                         compute_us=args.compute_us, deadline_us=args.deadline_us,
                         long_result_lock=args.long_lock == 'on',
                         settings_mutex=args.settings_mutex == 'on')
    summary = worst_case(model, args.grid_us, args.step_us)
    if args.json:
        print(json.dumps(summary))
    else:
        print(f"worst ready-to-release response: {summary['response_us']} us "
              f"against a {model.deadline_us} us deadline")
        print(f"release phases that miss: {summary['misses']} of {summary['samples']}")
        print(f"worst settings wait: {summary['settings_wait_us']} us; "
              f"worst publish wait: {summary['publish_wait_us']} us")
        print("Model only: exhaustive over the sampled release phases within "
              "this model, not over the board.")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
