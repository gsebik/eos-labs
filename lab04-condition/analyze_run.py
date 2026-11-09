#!/usr/bin/env python3
"""Decode L4 UART evidence. Summary maxima are never converted into a schedule.

Usage: python3 analyze_run.py capture.log --out analysis-directory
Only an intact sched_trace block can produce execution lanes/residency shares.
No plotting packages required. SVG labels and JSON retain measurement limits.
"""
import argparse
from collections import Counter
import csv
import hashlib
from html import escape
import json
from pathlib import Path
import re


COLORS = {'A': '#2563eb', 'B': '#15803d', 'C': '#b45309',
          'idle': '#e2e8f0', 'hb': '#7e22ce', 'report': '#be123c'}
NOTE = ('Instrumented early-run trace, not WCET. Residency includes IRQ time '
        'charged to the interrupted task. Unobserved prefix time is unknown.')


def fields(line):
    return dict(re.findall(r'([A-Za-z_][\w]*)=([^,\s]+)', line))


def numbers(row):
    return {k: int(v) if re.fullmatch(r'-?\d+', v) else v for k, v in row.items()}


def decode(text):
    """Return source records; reject malformed traces rather than inventing bars."""
    groups, instrument, events = [], [], []
    group = None
    sig = None
    header = end = None
    for raw in text.splitlines():
        line = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', raw).strip()
        row = numbers(fields(line))
        if line.startswith('sched,error='):
            raise ValueError('Firmware rejected the configuration: ' + line)
        if line.startswith('sched,hb='):
            group = {'t_ms': row.get('t_ms'), 'tasks': []}
            groups.append(group)
        elif line.startswith('sched,task=') and group is not None:
            required = ('task', 'n', 'miss', 'max_late_us', 'max_exec_us', 'max_overrun_us')
            if any(key not in row for key in required):
                raise ValueError('Incomplete scheduler row: ' + line)
            group['tasks'].append(row)
        elif line.startswith('sig rtos='):
            sig = row
            instrument.append(sig)
        elif sig is not None and (line.startswith('buffer_return_max_us=') or line.startswith('l4,')):
            sig.update(row)
        elif line.startswith('sched_trace,'):
            if header is not None:
                raise ValueError('More than one trace/boot: analyse each boot separately')
            header = row
        elif line.startswith('sched_event,'):
            events.append(row)
        elif line.startswith('sched_trace_end,'):
            if end is not None:
                raise ValueError('Duplicate trace ending')
            end = row
    complete_groups = [g for g in groups if len(g['tasks']) == 3 and
                       {r['task'] for r in g['tasks']} == {'A', 'B', 'C'}]
    discarded_groups = len(groups) - len(complete_groups)
    if events or header or end:
        if header is None or end is None:
            raise ValueError('Incomplete trace: retain the log through sched_trace_end')
        if any(k not in header for k in ('begin_us', 'end_us', 'cores', 'capacity_per_core')):
            raise ValueError('Incomplete trace header')
        if end.get('dropped') != 0:
            raise ValueError('Trace overflow: no execution timeline or residency estimate is valid')
        if end.get('n') != len(events):
            raise ValueError('Trace count mismatch: serial capture lost or duplicated rows')
        if header['cores'] not in (1, 2) or header['end_us'] <= header['begin_us']:
            raise ValueError('Invalid trace domain')
        by_core = {c: [] for c in range(header['cores'])}
        for event in events:
            required = ('core', 'seq', 't_us', 'task', 'kind', 'job', 'release_us', 'deadline_us')
            if any(k not in event for k in required):
                raise ValueError('Incomplete event')
            if event['core'] not in by_core or event['kind'] not in ('in', 'out', 'start', 'finish'):
                raise ValueError('Invalid trace core or event kind')
            if not header['begin_us'] <= event['t_us'] < header['end_us']:
                raise ValueError('Event outside the stated recording window')
            if event['kind'] in ('start', 'finish'):
                if event['t_us'] < event['release_us']:
                    raise ValueError('Job event precedes its release: invalid release timestamp; rebuild the corrected firmware')
                if event['deadline_us'] <= event['release_us']:
                    raise ValueError('Job deadline does not follow its release')
            by_core[event['core']].append(event)
        for core, rows in by_core.items():
            rows.sort(key=lambda e: e['seq'])
            if [e['seq'] for e in rows] != list(range(len(rows))):
                raise ValueError(f'Missing/duplicate trace sequence on core {core}')
            if len(rows) > header['capacity_per_core']:
                raise ValueError('Trace exceeds declared capacity')
            if any(b['t_us'] < a['t_us'] for a, b in zip(rows, rows[1:])):
                raise ValueError('Nonmonotonic shared-clock timestamps')
    if not complete_groups and not instrument and header is None:
        raise ValueError('No complete L4 evidence found in the input')
    return {'scheduler': complete_groups, 'instrument': instrument,
            'trace_header': header, 'events': events,
            'discarded_summary_groups': discarded_groups}


