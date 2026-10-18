#!/usr/bin/env python3
"""Capture the instrument's marker pins with the station's PicoGremlin.

The PicoGremlin is soldered to the back of the Pico 2 W, so its 24 analyzer
channels are already connected to the target GPIOs. You select channels; you do
not add wires. Channel i is GPIO 2+i for i <= 20, then GP26, GP27 and GP28.

The markers are the ones the firmware drives:

    GP16 heartbeat   GP17 display   GP18 DMA ISR   GP21 DSP work

This reads the analyzer's own CDC interface (not the UART bridge) and reports
the width of each marker's pulses, so a pin capture can be compared with the
trace's timestamps. It measures the pins in one recorded window under the
conditions you chose; it is not a worst-case proof and it is not the trace.

Requires pyserial. Usage:

    python3 analyzer.py --rate 1000000 --samples 100000
    python3 analyzer.py --channels 17,18,21 --rate 2000000 --samples 200000
"""
import argparse
import glob
import json
import os
import statistics
import struct
import sys
import time

LA_FRAME_START = bytes([0x55, 0xAA])
LA_FRAME_END = bytes([0xAA, 0x55])
LA_ESC = 0xF0

# Marker GPIO to friendly name, matching the firmware's report.
MARKERS = {16: 'heartbeat', 17: 'display', 18: 'isr', 21: 'work'}
DEFAULT_CHANNELS = [17, 18, 21]
# The analyzer caps samples by channel width: 8-bit 131072, 16-bit 65536,
# 24-bit 32768 (LogicAnalyzer.pio startCaptureSimple). A request above the cap
# is rejected outright with CAPTURE_ERROR.
MODE_MAX_SAMPLES = {0: 131072, 1: 65536, 2: 32768}


def channel_of(gpio):
    """Logic-analyzer channel index for a target GPIO."""
    if 2 <= gpio <= 22:
        return gpio - 2
    if gpio in (26, 27, 28):
        return 21 + (gpio - 26)
    raise ValueError(f'GPIO {gpio} is not an analyzer channel (2-22, 26-28)')


def find_la_port():
    """The analyzer CDC interface, via /dev/serial/by-id."""
    for path in sorted(glob.glob('/dev/serial/by-id/*LogicAnalyzer*-if03')):
        return os.path.realpath(path)
    return None


def la_escape(payload):
    out = bytearray()
    for byte in payload:
        if byte in (0xAA, 0x55, 0xF0):
            out.append(LA_ESC)
            out.append(byte ^ 0xF0)
        else:
            out.append(byte)
    return bytes(out)


def la_frame(command, payload=b''):
    return LA_FRAME_START + bytes([command]) + la_escape(payload) + LA_FRAME_END


def capture_request(channels, frequency, samples, trigger_channel):
    """mode 0/1/2 selects 8/16/24-bit samples; 24-bit covers every channel."""
    mode = 2 if max(channels) >= 16 else (1 if max(channels) >= 8 else 0)
    if samples > MODE_MAX_SAMPLES[mode]:
        raise ValueError(
            f'{samples} samples needs {24 if mode == 2 else 16 if mode == 1 else 8}-bit '
            f'mode, which holds at most {MODE_MAX_SAMPLES[mode]}; request fewer, '
            f'or capture only channels below {16 if mode == 2 else 8}')
    if not 0 <= trigger_channel <= 24:
        raise ValueError('trigger channel must be 0-24')
    mask = channels + [0] * (24 - len(channels))
    payload = struct.pack('<BBBxH24sBxIIIBBBx',
                          0, trigger_channel, 0, 0,    # simple trigger, capture now
                          bytes(mask), len(channels), frequency,
                          0, samples, 0, 0, mode)
    return payload, mode


def capture(port, channels, frequency, samples, trigger_channel, timeout=30.0):
    payload, mode = capture_request(channels, frequency, samples, trigger_channel)
    width = {0: 1, 1: 2, 2: 4}[mode]
    port.reset_input_buffer()
    port.write(la_frame(1, payload))
    port.flush()

    head = b''
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        chunk = port.read(4096)
        if chunk:
            head += chunk
            if b'CAPTURE_STARTED' in head or len(head) >= 4:
                break
    if b'CAPTURE_ERROR' in head or b'ERR_' in head:
        raise RuntimeError(f'analyzer rejected the request: {head[:80]!r}')
    if b'CAPTURE_STARTED' in head:
        head = head[head.index(b'CAPTURE_STARTED') + len(b'CAPTURE_STARTED\n'):]
    while len(head) < 4 and time.monotonic() < deadline:
        head += port.read(4 - len(head))
    if len(head) < 4:
        raise RuntimeError('timed out waiting for the sample count')
    count = struct.unpack('<I', head[0:4])[0]
    if count == 0 or count > 4_000_000:
        raise RuntimeError(f'implausible sample count {count}')
    need = count * width
    body = head[4:]
    while len(body) < need and time.monotonic() < deadline:
        chunk = port.read(min(8192, need - len(body)))
        if chunk:
            body += chunk
    if len(body) < need:
        raise RuntimeError(f'short capture: {len(body)} of {need} bytes')
    if width == 1:
        words = list(body[:count])
    elif width == 2:
        words = list(struct.unpack(f'<{count}H', body[:need]))
    else:
        words = list(struct.unpack(f'<{count}I', body[:need]))
    return words, mode


