# Changelog

The notes of the releases of the C++ port of the Claude Code CLI. The
program is run as `cc++`.

## Unreleased

- The terminal toolkit is **maya** now, and FTXUI is gone. Its input parser
  reads the keys, the mouse and the pastes of the terminal, and its platform
  layer knows the size of the screen. The screen itself is still drawn by the
  port, so that the conversation stays in the scrollback of the terminal the
  way the reference puts it there: the UI is the same as it was.
- The keys the port understands are the same as they were: the control of a
  letter is the letter with the control held, the command of the keyboard of
  Apple with the backspace is still the backspace with the command held, and
  a sequence that the terminal sent and the port does not know is dropped
  instead of being typed into the prompt as a question mark.
- The port asks for C++23, which maya needs.

- The conversation is in the scrollback of the terminal, the way the
  reference puts it there, and the box of the prompt stands under it instead
  of being pinned to the bottom of the screen: the user scrolls through the
  conversation with the scrollbar of the terminal. The full-screen layout is
  still there, and `CLAUDE_CODE_FULLSCREEN=1` asks for it.
- The screen of the full-screen layout is written again only where it is
  different, and the conversation is moved by the terminal itself when it
  grows or when the view is moved: a screen that is painted row by row
  flickers, and the banner with the mascot in it flickers most of all.
- The region of the prompt is wiped in the rows it stands in and not in the
  rows below it, and the text that was on a row before it is taken away with
  it: the frames of the old region do not stay behind in the scrollback. The
  wipe begins at the first column of the row, so that the text of the
  transcript under the region is not left behind either.
- The terminal is resized: the region of the prompt is taken down and drawn
  again where it stands, and it is drawn once — the box of the old width was
  drawn a second time and left behind on the screen. The transcript is
  in the scrollback of the terminal, and the terminal wraps it again by
  itself; the port wrote it again at every resize, so the same text was on
  the screen twice and the frames of the old screen piled up in the
  scrollback.
- The models of the session are the ones the settings name: the list of the
  `/model` picker is read from `ANTHROPIC_DEFAULT_SONNET_MODEL` and the
  other variables of the environment, the way a gateway that serves another
  model than Anthropic names them.
- The banner of the first run of the session is the plain one: the frame of
  the panel and the two feeds — the recent activity and the notes of the
  release — are not drawn, and the notes are taken as seen all the same, so
  that the panel does not come back at the next run.
- The list of the suggestions is built only when the text of the input has
  changed, and not twenty times a second: a session that waits for the user
  no longer burns a third of a core while it waits.
- `scripts/bench.py` measures the memory and the CPU of the port and of the
  reference the same way, and draws the graph of the two.
- The option of the keyboard of Apple takes the word before the cursor away
  and the command takes the line away, the way they do in the text fields
  of the system.


## 0.1.0

- The first release of the port: the whole CLI, the tools, the panels of
  the menus, the full-screen layout and the shell of the session.
- The session runs its own shell, so that `cd` in one command is felt by
  the next one; the folder of the shell is taken back when it walks out of
  the folder of the session.
- The context of the session is the size of the last call of the model, and
  the session compacts itself when the context is nearly full.
- The output of a tool is written to the transcript whole, when the tool
  ends — it is never streamed as it arrives.
- The panels of the plugins, of the servers of MCP and of the settings are
  the panels of the reference.
- The install script puts the program where the native installer of the
  reference puts it: `~/.local/share/claude/versions/<version>` and a
  symlink in `~/.local/bin`.
