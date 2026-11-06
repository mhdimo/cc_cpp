#!/usr/bin/env python3
"""Compares the screens of the port with the screens of the reference.

The sweep reads the source of both sides and finds the differences that are
written down in it; it cannot find the differences that are only in the
arrangement — a blank line between two blocks, a column here and there, a
dim word against a grey one. This harness drives the two binaries through the
same steps under a pty and compares the screens they draw, cell by cell.

    python3 tests/compare_screens.py                    # the port against the reference
    python3 tests/compare_screens.py --only banner      # one screen only
    python3 tests/compare_screens.py --show             # the screens themselves

The reference is the Claude Code CLI that is installed on the machine
(`/opt/homebrew/bin/claude`). The two are given the same home directory, the
same keys and the same width, so that the screens can be compared.

The installed CLI is not the tree the port was written from — it is an older
build of the same product, so a few of the screens differ for that reason
alone (the trust dialog of the installed build numbers its choices and the
tree does not, for one). Every difference that is reported is therefore
checked against the tree before it is taken as a divergence of the port.

The example command of the empty prompt box is one of eight that the
reference picks at random, so it is named rather than compared.
"""

import argparse
import difflib
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_smoke import Session  # noqa: E402
from screen import Screen  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = os.path.join(ROOT, "build", "src", "cc++")
REFERENCE = "/opt/homebrew/bin/claude"

COLUMNS = 80
ROWS = 24


def seed(home):
    """Writes the state the reference needs to skip its onboarding.

    The reference shows the screen of the first run when the configuration
    of the user has no theme; the comparison is about the screens that come
    after it.
    """
    import json

    config = os.path.join(home, ".claude.json")
    with open(config, "w") as handle:
        json.dump({"theme": "dark", "hasCompletedOnboarding": True}, handle)


def drive(binary, home, name, steps):
    """Drives a binary through the steps and returns the screens."""
    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "test-key",
           "ANTHROPIC_BASE_URL": "http://127.0.0.1:8123",
           "CLAUDE_CODE_SCROLLBACK": "1", "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC": "1"}

    session = Session(args=(), env=env, binary=binary)
    session.read(2.0)

    screens = {}
    for keys, wait in steps:
        # The screen is what the terminal shows; the bytes from the start
        # of the session are replayed, so that the frames that came before
        # are on the grid as well.
        mark = len(session.output)
        for key in keys:
            session.send(key)
        session.read(wait)
        data = session.output[:mark + len(session.output) - mark]
        if isinstance(data, bytes):
            data = data.decode("utf-8", "replace")
        screen = Screen(COLUMNS, ROWS)
        screen.feed(data)
        screens[name] = screen
    session.close()
    return screens


def prepare(binary):
    """The keys that lead past the trust dialog and into the session.

    The reference asks whether the folder is trusted, with "No, exit" the
    first choice, and then asks whether the API key of the environment may
    be used, with "No (recommended)" the first choice. The port asks the
    first question with "Yes, I trust this folder" as the first choice and
    does not ask the second one.
    """
    if binary == PORT:
        return [("\r", 2.0)]
    return [("\x1b[B", 0.4), ("\r", 1.5), ("\x1b[A", 0.4), ("\r", 3.0)]


def steps_for(name):
    """The keys that lead to each screen."""
    if name == "trust":
        return [("", 0.0)]
    if name == "banner":
        return [("", 2.0)]
    if name == "suggestions":
        return [("/", 1.0)]
    if name == "config":
        return [("/config\r", 2.0)]
    if name == "menu":
        return [("?", 1.5)]
    if name == "transcript":
        return [("\x0f", 1.5)]
    return []


def normalise(text):
    """The screen as it is compared: the volatile parts are named."""
    return re.sub(r'Try "[^"]*"', 'Try "<example>"', text)


def volatile_rows(one, other):
    """The rows that hold the example command.

    The reference picks one of eight example commands at random, so the row
    that holds it is left out of the comparison.
    """
    rows = set()
    for screen in (one, other):
        for index, line in enumerate(screen.text().split("\n")):
            if 'Try "' in line:
                rows.add(index)
    return rows


def compare_cells(one, other):
    """The differences between two screens, as (row, column, a, b)."""
    differences = []
    volatile = volatile_rows(one, other)
    for index, (a, b) in enumerate(zip(one.cells(), other.cells())):
        if a == b:
            continue
        row = index // COLUMNS
        if row in volatile:
            continue
        differences.append((row + 1, index % COLUMNS + 1, a, b))
    return differences


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=PORT)
    parser.add_argument("--reference", default=REFERENCE)
    parser.add_argument("--only", default="")
    parser.add_argument("--show", action="store_true", help="print the screens")
    args = parser.parse_args()

    names = [args.only] if args.only else ["trust", "banner", "suggestions", "config", "menu",
                                           "transcript"]

    home = tempfile.mkdtemp(prefix="cc-compare-")
    seed(home)
    failures = 0

    for name in names:
        steps = steps_for(name)
        if not steps:
            print(f"unknown screen: {name}")
            continue

        # Every screen is reached through the trust dialog, except the
        # screen of the trust dialog itself.
        if name == "trust":
            steps = steps_for(name)
            port_steps = steps_for(name)
        else:
            steps = prepare(args.reference) + steps
            port_steps = prepare(args.port) + steps_for(name)

        reference = drive(args.reference, home, name, steps)[name]
        port = drive(args.port, home, name, port_steps)[name]

        if args.show:
            print(f"=== the reference: {name}")
            print(reference.text())
            print(f"=== the port: {name}")
            print(port.text())

        differences = compare_cells(reference, port)
        if not differences:
            print(f"  ok   {name}")
            continue

        failures += 1
        print(f"  FAIL {name}: {len(differences)} cells differ")

        # The text is compared line by line as well, so that the difference
        # can be read.
        for line in difflib.unified_diff(normalise(reference.text()).split("\n"),
                                         normalise(port.text()).split("\n"),
                                         "the reference", "the port", lineterm=""):
            print("       " + line)

        # The cells that differ, with their attributes.
        for row, column, a, b in differences[:40]:
            print(f"       row {row} column {column}: the reference {a!r}, the port {b!r}")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
