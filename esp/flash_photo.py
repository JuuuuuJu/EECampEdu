import argparse
import csv
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image


ESP_DIR = Path(__file__).resolve().parent
PARTITIONS_CSV = ESP_DIR / "partitions.csv"
DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = "460800"
FRAME_WIDTH = 160
FRAME_HEIGHT = 160
PHOTO_MAGIC = 0x45454350  # "EECP"
PHOTO_VERSION = 1
PHOTO_FORMAT_GRAYSCALE = 0


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


def read_partition(name):
    with PARTITIONS_CSV.open(newline="", encoding="utf-8") as file:
        rows = csv.reader(line for line in file if not line.lstrip().startswith("#"))
        for row in rows:
            if len(row) < 5:
                continue
            if row[0].strip() != name:
                continue
            offset = int(row[3].strip(), 0)
            size = parse_size(row[4].strip())
            return offset, size
    raise RuntimeError(f"{name} partition not found in {PARTITIONS_CSV}")


def build_photo_blob(image_path, width, height):
    image = Image.open(image_path).convert("L").resize((width, height))
    payload = image.tobytes()
    header = struct.pack(
        "<IIIHHB7x",
        PHOTO_MAGIC,
        PHOTO_VERSION,
        len(payload),
        width,
        height,
        PHOTO_FORMAT_GRAYSCALE,
    )
    return header + payload


def main():
    parser = argparse.ArgumentParser(
        description="Convert an image to the firmware photo format and flash it into the photos partition."
    )
    parser.add_argument("image", help="Input image. Any Pillow-readable format is accepted.")
    parser.add_argument("-p", "--port", default=DEFAULT_PORT, help=f"Serial port. Default: {DEFAULT_PORT}")
    parser.add_argument("-b", "--baud", default=DEFAULT_BAUD, help=f"Baud rate. Default: {DEFAULT_BAUD}")
    parser.add_argument("--chip", default="esp32s3", help="ESP chip name. Default: esp32s3")
    parser.add_argument("--width", type=int, default=FRAME_WIDTH, help=f"Stored grayscale width. Default: {FRAME_WIDTH}")
    parser.add_argument("--height", type=int, default=FRAME_HEIGHT, help=f"Stored grayscale height. Default: {FRAME_HEIGHT}")
    parser.add_argument("--output-bin", help="Write the generated photo blob to this file instead of a temp file.")
    parser.add_argument("--dry-run", action="store_true", help="Print the esptool command without running it.")
    args = parser.parse_args()

    image_path = Path(args.image).expanduser().resolve()
    if not image_path.exists():
        raise FileNotFoundError(image_path)

    offset, partition_size = read_partition("photos")
    blob = build_photo_blob(image_path, args.width, args.height)
    if len(blob) > partition_size:
        raise RuntimeError(
            f"Generated photo blob is {len(blob)} bytes, larger than photos partition {partition_size} bytes."
        )

    if args.output_bin:
        blob_path = Path(args.output_bin).expanduser().resolve()
        blob_path.write_bytes(blob)
        cleanup = None
    else:
        temp = tempfile.NamedTemporaryFile(prefix="eecamp_photo_", suffix=".bin", delete=False)
        temp.write(blob)
        temp.close()
        blob_path = Path(temp.name)
        cleanup = blob_path

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
        str(blob_path),
    ]

    print(f"Photos partition offset: {hex(offset)}")
    print(f"Photos partition size  : {partition_size} bytes")
    print(f"Generated photo blob   : {len(blob)} bytes")
    print(f"Stored frame           : {args.width}x{args.height} grayscale")
    print("Command:")
    print(" ".join(cmd))

    try:
        if args.dry_run:
            return 0
        return subprocess.call(cmd)
    finally:
        if cleanup is not None:
            try:
                cleanup.unlink()
            except FileNotFoundError:
                pass


if __name__ == "__main__":
    sys.exit(main())
