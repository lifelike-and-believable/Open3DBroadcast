#!/usr/bin/env python3
"""Check that every tracked prebuilt third-party binary is recorded in its library README.

Each prebuilt library under a ThirdParty/ folder of the plugins keeps a README.md
with an inventory table (see moq-ffi, livekit_ffi, nng, opus). A tracked binary
(.dll, .lib, .so, .dylib, .a) must have a row there whose first cell holds the
binary's path relative to that README in backticks, and whose row holds the
file's SHA256 in backticks. The check fails when

  1. a tracked binary has no README in its ThirdParty/<library> folder,
  2. the README has no row for it,
  3. the row's SHA256 differs from the file's (a binary was replaced without
     updating the README, or the README was edited by hand), or
  4. a row names a .dll/.lib/.so/.dylib/.a that is not in the tree, unless the
     row says "not in the tree" (debug symbols are release assets).

Binaries come from `git ls-files`, so ignored local build output is not checked.
Pass --root to check another checkout.

Python 3.8 or later, standard library only. Exit codes: 0 no violation; 1 at
least one violation; 2 bad input (not a git checkout). --self-test runs the
check against a small generated tree, one consistent and one with a wrong hash,
a missing row, a missing README and a stale row, and exits 0 only if it passes
the clean tree and reports every defect in the bad one.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

BINARY_SUFFIXES = (".dll", ".lib", ".so", ".dylib", ".a")
SCAN_PREFIX = "ProjectSandbox/Plugins/"
HEX64 = re.compile(r"`([0-9A-Fa-f]{64})`")
BACKTICKED = re.compile(r"`([^`]+)`")


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def is_prebuilt(rel):
    parts = rel.split("/")
    return (rel.startswith(SCAN_PREFIX) and "ThirdParty" in parts[:-1]
            and rel.lower().endswith(BINARY_SUFFIXES))


def find_readme(root, rel):
    """Nearest README.md above the binary, not above the ThirdParty folder itself."""
    parts = rel.split("/")[:-1]
    while parts and parts[-1] != "ThirdParty":
        candidate = "/".join(parts + ["README.md"])
        if os.path.isfile(os.path.join(root, candidate)):
            return "/".join(parts)
        parts.pop()
    return None


def read_rows(readme_path):
    """{path relative to the README: (sha256 or None, row text)} for the table rows."""
    rows = {}
    with open(readme_path, encoding="utf-8") as handle:
        for line in handle:
            if not line.lstrip().startswith("|"):
                continue
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            first = BACKTICKED.search(cells[0]) if cells else None
            if not first:
                continue
            hashes = HEX64.findall(line)
            rows[first.group(1)] = (hashes[0].upper() if hashes else None, line)
    return rows


def check(root, tracked):
    errors = []
    binaries = sorted(p for p in tracked if is_prebuilt(p))
    libraries = {}
    for rel in binaries:
        folder = find_readme(root, rel)
        if folder is None:
            errors.append(f"{rel}: no README.md in its ThirdParty folder records this binary")
            continue
        libraries.setdefault(folder, []).append(rel)
    for folder, files in sorted(libraries.items()):
        readme = f"{folder}/README.md"
        rows = read_rows(os.path.join(root, readme))
        for rel in files:
            name = rel[len(folder) + 1:]
            row = rows.get(name)
            if row is None:
                errors.append(f"{rel}: {readme} has no row for `{name}`")
            elif row[0] is None:
                errors.append(f"{rel}: the row in {readme} has no SHA256")
            else:
                actual = sha256_of(os.path.join(root, rel))
                if row[0] != actual:
                    errors.append(f"{rel}: SHA256 is {actual}, {readme} records {row[0]}")
        for name, (_, line) in sorted(rows.items()):
            if name.lower().endswith(BINARY_SUFFIXES) and "not in the tree" not in line \
                    and not os.path.isfile(os.path.join(root, folder, name)):
                errors.append(f"{readme}: row for `{name}` but the file is not in the tree")
    return errors


def tracked_files(root):
    try:
        out = subprocess.run(["git", "-C", root, "ls-files"], check=True,
                             capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"error: cannot list tracked files in {root}: {exc}", file=sys.stderr)
        sys.exit(2)
    return out.splitlines()


def self_test():
    root = tempfile.mkdtemp(prefix="o3d-binaries-")
    try:
        lib = "ProjectSandbox/Plugins/P/Source/M/ThirdParty/foo"

        def write(rel, data):
            path = os.path.join(root, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as handle:
                handle.write(data)

        write(f"{lib}/lib/Win64/foo.lib", b"foo")
        good = hashlib.sha256(b"foo").hexdigest().upper()
        readme = (f"| Relative Path | SHA256 |\n| --- | --- |\n| `lib/Win64/foo.lib` | `{good}` |\n"
                  "| `foo.pdb` (not in the tree) | `" + "0" * 64 + "` |\n")
        write(f"{lib}/README.md", readme.encode())
        tracked = [f"{lib}/lib/Win64/foo.lib", f"{lib}/README.md"]
        if check(root, tracked):
            print("self-test failed: the consistent tree was rejected", file=sys.stderr)
            return 1

        write(f"{lib}/README.md", readme.replace(good, "1" * 64).encode()
              + b"| `lib/Win64/gone.lib` | `" + b"2" * 64 + b"` |\n")
        write(f"{lib}/lib/Win64/bar.lib", b"bar")
        write("ProjectSandbox/Plugins/P/Source/M/ThirdParty/baz/baz.dll", b"baz")
        tracked += [f"{lib}/lib/Win64/bar.lib", "ProjectSandbox/Plugins/P/Source/M/ThirdParty/baz/baz.dll"]
        found = "\n".join(check(root, tracked))
        expected = ["records " + "1" * 64, "has no row for `lib/Win64/bar.lib`",
                    "no README.md in its ThirdParty folder", "`lib/Win64/gone.lib` but the file is not in the tree"]
        missing = [e for e in expected if e not in found]
        if missing:
            print(f"self-test failed: not reported: {missing}\n{found}", file=sys.stderr)
            return 1
        print("self-test passed")
        return 0
    finally:
        shutil.rmtree(root, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    root = os.path.abspath(args.root)
    tracked = tracked_files(root)
    errors = check(root, tracked)
    if errors:
        print("Third-party binaries and their README inventories disagree:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    count = sum(1 for p in tracked if is_prebuilt(p))
    print(f"OK: {count} tracked prebuilt binaries match their README inventories.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
