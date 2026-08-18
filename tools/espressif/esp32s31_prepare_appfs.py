#!/usr/bin/env python3
"""Prepare the ESP32-S31 E.0 ROMFS seed and negative-test corpus."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
from pathlib import Path


def write_hash(path: Path) -> None:
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    path.with_name(path.name + ".sha256").write_text(digest + "\n")


def first_load_header(data: bytearray) -> int:
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise ValueError("sv32test is not an ELF32 image")

    phoff = struct.unpack_from("<I", data, 28)[0]
    phentsize = struct.unpack_from("<H", data, 42)[0]
    phnum = struct.unpack_from("<H", data, 44)[0]
    if phentsize != 32 or phoff + phentsize * phnum > len(data):
        raise ValueError("sv32test has an invalid program-header table")

    for index in range(phnum):
        offset = phoff + index * phentsize
        if struct.unpack_from("<I", data, offset)[0] == 1:  # PT_LOAD
            return offset

    raise ValueError("sv32test has no PT_LOAD segment")


def prepare(source_dir: Path, output_dir: Path, max_size: int,
            e0_corpus: bool) -> None:
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)

    regular_files = [path for path in source_dir.iterdir() if path.is_file()]
    if not regular_files:
        raise ValueError(f"no loadable applications found in {source_dir}")

    for source in regular_files:
        target = output_dir / source.name
        shutil.copyfile(source, target)
        write_hash(target)

    if not e0_corpus:
        return

    source = output_dir / "sv32test"
    if not source.exists():
        raise ValueError("sv32test is required for the E.0 validation corpus")

    original = bytearray(source.read_bytes())

    # A distinct but semantically identical replacement.  Bytes appended past
    # the ELF tables and loadable segments are ignored by the loader.

    replacement = output_dir / "sv32test.v2"
    replacement.write_bytes(original + b"S31-E0-V2\n")
    write_hash(replacement)

    truncated = output_dir / "sv32test.truncated"
    truncated.write_bytes(original[:128])
    write_hash(truncated)

    bad_elf = bytearray(original)
    bad_elf[1] ^= 0x20
    bad_elf_path = output_dir / "sv32test.badelf"
    bad_elf_path.write_bytes(bad_elf)
    write_hash(bad_elf_path)

    bad_hash = output_dir / "sv32test.badhash"
    bad_hash.write_bytes(original + b"corrupt")
    bad_hash.with_name(bad_hash.name + ".sha256").write_text(
        hashlib.sha256(original).hexdigest() + "\n"
    )

    bad_segment = bytearray(original)
    phdr = first_load_header(bad_segment)
    struct.pack_into("<I", bad_segment, phdr + 8, 0x70000000)  # p_vaddr
    struct.pack_into("<I", bad_segment, phdr + 12, 0x70000000)  # p_paddr
    bad_segment_path = output_dir / "sv32test.badsegment"
    bad_segment_path.write_bytes(bad_segment)
    write_hash(bad_segment_path)

    oversize = output_dir / "sv32test.oversize"
    if len(original) > max_size:
        raise ValueError("configured ELF size limit is below sv32test size")
    oversize.write_bytes(original + bytes(max_size + 1 - len(original)))
    write_hash(oversize)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-size", type=int, required=True)
    parser.add_argument("--e0-corpus", action="store_true")
    args = parser.parse_args()
    prepare(args.source, args.output, args.max_size, args.e0_corpus)


if __name__ == "__main__":
    main()
