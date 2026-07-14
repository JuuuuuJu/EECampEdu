import argparse
import re
import sys
import time

import serial


RESULT_RE = re.compile(rb"USB_CDC_BENCH_RESULT,[^\r\n]*")
STATUS_RE = re.compile(rb"USB_CDC_BENCH_(?:BEGIN|PROGRESS),[^\r\n]*")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run the ESP32-S3 USB CDC timed throughput benchmark and discard payload bytes."
    )
    parser.add_argument("--port", required=True, help="USB CDC serial port, for example COM6.")
    parser.add_argument("--baud", type=int, default=115200, help="CDC baud value. USB CDC mostly ignores this.")
    parser.add_argument("--seconds", type=int, default=600, help="Benchmark duration in seconds. Default: 600.")
    parser.add_argument("--timeout-extra", type=int, default=45, help="Extra seconds to wait after the requested duration.")
    parser.add_argument(
        "--progress-interval",
        type=float,
        default=1.0,
        help="Host-side progress refresh interval in seconds. Use 0 to disable. Default: 1.0.",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    command = f"T{args.seconds}\n".encode("ascii")
    deadline = time.monotonic() + args.seconds + args.timeout_extra
    bytes_read = 0
    buffer = b""
    printed = set()
    start_time = None
    next_host_progress = 0.0
    progress_line_active = False

    print(f"Opening {args.port} @ {args.baud}...")
    with serial.Serial(args.port, args.baud, timeout=0.2, write_timeout=2.0) as ser:
        time.sleep(0.5)
        ser.reset_input_buffer()
        ser.write(command)
        ser.flush()
        print(f"Sent command: {command.decode().strip()}")
        start_time = time.monotonic()
        next_host_progress = start_time

        while time.monotonic() < deadline:
            chunk = ser.read(16384)
            now = time.monotonic()
            if not chunk:
                if (
                    args.progress_interval > 0
                    and start_time is not None
                    and now >= next_host_progress
                ):
                    elapsed = max(now - start_time, 1e-9)
                    host_kbps = (bytes_read / 1024.0) / elapsed
                    print(
                        f"\rHost progress: elapsed={elapsed:7.1f}s "
                        f"received={bytes_read / (1024 * 1024):8.2f} MiB "
                        f"avg={host_kbps:8.2f} KiB/s",
                        end="",
                        flush=True,
                    )
                    progress_line_active = True
                    next_host_progress = now + args.progress_interval
                continue

            bytes_read += len(chunk)
            buffer += chunk
            if len(buffer) > 8192:
                buffer = buffer[-8192:]

            if args.progress_interval > 0 and start_time is not None and now >= next_host_progress:
                elapsed = max(now - start_time, 1e-9)
                host_kbps = (bytes_read / 1024.0) / elapsed
                print(
                    f"\rHost progress: elapsed={elapsed:7.1f}s "
                    f"received={bytes_read / (1024 * 1024):8.2f} MiB "
                    f"avg={host_kbps:8.2f} KiB/s",
                    end="",
                    flush=True,
                )
                progress_line_active = True
                next_host_progress = now + args.progress_interval

            for match in STATUS_RE.finditer(buffer):
                raw_line = match.group(0)
                if raw_line in printed:
                    continue
                printed.add(raw_line)
                if progress_line_active:
                    print()
                    progress_line_active = False
                print(raw_line.decode("ascii", errors="replace"))

            result = RESULT_RE.search(buffer)
            if result:
                line = result.group(0).decode("ascii", errors="replace")
                if progress_line_active:
                    print()
                    progress_line_active = False
                print(line)
                print(f"Host bytes read: {bytes_read}")
                return 0

    if progress_line_active:
        print()
    print("Timed out before USB_CDC_BENCH_RESULT was received.")
    print(f"Host bytes read before timeout: {bytes_read}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
