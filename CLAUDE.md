# CLAUDE.md

Guidance for working on this repository.

## What this is

A C++23 port of the Claude Code CLI. The reference implementation is the
TypeScript source at `~/Downloads/claude-code-main/src/` — **port from it
rather than inventing**: the same tool names, the same input schemas, the same
result strings, the same colours and the same terminal layout. When the
reference's exact wording cannot be recovered from the source, write an
equivalent and say so in the summary.

## Building and testing

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(sysctl -n hw.ncpu)
./build/tests/claude_code_tests     # unit tests
python3 tests/pty_smoke.py          # drives the CLI through a pty
```

The build fetches `ai-sdk-cpp`, `Catch2` and `maya` with `FetchContent`.

## Conventions

- 4-space indent, 120-column lines, `namespace cc::…`.
- `///` doc comments on every public entity, in the voice of the neighbouring
  files.
- Boost.JSON 1.90 is stricter than older releases:
  - `array::push_back` does not take a `std::string` — use `emplace_back` or
    `boost::json::value(s)`;
  - `value::is_int()` does not exist — use `is_int64()`;
  - `value::empty()` does not exist;
  - `boost::json::error_code` does not exist — use `boost::system::error_code`.
- `c++` 20, no exceptions across the UI boundary where avoidable: the tools
  report failures by throwing `cc::ToolError`, which the engine turns into an
  error result the model can see.

## The comments carry no signature

A comment that explains a decision — one that says *why* and not *what* —
is written and left there. It carries no signature: the agent that writes it
never signs itself, and the name of the author is not written into the code
either.

```cpp
// The reference reads the usage of the last message that carries one, and
// the sum would fill the bar at once.
```

A comment that only says what the next line says is not worth writing.

The commits of the tree follow the same rule of ownership: one commit per
change, signed by the author, and with no co-author at all — neither a
`Co-authored-by` trailer nor a `Co-authored-by` in the middle of the message.
The rules of the whole are in `CONTRIBUTING.md`.

## The terminal layer

The terminal is read by **maya**: its `maya::InputParser` turns the bytes of
the terminal into keys, mouse events and pastes, and its platform layer knows
the size of the screen. The screen itself is drawn by `ui::Terminal`, which
splits it in two:

- the **transcript** region is written once and stays in the terminal's
  scrollback (`write_static`);
- the **dynamic** region — the prompt box, the spinner, the dialogs — is
  redrawn in place (`render_dynamic`).

The region is drawn again only when its content has changed: the terminal
keeps what it drew last and skips the drawing when the lines are the same,
so that the redrawing does not flicker.

The work that goes with the drawing is another matter. The prompt box is
drawn twenty times a second — the loop waits fifty milliseconds for a key —
so everything that is built for it is built twenty times a second too. The
list of the suggestions is the dearest of them: it is walked over every
keystroke and built out of the commands of the session, and it is built
**only when the text of the input has changed**, or when the commands of the
session have changed (`App::suggestions_dirty_`). A session that waits for
the user must not burn a third of a core while it waits; `scripts/bench.py`
measures what it burns.

Anything that writes to the transcript while the dynamic region is up must go
through `write_static`, which takes the dynamic region down and puts it back —
the region's lines are kept across the erase, so it comes back as it was.

The answer of the model grows, so it is neither of the two: it is drawn with
`set_live_lines` and it stands right under the text that came before it — in
the classic layout in front of the dynamic region, in the full-screen layout
as a part of the transcript, which is kept in memory. `commit_live` writes the
answer that grew to the transcript and takes the answer that was growing away,
in one drawing, so that the answer is never on the screen twice.

Everything that goes to the transcript goes through the terminal, the banner
of the session included: the terminal keeps the row the cursor stands on, and
the dynamic region is drawn from that row. A banner that went to the standard
output left the terminal thinking the cursor was still at the top of the
screen, and the region of the prompt box was drawn over the banner — the user
never saw it.

The terminal knows when its input has closed (`Terminal::closed()`), so that
the loops that wait for a key end instead of waiting for one that will never
come. A session that waits for a key on a terminal that is gone spins for
ever.

