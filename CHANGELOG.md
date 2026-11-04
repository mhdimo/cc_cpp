# Changelog

The notes of the releases of the C++ port of the Claude Code CLI. The
program is run as `cc++`.

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
