"""A code mod's recomp.json (Heavenfall's, built with --port): the code it adds past the game's
memory, in its code.bin (an extension) and in files it loads at a fixed address (packs), read by
tools/code_mods.py and recompiled with the game's (tools/recomp/analysis.Image)."""
from __future__ import annotations

import hashlib
import json
import os
import struct
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import code_mods  # noqa: E402
import mods  # noqa: E402
from recomp.analysis import Analyzer, Image  # noqa: E402

BX_LR = 0xE12FFF1E
MOV_R0_1 = 0xE3A00001
TITLE = 0x0004000000126300


def words(*ws: int) -> bytes:
    return b"".join(struct.pack("<I", w) for w in ws)


def test_image_reads_and_counts_the_mods_code():
    base = 0x100000
    code = words(BX_LR) + bytes(0x100) + words(MOV_R0_1, BX_LR)  # the extension at base + 0x104
    pack = words(MOV_R0_1, BX_LR)
    img = Image(code, base, 4, code_ranges=[(base + 0x104, base + 0x10C)], segments=[(0xC700000, pack)])
    assert img.in_text(base) and not img.in_text(base + 0x40)
    assert img.in_text(base + 0x104) and img.in_text(0xC700004) and not img.in_text(0xC700008)
    assert img.read32(0xC700000) == MOV_R0_1 and img.read32(0xC700008) is None
    funcs = Analyzer(img).analyze_all({base: "arm", base + 0x104: "arm", 0xC700000: "arm"})
    assert set(funcs) == {base, base + 0x104, 0xC700000}


def write_mod(tmp_path, code: bytes, manifest: dict, packs: dict[str, bytes]):
    mod = tmp_path / "load" / "mods" / f"{TITLE:016X}"
    (mod / "exefs").mkdir(parents=True)
    (mod / "exefs" / "code.bin").write_bytes(code)
    for name, data in packs.items():
        (mod / name).parent.mkdir(parents=True, exist_ok=True)
        (mod / name).write_bytes(data)
    (mod / "recomp.json").write_text(json.dumps(manifest))
    return mod


def test_manifest_is_read_and_checked(tmp_path):
    code = words(BX_LR) * 8
    pack = words(MOV_R0_1, BX_LR)
    manifest = {"format": 2, "title": f"{TITLE:016X}", "version": "usa_update",
                "code_sha1": hashlib.sha1(code).hexdigest(), "code": [["0x111e000", "0x1120000"]],
                "entries": [["0x111e000", "arm"], ["0x111e011", "thumb"]],
                "packs": [{"file": "romfs/packs/town.bin", "address": "0xc700000",
                           "sha1": hashlib.sha1(pack).hexdigest(), "entries": [["0xc700000", "arm"]]}]}
    mod = write_mod(tmp_path, code, manifest, {"romfs/packs/town.bin": pack})
    path = code_mods.find_recomp_manifest(str(tmp_path), TITLE)
    assert path == str(mod / "recomp.json")
    m = code_mods.load_recomp_manifest(path, code, TITLE)
    assert m.ranges == [(0x111E000, 0x1120000)]
    assert m.entries == {0x111E000: "arm", 0x111E010: "thumb", 0xC700000: "arm"}
    assert m.segments == [(0xC700000, pack)]
    # the port's mod library counts the map and its pack among the code's inputs
    inputs = mods.code_inputs(str(tmp_path))
    assert [mods._code_kind(p) for p in inputs] == ["code.bin", "recomp", "pack"]
    # made for another code.bin, another game, or with a changed pack: refused
    with pytest.raises(code_mods.ModError):
        code_mods.load_recomp_manifest(path, code + b"\0", TITLE)
    with pytest.raises(code_mods.ModError):
        code_mods.load_recomp_manifest(path, code, 0x0004000000126100)
    (mod / "romfs" / "packs" / "town.bin").write_bytes(pack + b"\0")
    with pytest.raises(code_mods.ModError):
        code_mods.load_recomp_manifest(path, code, TITLE)


def test_mod_library_keeps_the_map(tmp_path):
    """mods.py deploys a mod's recomp.json to the mods folder with its code (Azahar ignores it)."""
    mod = tmp_path / "Heavenfall"
    (mod / "exefs").mkdir(parents=True)
    (mod / "exefs" / "code.bin").write_bytes(words(BX_LR))
    (mod / "exheader.bin").write_bytes(bytes(0x800))
    (mod / "recomp.json").write_text("{}")
    (mod / "mod.json").write_text(json.dumps({"name": "Heavenfall", "region": "USA",
                                              "contains": ["code", "exheader", "recomp"]}))
    m = mods.scan(str(mod), "heavenfall")
    assert "recomp" in m.contains and not m.errors
    assert any(f.dest == "recomp.json" for f in m.files)
