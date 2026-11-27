#include "ui/lang.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <string_view>
#include <unordered_map>

#ifndef MH4U_LANG_NO_SDL
#include <SDL.h>
#endif

namespace App::Ui::Lang {

namespace {

struct Pair {
    const char* en;
    const char* es;
};

// the Spanish texts (lang_es.inc): {"English", "Español"}, one per line
constexpr Pair SPANISH[] = {
#include "ui/lang_es.inc"
};

bool spanish = false;

const std::unordered_map<std::string_view, const char*>& Table() {
    static const auto table = [] {
        std::unordered_map<std::string_view, const char*> t;
        t.reserve(std::size(SPANISH));
        for (const Pair& p : SPANISH) {
            t.emplace(p.en, p.es);
        }
        return t;
    }();
    return table;
}

/// Texts made of a translation and an ID suffix, kept for the program's life (Tr's pointers).
std::deque<std::string>& Kept() {
    static std::deque<std::string> kept;
    return kept;
}
std::unordered_map<std::string, const char*>& Joined() {
    static std::unordered_map<std::string, const char*> joined;
    return joined;
}
std::mutex joined_mutex;

std::string SystemLanguage() {
#ifndef MH4U_LANG_NO_SDL
    if (SDL_Locale* locales = SDL_GetPreferredLocales()) {
        std::string first = locales[0].language ? locales[0].language : "";
        SDL_free(locales);
        return first;
    }
#endif
    return "";
}

} // namespace

void Set(const std::string& language) {
    std::string code;
    for (const char c : language) {
        code += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (code.empty() || code == "auto") {
        code = SystemLanguage();
    }
    spanish = code.rfind("es", 0) == 0;
}

const char* Current() {
    return spanish ? "es" : "en";
}

const char* Tr(const char* english) {
    if (!spanish || english == nullptr || *english == '\0') {
        return english;
    }
    const std::string_view text{english};
    // an indented sub-row's label ("    Across") keeps its indent; an ID suffix ("##id") stays
    const auto start = std::min(text.find_first_not_of(' '), text.size());
    const auto hashes = text.find("##", start);
    const std::string_view shown = text.substr(start, hashes == std::string_view::npos ? hashes : hashes - start);
    const auto& table = Table();
    const auto found = table.find(shown);
    if (found == table.end()) {
        return english;
    }
    if (start == 0 && hashes == std::string_view::npos) {
        return found->second;
    }
    // the translation with the indent and the ID suffix as they were (the ID is the same in both)
    std::lock_guard lock{joined_mutex};
    auto& joined = Joined();
    if (const auto it = joined.find(std::string(text)); it != joined.end()) {
        return it->second;
    }
    Kept().push_back(std::string(text.substr(0, start)) + found->second +
                     std::string(hashes == std::string_view::npos ? std::string_view{} : text.substr(hashes)));
    const char* result = Kept().back().c_str();
    joined.emplace(std::string(text), result);
    return result;
}

std::string Tr(const std::string& english) {
    return Tr(english.c_str());
}

} // namespace App::Ui::Lang
