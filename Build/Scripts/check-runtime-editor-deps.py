#!/usr/bin/env python3
"""Check that the plugin's runtime modules use no editor-only or Slate code.

The rule (WP-F7, ADR 0010; FAB-7, SND-34, TRB-46): editor UI lives in modules
of Type "Editor" (Open3DBroadcastEditor, Open3DBroadcastTests). A module whose
.uplugin Type is a runtime type (Runtime, RuntimeNoCommandlet, ClientOnly, ...)
must not

  1. name an editor-only or Slate module in its Build.cs, anywhere in the file
     (a Target.bBuildEditor block included), or
  2. #include an editor-only or Slate header in its sources (.h, .cpp, .inl),
     WITH_EDITOR-guarded or not.

Build.cs files are checked by their string literals, after comments are
removed, so a comment that names a module is fine. InputCore is not on the
list: it is a runtime module. A line that must stay can carry
"o3d-allow-editor-dependency: <reason>"; the reason is required. Nothing uses
that marker today.

Not checked: ThirdParty/ directories, and module directories that have no
.uplugin entry (Open3DBroadcastBuildFlags only holds shared build rules).

Plugins checked by default: ProjectSandbox/Plugins/Open3DBroadcast and the
WebRTC add-on ProjectSandbox/Plugins/Open3DBroadcastWebRTC (WP-F11). Pass
--plugin-dir (repeatable) to check others.

Python 3.8 or later, standard library only. Exit codes: 0 no violation; 1 at
least one violation; 2 bad input (no .uplugin, unreadable file). --self-test
runs the check against small generated plugins, one clean and one with a bad
Build.cs line and a bad include, and exits 0 only if it passes the clean one
and fails the bad one.
"""

import argparse
import json
import os
import re
import shutil
import sys
import tempfile

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_PLUGIN_DIRS = [
    os.path.join(REPO_ROOT, "ProjectSandbox", "Plugins", "Open3DBroadcast"),
    os.path.join(REPO_ROOT, "ProjectSandbox", "Plugins", "Open3DBroadcastWebRTC"),
]

# .uplugin module types that are built into games (UBT's ModuleHostType values).
RUNTIME_TYPES = {
    "Runtime",
    "RuntimeNoCommandlet",
    "RuntimeAndProgram",
    "CookedOnly",
    "ClientOnly",
    "ClientOnlyNoCommandlet",
    "ServerOnly",
}

# Engine modules a runtime module must not depend on. Slate and SlateCore are here because the
# plugin's runtime modules have no UI (ADR 0010); AppFramework is Slate application widgets.
FORBIDDEN_MODULES = {
    "AppFramework",
    "AssetTools",
    "BlueprintGraph",
    "Blutility",
    "ContentBrowser",
    "DetailCustomizations",
    "EditorFramework",
    "EditorStyle",
    "EditorSubsystem",
    "EditorWidgets",
    "GraphEditor",
    "Kismet",
    "KismetWidgets",
    "LevelEditor",
    "LiveLinkEditor",
    "MainFrame",
    "PropertyEditor",
    "Slate",
    "SlateCore",
    "ToolMenus",
    "ToolWidgets",
    "UMGEditor",
    "UnrealEd",
    "WorkspaceMenuStructure",
}

# Headers from those modules. Exact names (compared against the last path component too).
FORBIDDEN_HEADERS = {
    "DetailCategoryBuilder.h",
    "DetailLayoutBuilder.h",
    "DetailWidgetRow.h",
    "Editor.h",
    "EditorStyleSet.h",
    "IDetailChildrenBuilder.h",
    "IDetailCustomization.h",
    "IDetailGroup.h",
    "IDetailsView.h",
    "IPropertyTypeCustomization.h",
    "LevelEditor.h",
    "PropertyCustomizationHelpers.h",
    "PropertyEditorModule.h",
    "PropertyHandle.h",
    "ScopedTransaction.h",
    "Slate.h",
    "SlateBasics.h",
    "SlateCore.h",
    "SlateExtras.h",
    "ToolMenus.h",
    "UnrealEd.h",
    "UnrealEdGlobals.h",
}

