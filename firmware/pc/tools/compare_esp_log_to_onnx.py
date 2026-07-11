"""
Compare one ESP benchmark log against PC-side ONNX intermediate stats.

Usage:
    python compare_esp_log_to_onnx.py path/to/pasted-log.txt
    python compare_esp_log_to_onnx.py path/to/pasted-log.txt ./dataset/test/00/01_palm/frame_00_01_0010.png
"""
import glob
import os
import re
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
CNN_DIR = SCRIPT_DIR.parent
DATA_DIR = str(CNN_DIR / "dataset" / "test" / "00")


def parse_esp_first_sample(log_path):
    image_name = None
    layers = {}

    with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            if image_name is None:
                m = re.search(r"PC_DUMP\]?\s+(\S+\.png)", line)
                if m:
                    image_name = m.group(1)

            m = re.search(
                r"LAYER_DUMP,([^,]+),.*?min=(-?\d+),max=(-?\d+),clip=(\d+),sum=(-?\d+),first=(-?\d+)",
                line,
            )
            if m:
                layer = m.group(1)
                if layer not in layers:
                    layers[layer] = {
                        "min": int(m.group(2)),
                        "max": int(m.group(3)),
                        "clip": int(m.group(4)),
                        "sum": int(m.group(5)),
                        "first": int(m.group(6)),
                    }

            if line.startswith("File:"):
                break

    if image_name is None:
        raise SystemExit("Could not find PC_DUMP image name in ESP log.")
    return image_name, layers


def find_image(image_name):
    matches = sorted(glob.glob(os.path.join(DATA_DIR, "**", image_name), recursive=True))
    if not matches:
        raise SystemExit(f"Could not find {image_name} under {DATA_DIR}")
    return matches[0]


def parse_onnx_stats(image_path):
    proc = subprocess.run(
        [sys.executable, str(SCRIPT_DIR / "compare_onnx_layers.py"), image_path],
        check=True,
        capture_output=True,
        text=True,
        cwd=str(CNN_DIR),
    )
    stats = {}
    for line in proc.stdout.splitlines():
        m = re.search(
            r"ONNX_DUMP,([^,]+),.*?min=(-?\d+),max=(-?\d+),clip=(\d+),sum=(-?\d+),first=(-?\d+)",
            line,
        )
        if m:
            stats[m.group(1)] = {
                "min": int(m.group(2)),
                "max": int(m.group(3)),
                "clip": int(m.group(4)),
                "sum": int(m.group(5)),
                "first": int(m.group(6)),
            }
    return stats


def ratio(a, b):
    if b == 0:
        return "inf" if a != 0 else "1.000"
    return f"{a / b:.3f}"


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)

    log_path = sys.argv[1]
    image_path = sys.argv[2] if len(sys.argv) >= 3 else None
    image_name, esp = parse_esp_first_sample(log_path)
    image_path = image_path or find_image(image_name)
    onnx = parse_onnx_stats(image_path)

    print(f"image: {image_path}")
    print(f"{'Layer':<10} {'ESP max':>10} {'ONNX max':>10} {'max ratio':>10} {'ESP sum':>14} {'ONNX sum':>14} {'sum ratio':>10}")
    print("-" * 86)
    for layer in ["l1", "l2", "l3", "l4", "l5", "l6", "flatten", "l7", "l8"]:
        if layer not in esp or layer not in onnx:
            continue
        print(
            f"{layer:<10} "
            f"{esp[layer]['max']:>10} {onnx[layer]['max']:>10} {ratio(esp[layer]['max'], onnx[layer]['max']):>10} "
            f"{esp[layer]['sum']:>14} {onnx[layer]['sum']:>14} {ratio(esp[layer]['sum'], onnx[layer]['sum']):>10}"
        )


if __name__ == "__main__":
    main()
