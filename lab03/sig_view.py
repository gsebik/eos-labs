#!/usr/bin/env python3
"""Read a decoded trace as numbers and as pictures.

    python3 sig_view.py intervals trace.jsonl            # per-block intervals
    python3 sig_view.py intervals trace.jsonl --csv      # every block, as CSV
    python3 sig_view.py intervals trace.jsonl --svg intervals.svg
    python3 sig_view.py locks trace.jsonl                # who held the spectrum lock
    python3 sig_view.py timeline trace.jsonl --svg timeline.svg --job 30   # or the default
    python3 sig_view.py inheritance reportA.log --svg schedule.svg

`intervals` turns the five events of each block into the three numbers the lab
uses: computation, buffer return and publication response. `locks` lists the
spectrum-lock holds and waits the recorder saw. `timeline` draws a few blocks on
a real time axis. `inheritance` draws the H/M/L report as lanes. The pictures
are plain SVG: open them in a browser. None of this decides anything for you;
it draws what was recorded. The trace holds only the first ~2 s after reset, and
only the events the firmware records.
"""
import argparse
import csv
import json
import re
import statistics
import sys
from pathlib import Path

BLOCK_PERIOD_US = 5120
DEADLINE_US = 10240
TASKS = {0: 'idle', 1: 'processing', 2: 'display', 3: 'settings',
         4: 'reporting', 5: 'controller', 6: 'record'}


def load(path):
    lines = [json.loads(line) for line in Path(path).read_text().splitlines()
             if line.strip()]
    if not lines or lines[0].get('type') != 'eos-instrument-trace':
        raise ValueError(f'{path}: not a decoded trace (use sig_trace.py decode)')
    return lines[0], lines[1:]


def blocks(events):
    """One dict per block that has every event up to publication."""
    rows = {}
    keys = {'work_start': 'start', 'work_complete': 'done',
            'block_release': 'release', 'result_published': 'published'}
    for e in events:
        if e['ev'] == 'block_ready':
            rows[e['job']] = {'ready': e['t_us']}
        elif e['ev'] in keys and e['job'] in rows:
            rows[e['job']][keys[e['ev']]] = e['t_us']
    out = []
    for job, r in sorted(rows.items()):
        if all(k in r for k in ('ready', 'start', 'done', 'release', 'published')):
            out.append(dict(job=job, **r,
                            compute=r['done'] - r['start'],
                            buffer_return=r['release'] - r['ready'],
                            publication=r['published'] - r['ready']))
    return out


def lock_spans(events):
    """Pair each spectrum_take with the next spectrum_give of the same task."""
    spans, open_take = [], {}
    for e in events:
        if e['ev'] == 'spectrum_take':
            open_take[e['job']] = e
        elif e['ev'] == 'spectrum_give' and e['job'] in open_take:
            take = open_take.pop(e['job'])
            spans.append(dict(task=e['job'], take=take['t_us'], give=e['t_us'],
                              waited=take['arg'], held=e['arg']))
    unfinished = [dict(task=t, take=e['t_us'], waited=e['arg'])
                  for t, e in open_take.items()]
    return spans, unfinished


def name(task):
    return f"{TASKS.get(task, '?')} ({task})"


