#!/usr/bin/env python3
"""Checks that the session compacts itself when the context is full.

The reference compacts the conversation when the context window is nearly
full: the model writes a summary of the conversation and the conversation
becomes that summary and the last few messages. The size of the context is
the size of the last call of the model, not the sum of every call.
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
PORT = 8263


def main():
    home = tempfile.mkdtemp(prefix="cc-autocompact-")
    project = os.path.realpath(tempfile.mkdtemp(prefix="cc-autocompact-p-"))
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"theme": "dark", "hasCompletedOnboarding": True,
                   "projects": {project: {"hasTrustDialogAccepted": True}}}, handle)

    # The context of the session is nearly full: 190000 of the 200000 the
    # window holds.
    mock = subprocess.Popen(
        [sys.executable, MOCK, "--port", str(PORT), "--text", "the answer of the model.",
         "--input-tokens", "190000"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)

    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "test-key",
           "ANTHROPIC_BASE_URL": f"http://127.0.0.1:{PORT}", "CLAUDE_CODE_SCROLLBACK": "1"}

    session = Session(binary=BINARY, env=env, cwd=project)
    session.read(2.5)
    if "trust" in strip(session.output).lower():
        session.send("\r")
        session.read(2.0)

    print("the session compacts itself")
    before = len(session.output)
    session.send("hello\r")
    time.sleep(2.0)
    session.read(1.5)
    screen = strip(session.output[before:])
    check("the session says that it compacted the conversation",
          "compacted" in screen.lower(), repr(screen[-600:]))
    check("the answer of the model is there", "the answer of the model" in screen,
          repr(screen[-600:]))

    session.close()
    mock.terminate()

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
