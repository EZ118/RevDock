#!/usr/bin/env python3
"""Remove documentation, tests, examples and IDE projects from vendored deps.

Run from the repository root. The script is intentionally conservative: it
only removes paths that are not needed by the CMake builds and keeps every
LICENSE/README/CREDITS file.
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / "lib"

REMOVALS = {
    "SDL3": [
        ".github", "android-project", "docs", "examples", "test", "VisualC",
        "VisualC-GDK", "Xcode", "AGENTS.md", "CLAUDE.md", "BUGS.txt",
        "WhatsNew.txt", ".wikiheaders-options",
    ],
    "SDL3_ttf": [
        ".github", "build-scripts", "docs", "examples", "external", "mingw",
        "VisualC", "Xcode", "Android.mk", "INSTALL.md", ".wikiheaders-options",
    ],
    "SDL3_image": [
        ".github", "build-scripts", "docs", "examples", "external", "test",
        "VisualC", "Xcode", "Android.mk", "INSTALL.md", ".wikiheaders-options",
    ],
    "freetype": [
        ".gitlab-ci.yml", ".gitmodules", ".mailmap", "devel", "docs", "objs",
        "subprojects", "tests", "autogen.sh", "configure", "Makefile",
        "meson.build", "meson_options.txt", "vms_make.com", "MSBuild.rsp",
        "MSBuild.sln",
    ],
}

REQUIRED = {
    "SDL3": ["LICENSE.txt", "README.md", "CMakeLists.txt", "include", "src"],
    "SDL3_ttf": ["LICENSE.txt", "README.md", "CMakeLists.txt", "include", "src"],
    "SDL3_image": ["LICENSE.txt", "README.md", "CMakeLists.txt", "include", "src"],
    "freetype": ["LICENSE.TXT", "README", "CMakeLists.txt", "include", "src", "builds"],
    "nlohmann": ["LICENSE.MIT", "README.md", "single_include/nlohmann/json.hpp"],
    "doctest": ["LICENSE.txt", "README.md", "doctest.h"],
}


def trim() -> int:
    for name, paths in REMOVALS.items():
        base = LIB / name
        if not base.is_dir():
            print(f"warning: {base} is missing", file=sys.stderr)
            continue
        for rel in paths:
            target = base / rel
            if target.exists():
                if target.is_dir():
                    shutil.rmtree(target)
                else:
                    target.unlink()
                print(f"removed {target.relative_to(ROOT)}")

    missing = []
    for name, paths in REQUIRED.items():
        base = LIB / name
        for rel in paths:
            if not (base / rel).exists():
                missing.append(f"{name}/{rel}")
    if missing:
        print("missing required files:", *missing, sep="\n  ", file=sys.stderr)
        return 1
    print("third-party tree is trimmed and required license/readme files are present")
    return 0


if __name__ == "__main__":
    raise SystemExit(trim())