def cmd_intervals(args):
    header, events = load(args.trace)
    rows = blocks(events)
    if not rows:
        raise ValueError('no complete block in this trace')
    if args.csv:
        w = csv.writer(sys.stdout)
        w.writerow(['job', 'compute_us', 'buffer_return_us', 'publication_us'])
        for r in rows:
            w.writerow([r['job'], r['compute'], r['buffer_return'], r['publication']])
    else:
        print(f'{len(rows)} complete blocks, jobs {rows[0]["job"]}..{rows[-1]["job"]}')
        print(f'{"":22}{"median":>8}{"max":>8}')
        for label, key in (('computation (us)', 'compute'),
                           ('buffer return (us)', 'buffer_return'),
                           ('publication (us)', 'publication')):
            values = [r[key] for r in rows]
            print(f'{label:22}{int(statistics.median(values)):>8}{max(values):>8}')
        late = [r['job'] for r in rows if r['publication'] > BLOCK_PERIOD_US]
        miss = [r['job'] for r in rows if r['buffer_return'] > DEADLINE_US]
        print(f'publication over {BLOCK_PERIOD_US} us: {len(late)} blocks'
              + (f', jobs {late[:12]}' + (' ...' if len(late) > 12 else '') if late else ''))
        print(f'buffer return over {DEADLINE_US} us: {len(miss)} blocks'
              + (f', jobs {miss[:12]}' if miss else ''))
        longest = sorted(rows, key=lambda r: -r['compute'])[:5]
        print('longest computation spans (job: us): '
              + ', '.join(f'{r["job"]}: {r["compute"]}' for r in longest))
    if args.svg:
        Path(args.svg).write_text(intervals_svg(rows))
        print(f'wrote {args.svg}', file=sys.stderr)


def cmd_locks(args):
    header, events = load(args.trace)
    spans, unfinished = lock_spans(events)
    shown = [s for s in spans if s['held'] >= args.min_us or s['waited'] >= args.min_us]
    print(f'{len(spans)} lock holds recorded; showing those with a hold or a wait '
          f'of {args.min_us} us or more')
    print(f'{"take at (us)":>14}  {"task":<16}{"waited (us)":>12}{"held (us)":>11}')
    for s in shown:
        print(f'{s["take"]:>14}  {name(s["task"]):<16}{s["waited"]:>12}{s["held"]:>11}')
    if not shown:
        print('  (none)')
    for u in unfinished:
        print(f'no give recorded for {name(u["task"])} after the take at {u["take"]} us '
              '(the trace may have closed first)')
    held = {}
    for s in spans:
        held[s['task']] = max(held.get(s['task'], 0), s['held'])
    print('longest hold per task (us): '
          + ', '.join(f'{name(t)}: {v}' for t, v in sorted(held.items())))


# ----------------------------------------------------------------- SVG output
W, H = 960, 420
COL = dict(compute='#2b6cb0', buffer_return='#dd8a1d', publication='#2f855a',
           grid='#d0d4da', text='#1f2933')


def svg_open(width, height):
    return [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
            f'width="{width}" height="{height}" font-family="sans-serif" font-size="12">',
            f'<rect width="{width}" height="{height}" fill="white"/>']


def text(x, y, s, anchor='start', fill=None, size=None, weight=None):
    extra = (f' fill="{fill}"' if fill else f' fill="{COL["text"]}"')
    extra += f' font-size="{size}"' if size else ''
    extra += f' font-weight="{weight}"' if weight else ''
    s = s.replace('&', '&amp;').replace('<', '&lt;')
    return f'<text x="{x:.1f}" y="{y:.1f}" text-anchor="{anchor}"{extra}>{s}</text>'


