# Claude Code — C++20 port

![Claude Code, in C++](assets/cover.jpg)

A C++20 rewrite of the [Claude Code](https://claude.com/claude-code) CLI, built on
[ai-sdk-cpp](https://github.com/mhdimo/ai-sdk-cpp) and
[FTXUI](https://github.com/ArthurSonzogni/FTXUI).

The program is run as `cc++`.

## Installing

```sh
scripts/package.sh                       # writes dist/claude-code-cpp-<version>-<arch>.tar.gz
scripts/install.sh ~/.local/bin/cc++     # puts it where the reference puts it
```

The install follows the layout of the native installer of the reference: the
binary itself lives in `~/.local/share/claude/versions/<version>` and
`~/.local/bin/cc++` is a symlink to it, so that the two agree about where the
program is. `~/.local/bin` has to be in your `PATH`.

There is a man page in `docs/cc++.1` and a Homebrew formula in
`packaging/homebrew/cc-cpp.rb`.

## Building

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(sysctl -n hw.ncpu)
```

The build fetches three dependencies: `ai-sdk-cpp` (the model SDK), `Catch2`
(the test framework) and `FTXUI` (the terminal toolkit).

Requirements: a C++20 compiler, CMake 3.24+, Boost (headers only) and OpenSSL.

### Making the build quicker

The port is 250 files of its own and 82 of tests, and the headers of Boost
and of the SDK are heavy. Three things make the loop of the day quicker, and
the build finds the first two by itself:

| what | how | what it buys |
|---|---|---|
| the cache of the objects | `brew install ccache` | the second build of a change is a link, not a compile |
| the build that changes nothing | `-G Ninja` | 0.02 s instead of 2.15 s |
| the code of the day | `-DCMAKE_BUILD_TYPE=Debug` | the files are compiled with `-O0`, so three times quicker |
| the debug information of its own | `-DCC_SPLIT_DWARF=ON` | the objects are half the size and the link is quicker |

Measured on a ten-core machine, with `cli/app.cpp` (the heaviest file of the
tree, 7800 lines) as the example:

| build | before | after |
|---|---|---|
| nothing changed | 2.15 s | 0.02 s |
| one file changed | 12.3 s | 9.8 s, and 0.03 s the second time |
| the heaviest header changed | 14.0 s | 14.0 s, and 0.02 s the second time |
| everything, from nothing | 1 min 48 s | 1 min 48 s |

The clean build does not get quicker: the same work is done. The loop of the
day does, because the second build of a change is a link.

## Running

```sh
./build/src/cc++                   # the interactive prompt
./build/src/cc++ "explain this"    # one prompt, then exit
./build/src/cc++ -p "explain"      # print the answer and exit
./build/src/cc++ --help            # every flag
```

`ANTHROPIC_API_KEY` must be set. The session reads the same settings files as
the original: `~/.claude/settings.json`, `.claude/settings.json` and
`.claude/settings.local.json`, and the same memory files (`CLAUDE.md`).

The first time the session runs in a folder it asks whether you trust it, the
way the original does, and remembers the answer in `~/.claude.json`.

### The same data as the original

The port and the original can be used one after the other: they read and write
the same files.

| What | Where |
|---|---|
| the settings | `~/.claude/settings.json`, `.claude/settings.json`, `.claude/settings.local.json`, the managed settings of the system |
| the configuration of the user | `~/.claude.json` (and the legacy `~/.claude/.config.json`) |
| the memory | `~/.claude/CLAUDE.md`, `CLAUDE.md` of the folder, `~/.claude/rules/` |
| the conversations | `~/.claude/projects/<folder>/<session>.jsonl` |
| the history of the prompts | `~/.claude/history.jsonl` |
| the statistics | `~/.claude/stats-cache.json` |
| the output styles | `~/.claude/output-styles/` |
| the agents, the commands, the skills | `~/.claude/agents/`, `~/.claude/commands/`, `~/.claude/skills/` |
| the plugins | `~/.claude/plugins/` |
| the servers of MCP | `~/.claude.json`, `.mcp.json` of the folder |
| the keys the user has bound | `~/.claude/keybindings.json` |
| the copies of the files that changed | `~/.claude/file-history/` |

`CLAUDE_CONFIG_DIR` moves the folder of the configuration, the way it moves
it for the original.

## Layout

| Directory | What lives there |
|---|---|
| `src/core/` | Errors, identifiers, the conversation message types, the version |
| `src/types/` | The shared vocabulary — the permission modes and rules |
| `src/permissions/` | The permission engine: rules, modes, the decision pipeline |
| `src/tools/` | The `Tool` interface and the builtin tools |
| `src/query/` | The engine that runs the conversation |
| `src/ui/` | The terminal layer: theme, terminal, markdown, diff, prompt box, spinner, dialogs |
| `src/cli/` | Argument parsing and the REPL |
| `src/commands/` | The slash commands |
| `src/config/` | Settings files, the memory files, the trust decision |
| `src/session/` | The transcript on disk, resuming and compaction |
| `src/agents/` | The sub-agent definitions, loaded from `.claude/agents/` |
| `src/hooks/` | The hook runner, for the commands registered around tool calls |
| `src/mcp/` | The MCP client and the tools, resources and prompts the servers offer |
| `src/utils/` | Strings, paths, files and JSON helpers |
| `scripts/` | The installer and the script that builds the archive of a release |
| `packaging/` | The Homebrew formula |
| `docs/` | The man page |

The notes of the releases are in `CHANGELOG.md`, and the program shows them
in the banner and in the `/release-notes` command.

### The address of the API

The address of the API, the token and the headers are read from the
environment, and the `env` block of `~/.claude/settings.json` is applied to
the environment of the process — the reference does the same, so a gateway
that is written down in the settings is reached. The variables of the user
are applied before the folder is trusted; the ones of the folder wait for the
trust.

The tag of the context window is taken off the name of the model before it
is sent: `claude-opus-5-5` travels as `claude-opus-5-5`, and the
window of a million tokens is asked for with a header of its own.

### The terminal layer

The UI is built on FTXUI's screen and layout engine, but the frame loop is our
own, so that the transcript can be committed to the terminal's scrollback the
way Claude Code does it:

- `ui::Terminal` splits the screen into a **transcript** region, which is
  written once and stays in the scrollback, and a **dynamic** region, which is
  redrawn in place. The dynamic region holds the answer as it grows, the
  progress of the tool that is running — including the output of a shell
  command as it arrives — the spinner and the prompt box. The prompt box is
  pinned to the bottom of the screen: the empty lines above it are part of the
  region, so they are erased and drawn with it.
- `ui::Theme` holds the palette. The values are ported from the reference's
  `src/utils/theme.ts`, so the dark and light variants look the same.
- `ui::markdown`, `ui::diff`, `ui::prompt_input`, `ui::spinner`,
  `ui::tool_display`, `ui::permission_prompt`, `ui::question_dialog`,
  `ui::select`, `ui::trust_dialog`, `ui::help_overlay`, `ui::transcript_view`,
  `ui::diff_view`, `ui::message_selector` and `ui::status_line` render the
  individual pieces.

### The files the session changes

Every file the session edits is copied aside into
`~/.claude/file-history/<session id>/`, under a name that is the hash of its
path and the version of the copy. A rewind puts the files back the way they
were, the way the reference puts them back.

### The keys the user binds

The file `~/.claude/keybindings.json` holds the keys the user has bound to
something else:

```json
{
  "bindings": [
    { "context": "Global", "bindings": { "ctrl+g": "app:toggleTranscript" } },
    { "context": "Chat", "bindings": { "ctrl+t": "app:toggleTodos" } }
  ]
}
```

The `/keybindings` command writes the file with the default bindings when it
does not exist yet and opens it in the editor.

### What the small pieces do

The port follows the reference down to the details that show on the screen:

- the prompt box draws an example command while the input is empty — one of
  the eight the reference samples — and drops it once the first message has
  been sent; the character under the caret is drawn in reverse video, and the
  `❯` is dimmed while a request is running;
- the input is wrapped to the width of the terminal, three columns narrower
  than the box;
- the `?` menu names the newline instruction, and its columns are two columns
  apart;
- the commands are offered in the order the reference offers them: the exact
  name first, then the names that start with it, the shorter ones before the
  longer ones, and the whole list sorted by name when nothing has been typed;
- the spinner sweeps a band of three columns across the verb, turns red when
  the answer stops growing, picks a new verb for every turn and uses the
  verbs the user configured;
- the marker in front of a tool call blinks every 600 ms while the tool
  runs;
- the output of a shell command keeps its standard error apart, shows it in
  its own colour, and is pretty-printed when it is JSON; a command that
  printed nothing says `(No output)`;
- the error of a tool is named `Error: …`, cut to ten lines, and says how
  many lines are left;
- a table keeps a margin of four columns around it, centres its cells
  vertically and falls back to a list of key and value pairs when its rows
  would grow too tall;
- the help overlay truncates the description of a command rather than
  wrapping it, and sizes its list to the height of the terminal;
- a picker that has more entries than fit shows an arrow at the end that
  says there is more to see;
- a block of code is coloured the way `cli-highlight` colours it: a keyword
  is blue, a string is red, a comment is green, and the words of a language
  come from highlight.js, the highlighter the reference uses;
- a reference to a GitHub issue (`owner/repo#123`) becomes a link;
- a heading written as a line of text followed by `---` is a heading;
- the spinner says `thinking` while the model thinks and `thought for 8s`
  afterwards, and names the next task of the todo list under itself;
- `/btw` asks a question in a thread of its own: the tools are taken away
  and the turn ends as soon as the model answers, so the conversation of the
  session is left alone;
- `/plan` turns the plan mode on, and `/rewind` opens the rewind — the same
  view the double press of Escape opens;
- `/export` writes the conversation to a file in the folder of the session,
  named after the day, the time and the first thing the user said.

### The views

The reference draws its whole screen again whenever something changes; the
port writes the transcript to the terminal's scrollback instead, so the
full-screen views of the reference are separate widgets here:

| Key | View | What it shows |
|---|---|---|
| `Ctrl+O` | the transcript | the last thirty messages of the conversation, with the older ones behind the rule that names their number; `Ctrl+E` brings them back |
| `Shift+Up` | the message actions | a bar that copies the selected message, copies the path of the file a tool call touched, or puts a prompt back into the input |
| `Escape` `Escape` | the rewind | the prompts of the session, to rewind the conversation to one of them or to summarise from there |
| `/diff` | the diff | the files that changed in the working tree, and the diff of the one that is selected |
| `/mobile` | the code of the application | the code of the address of the mobile application; the tab switches between the two applications |

### The panels of the menus

The reference opens `/config`, `/status` and `/usage` in one panel with three
tabs — the state of the session, the configuration and the limits of the
plan. The tab key and the arrow keys on the side move from one tab to the
next; what is typed in the tab of the configuration filters the settings.

| Command | Panel | What it shows |
|---|---|---|
| `/config` | the settings | the settings of the session, with the value of each of them |
| `/status` | the state | the version, the name and the id of the session, the folder, the account, the address of the API, the model, the servers of MCP and the files the settings are read from |
| `/usage` | the limits | the limits of the plan, with the share of each that is used |
| `/stats` | the statistics | the sessions, the messages, the tokens, the streaks and the models of the sessions of the folder; `r` cycles through the ranges of days, and the tab of the models draws the chart of the tokens of the days |

### The account of the user

The port logs in the way the reference does. `/login` asks whether the user
has a Claude account or an Anthropic Console account, opens the browser at
the page of the provider, and waits for the code that comes back to a small
server the port listens on — the loopback address only. The tokens are kept
in the keychain of the machine, and the file `~/.claude/.credentials.json`
is used when there is no keychain. `/logout` forgets them.

The tokens are sent with the beta the reference sends them with, and the
limits of the plan of the user are asked for over the same API by `/usage`.

The "what's new" of the start of the session comes from the changelog of
the reference, which is fetched in the background and kept in
`~/.claude/cache/changelog.md`.

The banner of the start of the session has the three layouts of the
reference: the panel with the mascot, the greeting and the feeds when the
notes of the release are new to the user, the rounded box of a terminal
narrower than seventy columns, and the condensed mascot with three lines of
text once the notes have been seen. `CLAUDE_CODE_FORCE_FULL_LOGO=1` asks for
the full panel.

### The patterns of the hooks

The matcher of a hook is a pattern in the grammar of JavaScript, the way the
reference reads it: `^Write.*` and `^(Write|Edit)$` select the tools they
name, and a plain name — `Write` — selects that tool and nothing else. The
port has its own reader for that grammar, so the patterns JavaScript accepts
are the ones the port accepts: the groups may be named (`(?<tool>Write)`),
the pattern may look ahead and behind (`(?<=foo)bar`), and the references
to a group may be used (`\1`, `\k<name>`).

### The plugins

A plugin is a folder of commands, of agents and of hooks. The port reads them
from the cache of the plugins, in
`~/.claude/plugins/cache/<marketplace>/<plugin>/<version>/`, and the settings
say which of them are on.

The parts a plugin brings are the ones the reference reads:

- the markdown files of the folder `commands` are slash commands, named after
  the plugin: the file `build.md` of the plugin `my-plugin` is the command
  `/my-plugin:build`;
- the markdown files of the folder `skills` are commands too, and a skill
  named `SKILL.md` is named after the folder it is in;
- the markdown files of the folder `agents` are sub-agents;
- the file `hooks/hooks.json` holds hooks, in the same shape as the settings
  hold them;
- the file `.mcp.json` holds MCP servers, named after the plugin: the server
  `tools` of the plugin `my-plugin` is the server `plugin:my-plugin:tools`.

A plugin is told where it lives, so that it can name the files it brings:
`${CLAUDE_PLUGIN_ROOT}` becomes the folder of the plugin and
`${CLAUDE_PLUGIN_DATA}` the folder of its data.

A plugin may declare the values it needs in the `userConfig` of its
manifest. The user is asked for them when the plugin is turned on, and the
values are kept in the settings of the user; the plugin sees them as
`${user_config.KEY}`.

`/plugin` opens the screen of the plugins: the ones that are installed and
the ones the marketplaces offer, and what can be done with the one that is
picked. `/plugin marketplace add <source>` adds a marketplace, `/plugin
marketplace refresh <name>` fetches it again — a repository is cloned, a
file is downloaded — and `/reload-plugins` reads the plugins again and
reports what the session holds.

### The plan mode

`/plan` turns the plan mode on. In that mode the model may only read, and
the one file it may write is the plan of the session, in
`~/.claude/plans/<adjective>-<verb>-<noun>.md` — the name is made of three
words, the way the reference names it. The prompt of the session names the
file, and the model writes the plan into it as it works.

When the model calls `ExitPlanMode`, the plan is shown to the user with the
question of whether the work may start. `/plan` shows the plan of the
session as well, and `/plan open` opens it in the editor.

### The keys of vim

`/vim` turns the keys of the prompt box into the keys of vim, and back again.
The way the keys are read is remembered in `~/.claude.json`, so the choice
holds for the next session.

In the insert mode the prompt box takes the keys as it always does; the
escape leaves the mode, the way vim does it, and the marker `-- INSERT --`
tells which mode the box is in. In the normal mode the keys are the commands
of vim: the motions (`h`, `j`, `k`, `l`, `w`, `b`, `e`, `0`, `$`, `gg`, `G`),
the operators (`d`, `c`, `y` with a motion), `x`, `r`, `p`, `P`, `u`,
`J`, `o`, `O`, `>>`, `<<`, the finds (`f`, `F`, `t`, `T`) and the counts in
front of them.

### The agents that ship with the CLI

Besides the general-purpose agent, the port has the agents the reference
has: `Explore`, a fast agent that only reads and reports what it finds,
`Plan`, an agent that designs an implementation, and `statusline-setup`,
which writes the status line of the user.

### The full-screen layout

The reference has a second layout, which it switches on with
`CLAUDE_CODE_NO_FLICKER=1`: the alternate screen of the terminal becomes the
session's own window, the conversation scrolls in place, and the prompt box
and the status line sit at the bottom of it.

The port has the same layout, and the same environment variable; it also
accepts the plainer `CLAUDE_CODE_FULLSCREEN=1`:

```sh
CLAUDE_CODE_FULLSCREEN=1 claude
```

In that layout the transcript is kept in memory and the whole screen is
drawn again whenever something changes. The page keys scroll the
conversation, the mouse is reported, and the mascot answers a click: it
crouches and springs up with its arms raised, or it looks around.

The status line the user configures with `statusLine` in the settings file
runs as a shell command; it is given the state of the session as JSON on its
standard input, and whatever it writes is shown below the prompt box.

The prompt box stays usable while a turn is running. A message the user
sends then is queued and joins the turn that is running — the model sees it
alongside the results of the tools that just ran — rather than starting a
turn of its own.

### The commands of the user

The markdown files in `~/.claude/commands` and in `.claude/commands` of the
project are read as slash commands, the way the reference reads them. The
frontmatter of the file names the description and the hint about the
arguments; the body of the file is the prompt that is sent to the model, with
`$ARGUMENTS`, `$0` and `$1` replaced by what the user typed. A file in a
folder of its own gets a name of its own: `git/commit.md` is `/git:commit`.

The commands of the user are offered in the suggestions, marked `[custom]`,
and are listed in the `custom-commands` tab of the help overlay.

A command that is expanded into a message tells the model which command it
is, the way the reference does:

```
<command-message>commit</command-message>
<command-name>/commit</command-name>
<command-args>the message</command-args>
```

### The MCP servers

The servers are read from the same places the reference reads them from: the
`mcpServers` key of the global configuration file `~/.claude.json` (the scope
of the user), the `mcpServers` key of the folder of the session in the same
file (the local scope) and the `mcpServers` key of the `.mcp.json` file of the
folder (the scope of the project). The more specific scope wins.

The servers of the project are only used when the folder is trusted: a
repository must not be able to start a program of its own without the user
saying that they trust it. The reference asks about every server of the
project on its own; the port asks about the folder once, the way it asks
before it reads the files of the folder.

The servers connect in the background: the session is ready to talk before a
server that takes its time is up, the way the original is. The log of a
server is piped, not printed, so it never lands in the middle of the
interface; what the server wrote is kept and is part of the reason a server
that could not be used carries.

The tools of a server are offered to the model under the name
`mcp__<server>__<tool>`. The resources of a server are read with the
`ListMcpResourcesTool` and `ReadMcpResourceTool` tools, and its prompts
become slash commands: the prompt `review` of the server `my-server` is
`/my-server:review (MCP)`, and the arguments the user typed are given to the
prompt by name. The `/mcp` command lists the servers, whether they are
connected, and what they offer.

### The query engine

`query::Engine` owns the conversation. Each turn:

1. the user's message is recorded;
2. the model is streamed, and the text is rendered as markdown as it arrives;
3. the tools the model asks for are run, each one through the permission
   pipeline first;
4. the results are fed back, and the loop continues until the model answers
   without asking for a tool.

The tool result the model sees is byte-for-byte what the reference sends, which
is why the engine drives the Anthropic API directly rather than going through
the SDK's own tool loop.

## Tests

```sh
./build/tests/claude_code_tests    # the unit tests
python3 tests/pty_smoke.py          # drives the CLI through a pty
python3 tests/compare_screens.py    # the screens of the port against the reference's
python3 tests/memory_bench.py       # how much memory a session takes
```

The unit tests cover the tools, the permission rules, the markdown and diff
renderers, the prompt box, the overlays, the commands of the session, the
reader of the patterns of the hooks, the plugins and the fidelity of the
smaller details — the example command in the prompt box, the shortcuts menu,
the help overlay, the picker, the code of the mobile application, the output
of a shell command and the error of a tool.
The pty test checks what the CLI actually draws: the startup logo, the
trust dialog, the prompt bar, typing, the shortcuts menu, the suggestions
overlay, the help overlay, the transcript view, the diff view, the code of the
mobile application, the command of a plugin, the side question, the plan
mode, the rewind, the keys of vim, the permission mode cycle, the
double-tap escape and a clean exit.

The screen comparison drives the port and the Claude Code CLI that is
installed on the machine through the same steps — the trust dialog, the
banner, the list of the commands, the panel of the configuration — and
compares the screens they draw, cell by cell, with the colours as well. The
installed CLI is an older build of the same product than the tree the port
was written from, so a difference that it reports is checked against the
tree before it is taken as a divergence of the port.

The benchmark starts both CLIs under a pty and watches the resident memory of
the process and of its children:

| session | settled | the process alone |
|---|---|---|
| `claude` (the TypeScript reference) | 341 MB | 278 MB |
| `claude` (the port) | 91 MB | 34 MB |

The peak of the port is the `fork()` of the MCP servers: the children inherit
the pages of the parent until they `exec`, and the number falls back once they
have.

## What is not ported yet

- The remote control bridge: `/teleport` and `/share` are stubs in the
  reference itself, and the port leaves them out.
- The commands that need a wizard of their own or an account of Anthropic:
  `/install-github-app` (the wizard of the GitHub Actions), `/upgrade` (the
  page of the plan is opened, the login that follows is left out),
  `/feedback` (the report is sent, the questions of the form are not asked),
  `/voice`, `/tag`, `/passes`, `/brief`, `/heapdump`,
  `/rate-limit-options`, `/remote-control`, `/remote-env`, `/think-back`,
  `/web-setup`, `/install`, `/ultraplan` and `/ultrareview`. The reference
  keeps most of them out of its own list — they are behind a feature flag
  or an account — and the port keeps them out of its list as well, so that
  every command that is offered works.
- `/insights` reads the logs of the sessions of the machine and asks the
  model to make sense of them; the port leaves the command out.
- The sandbox runs the commands of the session under the seatbelt of macOS,
  the way the reference runs them under the sandbox runtime of Anthropic.
  The seatbelt is a tool of macOS, so the sandbox does nothing on the other
  systems, and the rules of the reference that name a runtime of its own
  are left out.
- The group of the servers of `claude.ai` in the `/mcp` panel: the reference
  fetches those servers from the API of Anthropic at run time, and the port
  has neither the fetch nor the transport of them.
- The secrets of the options of a plugin are kept in the settings of the
  user; the reference keeps them in the keychain of the machine.
- The state "needs authentication" of a server of MCP is worked out by the
  session — a server that is reached over the network, that has failed and
  that has no token — instead of being read from the answer of the server.
  A server that is merely down is shown as "needs authentication" rather
  than "failed".
- The full-screen layout is the port's own: the reference writes the
  conversation in the scrollback of the terminal. The banner of the session
  is one row too tall for a window of twenty-four rows when the list of the
  commands is open, and the top of it is cut. `CLAUDE_CODE_SCROLLBACK=1`
  asks for the layout of the reference.
