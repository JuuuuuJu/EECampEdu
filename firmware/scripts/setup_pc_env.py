import argparse
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def run(command):
    print(" ".join(str(part) for part in command), flush=True)
    return subprocess.check_call(command, cwd=REPO_ROOT)


def conda_env_exists(env_name):
    result = subprocess.run(
        ["conda", "env", "list"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    for line in result.stdout.splitlines():
        parts = line.split()
        if parts and parts[0] == env_name:
            return True
    return False


def main():
    parser = argparse.ArgumentParser(description="Create/update the Windows conda PC environment.")
    parser.add_argument("--env", default="eecampedu", help="Conda environment name. Default: eecampedu")
    parser.add_argument("--python", default="3.10", help="Python version. Default: 3.10")
    parser.add_argument("--no-create", action="store_true", help="Do not create the conda env if it is missing.")
    args = parser.parse_args()

    if not conda_env_exists(args.env):
        if args.no_create:
            raise RuntimeError(f"Conda environment '{args.env}' does not exist.")
        run(["conda", "create", "-y", "-n", args.env, f"python={args.python}"])

    run(["conda", "run", "--no-capture-output", "-n", args.env, "python", "-m", "pip", "install", "--upgrade", "pip"])
    run(["conda", "run", "--no-capture-output", "-n", args.env, "python", "-m", "pip", "install", "-r", "pc\\requirements.txt"])

    print(f"PC environment ready. Activate with: conda activate {args.env}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
