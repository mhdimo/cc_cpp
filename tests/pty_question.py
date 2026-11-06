#!/usr/bin/env python3
"""Drives the port through the question the model asks.

The model asks a question with the `AskUserQuestion` tool. The reference
shows the choices of the question where the permission of the tool would be
asked, so that the user answers the question and the tool runs at once; the
port asked the user whether they wanted to proceed first, which the
reference never does.

The test drives the port against a mock of the API that calls the tool, and
looks at what the port draws.

    python3 tests/pty_question.py
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
PORT = 8231
PORT_TWO = 8234

FAILURES = []


def check(name, condition, detail=""):
    if condition:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name} {detail}")
        FAILURES.append(name)


def start_mock(home):
    """Starts the mock of the API and returns the process."""
    return subprocess.Popen(
        [sys.executable, MOCK, "--port", str(PORT), "--text", "The answer is one.",
         "--tool", "AskUserQuestion",
         "--tool-input", json.dumps({"questions": [{
             "question": "Do you want to proceed?",
             "header": "Proceed",
             "options": [
                 {"label": "Yes", "description": "Go on with it"},
                 {"label": "No", "description": "Stop here"},
             ],
         }]})],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def start_mock_with_two_questions():
    """Starts a mock of the API that asks two questions at once."""
    return subprocess.Popen(
        [sys.executable, MOCK, "--port", str(PORT_TWO), "--echo",
         "--tool", "AskUserQuestion",
         "--tool-input", json.dumps({"questions": [
             {"question": "Which library should we use?", "header": "Library",
              "options": [{"label": "date-fns"}, {"label": "dayjs"}]},
             {"question": "Which language should we use?", "header": "Language",
              "options": [{"label": "C++"}, {"label": "Rust"}]},
         ]})],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def two_questions(home):
    """Drives the port through a call that asks two questions."""
    mock = start_mock_with_two_questions()
    time.sleep(1.0)
    if mock.poll() is not None:
        print(f"the mock API did not start on the port {PORT_TWO}")
        return

    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "test-key",
           "ANTHROPIC_BASE_URL": f"http://127.0.0.1:{PORT_TWO}",
           "CLAUDE_CODE_SCROLLBACK": "1"}

    session = Session(env=env)
    try:
        session.read(2.0)
        session.send("ask me two things\r")
        session.read(2.0)

        screen = strip(session.output)
        print("the questions of the model")

        # The bar of the dialog holds every question of the call.
        check("both questions are in the bar",
              "Library" in screen and "Language" in screen, repr(screen[-600:]))
        check("the first question is asked", "Which library should we use?" in screen)
        check("the second question is not asked yet",
              "Which language should we use?" not in screen)

        # The right arrow moves to the next question.
        mark = len(session.output)
        session.send("\x1b[C")  # Right
        session.read(1.0)
        screen = strip(session.output[mark:])
        check("the right arrow moves to the next question",
              "Which language should we use?" in screen, repr(screen[-400:]))

        # The left arrow moves back.
        mark = len(session.output)
        session.send("\x1b[D")  # Left
        session.read(1.0)
        screen = strip(session.output[mark:])
        check("the left arrow moves back", "Which library should we use?" in screen,
              repr(screen[-400:]))

        # The first question is answered: the dialog moves on to the second.
        session.send("\r")
        session.read(1.0)
        screen = strip(session.output)
        check("the answer moves on to the next question",
              "Which language should we use?" in screen, repr(screen[-400:]))

        # The second question is answered: the answers are reviewed.
        session.send("\r")
        session.read(1.0)
        screen = strip(session.output)
        check("the answers are reviewed", "Review your answers" in screen,
              repr(screen[-600:]))

        # The answers are handed over.
        session.send("\r")
        session.read(2.0)
        screen = strip(session.output)
        check("both answers go back to the model",
              "date-fns" in screen and "C++" in screen, repr(screen[-600:]))
    finally:
        session.close()
        mock.terminate()
        try:
            mock.wait(timeout=3)
        except subprocess.TimeoutExpired:
            mock.kill()


def main():
    if not os.path.exists(BINARY):
        print(f"binary not found: {BINARY}")
        return 1

    home = tempfile.mkdtemp(prefix="cc-question-")
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"theme": "dark", "hasCompletedOnboarding": True,
                   "projects": {os.getcwd(): {"hasTrustDialogAccepted": True}}}, handle)

    mock = start_mock(home)
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

        session.send("ask me something\r")
        session.read(2.0)

        screen = strip(session.output)

        print("the question of the model")
        check("the question is asked", "Do you want to proceed?" in screen, repr(screen[-600:]))
        check("the choices are offered", "Yes" in screen and "No" in screen)
        check("the tool is not approved first", "Do you want to proceed?" not in screen or
              "Tool use" not in screen,
              "the port asks the permission of the tool before the question")

        # The first choice is picked.
        session.send("\r")
        session.read(2.0)

        screen = strip(session.output)
        check("the answer goes back to the model", "The answer is one." in screen,
              repr(screen[-600:]))
    finally:
        session.close()
        mock.terminate()
        try:
            mock.wait(timeout=3)
        except subprocess.TimeoutExpired:
            mock.kill()

    # A call that asks two questions is asked in one dialog.
    two_questions(home)

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