def trace_metrics(data):
    h = data['trace_header']
    if h is None:
        return None
    segments, shares = [], []
    window = h['end_us'] - h['begin_us']
    for core in range(h['cores']):
        active = None
        seen_switch = False
        residency = Counter()
        for event in sorted((e for e in data['events'] if e['core'] == core), key=lambda e: e['seq']):
            if event['kind'] == 'in':
                if active is not None:
                    raise ValueError('Switch-in without matching switch-out')
                active = event
                seen_switch = True
            elif event['kind'] == 'out':
                if active is not None:
                    if active['task'] != event['task']:
                        raise ValueError('Switch-out names the wrong running task')
                    segment = {'core': core, 'task': active['task'],
                               'start_us': active['t_us'], 'finish_us': event['t_us']}
                    segments.append(segment)
                    residency[active['task']] += event['t_us'] - active['t_us']
                    active = None
                elif seen_switch:
                    raise ValueError('Switch-out without a matching running segment')
                seen_switch = True
                # An initial OUT names a task we did not see dispatched. Do not
                # invent the missing prefix; it remains explicitly unknown.
        if active is not None:
            segments.append({'core': core, 'task': active['task'],
                             'start_us': active['t_us'], 'finish_us': h['end_us']})
            residency[active['task']] += h['end_us'] - active['t_us']
        known = sum(residency.values())
        shares.append({'core': core, 'window_us': window,
                       'observed_task_residency_us': dict(residency),
                       'unattributed_us': window - known,
                       'nonidle_residency_fraction': (known - residency['idle']) / window})
    starts, jobs = {}, []
    incomplete_finishes = 0
    for event in sorted(data['events'], key=lambda e: (e['t_us'], e['core'], e['seq'])):
        key = (event['task'], event['job'])
        if event['kind'] == 'start':
            if key in starts:
                raise ValueError('Duplicate job start')
            starts[key] = event
        elif event['kind'] == 'finish':
            start = starts.pop(key, None)
            if start is None:
                incomplete_finishes += 1
                continue
            if (start['release_us'], start['deadline_us']) != (event['release_us'], event['deadline_us']):
                raise ValueError('Job boundary metadata changed')
            if event['t_us'] < start['t_us']:
                raise ValueError('Job finished before its start')
            resident = sum(max(0, min(s['finish_us'], event['t_us']) - max(s['start_us'], start['t_us']))
                           for s in segments if s['task'] == event['task'])
            jobs.append({'task': event['task'], 'job': event['job'],
                         'release_us': event['release_us'], 'start_us': start['t_us'],
                         'finish_us': event['t_us'], 'deadline_us': event['deadline_us'],
                          'start_delay_us': start['t_us'] - event['release_us'],
                         'elapsed_us': event['t_us'] - start['t_us'],
                         'response_us': event['t_us'] - event['release_us'],
                         'overrun_us': max(0, event['t_us'] - event['deadline_us']),
                         'scheduled_residency_us': resident})
    return {'segments': segments, 'jobs': jobs, 'core_shares': shares,
            'incomplete_jobs': len(starts) + incomplete_finishes, 'limits': NOTE}


