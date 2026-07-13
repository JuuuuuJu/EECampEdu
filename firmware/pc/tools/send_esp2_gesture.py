import argparse
import time

import serial

GESTURES = {
    "0": (0, "up"),
    "up": (0, "up"),
    "1": (1, "down"),
    "down": (1, "down"),
    "2": (2, "right"),
    "right": (2, "right"),
    "3": (3, "left"),
    "left": (3, "left"),
    "4": (4, "null"),
    "null": (4, "null"),
    "none": (4, "null"),
}


def build_command(text):
    key = text.strip().lower()
    if key in GESTURES:
        idx, name = GESTURES[key]
        return f"GESTURE,{idx},{name}"
    return text.strip()


def main():
    parser = argparse.ArgumentParser(description="Send one gesture/manual servo command to the ESP2 output controller.")
    parser.add_argument("command", help="Gesture name/index or manual command such as B90, A90, P100, C30.")
    parser.add_argument("--port", "-p", required=True, help="Serial port connected to ESP2, for example COM7.")
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
            ack = ser.readline().decode("utf-8", errors="ignore").strip()
            print(f"sent: {command}")
            print(f"recv: {ack if ack else '(no ack)'}")
            if index + 1 < args.repeat:
                time.sleep(args.delay)


if __name__ == "__main__":
    main()
