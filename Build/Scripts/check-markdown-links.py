#!/usr/bin/env python3
"""Check that the links in the repository's Markdown files point at something.

WP-D4 (docs stay honest): every git-tracked .md file is scanned for links, and
a link fails when

  - it is relative (or a GitHub link into this repository's own tree, such as
    https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/wire-format.md)
    and the file or folder it names is not in this checkout;
  - it has a #fragment into a Markdown file (or the same file) that has no
    heading or explicit anchor (<a name="..."> or id="...") with that slug.

Other web links are not fetched: the check runs offline and never flakes on a
site being down. Links inside fenced code blocks and inline code are ignored.
Headings are turned into anchors the way GitHub does it: lower case, characters
other than letters, digits, spaces, hyphens and underscores removed, spaces
turned into hyphens, and -1, -2 ... appended to repeated headings.

Not checked: files under a ThirdParty/ or thirdparty/ directory (vendored
documentation).

Python 3.8 or later, standard library only. Exit codes: 0 every link resolves;
1 at least one broken link; 2 bad input (not a git checkout). --self-test checks
the checker against generated fixtures.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import urllib.parse

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OWN_TREE_RE = re.compile(
    r"^https://github\.com/lifelike-and-believable/Open3DBroadcast/(?:blob|tree)/[^/]+/(?P<path>[^#?]*)(?:#(?P<frag>.*))?$",
    re.IGNORECASE)
INLINE_LINK_RE = re.compile(r"!?\[(?:[^\[\]]|\[[^\]]*\])*\]\(\s*<?(?P<target>[^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
REFERENCE_DEF_RE = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*<?(?P<target>\S+?)>?(?:\s+.*)?$")
HEADING_RE = re.compile(r"^\s{0,3}(#{1,6})\s+(?P<text>.*?)\s*#*\s*$")
EXPLICIT_ANCHOR_RE = re.compile(r"<a\s+[^>]*(?:name|id)\s*=\s*\"(?P<id>[^\"]+)\"|\sid\s*=\s*\"(?P<id2>[^\"]+)\"", re.IGNORECASE)
FENCE_RE = re.compile(r"^\s{0,3}(```|~~~)")
INLINE_CODE_RE = re.compile(r"`+[^`]*`+")


def is_skipped(rel_path):
    parts = rel_path.replace("\\", "/").split("/")
    return any(part in ("ThirdParty", "thirdparty") for part in parts[:-1])


def tracked_markdown(root):
    try:
        out = subprocess.run(["git", "ls-files", "-z", "--", "*.md"], cwd=root, check=True,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit("error: cannot list tracked files in {}: {}".format(root, error))
    return [p for p in out.decode("utf-8").split("\0") if p and not is_skipped(p)]


def read_lines(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read().splitlines()


def strip_markup(text):
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)  # links and images keep their text
    text = re.sub(r"<[^>]+>", "", text)                       # inline HTML
    return text.replace("`", "").replace("*", "")


def slugify(text):
    text = strip_markup(text).strip().lower()
    text = "".join(ch for ch in text if ch.isalnum() or ch in " -_")
    return text.replace(" ", "-")


def anchors_of(lines):
    anchors, seen, in_fence = set(), {}, False
    for line in lines:
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        for match in EXPLICIT_ANCHOR_RE.finditer(line):
            anchors.add(match.group("id") or match.group("id2"))
        heading = HEADING_RE.match(line)
        if heading:
            slug = slugify(heading.group("text"))
            count = seen.get(slug, 0)
            seen[slug] = count + 1
            anchors.add(slug if count == 0 else "{}-{}".format(slug, count))
    return anchors


def links_of(lines):
    in_fence = False
    for number, line in enumerate(lines, start=1):
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        code_free = INLINE_CODE_RE.sub("", line)
        for match in INLINE_LINK_RE.finditer(code_free):
            yield number, match.group("target")
        definition = REFERENCE_DEF_RE.match(code_free)
        if definition:
            yield number, definition.group("target")


def resolve(root, source_rel, target):
    """Returns (path relative to root or None for the same file, fragment), or None to skip."""
    own = OWN_TREE_RE.match(target)
    if own:
        return urllib.parse.unquote(own.group("path")).rstrip("/"), own.group("frag")
    if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.IGNORECASE):
        return None  # http:, https:, mailto: and the like
    path, _, fragment = target.partition("#")
    path = urllib.parse.unquote(path.split("?")[0])
    if not path:
        return None if not fragment else (None, fragment)
    if path.startswith("/"):
        resolved = path.lstrip("/")
    else:
        resolved = os.path.normpath(os.path.join(os.path.dirname(source_rel), path)).replace("\\", "/")
    return resolved, fragment or None


def check(root):
    problems = []
    anchor_cache = {}

    def anchors_for(rel):
        if rel not in anchor_cache:
            anchor_cache[rel] = anchors_of(read_lines(os.path.join(root, rel)))
        return anchor_cache[rel]

    for source in tracked_markdown(root):
        for number, target in links_of(read_lines(os.path.join(root, source))):
            result = resolve(root, source, target)
            if result is None:
                continue
            rel, fragment = result
            if rel is not None:
                if rel.startswith("..") or not os.path.exists(os.path.join(root, rel)):
                    problems.append("{}:{}: {} -> no such file or folder".format(source, number, target))
                    continue
            anchor_file = source if rel is None else rel
            if fragment and anchor_file.lower().endswith(".md") and os.path.isfile(os.path.join(root, anchor_file)):
                if urllib.parse.unquote(fragment).lower() not in anchors_for(anchor_file):
                    problems.append("{}:{}: {} -> no heading or anchor '#{}' in {}".format(source, number, target, fragment, anchor_file))
    return problems


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def self_test():
    root = tempfile.mkdtemp(prefix="o3d-md-links-")
    subprocess.run(["git", "init", "-q", root], check=True)
    write(os.path.join(root, "docs", "guide.md"),
          "# Guide\n\n## Quick Start\n\n## Quick Start\n\n## Options (`udp.mtu`)\n\n<a name=\"custom\"></a>\n")
    write(os.path.join(root, "ThirdParty", "lib", "README.md"), "[gone](missing.md)\n")
    good = ("[g](docs/guide.md) [q](docs/guide.md#quick-start) [q1](docs/guide.md#quick-start-1) "
            "[o](docs/guide.md#options-udpmtu) [c](docs/guide.md#custom) [d](docs/) [self](#top)\n\n# Top\n\n"
            "[web](https://example.com/x) [own](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/guide.md#guide)\n"
            "`[code](missing.md)`\n\n```\n[fenced](missing.md)\n```\n\n[ref]: docs/guide.md\n")
    bad = ("[f](docs/missing.md) [a](docs/guide.md#nope) [s](#nope) [u](../outside.md)\n"
           "[own](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/missing.md)\n\n[ref]: missing.md\n")
    write(os.path.join(root, "good.md"), good)
    subprocess.run(["git", "add", "-A"], cwd=root, check=True)
    clean = check(root)
    write(os.path.join(root, "bad.md"), bad)
    subprocess.run(["git", "add", "-A"], cwd=root, check=True)
    broken = check(root)
    print("self-test: good fixture -> {} problem(s); bad fixture -> {} problem(s)".format(len(clean), len(broken)))
    for problem in clean:
        print("  unexpected: " + problem)
    ok = not clean and len(broken) == 6
    if not ok:
        for problem in broken:
            print("  bad fixture: " + problem)
    print("self-test " + ("passed" if ok else "FAILED"))
    return 0 if ok else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--root", default=REPO_ROOT, help="repository root (default: this checkout)")
    parser.add_argument("--self-test", action="store_true", help="check the checker against generated fixtures")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not os.path.isdir(os.path.join(args.root, ".git")) and not os.path.isfile(os.path.join(args.root, ".git")):
        print("error: {} is not a git checkout".format(args.root), file=sys.stderr)
        return 2
    problems = check(args.root)
    for problem in problems:
        print(problem)
    print("{} broken link(s) in {} Markdown file(s)".format(len(problems), len(tracked_markdown(args.root))))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
