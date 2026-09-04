#!/usr/bin/env python3
"""Check that gameplay dispatch cannot silently choose broad interpretation."""

from __future__ import annotations

import argparse
from pathlib import Path

REMOVED_BACKGROUND_FILES = (
    "recompiler.c",
    "recompiler.h",
    "reaper.c",
    "reaper.h",
    "slist.h",
)
REMOVED_MODES = ("ENABLE_FIRST_PASS", "ENABLE_THREADED_COMPILER")


def contract_violations(
    file_names: set[str], build_text: str, config_text: str, runtime: str
) -> list[str]:
    findings: list[str] = []
    for name in REMOVED_BACKGROUND_FILES:
        if name in file_names:
            findings.append(f"obsolete background-interpretation path exists: {name}")

    for mode in REMOVED_MODES:
        if mode in build_text or mode in config_text:
            findings.append(f"obsolete product mode remains configurable: {mode}")

    required_markers = (
        "lightrec_fallback_block",
        "LIGHTREC_FALLBACK_JIT_COMPILE_FAILURE",
        "LIGHTREC_FALLBACK_UNSAFE_FETCH",
        "lightrec_execution_is_dynarec_dominated",
        "lightrec_run_block_boundary",
        "LIGHTREC_EXIT_BLOCK_BOUNDARY",
        "translated_blocks",
        "cache_misses",
    )
    for marker in required_markers:
        if marker not in runtime:
            findings.append(f"missing measured fallback contract marker: {marker}")

    direct_calls = runtime.count("lightrec_emulate_block(state, block, pc)")
    if direct_calls != 2:
        findings.append(
            "lightrec.c must have exactly the measured fallback wrapper and "
            f"diagnostic-only direct call; found {direct_calls}"
        )

    return findings


def find_violations(root: Path) -> list[str]:
    return contract_violations(
        {path.name for path in root.iterdir() if path.is_file()},
        (root / "CMakeLists.txt").read_text(encoding="utf-8"),
        (root / "lightrec-config.h.cmakein").read_text(encoding="utf-8"),
        "\n".join(
            (root / source).read_text(encoding="utf-8")
            for source in ("lightrec.c", "execution.c")
        ),
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    parser.add_argument("--selftest", action="store_true")
    return parser.parse_args()


def selftest(root: Path) -> int:
    findings = find_violations(root)
    if findings:
        raise AssertionError(f"shipping tree must be the positive fixture: {findings}")

    negative = contract_violations(
        {"recompiler.c"},
        "option(ENABLE_FIRST_PASS)",
        "",
        "void unrelated(void);",
    )
    if len(negative) < 6:
        raise AssertionError(
            f"negative fixture did not exercise every rule: {negative}"
        )
    print(
        "runtime contract selftest passed: shipping tree accepted and "
        f"negative fixture produced {len(negative)} findings"
    )
    return 0


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    if args.selftest:
        return selftest(root)

    findings = find_violations(root)
    if findings:
        for finding in findings:
            print(finding)
        return 1
    print("runtime contract passed: synchronous JIT default with measured fallback")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