# Header path prefixes from those modules.
FORBIDDEN_HEADER_PREFIXES = (
    "Editor/",
    "EditorFramework/",
    "Framework/Application/",
    "Framework/Commands/",
    "Framework/Docking/",
    "Framework/MultiBox/",
    "Framework/Notifications/",
    "Kismet2/",
    "Styling/AppStyle",
    "Styling/SlateStyle",
    "Subsystems/EditorSubsystem",
    "Toolkits/",
    "Widgets/",
)

SOURCE_EXTENSIONS = (".h", ".cpp", ".inl")
ALLOW_MARKER = re.compile(r"o3d-allow-editor-dependency:\s*(\S.*)?$")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')
STRING_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


class InputError(Exception):
    pass


def read_text(path):
    try:
        with open(path, encoding="utf-8-sig", errors="replace") as f:
            return f.read()
    except OSError as e:
        raise InputError("cannot read {}: {}".format(path, e))


def runtime_modules(plugin_dir):
    """Names of the .uplugin modules whose Type is a runtime type."""
    uplugins = [n for n in os.listdir(plugin_dir) if n.endswith(".uplugin")]
    if len(uplugins) != 1:
        raise InputError("expected one .uplugin in {}, found {}".format(plugin_dir, len(uplugins)))
    try:
        descriptor = json.loads(read_text(os.path.join(plugin_dir, uplugins[0])))
    except ValueError as e:
        raise InputError("cannot parse {}: {}".format(uplugins[0], e))
    modules = []
    for entry in descriptor.get("Modules", []):
        if entry.get("Type") in RUNTIME_TYPES:
            modules.append(entry["Name"])
    return modules


def strip_cs_comments(line, in_block):
    """Remove // and /* */ comments from one C# line, keeping string literals. Returns (text, in_block)."""
    out = []
    i = 0
    in_string = False
    while i < len(line):
        c = line[i]
        if in_block:
            if line.startswith("*/", i):
                in_block = False
                i += 2
            else:
                i += 1
            continue
        if in_string:
            out.append(c)
            if c == "\\" and i + 1 < len(line):
                out.append(line[i + 1])
                i += 2
                continue
            if c == '"':
                in_string = False
            i += 1
            continue
        if c == '"':
            in_string = True
            out.append(c)
            i += 1
            continue
        if line.startswith("//", i):
            break
        if line.startswith("/*", i):
            in_block = True
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out), in_block


def allowed(line, violations, path, number):
    """True when the line carries the allow marker with a reason; a marker without one is itself a violation."""
    match = ALLOW_MARKER.search(line)
    if not match:
        return False
    if not (match.group(1) or "").strip():
        violations.append("{}:{}: o3d-allow-editor-dependency needs a reason".format(path, number))
    return True


def check_build_cs(path, rel, violations):
    in_block = False
    for number, raw in enumerate(read_text(path).splitlines(), 1):
        code, in_block = strip_cs_comments(raw, in_block)
        for literal in STRING_RE.findall(code):
            if literal in FORBIDDEN_MODULES and not allowed(raw, violations, rel, number):
                violations.append("{}:{}: runtime module depends on editor-only module \"{}\"".format(rel, number, literal))


def forbidden_header(header):
    name = header.replace("\\", "/")
    if name in FORBIDDEN_HEADERS or name.rsplit("/", 1)[-1] in FORBIDDEN_HEADERS:
        return True
    return name.startswith(FORBIDDEN_HEADER_PREFIXES)


def check_sources(module_dir, plugin_dir, violations):
    for root, dirs, files in os.walk(module_dir):
        dirs[:] = sorted(d for d in dirs if d != "ThirdParty")
        for name in sorted(files):
            if not name.endswith(SOURCE_EXTENSIONS):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, plugin_dir).replace(os.sep, "/")
            for number, line in enumerate(read_text(path).splitlines(), 1):
                match = INCLUDE_RE.match(line)
                if match and forbidden_header(match.group(1)) and not allowed(line, violations, rel, number):
                    violations.append("{}:{}: runtime module includes editor-only header \"{}\"".format(rel, number, match.group(1)))


