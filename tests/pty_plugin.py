#!/usr/bin/env python3
"""Checks the panel of the plugins.

The panel of the plugins is the one the reference shows for `/plugin`: the
plugins that may be installed, the ones that are installed, the marketplaces
and the errors stand in four tabs. The tab of the marketplaces has the list
of the marketplaces, the menu of the one that is chosen and the question
that asks whether it is to be taken away.
"""

import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_smoke import FAILURES, Session, check, strip  # noqa: E402


def main():
    home = tempfile.mkdtemp(prefix="cc-plugin-")
    env = {**os.environ, "HOME": home, "ANTHROPIC_API_KEY": "",
           "ANTHROPIC_BASE_URL": "http://127.0.0.1:9",
           "CLAUDE_CODE_SCROLLBACK": "1"}

    plugins = os.path.join(home, ".claude", "plugins")
    manifest = os.path.join(plugins, "marketplaces", "demo", ".claude-plugin")
    os.makedirs(manifest)
    with open(os.path.join(manifest, "marketplace.json"), "w") as handle:
        json.dump({"name": "demo", "owner": {"name": "Someone"},
                   "plugins": [{"name": "alpha", "description": "The first plugin"},
                               {"name": "beta", "description": "The second plugin"}]}, handle)
    with open(os.path.join(plugins, "known_marketplaces.json"), "w") as handle:
        json.dump({"demo": {"source": "owner/repo",
                            "installLocation": os.path.join(plugins, "marketplaces", "demo"),
                            "lastUpdated": "2026-10-01T12:00:00.000Z",
                            "autoUpdate": True}}, handle)

    cache = os.path.join(plugins, "cache", "demo", "alpha", "1.0.0", ".claude-plugin")
    os.makedirs(cache)
    with open(os.path.join(cache, "plugin.json"), "w") as handle:
        json.dump({"name": "alpha", "version": "1.0.0", "description": "The first plugin"}, handle)
    os.makedirs(os.path.join(home, ".claude"), exist_ok=True)
    with open(os.path.join(home, ".claude", "settings.json"), "w") as handle:
        json.dump({"enabledPlugins": {"alpha@demo": True}}, handle)
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"hasCompletedOnboarding": True}, handle)

    session = Session(env=env)
    session.read(1.5)
    session.send("\r")  # The folder is trusted.
    session.read(0.8)

    print("the panel")
    before = len(session.output)
    session.send("/plugin\r")
    session.read(1.0)
    screen = strip(session.output[before:])
    check("the panel is drawn", "Plugins" in screen and "Discover" in screen, repr(screen[-400:]))
    check("the tab of the marketplaces is there", "Marketplaces" in screen, repr(screen[-400:]))
    check("the field of the search is drawn", "⌕" in screen, repr(screen[-400:]))
    # The field of the search fits in the terminal: the panel leaves a column
    # on each side of it.
    for line in screen.splitlines():
        if "╭" in line:
            check("the field of the search fits", len(line) <= 80, repr(line))
            break

    print("the tab of the installed plugins")
    before = len(session.output)
    session.send("\t")
    session.read(0.6)
    screen = strip(session.output[before:])
    check("the plugin of the cache is there", "alpha" in screen and "demo" in screen,
          repr(screen[-400:]))
    # The reference puts the sign of the state in front of the name and the
    # badge of the plugin behind it.
    check("the state of the plugin is named", "✔" in screen, repr(screen[-400:]))
    check("the plugin carries its badge", "Plugin" in screen, repr(screen[-400:]))

    print("the tab of the marketplaces")
    before = len(session.output)
    session.send("\t")
    session.read(0.6)
    screen = strip(session.output[before:])
    check("the marketplaces are managed", "Manage marketplaces" in screen, repr(screen[-400:]))
    check("a marketplace can be added", "Add Marketplace" in screen, repr(screen[-400:]))
    check("the marketplace is named", "demo" in screen, repr(screen[-400:]))
    check("the number of the plugins is named", "2 available" in screen, repr(screen[-400:]))
    check("the number of the installed plugins is named", "1 installed" in screen,
          repr(screen[-400:]))

    print("the menu of the marketplace")
    session.send("\x1b[B")  # The arrow down picks the marketplace.
    session.read(0.4)
    before = len(session.output)
    session.send("\r")
    session.read(0.6)
    screen = strip(session.output[before:])
    check("the plugins of the marketplace can be browsed", "Browse plugins (2)" in screen,
          repr(screen[-400:]))
    check("the marketplace can be updated", "Update marketplace" in screen, repr(screen[-400:]))
    check("the update of the marketplace can be turned off", "Disable auto-update" in screen,
          repr(screen[-400:]))
    check("the marketplace can be taken away", "Remove marketplace" in screen, repr(screen[-400:]))
    check("the day of the last fetch is named", "last updated" in screen, repr(screen[-400:]))

    print("the question about the marketplace")
    for _ in range(3):
        session.send("\x1b[B")
    session.read(0.4)
    before = len(session.output)
    session.send("\r")
    session.read(0.6)
    screen = strip(session.output[before:])
    check("the marketplace is named in the question", "Remove marketplace demo" in screen,
          repr(screen[-400:]))
    check("the plugins that go away are named", "This will also uninstall 1 plugin" in screen,
          repr(screen[-400:]))
    check("the question is answered with a key", "to confirm" in screen, repr(screen[-400:]))

    session.send("n")  # The marketplace stays.
    session.read(0.6)

    print("the list of the plugins")
    before = len(session.output)
    session.send("\x1b")  # The escape leaves the marketplace.
    session.read(0.4)
    session.send("\x1b")  # And the tab.
    session.read(0.4)
    session.send("\x1b")  # And the panel.
    session.read(0.6)

    session.close()

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
