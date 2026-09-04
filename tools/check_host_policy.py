#!/usr/bin/env python3
"""Exercise the authoritative CMake host classifier with positive and refusals."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

CASES = (
    ("Linux", "x86_64", "proven", "runtime contract test"),
    ("Windows", "AMD64", "refused", "no maintained runtime-JIT proof"),
    ("Darwin", "arm64", "refused", "MAP_JIT"),
    ("Android", "arm64-v8a", "refused", "x18"),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[1]
    )
    parser.add_argument("--cmake", default="cmake")
    return parser.parse_args()


def probe(cmake: str, module: Path, system: str, processor: str) -> str:
    script = (
        f'include("{module.as_posix()}")\n'
        f'lightrec_classify_host("{system}" "{processor}" class reason)\n'
        'message("RESULT=${class}|${reason}")\n'
    )
    build_dir = module.parents[1] / "build" / "host-policy-test"
    build_dir.mkdir(parents=True, exist_ok=True)
    probe_path = build_dir / "probe.cmake"
    probe_path.write_text(script, encoding="utf-8")
    result = subprocess.run(
        [cmake, "-P", str(probe_path)],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout + result.stderr


def main() -> int:
    args = parse_args()
    module = args.root.resolve() / "cmake" / "LightrecHost.cmake"
    for system, processor, expected_class, expected_reason in CASES:
        output = probe(args.cmake, module, system, processor)
        expected = f"RESULT={expected_class}|"
        if expected not in output or expected_reason not in output:
            print(f"host policy mismatch for {system}/{processor}: {output.strip()}")
            return 1
    print("host policy passed: 1 proven target and 3 explicit platform refusals")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
