#!/usr/bin/env python3
"""Drives the goal of the session through a pty and checks what it says.

The goal is set with `/goal`, and it is taken away with `/goal clear`.
"""

import atexit
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_smoke import Session, strip  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FAILURES = []


def check(name, condition, detail=""):
    if condition:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name} {detail}")
        FAILURES.append(name)


def main():
    home = tempfile.mkdtemp(prefix="cc-goal-")
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"hasCompletedOnboarding": True}, handle)

    env = {**os.environ, "HOME": home,
           "ANTHROPIC_API_KEY": "sk-ant-0123456789abcdefghijklmnopqrstuvwxyz",
           "ANTHROPIC_BASE_URL": "http://127.0.0.1:9",
           "CLAUDE_CODE_SCROLLBACK": "1"}

    session = Session(env=env)
    try:
        session.read(2.5)
        session.send("\r")  # The folder is trusted.
        session.read(1.0)

        print("no goal")
        mark = len(session.output)
        session.send("/goal\r")
        session.read(1.0)
        screen = strip(session.output[mark:])
        check("the usage is said", "No goal set" in screen, repr(screen[-600:]))

        print("the goal is set")
        mark = len(session.output)
        session.send("/goal all the tests pass\r")
        session.read(1.0)
        screen = strip(session.output[mark:])
        check("the goal is named", "Goal set" in screen, repr(screen[-600:]))
        check("the condition is shown", "all the tests pass" in screen, repr(screen[-600:]))

        print("the goal is shown again")
        mark = len(session.output)
        session.send("/goal\r")
        session.read(1.0)
        screen = strip(session.output[mark:])
        check("the goal is still there", "Goal active" in screen, repr(screen[-600:]))
        check("the goal was not evaluated yet", "not yet evaluated" in screen, repr(screen[-600:]))

        print("an empty condition is refused")
        mark = len(session.output)
        session.send("/goal \u200b\r")
        session.read(1.0)
        screen = strip(session.output[mark:])
        check("the condition is refused", "empty" in screen, repr(screen[-600:]))

        print("the goal is cleared")
        mark = len(session.output)
        session.send("/goal clear\r")
        session.read(1.0)
        screen = strip(session.output[mark:])
        check("the goal is cleared", "Goal cleared" in screen, repr(screen[-600:]))

        mark = len(session.output)
        session.send("/goal\r")
        session.read(1.0)
        screen = strip(session.output[mark:])
        check("there is no goal any more", "No goal set" in screen, repr(screen[-600:]))
    finally:
        session.close()
        shutil.rmtree(home, ignore_errors=True)

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
