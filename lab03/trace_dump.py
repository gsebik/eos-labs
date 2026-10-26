#!/usr/bin/env python3
"""Copy the event trace out of the running board and decode it.

    python3 trace_dump.py build/lab03_instrument_rtos.elf

Give the ELF that is running on the board. The tool finds where the trace lives
in RAM (`nm`), starts the OpenOCD server (`picolab ocd`), reads two regions of
RAM over SWD and decodes them. It reads the RAM without stopping the processor, and makes sure the
instrument is running when it finishes.

    metadata.bin   three 32-bit words: events recorded, truncated, frozen
    trace.bin      the raw event array (2048 events of 24 bytes)
    trace.jsonl    the decoded events          trace.csv   the same as a table

The metadata is read first, so every event it counts is already complete.
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
EVENTS, RECORD = 2048, 24    # SIG_TRACE_CAPACITY and sizeof(sig_event_t), trace.h
METADATA_BYTES = 12          # count, truncated, frozen


def symbols(elf):
    nm = shutil.which('arm-none-eabi-nm') or shutil.which('nm')
    out = subprocess.run([nm, str(elf)], capture_output=True, text=True, check=True).stdout
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in ('g_sig_trace', 'g_sig_trace_metadata'):
            found[parts[2]] = int(parts[0], 16)
    if len(found) != 2:
        sys.exit(f'{elf} has no g_sig_trace symbols: give the ELF that is running')
    return found


def connect(port=4444, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            return socket.create_connection(('127.0.0.1', port), timeout=3)
        except OSError:
            time.sleep(0.3)
    return None


def port_open(port=4444):
    with socket.socket() as s:
        s.settimeout(0.3)
        return s.connect_ex(('127.0.0.1', port)) == 0


def command(sock, text):
    """Send one OpenOCD command and return its output."""
    sock.sendall((text + '\n').encode())
    reply = b''
    while not reply.rstrip().endswith(b'>'):
        chunk = sock.recv(4096)
        if not chunk:
            break
        reply += chunk
    return reply.decode(errors='replace')


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('elf', type=Path, help='the ELF running on the board')
    parser.add_argument('--out', type=Path, default=Path('.'),
                        help='folder for the files (default: here)')
    args = parser.parse_args()
    if not args.elf.is_file():
        parser.error(f'no such file: {args.elf}')
    where = symbols(args.elf)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary, meta = out / 'trace.bin', out / 'metadata.bin'

    if port_open():
        sys.exit('an OpenOCD server is already running (picolab ocd); stop it first')
    server = subprocess.Popen(['picolab', 'ocd'], start_new_session=True,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        sock = connect()
        if sock is None:
            sys.exit('the OpenOCD server did not start; is another picolab ocd running?')
        with sock:
            command(sock, '')    # swallow the banner and first prompt
            command(sock, 'targets rp2350.cm0')
            for path, name, size in ((meta, 'g_sig_trace_metadata', METADATA_BYTES),
                                     (binary, 'g_sig_trace', EVENTS * RECORD)):
                for _ in range(3):
                    reply = command(sock, f'dump_image {path} 0x{where[name]:08x} {size}')
                    if 'dumped' in reply:
                        break
                    time.sleep(0.5)
                if 'dumped' not in reply:
                    sys.exit(f'OpenOCD could not read {name}:\n{reply}')
            # Leave the instrument running whatever state the connection left it in.
            command(sock, 'targets rp2350.cm0')
            command(sock, 'resume')
    finally:
        try:
            os.killpg(os.getpgid(server.pid), 15)
            server.wait(timeout=10)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            pass
        for _ in range(30):         # let the port close before anyone starts the next server
            if not port_open():
                break
            time.sleep(0.2)

    sig = HERE / 'sig_trace.py'
    for suffix, extra in (('jsonl', []), ('csv', ['--csv'])):
        with open(out / f'trace.{suffix}', 'w') as stream:
            subprocess.run([sys.executable, str(sig), 'decode', str(binary),
                            '--metadata', str(meta), '--elf', str(args.elf)] + extra,
                           stdout=stream, check=True)
    print(f'wrote {binary.name}, {meta.name}, trace.jsonl and trace.csv in {args.out}')


if __name__ == '__main__':
    main()
