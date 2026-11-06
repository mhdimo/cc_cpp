#!/usr/bin/env python3
"""Drives the port through a resize of the terminal.

The prompt box and the answer that streams have to follow the size of the
terminal: the text is wrapped again at the new width, the box is drawn again
at the new size, and the frames of the old region do not stay behind in the
scrollback.

    python3 tests/pty_resize.py
"""

import json
import os
import re
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
PORT = 8243

# The answer the mock of the API streams: the words are the ones of the
# alphabet of the pilot, so that a word that stands on the screen twice is
# easy to see.
ANSWER = ("alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima "
          "mike november oscar papa quebec romeo sierra tango uniform victor whiskey "
          "xray yankee zulu")

FAILURES = []


def check(name, condition, detail=""):
    if condition:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name} {detail}")
        FAILURES.append(name)


def start_mock(*args):
    """Starts the mock of the API."""
    return subprocess.Popen(
        [sys.executable, MOCK, "--port", str(PORT), *args],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def make_home():
    """A home directory of its own, so that the settings of the user are not
    touched."""
    home = tempfile.mkdtemp(prefix="cc-resize-")
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"hasCompletedOnboarding": True}, handle)
    return home


def row(screen, number):
    """The row of the screen, as a line of text."""
    return "".join(screen.grid[number])


def rows(screen):
    """Every row of the screen, as a line of text."""
    return [row(screen, number).rstrip() for number in range(screen.rows)]


def has(screen, text):
    """True when the text stands on the screen."""
    return any(text in line for line in rows(screen))


class Watcher:
    """The screen of a session, resized when the session is.

    The bytes that were written before the resize are laid out at the old
    size, the terminal wraps them again when it is resized, and the bytes
    that were written after it are laid out at the new size.
    """

    def __init__(self, session, columns, rows_):
        self.session = session
        self.screen = Screen(columns, rows_)
        self.mark = 0

    def flush(self):
        """Lays out the bytes that were written since the last time."""
        self.screen.feed(self.session.output[self.mark:])
        self.mark = len(self.session.output)

    def resize(self, columns, rows_):
        """The terminal was resized: the text on the screen is wrapped again."""
        self.screen.resize(columns, rows_)

    def now(self):
        """The screen of the terminal, as it stands now."""
        self.flush()
        return self.screen


def frames_checks():
    """The region is drawn once, and the frames of the old one do not stay."""
    print("the frames of the old region")
    home = make_home()
    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "",
           "ANTHROPIC_BASE_URL": "http://127.0.0.1:9", "CLAUDE_CODE_SCROLLBACK": "1"}

    session = Session(env=env)
    try:
        session.read(1.5)
        session.send("\r")  # Enter trusts the folder
        session.read(1.0)

        # The region is drawn once when the terminal is resized: the box of
        # the old width would be left behind on the screen.
        mark = len(session.output)
        session.resize(40, 24)
        session.read(1.5)

        drawn = session.output[mark:]
        check("a width change resets the reflowed inline screen",
              "\x1b[3J\x1b[2J\x1b[H" in drawn, repr(drawn[-400:]))
        check("the region is drawn once", drawn.count("? for shortcuts") == 1,
              repr(drawn[-400:]))
        check("the box is drawn at the new width", "\u2500" * 40 in drawn,
              repr(drawn[-400:]))
        check("the box of the old width is not drawn again", "\u2500" * 80 not in drawn,
              repr(drawn[-400:]))
    finally:
        session.close()
        shutil.rmtree(home, ignore_errors=True)