def svg_frame(title, body, height=430):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="1120" height="{height}" '
            f'viewBox="0 0 1120 {height}" role="img"><title>{escape(title)}</title>'
            '<rect width="100%" height="100%" fill="white"/>'
            '<g font-family="sans-serif" fill="#172033">'
            f'<text x="24" y="35" font-size="23">{escape(title)}</text>{body}</g></svg>\n')


def txt(x, y, text, size=17):
    return f'<text x="{x}" y="{y}" font-size="{size}">{escape(str(text))}</text>'


def trace_svg(header, metrics):
    begin, end = header['begin_us'], header['end_us']
    x = lambda t: 120 + 950 * (t - begin) / (end - begin)
    body = ''
    for core in range(header['cores']):
        y = 90 + core * 85
        body += txt(20, y + 24, f'Core {core}')
        body += f'<rect x="120" y="{y}" width="950" height="36" fill="#f1f5f9"/>'
        for seg in metrics['segments']:
            if seg['core'] != core:
                continue
            tooltip = f'{seg["task"]}: {seg["start_us"]}–{seg["finish_us"]} board µs'
            body += (f'<rect x="{x(seg["start_us"]):.3f}" y="{y}" '
                     f'width="{max(.2,x(seg["finish_us"])-x(seg["start_us"])):.3f}" height="36" '
                     f'fill="{COLORS.get(seg["task"], "#475569")}"><title>{escape(tooltip)}</title></rect>')
    for tick in range(7):
        t = begin + (end - begin) * tick / 6
        body += txt(x(t) - 8, 270, f'{(t-begin)/1000:g}')
    body += txt(120, 300, 'Time since recorder start (ms); bars come from recorded switch-in/out events.')
    body += txt(120, 332, 'A blue · B green · C amber · idle grey · heartbeat purple · reporter red')
    body += txt(24, 375, 'Blank/unattributed time is not evidence of idle CPU. Hover over a segment for timestamps.', 16)
    body += txt(24, 402, NOTE, 14)
    return svg_frame('Measured scheduled execution — not reconstructed from summary maxima', body)


def job_svg(job, metrics):
    begin = min(job['release_us'], job['start_us'])
    end = max(job['deadline_us'], job['finish_us']) + 1
    x = lambda t: 140 + 900 * (t - begin) / (end - begin)
    body = txt(24, 84, f'{job["task"]}, job {job["job"]}: timestamps all use the board timer')
    body += f'<rect x="{x(begin)}" y="135" width="{x(job["start_us"])-x(begin)}" height="30" fill="#e2e8f0"/>'
    body += f'<rect x="{x(job["start_us"])}" y="135" width="{x(job["finish_us"])-x(job["start_us"])}" height="30" fill="#fef3c7"/>'
    for segment in metrics['segments']:
        if segment['task'] != job['task']:
            continue
        a, b = max(segment['start_us'], job['start_us']), min(segment['finish_us'], job['finish_us'])
        if b > a:
            body += f'<rect x="{x(a)}" y="140" width="{x(b)-x(a)}" height="20" fill="{COLORS[job["task"]]}"/>'
    for t, label, y in [(job['release_us'], 'intended release', 210), (job['start_us'], 'start', 245),
                        (job['deadline_us'], 'deadline', 280), (job['finish_us'], 'finish', 315)]:
        body += f'<path d="M{x(t)} 115 V175" stroke="#172033" stroke-dasharray="4 3"/>'
        body += txt(140, y, f'{label}: {t} µs ({(t-begin)/1000:.3f} ms after left edge)')
    body += txt(24, 355, f'Start delay {job["start_delay_us"]} µs; elapsed E {job["elapsed_us"]} µs; response R {job["response_us"]} µs.')
    body += txt(24, 385, f'Deadline overrun {job["overrun_us"]} µs. Coloured pieces: scheduled residency; pale gaps: not scheduled.')
    body += txt(24, 412, 'Intended release may precede this loop iteration; delayed jobs can carry backlog. None of these values is a WCET proof.', 14)
    return svg_frame('One observed job: release, execution segments, deadline and completion', body, 440)


