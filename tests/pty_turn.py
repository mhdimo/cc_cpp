#!/usr/bin/env python3
"""Drives the port through a turn that is running.

The keys of the user have to reach the session while the answer is running:
Ctrl+O opens the transcript and Ctrl-C cuts the answer short. The reference
claims the key of the cancel while a task is running, so the answer is
interrupted instead of the session asking for a second press.

    python3 tests/pty_turn.py
"""

import json
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pty_smoke import Session, strip  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.path.join(ROOT, "build", "src", "cc++")
MOCK = os.path.join(ROOT, "tests", "mock_api.py")
PORT = 8242

FAILURES = []


def check(name, condition, detail=""):
    if condition:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name} {detail}")
        FAILURES.append(name)


def start_mock():
    """Starts the mock of the API, with an answer that takes a while."""
    return subprocess.Popen(
        [sys.executable, MOCK, "--port", str(PORT), "--text", "one two three four five six.",
         "--delay", "8"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def main():
    if not os.path.exists(BINARY):
        print(f"binary not found: {BINARY}")
        return 1

    home = tempfile.mkdtemp(prefix="cc-turn-")
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"theme": "dark", "hasCompletedOnboarding": True,
                   "projects": {os.getcwd(): {"hasTrustDialogAccepted": True}}}, handle)

    mock = start_mock()
    time.sleep(1.0)
    if mock.poll() is not None:
        print(f"the mock API did not start on the port {PORT}")
        return 1

    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "test-key",
           "ANTHROPIC_BASE_URL": f"http://127.0.0.1:{PORT}",
           "CLAUDE_CODE_SCROLLBACK": "1"}

    session = Session(env=env)
    try:
        session.read(2.0)

        session.send("tell me a story\r")
        session.read(1.5)

        print("the keys while the answer runs")
        mark = len(session.output)
        session.send("\x0f")  # Ctrl+O
        session.read(1.5)
        screen = strip(session.output[mark:])
        check("ctrl+o opens the transcript", "Showing detailed transcript" in screen,
              repr(screen[-400:]))

        # The same key closes it again: the key is a toggle.
        mark = len(session.output)
        session.send("\x0f")  # Ctrl+O
        session.read(1.0)
        screen = strip(session.output[mark:])
        check("ctrl+o closes the transcript again", "Showing detailed transcript" not in screen,
              repr(screen[-400:]))

        mark = len(session.output)
        session.send("\x03")  # Ctrl+C
        session.read(2.0)
        screen = strip(session.output[mark:])
        # The reference draws the interrupt as "Interrupted · What should
        # Claude do instead?"; the marker of the conversation is never on
        # the screen.
        check("ctrl+c cuts the answer short", "Interrupted" in screen, repr(screen[-600:]))
        check("the turn ends", "one two three" not in screen)
    finally:
        session.close()
        mock.terminate()
        try:
            mock.wait(timeout=3)
        except subprocess.TimeoutExpired:
            mock.kill()

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