def pulse_stats(words, channel, dt_us):
    """High-pulse widths and rise-to-rise periods for one channel, in us."""
    bits = [(word >> channel) & 1 for word in words]
    widths, periods, last_rise, first_rise = [], [], None, None
    start = None
    for index, bit in enumerate(bits):
        if bit and start is None:
            start = index
            if first_rise is None:
                first_rise = index
            if last_rise is not None:
                periods.append((index - last_rise) * dt_us)
            last_rise = index
        elif not bit and start is not None:
            widths.append((index - start) * dt_us)
            start = None
    return widths, periods


def summarize(words, channels, frequency):
    dt_us = 1_000_000.0 / frequency
    out = {}
    for gpio in channels:
        widths, periods = pulse_stats(words, channel_of(gpio), dt_us)
        name = MARKERS.get(gpio, f'gp{gpio}')
        entry = {'gpio': gpio, 'pulses': len(widths)}
        if widths:
            entry['high_us'] = {
                'min': round(min(widths), 2),
                'median': round(statistics.median(widths), 2),
                'max': round(max(widths), 2),
            }
        if periods:
            entry['period_us'] = {
                'min': round(min(periods), 2),
                'median': round(statistics.median(periods), 2),
                'max': round(max(periods), 2),
            }
        out[name] = entry
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--channels', default=','.join(str(g) for g in DEFAULT_CHANNELS),
                        help='target GPIO numbers to capture (default: display,isr,work)')
    parser.add_argument('--rate', type=int, default=1_000_000,
                        help='sample rate in Hz (default 1 MHz)')
    parser.add_argument('--samples', type=int, default=32768,
                        help='samples to capture (default 32768, the 24-bit cap)')
    parser.add_argument('--port', default=None, help='override the analyzer port')
    parser.add_argument('--trigger', type=int, default=16,
                        help='GPIO whose rising edge starts the capture '
                             '(default 16, the heartbeat)')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()

    try:
        import serial
    except ImportError:
        parser.error('pyserial is required (pip install pyserial)')
    try:
        channels = [int(value) for value in args.channels.split(',') if value]
    except ValueError:
        parser.error('--channels takes comma-separated GPIO numbers')
    try:
        for gpio in channels:
            channel_of(gpio)
    except ValueError as error:
        parser.error(str(error))

    port_path = args.port or find_la_port()
    if not port_path:
        parser.error('no PicoGremlin analyzer port found (looked for '
                     '/dev/serial/by-id/*LogicAnalyzer*-if03)')

    port = serial.Serial(port_path, 115200, timeout=1.0)
    try:
        # The protocol wants analyzer channel indices, not GPIO numbers.
        indices = [channel_of(gpio) for gpio in channels]
        words, mode = capture(port, indices, args.rate, args.samples,
                              channel_of(args.trigger))
    finally:
        port.close()

    summary = summarize(words, channels, args.rate)
    window_ms = 1000.0 * len(words) / args.rate
    if args.json:
        print(json.dumps({'port': port_path, 'mode': mode, 'samples': len(words),
                          'window_ms': round(window_ms, 2), 'channels': summary},
                         indent=2))
    else:
        print(f'captured {len(words)} samples ({window_ms:.1f} ms) at '
              f'{args.rate} Hz from {port_path}')
        for name, entry in summary.items():
            if 'high_us' in entry:
                high = entry['high_us']
                print(f"  {name:<9} GP{entry['gpio']:<3} {entry['pulses']:>5} pulses, "
                      f"high {high['min']} / {high['median']} / {high['max']} us "
                      f"(min/median/max)")
            else:
                print(f"  {name:<9} GP{entry['gpio']:<3} no pulses in this window")
        print('Pin measurement only: one recorded window, not the trace and not '
              'a worst case.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
