#!/usr/bin/env python3
"""Zero-jitter, single-core fixed-priority RTA; all durations are microseconds.

One bounded lower-priority blocking term per task is supplied as B_us. Tasks
must have distinct priorities and D <= T. This is a model check, not a WCET
measurement, scheduler simulator, or proof about a compiled firmware image.
"""
import argparse
import json
from pathlib import Path


def analyze(tasks):
    if not isinstance(tasks, list) or not tasks:
        raise ValueError('provide a non-empty JSON list of tasks')
    names, priorities = set(), set()
    for task in tasks:
        if not isinstance(task, dict):
            raise ValueError('each task must be an object')
        if set(task) - {'name', 'priority', 'C_us', 'B_us', 'T_us', 'D_us'}:
            raise ValueError('unsupported field: this model excludes jitter and offsets')
        name = task.get('name')
        if not isinstance(name, str) or not name or name in names:
            raise ValueError('task names must be non-empty and unique')
        names.add(name)
        for key in ('priority', 'C_us', 'T_us', 'D_us'):
            value = task.get(key)
            if type(value) is not int or value <= 0:
                raise ValueError(f'{name}: {key} must be a positive integer')
        block = task.get('B_us', 0)
        if type(block) is not int or block < 0:
            raise ValueError(f'{name}: B_us must be a non-negative integer')
        if task['D_us'] > task['T_us']:
            raise ValueError(f'{name}: this check requires D_us <= T_us')
        if task['priority'] in priorities:
            raise ValueError('priorities must be distinct')
        priorities.add(task['priority'])

    results = []
    for task in sorted(tasks, key=lambda row: -row['priority']):
        base = task['C_us'] + task.get('B_us', 0)
        trace = [base]
        for _ in range(10000):
            if trace[-1] > task['D_us']:
                break
            interference = sum(
                ((trace[-1] + other['T_us'] - 1) // other['T_us']) * other['C_us']
                for other in tasks if other['priority'] > task['priority'])
            updated = base + interference
            if updated == trace[-1]:
                results.append({'name': task['name'], 'trace_us': trace,
                                'R_us': updated, 'pass': True})
                break
            trace.append(updated)
        else:
            raise ValueError('iteration limit exceeded; inspect the model')
        if len(results) == 0 or results[-1]['name'] != task['name']:
            results.append({'name': task['name'], 'trace_us': trace,
                            'R_us': None, 'pass': False})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('taskset', type=Path)
    args = parser.parse_args()
    try:
        results = analyze(json.loads(args.taskset.read_text()))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    for row in results:
        status = 'PASS' if row['pass'] else 'BOUND EXCEEDS DEADLINE'
        trace = ' -> '.join(str(value) for value in row['trace_us'])
        print(f"{row['name']}: {trace} us [{status}]")
    print('Model result only; execution, blocking and overhead bounds require evidence.')
    return 0 if all(row['pass'] for row in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