def check(plugin_dir):
    """Returns (violations, checked module names)."""
    violations = []
    modules = runtime_modules(plugin_dir)
    for module in modules:
        module_dir = os.path.join(plugin_dir, "Source", module)
        build_cs = os.path.join(module_dir, module + ".Build.cs")
        if not os.path.isfile(build_cs):
            raise InputError("runtime module {} has no {}".format(module, os.path.relpath(build_cs, plugin_dir)))
        check_build_cs(build_cs, os.path.relpath(build_cs, plugin_dir).replace(os.sep, "/"), violations)
        check_sources(module_dir, plugin_dir, violations)
    return violations, modules


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def make_fixture(root, bad):
    """A two-module plugin: one runtime module and one editor module that may use Slate."""
    write(os.path.join(root, "Fixture.uplugin"), json.dumps({"Modules": [
        {"Name": "FixtureRuntime", "Type": "Runtime"},
        {"Name": "FixtureEditor", "Type": "Editor"},
    ]}))
    deps = '"Core", "Engine"' + (', "UnrealEd"' if bad else "")
    write(os.path.join(root, "Source", "FixtureRuntime", "FixtureRuntime.Build.cs"),
          '// "Slate" in a comment is fine\nPrivateDependencyModuleNames.AddRange(new string[] { ' + deps + ' });\n')
    include = '#include "Widgets/SCompoundWidget.h"\n' if bad else '#include "CoreMinimal.h"\n'
    write(os.path.join(root, "Source", "FixtureRuntime", "Private", "Fixture.cpp"), include)
    write(os.path.join(root, "Source", "FixtureRuntime", "ThirdParty", "lib", "Vendored.h"), '#include "Editor.h"\n')
    write(os.path.join(root, "Source", "FixtureEditor", "FixtureEditor.Build.cs"),
          'PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "UnrealEd" });\n')
    write(os.path.join(root, "Source", "FixtureEditor", "Private", "Panel.cpp"), '#include "Widgets/SCompoundWidget.h"\n')


def self_test():
    root = tempfile.mkdtemp(prefix="o3d-editor-deps-")
    try:
        clean = os.path.join(root, "clean")
        bad = os.path.join(root, "bad")
        make_fixture(clean, bad=False)
        make_fixture(bad, bad=True)
        clean_violations, _ = check(clean)
        bad_violations, _ = check(bad)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    ok = not clean_violations and len(bad_violations) == 2
    print("self-test: clean fixture -> {} violation(s); bad fixture -> {} violation(s)".format(len(clean_violations), len(bad_violations)))
    for v in clean_violations + bad_violations:
        print("  " + v)
    print("self-test " + ("passed" if ok else "FAILED"))
    return 0 if ok else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--plugin-dir", action="append", dest="plugin_dirs",
                        help="plugin root; repeatable (default: Open3DBroadcast and Open3DBroadcastWebRTC)")
    parser.add_argument("--self-test", action="store_true", help="check the checker against generated fixtures")
    args = parser.parse_args(argv)

    violations = []
    try:
        if args.self_test:
            return self_test()
        for plugin_dir in args.plugin_dirs or DEFAULT_PLUGIN_DIRS:
            found, modules = check(os.path.abspath(plugin_dir))
            print("Checked runtime modules of {}: {}".format(os.path.basename(os.path.abspath(plugin_dir)), ", ".join(modules)))
            violations.extend(found)
    except InputError as e:
        print("error: {}".format(e), file=sys.stderr)
        return 2

    if violations:
        for v in violations:
            print("::error::" + v)
        print("{} violation(s). Editor UI belongs in an Editor-type module such as Open3DBroadcastEditor (ADR 0010).".format(len(violations)))
        return 1
    print("OK: no runtime module uses an editor-only module or header.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
