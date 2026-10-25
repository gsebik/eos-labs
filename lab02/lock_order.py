#!/usr/bin/env python3
"""Tiny untimed state explorer: two workers, two single-instance mutexes.

State = (program counters, explicit-wait flags, mutex owners).
Owner -1 means free; 0/1 means A/B. Each worker takes two locks, then gives
them in reverse order and reaches DONE. This is not FreeRTOS execution or
UPPAAL's timed-deadlock semantics. All rules are visible Python, no solver.
"""
import argparse
from collections import deque
import json


def successors(state, orders, explicit_waits=False):
    counters, waiting, owners = state
    for worker in (0, 1):
        pc = counters[worker]
        if pc == 4: continue
        resource = orders[worker][pc if pc < 2 else 3 - pc]
        next_pc, next_wait, next_owners = list(counters), list(waiting), list(owners)
        if pc < 2:
            if owners[resource] != -1:
                if not explicit_waits or waiting[worker]: continue
                next_wait[worker] = True
                label = f'{"AB"[worker]} requests mutex {resource + 1} and blocks'
            else:
                next_owners[resource] = worker
                next_pc[worker] += 1
                next_wait[worker] = False
                label = f'{"AB"[worker]} acquires mutex {resource + 1}'
        else:
            if owners[resource] != worker:
                raise ValueError('invalid ownership on release')
            next_owners[resource] = -1
            next_pc[worker] += 1
            label = f'{"AB"[worker]} releases mutex {resource + 1}'
        yield (tuple(next_pc), tuple(next_wait), tuple(next_owners)), label


def explore(ordered=False, explicit_waits=False):
    orders = ((0, 1), (0, 1) if ordered else (1, 0))
    initial = ((0, 0), (False, False), (-1, -1))
    pending = deque([initial])
    parents = {initial: None}
    successful_terminals = 0
    while pending:
        state = pending.popleft()
        edges = list(successors(state, orders, explicit_waits))
        if not edges:
            if state[0] == (4, 4):
                successful_terminals += 1
                continue
            witness = []
            cursor = state
            while parents[cursor] is not None:
                previous, action = parents[cursor]
                witness.append(action)
                cursor = previous
            return dict(verdict='DEADLOCK_REACHABLE', states=len(parents),
                        counterexample=['Initial', *reversed(witness), 'DEADLOCK'])
        for target, action in edges:
            if target not in parents:
                parents[target] = (state, action)
                pending.append(target)
    return dict(verdict='NO_ERRONEOUS_TERMINAL_IN_MODEL', states=len(parents),
                successful_terminals=successful_terminals, counterexample=[])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ordered', action='store_true')
    parser.add_argument('--explicit-waits', action='store_true')
    args = parser.parse_args()
    result = explore(args.ordered, args.explicit_waits)
    print(json.dumps(result, indent=2))
    print('Finite untimed model only; not a proof of the C firmware or timing.')
    return 1 if result['verdict'] == 'DEADLOCK_REACHABLE' else 0


if __name__ == '__main__':
    raise SystemExit(main())
