#!/usr/bin/env python3
"""Drives the port through the picker of the model.

The list of the models is the one the settings name: a session may serve
another model than Anthropic, and the settings say which one it is.

    python3 tests/pty_model.py
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
PORT = 8243

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

    home = tempfile.mkdtemp(prefix="cc-model-")
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"theme": "dark", "hasCompletedOnboarding": True,
                   "projects": {os.getcwd(): {"hasTrustDialogAccepted": True}}}, handle)

    # Configured tier names stay distinct even when they resolve to the same
    # gateway model id. The session model in `model` is overridden by
    # ANTHROPIC_MODEL and is not added as a separate custom option.
    os.makedirs(os.path.join(home, ".claude"))
    with open(os.path.join(home, ".claude", "settings.json"), "w") as handle:
        json.dump({
            "model": "fable",
            "env": {
                "ANTHROPIC_MODEL": "configured-shared-model[1m]",
                "ANTHROPIC_DEFAULT_SONNET_MODEL": "configured-shared-model[1m]",
                "ANTHROPIC_DEFAULT_OPUS_MODEL": "configured-shared-model[1m]",
                "ANTHROPIC_DEFAULT_HAIKU_MODEL": "configured-shared-model[1m]",
            },
        }, handle)

    mock = subprocess.Popen(
        [sys.executable, MOCK, "--port", str(PORT), "--text", "hello."],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(1.0)
    if mock.poll() is not None:
        print(f"the mock API did not start on the port {PORT}")
        return 1

    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "test-key",
           "ANTHROPIC_BASE_URL": f"http://127.0.0.1:{PORT}"}
    # The models of the shell of the user are left out: the settings of the
    # test name the models of the session.
    for name in list(env):
        if name.startswith("ANTHROPIC_DEFAULT_") or name.startswith("ANTHROPIC_CUSTOM_MODEL"):
            del env[name]

    session = Session(env=env)
    try:
        session.read(2.0)
        session.send("/model\r")
        session.read(2.0)
        screen = strip(session.output)
        picker = screen.rsplit("Select model", 1)[-1]

        check("the picker is opened", "Select model" in screen, repr(screen[-400:]))
        check("the configured model id is offered for each tier",
              picker.count("configured-shared-model[1m]") == 4, repr(picker[-800:]))
        check("each tier keeps its own configured-model description",
              all(name in picker for name in ("Custom Opus model (1M context)",
                                               "Custom Sonnet model (1M context)",
                                               "Custom Haiku model (1M context)")),
              repr(picker[-800:]))
        check("the overridden model from settings is not added separately",
              "fable" not in picker.lower(), repr(picker[-800:]))
    finally:
        session.close()
        mock.terminate()

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