def summary_svg(groups):
    body = txt(24, 76, 'Window maxima, not per-job samples. First group is startup; incomplete groups are excluded.')
    labels = [('max_late_us', 'start delay'), ('max_exec_us', 'elapsed E'), ('max_response_us', 'response R')]
    maximum = max([r.get(k, 0) for g in groups for r in g['tasks'] for k, _ in labels] + [1])
    y = 112
    for task in ('A', 'B', 'C'):
        rows = [r for g in groups for r in g['tasks'] if r['task'] == task]
        for key, label in labels:
            value = max(r.get(key, 0) for r in rows)
            body += txt(24, y+18, f'{task}: {label}')
            body += f'<rect x="205" y="{y}" height="22" width="{720*value/maximum:.2f}" fill="{COLORS[task]}"/>'
            body += txt(935, y+18, f'{value} µs')
            y += 34
    body += txt(24, y+25, 'Do not add maxima from separate columns: they may describe different jobs.')
    return svg_frame('Scheduler report summary', body, y+65)


def instrument_svg(groups, rates):
    rows = [g for g in groups if 'compute_max_us' in g and 't_ms' in g]
    if not rows:
        return None
    t0, t1 = rows[0]['t_ms'], max(rows[-1]['t_ms'], rows[0]['t_ms'] + 1)
    maximum = max([g['compute_max_us'] for g in rows] +
                  [g.get('freshness_deadline_us', 0) for g in rows] + [1])
    x = lambda t: 120 + 930 * (t-t0)/(t1-t0)
    y = lambda value: 255 - 150 * value/maximum
    body = txt(24, 74, 'Elapsed DSP maxima from status groups; these are not CPU demand or utilisation.')
    body += '<path d="M120 100V255H1050" fill="none" stroke="#475569"/>'
    points = ' '.join(f'{x(g["t_ms"])},{y(g["compute_max_us"])}' for g in rows)
    body += f'<polyline points="{points}" fill="none" stroke="#2563eb" stroke-width="3"/>'
    for g in rows:
        body += f'<circle cx="{x(g["t_ms"])}" cy="{y(g["compute_max_us"])}" r="4" fill="#2563eb"><title>{g["t_ms"]} ms: {g["compute_max_us"]} µs</title></circle>'
    deadline = rows[0].get('freshness_deadline_us')
    if deadline and all(g.get('freshness_deadline_us') == deadline for g in rows):
        body += f'<path d="M120 {y(deadline)}H1050" stroke="#b45309" stroke-dasharray="7 5"/>'
        body += txt(140, y(deadline)-9, f'Configured freshness deadline: {deadline} µs')
    body += txt(24, 125, f'{maximum} µs', 14) + txt(24, 255, '0 µs', 14)
    body += txt(120, 286, f'Board time: {t0} to {t1} ms. Blue: observed compute_max_us.')
    if rates:
        values = ', '.join(f'{r["results_per_second"]:.1f}' for r in rates[:8])
        body += txt(24, 330, 'Results/s in successive measured intervals: ' + values, 16)
    body += txt(24, 370, 'Throughput = Δprocessed × 1000 / Δt_ms. It is not a task execution-time bound.', 16)
    body += txt(24, 405, 'Keep FFT size, repeats, rows, clock and run length matched; inspect full CSV records.', 16)
    return svg_frame('Instrument progress and elapsed DSP bracket', body, 440)


