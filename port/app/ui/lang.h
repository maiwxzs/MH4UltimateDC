// The in-game menu's language: its texts are written in English in the code and the index, and
// Tr() gives them in the language chosen (mh4u-app.ini [Display] language: auto, en, es). A text
// without a translation stays in English. Only the menu's own texts: the game's are the game's.
#pragma once

#include <string>

namespace App::Ui::Lang {

/// "en", "es", or "auto" (the system's preferred language, when it has a translation). Call it
/// on the thread that draws the menu.
void Set(const std::string& language);
/// The language in use: "en" or "es".
const char* Current();

/// `english` in the language in use, or itself without a translation. Dear ImGui's ID suffix
/// ("Reset##keys", "Label###id") is kept as it is and only the part before it is translated. The
/// result lives as long as the program.
const char* Tr(const char* english);
std::string Tr(const std::string& english);

} // namespace App::Ui::Lang
