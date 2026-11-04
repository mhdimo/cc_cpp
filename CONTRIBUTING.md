# Contributing

Thank you for helping with the port. This file says how the work is done
here, so that everybody's changes look like they came from the same hand.

## What this project is

A C++20 rewrite of the [Claude Code](https://claude.com/claude-code) CLI,
built on [ai-sdk-cpp](https://github.com/mhdimo/ai-sdk-cpp) and
[FTXUI](https://github.com/ArthurSonzogni/FTXUI).

The port is not a reimagining: it is a port. The reference implementation in
`~/Downloads/claude-code-main/src/` is the authority — the same tools, the
same input schemas, the same result strings, the same colours and the same
terminal layout. When the reference's exact wording cannot be recovered from
the source, write an equivalent and say so in the summary of the change.

The program is run as `cc++`.

## Building and testing

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(sysctl -n hw.ncpu)
./build/tests/claude_code_tests     # the unit tests
python3 tests/pty_smoke.py          # drives the CLI through a pty
```

The build fetches `ai-sdk-cpp`, `Catch2` and `FTXUI` with `FetchContent`.
The pty suites live in `tests/pty_*.py`; each one starts the real binary
under a pseudo-terminal and looks at the screen it draws. `tests/golden_screens.py`
compares four screens against the reference.

See [README.md](README.md) for the ways of making the build quicker.

## The style of the code

The conventions of the tree are the ones of `CLAUDE.md`, which is the
reference for the house style:

- 4-space indent, 120-column lines, `namespace cc::…`.
- `///` doc comments on every public entity, in the voice of the neighbouring
  files.
- No exceptions across the boundary of the UI where they can be avoided: the
  tools report a failure by throwing `cc::ToolError`, and the engine turns it
  into an error result the model can see.

Read the files around the one you touch before you write: the port is
consistent on purpose, and a change that breaks the consistency reads as a
foreign patch.

### The comments are signed

Every comment that explains a decision — the ones that say *why* and not *what*
— carries a signature at its end:

```cpp
// The reference reads the usage of the last message that carries one, and
// the sum would fill the bar at once. — signed: Claude (claude-opus-5-5)
```

The signature names the agent that wrote the comment and the model that was
running when it was written. A comment with no signature is a comment that
nobody owns, and the port has too many of those already.

The signature goes on its own line when the comment runs for more than a
couple of lines, so that the text of the comment stays readable:

```cpp
// The child leads a group of its own, so that the whole of what it starts
// is killed with it: the shell of the command forks what it runs, and the
// child of the shell is the one that holds the pipes open.
// signed: Claude (claude-opus-5-5)
```

A comment that only says what the next line says is not worth writing at all,
and it carries no signature.

## The style of the commits

- **One commit per change.** Squash your work into a single commit before it
  is offered. A pull request that carries the history of the afternoon is
  hard to read and hard to revert.
- **Signed by the author.** The commit carries the `Signed-off-by` line of
  the person who wrote it, and it is signed with GPG (`git config commit.gpgsign true`)
  or with SSH (`git config gpg.format ssh`). The signature is checked.
- **No co-author at all.** No `Co-authored-by` trailer, and no `Co-authored-by`
  in the middle of the message either. The commit is the author's own work;
  a trailer that names somebody else who did not write the code is a claim
  the history cannot check.

The message says what changed and why, in the imperative:

```
Fix the erase of the dynamic region after a resize

The region was measured in lines and not in rows, so a line that was wider
than the terminal was erased wrongly and the frames piled up in the
scrollback.
```

## Opening a pull request

1. Build it and run the tests — the unit tests and the pty suites.
2. Say what the change does and which part of the reference it follows.
3. Name the reference's file when the port was written from it, so that the
   next one can find it again.
4. Note what is left out, if anything is. A half-ported feature that says so
   is worth more than a broken one that does not.

## Reporting a difference

A bug report that shows the screen is worth ten that describe it. The pty
harness makes a screen easy to keep:

```python
from pty_smoke import Session
session = Session()
session.read(2)
print(repr(session.output))
```

## Cutting a release

1. Write the notes of the release in `CHANGELOG.md`. The notes are the
   lines that begin with a dash, under a section that begins with `## ` and
   carries the version. The port shows them in the banner and the
   `/release-notes` command prints them.
2. Put the version of the release in `src/core/version.cpp`.
3. Build the archive of the release:

   ```sh
   scripts/package.sh
   ```

   The archive is written to `dist/`. It holds the program, the man page,
   the changelog and the README.
4. Build the tests and run them:

   ```sh
   cmake --build build -j$(sysctl -n hw.ncpu)
   ./build/tests/claude_code_tests
   python3 tests/golden_screens.py
   python3 tests/pty_smoke.py
   ```

5. Commit the release. The commit is squashed, signed by the author and
   carries no co-author — the rules above.
6. Tag it:

   ```sh
   git tag -s v0.1.0 -m "CC CPP – Release"
   ```

   The tag is signed with the same key as the commits.

## The licence

The port is a reimplementation: it follows the reference, but it is not the
reference's code. The licence of the reference does not carry over to it,
and no licence has been chosen for the port yet — the one that is chosen
here is the one that governs it.
