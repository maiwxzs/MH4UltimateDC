#!/usr/bin/env python3
"""Heavenfall Beta for the port: made from the player's own game, added to the mod library.

Heavenfall Beta (popzol and BlitzWOLF, GPL-3.0, https://github.com/popzol/4u-heavenfall-mod-beta)
is a mod for MH4U built on the player's machine from their own game. Its --port mode makes it
for a recompiled game: its code.bin, its exheader and recomp.json, the map of the code it adds
past the game's memory, which tools/build_game.py recompiles with the game's (code_mods.py).

    python3 tools/heavenfall.py status [--json]
    python3 tools/heavenfall.py install --rom GAME.3ds --update UPDATE.cia [--set KEY=VALUE ...] [--json]
    python3 tools/heavenfall.py remove [--json]

install: downloads Heavenfall's source at the version below (once), installs the packages its
command line needs (zstandard, numpy) beside it, runs it with --port and the player's choices, and adds
the result to the port's mod library as "Heavenfall" (tools/mods.py add --replace), on. The code
library must be built again afterwards (the launcher offers it). The choices are kept in
<user dir>/mh4u-heavenfall/settings.json; --set changes them (keys: SETTINGS below).
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mods  # noqa: E402

# The Heavenfall Beta this port is made for (its --port mode and recomp.json format 2)
VERSION = "182e89cdbc6b8f8da5acc26366989f96f8ec5dcb"
SOURCE_URL = f"https://github.com/popzol/4u-heavenfall-mod-beta/archive/{VERSION}.zip"
# what its command line imports beyond the standard library (its requirements.txt's versions;
# numpy for the interface's pictures and the music, which the single screen and the jukebox build)
PACKAGES = ["zstandard==0.25.0", "numpy==2.5.0"]
NAME = "Heavenfall"               # its name in the mod library
HOME = "mh4u-heavenfall"          # in Azahar's user folder: its source, packages, output, settings

# The player's choices: key -> (kind, default, Heavenfall's option). "bool" options go as
# --option / --no-option; "choice" ones as --option VALUE.
SETTINGS = {
    "single_screen": ("bool", True, "--single-screen"),
    "lock_on": ("bool", True, "--lock-on"),
    "hud_scale": ("choice", "100", "--hud-scale"),
    "damage_numbers": ("bool", False, "--damage-numbers"),
    "quality_of_life": ("bool", True, "--quality-of-life"),
    "hold_a": ("bool", False, "--hold-a"),
    "fast_text": ("bool", False, "--fast-text"),
    "box_equipment": ("bool", True, "--box-equipment"),
    "layered_equipment": ("bool", False, "--layered-equipment"),
    "rare_exchange": ("bool", False, "--rare-exchange"),
    "jukebox": ("bool", False, "--jukebox"),
    "skip_intro": ("bool", False, "--skip-intro"),
}
CHOICES = {"hud_scale": ("100", "90", "80", "70", "60")}
# Always: the port does the frame rate, the field of view and the picture's effects itself, so
# Heavenfall's cheats stay off (its frame-rate cheat would hold the game at 60 at most)
FIXED = ["--no-cheats-enabled", "--cheat-fps", "off"]


class Error(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def home(user_dir: str) -> str:
    return os.path.join(user_dir, HOME)


def say(text: str) -> None:
    print(text, flush=True)


def load_settings(user_dir: str) -> dict:
    values = {key: default for key, (_, default, _) in SETTINGS.items()}
    try:
        with open(os.path.join(home(user_dir), "settings.json"), encoding="utf-8") as f:
            saved = json.load(f)
        if isinstance(saved, dict):
            values.update({k: v for k, v in saved.items() if k in SETTINGS and valid(k, v)})
    except (OSError, ValueError):
        pass
    return values


def valid(key: str, value) -> bool:
    kind = SETTINGS[key][0]
    return isinstance(value, bool) if kind == "bool" else value in CHOICES[key]


def parse_set(items: list[str]) -> dict:
    out = {}
    for item in items:
        key, _, value = item.partition("=")
        if key not in SETTINGS:
            raise Error("usage", f"--set {item}: {key} isn't one of {', '.join(SETTINGS)}")
        if SETTINGS[key][0] == "bool":
            if value.lower() not in ("on", "off", "true", "false", "1", "0", "yes", "no"):
                raise Error("usage", f"--set {item}: on or off")
            out[key] = value.lower() in ("on", "true", "1", "yes")
        else:
            if value not in CHOICES[key]:
                raise Error("usage", f"--set {item}: one of {', '.join(CHOICES[key])}")
            out[key] = value
    return out


def save_settings(user_dir: str, values: dict) -> None:
    os.makedirs(home(user_dir), exist_ok=True)
    with open(os.path.join(home(user_dir), "settings.json"), "w", encoding="utf-8") as f:
        json.dump(values, f, indent=1)


def options(values: dict) -> list[str]:
    args = []
    for key, (kind, _, flag) in SETTINGS.items():
        if kind == "bool":
            args.append(flag if values[key] else "--no-" + flag[2:])
        else:
            args += [flag, str(values[key])]
    return args + FIXED


def fetch_source(user_dir: str) -> str:
    """Heavenfall's source at VERSION, downloaded and unpacked once."""
    root = os.path.join(home(user_dir), f"source-{VERSION[:12]}")
    if os.path.isfile(os.path.join(root, "core", "__main__.py")):
        return root
    os.makedirs(home(user_dir), exist_ok=True)
    archive = os.path.join(home(user_dir), f"heavenfall-{VERSION[:12]}.zip")
    if not os.path.isfile(archive):
        say(f"downloading Heavenfall Beta ({VERSION[:12]})")
        part = archive + ".part"
        try:
            with urllib.request.urlopen(SOURCE_URL, timeout=60) as r, open(part, "wb") as f:
                shutil.copyfileobj(r, f)
        except OSError as e:
            raise Error("download", f"Heavenfall's source couldn't be downloaded ({e}): {SOURCE_URL}")
        os.replace(part, archive)
    say("unpacking it")
    tmp = root + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(tmp)
    inner = [d for d in os.listdir(tmp) if os.path.isdir(os.path.join(tmp, d))]
    if len(inner) != 1 or not os.path.isfile(os.path.join(tmp, inner[0], "core", "__main__.py")):
        raise Error("download", "the download isn't Heavenfall's source; delete " + archive + " and try again")
    shutil.rmtree(root, ignore_errors=True)
    os.replace(os.path.join(tmp, inner[0]), root)
    shutil.rmtree(tmp, ignore_errors=True)
    return root


