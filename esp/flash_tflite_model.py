import argparse
import csv
import re
import subprocess
import sys
from pathlib import Path


ESP_DIR = Path(__file__).resolve().parent
PARTITIONS_CSV = ESP_DIR / "partitions.csv"
DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = "460800"


def parse_size(text):
    value = text.strip().lower()
    match = re.fullmatch(r"(\d+)([km]?)", value)
    if not match:
        return int(value, 0)
    number = int(match.group(1))
    suffix = match.group(2)
    if suffix == "k":
        return number * 1024
    if suffix == "m":
        return number * 1024 * 1024
    return number


def read_model_partition():
    with PARTITIONS_CSV.open(newline="", encoding="utf-8") as file:
        rows = csv.reader(line for line in file if not line.lstrip().startswith("#"))
        for row in rows:
            if len(row) < 5:
                continue
            name = row[0].strip()
            if name != "model":
                continue
            offset = int(row[3].strip(), 0)
            size = parse_size(row[4].strip())
            return offset, size
    raise RuntimeError(f"model partition not found in {PARTITIONS_CSV}")


def main():
    parser = argparse.ArgumentParser(description="Flash a TFLite model into the ESP32 model partition.")
    parser.add_argument("model", help="Path to .tflite model file.")
    parser.add_argument("-p", "--port", default=DEFAULT_PORT, help=f"Serial port. Default: {DEFAULT_PORT}")
    parser.add_argument("-b", "--baud", default=DEFAULT_BAUD, help=f"Baud rate. Default: {DEFAULT_BAUD}")
    parser.add_argument("--chip", default="esp32s3", help="ESP chip name. Default: esp32s3")
    parser.add_argument("--dry-run", action="store_true", help="Print the esptool command without running it.")
    args = parser.parse_args()

    model_path = Path(args.model).expanduser().resolve()
    if not model_path.exists():
        raise FileNotFoundError(model_path)

    offset, partition_size = read_model_partition()
    model_size = model_path.stat().st_size
    if model_size > partition_size:
        raise RuntimeError(
            f"{model_path.name} is {model_size} bytes, larger than model partition "
            f"{partition_size} bytes."
        )

    cmd = [
        "esptool.py",
        "--chip",
        args.chip,
        "-p",
        args.port,
        "-b",
        args.baud,
        "write_flash",
        hex(offset),
        str(model_path),
    ]

    print(f"Model partition offset: {hex(offset)}")
    print(f"Model partition size  : {partition_size} bytes")
    print(f"Model file size       : {model_size} bytes")
    print("Command:")
    print(" ".join(cmd))

    if args.dry_run:
        return 0

    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