def scrollback_checks():
    """The prompt box follows the size of the terminal."""
    print("the prompt box")
    home = make_home()
    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "",
           "ANTHROPIC_BASE_URL": "http://127.0.0.1:9", "CLAUDE_CODE_SCROLLBACK": "1"}

    session = Session(env=env)
    try:
        session.read(1.5)
        session.send("\r")  # Enter trusts the folder
        session.read(1.0)

        watcher = Watcher(session, 80, 24)
        screen = watcher.now()
        # The box stands under the transcript, and not at the bottom of the
        # screen: the conversation is in the scrollback of the terminal, and
        # the user scrolls through it with the scrollbar of the terminal.
        check("the box stands under the transcript",
              has(screen, "? for shortcuts"), repr(screen.text()[-200:]))
        check("the bottom of the screen is empty",
              not row(screen, -1).strip() and not row(screen, -2).strip(),
              repr(rows(screen)[-4:]))

        # The terminal becomes narrower: the box is drawn again at the new
        # width, and it stands at the bottom of the screen still.
        session.read(0.4)
        watcher.flush()
        session.resize(40, 24)
        watcher.resize(40, 24)
        session.read(1.5)

        screen = watcher.now()
        check("the box stands under the transcript in the narrower screen",
              has(screen, "? for shortcuts"), repr(screen.text()[-200:]))
        check("the rule of the box is as wide as the terminal",
              "─" * 40 in rows(screen), repr(rows(screen)[-6:]))
        check("the rule of the box is not wider than the terminal",
              all("─" * 41 not in line for line in rows(screen)), repr(rows(screen)[-6:]))
        check("every row of the screen is inside the terminal",
              all(len(line) <= 40 for line in rows(screen)), repr(rows(screen)[-6:]))

        # The terminal becomes wider again: the box follows it back.
        session.read(0.4)
        watcher.flush()
        session.resize(100, 24)
        watcher.resize(100, 24)
        session.read(1.5)

        screen = watcher.now()
        check("the box stands under the transcript in the wider screen",
              has(screen, "? for shortcuts"), repr(screen.text()[-200:]))
        check("the rule of the box is as wide as the terminal",
              "─" * 100 in rows(screen), repr(rows(screen)[-6:]))
        check("the rule of the box is not wider than the terminal",
              all("─" * 101 not in line for line in rows(screen)), repr(rows(screen)[-6:]))
    finally:
        session.close()
        shutil.rmtree(home, ignore_errors=True)


def rapid_scrollback_checks():
    """Repeated width changes do not leave prompt frames behind."""
    print("rapid prompt resizes")
    home = make_home()
    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "",
           "ANTHROPIC_BASE_URL": "http://127.0.0.1:9", "CLAUDE_CODE_SCROLLBACK": "1"}

    session = Session(env=env)
    try:
        session.read(1.5)
        session.send("\r")
        session.read(1.0)

        watcher = Watcher(session, 80, 24)
        for columns in (40, 500, 80, 400, 40, 500):
            watcher.flush()
            session.resize(columns, 24)
            watcher.resize(columns, 24)
        session.read(0.8)

        screen = watcher.now()
        text = screen.text()
        check("the prompt stays visible after rapid resizes",
              "? for shortcuts" in text, repr(rows(screen)[-8:]))
        check("the prompt is drawn once after rapid resizes",
              text.count("? for shortcuts") == 1, repr(rows(screen)[-8:]))
        check("the box rule matches the final width after rapid resizes",
              "─" * 500 in rows(screen), repr(rows(screen)[-8:]))
    finally:
        session.close()
        shutil.rmtree(home, ignore_errors=True)


def streaming_checks():
    """The answer that streams follows the size of the terminal."""
    print("the answer that streams")
    home = make_home()
    mock = start_mock("--text", ANSWER, "--chunk-delay", "0.15")
    time.sleep(1.0)
    if mock.poll() is not None:
        check("the mock of the API starts", False, "the mock did not start")
        return

    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "test-key",
           "ANTHROPIC_BASE_URL": f"http://127.0.0.1:{PORT}", "CLAUDE_CODE_SCROLLBACK": "1"}

    session = Session(env=env)
    try:
        session.read(2.0)
        session.send("\r")  # Enter trusts the folder
        session.read(1.0)
        session.send("tell me a story\r")
        # The answer starts to stream.
        session.wait_for("alpha", timeout=10.0)
        session.read(0.6)

        watcher = Watcher(session, 80, 24)
        watcher.flush()
        session.resize(40, 24)
        session.read(1.5)
        watcher.resize(40, 24)

        screen = watcher.now()
        text = screen.text()
        # The answer is wrapped again at the new width, so it stands under
        # the text that came before it, and nothing of the old region is left
        # behind.
        check("the answer is on the screen once", text.count("alpha") == 1,
              repr(text[-400:]))
        check("the answer is wrapped at the new width",
              any("⏺ alpha bravo charlie" in line for line in rows(screen)),
              repr(rows(screen)[-8:]))
        check("the box is drawn at the new width", "─" * 40 in text, repr(text[-400:]))

        # The terminal becomes shorter: the answer that does not fit goes to
        # the transcript, the way the reference scrolls it up, and the region
        # never grows taller than the screen.
        session.read(0.4)
        watcher.flush()
        session.resize(40, 10)
        watcher.resize(40, 10)
        session.read(1.5)

        screen = watcher.now()
        # The answer that does not fit goes to the transcript, so the part of
        # it that is on the screen is the tail of it — but it is never on the
        # screen twice.
        check("the answer is not on the screen twice in the shorter screen",
              screen.text().count("alpha") <= 1, repr(screen.text()[-400:]))
        check("the region fits in the shorter screen",
              len([line for line in rows(screen) if line]) <= 10, repr(rows(screen)))
        check("the box is at the bottom of the shorter screen",
              bool(rows(screen)[-1]), repr(rows(screen)))
    finally:
        session.close()
        mock.terminate()
        try:
            mock.wait(timeout=3)
        except subprocess.TimeoutExpired:
            mock.kill()
        shutil.rmtree(home, ignore_errors=True)


