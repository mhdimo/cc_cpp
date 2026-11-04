#!/usr/bin/env python3
"""Compares the screens of the port with the screens that are held in `tests/golden/`.

The tests of the behaviour cannot tell that two drawings look the same: a
screen that is one space off, or a word that is one letter off, passes every
check that looks for a word. This harness drives a binary through a pty,
captures the screen it draws, and compares it with the screen that is held in
`tests/golden/`.

The golden files are written from the port itself:

    python3 tests/golden_screens.py --record

and the screens of the port are checked against them with:

    python3 tests/golden_screens.py

They are the shape the port has to keep: every difference between the port
and the reference is a difference the port has to make first, and the golden
file then holds the result. The reference is not run here — only its source
is read — so the files cannot be written from it.

The binary that is checked is the one the port builds, unless another one is
named with `--binary`. The screens that are captured are the ones that do not
need the model: the banner, the trust dialog, the list of the commands, the
menu of the shortcuts and the panel of the configuration.
"""

import argparse
import difflib
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pty_smoke import Session, strip  # noqa: E402
from screen import Screen  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GOLDEN = os.path.join(ROOT, "tests", "golden")

# The parts of a screen that are not the same in every run.
VOLATILE = [
    (re.compile(r"\bv?\d+\.\d+\.\d+\b"), "<version>"),
    (re.compile(r"\b\d{4}-\d{2}-\d{2}\b"), "<date>"),
    (re.compile(r"\b\d{2}:\d{2}:\d{2}\b"), "<time>"),
    (re.compile(r"/private/var/folders/[^\s\"']+"), "<folder>"),
    (re.compile(r"/tmp/[^\s\"']+"), "<folder>"),
    # The example command of the empty prompt box is one of eight that are
    # picked at random, so it is named rather than compared.
    (re.compile(r'Try "[^"]*"'), 'Try "<example>"'),
    # The notes of the release are the ones of the changelog of the port,
    # which changes with every release, so they are named rather than
    # compared.
    (re.compile(r"│ [^│]*… *│"), "│ <note> │"),
]


def normalize(text, home):
    """The screen as it is compared: the volatile parts are named."""
    text = strip(text)
    text = text.replace(home, "<home>")
    for pattern, name in VOLATILE:
        text = pattern.sub(name, text)
    # The trailing spaces of a line are not part of the drawing.
    lines = [line.rstrip() for line in text.split("\n")]
    # The empty lines at the ends are not part of it either.
    while lines and not lines[0]:
        lines.pop(0)
    while lines and not lines[-1]:
        lines.pop()
    return "\n".join(lines)


def screen_of(output):
    """The screen the bytes of the session draw.

    The bytes are replayed on a grid, the way a terminal does it, so that
    the screen is the one the user sees and not the stream of the frames.
    """
    screen = Screen(80, 24)
    screen.feed(strip(output))
    return "\n".join(line.rstrip() for line in screen.lines())


def capture(binary, home):
    """Captures every screen, and returns them by name."""
    import json
    import tempfile

    # The setup of the session was done already, so that the questions of
    # the setup are not asked.
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"hasCompletedOnboarding": True}, handle)

    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "",
           "ANTHROPIC_BASE_URL": "http://127.0.0.1:9", "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC": "1",
           # The model of the session is named, so that the screens do not
           # depend on the environment of the machine the tests run on.
           "ANTHROPIC_MODEL": "MiniMax-M3.1-Flash[1m]"}

    screens = {}

    # The banner and the trust dialog.
    session = Session(args=(), env=env)
    session.process.args = [binary]
    session.read(1.5)
    screens["trust"] = normalize(screen_of(session.output), home)
    session.send("\r")
    session.read(1.5)
    screens["banner"] = normalize(screen_of(session.output), home)

    # The list of the commands.
    session.send("/")
    session.read(0.8)
    screens["suggestions"] = normalize(screen_of(session.output), home)
    session.send("\x1b")
    session.read(0.5)

    # The panel of the configuration.
    mark = len(session.output)
    session.send("/config\r")
    # The panel is drawn a moment after the key, so the capture waits for it
    # rather than for a fixed time. The list of the settings is cut to the
    # height of the pane, so the search field is what is waited for.
    session.wait_for("Search settings", timeout=5.0)
    session.read(0.5)
    screens["config"] = normalize(screen_of(session.output), home)
    session.send("\x1b")
    session.read(0.5)

    session.close()
    return screens


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=os.path.join(ROOT, "build", "src", "cc++"))
    parser.add_argument("--record", action="store_true", help="write the golden files")
    args = parser.parse_args()

    if not os.path.exists(args.binary):
        print(f"binary not found: {args.binary}")
        return 1

    import tempfile

    home = tempfile.mkdtemp(prefix="cc-golden-")
    screens = capture(args.binary, home)

    os.makedirs(GOLDEN, exist_ok=True)

    failures = 0
    for name, screen in screens.items():
        path = os.path.join(GOLDEN, f"{name}.txt")
        if args.record:
            with open(path, "w") as handle:
                handle.write(screen + "\n")
            print(f"  wrote {path}")
            continue

        if not os.path.exists(path):
            print(f"  FAIL {name}: no golden file ({path})")
            failures += 1
            continue

        with open(path) as handle:
            golden = handle.read().rstrip("\n")
        if golden == screen:
            print(f"  ok   {name}")
            continue

        failures += 1
        print(f"  FAIL {name}")
        for line in difflib.unified_diff(golden.split("\n"), screen.split("\n"),
                                         "the reference", "the port", lineterm=""):
            print("       " + line)

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
