#!/usr/bin/env python3
"""Decode and monitor the DSP instrument's finite app-event trace.

Binary records are little-endian <QIIII (24 bytes). The buffer is dumped with
three metadata words: count, truncated, frozen. GDB must halt the core before
dumping. All times use the one target microsecond timer; no DWT or analyzer
timestamp merging is performed.

The recorder captures a bounded window that ends at the first deadline miss
(frozen) or when the buffer fills (truncated), so a dump contains the events
that led to the interesting moment rather than a holey tail.

The monitor checks the ready-to-release block response, observed drops and
invalid lifecycle events, and breaks each job into its settings wait, compute
and publication wait. It is NOT a complete FreeRTOS schedule, does not validate
every task interaction, and does not prove that no physical samples were lost.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct
import sys

RECORD = struct.Struct('<QIIII')
EVENTS = {1: 'block_ready', 2: 'work_start', 3: 'work_complete',
          4: 'block_release', 5: 'drop', 6: 'display_start', 7: 'display_done',
          8: 'settings_write', 9: 'settings_wait', 10: 'publish_wait',
          11: 'result_published'}
# The two completion endpoints measure different obligations. Buffer return
# decides whether acquisition can reuse the storage; publication decides whether
# the worker can take its next block. They must not share one verdict.
COMPLETIONS = ('block_release', 'result_published')
POOL_SIZE = 3
METADATA_WORDS = 3


def decode(raw, count, truncated=0, frozen=0):
    if type(count) is not int or count < 0:
        raise ValueError('count must be a non-negative integer')
    if truncated not in (0, 1) or frozen not in (0, 1):
        raise ValueError('truncated and frozen must be 0 or 1')
    if len(raw) % RECORD.size or count > len(raw) // RECORD.size:
        raise ValueError('partial record or count exceeds dump length')
    records = []
    for index in range(count):
        time, kind, job, arg, aux = RECORD.unpack_from(raw, index * RECORD.size)
        if kind not in EVENTS:
            raise ValueError(f'unknown event {kind} at record {index}')
        records.append(dict(t_us=time, ev=EVENTS[kind], job=job, arg=arg, aux=aux))
    header = dict(type='eos-instrument-trace', version=2, source='target',
                  time_domain='target-timer-us', resolution_us=1, core=0,
                  pool_size=POOL_SIZE, truncated=truncated, frozen=frozen)
    return header, records


def close(job, pending, response_time, deadline_us, phases, jobs, finished,
          violations):
    """Close one job at the selected completion endpoint."""
    response = response_time - pending['ready']
    if response > deadline_us:
        violations.append(dict(job=job, why='response exceeds deadline',
                               response_us=response, deadline_us=deadline_us))
    phases.append(dict(
        job=job, completion=response_time == pending.get('published_at')
        and 'result_published' or 'block_release',
        response_us=response,
        settings_wait_us=pending['started_at'] - pending['ready'],
        compute_us=pending['completed_at'] - pending['started_at'],
        publish_wait_us=response_time - pending['completed_at'],
        buffer_return_us=(pending['released_at'] - pending['ready']
                          if 'released_at' in pending else None),
        publication_response_us=(pending['published_at'] - pending['ready']
                                 if 'published_at' in pending else None)))
    jobs.pop(job)
    finished.append(job)


def monitor(header, records, deadline_us=10240, completion='block_release'):
    if (header.get('type'), header.get('version'), header.get('time_domain')) != (
            'eos-instrument-trace', 2, 'target-timer-us'):
        raise ValueError('unsupported trace schema or time domain')
    if header.get('core') != 0 or header.get('resolution_us') != 1:
        raise ValueError('this monitor requires the core-0 microsecond trace')
    if header.get('pool_size') != POOL_SIZE:
        raise ValueError('this monitor requires the three-buffer pool trace')
    truncated, frozen = header.get('truncated'), header.get('frozen')
    if truncated not in (0, 1) or frozen not in (0, 1):
        raise ValueError('invalid truncated/frozen flags')
    if type(deadline_us) is not int or deadline_us <= 0:
        raise ValueError('deadline must be a positive integer number of microseconds')
    if completion not in COMPLETIONS:
        raise ValueError(f'completion must be one of {COMPLETIONS}')
    jobs, owners, finished, violations = {}, {}, [], []
    phases = []
    previous = -1
    for index, event in enumerate(records):
        for key in ('t_us', 'job', 'arg', 'aux'):
            if type(event.get(key)) is not int or event[key] < 0:
                raise ValueError(f'invalid {key} at event {index}')
        time, kind, job = event['t_us'], event.get('ev'), event['job']
        if kind not in EVENTS.values() or time < previous:
            raise ValueError(f'unknown event or decreasing timestamp at event {index}')
        previous = time
        if kind == 'block_ready':
            buffer = event['arg']
            if (buffer >= POOL_SIZE or job in jobs or job in finished
                    or buffer in owners):
                violations.append(dict(index=index, job=job,
                                       why='duplicate job/buffer use'))
                continue
            jobs[job] = dict(ready=time, buffer=buffer, started=False,
                             complete=False)
            owners[buffer] = job
        elif kind == 'work_start':
            if (job not in jobs or jobs[job]['started']
                    or event['arg'] != jobs[job]['buffer']):
                violations.append(dict(index=index, job=job,
                                       why='start without unique ready'))
            else:
                jobs[job]['started'] = True
                jobs[job]['started_at'] = time
        elif kind == 'work_complete':
            pending = jobs.get(job)
            if (pending is None or not pending['started']
                    or event['arg'] != pending['buffer']):
                violations.append(dict(index=index, job=job,
                                       why='completion without start'))
            else:
                pending['complete'] = True
                pending['completed_at'] = time
        elif kind == 'block_release':
            pending = jobs.get(job)
            if (pending is None or not pending['complete']
                    or event['arg'] != pending['buffer']):
                violations.append(dict(index=index, job=job,
                                       why='release without completion'))
                continue
            # Input storage becomes available to DMA here, not when the
            # arithmetic ends. Publication is a separate, later obligation.
            pending['released_at'] = time
            owners.pop(pending['buffer'], None)
            if completion == 'block_release':
                close(job, pending, time, deadline_us, phases, jobs, finished,
                      violations)
        elif kind == 'result_published':
            # Optional for older traces: a trace recorded before this event
            # existed still decodes, because block_release then closes the job.
            pending = jobs.get(job)
            if pending is None:
                continue
            pending['published_at'] = time
            if completion == 'result_published':
                close(job, pending, time, deadline_us, phases, jobs, finished,
                      violations)
        elif kind == 'drop':
            violations.append(dict(index=index, job=job, why='drop'))
    if violations:
        verdict = 'FAIL'
    elif jobs or not finished:
        verdict = 'INCONCLUSIVE'
    elif truncated:
        verdict = 'NO_VIOLATION_IN_WINDOW'
    else:
        verdict = 'NO_VIOLATION_IN_TRACE'
    worst = max(phases, key=lambda row: row['response_us'], default=None)
    return dict(verdict=verdict, completed=len(finished), pending=list(jobs),
                truncated=truncated, frozen=frozen, violations=violations,
                worst=worst,
                scope='Recorded app events only; not all schedules, ISR latency or WCET.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    dump = commands.add_parser('decode')
    dump.add_argument('binary', type=Path)
    counts = dump.add_mutually_exclusive_group(required=True)
    counts.add_argument('--metadata', type=Path,
                        help='GDB dump of the three metadata words')
    counts.add_argument('--count', type=int)
    dump.add_argument('--truncated', action='store_true')
    dump.add_argument('--frozen', action='store_true')
    dump.add_argument('--elf', type=Path, required=True,
                      help='ELF recorded by the experimenter, not read from the target')
    dump.add_argument('--csv', action='store_true',
                      help='write one CSV row per event instead of JSON lines')
    check = commands.add_parser('check')
    check.add_argument('trace', type=Path)
    check.add_argument('--deadline-us', type=int, default=10240)
    check.add_argument('--completion', choices=COMPLETIONS, default='block_release',
                       help='which obligation to check against the deadline')
    args = parser.parse_args()
    try:
        if args.command == 'decode':
            if args.metadata:
                metadata = args.metadata.read_bytes()
                if len(metadata) != 4 * METADATA_WORDS:
                    raise ValueError('metadata must contain exactly three uint32 words')
                count, truncated, frozen = struct.unpack('<III', metadata)
            else:
                count, truncated, frozen = args.count, int(args.truncated), int(args.frozen)
            header, records = decode(args.binary.read_bytes(), count,
                                     truncated, frozen)
            header['recorded_elf_sha256'] = hashlib.sha256(args.elf.read_bytes()).hexdigest()
            if args.csv:
                writer = csv.writer(sys.stdout)
                writer.writerow(['t_us', 'ev', 'job', 'arg', 'aux'])
                for event in records:
                    writer.writerow([event['t_us'], event['ev'], event['job'],
                                     event['arg'], event['aux']])
                return 0
            print(json.dumps(header))
            for event in records:
                print(json.dumps(event))
            return 0
        lines = [json.loads(line) for line in args.trace.read_text().splitlines()
                 if line.strip()]
        if not lines:
            raise ValueError('empty trace')
        result = monitor(lines[0], lines[1:], args.deadline_us,
                         completion=args.completion)
        print(json.dumps(result, indent=2))
        return 1 if result['verdict'] == 'FAIL' else (2 if result['verdict'] == 'INCONCLUSIVE' else 0)
    except (OSError, ValueError, TypeError, KeyError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    raise SystemExit(main())
