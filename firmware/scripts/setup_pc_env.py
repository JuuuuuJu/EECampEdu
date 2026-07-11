import runpy
from pathlib import Path


if __name__ == "__main__":
    root_setup = Path(__file__).resolve().parents[2] / "scripts" / "setup_env.py"
    runpy.run_path(str(root_setup), run_name="__main__")