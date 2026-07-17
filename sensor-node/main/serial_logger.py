#!/usr/bin/env python3
"""
Serial logger for the gas_monitor firmware.

Captures CSV lines from the ESP32 over USB-serial and writes them to a file,
prepending a wall-clock ISO timestamp to each row.

Usage:
    python serial_logger.py --port COM5 --out gas_log_20260629.csv
    python serial_logger.py --port /dev/ttyUSB0 --out gas_log.csv

Requires: pip install pyserial

Stop with Ctrl-C. The file is flushed after each line, so partial logs are
always usable.
"""
import argparse
import datetime as dt
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial not installed. Run: pip install pyserial", file=sys.stderr)
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True,
                    help="Serial port, e.g. COM5 or /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", required=True, help="Output CSV path")
    args = ap.parse_args()

    print(f"[logger] Opening {args.port} at {args.baud} baud")
    print(f"[logger] Writing to {args.out}")
    print(f"[logger] Ctrl-C to stop\n")

    header_written = False
    with open(args.out, "w", buffering=1) as f:   # line-buffered
        while True:
            try:
                with serial.Serial(args.port, args.baud, timeout=2) as ser:
                    while True:
                        raw = ser.readline()
                        if not raw:
                            continue
                        try:
                            line = raw.decode("utf-8", errors="replace").strip()
                        except Exception:
                            continue
                        if not line:
                            continue

                        ts = dt.datetime.now().isoformat(timespec="milliseconds")

                        # Pass through comments and header verbatim, prefixed
                        # with the wall-clock for traceability.
                        if line.startswith("#"):
                            print(f"[{ts}] {line}")
                            f.write(f"# {ts} {line[1:].strip()}\n")
                            continue

                        if line.startswith("t_s,"):
                            if not header_written:
                                f.write("wall_iso," + line + "\n")
                                header_written = True
                                print(f"[{ts}] HEADER: {line}")
                            continue

                        # Data row
                        f.write(f"{ts},{line}\n")
                        # Mirror to stdout so you can see it live
                        print(f"[{ts}] {line}")

            except serial.SerialException as e:
                print(f"[logger] Serial error: {e}; retrying in 2 s")
                time.sleep(2)
            except KeyboardInterrupt:
                print("\n[logger] Stopped by user")
                return


if __name__ == "__main__":
    main()