def ensure_packages(user_dir: str) -> str:
    """The packages Heavenfall's command line needs, in a folder of their own (not this Python's)."""
    target = os.path.join(home(user_dir), "packages")
    marker = os.path.join(target, ".installed")
    want = "\n".join(PACKAGES)
    try:
        with open(marker, encoding="utf-8") as f:
            if f.read() == want:
                return target
    except OSError:
        pass
    say("installing the packages it needs (" + ", ".join(PACKAGES) + ")")
    base = [sys.executable, "-m", "pip", "install", "--disable-pip-version-check", "--no-input", "--target", target]
    # Heavenfall's own versions first; if this Python has no ready-made copy of them (a newer or
    # older Python than theirs), any ready-made version: none is built from source, which would need
    # a compiler the player doesn't have
    unpinned = [p.split("==")[0] for p in PACKAGES]
    tail = ""
    for packages in (PACKAGES, unpinned):
        shutil.rmtree(target, ignore_errors=True)
        p = subprocess.run(base + ["--only-binary=:all:", *packages], capture_output=True, text=True)
        print(p.stdout, end="", flush=True)
        print(p.stderr, end="", flush=True)
        if p.returncode == 0:
            break
        tail = (p.stderr or p.stdout).strip().splitlines()[-1:] or [""]
        tail = tail[0]
    else:
        python = sys.version.split()[0]
        raise Error("packages", f"pip couldn't install {', '.join(unpinned)} for Python {python}: {tail} "
                                "(no internet? Or a Python too new for them: Python 3.12 or 3.13 works)")
    with open(marker, "w", encoding="utf-8") as f:
        f.write(want)
    return target