def intervals_svg(rows):
    left, right, top, bottom = 70, 20, 40, 50
    ymax = max(max(r['publication'], r['buffer_return'], r['compute']) for r in rows)
    ymax = max(ymax * 1.08, DEADLINE_US * 1.05 if ymax > BLOCK_PERIOD_US else BLOCK_PERIOD_US * 1.2)
    x0, x1 = rows[0]['job'], rows[-1]['job']
    px = lambda j: left + (j - x0) / max(1, x1 - x0) * (W - left - right)
    py = lambda v: H - bottom - v / ymax * (H - top - bottom)
    out = svg_open(W, H)
    out.append(text(left, 22, 'Three intervals of each block (from the recorded trace)',
                    weight='bold', size=14))
    step = 1000 if ymax < 12000 else 2000 if ymax < 24000 else 5000
    v = 0
    while v <= ymax:
        out.append(f'<line x1="{left}" x2="{W-right}" y1="{py(v):.1f}" y2="{py(v):.1f}" '
                   f'stroke="{COL["grid"]}"/>')
        out.append(text(left - 6, py(v) + 4, f'{v/1000:g} ms', 'end'))
        v += step
    for limit, label in ((BLOCK_PERIOD_US, '5.12 ms: one block period (LATE above)'),
                         (DEADLINE_US, '10.24 ms: buffer-return limit')):
        if limit < ymax:
            out.append(f'<line x1="{left}" x2="{W-right}" y1="{py(limit):.1f}" '
                       f'y2="{py(limit):.1f}" stroke="#c53030" stroke-dasharray="6 4"/>')
            out.append(text(left + 6, py(limit) - 4, label, 'start', '#c53030'))
    for key, label in (('buffer_return', 'buffer return'),
                       ('publication', 'publication response'),
                       ('compute', 'computation')):
        for r in rows:
            x, y = px(r['job']), py(r[key])
            if key == 'compute':
                out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="2.6" fill="{COL[key]}"/>')
            elif key == 'buffer_return':
                out.append(f'<rect x="{x-3.5:.1f}" y="{y-3.5:.1f}" width="7" height="7" '
                           f'fill="none" stroke="{COL[key]}" stroke-width="1.4"/>')
            else:
                out.append(f'<path d="M{x:.1f} {y-4.5:.1f}L{x+4.5:.1f} {y:.1f}L{x:.1f} {y+4.5:.1f}'
                           f'L{x-4.5:.1f} {y:.1f}Z" fill="none" stroke="{COL[key]}" stroke-width="1.4"/>')
    for i, (key, label) in enumerate((('compute', 'computation'),
                                      ('buffer_return', 'buffer return'),
                                      ('publication', 'publication response'))):
        x = left + 10 + i * 190
        out.append(f'<circle cx="{x}" cy="{H-14}" r="5" fill="{COL[key]}"/>')
        out.append(text(x + 10, H - 10, label))
    out.append(text((left + W - right) / 2, H - 30, f'block (job number {x0} to {x1})', 'middle'))
    out.append('</svg>')
    return '\n'.join(out)


LANE_COL = {1: '#2b6cb0', 2: '#805ad5', 3: '#718096', 4: '#c05621', 6: '#2c7a7b'}