The dynamic region stands where the cursor is, under the text that came
before it. It is not pinned to the bottom of the screen: the conversation is
in the scrollback of the terminal, and the user scrolls through it with the
scrollbar of the terminal. The region must never be taller than the screen —
a region that is taller cannot be erased, and the frames pile up in the
scrollback — so the answer that does not fit is written to the transcript, the
way the reference scrolls it up.

The region is wiped in the rows it stands in and not in the rows below it:
the text that was on a row before it is taken away with it, and the rows the
user has scrolled up to are left alone. The wipe is measured from the cursor,
which stands at the end of the last row of the region.

Only the thread that owns the terminal may draw. The coroutine of the engine
is resumed on a thread of the SDK's own (`FutureAwaitable` resumes it where
the future is waited for), so the hooks of the engine must not touch the
terminal: they put their work in the queue of the app (`post_ui`), and the
thread that owns the terminal does it (`drain_ui`). The hooks that ask the
user something wait for the answer (`run_on_ui`).

The raw mode setup keeps `OPOST` on, so a newline still returns the carriage.

The port decodes a bracketed paste itself, and not the toolkit: the markers
and the text they wrap never reach the key parser, so a pasted newline cannot
be mistaken for the Enter that commits the prompt. A paste arrives as one
`Key::Type::Paste`.

The paste is cut out of the stream before the parser of maya sees it, and that
is on purpose. The parser of maya would read a burst of plain bytes as a paste
of its own, and it would wait two seconds for the end of a paste that never
comes; the port waits half a second, and a burst of text that the terminal
sent without the markers is text, the way it was before. The bytes are handed
over to the parser in small pieces for the same reason: a run of text is text.

The keys are read by maya and translated by the port, and the translation
keeps the rules of the reference:

- the control of a letter is the letter itself with the control held, and
  the key of the UI says which letter it is (`Key::Type::CtrlC` and the
  like);
- the command of the keyboard of Apple with the backspace arrives as the
  control of the letter H with the option held — the terminal tells the two
  apart by nothing at all — and the port reads it as the backspace with the
  command held;
- a sequence that the terminal sent and the UI does not know is dropped and
  not typed into the prompt: the parser of maya hands such a sequence over
  as a question mark, and the question mark is not something the user typed;
- the option is only read with the letters, the digits and the space; the
  option with anything else is a key of its own.

The bytes that were held back for a paste marker that never came are read by
the port and not by the parser: a lone escape is the escape key, and the
escape that was held back behind another escape is the escape with the option
held. The parser is left with nothing, so that a sequence which arrives after
the silence is read as the text it is.

