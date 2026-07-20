import argparse
import time

import serial

ACTIONS = {
    "up": "up",
    "down": "down",
    "left": "left",
    "right": "right",
    "clamp": "clamp",
    "close": "clamp",
    "grab": "clamp",
    "release": "release",
    "open": "release",
    "none": "none",
    "null": "none",
}

LEGACY_GESTURES = {
    "0": (0, "up"),
    "1": (1, "down"),
    "2": (2, "right"),
    "3": (3, "left"),
    "4": (4, "null"),
}


def build_command(text):
    key = text.strip().lower()
    if key in ACTIONS:
        return f"ACTION,{ACTIONS[key]}"
    if key in LEGACY_GESTURES:
        idx, name = LEGACY_GESTURES[key]
        return f"GESTURE,{idx},{name}"
    return text.strip()


def main():
    parser = argparse.ArgumentParser(description="Send one output action/manual servo command to the control board output controller.")
    parser.add_argument("command", help="Output action such as up/down/left/right/clamp/release/none, legacy gesture index 0-4, or manual servo command such as B90, A90, P100, C30.")
    parser.add_argument("--port", "-p", required=True, help="Serial port connected to control board, for example COM7.")
    parser.add_argument("--baudrate", "--baud", type=int, default=115200)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--delay", type=float, default=0.25, help="Delay between repeated commands in seconds.")
    parser.add_argument("--timeout", type=float, default=1.0)
    args = parser.parse_args()

    command = build_command(args.command)
    with serial.Serial(args.port, args.baudrate, timeout=args.timeout, write_timeout=args.timeout) as ser:
        time.sleep(2.0)
        ser.reset_input_buffer()
        for index in range(args.repeat):
            ser.write((command + "\n").encode("ascii"))
            ser.flush()
            print(f"sent: {command}")

            if command.strip().lower() in {"test", "sweep"}:
                deadline = time.time() + max(args.timeout, 5.0)
                got_any = False
                while time.time() < deadline:
                    line = ser.readline().decode("utf-8", errors="ignore").strip()
                    if not line:
                        continue
                    got_any = True
                    print(f"recv: {line}")
                    if line.startswith("OK_TEST") or line.startswith("TEST_DONE"):
                        if line.startswith("OK_TEST"):
                            break
                if not got_any:
                    print("recv: (no ack)")
            else:
                ack = ser.readline().decode("utf-8", errors="ignore").strip()
                print(f"recv: {ack if ack else '(no ack)'}")

            if index + 1 < args.repeat:
                time.sleep(args.delay)


if __name__ == "__main__":
    main()
