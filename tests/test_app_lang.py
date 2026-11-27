"""port/app/ui/lang.cpp: the menu's language. A text is given in Spanish when there is a
translation, in English otherwise; Dear ImGui's ID suffix and a sub-row's indent stay as they were;
and every text of the menu's index (labels, helps, section titles) has its Spanish one."""
from __future__ import annotations

import os
import re
import shutil
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "port", "app")
UI = os.path.join(APP, "ui")

CXX = next((c for c in ("c++", "clang++", "g++") if shutil.which(c)), None)
pytestmark = pytest.mark.skipif(CXX is None, reason="needs a C++ compiler")

DRIVER = r"""
#include <cstdio>
#include <string>
#include "ui/lang.h"
#include "ui/settings_index.h"

using namespace App::Ui;

// LANGUAGE TEXT...: a line per text, as Tr gives it; LANGUAGE index: the index's untranslated texts
int main(int argc, char** argv) {
    Lang::Set(argv[1]);
    if (std::string(argv[2]) == "index") {
        for (int i = 0; i < Index::ROW_COUNT; ++i) {
            const Index::Row& r = Index::ROWS[i];
            for (const char* t : {r.label, r.help}) {
                if (std::string(Lang::Tr(t)) == t) std::printf("%s\n", t);
            }
        }
        for (int i = 0; i < Index::SECTION_COUNT; ++i) {
            const char* t = Index::SECTIONS[i].title;
            if (*t && std::string(Lang::Tr(t)) == t) std::printf("%s\n", t);
        }
        return 0;
    }
    for (int i = 2; i < argc; ++i) std::printf("%s\n", Lang::Tr(argv[i]));
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    work = tmp_path_factory.mktemp("lang")
    src = work / "driver.cpp"
    src.write_text(DRIVER)
    exe = work / "driver"
    subprocess.run([CXX, "-std=c++17", "-DMH4U_LANG_NO_SDL", "-I", APP, str(src), os.path.join(UI, "lang.cpp"),
                    os.path.join(UI, "settings_index.cpp"), "-o", str(exe)], check=True)
    return exe


def tr(driver, language, *texts):
    out = subprocess.run([str(driver), language, *texts], capture_output=True, check=True).stdout.decode("utf-8")
    return out.splitlines()


def test_english_stays_english(driver):
    assert tr(driver, "en", "Full screen", "Back to the game") == ["Full screen", "Back to the game"]


def test_spanish(driver):
    assert tr(driver, "es", "Full screen", "Back to the game") == ["Pantalla completa", "Volver al juego"]
    # a text without a translation stays as it is
    assert tr(driver, "es", "Nothing like this") == ["Nothing like this"]


def test_id_suffix_and_indent_kept(driver):
    assert tr(driver, "es", "Reset##keys", "Cancel###id", "    Across") == [
        "Reiniciar##keys", "Cancelar###id", "    Horizontal"]


def test_the_whole_index_is_translated(driver):
    # the 3DS's button names are the same in Spanish
    assert set(tr(driver, "es", "index")) <= {"ZL", "ZR", "SELECT"}


def test_placeholders_kept():
    """A help with a {key:...} placeholder keeps it in Spanish, and no text loses a newline."""
    with open(os.path.join(UI, "lang_es.inc"), encoding="utf-8") as f:
        pairs = re.findall(r'^\{"((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)"\},$', f.read(), re.M)
    assert len(pairs) > 400
    for en, es in pairs:
        assert re.findall(r"\{key:\w+\}", en) == re.findall(r"\{key:\w+\}", es), en
        assert en.count("\\n") == es.count("\\n"), en
        assert "%" not in es or "%" in en, en
