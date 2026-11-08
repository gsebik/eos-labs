#!/usr/bin/env python3
"""Control-room stimulus for the L4 condition monitor (UART transport).

Sends 'q' (one query) or 'b' (burst of ten) and measures the PC-side round trip.
The board measures its own release-to-response span; keep the two clock domains
separate. This is a prototype for the WiFi tool that will follow.
"""
import argparse
import json
import time

import serial


def run(port, baud, mode, count, gap_s):
    rows = []
    expected = 10 if mode == "burst" else 1
    key = b"b" if mode == "burst" else b"q"
    with serial.Serial(port, baud, timeout=0.2) as ser:
        time.sleep(1.0)
        for i in range(count):
            ser.reset_input_buffer()
            t0 = time.perf_counter()
            ser.write(key + b"\n")
            received = 0
            deadline = time.perf_counter() + 3.0
            while received < expected and time.perf_counter() < deadline:
                line = ser.readline().decode(errors="replace").strip()
                if not line.startswith("cmd,"):
                    continue
                fields = dict(token.split("=", 1) for token in line.split(",")[1:]
                              if "=" in token)
                seq = int(fields.get("seq", -1))
                kind = fields.get("type", "")
                ok = kind == mode[0] and seq == received
                rows.append({"burst": i, "seq": seq, "line": line,
                             "pc_rtt_us": round((time.perf_counter() - t0) * 1e6),
                             "matched": ok})
                received += 1
            if received != expected:
                rows.append({"burst": i, "seq": None, "line": "timeout",
                             "pc_rtt_us": None, "matched": False})
            time.sleep(gap_s)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--mode", choices=("query", "burst"), default="query")
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--gap-ms", type=float, default=1000.0)
    ap.add_argument("--json")
    args = ap.parse_args()
    rows = run(args.port, args.baud, args.mode, args.count, args.gap_ms / 1000.0)
    for row in rows:
        print(row["pc_rtt_us"], row["matched"], row["line"])
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(rows, fh, indent=2)
            fh.write("\n")


if __name__ == "__main__":
    main()
