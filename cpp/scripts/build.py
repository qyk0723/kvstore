#!/usr/bin/env python3
"""Dependency-free build driver for environments without make or CMake.

    python3 scripts/build.py                 # build with the system g++
    python3 scripts/build.py --cxx clang++   # pick a compiler
    python3 scripts/build.py --target x86_64-linux-gnu --cxx zig
                                             # cross-compile (needs `zig c++`)

It is also the script used to verify the tree on Windows, where the project
itself targets Linux: the reactor falls back to a poll() backend there, so the
same sources build and the whole test suite runs.

This is a convenience, not the canonical build: CMakeLists.txt and the Makefile
are the supported entry points on Linux.
"""

import argparse
import os
import pathlib
import shlex
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"


def sources(pattern):
    return sorted(ROOT.glob(pattern))


def run(cmd):
    printable = " ".join(str(part) for part in cmd)
    if os.environ.get("VERBOSE"):
        print(printable)
    result = subprocess.run(cmd, cwd=ROOT)
    if result.returncode != 0:
        sys.exit(f"failed: {printable}")


def up_to_date(source, target, newest_header):
    if not target.exists():
        return False
    stamp = target.stat().st_mtime
    return stamp >= source.stat().st_mtime and stamp >= newest_header


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--std", default="c++17")
    parser.add_argument("--target", default="")
    parser.add_argument("--out", default=str(BUILD))
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--clean", action="store_true")
    args = parser.parse_args()

    out = pathlib.Path(args.out)
    if not out.is_absolute():
        out = ROOT / out
    if args.clean and out.exists():
        import shutil

        shutil.rmtree(out)

    base = shlex.split(args.cxx, posix=(os.name != "nt"))
    if args.target:
        base += ["-target", args.target]
    base += [f"-std={args.std}", "-O0" if args.debug else "-O2", "-g",
             "-Wall", "-Wextra", "-Wpedantic", "-I", str(ROOT / "include")]

    windows_target = ("windows" in args.target) if args.target else (os.name == "nt")
    exe_suffix = ".exe" if windows_target else ""

    every_source = sources("src/**/*.cpp")
    core = [s for s in every_source if s.name != "main.cpp"]
    main_src = [s for s in every_source if s.name == "main.cpp"]
    tests = sources("tests/*.cpp")
    bench = [ROOT / "bench" / "bench.cpp"]
    if not core:
        sys.exit("no sources found; run from the project directory")

    objects = {}
    headers = sorted(ROOT.glob("include/**/*.hpp"))
    newest_header = max((h.stat().st_mtime for h in headers), default=0.0)
    for source in every_source + tests + bench:
        obj = out / "obj" / source.relative_to(ROOT).with_suffix(".o")
        objects[source] = obj
        if not up_to_date(source, obj, newest_header):
            obj.parent.mkdir(parents=True, exist_ok=True)
            print(f"  CXX {source.relative_to(ROOT)}")
            run(base + ["-c", str(source), "-o", str(obj)])

    link_flags = ["-lws2_32"] if windows_target else ["-pthread"]
    core_objs = [str(objects[s]) for s in core]

    targets = [
        ("kvstore", core_objs + [str(objects[s]) for s in main_src]),
        ("kv_tests", core_objs + [str(objects[s]) for s in tests]),
        ("kv_bench", core_objs + [str(objects[s]) for s in bench]),
    ]
    for name, objs in targets:
        binary = out / (name + exe_suffix)
        print(f"  LD  {binary.relative_to(ROOT)}")
        run(base + ["-o", str(binary)] + objs + link_flags)

    print(f"\nbuilt into {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
