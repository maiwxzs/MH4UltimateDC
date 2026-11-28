#!/usr/bin/env python3
"""Community code mods for the game's code.bin, applied the way Azahar applies them.

Azahar loads `load/mods/<program id>/exefs/code.bin` (or `.../code.bin`) in place of the
game's code, then patches it with the first of `exefs/code.ips`, `exefs/code.bps`,
`code.ips`, `code.bps` that exists (src/core/file_sys/ncch_container.cpp, patch.cpp; the
program id of an update counts as the base game's). The port's code library is the game's
code recompiled, so it has to be built from the same modded code: a mod that changes the
code (.text) makes the running code differ from the recompiled one, and the port then
interprets everything. A mod that only changes data (.rodata/.data) needs no rebuild.

    python3 tools/code_mods.py AZAHAR_USER_DIR 0004000000126300 code.bin   # which mod applies
"""
from __future__ import annotations

import os
import sys
import zlib


class ModError(ValueError):
    pass


def apply_ips(code: bytes, patch: bytes) -> bytes:
    """IPS: "PATCH", records of (3-byte offset, 2-byte length, data) or, with length 0, an RLE
    run (2-byte count, 1 byte), then "EOF". Like Azahar, a record may not write past the end
    of the code, and anything after "EOF" is ignored."""
    if patch[:5] != b"PATCH":
        raise ModError("not an IPS patch")
    out = bytearray(code)
    cursor, end = 5, len(patch) - 3
    while cursor < end:
        if patch[cursor:cursor + 3] == b"EOF":
            break
        offset = int.from_bytes(patch[cursor:cursor + 3], "big")
        length = int.from_bytes(patch[cursor + 3:cursor + 5], "big")
        if length == 0:
            count = int.from_bytes(patch[cursor + 5:cursor + 7], "big")
            if offset + count > len(out):
                raise ModError(f"IPS run at {offset:#x} goes past the end of the code")
            out[offset:offset + count] = patch[cursor + 7:cursor + 8] * count
            cursor += 8
            continue
        if offset + length > len(out):
            raise ModError(f"IPS record at {offset:#x} goes past the end of the code")
        out[offset:offset + length] = patch[cursor + 5:cursor + 5 + length]
        cursor += 5 + length
    return bytes(out)


def _bps_number(patch: bytes, pos: int) -> tuple[int, int]:
    data, shift = 0, 1
    while pos < len(patch):
        x = patch[pos]
        pos += 1
        data += (x & 0x7F) * shift
        if x & 0x80:
            break
        shift <<= 7
        data += shift
    return data, pos


def apply_bps(code: bytes, patch: bytes) -> bytes:
    """BPS ("BPS1"), checked by its source and target CRC32s. As in Azahar, no metadata, and
    the result keeps the code's length when the target is shorter."""
    if patch[:4] != b"BPS1" or len(patch) < 16:
        raise ModError("not a BPS patch")
    source_size, pos = _bps_number(patch, 4)
    target_size, pos = _bps_number(patch, pos)
    metadata_size, pos = _bps_number(patch, pos)
    if source_size > len(code) or metadata_size != 0:
        raise ModError("BPS sizes don't fit this code (or the patch has metadata)")
    end = len(patch) - 12
    source_crc = int.from_bytes(patch[end:end + 4], "little")
    target_crc = int.from_bytes(patch[end + 4:end + 8], "little")
    if zlib.crc32(code[:source_size]) != source_crc:
        raise ModError("this BPS patch is for another code.bin (source CRC32 differs)")
    target = bytearray(max(len(code), target_size))
    out = source_rel = target_rel = 0
    while pos < end:
        data, pos = _bps_number(patch, pos)
        command, length = data & 3, (data >> 2) + 1
        if out + length > len(target):
            raise ModError("BPS patch writes past the end")
        if command == 0:      # SourceRead
            target[out:out + length] = code[out:out + length]
        elif command == 1:    # TargetRead
            target[out:out + length] = patch[pos:pos + length]
            pos += length
        elif command == 2:    # SourceCopy
            delta, pos = _bps_number(patch, pos)
            source_rel += -(delta >> 1) if delta & 1 else delta >> 1
            if source_rel < 0 or source_rel + length > len(code):
                raise ModError("BPS source copy out of range")
            target[out:out + length] = code[source_rel:source_rel + length]
            source_rel += length
        else:                 # TargetCopy, byte by byte (it may overlap what it writes)
            delta, pos = _bps_number(patch, pos)
            target_rel += -(delta >> 1) if delta & 1 else delta >> 1
            if target_rel < 0 or target_rel + length > len(target):
                raise ModError("BPS target copy out of range")
            for i in range(length):
                target[out + i] = target[target_rel]
                target_rel += 1
        out += length
    if zlib.crc32(bytes(target[:target_size])) != target_crc:
        raise ModError("BPS result doesn't match the patch's target CRC32")
    return bytes(target)


def mod_dir(user_dir: str, program_id: int) -> str:
    """Azahar's mod folder for a title: an update's mods are the base game's."""
    if program_id & 0x000000FF_00000000 == 0x0000000E_00000000:
        program_id &= ~0x0000000E_00000000
    return os.path.join(user_dir, "load", "mods", f"{program_id:016X}")


def find_mods(user_dir: str, program_id: int) -> tuple[str | None, str | None]:
    """(replacement code.bin, patch) that Azahar would use for this title, or None."""
    base = mod_dir(user_dir, program_id)
    replacement = next((p for p in (os.path.join(base, "exefs", "code.bin"), os.path.join(base, "code.bin"))
                        if os.path.isfile(p)), None)
    patch = next((p for p in (os.path.join(base, "exefs", "code.ips"), os.path.join(base, "exefs", "code.bps"),
                              os.path.join(base, "code.ips"), os.path.join(base, "code.bps"))
                  if os.path.isfile(p)), None)
    return replacement, patch


