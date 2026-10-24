import json
import os

PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "keys", "keys.json")


def key(name):
    try:
        with open(PATH) as f:
            return bytes.fromhex(json.load(f)[name])
    except (OSError, KeyError, ValueError):
        raise SystemExit("missing key %s in %s" % (name, os.path.normpath(PATH)))
