#!/usr/bin/env python3
"""Drives the auto mode through a pty and checks what the classifier does.

The auto mode asks a model of its own whether a call may run without the
user. The mock of the API answers that question, so that the test can see
whether the user is interrupted or not.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_smoke import Session, strip  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOCK = os.path.join(ROOT, "tests", "mock_api.py")

FAILURES = []


def check(name, condition, detail=""):
    if condition:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name} {detail}")
        FAILURES.append(name)


def run(block):
    """Drives a session whose classifier blocks or allows the call."""
    home = tempfile.mkdtemp(prefix="cc-auto-")
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"theme": "dark", "hasCompletedOnboarding": True,
                   "projects": {os.getcwd(): {"hasTrustDialogAccepted": True}}}, handle)

    port = 8261 if block else 8262
    args = [sys.executable, MOCK, "--port", str(port), "--text", "done.",
            "--tool", "Bash", "--tool-input", json.dumps({"command": "echo hello"})]
    if block:
        args.append("--classify-block")
    mock = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)

    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "test-key",
           "ANTHROPIC_BASE_URL": f"http://127.0.0.1:{port}",
           "CLAUDE_CODE_SCROLLBACK": "1"}

    session = Session(env=env)
    try:
        session.read(2.0)
        session.send("run it\r")
        session.read(4.0)
        screen = strip(session.output)

        # The classifier answers first, and the user is only interrupted
        # when the classifier says that the call is to be blocked.
        if block:
            check("the call is refused", "Permission for this action has been denied" in screen,
                  repr(screen[-800:]))
            check("the reason of the classifier is named",
                  "The action leaves the folder of the session." in screen, repr(screen[-800:]))
            check("the tool did not run", "⎿  hello" not in screen, repr(screen[-800:]))
        else:
            check("the user is not interrupted", "Do you want to proceed" not in screen,
                  repr(screen[-800:]))
            check("the tool ran", "⎿  hello" in screen, repr(screen[-800:]))
    finally:
        session.close()
        mock.terminate()
        shutil.rmtree(home, ignore_errors=True)


def main():
    print("the classifier allows the action")
    run(block=False)

    print("the classifier blocks the action")
    run(block=True)

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
