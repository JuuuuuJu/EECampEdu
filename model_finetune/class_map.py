"""Shared gesture class-order / output-action mapping.

The camp lets students upload their *own* six gesture folders with arbitrary
names (e.g. ``n1..n6`` instead of ``up/ok/thumb/palm/rock/stone``). To keep the
whole pipeline (train -> quantize -> benchmark -> robot output) consistent, the
class *order* is recorded once in a single JSON file and every stage reads it.

Canonical file (generated at dataset-upload time, git-ignored):

    model_finetune/dataset/class_mapping.json

Schema (see model_finetune/class_mapping.example.json)::

    {
      "version": 1,
      "class_order": ["n1", "n2", "n3", "n4", "n5", "n6"],
      "num_classes": 6,
      "classes": [
        {"index": 0, "name": "n1", "action": "up"},
        {"index": 1, "name": "n2", "action": "down"},
        ...
      ]
    }

Design notes:
  * The model output is purely an index; ``class_order[i]`` is the human label of
    output index ``i``. The ESP firmware never needs the names — only the index
    and scores (see firmware/esp app_main.cpp RESULT line).
  * ``action`` maps each trained class to one robot-arm output action. The valid
    actions match the ESP2 output firmware's index-driven table
    (firmware/esp2_output/main/app_main.c): up/down/left/right/clamp/release.
  * Every consumer falls back to a caller-supplied default when the file is
    missing or malformed, so existing behaviour is preserved with no dataset.
"""

import json
from pathlib import Path

MODEL_FINETUNE_DIR = Path(__file__).resolve().parent
DATASET_DIR = MODEL_FINETUNE_DIR / "dataset"
CLASS_MAP_PATH = DATASET_DIR / "class_mapping.json"

# Exactly six gesture classes are expected for this project.
NUM_CLASSES = 6

# Robot-arm output actions a class may be mapped to. Order/index here matches the
# ESP2 output firmware apply_action() table so the student PC control app can send
# either the action name or its index.
OUTPUT_ACTIONS = ["up", "down", "left", "right", "clamp", "release"]


def _read(path=CLASS_MAP_PATH):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def _valid_order(value):
    if not isinstance(value, list) or not value:
        return None
    order = [str(x) for x in value if isinstance(x, str) and str(x).strip()]
    return order or None


def load_class_order(default=None, path=CLASS_MAP_PATH):
    """Return the saved class order, or ``default`` if unavailable.

    ``default`` lets each script keep its historical class list when no
    class_mapping.json has been produced yet (e.g. the 6-class MobileNet default or
    the 4-class Mini-ResNet default).
    """
    data = _read(path)
    if data is not None:
        order = _valid_order(data.get("class_order"))
        if order:
            return order
        # tolerate a bare {"classes": [{"index":..,"name":..}]} form
        classes = data.get("classes")
        if isinstance(classes, list) and classes:
            try:
                ordered = sorted(classes, key=lambda c: int(c.get("index", 0)))
                names = _valid_order([c.get("name") for c in ordered])
                if names:
                    return names
            except (TypeError, ValueError):
                pass
    return list(default) if default is not None else None


def load_class_map(default_order=None, path=CLASS_MAP_PATH):
    """Return the full mapping dict, synthesising one from ``default_order`` if needed."""
    data = _read(path)
    order = load_class_order(default=default_order, path=path)
    if order is None:
        return None
    actions = {}
    if isinstance(data, dict):
        for entry in data.get("classes", []) or []:
            name = entry.get("name")
            if name is not None and entry.get("action"):
                actions[str(name)] = entry["action"]
    return _build(order, actions)


def _build(class_order, actions=None):
    actions = actions or {}
    classes = []
    for index, name in enumerate(class_order):
        action = actions.get(name) or actions.get(str(index))
        classes.append({"index": index, "name": name, "action": action})
    return {
        "version": 1,
        "class_order": list(class_order),
        "num_classes": len(class_order),
        "classes": classes,
    }


def save_class_map(class_order, actions=None, path=CLASS_MAP_PATH):
    """Write class_mapping.json for ``class_order`` (list of names) + optional actions dict."""
    payload = _build(class_order, actions)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return payload


if __name__ == "__main__":
    # Small CLI: print the resolved class order (for debugging / scripts).
    print(json.dumps(load_class_map(default_order=None) or {"class_order": None}, indent=2))
