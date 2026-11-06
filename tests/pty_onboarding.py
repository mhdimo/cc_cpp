#!/usr/bin/env python3
"""Drives the setup of the session through a pty and checks what it asks.

The questions of the setup are asked only once, so the session is given a
home directory of its own, without the flag that says that the setup was
done already.
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
    home = tempfile.mkdtemp(prefix="cc-onboarding-")
    env = {**os.environ, "HOME": home,
           "ANTHROPIC_API_KEY": "sk-ant-0123456789abcdefghijklmnopqrstuvwxyz",
           "ANTHROPIC_BASE_URL": "http://127.0.0.1:9",
           "CLAUDE_CODE_SCROLLBACK": "1"}

    session = Session(env=env)
    try:
        session.read(2.5)
        screen = strip(session.output)

        print("welcome")
        check("the welcome is drawn", "Welcome to Claude Code" in screen, repr(screen[:300]))
        check("the picture of the mascot is drawn", "░" in screen or "█" in screen,
              repr(screen[:300]))

        print("theme")
        check("the theme is asked for", "Let's get started." in screen, repr(screen[-600:]))
        check("the way to change the theme later is said",
              "To change this later, run /theme" in screen, repr(screen[-600:]))
        check("the question of the theme is asked",
              "Choose the text style that looks best with your terminal" in screen,
              repr(screen[-600:]))

        mark = len(session.output)
        session.send("\r")  # The theme is chosen.
        session.read(1.0)
        screen = strip(session.output[mark:])

        print("api key")
        check("the key of the API is asked for",
              "Detected a custom API key in your environment" in screen, repr(screen[-600:]))
        check("the key is shown", "sk-ant-..." in screen, repr(screen[-600:]))
        check("the answer that is recommended is shown",
              "No (recommended)" in screen, repr(screen[-600:]))

        mark = len(session.output)
        session.send("\r")  # The key is refused.
        session.read(1.0)
        screen = strip(session.output[mark:])

        print("security")
        check("the notes of the safety are shown", "Security notes:" in screen,
              repr(screen[-800:]))
        check("the first note is shown", "Claude can make mistakes" in screen,
              repr(screen[-800:]))
        check("the second note is shown",
              "Due to prompt injection risks, only use it with code you trust" in screen,
              repr(screen[-800:]))
        check("the guide is named", "code.claude.com/docs/en/security" in screen,
              repr(screen[-800:]))
        check("the way to go on is said", "Press Enter to continue" in screen,
              repr(screen[-800:]))

        mark = len(session.output)
        session.send("\r")  # The notes are read.
        session.read(1.0)
        screen = strip(session.output[mark:])

        print("terminal setup")
        check("the settings of the terminal are asked for",
              "Use Claude Code's terminal setup?" in screen, repr(screen[-600:]))
        check("the newlines are named", "Shift+Enter for newlines" in screen,
              repr(screen[-600:]))
        check("the way to do it later is said",
              "No, maybe later with /terminal-setup" in screen, repr(screen[-600:]))

        mark = len(session.output)
        session.send("\r")  # The settings of the terminal are left alone.
        session.read(1.0)
        screen = strip(session.output[mark:])

        print("trust")
        check("the trust of the folder is asked for", "Accessing workspace:" in screen,
              repr(screen[-600:]))

        # The banner that follows the setup of the session: the frame of the
        # panel and the feeds are not drawn at the first run.
        mark = len(session.output)
        session.send("\r")  # The folder is trusted.
        session.read(1.5)
        screen = strip(session.output[mark:])

        print("banner")
        check("the banner has no frame", "╭" not in screen, repr(screen[-600:]))
        check("the banner has no recent activity", "Recent activity" not in screen,
              repr(screen[-600:]))
        check("the banner has no notes of the release", "What's new" not in screen,
              repr(screen[-600:]))
        check("the mascot is drawn", "▐▛███▛█" in screen, repr(screen[-600:]))

        # The setup is not asked again in the next session.
        with open(os.path.join(home, ".claude.json"), "w") as handle:
            json.dump({}, handle)
        config = json.load(open(os.path.join(home, ".claude.json")))
        config["hasCompletedOnboarding"] = True
        with open(os.path.join(home, ".claude.json"), "w") as handle:
            json.dump(config, handle)
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
