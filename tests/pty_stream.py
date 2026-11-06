#!/usr/bin/env python3
"""Drives the port through an answer that streams and checks where it stands.

The answer of the model has to stream in the place where it began: its first
line stands right under the line of the prompt of the user, and the box of
the prompt stands at the bottom of the screen. The reference streams it that
way, and the answer that does not fit is written to the transcript, the way
the reference scrolls it up.

    python3 tests/pty_stream.py
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_smoke import BINARY, Session  # noqa: E402
from screen import Screen  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOCK = os.path.join(ROOT, "tests", "mock_api.py")
PORT = 8244

COLUMNS = 80
ROWS = 24

# An answer that is long enough to take more than one line and short enough
# to stay on the screen.
TEXT = ("The first sentence of the answer of the model, which streams in "
        "pieces. The second sentence follows it and is a little longer than "
        "the first one. The third sentence is here so that the answer takes "
        "more than one line of the screen.")

FAILURES = []


def check(name, condition, detail=""):
    if condition:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name} {detail}")
        FAILURES.append(name)


def start_mock():
    """Starts the mock of the API, with an answer that streams slowly."""
    return subprocess.Popen(
        [sys.executable, MOCK, "--port", str(PORT), "--text", TEXT,
         "--chunk-delay", "0.3"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class ScreenWatcher:
    """Replays the bytes the session writes on a grid, the way a terminal does."""

    def __init__(self, session, columns=COLUMNS, rows=ROWS):
        self.session = session
        self.screen = Screen(columns, rows)
        self.mark = 0

    def update(self, wait=0.2):
        """Reads what the session wrote and replays it on the grid."""
        self.session.read(wait)
        raw = self.session.output[self.mark:]
        self.mark = len(self.session.output)
        self.screen.feed(raw)
        return self.screen.lines()

    def row_of(self, text):
        """The row the text stands on, counted from zero."""
        for number, line in enumerate(self.screen.lines()):
            if text in line:
                return number
        return -1


def stream_checks(name, env):
    """Drives a session and checks where the answer stands while it streams."""
    print(name)

    home = tempfile.mkdtemp(prefix="cc-stream-")
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"theme": "dark", "hasCompletedOnboarding": True,
                   "projects": {ROOT: {"hasTrustDialogAccepted": True}}}, handle)

    mock = start_mock()
    time.sleep(1.0)
    # The session gets its own home directory, so that the test does not
    # touch the settings of the user.
    env = {**env, "HOME": home}
    session = Session(env=env)
    watcher = ScreenWatcher(session)
    try:
        session.read(2.0)
        session.resize(COLUMNS, ROWS)
        watcher.update(0.5)

        session.send("tell me a story\r")

        # The answer streams: the screen is watched while it grows.
        rows = []
        deadline = time.time() + 20.0
        while time.time() < deadline:
            lines = watcher.update(0.2)
            answer = watcher.row_of("The first sentence")
            if answer >= 0:
                rows.append((answer, lines))
                if len(rows) >= 3:
                    break

        check(f"{name}: the answer is on the screen", len(rows) > 0)
        if not rows:
            return

        first_answer, lines = rows[0]
        prompt = watcher.row_of("tell me a story")
        # The reference leaves one empty line between the prompt of the user
        # and the answer of the model.
        check(f"{name}: the answer stands under the prompt of the user",
              prompt >= 0 and first_answer == prompt + 2,
              f"the prompt is on row {prompt}, the answer on row {first_answer}")

        # The answer grows in the place where it began: its first line does
        # not move while the text streams.
        check(f"{name}: the answer stays in its place",
              all(row == first_answer for row, _ in rows),
              f"the rows of the answer are {[row for row, _ in rows]}")

        # The box of the prompt stands under the conversation in the
        # scrollback layout, and at the bottom of the screen in the
        # full-screen one, the way the reference puts it there.
        bottom = "".join(watcher.screen.grid[ROWS - 1]).rstrip()
        if "full-screen" in name:
            check(f"{name}: the box of the prompt stands at the bottom of the screen",
                  "auto mode on" in bottom, repr(bottom))
        else:
            check(f"{name}: the box of the prompt stands under the conversation",
                  any("auto mode on" in "".join(watcher.screen.grid[row])
                      for row in range(ROWS)),
                  repr(bottom))
    finally:
        session.close()
        mock.terminate()
        try:
            mock.wait(timeout=3)
        except subprocess.TimeoutExpired:
            mock.kill()
        shutil.rmtree(home, ignore_errors=True)


def main():
    if not os.path.exists(BINARY):
        print(f"binary not found: {BINARY}")
        return 1

    base = {**os.environ, "ANTHROPIC_API_KEY": "test-key",
            "ANTHROPIC_BASE_URL": f"http://127.0.0.1:{PORT}"}

    stream_checks("the scrollback layout", {**base, "CLAUDE_CODE_SCROLLBACK": "1"})
    stream_checks("the full-screen layout", {**base, "CLAUDE_CODE_FULLSCREEN": "1"})

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