def long_stream_resize_checks():
    """A long, unbroken live paragraph stays bounded while the width changes."""
    print("a long live paragraph during resizes")
    home = make_home()
    answer = " ".join(f"word{index:04d}" for index in range(1000))
    mock = start_mock("--text", answer, "--chunk-delay", "0.002")
    time.sleep(0.3)
    if mock.poll() is not None:
        check("the mock of the long answer starts", False, "the mock did not start")
        return

    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "test-key",
           "ANTHROPIC_BASE_URL": f"http://127.0.0.1:{PORT}", "CLAUDE_CODE_SCROLLBACK": "1"}
    session = Session(env=env)
    try:
        session.read(2.0)
        session.send("\r")
        session.read(1.0)
        session.send("tell me a story\r")
        session.wait_for("word0000", timeout=10.0)
        session.read(0.3)

        watcher = Watcher(session, 80, 24)
        watcher.now()
        for columns in (30, 100, 40, 120, 30):
            watcher.flush()
            session.resize(columns, 24)
            watcher.resize(columns, 24)
            session.read(0.15)

        screen = watcher.now()
        tokens = re.findall(r"word\d{4}", screen.text())
        check("the prompt remains visible during the long answer",
              "❯" in screen.text(), repr(rows(screen)[-8:]))
        check("the visible live answer has no repeated rows",
              len(tokens) == len(set(tokens)), repr(rows(screen)[-12:]))
    finally:
        session.close()
        mock.terminate()
        try:
            mock.wait(timeout=3)
        except subprocess.TimeoutExpired:
            mock.kill()
        shutil.rmtree(home, ignore_errors=True)


def fullscreen_checks():
    """The conversation of the full-screen layout follows the size."""
    print("the full-screen layout")
    home = make_home()
    # The API is out of reach: the command of the shell is asked about by
    # the model, and the request fails at once, so that the box of the
    # prompt stands at the bottom of the window when the window is resized.
    # The address is a dead one on purpose: the test must not wait for a
    # model that answers in another country.
    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "test-key",
           "ANTHROPIC_BASE_URL": "http://127.0.0.1:9", "CLAUDE_CODE_SCROLLBACK": "",
           "CLAUDE_CODE_FULLSCREEN": "1"}

    session = Session(env=env)
    try:
        session.read(1.5)
        session.send("\r")  # Enter trusts the folder
        session.read(1.0)

        session.send("!echo alpha bravo charlie delta echo foxtrot golf hotel india\r")
        session.read(1.5)

        watcher = Watcher(session, 80, 24)
        watcher.flush()
        session.resize(40, 24)
        session.read(1.5)
        watcher.resize(40, 24)

        screen = watcher.now()
        text = screen.text()
        check("the box is at the bottom of the narrower window",
              "? for shortcuts" in row(screen, -1), repr(text[-200:]))
        check("the rule of the box is as wide as the window",
              "─" * 40 in rows(screen), repr(rows(screen)[-6:]))
        check("the conversation is on the screen", "alpha" in text, repr(text[-400:]))
        check("every row of the window is inside it",
              all(len(line) <= 40 for line in rows(screen)), repr(rows(screen)))
    finally:
        session.close()
        shutil.rmtree(home, ignore_errors=True)


def main():
    if not os.path.exists(BINARY):
        print(f"binary not found: {BINARY}")
        return 1

    frames_checks()
    scrollback_checks()
    rapid_scrollback_checks()
    streaming_checks()
    long_stream_resize_checks()
    fullscreen_checks()

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
