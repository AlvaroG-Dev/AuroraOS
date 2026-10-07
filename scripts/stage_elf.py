#!/usr/bin/env python3
"""Stage an ELF executable and its shared-library closure into AuroraOS sysroot."""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path


NEEDED_RE = re.compile(r"Shared library: \[(.+?)\]")
INTERP_RE = re.compile(r"Requesting program interpreter: (.+?)\]")


def readelf(*args: str) -> str:
    return subprocess.check_output(["readelf", *args], text=True, stderr=subprocess.STDOUT)


def needed(path: Path) -> list[str]:
    try:
        out = readelf("-d", str(path))
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(f"readelf failed for {path}: {exc.output}") from exc
    return NEEDED_RE.findall(out)


def interpreter(path: Path) -> str | None:
    try:
        out = readelf("-l", str(path))
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(f"readelf failed for {path}: {exc.output}") from exc
    for line in out.splitlines():
        if "Requesting program interpreter:" in line:
            return line.split("Requesting program interpreter:", 1)[1].strip()
    return None


def find_library(name: str, pool: Path) -> Path | None:
    direct = pool / name
    if direct.is_file():
        return direct
    matches = sorted(p for p in pool.rglob(name) if p.is_file())
    return matches[0] if matches else None


def destination_for(source: Path, pool: Path) -> Path:
    # lib/foo.so -> /lib/foo.so
    # lib/x86_64-linux-gnu/foo.so -> /lib/x86_64-linux-gnu/foo.so
    return Path("/lib") / source.relative_to(pool)


def install_library(source: Path, sysroot: Path, pool: Path) -> Path:
    destination = destination_for(source, pool)
    target = sysroot / destination.relative_to("/")
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        return destination
    shutil.copy2(source, target)
    print(f"[stage] library: {source} -> {destination}")
    return destination


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(f"usage: {sys.argv[0]} <binary> <sysroot> [destination-dir]", file=sys.stderr)
        return 2

    binary = Path(sys.argv[1]).resolve()
    sysroot = Path(sys.argv[2]).resolve()
    destdir = Path(sys.argv[3] if len(sys.argv) == 4 else "/bin")

    if not binary.is_file():
        print(f"stage_elf: missing binary: {binary}", file=sys.stderr)
        return 1
    if not destdir.is_absolute():
        print("stage_elf: destination-dir must be absolute", file=sys.stderr)
        return 2

    sysroot_dest = sysroot / destdir.relative_to("/")
    sysroot_dest.mkdir(parents=True, exist_ok=True)
    target = sysroot_dest / binary.name
    if not target.exists():
        shutil.copy2(binary, target)
        print(f"[stage] executable: {binary} -> {destdir / binary.name}")
    else:
        print(f"[stage] executable: {destdir / binary.name} already staged; keeping existing file")

    try:
        readelf("-h", str(binary))
    except subprocess.CalledProcessError:
        print(f"[stage] {binary.name}: non-ELF executable, no library resolution required")
        return 0

    repo_root = Path(__file__).resolve().parents[1]
    pool = repo_root / "lib"
    if not pool.is_dir():
        print("[stage] library pool: lib/ does not exist; skipping shared libraries")
        return 0

    interp = interpreter(binary)
    deps = list(needed(binary))
    if not interp and not deps:
        print(f"[stage] {binary.name}: static ELF, no shared libraries required")
        return 0

    queue = []
    if interp:
        queue.append(Path(interp).name)
        print(f"[stage] interpreter: {interp}")
    queue.extend(deps)

    seen: set[str] = set()
    while queue:
        name = queue.pop(0)
        if name in seen:
            continue
        seen.add(name)

        source = find_library(name, pool)
        if source is None:
            raise RuntimeError(
                f"missing shared library {name!r} in AuroraOS lib/ pool "
                f"(required by {binary.name})"
            )

        install_library(source, sysroot, pool)
        for child in needed(source):
            if child not in seen:
                queue.append(child)

    print(f"[stage] {binary.name}: resolved {len(seen)} shared libraries")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"stage_elf: ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
