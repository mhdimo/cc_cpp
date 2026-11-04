#!/usr/bin/env python3
"""Checks that the model is asked after a command of the shell.

A command that starts with `!` runs in the shell of the session. The
command and its output join the conversation, and the model is asked about
them, the way the reference asks it.
"""

import json
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_smoke import BINARY, FAILURES, Session, check, strip  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOCK = os.path.join(ROOT, "tests", "mock_api.py")
PORT = 8247


def main():
    home = tempfile.mkdtemp(prefix="cc-bashturn-")
    project = os.path.realpath(tempfile.mkdtemp(prefix="cc-bashturn-p-"))
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"theme": "dark", "hasCompletedOnboarding": True,
                   "projects": {project: {"hasTrustDialogAccepted": True}}}, handle)

    mock = subprocess.Popen(
        [sys.executable, MOCK, "--port", str(PORT), "--text", "the answer of the model."],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)

    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "test-key",
           "ANTHROPIC_BASE_URL": f"http://127.0.0.1:{PORT}", "CLAUDE_CODE_SCROLLBACK": "1"}

    session = Session(binary=BINARY, env=env, cwd=project)
    session.read(2.5)
    if "trust" in strip(session.output).lower():
        session.send("\r")
        session.read(2.0)

    print("the command of the shell")
    before = len(session.output)
    session.send("!echo hello-from-the-shell\r")
    time.sleep(2.0)
    session.read(1.5)
    screen = strip(session.output[before:])
    check("the command is shown", "! echo hello-from-the-shell" in screen, repr(screen[-400:]))
    check("the output of the command is shown", "hello-from-the-shell" in screen,
          repr(screen[-400:]))

    print("the model is asked")
    check("the model answered", "the answer of the model" in screen, repr(screen[-600:]))

    session.close()
    mock.terminate()

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