def timeline_svg(events, job, count):
    rows = {r['job']: r for r in blocks(events)}
    spans, _ = lock_spans(events)
    if job is None:
        job = max(rows.values(), key=lambda r: r['publication'])['job']
    first = rows[job]
    jobs = [rows[j] for j in sorted(rows) if job <= j < job + count]
    t0 = first['ready'] - 500
    t1 = max(r['published'] for r in jobs) + 500
    owners = sorted({s['task'] for s in spans if s['give'] >= t0 and s['take'] <= t1})
    lanes = ['processing job'] + [f'{name(t)} holds spectrum lock' for t in owners]
    left, right, top = 260, 20, 50
    lane_h = 46
    height = top + lane_h * len(lanes) + 90
    px = lambda t: left + (t - t0) / (t1 - t0) * (W - left - right)
    out = svg_open(W, height)
    out.append(text(left, 22, f'Blocks {jobs[0]["job"]} to {jobs[-1]["job"]} on a time axis '
                    '(from the recorded trace)', weight='bold', size=14))
    for i, label in enumerate(lanes):
        y = top + i * lane_h
        out.append(f'<line x1="{left}" x2="{W-right}" y1="{y+lane_h-4}" y2="{y+lane_h-4}" '
                   f'stroke="{COL["grid"]}"/>')
        out.append(text(left - 8, y + 24, label, 'end'))
    for r in jobs:
        y = top
        for a, b, colour, label in (
                (r['ready'], r['start'], '#cbd5e0', 'queued'),
                (r['start'], r['done'], '#2b6cb0', 'work_start to work_complete'),
                (r['done'], r['published'], '#dd8a1d', 'after the computation')):
            if b > a:
                out.append(f'<rect x="{px(a):.1f}" y="{y+8}" width="{max(1.5, px(b)-px(a)):.1f}" '
                           f'height="24" fill="{colour}"><title>{label}: {b-a} us</title></rect>')
        out.append(f'<line x1="{px(r["ready"]):.1f}" x2="{px(r["ready"]):.1f}" y1="{y}" '
                   f'y2="{y+lane_h-4}" stroke="#2d3748"/>')
        out.append(text(px(r['ready']) + 2, y + 6, f'#{r["job"]}', size=10))
        dl = r['ready'] + BLOCK_PERIOD_US
        if t0 <= dl <= t1:
            out.append(f'<line x1="{px(dl):.1f}" x2="{px(dl):.1f}" y1="{y}" y2="{y+lane_h-4}" '
                       'stroke="#c53030" stroke-dasharray="4 3"/>')
    for e in events:
        if (e['ev'] == 'spectrum_take' and e['job'] == 1 and e['arg'] >= 200
                and t0 <= e['t_us'] <= t1):
            a = max(e['t_us'] - e['arg'], t0)
            out.append(f'<rect x="{px(a):.1f}" y="{top+32}" width="{max(1.5, px(e["t_us"])-px(a)):.1f}" '
                       f'height="7" fill="#e53e3e"><title>processing waited {e["arg"]} us '
                       'for the spectrum lock</title></rect>')
    for i, t in enumerate(owners, start=1):
        y = top + i * lane_h
        for s in spans:
            if s['task'] == t and s['give'] >= t0 and s['take'] <= t1:
                out.append(f'<rect x="{px(max(s["take"], t0)):.1f}" y="{y+8}" '
                           f'width="{max(1.5, px(min(s["give"], t1))-px(max(s["take"], t0))):.1f}" '
                           f'height="24" fill="{LANE_COL.get(t, "#4a5568")}">'
                           f'<title>held {s["held"]} us</title></rect>')
    ay = top + lane_h * len(lanes) + 8
    out.append(f'<line x1="{left}" x2="{W-right}" y1="{ay}" y2="{ay}" stroke="#2d3748"/>')
    span = t1 - t0
    step = 1000 if span < 12000 else 2000 if span < 30000 else 5000
    t = first['ready'] + ((t0 - first['ready']) // step + 1) * step
    while t < t1:
        out.append(f'<line x1="{px(t):.1f}" x2="{px(t):.1f}" y1="{ay}" y2="{ay+5}" stroke="#2d3748"/>')
        out.append(text(px(t), ay + 18, f'{(t - first["ready"]) / 1000:g} ms', 'middle'))
        t += step
    out.append(text(left, ay + 38, 'time relative to the first block_ready; '
                    'red dashed line = that block ready + 5.12 ms', size=11))
    leg = [('#cbd5e0', 'queued'), ('#2b6cb0', 'work_start to work_complete'),
           ('#dd8a1d', 'after the computation, until published'),
           ('#e53e3e', 'processing waited for the spectrum lock')]
    for i, (colour, label) in enumerate(leg):
        x, y = left + (i % 2) * 330, ay + 46 + (i // 2) * 18
        out.append(f'<rect x="{x}" y="{y}" width="12" height="12" fill="{colour}"/>')
        out.append(text(x + 18, y + 11, label, size=11))
    out.append('</svg>')
    return '\n'.join(out)


def cmd_timeline(args):
    header, events = load(args.trace)
    if not blocks(events):
        raise ValueError('no complete block in this trace')
    Path(args.svg).write_text(timeline_svg(events, args.job, args.jobs))
    print(f'wrote {args.svg}')


LINE = re.compile(r'^\s*(H|M|L)\s+(\w+)\s+t=\s*(\d+)\s+us\s+priority=(\d+)')


def inheritance_events(text_in):
    rows = []
    for line in text_in.splitlines():
        m = LINE.match(line)
        if m:
            rows.append(dict(task=m.group(1), event=m.group(2), t=int(m.group(3)),
                             priority=int(m.group(4))))
    config = re.search(r'resource=(\S+)', text_in)
    return (config.group(1) if config else '?'), rows


def inheritance_svg(config, rows):
    left, right, top, lane_h = 80, 30, 60, 70
    t1 = max(r['t'] for r in rows) * 1.04 or 1
    px = lambda t: left + t / t1 * (W - left - right)
    out = svg_open(W, top + 3 * lane_h + 70)
    out.append(text(left, 24, f'Recorded events, resource = {config}', weight='bold', size=14))
    out.append(text(left, 42, 'Marks are recorded events; what ran between them is for '
                    'you to work out.', size=11))
    for i, task in enumerate('HML'):
        y = top + i * lane_h
        out.append(f'<line x1="{left}" x2="{W-right}" y1="{y+40}" y2="{y+40}" '
                   f'stroke="{COL["grid"]}"/>')
        out.append(text(left - 10, y + 44, task, 'end', weight='bold', size=14))
        mine = [r for r in rows if r['task'] == task]
        for k, r in enumerate(mine):
            x = px(r['t'])
            out.append(f'<circle cx="{x:.1f}" cy="{y+40}" r="6" fill="#2b6cb0"/>')
            up = y + 24 - (k % 2) * 14
            out.append(text(x, up, f'{r["event"]}', 'middle', size=11))
            out.append(text(x, y + 62, f'{r["t"]} us, prio {r["priority"]}', 'middle', size=10))
    ay = top + 3 * lane_h + 4
    out.append(f'<line x1="{left}" x2="{W-right}" y1="{ay}" y2="{ay}" stroke="#2d3748"/>')
    step = 1000 if t1 < 12000 else 2000
    t = 0
    while t <= t1:
        out.append(text(px(t), ay + 16, f'{t/1000:g} ms', 'middle'))
        t += step
    out.append(text((left + W - right) / 2, ay + 36, 'time since the interval opened', 'middle'))
    out.append('</svg>')
    return '\n'.join(out)


def cmd_inheritance(args):
    config, rows = inheritance_events(Path(args.log).read_text())
    if not rows:
        raise ValueError('no H/M/L event lines found; capture the console while '
                         'the report is printed')
    for r in rows:
        print(f'{r["task"]:<2}{r["event"]:<10}{r["t"]:>7} us  priority {r["priority"]}')
    if args.svg:
        Path(args.svg).write_text(inheritance_svg(config, rows))
        print(f'wrote {args.svg}', file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    cmds = parser.add_subparsers(dest='command', required=True)
    p = cmds.add_parser('intervals')
    p.add_argument('trace', type=Path)
    p.add_argument('--csv', action='store_true', help='one row per block')
    p.add_argument('--svg', type=Path)
    p.set_defaults(run=cmd_intervals)
    p = cmds.add_parser('locks')
    p.add_argument('trace', type=Path)
    p.add_argument('--min-us', type=int, default=500)
    p.set_defaults(run=cmd_locks)
    p = cmds.add_parser('timeline')
    p.add_argument('trace', type=Path)
    p.add_argument('--svg', type=Path, required=True)
    p.add_argument('--job', type=int,
                   help='first block to draw (default: the block with the longest publication)')
    p.add_argument('--jobs', type=int, default=3, help='how many blocks (default 3)')
    p.set_defaults(run=cmd_timeline)
    p = cmds.add_parser('inheritance')
    p.add_argument('log', type=Path)
    p.add_argument('--svg', type=Path)
    p.set_defaults(run=cmd_inheritance)
    args = parser.parse_args()
    try:
        args.run(args)
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    raise SystemExit(main())
