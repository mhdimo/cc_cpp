export const meta = {
  name: 'ux-fix-areas',
  description: 'Fixes the open UX findings of every area of the port, one worktree per area',
  phases: [
    { title: 'Fix', detail: 'one agent per area, each in a worktree of its own' },
  ],
}

const VERDICT = {
  type: 'object',
  properties: {
    fixed: { type: 'array', items: { type: 'string' } },
    branch: { type: 'string' },
    commit: { type: 'string' },
    notes: { type: 'string' },
  },
  required: ['fixed', 'notes'],
}

const AREAS = [{"area": "panels", "indices": [["panels", 1], ["panels", 2], ["panels", 3], ["panels", 4], ["panels", 5], ["panels", 6], ["panels", 7], ["panels", 8], ["panels", 9], ["panels", 10], ["panels", 11], ["panels", 12], ["panels", 13], ["panels", 14], ["panels", 15], ["panels", 16], ["panels", 17], ["panels", 18], ["panels", 19], ["panels", 20], ["panels", 21], ["panels", 22], ["panels", 23], ["panels", 24], ["panels", 25], ["panels", 26], ["panels", 27], ["panels", 28], ["panels", 29]], "files": ["src/cli/app.cpp", "src/mcp/mcp.hpp", "src/ui/sandbox_panel.cpp", "src/ui/settings_panel.cpp", "src/ui/settings_tabs.cpp", "src/ui/stats_panel.cpp", "src/utils/time.cpp"], "done": []}, {"area": "session", "indices": [["session", 1], ["session", 2], ["session", 3], ["session", 4], ["session", 5], ["session", 6], ["session", 7], ["session", 8], ["session", 9], ["session", 10], ["session", 11], ["session", 12], ["session", 13], ["session", 14], ["session", 15], ["session", 16], ["session", 17], ["session", 18], ["session", 19], ["session", 20], ["session", 21], ["session", 22], ["session", 23]], "files": ["src/cli/app.cpp", "src/session/file_history.cpp", "src/session/file_history.hpp", "src/tools/registry.cpp", "src/ui/message_selector.cpp", "src/ui/terminal.cpp", "src/ui/theme.hpp"], "done": ["session 16", "session 17"]}, {"area": "dialogs", "indices": [["dialogs", 1], ["dialogs", 2], ["dialogs", 3], ["dialogs", 4], ["dialogs", 5], ["dialogs", 6], ["dialogs", 7], ["dialogs", 8], ["dialogs", 9], ["dialogs", 10], ["dialogs", 11], ["dialogs", 12], ["dialogs", 13], ["dialogs", 14], ["dialogs", 15], ["dialogs", 16], ["dialogs", 17], ["dialogs", 18], ["dialogs", 19], ["dialogs", 20], ["dialogs", 21], ["dialogs", 22]], "files": ["src/cli/app.cpp", "src/tools/impl/bash_tool.cpp", "src/tools/impl/misc_tools.cpp", "src/tools/tool_names.hpp", "src/ui/ansi.cpp", "src/ui/input_dialog.cpp", "src/ui/permission_prompt.cpp", "src/ui/question_dialog.cpp", "src/ui/select.cpp", "src/ui/trust_dialog.cpp", "src/utils/string.cpp"], "done": []}, {"area": "overlays", "indices": [["overlays", 1], ["overlays", 2], ["overlays", 3], ["overlays", 4], ["overlays", 5], ["overlays", 6], ["overlays", 7], ["overlays", 8], ["overlays", 9], ["overlays", 10], ["overlays", 11], ["overlays", 12], ["overlays", 13], ["overlays", 14], ["overlays", 15], ["overlays", 16], ["overlays", 17], ["overlays", 18]], "files": ["src/cli/app.cpp", "src/cli/app.hpp", "src/main.cpp", "src/ui/ansi.hpp", "src/ui/heatmap.cpp", "src/ui/help_overlay.cpp", "src/ui/prompt_input.cpp", "src/ui/sandbox_panel.cpp", "src/ui/suggestions.cpp"], "done": []}, {"area": "tools", "indices": [["tools", 1], ["tools", 2], ["tools", 3], ["tools", 4], ["tools", 5], ["tools", 6], ["tools", 7], ["tools", 8], ["tools", 9], ["tools", 10], ["tools", 11], ["tools", 12], ["tools", 13], ["tools", 14], ["tools", 15], ["tools", 16]], "files": ["src/cli/app.cpp", "src/cli/app.hpp", "src/tools/impl/agent_tool.cpp", "src/tools/impl/agent_tool.hpp", "src/ui/diff.cpp", "src/ui/spinner.cpp", "src/ui/spinner.hpp", "src/ui/tool_display.cpp", "src/ui/tool_display.hpp"], "done": []}, {"area": "answer", "indices": [["answer", 1], ["answer", 2], ["answer", 3], ["answer", 4], ["answer", 5], ["answer", 6], ["answer", 7], ["answer", 8], ["answer", 9], ["answer", 10], ["answer", 11], ["answer", 12], ["answer", 13], ["answer", 14], ["answer", 15], ["answer", 16]], "files": ["src/cli/app.cpp", "src/ui/ansi.cpp", "src/ui/markdown.cpp"], "done": []}, {"area": "prompt", "indices": [["prompt", 1], ["prompt", 2], ["prompt", 3], ["prompt", 4], ["prompt", 5], ["prompt", 6], ["prompt", 7], ["prompt", 8], ["prompt", 9], ["prompt", 10], ["prompt", 11], ["prompt", 12], ["prompt", 13], ["commands", 10], ["commands", 11]], "files": ["src/cli/app.cpp", "src/commands/custom.cpp", "src/commands/registry.hpp", "src/ui/prompt_input.cpp", "src/ui/terminal.cpp"], "done": []}, {"area": "startup", "indices": [["startup", 1], ["startup", 2], ["startup", 3], ["startup", 4], ["startup", 5], ["startup", 6], ["startup", 7], ["startup", 8], ["startup", 9], ["startup", 10], ["startup", 11], ["startup", 12]], "files": ["src/cli/app.cpp", "src/cli/args.cpp", "src/commands/registry.cpp", "src/release_notes.cpp", "src/release_notes.hpp", "src/sandbox/manager.cpp", "src/ui/feed.cpp", "src/ui/logo.cpp"], "done": []}, {"area": "views", "indices": [["views", 1], ["views", 2], ["views", 3], ["views", 4], ["views", 5], ["views", 6], ["views", 7], ["views", 8], ["views", 9], ["views", 10], ["views", 11]], "files": ["src/cli/app.cpp", "src/ui/diff_view.cpp", "src/ui/logo.cpp", "src/ui/mobile_view.cpp", "src/ui/qr.cpp", "src/ui/tool_display.cpp", "src/ui/transcript_view.cpp"], "done": ["views 1", "views 2", "views 9"]}]

