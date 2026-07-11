import argparse
import os
import stat
import shutil
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
APP_DIR = REPO_ROOT / "apps" / "esp32_cam_input_app"
BUILD_DIR = APP_DIR / "build"


def run(command, *, shell=False):
    print(command if isinstance(command, str) else " ".join(str(x) for x in command), flush=True)
    return subprocess.check_call(command, cwd=REPO_ROOT, shell=shell)


def remove_tree(path):
    def make_writable_and_retry(func, failed_path, exc_info):
        try:
            os.chmod(failed_path, stat.S_IWRITE | stat.S_IREAD)
            func(failed_path)
        except Exception:
            raise exc_info[1]

    shutil.rmtree(path, onerror=make_writable_and_retry)


def find_vswhere():
    candidates = [
        Path(os.environ.get("ProgramFiles(x86)", "")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe",
        Path(os.environ.get("ProgramFiles", "")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def find_vcvars64():
    vswhere = find_vswhere()
    if not vswhere:
        return None
    result = subprocess.run(
        [
            str(vswhere),
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property",
            "installationPath",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    install_path = result.stdout.strip().splitlines()[0] if result.stdout.strip() else ""
    if not install_path:
        return None
    vcvars = Path(install_path) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    return vcvars if vcvars.exists() else None


def main():
    parser = argparse.ArgumentParser(description="Build the EECampEdu Dear ImGui input demo with a 64-bit Windows compiler.")
    parser.add_argument("--clean", action="store_true", help="Delete the app build directory before configuring.")
    parser.add_argument("--config", default="Release", help="Build configuration. Default: Release")
    args = parser.parse_args()

    if args.clean and BUILD_DIR.exists():
        remove_tree(BUILD_DIR)

    vcvars64 = find_vcvars64()
    if not vcvars64:
        print("[ERROR] Visual Studio Build Tools x64 C++ compiler was not found.", file=sys.stderr)
        print("Install Visual Studio Build Tools with 'Desktop development with C++',", file=sys.stderr)
        print("or run CMake manually from an x64 Native Tools Command Prompt.", file=sys.stderr)
        print("Do not use C:/MinGW here: conda SDL3 is 64-bit and the old MinGW on PATH is incompatible.", file=sys.stderr)
        return 2

    configure = (
        f'call "{vcvars64}" && '
        f'cmake -S "{APP_DIR}" -B "{BUILD_DIR}" -G Ninja '
        f'-DCMAKE_BUILD_TYPE={args.config}'
    )
    build = (
        f'call "{vcvars64}" && '
        f'cmake --build "{BUILD_DIR}" --config {args.config}'
    )

    run(f'cmd /s /c "{configure}"', shell=True)
    run(f'cmd /s /c "{build}"', shell=True)
    print(f"Built: {BUILD_DIR / 'eecampedu_input_demo.exe'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())