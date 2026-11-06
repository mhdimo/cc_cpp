#!/usr/bin/env python3
"""Drives the CLI through a pty and checks what it draws.

The terminal UI only behaves correctly on a real terminal, so the checks
here run the binary under a pty and look at the bytes it writes.
"""

import atexit
import codecs
import fcntl
import os
import json
import pty
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from screen import Screen  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.path.join(ROOT, "build", "src", "cc++")

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07]*\x07|\x1b\][^\x1b]*\x1b\\")

# The sessions that are still open. A check that fails raises, and the
# session that was running would then be left behind: the harness closes
# them all when it ends, whatever happened in between.
LIVE = []


def close_all():
    """Closes every session that is still open."""
    while LIVE:
        LIVE.pop().close()


atexit.register(close_all)


def strip(text: str) -> str:
    return ANSI.sub("", text)


class Session:
    def __init__(self, args=(), env=None, binary=None, cwd=None):
        self.master, slave = pty.openpty()
        os.set_blocking(self.master, False)
        self.process = subprocess.Popen(
            [binary or BINARY, *args],
            stdin=slave,
            stdout=slave,
            stderr=slave,
            cwd=cwd or ROOT,
            env=env,
            preexec_fn=os.setsid,
        )
        os.close(slave)
        self.output = ""
        # The reader keeps the bytes of a character that was cut in half by
        # the end of a read, so that the character is not turned into a
        # replacement one.
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        LIVE.append(self)

    def read(self, timeout=1.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            ready, _, _ = select.select([self.master], [], [], 0.1)
            if not ready:
                continue
            try:
                chunk = os.read(self.master, 65536)
            except OSError:
                break
            if not chunk:
                break
            self.output += self.decoder.decode(chunk)
        return self.output

    def wait_for(self, text, timeout=3.0):
        """Reads until `text` shows up, and returns everything after `mark`."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if text in strip(self.output):
                return True
            self.read(0.1)
        return text in strip(self.output)

    def screen_text(self, columns=80, rows=24):
        """The screen of the terminal, as it stands now.

        The bytes are replayed on a grid, the way a terminal does it, so
        that the screen is the one the user sees and not the stream of the
        frames.
        """
        screen = Screen(columns, rows)
        screen.feed(self.output)
        return screen.text()

    def send(self, text):
        os.write(self.master, text.encode())

    def resize(self, columns, rows):
        """Resizes the terminal and tells the session about it."""
        fcntl.ioctl(self.master, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, columns, 0, 0))
        os.killpg(os.getpgid(self.process.pid), signal.SIGWINCH)

    def wait(self, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline and self.process.poll() is None:
            self.read(0.2)
        return self.process.poll()

    def close(self):
        if self in LIVE:
            LIVE.remove(self)
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        os.close(self.master)


FAILURES = []


def check(name, condition, detail=""):
    if condition:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name} {detail}")
        FAILURES.append(name)


def main():
    if not os.path.exists(BINARY):
        print(f"binary not found: {BINARY}")
        return 1

    # The session gets its own home directory, so the test does not touch the
    # user's settings and does not depend on them.
    home = tempfile.mkdtemp(prefix="cc-pty-")
    # The session talks to nothing: the address points at a closed port, so
    # a request fails at once instead of reaching the network.
    # The scrollback layout is asked for: the checks below look at the bytes
    # the binary writes, and the full-screen layout writes the whole screen
    # again on every frame.
    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "",
           "ANTHROPIC_BASE_URL": "http://127.0.0.1:9",
           "CLAUDE_CODE_SCROLLBACK": "1",
           # The CLI tells the user about the tmux session it runs in, the
           # way the reference does it.
           "CLAUDE_CODE_TMUX_SESSION": "cc-smoke",
           "CLAUDE_CODE_TMUX_PREFIX": "C-b"}

    # A plugin of the cache, with a command of its own.
    plugin = os.path.join(home, ".claude", "plugins", "cache", "official", "greeter", "1.0.0")
    os.makedirs(os.path.join(plugin, "commands"))
    os.makedirs(os.path.join(plugin, ".claude-plugin"))
    with open(os.path.join(plugin, ".claude-plugin", "plugin.json"), "w") as handle:
        json.dump({"name": "greeter", "version": "1.0.0", "description": "Says hello"}, handle)
    with open(os.path.join(plugin, "commands", "greet.md"), "w") as handle:
        handle.write("---\ndescription: Greets the user\n---\nSay hello.\n")
    with open(os.path.join(home, ".claude", "settings.json"), "w") as handle:
        json.dump({"enabledPlugins": {"greeter@official": True}}, handle)
    # The setup of the session was done already, so that the questions of
    # the setup are not asked.
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"hasCompletedOnboarding": True}, handle)

    session = Session(env=env)
    session.read(1.5)

    print("trust")
    screen = strip(session.output)
    check("the trust dialog is shown", "Accessing workspace:" in screen, repr(screen[-600:]))
    session.send("\r")  # Enter trusts the folder
    session.read(0.8)

    screen = strip(session.output)

    print("startup")
    check("the logo is drawn", "Claude Code" in screen, repr(screen[:200]))
    check("the mascot is drawn", "▐▛███▛█" in screen, repr(screen[:200]))
    # The session tells the user about the tmux session it runs in.
    check("the tmux session is named", "tmux session: cc-smoke" in screen, repr(screen[:400]))
    check("the way to leave tmux is said", "Detach: C-b d" in screen, repr(screen[:400]))
    check("the prompt bar is drawn", "─" * 20 in screen)
    check("the pointer is drawn", "❯" in screen)
    check("the hint is drawn", "? for shortcuts" in screen)
    # The prompt box sits at the bottom of the screen, the way Claude Code
    # puts it, with the space above it left empty.
    check("the prompt box is pinned to the bottom",
          strip(session.output).rstrip().endswith("? for shortcuts"),
          repr(strip(session.output)[-200:]))
    logo_lines = [line for line in screen.splitlines()
                  if "▐▛███▛█" in line or "▝▜██████▀" in line]
    check(
        "the logo lines are not staircased",
        bool(logo_lines) and not logo_lines[0].startswith("  ") and
        all(not line.startswith(" ") for line in logo_lines[1:]),
        repr(logo_lines),
    )

    print("typing")
    session.send("hello")
    session.read(0.6)
    check("the typed text appears", "hello" in strip(session.output))

    print("the keys with a modifier")
    session.send("\x15")  # Ctrl-U clears the line
    session.read(0.4)
    session.send("one two three")
    session.read(0.6)
    # The option of the keyboard of Apple takes the word before the cursor
    # away.
    session.send("\x1b\x7f")
    session.read(0.4)
    text = session.screen_text()
    check("the option with the backspace takes the word away",
          "one two" in text and "three" not in text, repr(text[-200:]))
    # The command of the keyboard of Apple takes the line before the cursor
    # away.
    session.send("\x1b\x08")
    session.read(0.4)
    text = session.screen_text()
    check("the command with the backspace takes the line away",
          "one two" not in text, repr(text[-200:]))
    # The option with the delete key takes the word under the cursor away.
    session.send("one two three")
    session.read(0.4)
    session.send("\x01")  # Ctrl-A moves the cursor to the start of the line
    session.read(0.4)
    session.send("\x1b[3;3~")
    session.read(0.4)
    text = session.screen_text()
    check("the option with the delete key takes the word away",
          "one" not in text and "two three" in text, repr(text[-200:]))
    # The command with the delete key takes the rest of the line away.
    session.send("\x1b[3;9~")
    session.read(0.4)
    text = session.screen_text()
    check("the command with the delete key takes the rest of the line away",
          "two three" not in text, repr(text[-200:]))
    session.send("\x15")  # Ctrl-U clears the line
    session.read(0.4)

    print("shortcuts menu")
    session.send("\x15")  # Ctrl-U clears the line
    session.read(0.4)
    session.send("?")
    session.read(0.6)
    menu = strip(session.output)
    check("the shortcuts menu opens", "for bash mode" in menu, repr(menu[-400:]))
    session.send("\x1b")  # Escape closes it
    session.read(0.4)

    print("suggestions")
    session.send("/cl")
    session.read(0.6)
    overlay = strip(session.output)
    check("the command overlay opens", "/clear" in overlay, repr(overlay[-400:]))
    session.send("\x15")  # Ctrl-U clears the line
    session.read(0.4)

    print("suggestions")
    # The list of the suggestions is longer than the screen, so the window
    # follows the highlight.
    session.send("/")
    session.read(0.6)
    before = len(session.output)
    for _ in range(8):
        session.send("\x1b[B")  # The down arrow.
        session.read(0.15)
    session.read(0.6)
    frame = session.screen_text()
    # The window followed the highlight, so the list no longer begins with
    # its first command.
    check("the suggestions scroll with the highlight", "/add-dir" not in frame,
          repr(frame[-400:]))
    session.send("\x15")  # Ctrl-U clears the line.
    session.read(0.4)

    session.send("/con")
    session.read(0.6)
    overlay = strip(session.output)
    check("the exact command comes first", "/config" in overlay, repr(overlay[-400:]))
    before = len(session.output)
    session.send("\r")  # Enter takes the highlighted suggestion
    # The panel is drawn a moment after the key, so the test waits for it
    # rather than for a fixed time.
    session.wait_for("Usage statistics", timeout=5.0)
    accepted = strip(session.output[before:])
    check("enter takes the highlighted suggestion", "Unknown command: /con" not in accepted,
          repr(accepted[-400:]))
    # The command that was taken is the one that runs: `/config` opens the
    # panel of the settings.
    check("the taken command runs", "Status" in accepted and "Config" in accepted,
          repr(accepted[-400:]))
    session.send("\x1b")  # The escape leaves the search of the panel.
    session.read(0.5)
    session.send("\x1b")  # The second one closes the panel.
    session.read(0.5)
    session.send("\x15")  # Ctrl-U clears the line
    session.read(0.4)

    print("commands")
    session.send("/version\r")
    session.read(0.6)
    check("the version command answers", "0.1.0" in strip(session.output), repr(strip(session.output)[-300:]))

    print("plugin")
    session.send("/gree")
    session.read(0.6)
    overlay = strip(session.output)
    check("the command of the plugin is offered", "/greeter:greet" in overlay, repr(overlay[-400:]))
    session.send("\x15")  # Ctrl-U clears the line
    session.read(0.4)

    before = len(session.output)
    session.send("/plugin\r")
    session.read(1.0)
    screen = strip(session.output[before:])
    check("/plugin opens the screen of the plugins", "Plugins" in screen and "Discover" in screen,
          repr(screen[-400:]))
    check("/plugin lists the marketplaces", "Marketplaces" in screen, repr(screen[-400:]))
    # The plugin of the cache stands in the tab of the plugins that are
    # installed, with the marketplace it comes from.
    before = len(session.output)
    session.send("\t")  # The tab of the plugins that are installed.
    session.read(0.6)
    screen = strip(session.output[before:])
    check("/plugin lists the installed plugins", "greeter" in screen and "official" in screen,
          repr(screen[-400:]))
    session.send("\x1b")  # Escape closes the screen.
    session.read(0.6)

    before = len(session.output)
    session.send("/reload-plugins\r")
    session.read(1.0)
    report = strip(session.output[before:])
    check("/reload-plugins reports what it read", "Reloaded:" in report, repr(report[-400:]))
    check("/reload-plugins counts the plugin", "1 plugin" in report, repr(report[-400:]))

    print("btw")
    before = len(session.output)
    session.send("/btw what is this")
    session.read(0.6)
    session.send("\r")
    session.read(1.5)
    panel = strip(session.output[before:])
    check("/btw shows the question", "/btw what is this" in panel, repr(panel[-400:]))
    check("/btw shows the answer", "Answering" in panel or "Error" in panel, repr(panel[-400:]))
    session.send("\x1b")  # Escape dismisses the panel.
    session.read(0.6)

    print("export")
    before = len(session.output)
    session.send("/export\r")
    session.read(0.6)
    session.send("\r")  # The command takes a file name; the second Enter runs it.
    session.read(1.0)
    exported = strip(session.output[before:])
    check("/export says when there is nothing to export",
          "no conversation to export" in exported, repr(exported[-400:]))

    print("vim")
    before = len(session.output)
    session.send("/vim\r")
    session.read(1.0)
    vimmed = strip(session.output[before:])
    check("/vim turns the keys of vim on", "Editor mode set to vim" in vimmed,
          repr(vimmed[-400:]))
    # The escape leaves the insert mode; the keys of vim take over.
    session.send("hello")
    session.read(0.5)
    session.send("\x1b")
    session.read(0.5)
    before = len(session.output)
    session.send("x")
    session.read(0.5)
    frame = session.screen_text()
    check("the vim mode removes a character", "hell" in frame, repr(frame[-300:]))
    session.send("i")  # `i` goes back to the insert mode.
    session.read(0.4)
    session.send("\x15")  # Ctrl-U clears the line.
    session.read(0.4)
    session.send("/vim\r")
    session.read(1.0)

    print("plan")
    # The command takes an argument, so the first Enter takes the suggestion
    # and the second one runs the command.
    before = len(session.output)
    session.send("/plan\r")
    session.read(0.6)
    session.send("\r")
    session.read(1.0)
    planned = strip(session.output[before:])
    check("/plan turns the plan mode on", "Enabled plan mode" in planned, repr(planned[-400:]))
    before = len(session.output)
    session.send("/plan\r")
    session.read(0.6)
    session.send("\r")
    session.read(1.0)
    again = strip(session.output[before:])
    check("/plan says the mode is already on", "Already in plan mode" in again, repr(again[-400:]))

    print("rewind")
    before = len(session.output)
    session.send("/rewind\r")
    session.read(1.0)
    rewind = strip(session.output[before:])
    check("/rewind opens the rewind", "Rewind" in rewind or "rewind" in rewind, repr(rewind[-400:]))
    session.send("\x1b")  # Escape closes the rewind.
    session.read(0.6)

    session.send("/help\r")
    session.read(0.8)
    help_text = strip(session.output)
    check("the help overlay opens", "Shortcuts" in help_text and "general" in help_text,
          repr(help_text[-400:]))
    session.send("\x1b")  # Escape closes the overlay
    session.read(0.6)

    print("transcript")
    before = len(session.output)
    session.send("\x0f")  # Ctrl+O
    session.read(0.8)
    transcript = strip(session.output[before:])
    check("ctrl+o opens the transcript", "Showing detailed transcript" in transcript,
          repr(transcript[-400:]))
    check("the transcript names the toggle", "ctrl+o to toggle" in transcript,
          repr(transcript[-400:]))
    session.send("\x1b")  # Escape closes the transcript
    session.read(0.6)

    print("diff")
    before = len(session.output)
    session.send("/diff\r")
    session.read(1.5)
    diff = strip(session.output[before:])
    check("/diff opens the diff view", "Uncommitted changes" in diff, repr(diff[-600:]))
    check("the diff view names the keys", "esc close" in diff, repr(diff[-600:]))
    session.send("\x1b")  # Escape closes the diff view
    session.read(0.6)

    print("mobile")
    before = len(session.output)
    session.send("/mobile\r")
    session.read(1.5)
    mobile = strip(session.output[before:])
    check("/mobile shows the code of the address", "iOS" in mobile and "Android" in mobile,
          repr(mobile[-600:]))
    check("/mobile names the address", "apps.apple.com" in mobile, repr(mobile[-600:]))
    check("/mobile names the keys", "tab to switch" in mobile, repr(mobile[-600:]))
    session.send("\t")  # The tab switches to the other application.
    session.read(0.8)
    mobile = strip(session.output[before:])
    check("the tab switches the application", "play.google.com" in mobile, repr(mobile[-600:]))
    session.send("\x1b")  # Escape closes the view
    session.read(0.6)

    print("message actions")
    before = len(session.output)
    session.send("\x1b[1;2A")  # Shift+Up
    session.read(0.8)
    actions = strip(session.output[before:])
    # There is no conversation yet, so the bar has nothing to act on.
    check("shift+up without a conversation does nothing", "navigate" not in actions,
          repr(actions[-400:]))

    print("permission mode")
    # The cycle starts from whatever mode the session is in, so collect the
    # modes that go by rather than counting the presses.
    modes = []
    for _ in range(4):
        session.send("\x1b[Z")  # Shift+Tab
        session.read(0.6)
        screen = strip(session.output)
        for name, marker in (("accept", "accept edits on"), ("plan", "plan mode on"),
                             ("bypass", "bypass permissions on")):
            if marker in screen:
                modes.append(name)
    check("shift+tab cycles the permission mode", "accept" in modes and "plan" in modes,
          repr(modes))

    print("clearing the input")
    session.send("some text")
    session.read(0.5)
    before = len(session.output)
    session.send("\x1b")  # Escape
    session.wait_for("Esc again to clear")
    session.read(0.3)  # Let the redraw finish before the second press.
    check("escape asks for a second press", "Esc again to clear" in strip(session.output[before:]),
          repr(strip(session.output[before:])[-400:]))
    before = len(session.output)
    session.send("\x1b")  # Escape again, within the double-press window
    session.read(0.5)
    frame = session.screen_text()
    check("the second escape clears the input", "Esc again to clear" not in frame, repr(frame[-400:]))

    print("exiting")
    before = len(session.output)
    session.send("\x03")  # Ctrl-C
    session.read(0.4)
    check("ctrl+c asks for a second press", "Press Ctrl-C again to exit" in strip(session.output[before:]),
          repr(strip(session.output[before:])[-400:]))
    session.send("\x03")  # Ctrl-C again, within the double-press window
    code = session.wait(5)
    check("the process exits cleanly", code == 0, f"exit code {code}")

    session.close()
    shutil.rmtree(home, ignore_errors=True)

    fullscreen_checks()

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


def fullscreen_checks():
    """Drives a session in the full-screen layout and checks what it draws."""
    print("fullscreen")
    home = tempfile.mkdtemp(prefix="cc-pty-full-")
    # The setup of the session was done already, so that the questions of
    # the setup are not asked.
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"hasCompletedOnboarding": True}, handle)
    env = {**os.environ, "HOME": home, "CLAUDE_CODE_FULLSCREEN": "1"}

    session = Session(env=env)
    session.read(1.5)
    session.send("\r")  # Enter trusts the folder
    session.read(0.8)

    check("the alternate screen is entered", "\x1b[?1049h" in session.output,
          repr(session.output[:200]))
    check("the prompt bar is drawn", "─" * 20 in strip(session.output))

    # The banner stands at the top of the window and the box of the prompt
    # at the bottom of it, the way the reference draws them.
    window = Screen(80, 24)
    window.feed(session.output)
    check("the banner stands at the top of the window",
          "Claude Code" in "".join(window.grid[0]), repr("".join(window.grid[0])))
    check("the box stands at the bottom of the window",
          "? for shortcuts" in "".join(window.grid[-1]), repr("".join(window.grid[-1])))

    print("mascot")
    # The mascot stands in the banner; a click on it makes it move. Where it
    # stands is found on the screen, because the banner puts it in a panel
    # whose width depends on the terminal.
    screen = Screen(80, 24)
    screen.feed(session.output)
    mascot_row = -1
    mascot_column = 0
    for number, line in enumerate(screen.lines()):
        column = line.find("▐▛███▛█")
        if column >= 0:
            mascot_row = number
            mascot_column = column
            break
    check("the mascot is drawn", mascot_row >= 0, repr(screen.text()[:400]))

    before = len(session.output)
    # A click on the mascot: the mouse is reported as a press and a release.
    click = f"\x1b[<0;{mascot_column + 3};{mascot_row + 1}M\x1b[<0;{mascot_column + 3};{mascot_row + 1}m"
    session.send(click)
    session.read(1.5)
    moved = strip(session.output[before:])
    check("the mascot answers the click",
          "▗▟" in moved or "▙███▙" in moved or "▟███▟" in moved,
          repr(moved[-400:]))
    check("the mascot is at rest again", "▐▛███▛█" in moved, repr(moved[-400:]))

    # The conversation scrolls in place: the page keys move the view.
    session.send("!seq 1 40\r")
    session.read(1.5)
    # The screen is looked at, and not the last frame of the bytes: the
    # full-screen layout draws the banner again when the mascot moves, and
    # that drawing holds the banner alone. The screen is what the user
    # sees.
    screen = Screen(80, 24)
    screen.feed(session.output)
    shown = screen.text()
    # The output of the command is cut short after three lines, the way
    # Claude Code cuts it short, and the number of the lines that are left
    # is said.
    check("the output of the command is cut short",
          "… +37 lines (ctrl+o to expand)" in shown, repr(shown[-400:]))
    # The model is asked about the command and its output, the way the
    # reference asks it: the request is on its way while the command is on
    # the screen. The hint is in the footer of the box, and the footer is
    # drawn again with every redraw — so the whole of the output is looked
    # at, not only the last frame.
    check("the model is asked about the command", "esc to interrupt" in strip(session.output),
          repr(shown[-200:]))
    # There is no API to answer, so the request fails and the box of the
    # prompt stands at the bottom of the window again. The wait is for the
    # box itself to come back, and not for the hint to go: the hint is in
    # the footer, and the footer of the last frame is not the last thing
    # that was drawn while the spinner is still going.
    deadline = time.time() + 8
    while time.time() < deadline:
        session.read(0.3)
        screen = Screen(80, 24)
        screen.feed(session.output)
        if screen.text().rstrip().endswith("? for shortcuts"):
            break
    screen = Screen(80, 24)
    screen.feed(session.output)
    shown = screen.text()
    check("the box is at the bottom of the window", shown.rstrip().endswith("? for shortcuts"),
          repr(shown[-200:]))

    before = len(session.output)
    session.send("\x1b[5~")  # PageUp
    session.read(0.8)
    screen = Screen(80, 24)
    screen.feed(session.output)
    shown = screen.text()
    # The conversation is shorter than the window, so there is nothing to
    # scroll: the conversation stays on the screen.
    check("page up keeps the conversation", "! seq 1 40" in shown, repr(shown[-400:]))
    check("the box stays at the bottom", shown.rstrip().endswith("? for shortcuts"),
          repr(shown[-200:]))

    before = len(session.output)
    session.send("\x1b[6~")  # PageDown
    session.read(0.8)
    screen = Screen(80, 24)
    screen.feed(session.output)
    shown = screen.text()
    check("page down scrolls back", "! seq 1 40" in shown, repr(shown[-400:]))

    print("selection")
    # A piece of text is selected with the mouse: the text goes into the
    # clipboard, the way the reference puts it there.
    # The conversation is scrolled back to the top first, so that the banner
    # stands whole on the screen: the banner of the session is at the top of
    # the conversation, and the conversation is longer than the window.
    session.send("\x1b[5~")  # PageUp
    session.read(0.8)
    screen = Screen(80, 24)
    screen.feed(session.output)
    banner_row = -1
    banner_column = 0
    for number, line in enumerate(screen.lines()):
        column = line.find("Claude Code")
        if column >= 0:
            banner_row = number
            banner_column = column
            break
    check("the banner is on the screen", banner_row >= 0, repr(screen.text()[:200]))
    if banner_row >= 0:
        before = len(session.output)
        row = banner_row + 1
        session.send(f"\x1b[<0;{banner_column + 1};{row}M")   # The button goes down.
        session.read(0.2)
        session.send(f"\x1b[<32;{banner_column + 11};{row}M")  # The mouse moves.
        session.read(0.2)
        session.send(f"\x1b[<0;{banner_column + 11};{row}m")   # The button is let go.
        session.read(0.6)
        copied = session.output[before:]
        check("the text that was selected is put in the clipboard",
              "\x1b]52;c;" in copied, repr(copied[-300:]))

    print("resize")
    # The window is made wider and taller: the screen is drawn again, and
    # it is drawn at the new size.
    before = len(session.output)
    session.resize(100, 30)
    session.read(1.0)
    drawn = session.output[before:]
    check("the screen is drawn again after the resize", "\x1b[H" in drawn,
          repr(strip(drawn)[-300:]))
    window = Screen(100, 30)
    window.feed(drawn)
    check("the box is at the bottom of the new window",
          "? for shortcuts" in "".join(window.grid[-1]), repr("".join(window.grid[-1])))
    session.resize(80, 24)
    session.read(0.6)

    session.send("\x03")
    session.read(0.4)
    session.send("\x03")
    session.wait(5)
    check("the alternate screen is left", "\x1b[?1049l" in session.output)

    session.close()
    shutil.rmtree(home, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