phase('Fix')

const results = await parallel(
  AREAS.map((area) => () =>
    agent(
      `You are fixing ${area.indices.length} UX divergence(s) between a C++20 port of the Claude Code CLI and the TypeScript reference it is a port of.

YOU ARE IN A GIT WORKTREE OF THE PORT. The port is the tree you are in — run \`git rev-parse --show-toplevel\` to see where you are. Do not edit anything outside your worktree. The reference is at /Users/liang/Downloads/claude-code-main/src/ .

Read the CLAUDE.md of the port first — it states the rule: port from the reference rather than inventing (the same names, the same strings, the same colours, the same layout). Follow its conventions: 4-space indent, 120-column lines, namespace cc::…, /// doc comments on every public entity in the voice of the neighbouring files, C++20, no exceptions across the UI boundary. Boost.JSON 1.90: array::push_back does not take a std::string (use emplace_back); value::is_int() does not exist (use is_int64()); value::empty() does not exist; boost::json::error_code does not exist (use boost::system::error_code); object::find() returns an iterator compared with end(), never with nullptr; key_value_pair has a value() method.

THE FINDINGS are in /tmp/open_findings_2.txt — one section per finding, headed "### <area> <number> [<severity>]", with the problem, the reference's lines, the port's lines, what the user sees and the fix. The area of this batch is "${area.area}". Your findings are ${area.indices.map(([a, n]) => a + ' ' + n).join(', ')}. Read exactly those sections (use grep -n to find them).

These findings are ALREADY FIXED in the port — check that the fix is there, say so, and leave them alone: ${area.done.join(', ')}

Each finding names the reference's source lines and the port's source lines. READ BOTH before you change anything, and check whether the port has already moved on since the finding was written — if it has, say so and do not undo it.

The reference's exact wording is what counts. When it cannot be recovered from the source, write an equivalent and say so in your notes. The installed Claude Code CLI on this machine (/opt/homebrew/bin/claude) is an OLDER build than the tree: it is a useful cross-check, but where the two disagree, THE TREE WINS.

THE FILES YOU MAY EDIT:
${area.files.map((f) => '  ' + f).join('\\n')}

Make the smallest change that fixes the finding. Do not reformat, do not rename, do not touch the lines that are not part of the fix — the areas are merged together afterwards, and a change that reaches outside your findings makes the merge harder.

Build the port with:  sh /tmp/wt_build.sh
It configures and builds inside your worktree and prints the compiler errors when the build fails. The tests are at build/tests/claude_code_tests — run them all; they must pass. If a test fails because of your change, fix your change (or, if the test genuinely encodes the old, wrong behaviour, update the test — and say so in your notes).

Add a test for what you changed when there is a natural place for one (tests/ is globbed by CMake, so a new tests/test_<name>.cpp is picked up).

WHEN YOU ARE DONE, commit your work in your worktree:
    git add -A && git commit -m "Fix the ${area.area} findings: ${area.indices.map(([a, n]) => a + ' ' + n).join(', ')}"
Report the branch name and the commit hash in your notes. If you could not fix a finding, do not commit a half-finished change for it — say so in your notes, with what is missing.

Report: the files you changed, what you changed in each, the findings you fixed (as \`<area> <number>\`), the findings you could not fix and why, the tests you added, and anything you could not recover from the reference.`,
      { label: `fix:${area.area}`, phase: 'Fix', schema: VERDICT, isolation: 'worktree' },
    ),
  ),
)

const all = results.filter(Boolean)
return {
  fixed: all.flatMap((r) => r.fixed || []),
  branches: all.map((r) => ({ branch: r.branch, commit: r.commit })),
  notes: all.map((r) => r.notes),
}