The footer of the prompt box has two columns: the hint on the left and the
notification on the right. The message about the escape ("Esc again to
clear") and the name of the paste ("Pasting text…") are raised in the
notification, the way the reference raises them; they do not take the place
of the hint.

The reference redraws its whole screen whenever something changes; the port
writes the transcript to the scrollback instead. The full-screen views of the
reference are therefore separate widgets, each with its own `run` loop:
`ui::TranscriptView` (Ctrl+O), `ui::DiffView` (`/diff`),
`ui::MessageSelector` (the rewind) and `ui::Select` (the pickers).

The transcript shows the last thirty messages of the conversation, the way
the reference shows them; the older ones are hidden behind the rule that
names their number, and Ctrl+E brings them back.

The reference has a second layout as well — the full-screen one, which the
alternate screen of the terminal turns into the session's own window. The
port has it too, and it is asked for with `CLAUDE_CODE_FULLSCREEN=1` or with
`CLAUDE_CODE_NO_FLICKER=1` (the reference's own variable). The scrollback
layout is the one of the reference, and it is the one the port draws by
default. In the full-screen layout the transcript is kept in memory by
`ui::Terminal`, the screen is written again only where it is different, the
conversation is moved by the terminal itself when it grows or when the view
is moved, the page keys scroll the conversation, and the mouse is reported so
that the mascot can answer a click.

The user's own status line runs as a shell command: `ui::run_status_line`
gives it the state of the session as JSON on its standard input and shows
whatever it writes below the prompt box.

## The MCP servers

The servers are read from `~/.claude.json` (the scope of the user and the
scope of the session) and from the `.mcp.json` file of the folder (the scope
of the project); the more specific scope wins. The servers of the project are
only used when the folder is trusted — a repository must not be able to start a
program of its own on its own. The trust question is therefore asked before
the session is set up, not after.

The prompts of a server are slash commands: the command is
`mcp__<server>__<prompt>` and the user sees `<server>:<prompt> (MCP)`. The
`(MCP)` marker is the second word of the command, so the arguments of the
command start after it — `cc::parse_slash_command` splits the command the way
the reference does.

The servers connect in the background: the session must be ready to talk
before a server that takes its time is up. The reference does the same — a
server never holds up the first turn — and the tools of the servers are asked
for again at the start of every turn, so a server that connects late is still
part of the next one.

The standard error of a server is piped, not inherited: the log of a server
would otherwise land in the middle of the interface. The reference starts its
servers with `stderr: 'pipe'` for the same reason, and keeps what the server
wrote, so the reason a server could not be used carries it.

The servers of the project are only used when the folder is trusted: a
repository must not be able to start a program of its own without the user
saying that they trust it. The trust question is therefore asked before the
session is set up, not after.

The servers of the `.mcp.json` file of the folder are asked for one by one,
and the answer is remembered in the settings of the folder — the keys
`enableAllProjectMcpServers`, `enabledMcpjsonServers` and
`disabledMcpjsonServers` say which ones may run without the question.

## The query engine

`query::Engine` drives the Anthropic API directly rather than going through the
SDK's own tool loop, because the SDK re-serialises a tool's result as JSON and
the model has to receive the reference's exact text. The engine:

1. records the user's message;
2. streams the model and renders the text as markdown as it arrives;
3. runs the tools the model asks for, each through the permission pipeline;
4. feeds the results back and repeats until the model answers without asking
   for a tool.

A tool attaches a structured payload to `ToolResult::tool_use_result` when the
UI should show a summary of the result rather than its raw text — `Read 42
lines`, `Found 3 files`, `Done (3 tool uses · 1.3k tokens · 5s)`. The engine
only fills the field in when the tool left it null.

The output of a tool is written to the transcript **whole**, when the tool
ends — it is never streamed as it arrives. The reference cuts a long output
short and leaves the rest behind "ctrl+o to expand"; the port does not, at
the wish of the one who uses it, so the transcript carries the whole of it
and the expand has nothing left to reveal. The line of the tool that is
running stays in the dynamic region, where its marker can blink, and it is
handed over to the transcript before the result is written.

## The shell of the session

The commands of the shell run in a shell that stays alive:
`cc::tools::ShellSession` keeps a bash of its own, so `cd` in one command is
felt by the next one, the way the reference feels it. The shell is asked for
the end of a command with a marker of its own, so the output is cut off where
the command ends and the exit status and the folder of the shell are read
from the marker.

When the shell walks out of the folder of the session, the reference takes it
back and says so — *Shell cwd was reset to `<path>`*. The port does the same,
and the note is a part of the error output of the tool rather than a message
of its own.

The session tells the model what the shell did, so that the model knows where
the shell stands.

## The context of the session

The size of the context is the size of the **last** call of the model, not
the sum of every call: the reference reads the usage of the last message
that carries one, and so does the port (`query::Engine::last_usage`). The sum
fills the bar at once and sends the session into a compact that is not due.

The session compacts itself when the context is nearly full: the model writes
the summary and the conversation becomes that summary and the last few
messages. The `/compact` command does the same, with the prompt of the
reference (`cc::session::compact_prompt`), so the two write the same summary.

## The address of the API

The address, the token and the headers of the API are read from the
environment (`src/auth/environment.*`), and the `env` block of the settings
is applied to the environment of the process before the session is set up —
the reference does the same, so a gateway that is written down in
`~/.claude/settings.json` is reached.

The variables of the user are applied before the folder is trusted; the ones
of the folder wait for the trust, and only the safe ones (`SAFE_ENV_VARS` of
the reference) are applied before it.

The tag of the context window is taken off the name of the model before it is
sent: `claude-sonnet-4-5[1m]` travels as `claude-sonnet-4-5`, and the window
of a million tokens is asked for with a header of its own. A gateway that
knows no such tag would refuse the name.

## The panels of the menus

The panels of the commands are panels of their own: `/mcp` opens the panel
of the servers of MCP (`ui::McpPanel`), with the servers under the scope
they are in and the state of each of them named, and `/resume` opens the
picker of the conversations (`ui::SessionPicker`) instead of a list of
them. The commands that have no panel of their own print their text.

The reference opens `/config`, `/status` and `/usage` in one panel with three
tabs — the state of the session, the configuration and the limits of the
plan. The port has the same panel (`ui::SettingsTabs`), and `/stats` opens
the panel of the statistics (`ui::StatsPanel`), which has the tabs of the
overview and the models and cycles through the ranges of days with `r`.

The tab of the models draws the chart of the tokens of the days and the
models in two columns of thirty-six characters; the list of the models is
scrolled with the arrow keys, and the scroll is kept in the panel.

The statistics are a section of the panel of the configuration as well, so
that they are reached from `/config` too.

`/plugin` (and `/marketplace`, which is an alias of it) opens the panel of
the plugins (`ui::PluginPanel`), with the four tabs of the reference: the
plugins that may be installed, the ones that are installed, the
marketplaces and the errors. The tab of the marketplaces is the one the
reference shows: the entry that adds a marketplace stands at the top of the
list, every marketplace carries its source, the number of the plugins it
offers and the day it was fetched last, and the menu of the marketplace
that is chosen offers the browsing of its plugins, the update of it, the
turning of its automatic update on and off and its removal. The question
that asks whether the marketplace is to be taken away stands between the
two, and it is answered with `y` or `n`.

The lists of the panel show a window of their rows and not all of them:
the reference shows five plugins in the tab of the plugins that may be
installed and eight in the tab of the ones that are installed, and it says
with `↑ more above` and `↓ more below` that there are more. The window
follows the row that is chosen.

The `ansi` themes of the reference name their colours after the palette of
the terminal, not after a twenty-four bit colour — `cc::Rgb` carries the
colour of the palette beside the twenty-four bit one, and the palette wins
when it is there.

The rows of the tab of the configuration are in the order the reference
shows them, and a row that says yes or no says `true` or `false`, the way
the reference writes it. The rows that the reference shows only to its own
builds — the speculative execution, the status in the terminal tab, the
teammate model, the remote control and the pushes — are not shown at all.

## The goal of the session

`/goal <condition>` sets a goal: the session goes on working until the
condition is met, and `/goal clear` takes it away. The reference asks a
model of its own whether the goal is met when the turn is over
(`cc::goal::EvaluatorTool`), and the session goes on working when it is not.

The goal is not in the tree that was read — it is in the CLI that is
installed — so the wording of the messages is taken from there: *Goal set:
…*, *Goal active: … (not yet evaluated)*, *Last check: …*, *Goal cleared: …*,
*No goal set. Usage: `/goal <condition>`* and the note that the condition
may not be empty once the characters that are not shown are taken away.

The goal is a directive for the model: the session goes on working towards
it at once, with the prompt of the reference (`cc::goal::goal_prompt`), and
the model is asked not to ask the user what to do.

## The IDE

The IDE is a server of MCP: the session talks to it over the address that
is in the lock file of `~/.claude/ide`, and the tools of the extension are
the tools of the session.

The extension of Claude Code is installed in the IDE of the terminal when
it is missing or older than the session (`cc::install_extension`), and the
tool of the differences is the automatic one afterwards.

The change of a file is shown in the IDE when the tool of the differences
is the automatic one and the session talks to an IDE: the extension is asked
to open the change (`openDiff`), and the answer of the user there — the
file saved, or the change rejected — is the answer of the permission. The
call is made in the background, because it waits for the user.

The text the user selects with the mouse in the full-screen layout goes
into the clipboard (`ui::copy_to_clipboard`): the escape of the clipboard
of the terminals, and the tool of the system as well. The terminal of the
user owns the selection in the other layout, the way the reference leaves
it to the terminal.

## The setup of the session

The first time a session runs, the reference asks the questions of the
setup: the welcome, the theme of the session, the key of the API that was
found in the environment, the notes of the safety and the settings of the
terminal. The port asks the same questions (`ui::Onboarding`), and the
welcome is the picture of fifty-eight columns that the reference draws —
the art is in `src/ui/welcome.cpp`, written from the tree of the reference.

The questions are asked only once: the flag `hasCompletedOnboarding` of the
global configuration file says that the setup was done, the way the
reference keeps it.

## The notes of the release

The port brings a changelog of its own — `CHANGELOG.md` of the repository —
and the notes of the release are the notes of the port and not the notes of
the reference. The file is looked for beside the program, in the folder of
the data of the program (`share/claude`), and in the root of the repository,
which is where it is when the program is run from the build folder
(`cc::bundled_changelog`).

The path of the program itself is remembered at the start
(`cc::set_program_path`), for the port looks for the files that came with
it beside itself.

The notes are shown in the banner, in the *What's new* feed of the reference,
and the `/release-notes` command prints the whole changelog. The changelog
of the reference is fetched only when the port brings none of its own.

## The end of the session

When the session ends, the reference says how it is taken up again:
*Resume this session with:* and *claude --resume <id>*, in the dim colour.
The port says the same with its own name — *cc++ --resume <id>* — and only
when the session was written down and the terminal is there to show it.

## The work of the session

The work that does not need the user is done in the background: the files
that are older than the period of the user are pruned in a thread of their
own, and the use of a command is written to the global configuration file
at most once a minute, the way the reference writes it.

## The worktrees of the agents

The agents that work on the tree at the same time are given a worktree of
their own, under `.claude/worktrees/`. Each of them builds the tree in
there, and each of them leaves five hundred megabytes behind.

When the work is done the worktrees are taken away:

```sh
git worktree remove --force .claude/worktrees/<name>
git worktree prune
```

The branches of the worktrees keep the commits of the agents, so nothing is
lost — but the folders themselves are five gigabytes of build output, and
the machine is better without them.

## The files outside the folder of the session

A project may pull a file in from outside its own folder with an `@path`
import in its `CLAUDE.md`. The reference asks the user about those files
once, right after the folder is trusted, and reads them only when the user
says yes; the port does the same (`ui::ExternalIncludesDialog`). The answer
is kept in the bucket of the project in the global configuration file, under
the keys `hasClaudeMdExternalIncludesApproved` and
`hasClaudeMdExternalIncludesWarningShown`, the way the reference keeps it.

The files of the user are never among them: the memory of the user may pull
a file in from anywhere, and the reference reads it all the same.

## The auto mode

The auto mode asks a model of its own whether a call may run without the
user. The port asks it the way the reference does: the model is given a
tool of its own, `classify_result`, and the answer of the tool carries the
decision (`cc::permissions::ClassifierTool`). The call runs when the model
says that it is safe, and is refused when it says that it is not.

The prompt of the reference is not in the tree that was read, so the port
writes one of its own with the same purpose; the tools that only read are
not classified, the way the reference does not classify them.

## The patterns and the codes

The matcher of a hook is a pattern in the grammar of JavaScript, the way the
reference reads it. `std::regex` is a little narrower — it knows neither the
named groups nor the lookaround — so `cc::JsRegex` in
`src/utils/js_regex.cpp` reads that grammar itself. It follows the grammar
of JavaScript closely: the escape of a property of the Unicode standard is
only read as such when the pattern carries the flag `u`, and the escape of a
number that names no group is the octal one, the way JavaScript reads it.

The code of the mobile application is made by `cc::ui::QrCode` in
`src/ui/qr.cpp`. It writes the text in the mode that holds the fewest bits,
the way the library of the reference does: the runs of the digits, the runs
of the letters and the runs of everything else are the candidates, and the
shortest stream of bits decides which of them is used.

## The options of a plugin

A plugin declares the values it needs in the `userConfig` of its manifest.
The user is asked for them when the plugin is turned on, and the values are
kept in the settings of the user, under `pluginConfigs[<plugin>].options`.
The plugin sees them as `${user_config.KEY}`.

## The keys of vim

The keys of vim live in `src/ui/vim.cpp`. The state of the mode is a small
state machine of its own: a count may be typed before a motion, an operator
waits for a motion, and a find waits for the character that is looked for.

The mode is remembered in `~/.claude.json`, under `editorMode`, the way the
reference remembers it. The escape leaves the insert mode and moves the
cursor one to the left, the way vim does it.

## The agents that ship with the CLI

The definitions of the agents that the CLI brings are in
`src/agents/builtin.cpp`: the general-purpose agent, `Explore`, `Plan` and
`statusline-setup`. The tools of an agent are the ones it may use; an empty
list means every tool.

## The history of the prompts

The prompts the user writes are kept in `~/.claude/history.jsonl`, the file
the reference keeps them in, so that the arrow up finds the prompts of the
earlier sessions — and of the reference. Each line holds the text, the time,
the folder and the id of the session, the way the reference writes it.

The line is written in one go, with the file opened in append mode: the
reference writes its own lines the same way, and a write that is not split
cannot interleave with it.

## The statistics of the sessions

The statistics of the sessions are collected from the transcripts of every
project, the way the reference collects them, and they are kept in
`~/.claude/stats-cache.json` — the file the reference keeps them in, with
the same keys and the same shapes, so that the two read each other's cache.

The cache holds everything up to yesterday; the day that is going on is
always read again, because it is not finished. The days that are over are
not read a second time, so the panel opens at once however long the history
is.

## The plan of a session

The plan of a session lives in `~/.claude/plans/<slug>.md`, and the slug is
made of three words — an adjective, a verb and a noun — the way the
reference names it. The words are in `src/utils/words.cpp`, written by
`scripts/extract-word-slugs.py` from the list of the reference; re-run it
when the list changes.

The plan mode names the file in the prompt of the session, so the model
writes the plan there and nowhere else. The prompt is built again for every
request, so that a change of the permission mode is picked up.

## The plugins

The plugins of the user are kept in `~/.claude/plugins`, and the installed
ones in `cache/<marketplace>/<plugin>/<version>/` — the layout the
reference uses, so that the two agree on the name of the folder a plugin
lives in. The settings say which of them are on, under the key
`enabledPlugins`, with the name `name@marketplace`.

A plugin brings commands, agents, hooks and MCP servers, and the port reads
the four the way the reference reads them: the markdown files of `commands`
and of `skills` are slash commands named after the plugin, the markdown
files of `agents` are sub-agents, `hooks/hooks.json` holds hooks in the
shape the settings hold them, and `.mcp.json` holds servers named
`plugin:<plugin>:<server>`.

The name of a command of a plugin is built from the file it was read from,
and the folders of the file are joined by colons — the same way the
`git/commit.md` of the user becomes `/git:commit`.

## Things that are easy to get wrong

- The erase sequence must return the carriage before it wipes, or every
  redraw starts one column further right.
- The height of the dynamic region is measured in *rows*, not in lines: a
  line that is wider than the terminal takes a row for every part of it that
  does not fit. A region whose height is counted in lines is erased wrongly,
  and the frames pile up in the scrollback.
- The mouse is reported in columns and rows that count from one, so a click
  on the first column arrives as `1`.
- The mouse is reported in the SGR mode, and the bytes of an event that has
  not arrived whole are held back until the rest of it arrives: an event
  that is read as text lands in the prompt box. The wheel is the sixth bit
  of the button, and it is not a press of the left button.
- The answer that grows stands right under the text that came before it,
  the way the reference streams it: it is the first part of the dynamic
  region. In the full-screen layout the answer is a part of the transcript,
  which is kept in memory, and the screen is written again wherever it is
  different whenever the answer changes.
- The mascot stands in the panel of the banner, whose width depends on the
  terminal; `ui::Logo::mascot_place` says where it stands, and the click
  has to be measured against it.
- The banner has three layouts — the panel, the rounded box of a narrow
  terminal and the condensed one — and the mascot stands in a different
  place in each of them, so the place is asked for with the layout that
  was drawn.
- The key decoder produces several events from one read; the queue has to be
  drained before more bytes are read, or most keystrokes are lost.
- The text is wrapped the way `wrap-ansi` wraps it: the row is filled, and
  a line of the width of the row is a line that is allowed. A wrapper that
  breaks one column early makes every paragraph of the interface one word
  shorter.
- A command of the CLI runs as soon as its suggestion is taken, even when it
  takes arguments — the hint about the arguments does not stop it. Only a
  command of the user waits for the arguments it takes.
- The keychain of the machine is slow to read: the tokens of the account are
  read once and kept until they are written again. The list of the commands
  is built again on every keystroke, so anything that asks for the account
  from there is asked for once and not once per command.
- An escape sequence that is split across two reads is only broken when the
  terminal has been quiet for 50 ms — the reference's rule. A suffix that could
  still grow into a paste marker is held back until the next read.
- `std::filesystem::path::string()` returns by value — do not return a
  reference to it.
- `requires` is a C++20 keyword and cannot name a variable.
- Boost.JSON's `key_value_pair` has a `value()` *method*, not a `value`
  member.
- The iterator `object::find()` returns is compared with `end()`, not with
  `nullptr`: the end of an object is a pointer of its own, so a comparison
  with `nullptr` is always true, and the member that is not there is read
  anyway.
- The text of the model is walked a character at a time, never a byte at a
  time: a byte of a character of more than one byte is a broken character,
  and a character that is dropped takes the space next to it with it.
- Boost.JSON 1.90 has no pretty-printer: `serialize` takes no indent, so
  `ui::pretty_json` in `src/ui/tool_display.cpp` walks the value itself.
- The colours of a block of code come from the theme of `cli-highlight`, the
  highlighter the reference uses; they are the colours of the terminal's own
  palette, so they are set with `Style::fg(BasicColor)`.
- The words of a language come from highlight.js. They are written into
  `src/ui/code_languages.cpp` by `scripts/extract-code-languages.py`, which
  reads the language definitions of highlight.js as text — it never runs
  them. Re-run it when the file needs to change.
- The copies of the files the session changes are named after the SHA-256 of
  their path, the way the reference names them, so the two agree on the
  names of the files in `~/.claude/file-history`.
- The line of a tool that is running lives in the dynamic region, so that its
  marker can blink. It has to be written to the transcript when the tool
  ends — the dynamic region is taken down by `write_static`, so the line has
  to be handed over before the result is written.
- `write_static` puts the dynamic region back as it was, so the region's
  lines have to be changed *before* the call, not after it.
- The mascot is three rows tall, and the feet are the third one: dropping
  them puts the logo out of step with the reference.
- The prompts of an MCP server are slash commands whose name holds a space
  (`<server>:<prompt> (MCP)`), so the command has to be split the way the
  reference splits it — `cc::parse_slash_command`, not `split_once`.
- The list of the suggestions takes the place of the footer of the prompt
  box: the mode of the session and the status line of the user are left out
  while it is showing, the way the reference leaves them out.
- The thread that owns the terminal is the only one that may draw. The
  engine's coroutine is resumed on a thread of the SDK's own, so a hook that
  writes to the terminal races with the main loop, and the frames of the two
  threads interleave in the scrollback.
- The dynamic region must not be taller than the screen: the erase cannot
  reach further than the last row, so the frames pile up in the scrollback.
  The answer that does not fit is written to the transcript instead.

- The QR code of the mobile application is written in the mode that holds
  the fewest bits, and the mask is the one that hides the code the best. The
  rules of the penalty are the ones of the library the reference uses, and
  they differ from the ones of the standard: the share of the dark modules
  is rounded up, and the shapes that hide the code are counted with a
  window of eleven modules.
- The side question of `/btw` runs in an engine of its own, with the
  tools taken away, so the conversation of the session never sees it
  and the turn of the session is not interrupted.
- The commands of a plugin are named after the plugin, so the name of the
  file alone is not enough — the folders of the file and the name of
  the plugin are part of it.
- The placeholders of the patterns of the hooks are read in JavaScript's
  grammar, so a pattern that JavaScript would refuse has to be refused
  too — the reader has to agree with itself about the groups it found and
  the groups it numbered.
