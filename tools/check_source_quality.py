#!/usr/bin/env python3
"""Run the maintained-fork formatting, analyzer, and structure checks."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

UPSTREAM_BASE = "550f700c037e8713e4567227514424621cd39cd7"
DEFAULT_LINE_CAP = 1_200
LEGACY_LINE_CAPS = {
    "emitter.c": 3_089,
    "interpreter.c": 1_303,
    "lightrec.c": 2_073,
    "optimizer.c": 2_454,
}


def run(command: list[str], *, cwd: Path, input_text: str | None = None) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        input=input_text,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"{result.stdout}{result.stderr}"
        )
    return result.stdout


def source_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for suffix in ("*.c", "*.h"):
        files.extend(root.glob(suffix))
        files.extend((root / "tests").glob(suffix))
    return sorted(files)


def check_structure(root: Path) -> None:
    findings: list[str] = []
    for path in source_files(root):
        relative = path.relative_to(root).as_posix()
        line_count = len(path.read_text(encoding="utf-8").splitlines())
        cap = LEGACY_LINE_CAPS.get(relative, DEFAULT_LINE_CAP)
        if line_count > cap:
            findings.append(f"{relative}: {line_count} lines exceeds cap {cap}")
    if findings:
        raise RuntimeError("structure limits failed:\n" + "\n".join(findings))


def check_format(root: Path, clang_format_diff: str, clang_format: str) -> None:
    patch = run(
        [
            "git",
            "diff",
            "--unified=0",
            UPSTREAM_BASE,
            "--",
            "*.c",
            "*.h",
            ":!tlsf/**",
        ],
        cwd=root,
    )
    proposed = run(
        [clang_format_diff, "-p1", "-style=file"],
        cwd=root,
        input_text=patch,
    )
    if proposed:
        raise RuntimeError("changed source is not clang-formatted:\n" + proposed)

    untracked = run(
        ["git", "ls-files", "--others", "--exclude-standard", "--", "*.c", "*.h"],
        cwd=root,
    ).splitlines()
    if untracked:
        run([clang_format, "--dry-run", "--Werror", *untracked], cwd=root)


def check_tidy(root: Path, build: Path, clang_tidy: str) -> int:
    compile_commands = build / "compile_commands.json"
    if not compile_commands.is_file():
        raise RuntimeError(f"missing compile database: {compile_commands}")
    entries = json.loads(compile_commands.read_text(encoding="utf-8"))
    translation_units = sorted(
        {
            Path(entry["file"]).resolve()
            for entry in entries
            if "/tlsf/" not in Path(entry["file"]).resolve().as_posix()
        }
    )
    if not translation_units:
        raise RuntimeError("compile database contains no first-party translation units")
    run(
        [clang_tidy, "--quiet", "-p", str(build), *map(str, translation_units)],
        cwd=root,
    )
    return len(translation_units)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--clang-format", default="clang-format")
    parser.add_argument("--clang-format-diff", default="clang-format-diff")
    parser.add_argument("--clang-tidy", default="clang-tidy")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    build = args.build.resolve()
    try:
        check_structure(root)
        check_format(root, args.clang_format_diff, args.clang_format)
        translation_units = check_tidy(root, build, args.clang_tidy)
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        return 1
    print(
        "source quality passed: changed lines formatted, structure ratchets held, "
        f"and clang-tidy analyzed {translation_units} translation units"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
