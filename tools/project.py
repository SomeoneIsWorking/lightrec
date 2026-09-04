#!/usr/bin/env python3
"""Canonical CMake build and verification operations for maintained Lightrec."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD = ROOT / "build" / "ci"


def run(command: list[str]) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True, env=os.environ.copy())


def configure(build: Path) -> None:
    run(
        [
            "cmake",
            "-S",
            str(ROOT),
            "-B",
            str(build),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DCMAKE_C_COMPILER=clang",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DBUILD_SHARED_LIBS=OFF",
            "-DBUILD_TESTING=ON",
            "-DLIGHTREC_MAINTAINER_CHECKS=ON",
        ]
    )


def build_project(build: Path) -> None:
    run(["cmake", "--build", str(build)])


def test_project(build: Path) -> None:
    run(["ctest", "--test-dir", str(build), "--output-on-failure"])
