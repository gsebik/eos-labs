#!/usr/bin/env python3
"""Turn a captured status log into one CSV row per complete status group.

The instrument prints a status group about once a second: a `sig rtos=` line, a
field line, a `heap_free=` line and the kernel's CPU shares. A group is one
record -- its maxima all describe the same window -- so this tool keeps groups
together and never mixes fields from two of them.

    picolab monitor | tee session.log
    python3 status_log.py session.log > session.csv      # one row per group
    python3 status_log.py session.log --summary          # the numbers to note

Time between groups is taken from the `blocks` counter, not from the clock: one
block is always 256 samples at `adc_hz`, and the DMA paces it in hardware. The
console output itself takes about a tenth of a second per group, so groups are
about 1.1 s apart, not 1.0 s.

Incomplete groups (interleaved console text, a capture cut mid-group) are
reported on stderr and skipped, because a partial group is not a measurement.
"""
from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path

SAMPLES_PER_BLOCK = 256
FIELD = re.compile(r'(\w+)=(-?\d+)')
CPU = re.compile(r'^(\w+)\s+\d+\s+(<1|\d+)%\s*$')


def groups(text: str):
    """Yield (complete, lines) per status group, in order."""
    current = None
    for line in text.splitlines():
        if line.startswith('sig rtos='):
            if current is not None:
                yield any('heap_free=' in l for l in current), current
            current = []
        if current is not None:
            current.append(line)
    if current is not None:
        yield any('heap_free=' in l for l in current), current


def fields_of(lines):
    values = {}
    for line in lines:
        for name, value in FIELD.findall(line):
            values[name] = int(value)
        cpu = CPU.match(line.strip().replace('\t', ' '))
        if cpu:
            values['cpu_' + cpu.group(1).lower() + '_pct'] = (
                0 if cpu.group(2) == '<1' else int(cpu.group(2)))
    return values


def build(text: str):
    rows, skipped = [], 0
    for complete, lines in groups(text):
        values = fields_of(lines)
        if not complete or 'processed' not in values or 'rtos' not in values:
            skipped += 1
            continue
        rows.append(values)
    return rows, skipped


def add_derived(rows):
    """Add deltas and rates that only make sense between two groups."""
    first = rows[0] if rows else None
    for index, row in enumerate(rows):
        row['group'] = index
        rate = row.get('adc_hz') or (first or {}).get('adc_hz')
        if rate and 'blocks' in row:
            row['t_s'] = round((row['blocks'] - first['blocks']) * SAMPLES_PER_BLOCK / rate, 3)
        else:
            row['t_s'] = float(index)
        if index == 0:
            continue
        previous = rows[index - 1]
        for name in ('processed', 'dropped', 'late', 'overrun'):
            if name in row and name in previous:
                row[name + '_delta'] = row[name] - previous[name]
        elapsed = row['t_s'] - previous['t_s']
        if elapsed > 0:
            for name, column in (('processed', 'blocks_per_s'), ('dropped', 'lost_per_s'),
                                 ('late', 'late_per_s')):
                if name + '_delta' in row:
                    row[column] = round(row[name + '_delta'] / elapsed, 3)
    return rows


def summary(rows):
    """Print the numbers a measurement note needs, from the whole capture."""
    first, last = rows[0], rows[-1]
    span = last['t_s']
    print(f'{len(rows)} complete groups; window {span:.1f} s '
          f'(from the blocks counter, {SAMPLES_PER_BLOCK} samples per block)')
    if span <= 0:
        print('need at least two groups for rates')
        return
    for label, key in (('LOST  (dropped)', 'dropped'), ('LATE', 'late'),
                       ('processed', 'processed')):
        if key in last and key in first:
            print(f'{label:16} {last[key]:>8} since reset   '
                  f'{(last[key] - first[key]) / span:8.2f} per second in the window')
    print('maxima since reset, last group (us):')
    for key in ('compute_max_us', 'buffer_return_max_us', 'publication_response_max_us',
                'publish_wait_max_us', 'settings_wait_max_us', 'draw_max_us'):
        if key in last:
            print(f'  {key:30}{last[key]:>8}')
    if 'records_sent' in last:
        print(f'records sent / acknowledged: {last["records_sent"]} / {last["records_acked"]}')
    shares = {k[4:-4]: v for k, v in last.items() if k.startswith('cpu_')}
    if shares:
        print('CPU share since boot, last group: '
              + ', '.join(f'{k} {v}%' for k, v in shares.items()))


def write_csv(rows, stream):
    if not rows:
        return
    columns = ['group', 't_s']
    for row in rows:
        for key in row:
            if key not in columns and not key.endswith(('_delta', '_per_s')):
                columns.append(key)
    for row in rows:
        for key in row:
            if key not in columns:
                columns.append(key)
    writer = csv.DictWriter(stream, fieldnames=columns, restval='')
    writer.writeheader()
    writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('log', nargs='?', type=Path, default=None,
                        help='captured console text; stdin when omitted')
    parser.add_argument('--summary', action='store_true',
                        help='print the numbers to note instead of the CSV')
    parser.add_argument('--keep-incomplete', action='store_true',
                        help='include groups that never reached heap_free')
    args = parser.parse_args()

    text = args.log.read_text(errors='replace') if args.log else sys.stdin.read()
    rows, skipped = build(text)
    if args.keep_incomplete and skipped:
        # A partial group has no reliable maxima, so it is only useful for the
        # counters that did arrive. Mark the row so it cannot be mistaken.
        for complete, lines in groups(text):
            if not complete:
                values = fields_of(lines)
                if values:
                    values['incomplete'] = 1
                    rows.append(values)
    rows = add_derived(rows)
    if args.summary and rows:
        summary(rows)
    elif not args.summary:
        write_csv(rows, sys.stdout)
    if skipped:
        print(f'status_log: skipped {skipped} incomplete group(s)', file=sys.stderr)
    if not rows:
        print('status_log: no complete status group found', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
