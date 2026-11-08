#!/usr/bin/env python3
"""Control-room stimulus for the L4 condition monitor (UART transport).

Sends 'q' (one query) or 'b' (burst of ten) and measures the PC-side round trip.
With --status, retain the periodic board command counters before sending again.
--raw-log saves every received byte chunk and transmitted request as JSONL with
host-clock timestamps. Board counters are report-window maxima, not reply IDs.
"""
import argparse
from contextlib import ExitStack
import json
import math
from pathlib import Path
import time

import serial


def fields(line):
    return dict(token.split("=", 1) for token in line.split(",")[1:]
                if "=" in token)


def integer(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


class Reader:
    """One UART reader: retain partial lines and log bytes before interpreting."""

    def __init__(self, ser, raw, started):
        self.ser, self.raw, self.started = ser, raw, started
        self.pending = b""
        self.request = None

    def record(self, event, when=None, **data):
        if self.raw is not None:
            now = time.perf_counter() if when is None else when
            item = {"event": event, "host_elapsed_us": round((now - self.started) * 1e6),
                    "request": self.request, **data}
            self.raw.write(json.dumps(item) + "\n")
            self.raw.flush()

    def line(self):
        if b"\n" not in self.pending:
            chunk = self.ser.readline()
            if chunk:
                self.record("rx", bytes_hex=chunk.hex(), text=chunk.decode(errors="replace"))
                self.pending += chunk
        if b"\n" not in self.pending:
            return None
        line, self.pending = self.pending.split(b"\n", 1)
        return line.decode(errors="replace").strip()

    def drain(self, seconds):
        self.request = None
        deadline = time.perf_counter() + seconds
        while time.perf_counter() < deadline:
            self.line()


def run(port, baud, mode, count, gap_s, *, status=False, raw_log=None,
        response_timeout_s=3.0, status_timeout_s=3.0, startup_s=1.0):
    rows = []
    expected = 10 if mode == "burst" else 1
    key = b"b" if mode == "burst" else b"q"
    started = time.perf_counter()
    with ExitStack() as stack:
        raw = stack.enter_context(open(raw_log, "w", encoding="utf-8")) if raw_log else None
        ser = stack.enter_context(serial.Serial(port, baud, timeout=0.05, exclusive=True))
        reader = Reader(ser, raw, started)
        reader.record("open", port=port, baud=baud, mode=mode)
        reader.drain(startup_s)
        for i in range(count):
            reader.request = i
            t0 = time.perf_counter()
            payload = key + b"\n"
            reader.record("tx", when=t0, bytes_hex=payload.hex(), text=payload.decode())
            if ser.write(payload) != len(payload):
                raise serial.SerialTimeoutException("incomplete UART request write")
            received = 0
            deadline = t0 + response_timeout_s
            board_status = []
            status_received = False
            last_row = None
            while time.perf_counter() < deadline:
                line = reader.line()
                if line is None:
                    continue
                if line.startswith("l4,") and status:
                    values = fields(line)
                    snapshot = {"line": line, "host_elapsed_us": round(
                        (time.perf_counter() - started) * 1e6), "fields": values}
                    board_status.append(snapshot)
                    # Only a report after the complete reply set can end the wait.
                    n, span = integer(values.get("cmd_n")), integer(values.get("cmd_max_us"))
                    if received == expected and n is not None and n > 0 and span is not None and span >= 0:
                        status_received = True
                        break
                    continue
                if not line.startswith("cmd,"):
                    continue
                values = fields(line)
                seq = integer(values.get("seq"))
                kind = values.get("type", "")
                ok = received < expected and kind == mode[0] and seq == received
                row = {"burst": i, "seq": seq, "line": line,
                       "pc_rtt_us": round((time.perf_counter() - t0) * 1e6), "matched": ok}
                rows.append(row)
                if ok and received < expected:
                    received += 1
                    last_row = row
                    if received == expected:
                        if not status:
                            break
                        deadline = time.perf_counter() + status_timeout_s
            if received != expected:
                rows.append({"burst": i, "seq": None, "line": "timeout",
                             "pc_rtt_us": None, "matched": False})
                last_row = rows[-1]
                reader.record("response_timeout", received=received, expected=expected)
            if status and last_row is not None:
                last_row["board_status"] = board_status
                last_row["status_received"] = status_received
                last_row["status_scope"] = "periodic report window, not automatically per-request"
                if not status_received:
                    reader.record("status_unavailable")
            # Do not let a late response to a timed-out request impersonate the next one.
            if received != expected or (status and not status_received):
                break
            reader.drain(gap_s)
        reader.record("close")
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--mode", choices=("query", "burst"), default="query")
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--gap-ms", type=float, default=1000.0)
    ap.add_argument("--json")
    ap.add_argument("--status", action="store_true",
                    help="wait for and show the board's periodic command-status report")
    ap.add_argument("--raw-log", help="JSONL of all RX bytes and TX requests with host timestamps")
    ap.add_argument("--response-timeout", type=float, default=3.0, metavar="SECONDS")
    ap.add_argument("--status-timeout", type=float, default=3.0, metavar="SECONDS")
    args = ap.parse_args()
    if args.count < 1 or args.baud < 1 or args.gap_ms < 0:
        ap.error("count and baud must be positive; gap must be non-negative")
    if args.response_timeout <= 0 or args.status_timeout <= 0:
        ap.error("timeouts must be positive")
    if not all(math.isfinite(x) for x in (args.gap_ms, args.response_timeout, args.status_timeout)):
        ap.error("gap and timeouts must be finite")
    if args.raw_log and args.json and Path(args.raw_log).resolve() == Path(args.json).resolve():
        ap.error("--raw-log and --json must name different files")
    try:
        rows = run(args.port, args.baud, args.mode, args.count, args.gap_ms / 1000.0,
                   status=args.status, raw_log=args.raw_log,
                   response_timeout_s=args.response_timeout, status_timeout_s=args.status_timeout)
    except (OSError, serial.SerialException) as exc:
        ap.exit(1, f"UART capture failed: {exc}\n")
    for row in rows:
        print(row["pc_rtt_us"], row["matched"], row["line"])
        for snapshot in row.get("board_status", []):
            print("board", row["burst"], snapshot["host_elapsed_us"], snapshot["line"])
        if row.get("status_received") is False:
            print("board", row["burst"], "unavailable: no command-status report; do not use a zero span")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(rows, fh, indent=2)
            fh.write("\n")
    return int(any(not row["matched"] or row.get("status_received") is False for row in rows))


if __name__ == "__main__":
    raise SystemExit(main())
