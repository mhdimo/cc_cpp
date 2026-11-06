#!/usr/bin/env python3
"""Writes the workflow that fixes the findings of one area.

The findings are in /tmp/open_findings_2.txt (written by the second
verification sweep). The findings of an area are grouped by the files they
touch, so that no two agents edit the same file; one agent is written per
group.

    python3 scripts/make_fix_workflow.py <area> <out.js>
"""

import json
import re
import sys
from collections import defaultdict

FINDINGS = "/tmp/open_findings_2.txt"

SCRIPT = """export const meta = {
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

const AREA = %(area)r
const GROUPS = %(groups)s

phase('Fix')

const results = await parallel(
  GROUPS.map((group) => () =>
    agent(
      `You are fixing ${group.indices.length} UX divergence(s) between a C++20 port of the Claude Code CLI and the TypeScript reference it is a port of.

The port is at /Users/liang/cc_cpp/ . The reference is at /Users/liang/Downloads/claude-code-main/src/ .

Read /Users/liang/cc_cpp/CLAUDE.md first — it states the rule: port from the reference rather than inventing (the same names, the same strings, the same colours, the same layout). Follow its conventions: 4-space indent, 120-column lines, namespace cc::…, /// doc comments on every public entity in the voice of the neighbouring files, C++20, no exceptions across the UI boundary. Boost.JSON 1.90: array::push_back does not take a std::string (use emplace_back); value::is_int() does not exist (use is_int64()); value::empty() does not exist; boost::json::error_code does not exist (use boost::system::error_code); object::find() returns an iterator compared with end(), never with nullptr; key_value_pair has a value() method.

THE FINDINGS are in %(findings)s — one section per finding, headed "### <area> <number> [<severity>]", with the problem, the reference's lines, the port's lines, what the user sees and the fix. The area of this batch is "${AREA}". Your findings are numbers ${group.indices.join(', ')}. Read exactly those sections (use grep -n to find them).

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
"""


def read_findings():
    """Reads the findings of the second sweep."""
    text = open(FINDINGS).read()
    found = {}
    for m in re.finditer(
        r"### (\w+) (\d+) \[(\w+)\]\nPROBLEM: (.*?)\nREFERENCE: (.*?)\nPORT: (.*?)\nVISIBLE: (.*?)\nFIX: (.*?)(?=\n### |\n## area|\Z)",
        text,
        flags=re.S,
    ):
        found[(m.group(1), int(m.group(2)))] = {
            "area": m.group(1),
            "severity": m.group(3),
            "problem": m.group(4).strip(),
            "reference": m.group(5).strip(),
            "port": m.group(6).strip(),
            "visible": m.group(7).strip(),
            "fix": m.group(8).strip(),
        }
    return found


def files_of(finding):
    """The files of the port that a finding is about."""
    text = finding["problem"] + "\n" + finding["port"] + "\n" + finding["fix"]
    found = set(re.findall(r"(?:/Users/liang/cc_cpp/)?(src/[\w./]+\.(?:cpp|hpp))", text))
    return sorted(found)


def main():
    area = sys.argv[1]
    out_path = sys.argv[2]
    findings = read_findings()

    mine = sorted(n for (a, n), f in findings.items() if a == area)
    if not mine:
        print("nothing to do", file=sys.stderr)
        return 1

    groups = defaultdict(list)
    for n in mine:
        finding = findings[(area, n)]
        files = files_of(finding)
        if not files:
            print(f"SKIPPED {n}: no file named — {finding['problem'][:80]}", file=sys.stderr)
            continue
        groups[tuple(files)].append(n)

    # Two groups that name the same file would have two agents editing it at
    # the same time, so they are merged into one.
    merged = []
    for files, indices in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        for entry in merged:
            if set(files) & set(entry["files"]):
                entry["files"] = sorted(set(entry["files"]) | set(files))
                entry["indices"] = sorted(set(entry["indices"]) | set(indices))
                break
        else:
            merged.append({"files": list(files), "indices": list(indices)})

    # A group that grew by the merge may now share a file with another one.
    changed = True
    while changed:
        changed = False
        for i in range(len(merged)):
            for j in range(i + 1, len(merged)):
                if set(merged[i]["files"]) & set(merged[j]["files"]):
                    merged[i]["files"] = sorted(set(merged[i]["files"]) | set(merged[j]["files"]))
                    merged[i]["indices"] = sorted(set(merged[i]["indices"]) | set(merged[j]["indices"]))
                    merged.pop(j)
                    changed = True
                    break
            if changed:
                break

    spec = [{"files": ", ".join(entry["files"]), "indices": entry["indices"]}
            for entry in sorted(merged, key=lambda e: -len(e["indices"]))]

    script = SCRIPT % {
        "area": area,
        "groups": json.dumps(spec, indent=2),
        "findings": FINDINGS,
        "indices": ", ".join(str(n) for n in mine),
    }
    with open(out_path, "w") as handle:
        handle.write(script)

    print(f"{area}: {len(mine)} findings in {len(spec)} groups")
    for entry in spec:
        print(f"  {entry['indices']} -> {entry['files']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