class RecompManifest:
    """A code mod's own map of the code it adds past the game's memory, for recompilers
    (`recomp.json` beside its exheader.bin, as Heavenfall writes it with --port):

        {"format": 1 or 2, "title": "0004000000126300", "version": "usa_update",
         "code_sha1": "<sha1 of exefs/code.bin>",
         "code": [["0x111e000", "0x1200000"]], "entries": [["0x111e000", "arm"], ...],
         "packs": [{"file": "romfs/packs/town.bin", "address": "0xc700000", "sha1": "...",
                    "entries": [["0xc700000", "thumb"], ...]}, ...]}   (format 2 only)

    `code`: ranges of the mod's code.bin past the game's memory that hold code; `packs`: files
    of its RomFS the game loads at a fixed address while a scene runs (the same bytes each
    time), code too. `entries`: where that code is entered (calls inside it are followed)."""

    def __init__(self, path: str, ranges: list[tuple[int, int]], entries: dict[int, str],
                 segments: list[tuple[int, bytes]], pack_files: list[str]):
        self.path, self.ranges, self.entries = path, ranges, entries
        self.segments, self.pack_files = segments, pack_files


def find_recomp_manifest(user_dir: str, program_id: int) -> str | None:
    """The recomp.json of this title's mod folder, or None."""
    path = os.path.join(mod_dir(user_dir, program_id), "recomp.json")
    return path if os.path.isfile(path) else None


def load_recomp_manifest(path: str, code: bytes, program_id: int) -> RecompManifest:
    """The manifest at `path`, checked against the code it was made for (`code`, the mods
    applied) and the title; its packs read from the mod's folder and checked too."""
    import hashlib
    import json
    base = os.path.dirname(path)
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    if data.get("format") not in (1, 2):
        raise ModError(f"{path}: a recomp.json of format {data.get('format')!r}, which this port doesn't know")
    title = int(str(data.get("title", "0")), 16)
    base_id = program_id & ~0x0000000E_00000000 if program_id & 0x000000FF_00000000 == 0x0000000E_00000000 \
        else program_id
    if title and title != base_id:
        raise ModError(f"{path}: made for title {title:016X}, not this game ({base_id:016X})")
    if data.get("code_sha1") and hashlib.sha1(code).hexdigest() != data["code_sha1"]:
        raise ModError(f"{path}: made for another code.bin than the mod's (its code_sha1 differs): build the mod "
                       "again, or remove the old recomp.json")

    def mode(m: str) -> str:
        if m not in ("arm", "thumb"):
            raise ModError(f"{path}: an entry's instruction set is {m!r}, not arm or thumb")
        return m

    ranges = [(int(s, 16), int(e, 16)) for s, e in data.get("code", [])]
    entries = {int(a, 16) & ~1: mode(m) for a, m in data.get("entries", [])}
    segments, files = [], []
    for pack in data.get("packs", []) if data["format"] == 2 else []:
        file = os.path.join(base, *pack["file"].replace("\\", "/").split("/"))
        if not os.path.isfile(file):
            raise ModError(f"{path}: its pack {pack['file']} isn't in the mod's folder")
        with open(file, "rb") as f:
            image = f.read()
        if pack.get("sha1") and hashlib.sha1(image).hexdigest() != pack["sha1"]:
            raise ModError(f"{path}: its pack {pack['file']} isn't the one it was made with (sha1 differs)")
        segments.append((int(pack["address"], 16), image))
        entries.update({int(a, 16) & ~1: mode(m) for a, m in pack.get("entries", [])})
        files.append(file)
    return RecompManifest(path, ranges, entries, segments, files)


def apply_mods(code: bytes, user_dir: str, program_id: int) -> tuple[bytes, list[str]]:
    """The code as Azahar will run it with this user folder's mods, and the files used."""
    replacement, patch = find_mods(user_dir, program_id)
    used = []
    if replacement:
        with open(replacement, "rb") as f:
            code = f.read()
        used.append(replacement)
    if patch:
        with open(patch, "rb") as f:
            data = f.read()
        code = apply_ips(code, data) if patch.endswith(".ips") else apply_bps(code, data)
        used.append(patch)
    return code, used


def changed_bytes(old: bytes, new: bytes, start: int, size: int) -> int:
    """How many bytes of [start, start + size) differ (bytes past either end count)."""
    a, b = old[start:start + size], new[start:start + size]
    return sum(x != y for x, y in zip(a, b)) + abs(len(a) - len(b))


def describe(old: bytes, new: bytes, segments: list[tuple[str, int, int]]) -> list[str]:
    """Per segment (name, offset in the code, size): how many bytes the mods change."""
    return [f"{name}: {changed_bytes(old, new, offset, size)} bytes changed" for name, offset, size in segments]


def main(argv=None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if len(args) != 3:
        print("usage: code_mods.py AZAHAR_USER_DIR PROGRAM_ID_HEX code.bin", file=sys.stderr)
        return 1
    with open(args[2], "rb") as f:
        code = f.read()
    new, used = apply_mods(code, args[0], int(args[1], 16))
    if not used:
        print(f"no code mods in {mod_dir(args[0], int(args[1], 16))}")
        return 0
    print("\n".join(used))
    print(f"{changed_bytes(code, new, 0, max(len(code), len(new)))} bytes changed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
