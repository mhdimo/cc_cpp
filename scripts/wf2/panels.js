export const meta = {
  name: 'ux-fix-area-2',
  description: 'Fixes the open UX findings of one area of the port',
  phases: [
    { title: 'Fix', detail: 'one agent per group of files' },
  ],
}

const VERDICT = {
  type: 'object',
  properties: {
    fixed: { type: 'array', items: { type: 'integer' } },
    notes: { type: 'string' },
  },
  required: ['fixed', 'notes'],
}

const AREA = 'panels'
const GROUPS = [
  {
    "files": "src/cli/app.cpp, src/mcp/mcp.hpp, src/ui/settings_panel.cpp, src/ui/settings_tabs.cpp, src/utils/time.cpp",
    "indices": [
      1,
      2,
      3,
      4,
      5,
      6,
      7,
      8,
      9,
      10,
      11,
      22,
      24,
      25,
      29
    ]
  },
  {
    "files": "src/ui/stats_panel.cpp",
    "indices": [
      12,
      13,
      14,
      15,
      16,
      17,
      23,
      26
    ]
  },
  {
    "files": "src/ui/sandbox_panel.cpp",
    "indices": [
      18,
      19,
      20,
      21,
      27,
      28
    ]
  }
]

phase('Fix')

const results = await parallel(
  GROUPS.map((group) => () =>
    agent(
      `You are fixing ${group.indices.length} UX divergence(s) between a C++20 port of the Claude Code CLI and the TypeScript reference it is a port of.

The port is at /Users/liang/cc_cpp/ . The reference is at /Users/liang/Downloads/claude-code-main/src/ .

Read /Users/liang/cc_cpp/CLAUDE.md first — it states the rule: port from the reference rather than inventing (the same names, the same strings, the same colours, the same layout). Follow its conventions: 4-space indent, 120-column lines, namespace cc::…, /// doc comments on every public entity in the voice of the neighbouring files, C++20, no exceptions across the UI boundary. Boost.JSON 1.90: array::push_back does not take a std::string (use emplace_back); value::is_int() does not exist (use is_int64()); value::empty() does not exist; boost::json::error_code does not exist (use boost::system::error_code); object::find() returns an iterator compared with end(), never with nullptr; key_value_pair has a value() method.

THE FINDINGS are in /tmp/open_findings_2.txt — one section per finding, headed "### <area> <number> [<severity>]", with the problem, the reference's lines, the port's lines, what the user sees and the fix. The area of this batch is "${AREA}". Your findings are numbers ${group.indices.join(', ')}. Read exactly those sections (use grep -n to find them).

Each finding names the reference's source lines and the port's source lines. READ BOTH before you change anything, and check whether the port has already moved on since the finding was written — if it has, say so and do not undo it.

YOU OWN THESE FILES AND NO OTHERS:
  ${group.files}

Do not edit any other file. If a finding seems to need a change in a file you do not own, do not make it — say so in your notes, with the exact change that is needed.

Build with:  sh /tmp/cc_build.sh
That script serialises the builds (several agents share the build directory) and prints the compiler errors when the build fails. If the build fails because of YOUR files, fix it and build again. If it fails because of somebody else's files, note it and move on — do not edit those files.

Run the tests with: /Users/liang/cc_cpp/build/tests/claude_code_tests
All the tests must pass. If a test fails because of your change, fix your change (or, if the test genuinely encodes the old, wrong behaviour, update the test — and say so in your notes). If it fails because of somebody else's file, note it and move on.

Add a test for what you changed when there is a natural place for one (tests/ is globbed by CMake, so a new tests/test_<name>.cpp is picked up).

When the reference's exact behaviour cannot be recovered from the source, write an equivalent and say so in your notes.

Report: the files you changed, what you changed in each, the findings you fixed (by number), the findings you could not fix and why, the tests you added, and anything you could not recover from the reference.`,
      { label: `fix:${group.files.slice(0, 40)}`, phase: 'Fix', schema: VERDICT },
    ),
  ),
)

const all = results.filter(Boolean)
return {
  fixed: all.flatMap((r) => r.fixed || []),
  notes: all.map((r) => r.notes),
}
