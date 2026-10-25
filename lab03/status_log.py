#!/usr/bin/env python3
"""Turn a captured status log into one CSV row per complete status group.

The instrument prints a status group every second: a `sig rtos=` line, a field
line and a `heap_free=` line. A group is one record -- its maxima all describe
the same window -- so this tool keeps groups together and never mixes fields
from two of them.

    picolab monitor | tee session.log
    python3 status_log.py session.log > session.csv

Then, for example:

    import pandas as pd
    df = pd.read_csv("session.csv")
    print(df[["t_s", "processed_delta", "buffer_return_max_us"]])

Incomplete groups (interleaved console text, a capture cut mid-group) are
reported on stderr and skipped, because a partial group is not a measurement.
"""
from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path

# The reporting task runs once a second, so consecutive groups are one second
# apart. That is the nominal cadence used for rates, not a measurement of it.
NOMINAL_PERIOD_S = 1.0
FIELD = re.compile(r'(\w+)=(-?\d+)')


def groups(text: str):
    """Yield (complete, fields) per status group, in order."""
    current = None
    for line in text.splitlines():
        if line.startswith('sig rtos='):
            if current is not None:
                yield False, current
            current = []
        if current is None:
            continue
        current.append(line)
        if 'heap_free=' in line:
            yield True, current
            current = None
    if current is not None:
        yield False, current


def fields_of(lines):
    values = {}
    for line in lines:
        for name, value in FIELD.findall(line):
            values[name] = int(value)
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
    for index, row in enumerate(rows):
        row['group'] = index
        row['t_s'] = round(index * NOMINAL_PERIOD_S, 3)
        if index == 0:
            continue
        previous = rows[index - 1]
        for name in ('processed', 'dropped', 'overrun'):
            if name in row and name in previous:
                row[name + '_delta'] = row[name] - previous[name]
        elapsed = row['t_s'] - previous['t_s']
        if elapsed > 0 and 'processed_delta' in row:
            row['blocks_per_s'] = round(row['processed_delta'] / elapsed, 3)
    return rows


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
    write_csv(rows, sys.stdout)
    if skipped:
        print(f'status_log: skipped {skipped} incomplete group(s)', file=sys.stderr)
    if not rows:
        print('status_log: no complete status group found', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
