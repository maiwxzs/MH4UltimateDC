"""tools/heavenfall.py: the player's choices of Heavenfall's parts, as its command line takes them."""
from __future__ import annotations

import json
import os
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import heavenfall  # noqa: E402


def test_defaults_and_cheats_off(tmp_path):
    values = heavenfall.load_settings(str(tmp_path))
    args = heavenfall.options(values)
    assert "--single-screen" in args and "--lock-on" in args and "--no-layered-equipment" in args
    assert args[args.index("--hud-scale") + 1] == "100"
    # the port does the frame rate itself: Heavenfall's cheats always off
    assert args[-3:] == ["--no-cheats-enabled", "--cheat-fps", "off"]


def test_choices_saved_and_checked(tmp_path):
    values = {**heavenfall.load_settings(str(tmp_path)), **heavenfall.parse_set(["hud_scale=70", "single_screen=off"])}
    heavenfall.save_settings(str(tmp_path), values)
    again = heavenfall.load_settings(str(tmp_path))
    assert again["hud_scale"] == "70" and again["single_screen"] is False
    with pytest.raises(heavenfall.Error):
        heavenfall.parse_set(["hud_scale=55"])
    with pytest.raises(heavenfall.Error):
        heavenfall.parse_set(["fps=60"])
    # a damaged or foreign value in the file: the default
    (tmp_path / heavenfall.HOME / "settings.json").write_text(json.dumps({"hud_scale": "12", "jukebox": "yes"}))
    assert heavenfall.load_settings(str(tmp_path))["hud_scale"] == "100"


def test_status_without_install(tmp_path, capsys):
    assert heavenfall.main(["--user-dir", str(tmp_path), "--json", "status"]) == 0
    out = json.loads(capsys.readouterr().out.strip().splitlines()[-1])
    assert out["ok"] and not out["installed"]
