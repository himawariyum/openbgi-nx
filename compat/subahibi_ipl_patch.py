#!/usr/bin/env python3
"""Patch the startup FPS setting in Wonderful Everyday / SubaHibi HD's ipl._bp.

The HD game's startup bytecode explicitly executes:

    push_i8 0
    gfx0.set_fps

OpenBGI profiles >= 1.640 reject an FPS value of zero. This utility changes
only that immediate value (default: 60). It does NOT implement the separate
OpenBGI VM opcode 0x910A; that requires a change to OpenBGI itself.

No third-party packages are required.

Examples:
    # Safely create ipl_patched._bp beside the source file:
    python subahibi_ipl_patch.py "C:/Games/SubaHibi/ipl._bp"

    # Choose a different FPS value:
    python subahibi_ipl_patch.py ipl._bp --fps 60 --output ipl_60fps._bp

    # Patch in place, preserving ipl._bp.bak first:
    python subahibi_ipl_patch.py ipl._bp --in-place

The patcher validates the file signature, embedded game identifier, header,
and exact instruction sequence before modifying anything. It refuses to
silently patch a different or unexpected bytecode layout.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Union

PathLike = Union[str, os.PathLike[str]]

# This ipl._bp format has a 16-byte header followed by its code area.
HEADER_SIZE = 0x10
EXPECTED_HEADER_SIZE_FIELD = 0x10

# Offsets are relative to the start of the code area, as shown by bpasm.
FPS_CALL_CODE_OFFSET = 0x142
EXPECTED_INSTRUCTION = bytes((0x00, 0x00, 0x90, 0x02))
# push_i8 0; gfx1.0x00 follows immediately in the supplied HD boot program.
EXPECTED_FOLLOWING_INSTRUCTION = bytes((0x00, 0x00, 0x91, 0x00))
GAME_IDENTIFIER = b"SubarashikihibiHD"


class PatchError(Exception):
    """Raised when the input is not the expected SubaHibi HD boot program."""


@dataclass(frozen=True)
class PatchResult:
    source: Path
    destination: Path
    old_fps: int
    new_fps: int
    changed: bool
    source_sha256: str
    output_sha256: str


def _validate_fps(fps: int) -> None:
    # The immediate is a signed 8-bit value in this instruction encoding.
    # Restrict to the positive signed-byte range to avoid ambiguous encodings.
    if not isinstance(fps, int) or isinstance(fps, bool):
        raise PatchError("FPS must be an integer.")
    if not 1 <= fps <= 127:
        raise PatchError("FPS must be between 1 and 127 for this push_i8 patch.")


def patch_bytes(data: bytes, fps: int = 60) -> tuple[bytes, int, bool]:
    """Return (patched_bytes, old_fps, changed) after validating the input."""
    _validate_fps(fps)

    if len(data) < HEADER_SIZE:
        raise PatchError("File is too short to contain a BGI bytecode header.")

    header_size = int.from_bytes(data[0:4], "little")
    code_size = int.from_bytes(data[4:8], "little")
    if header_size != EXPECTED_HEADER_SIZE_FIELD:
        raise PatchError(
            f"Unexpected header-size field {header_size:#x}; expected 0x10."
        )
    if code_size <= FPS_CALL_CODE_OFFSET + len(EXPECTED_INSTRUCTION):
        raise PatchError(
            f"Declared code area ({code_size:#x} bytes) is too small for the FPS call."
        )
    if len(data) < HEADER_SIZE + code_size:
        raise PatchError(
            "File is truncated: declared code area extends beyond end of file."
        )
    if GAME_IDENTIFIER not in data:
        raise PatchError(
            "The embedded 'SubarashikihibiHD' identifier was not found; "
            "this does not look like the expected HD ipl._bp."
        )

    absolute = HEADER_SIZE + FPS_CALL_CODE_OFFSET
    actual = data[absolute:absolute + len(EXPECTED_INSTRUCTION)]
    if len(actual) != len(EXPECTED_INSTRUCTION):
        raise PatchError("Could not read the expected FPS instruction.")

    # The first byte is push_i8 (0x00), the second is its immediate, and
    # 90 02 is gfx0.set_fps. Verify opcode bytes independently of the operand.
    if actual[0] != 0x00 or actual[2:4] != bytes((0x90, 0x02)):
        raise PatchError(
            f"Unexpected instruction at code offset {FPS_CALL_CODE_OFFSET:#x}: "
            f"{actual.hex(' ')}; expected '00 <fps> 90 02'."
        )

    following = data[
        absolute + len(EXPECTED_INSTRUCTION):
        absolute + len(EXPECTED_INSTRUCTION) + len(EXPECTED_FOLLOWING_INSTRUCTION)
    ]
    if following != EXPECTED_FOLLOWING_INSTRUCTION:
        raise PatchError(
            "The instruction following set_fps does not match the known "
            f"SubaHibi HD layout: {following.hex(' ')}. Refusing to patch."
        )

    old_fps = actual[1]
    if old_fps != 0 and old_fps != fps:
        raise PatchError(
            f"The startup FPS immediate is already {old_fps}, not zero. "
            "Refusing to overwrite an unknown/custom patch."
        )

    if old_fps == fps:
        return data, old_fps, False

    patched = bytearray(data)
    patched[absolute + 1] = fps
    return bytes(patched), old_fps, True


def _atomic_write(destination: Path, content: bytes) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(
        prefix=destination.name + ".", suffix=".tmp", dir=str(destination.parent)
    )
    temp_path = Path(temp_name)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_path, destination)
    except Exception:
        try:
            temp_path.unlink(missing_ok=True)
        finally:
            raise


def patch_file(
    source: PathLike,
    *,
    fps: int = 60,
    output: Optional[PathLike] = None,
    in_place: bool = False,
    overwrite_backup: bool = False,
) -> PatchResult:
    """Patch an ipl._bp file.

    By default, writes ``ipl_patched._bp`` alongside the input. Use
    ``in_place=True`` to replace the source after creating ``ipl._bp.bak``.
    ``output`` and ``in_place`` are mutually exclusive.
    """
    src = Path(source).expanduser().resolve()
    if not src.is_file():
        raise PatchError(f"Input file does not exist or is not a file: {src}")
    if output is not None and in_place:
        raise PatchError("Use either output=... or in_place=True, not both.")

    original = src.read_bytes()
    patched, old_fps, changed = patch_bytes(original, fps=fps)

    if in_place:
        destination = src
        backup = src.with_name(src.name + ".bak")
        if changed:
            if backup.exists() and not overwrite_backup:
                raise PatchError(
                    f"Backup already exists: {backup}. Move it, delete it, or "
                    "pass --overwrite-backup to replace it."
                )
            shutil.copy2(src, backup)
            _atomic_write(destination, patched)
    else:
        if output is None:
            destination = src.with_name(f"{src.stem}_patched{src.suffix}")
        else:
            destination = Path(output).expanduser().resolve()
        if destination == src:
            raise PatchError(
                "Output resolves to the input file. Use --in-place to patch "
                "the source safely with a backup."
            )
        if destination.exists() and changed:
            raise PatchError(
                f"Output already exists: {destination}. Choose another path."
            )
        if changed:
            _atomic_write(destination, patched)
        elif not destination.exists():
            # Preserve a standalone output artifact for idempotent invocations.
            _atomic_write(destination, patched)

    final_bytes = destination.read_bytes()
    return PatchResult(
        source=src,
        destination=destination,
        old_fps=old_fps,
        new_fps=fps,
        changed=changed,
        source_sha256=hashlib.sha256(original).hexdigest(),
        output_sha256=hashlib.sha256(final_bytes).hexdigest(),
    )


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Patch the SubaHibi HD ipl._bp startup FPS value."
    )
    parser.add_argument("input", help="Path to the game's ipl._bp")
    parser.add_argument(
        "--fps", type=int, default=60,
        help="positive FPS value (1..127); default: 60",
    )
    destination = parser.add_mutually_exclusive_group()
    destination.add_argument(
        "--output", "-o", help="output path (default: ipl_patched._bp)"
    )
    destination.add_argument(
        "--in-place", action="store_true",
        help="replace input after creating input-name.bak",
    )
    parser.add_argument(
        "--overwrite-backup", action="store_true",
        help="allow replacing an existing .bak when using --in-place",
    )
    args = parser.parse_args(argv)

    try:
        result = patch_file(
            args.input,
            fps=args.fps,
            output=args.output,
            in_place=args.in_place,
            overwrite_backup=args.overwrite_backup,
        )
    except (PatchError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    status = "already patched" if not result.changed else "patched"
    print(f"{status}: {result.destination}")
    print(f"FPS: {result.old_fps} -> {result.new_fps}")
    print(f"SHA-256: {result.output_sha256}")
    if args.in_place and result.changed:
        print(f"Backup: {result.source}.bak")
    print(
        "NOTE: This changes only the startup FPS value."
        "Needs openbgi-nx a0bc12a8 or later to work."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