def write_csv(path, rows):
    if not rows:
        return
    keys = list(dict.fromkeys(k for row in rows for k in row))
    with path.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)


def analyse(log, output):
    data = decode(log.read_text(errors='replace'))
    metrics = trace_metrics(data)
    # A traced build's UART dump disturbs later report windows. Its RAM events,
    # not those later summaries, are the comparable evidence.
    if metrics is not None:
        data['summary_limit'] = 'Traced build: use RAM trace, not post-dump report maxima for comparisons.'
    rates = []
    for before, after in zip(data['instrument'], data['instrument'][1:]):
        if not all(k in r for r in (before, after) for k in ('t_ms', 'processed')):
            continue
        dt, done = after['t_ms'] - before['t_ms'], after['processed'] - before['processed']
        if dt <= 0 or done < 0:
            raise ValueError('Instrument clock/counter reset: analyse each boot separately')
        rates.append({'from_t_ms': before['t_ms'], 'to_t_ms': after['t_ms'],
                      'processed_delta': done, 'results_per_second': done * 1000 / dt})
    report = {'input': str(log.resolve()), 'input_sha256': hashlib.sha256(log.read_bytes()).hexdigest(),
              'scheduler_groups': len(data['scheduler']),
              'discarded_summary_groups': data['discarded_summary_groups'],
              'instrument_groups': len(data['instrument']), 'rates': rates,
              'trace_header': data['trace_header'], 'trace': metrics, 'limits': data.get('summary_limit',
                  'Summary logs contain no execution timeline or measured CPU utilisation.')}
    output.mkdir(parents=True, exist_ok=False)
    write_csv(output/'scheduler.csv', [dict(t_ms=g['t_ms'], **r) for g in data['scheduler'] for r in g['tasks']])
    write_csv(output/'instrument.csv', data['instrument'])
    write_csv(output/'throughput.csv', rates)
    if data['scheduler']:
        (output/'summary.svg').write_text(summary_svg(data['scheduler']))
    instrument_plot = instrument_svg(data['instrument'], rates)
    if instrument_plot:
        (output/'instrument.svg').write_text(instrument_plot)
    if metrics is not None:
        write_csv(output/'jobs.csv', metrics['jobs'])
        write_csv(output/'segments.csv', metrics['segments'])
        (output/'timeline.svg').write_text(trace_svg(data['trace_header'], metrics))
        if metrics['jobs']:
            chosen = max(metrics['jobs'], key=lambda j: (j['overrun_us'], j['response_us']))
            (output/'job.svg').write_text(job_svg(chosen, metrics))
            report['selected_job'] = chosen
    (output/'analysis.json').write_text(json.dumps(report, indent=2)+'\n')
    images = ''.join(f'<h2>{escape(p.stem)}</h2><img style="max-width:100%" src="{p.name}" alt="{escape(p.stem)}">'
                     for p in sorted(output.glob('*.svg')))
    (output/'index.html').write_text('<!doctype html><html lang="en"><meta charset="utf-8">'
         '<meta name="viewport" content="width=device-width"><title>L4 evidence</title>'
         '<body style="font:18px sans-serif;max-width:1120px;margin:auto;padding:1rem">'
         '<h1>L4 capture analysis</h1><p>'+escape(report['limits'])+'</p>'+images+
         '<p>Read analysis.json and CSV files for the exact data. No summary maximum is labelled CPU demand.</p></body></html>')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    try:
        report = analyse(args.log, args.out)
    except (ValueError, OSError, KeyError, TypeError) as exc:
        parser.exit(1, f'Cannot analyse capture: {exc}\nUse a complete single-boot log and a new output directory.\n')
    print(f'{args.out}/index.html: {report["scheduler_groups"]} scheduler groups; {report["instrument_groups"]} instrument groups')
    print(report['limits'])


if __name__ == '__main__':
    main()
