#!/usr/bin/env python3
"""Checks the commands of the bash mode.

A command that starts with `!` runs in the shell of the session, the way
Claude Code runs it: the output is cut short and the number of the lines
that are left is said, and the transcript (Ctrl+O) shows the whole of it.
The shell stays alive for the whole session, so that a `cd` of a command
moves the folder the session is in; when the folder leaves the folder of
the project, it is put back and the model is told about it.
"""

import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_smoke import FAILURES, Session, check, strip  # noqa: E402


def start(env, cwd):
    """Starts a session and answers the question about the folder."""
    session = Session(env=env, cwd=cwd)
    session.read(2.5)
    if "trust" in strip(session.output).lower():
        session.send("\r")
        session.read(2.0)
    return session


def main():
    home = tempfile.mkdtemp(prefix="cc-bash-")
    project = os.path.realpath(tempfile.mkdtemp(prefix="cc-bash-project-"))
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"hasCompletedOnboarding": True,
                   "projects": {project: {"hasTrustDialogAccepted": True}}}, handle)

    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "",
           "ANTHROPIC_BASE_URL": "http://127.0.0.1:9",
           "CLAUDE_CODE_SCROLLBACK": "1"}

    session = start(env, project)

    print("the command of the shell")
    before = len(session.output)
    session.send("!seq 1 12\r")
    time.sleep(1.5)
    session.read(0.8)
    screen = strip(session.output[before:])
    check("the command is shown", "! seq 1 12" in screen, repr(screen[-400:]))
    check("the output is shown", "1" in screen and "2" in screen and "3" in screen,
          repr(screen[-400:]))
    check("the output is cut short", "lines (ctrl+o to expand)" in screen, repr(screen[-400:]))

    print("the transcript shows the whole output")
    before = len(session.output)
    session.send("\x0f")  # Ctrl+O
    time.sleep(1.0)
    session.read(0.8)
    screen = strip(session.output[before:])
    check("the whole output is there", "12" in screen, repr(screen[-600:]))
    check("the output is not cut short any more",
          "lines (ctrl+o to expand)" not in screen, repr(screen[-600:]))

    session.send("\x0f")  # Ctrl+O again: the transcript closes.
    session.read(0.6)

    print("the standard error of the command")
    before = len(session.output)
    session.send("!echo to-stderr 1>&2\r")
    time.sleep(1.2)
    session.read(0.6)
    screen = strip(session.output[before:])
    check("the standard error is shown", "to-stderr" in screen, repr(screen[-400:]))

    print("the folder of the session")
    before = len(session.output)
    session.send("!cd ..\r")
    time.sleep(1.2)
    session.read(0.6)
    screen = strip(session.output[before:])
    check("the shell is put back", "Shell cwd was reset to" in screen, repr(screen[-400:]))

    before = len(session.output)
    session.send("!pwd\r")
    time.sleep(1.2)
    session.read(0.6)
    screen = strip(session.output[before:])
    # The path is cut short and wrapped, so only its first part is looked
    # for.
    check("the folder of the session is the one of the command", "cc-bash-proje" in screen,
          repr(screen[-400:]))

    session.close()

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
