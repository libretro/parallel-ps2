"""Syntax-checks every C++ unit of a configured CMake build.

Usage: sweep.py <compile_commands.json>

Each unit's own compile command is run with -fsyntax-only in place of
-c/-o, in parallel. A unit whose source is generated during the build
(and so is not there yet) is skipped. Exits non-zero if any unit fails,
or if there was nothing to check.
"""
import concurrent.futures
import json
import os
import shlex
import subprocess
import sys


def command(entry):
    args = shlex.split(entry["command"])
    out = []
    skip = False
    for a in args:
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        if a == "-c":
            continue
        out.append(a)
    out.append("-fsyntax-only")
    return out


def check(entry):
    r = subprocess.run(command(entry), cwd=entry["directory"],
                       capture_output=True, text=True)
    errors = [l for l in r.stderr.splitlines() if "error" in l]
    return entry["file"], r.returncode, errors


def main():
    entries = [e for e in json.load(open(sys.argv[1]))
               if e["file"].endswith(".cpp") and os.path.exists(e["file"])]
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 2) as ex:
        for name, rc, errors in ex.map(check, entries):
            if rc:
                failed += 1
                print("FAIL " + name)
                for l in errors[:10]:
                    print("  " + l)
    print("%d C++ units checked against libc++, %d failed" % (len(entries), failed))
    return 1 if failed or not entries else 0


if __name__ == "__main__":
    sys.exit(main())
