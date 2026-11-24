#!/usr/bin/env python3
"""Verify vendored dependency versions and license files."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / "lib"

CHECKS = {
    "SDL3": ["LICENSE.txt", "README.md", "include/SDL3/SDL.h"],
    "SDL3_ttf": ["LICENSE.txt", "README.md", "include/SDL3_ttf/SDL_ttf.h"],
    "SDL3_image": ["LICENSE.txt", "README.md", "include/SDL3_image/SDL_image.h"],
    "freetype": ["LICENSE.TXT", "README", "include/freetype/freetype.h"],
    "nlohmann": ["LICENSE.MIT", "README.md", "single_include/nlohmann/json.hpp"],
    "doctest": ["LICENSE.txt", "README.md", "doctest.h"],
}


def main() -> int:
    failures: list[str] = []
    for name, files in CHECKS.items():
        base = LIB / name
        missing = [rel for rel in files if not (base / rel).exists()]
        if missing:
            failures.extend(f"missing {name}/{rel}" for rel in missing)
            print(f"{name:12} FAIL")
        else:
            print(f"{name:12} ok")

    inter = ROOT / "assets" / "fonts" / "Inter"
    for rel in ["Inter-Regular.ttf", "OFL.txt"]:
        if not (inter / rel).exists():
            failures.append(f"missing assets/fonts/Inter/{rel}")

    if failures:
        print("\nFailures:", *failures, sep="\n  ", file=sys.stderr)
        return 1
    print("dependency verification passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
