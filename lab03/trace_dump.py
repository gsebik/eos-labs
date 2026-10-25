#!/usr/bin/env python3
"""Copy the event trace out of the running board and decode it.

    python3 trace_dump.py build/lab03_instrument_rtos.elf

Give the ELF that is running on the board; the debugger loads nothing. The tool
starts the OpenOCD server, halts the core for a moment, copies two regions of
RAM, lets the board run again and decodes them:

    trace.bin      the raw event array (g_sig_trace, 2048 events of 24 bytes)
    metadata.bin   three 32-bit words: events recorded, truncated, frozen
    trace.jsonl    the decoded events          trace.csv   the same as a table

By hand this is `picolab ocd` in one terminal and arm-none-eabi-gdb in another;
see the lab manual, "Reference: capture the event trace".
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
EVENTS, RECORD = 2048, 24   # SIG_TRACE_CAPACITY and sizeof(sig_event_t), trace.h


def wait_for_port(port=3333, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        with socket.socket() as s:
            s.settimeout(0.3)
            if s.connect_ex(('127.0.0.1', port)) == 0:
                return True
        time.sleep(0.3)
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('elf', type=Path, help='the ELF running on the board')
    parser.add_argument('--out', type=Path, default=Path('.'),
                        help='folder for the files (default: here)')
    args = parser.parse_args()
    gdb = shutil.which('arm-none-eabi-gdb') or shutil.which('gdb-multiarch')
    if not gdb:
        parser.error('no arm-none-eabi-gdb found (picolab doctor)')
    if not args.elf.is_file():
        parser.error(f'no such file: {args.elf}')
    args.out.mkdir(parents=True, exist_ok=True)
    binary, meta = args.out / 'trace.bin', args.out / 'metadata.bin'

    server = subprocess.Popen(['picolab', 'ocd'], start_new_session=True,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_for_port():
            sys.exit('the OpenOCD server did not start; is another picolab ocd running?')
        commands = [
            'target extended-remote 127.0.0.1:3333', 'monitor halt',
            f'dump binary memory {binary} (char*)&g_sig_trace '
            f'((char*)&g_sig_trace+{EVENTS * RECORD})',
            f'dump binary memory {meta} (char*)&g_sig_trace_metadata '
            f'((char*)&g_sig_trace_metadata+12)',
            'detach', 'quit']
        cmd = [gdb, '-q', '-batch', str(args.elf)]
        for c in commands:
            cmd += ['-ex', c]
        done = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if done.returncode or not binary.exists() or not meta.exists():
            sys.exit('GDB could not read the trace:\n' + done.stdout + done.stderr)
    finally:
        try:
            os.killpg(os.getpgid(server.pid), 15)
            server.wait(timeout=10)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            pass

    sig = HERE / 'sig_trace.py'
    for suffix, extra in (('jsonl', []), ('csv', ['--csv'])):
        with open(args.out / f'trace.{suffix}', 'w') as stream:
            subprocess.run([sys.executable, str(sig), 'decode', str(binary),
                            '--metadata', str(meta), '--elf', str(args.elf)] + extra,
                           stdout=stream, check=True)
    print(f'wrote {binary.name}, {meta.name}, trace.jsonl and trace.csv in {args.out}')
    print('the board is running again')


if __name__ == '__main__':
    main()