def run_heavenfall(source: str, packages: str, rom: str, update: str, out: str, values: dict) -> None:
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join([source, packages] + ([env["PYTHONPATH"]] if env.get("PYTHONPATH") else []))
    env["PYTHONUNBUFFERED"] = "1"
    cmd = [sys.executable, "-m", "core", "--rom", rom, "--code", update, "--port", "--out", out] + options(values)
    say("making the mod from your game (a few minutes): " + " ".join(cmd[3:]))
    if subprocess.run(cmd, cwd=source, env=env).returncode != 0:
        raise Error("heavenfall", "Heavenfall couldn't make the mod: its lines above say why")
    if not os.path.isfile(os.path.join(out, "recomp.json")) and not os.path.isfile(os.path.join(out, "exefs", "code.bin")):
        raise Error("heavenfall", f"Heavenfall made no mod in {out}")


def mods_cli(user_dir: str, args: list[str]) -> dict:
    cmd = [sys.executable, "-u", os.path.join(HERE, "mods.py"), "--json", "--user-dir", user_dir] + args
    p = subprocess.run(cmd, capture_output=True, text=True)
    try:
        result = json.loads(p.stdout.strip().splitlines()[-1]) if p.stdout.strip() else {}
    except ValueError:
        result = {}
    if p.returncode != 0 or not result.get("ok", p.returncode == 0):
        err = result.get("error", {}) if isinstance(result.get("error"), dict) else {}
        raise Error(err.get("code", "mods"), err.get("message") or p.stderr.strip() or p.stdout.strip())
    return result


def installed(user_dir: str) -> bool:
    return os.path.isfile(os.path.join(user_dir, mods.LIBRARY, NAME, mods.MANIFEST))


def cmd_status(args) -> dict:
    return {"ok": True, "installed": installed(args.user_dir), "version": VERSION,
            "settings": load_settings(args.user_dir), "python_ok": sys.version_info >= (3, 12)}


def cmd_install(args) -> dict:
    if sys.version_info < (3, 12):
        raise Error("python", f"Heavenfall needs Python 3.12 or newer; this is {sys.version.split()[0]}")
    for path, what in ((args.rom, "--rom (your decrypted game, .3ds or .cia)"),
                       (args.update, "--update (your decrypted v1.3.0 or v1.1.0 update, .cia)")):
        if not path or not os.path.isfile(path):
            raise Error("usage", f"{what}: {path or 'not given'} isn't a file")
    values = {**load_settings(args.user_dir), **parse_set(args.set or [])}
    save_settings(args.user_dir, values)
    source = fetch_source(args.user_dir)
    packages = ensure_packages(args.user_dir)
    out = os.path.join(home(args.user_dir), "out", NAME)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    run_heavenfall(source, packages, os.path.abspath(args.rom), os.path.abspath(args.update), out, values)
    say("adding it to the mod library")
    mods_cli(args.user_dir, ["add", out, "--name", NAME] + (["--replace"] if installed(args.user_dir) else []))
    mods_cli(args.user_dir, ["deploy"])
    say("done: build the code library again so that the game runs Heavenfall's code")
    return {"ok": True, "installed": True, "settings": values, "build": True}


def cmd_remove(args) -> dict:
    if installed(args.user_dir):
        mods_cli(args.user_dir, ["remove", NAME])
        mods_cli(args.user_dir, ["deploy"])
    return {"ok": True, "installed": False, "build": True}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--user-dir", default=mods.default_user_dir(), help="Azahar's user folder")
    ap.add_argument("--json", action="store_true", help="end with one JSON line (for the port's launcher)")
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    p = sub.add_parser("install")
    p.add_argument("--rom", help="your decrypted MH4U game (.3ds, .cci or .cia)")
    p.add_argument("--update", help="your decrypted update: USA v1.3.0 or EUR v1.1.0 (.cia)")
    p.add_argument("--set", action="append", metavar="KEY=VALUE", help="a choice: " + ", ".join(SETTINGS))
    sub.add_parser("remove")
    args = ap.parse_args(argv)
    args.user_dir = os.path.abspath(args.user_dir)
    try:
        result = {"status": cmd_status, "install": cmd_install, "remove": cmd_remove}[args.command](args)
        code = 0
    except Error as e:
        result, code = {"ok": False, "error": {"code": e.code, "message": str(e)}}, 1
        if not args.json:
            print(f"error: {e}", file=sys.stderr)
    if args.json:
        print(json.dumps(result), flush=True)
    elif code == 0 and args.command == "status":
        print(json.dumps(result, indent=1))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
