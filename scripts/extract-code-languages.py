#!/usr/bin/env python3
"""Extracts the keyword lists of the highlight.js languages.

The reference highlights a block of code with `cli-highlight`, which wraps
highlight.js; the words a language knows therefore come from highlight.js.
This script reads the language definitions of highlight.js as text and writes
them into `src/ui/code_languages.cpp`, so that the port recognises the same
words.

    python3 scripts/extract-code-languages.py [path/to/highlight.js]

The definitions are read, never run: the script pulls the word lists out of
the source with a small parser rather than evaluating the JavaScript.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The languages the port knows, with the way their comments and strings are
# written. The names are the ones highlight.js uses.
LANGUAGES = [
    ("arduino", "c", None, False, False),
    ("bash", "hash", None, False, False),
    ("c", "c", None, False, False),
    ("clojure", "semi", None, False, False),
    ("cpp", "c", None, False, False),
    ("csharp", "c", None, False, False),
    ("css", "block", None, False, False),
    ("dart", "c", None, False, False),
    ("diff", "none", None, False, False),
    ("dockerfile", "hash", None, False, False),
    ("elixir", "hash", None, False, False),
    ("erlang", "percent", None, False, False),
    ("go", "c", None, False, False),
    ("graphql", "none", None, False, False),
    ("groovy", "c", None, False, False),
    ("haskell", "semi", None, False, False),
    ("haxe", "c", None, False, False),
    ("ini", "semi", None, False, False),
    ("java", "c", None, False, False),
    ("javascript", "c", None, True, False),
    ("json", "none", None, False, False),
    ("julia", "hash", None, False, False),
    ("kotlin", "c", None, False, False),
    ("less", "c", None, False, False),
    ("lua", "semi", None, False, False),
    ("makefile", "hash", None, False, False),
    ("markdown", "none", None, False, False),
    ("nginx", "hash", None, False, False),
    ("nim", "hash", None, False, False),
    ("objectivec", "c", None, False, False),
    ("perl", "hash", None, False, False),
    ("php", "c", None, False, False),
    ("plaintext", "none", None, False, False),
    ("powershell", "hash", "angle", False, False),
    ("protobuf", "c", None, False, False),
    ("python", "hash", None, False, True),
    ("r", "hash", None, False, False),
    ("ruby", "hash", None, False, False),
    ("rust", "c", None, False, False),
    ("scala", "c", None, False, False),
    ("scss", "c", None, False, False),
    ("sql", "semi", "slash", False, False),
    ("swift", "c", None, False, False),
    ("typescript", "c", None, False, False),
    ("vbnet", "quote", None, False, False),
    ("verilog", "c", None, False, False),
    ("xml", "xml", None, False, False),
    ("yaml", "hash", None, False, False),
]

# The scopes the port knows, in the order of the enum.
SCOPES = [
    "keyword",
    "built_in",
    "type",
    "literal",
    "title",
    "symbol",
    "meta",
    "attr",
    "name",
    "variable",
    "doctag",
    "section",
    "meta-keyword",
    "meta-string",
    "template-variable",
    "params",
    "class",
    "function",
    "regexp",
    "subst",
]

# The scope of a word the language does not name.
DEFAULT_SCOPE = "keyword"


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def find_language_file(directory, name):
    """The file of the language, or None."""
    for candidate in (name + ".js", name + ".js.js"):
        path = os.path.join(directory, candidate)
        if os.path.exists(path):
            return path
    return None


def strip_comments(text):
    """Removes the comments of a JavaScript source."""
    out = []
    i = 0
    while i < len(text):
        if text.startswith("//", i):
            end = text.find("\n", i)
            i = len(text) if end == -1 else end
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i)
            i = len(text) if end == -1 else end + 2
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def strings_of(text):
    """The string literals of a JavaScript expression, joined."""
    parts = []
    i = 0
    while i < len(text):
        if text[i] in "'\"":
            quote = text[i]
            i += 1
            chunk = []
            while i < len(text) and text[i] != quote:
                if text[i] == "\\":
                    i += 1
                chunk.append(text[i])
                i += 1
            i += 1
            parts.append("".join(chunk))
            continue
        if text.startswith("//", i):
            end = text.find("\n", i)
            i = len(text) if end == -1 else end
            continue
        i += 1
    return " ".join(parts)


def find_const(source, name):
    """The text of `const NAME = ...;`, or None."""
    match = re.search(r"\bconst\s+" + re.escape(name) + r"\s*=", source)
    if not match:
        return None
    return balanced(source, match.end())


def balanced(source, start):
    """The text of an expression that starts at `start`."""
    depth = 0
    i = start
    while i < len(source):
        c = source[i]
        if c in "'\"":
            i += 1
            while i < len(source) and source[i] != c:
                if source[i] == "\\":
                    i += 1
                i += 1
            i += 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                break
            depth -= 1
        elif c == ";" and depth == 0:
            break
        i += 1
    return source[start:i]


def keyword_object(expression, source, depth=0):
    """The scopes of a keyword object, as a dictionary of lists."""
    if not expression.startswith("{"):
        return None

    scopes = {}
    i = 1
    while i < len(expression):
        match = re.compile(r"\s*,?\s*'?([a-zA-Z_$-]+)'?\s*:\s*").match(expression, i)
        if not match:
            break
        scope = match.group(1)
        value, i = read_value(expression, match.end())
        if scope == "$pattern":
            # The pattern the words are matched with is not a word.
            continue
        words = words_of(source, value, depth + 1)
        if words:
            scopes.setdefault(scope, []).extend(words)
    return scopes


def words_of(source, value, depth=0):
    """The words of a keyword value."""
    if depth > 8:
        return []

    if value.startswith("'"):
        return strings_of(value).split()
    if value.startswith("["):
        words = strings_of(value).split()
        for name in re.findall(r"\.\.\.\s*([A-Za-z_$][A-Za-z0-9_$]*)", value):
            spread = find_const(source, name)
            if spread:
                words.extend(words_of(source, spread.strip(), depth + 1))
        return words

    const = find_const(source, value)
    if not const:
        return []
    const = const.strip()

    if const.startswith("'"):
        return strings_of(const).split()
    if const.startswith("["):
        words = []
        words.extend(strings_of(const).split())
        # The arrays that are spread into this one.
        for name in re.findall(r"\.\.\.\s*([A-Za-z_$][A-Za-z0-9_$]*)", const):
            spread = find_const(source, name)
            if spread:
                words.extend(words_of(source, spread.strip(), depth + 1))
        return words
    if const.startswith("{"):
        found = keyword_object(const, source, depth + 1) or {}
        return found.get(DEFAULT_SCOPE, [])
    return []


def read_value(source, start):
    """The value of an expression, and the place it ends.

    A value is a string, an object or the name of a list that is defined
    somewhere else. The strings that follow a `+` are joined to the first
    one, the way the JavaScript does.
    """
    i = start
    while i < len(source) and source[i] in " \t\n":
        i += 1
    if i >= len(source):
        return "", i

    if source[i] in "'\"":
        quote = source[i]
        i += 1
        chunk = []
        while i < len(source) and source[i] != quote:
            if source[i] == "\\":
                i += 1
            chunk.append(source[i])
            i += 1
        i += 1
        text = "".join(chunk)
        # The strings that are joined to this one.
        while True:
            after = re.compile(r"\s*\+\s*").match(source, i)
            if not after:
                break
            more, i = read_value(source, after.end())
            if not more.startswith("'"):
                break
            text += " " + more[1:-1]
        return "'" + text + "'", i

    if source[i] == "/":
        # A regular expression.
        i += 1
        chunk = []
        while i < len(source) and source[i] != "/":
            if source[i] == "\\":
                i += 1
            chunk.append(source[i])
            i += 1
        return "/" + "".join(chunk) + "/", i + 1

    if source[i] in "{[":
        close = "}" if source[i] == "{" else "]"
        depth = 0
        j = i
        while j < len(source):
            c = source[j]
            if c in "'\"":
                j += 1
                while j < len(source) and source[j] != c:
                    if source[j] == "\\":
                        j += 1
                    j += 1
            elif c in "{[":
                depth += 1
            elif c in "}]":
                depth -= 1
                if depth == 0:
                    return source[i : j + 1], j + 1
            j += 1
        return source[i:], len(source)

    match = re.compile(r"[A-Za-z_$][A-Za-z0-9_$]*").match(source, i)
    if not match:
        return "", i

    name = match.group(0)
    end = match.end()

    # A member of an object, e.g. `hljs.UNDERSCORE_IDENT_RE`.
    while True:
        after = re.compile(r"\s*\.\s*([A-Za-z_$][A-Za-z0-9_$]*)").match(source, end)
        if not after:
            break
        name = after.group(1)
        end = after.end()

    # A call: the argument is what names the list.
    after = re.compile(r"\s*\(").match(source, end)
    if after:
        depth = 0
        j = after.end() - 1
        while j < len(source):
            c = source[j]
            if c in "'\"":
                j += 1
                while j < len(source) and source[j] != c:
                    if source[j] == "\\":
                        j += 1
                    j += 1
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        argument = source[after.end() : j]
        first = re.match(r"\s*([A-Za-z_$][A-Za-z0-9_$]*|'[^']*')", argument)
        if first:
            return first.group(1), j + 1

    return name, end


def scopes_of(source, value, depth=0):
    """The scopes of a keyword value."""
    if depth > 6:
        return {}

    if value.startswith("{"):
        return keyword_object(value, source, depth + 1) or {}
    if value.startswith("'"):
        return {DEFAULT_SCOPE: strings_of(value).split()}
    if value.startswith("["):
        return {DEFAULT_SCOPE: words_of(source, value, depth + 1)}

    const = find_const(source, value)
    if not const:
        return {}
    const = const.strip()
    if const.startswith("{"):
        return keyword_object(const, source, depth + 1) or {}
    if const.startswith("'"):
        return {DEFAULT_SCOPE: strings_of(const).split()}
    if const.startswith("["):
        return {DEFAULT_SCOPE: words_of(source, const, depth + 1)}
    return {}


def collect(source, name, depth=0):
    """The scopes of the keywords of a language."""
    if depth > 6:
        return {}

    scopes = {}
    for match in re.finditer(r"\bkeywords\s*:\s*", source):
        value, _ = read_value(source, match.end())
        if not value:
            continue
        for scope, words in scopes_of(source, value, depth + 1).items():
            scopes.setdefault(scope, []).extend(words)
    return scopes


def aliases_of(source):
    """The aliases of a language."""
    found = []
    match = re.search(r"\baliases\s*:\s*(\[[^\]]*\])", source)
    if match:
        found.extend(re.findall(r"['\"]([^'\"]+)['\"]", match.group(1)))
    return found


def camel(name):
    return re.sub(r"-([a-z])", lambda m: m.group(1).upper(), name)


def capitalize(text):
    return re.sub(r"-([a-z])", lambda m: m.group(1).upper(), text[0].upper() + text[1:])


SCOPE_CLASSES = {
    "keyword": "Keyword",
    "built_in": "BuiltIn",
    "type": "Type",
    "literal": "Literal",
    "title": "Title",
    "symbol": "Symbol",
    "meta": "Meta",
    "attr": "Attr",
    "name": "Name",
    "variable": "Variable",
    "doctag": "Doctag",
    "section": "Section",
    "meta-keyword": "MetaKeyword",
    "meta-string": "MetaString",
    "template-variable": "TemplateVariable",
    "params": "Params",
    "class": "Class",
    "function": "Function",
    "regexp": "Regexp",
    "subst": "Subst",
}


def scope_class(scope):
    return SCOPE_CLASSES.get(scope, capitalize(scope))


def comment_style(style):
    return {
        "c": "CLike",
        "hash": "Hash",
        "semi": "Semi",
        "block": "Block",
        "quote": "Quote",
        "percent": "Percent",
        "xml": "Xml",
        "none": "None",
    }[style]


def block_style(style):
    if style is None:
        return None
    return {"slash": "Slash", "angle": "Angle"}[style]


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.expanduser("~"), "node_modules", "highlight.js", "lib", "languages"
    )
    if not os.path.isdir(directory):
        print("highlight.js was not found at " + directory, file=sys.stderr)
        return 1

    lines = [
        "// Generated by scripts/extract-code-languages.py — do not edit.",
        "//",
        "// The keyword lists come from highlight.js, the highlighter the reference",
        "// uses through cli-highlight.",
        "",
        '#include "ui/code_languages.hpp"',
        "",
        "namespace cc::ui::code {",
        "",
    ]

    entries = []
    for name, comments, block, backtick, triple in LANGUAGES:
        path = find_language_file(directory, name)
        if path is None:
            print("skipping the unknown language " + name, file=sys.stderr)
            continue

        source = strip_comments(read(path))
        scopes = collect(source, name)

        # Only the scopes the port knows are kept.
        kept = {}
        for scope in SCOPES:
            words = scopes.get(scope)
            if not words:
                continue
            unique = sorted({w.split("|")[0] for w in words if w.split("|")[0]})
            if unique:
                kept[scope] = unique

        if not kept:
            kept = {}

        prefix = camel(name)
        aliases = [name] + aliases_of(source)
        lines.append(
            "const char* const k%sAliases[] = {%s};"
            % (prefix, ", ".join('"%s"' % a for a in aliases))
        )
        for scope, words in kept.items():
            lines.append(
                "const char* const k%s%s[] = {%s};"
                % (prefix, scope_class(scope), ", ".join('"%s"' % w for w in words))
            )
        lines.append("")
        entries.append((name, prefix, aliases, comments, block, backtick, triple, kept))

    lines.append("const LanguageDefinition kDefinitions[] = {")
    for name, prefix, aliases, comments, block, backtick, triple, kept in entries:
        lines.append("    {")
        lines.append('        .name = "%s",' % name)
        lines.append("        .aliases = k%sAliases," % prefix)
        lines.append("        .alias_count = %d," % len(aliases))
        lines.append("        .comments = CommentStyle::%s," % comment_style(comments))
        if block:
            lines.append("        .block_comment = BlockComment::%s," % block_style(block))
        if backtick:
            lines.append("        .backtick_strings = true,")
        if triple:
            lines.append("        .triple_strings = true,")
        if kept:
            lines.append("        .scopes = {")
            for scope, words in kept.items():
                lines.append(
                    "            {Scope::%s, k%s%s, %d},"
                    % (scope_class(scope), prefix, scope_class(scope), len(words))
                )
            lines.append("        },")
        lines.append("    },")
    lines.append("};")
    lines.append("")
    lines.append("const LanguageDefinition* definitions() { return kDefinitions; }")
    lines.append("")
    lines.append(
        "size_t definition_count() { return sizeof(kDefinitions) / sizeof(kDefinitions[0]); }"
    )
    lines.append("")
    lines.append("} // namespace cc::ui::code")
    lines.append("")

    target = os.path.join(ROOT, "src", "ui", "code_languages.cpp")
    with open(target, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))

    print("wrote " + target)
    for name, _, _, _, _, _, _, kept in entries:
        print("  %-12s %d scopes" % (name, len(kept)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
