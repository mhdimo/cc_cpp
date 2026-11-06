#!/usr/bin/env python3
"""Drives the question about the files outside the folder of the session.

A project that pulls a file in from outside its folder is asked about once:
the question is asked after the folder is trusted, and the answer is
remembered in the settings of the project.
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
    home = tempfile.mkdtemp(prefix="cc-external-")
    # A file that lies outside the folder of the session.
    outside = tempfile.mkdtemp(prefix="cc-external-outside-")
    with open(os.path.join(outside, "notes.md"), "w") as handle:
        handle.write("The notes.\n")

    # The folder the session runs in, with a memory file that pulls the
    # file in.
    project = tempfile.mkdtemp(prefix="cc-external-project-")
    with open(os.path.join(project, "CLAUDE.md"), "w") as handle:
        handle.write("@" + os.path.join(outside, "notes.md") + "\n")

    # The setup of the session is done, so that only the question about the
    # files outside the folder is asked.
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"hasCompletedOnboarding": True}, handle)

    env = {**os.environ, "HOME": home,
           "ANTHROPIC_API_KEY": "sk-ant-0123456789abcdefghijklmnopqrstuvwxyz",
           "ANTHROPIC_BASE_URL": "http://127.0.0.1:9",
           "CLAUDE_CODE_SCROLLBACK": "1"}

    session = Session(env=env, cwd=project)
    try:
        session.read(2.5)
        screen = strip(session.output)

        print("trust")
        check("the trust of the folder is asked for", "Accessing workspace:" in screen,
              repr(screen[-600:]))

        mark = len(session.output)
        session.send("\r")  # The folder is trusted.
        session.read(1.5)
        screen = strip(session.output[mark:])

        print("external includes")
        check("the question is asked", "Allow external CLAUDE.md file imports?" in screen,
              repr(screen[-800:]))
        check("what the project does is said",
              "imports files outside the current working directory" in screen,
              repr(screen[-800:]))
        check("the files are listed", "External imports:" in screen, repr(screen[-800:]))
        check("the file is named", "notes.md" in screen, repr(screen[-800:]))
        check("the answer that allows them is offered", "Yes, allow external imports" in screen,
              repr(screen[-800:]))
        check("the answer that refuses them is offered", "No, disable external imports" in screen,
              repr(screen[-800:]))

        mark = len(session.output)
        session.send("\r")  # The files are allowed.
        session.read(1.5)
        screen = strip(session.output[mark:])

        print("the answer is remembered")
        with open(os.path.join(home, ".claude.json")) as handle:
            config = json.load(handle)

        projects = config.get("projects", {})
        buckets = [entry for entry in projects.values()
                   if isinstance(entry, dict)
                   and "hasClaudeMdExternalIncludesApproved" in entry]
        check("the answer is written in the settings of the project", len(buckets) == 1,
              repr(config.get("projects")))
        if buckets:
            check("the answer is that the files are allowed",
                  buckets[0]["hasClaudeMdExternalIncludesApproved"] is True, repr(buckets[0]))
            check("the question is remembered as asked",
                  buckets[0].get("hasClaudeMdExternalIncludesWarningShown") is True,
                  repr(buckets[0]))
    finally:
        session.close()

    # The question is not asked again in the next session.
    session = Session(env=env, cwd=project)
    try:
        session.read(2.5)
        screen = strip(session.output)

        print("the question is asked once")
        check("the question is not asked again",
              "Allow external CLAUDE.md file imports?" not in screen, repr(screen[-800:]))
        check("the trust of the folder is not asked for either",
              "Accessing workspace:" not in screen, repr(screen[-800:]))

        print("the panel of the configuration")
        mark = len(session.output)
        session.send("/config\r")
        session.wait_for("Search settings", timeout=5.0)
        session.read(0.5)

        # The row is far down the list, so the list is filtered first.
        session.send("External")
        session.read(0.5)
        screen = strip(session.output[mark:])
        check("the row of the files is there", "External CLAUDE.md includes" in screen,
              repr(screen[-800:]))
        check("the answer is shown", "true" in screen, repr(screen[-800:]))
    finally:
        session.close()
        shutil.rmtree(home, ignore_errors=True)
        shutil.rmtree(outside, ignore_errors=True)
        shutil.rmtree(project, ignore_errors=True)

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
