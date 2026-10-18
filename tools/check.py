#!/usr/bin/env python3
"""Validate EOS @@CHECK records against inclusive TOML bounds."""

from __future__ import annotations

import argparse
import re
import sys
import tomllib
from pathlib import Path

RECORD = re.compile(r"^@@CHECK\s+(\S+)\s+(PASS|FAIL)\s+(-?\d+)\s+(\S+)\s*$")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("expectations", type=Path, help="check.toml")
    parser.add_argument("log", type=Path, help="captured serial/RTT output")
    args = parser.parse_args()

    expected = tomllib.loads(args.expectations.read_text())
    records: dict[str, tuple[str, int, str]] = {}
    for line in args.log.read_text(errors="replace").splitlines():
        match = RECORD.match(line.strip())
        if match:
            name, status, value, unit = match.groups()
            records[name] = (status, int(value), unit)

    failures = 0
    for name, rule in expected.get("check", {}).items():
        record = records.get(name)
        if record is None:
            print(f"FAIL {name}: no @@CHECK record")
            failures += 1
            continue
        status, value, unit = record
        low, high = rule["min"], rule["max"]
        ok = status == "PASS" and unit == rule["unit"] and low <= value <= high
        print(f"{'PASS' if ok else 'FAIL'} {name}: {value} {unit} (expected {low}..{high} {rule['unit']})")
        failures += not ok

    unexpected = records.keys() - expected.get("check", {}).keys()
    for name in sorted(unexpected):
        print(f"NOTE {name}: no range configured; record retained")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
