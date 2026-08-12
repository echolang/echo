#!/usr/bin/env python3
"""Rewrite the `--- OUT --->` section of every tests_eco case from a live echoc run.

The corpus pins the compiler's output byte for byte, so a deliberate change to how a diagnostic reads
has to be recorded in ~200 files at once.  This re-runs each case exactly as `EchoTests::echoc_command`
(tests/e2e_eco.cpp) builds its command line and replaces the OUT body with what came back.  Every other
section - IR, UNIT_IR, AST, RAST - and the whole settings header are left untouched.

**Run it over the entire corpus, not just the error cases.**  The ~350 passing cases' OUT is program
output, which this change cannot affect, so they must come back byte-identical.  If one of them moves,
that is a bug in the compiler and not a golden to accept - which is the whole reason to regenerate
broadly rather than narrowly.

    tools/regen_eco_goldens.py                 # show what would change
    tools/regen_eco_goldens.py --write         # write it
    tools/regen_eco_goldens.py --write errors  # one subdirectory
"""

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CORPUS = REPO / "tests_eco"
ECHOC = REPO / "build" / "echoc"

DELIMITER = re.compile(r"^--- ([A-Z_]+) --->$")


def belongs_to_a_module(eco: Path) -> bool:
    """Mirrors EchoTests::belongs_to_a_module - a source under a manifest directory is built through
    the manifest, never on its own, so it is not a case."""
    directory = CORPUS
    for part in eco.parent.relative_to(CORPUS).parts:
        if (directory / "module.eco").exists():
            return True
        directory = directory / part
    return (directory / "module.eco").exists()


def parse_test(path: Path):
    """The settings header and the byte range of the OUT body. Deliberately a much smaller parser than
    EchoTests::parse_eco_test_file: everything it validates, the suite validates again."""
    lines = path.read_text().splitlines(keepends=True)

    settings = {}
    out_start = None
    out_end = len(lines)
    seen_first_header = False

    for index, line in enumerate(lines):
        match = DELIMITER.match(line.rstrip("\n").rstrip("\r"))
        if match:
            if out_start is not None and out_end == len(lines):
                out_end = index
            if match.group(1) == "OUT":
                out_start = index + 1
            seen_first_header = True
            continue

        if seen_first_header:
            continue

        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if ":" in stripped:
            key, value = stripped.split(":", 1)
            settings[key.strip()] = value.strip()

    return lines, settings, out_start, out_end


def stdin_prefix(settings: dict) -> str:
    """Mirrors EcoTestFile::stdin_prefix - the `stdin:` words as one line each, piped in."""
    lines = settings.get("stdin", "").split()
    if not lines:
        return ""
    return "printf '" + "".join(line + "\\n" for line in lines) + "' | "


def build_command(eco: Path, settings: dict, scratch: Path, binary: Path | None) -> str:
    """The same command line tests/e2e_eco.cpp:echoc_command produces, in the same order."""
    mode = settings.get("mode", "run")
    is_build = mode == "build"

    flags = "--track-allocations "
    if settings.get("stdlib", "on") == "off":
        flags += "--no-stdlib "
    for manifest in settings.get("modules", "").split():
        flags += f"-m {shlex.quote(str(CORPUS / manifest))} "
    if settings.get("flags"):
        flags += settings["flags"] + " "

    arguments = settings.get("args", "")
    program_arguments = "" if (is_build or not arguments) else " -- " + arguments

    environment = "".join(pair + " " for pair in settings.get("env", "").split())

    # mirrors EcoTestFile::stdin_prefix - one word per line, ahead of the environment assignments so
    # those still belong to the program rather than to the printf
    feed = stdin_prefix(settings)

    subcommand = f"build -o {shlex.quote(str(binary))} " if is_build else "run "

    return (
        feed
        + environment
        + shlex.quote(str(ECHOC))
        + " "
        + subcommand
        + "--build-dir "
        + shlex.quote(str(scratch / "cache"))
        + " "
        + flags
        + shlex.quote(str(eco))
        + program_arguments
        + " 2>&1"
    )


def capture(command: str) -> str:
    result = subprocess.run(command, shell=True, capture_output=True, text=True, cwd=REPO)
    return result.stdout


def regenerate(path: Path, eco: Path, write: bool) -> bool:
    lines, settings, out_start, out_end = parse_test(path)
    if out_start is None:
        return False

    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp)
        is_build = settings.get("mode", "run") == "build"
        binary = scratch / eco.stem if is_build else None

        output = capture(build_command(eco, settings, scratch, binary))

        # a `mode: build` case asserts the *linked program's* output, not the build log - the build's
        # stdout carries absolute object paths, which is why the suite judges it by exit status alone.
        # the program's arguments and environment belong to this invocation and not to the build, which
        # is the one place the two modes' command lines differ
        if is_build and binary is not None and binary.exists():
            environment = "".join(pair + " " for pair in settings.get("env", "").split())
            arguments = settings.get("args", "")
            output = capture(
                stdin_prefix(settings)
                + environment
                + shlex.quote(str(binary))
                + (" " + arguments if arguments else "")
                + " 2>&1")
        elif is_build:
            output = ""

    # exactly one, mirroring EchoTests::strip_trailing_newline - the harness forgives one and compares
    # the rest byte for byte, so stripping every trailing newline here would record a golden the suite
    # then reads back as one line short
    body = output[:-1] if output.endswith("\n") else output
    rewritten = "".join(lines[:out_start] + ([body + "\n"] if body else []) + lines[out_end:])

    # compared as text and not as a list of lines: the captured body is one string where the file holds
    # one entry per line, so a list comparison reports every unchanged golden as changed
    if rewritten == "".join(lines):
        return False

    if write:
        path.write_text(rewritten)

    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("subdir", nargs="?", default="", help="limit to one directory under tests_eco/")
    parser.add_argument("--write", action="store_true", help="write the files instead of listing them")
    args = parser.parse_args()

    if not ECHOC.exists():
        print(f"no compiler at {ECHOC} - build it first", file=sys.stderr)
        return 1

    if shutil.which("clang") is None:
        print("clang is not on PATH - `mode: build` cases would regenerate as empty", file=sys.stderr)
        return 1

    root = CORPUS / args.subdir if args.subdir else CORPUS

    changed = []
    for eco in sorted(root.rglob("*.eco")):
        if belongs_to_a_module(eco):
            continue

        test = eco.with_suffix(".test")
        if not test.exists():
            continue

        if regenerate(test, eco, args.write):
            changed.append(test.relative_to(REPO))

    verb = "rewrote" if args.write else "would rewrite"
    for path in changed:
        print(f"{verb} {path}")

    print(f"\n{verb} {len(changed)} of the corpus")
    return 0


if __name__ == "__main__":
    sys.exit(main())
