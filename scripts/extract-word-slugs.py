#!/usr/bin/env python3
"""Writes the words the plan slugs are made of into src/utils/words.cpp.

The reference keeps the words in `src/utils/words.ts`; they are read here as
text and never run, the way the words of the languages of the code blocks are
read.  Re-run it when the list of the reference changes.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.expanduser("~/Downloads/claude-code-main/src/utils/words.ts")
TARGET = os.path.join(ROOT, "src", "utils", "words.cpp")


def read_list(text, name):
    """The words of one list of the reference."""
    match = re.search(r"const %s = \[(.*?)\n\]" % name, text, re.S)
    if not match:
        raise SystemExit("the list %s was not found" % name)
    body = match.group(1)
    words = re.findall(r"'([^']+)'", body)
    # The comments of the reference are not words.
    return [w for w in words if w]


def main():
    if not os.path.exists(SOURCE):
        raise SystemExit("the reference was not found at %s" % SOURCE)

    with open(SOURCE, encoding="utf-8") as handle:
        text = handle.read()

    adjectives = read_list(text, "ADJECTIVES")
    nouns = read_list(text, "NOUNS")
    verbs = read_list(text, "VERBS")

    def render(name, words):
        lines = ["const std::vector<std::string_view> k%s = {" % name]
        line = "    "
        for word in words:
            piece = '"%s", ' % word
            if len(line) + len(piece) > 116:
                lines.append(line.rstrip())
                line = "    "
            line += piece
        if line.strip():
            lines.append(line.rstrip())
        lines.append("};")
        return "\n".join(lines)

    out = []
    out.append('#include "utils/words.hpp"')
    out.append("")
    out.append("#include <random>")
    out.append("")
    out.append("namespace cc {")
    out.append("namespace {")
    out.append("")
    out.append(render("Adjectives", adjectives))
    out.append("")
    out.append(render("Verbs", verbs))
    out.append("")
    out.append(render("Nouns", nouns))
    out.append("")
    out.append("/// Picks one of the words of a list.")
    out.append("std::string_view pick(const std::vector<std::string_view>& words) {")
    out.append("    static std::mt19937 generator(std::random_device{}());")
    out.append("    std::uniform_int_distribution<size_t> pick_one(0, words.size() - 1);")
    out.append("    return words[pick_one(generator)];")
    out.append("}")
    out.append("")
    out.append("} // namespace")
    out.append("")
    out.append("std::string generate_word_slug() {")
    out.append("    return std::string(pick(kAdjectives)) + \"-\" + std::string(pick(kVerbs)) +")
    out.append("           \"-\" + std::string(pick(kNouns));")
    out.append("}")
    out.append("")
    out.append("std::string generate_short_word_slug() {")
    out.append("    return std::string(pick(kAdjectives)) + \"-\" + std::string(pick(kNouns));")
    out.append("}")
    out.append("")
    out.append("} // namespace cc")
    out.append("")

    with open(TARGET, "w", encoding="utf-8") as handle:
        handle.write("\n".join(out))

    print("wrote %s: %d adjectives, %d verbs, %d nouns"
          % (TARGET, len(adjectives), len(verbs), len(nouns)))


if __name__ == "__main__":
    main()
