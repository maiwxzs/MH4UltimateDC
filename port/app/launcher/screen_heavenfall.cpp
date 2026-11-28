// Heavenfall: the mod Heavenfall Beta (popzol and BlitzWOLF, GPL-3.0) made from the player's own
// game for the port (tools/heavenfall.py: its --port mode, added to the mod library as
// "Heavenfall"), with the player's choice of its parts. The choices are kept in
// <Azahar's user folder>/mh4u-heavenfall/settings.json, which the tool reads; "Install" (or
// "Apply") makes the mod again with them, "Remove" takes it out; the code library is built again
// after either (the game runs Heavenfall's code only from a library built with it).
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <SDL.h>
#include <imgui.h>
#include <json.hpp>

#include "common/logging/log.h"
#include "launcher/process.h"
#include "launcher/state.h"
#include "launcher/tools.h"
#include "launcher/ui.h"
#include "ui/lang.h"

namespace Launcher {

namespace {

using Json = nlohmann::json;
using App::Ui::Lang::Tr;

constexpr const char* PAGE = "https://github.com/popzol/4u-heavenfall-mod-beta";

/// One choice, as tools/heavenfall.py's SETTINGS names it.
struct Option {
    const char* key;
    const char* label;
    const char* about;
    bool fallback;
};

struct Section {
    const char* title;
    std::initializer_list<Option> options;
};

const Section SECTIONS[] = {
    {"SCREEN",
     {{"single_screen", "One screen",
       "The touch screen's menus on the top screen: quest list, shops, Canteen, item box, Gathering Hall, Guild "
       "Cards, character creation. X opens the World Map in town, L the town map.",
       true}}},
    {"HUNTING",
     {{"lock_on", "Lock on with L + up",
       "Hold L and press up on the D-pad to lock on to a large monster or switch to the other; its face shows on "
       "the top screen.",
       true},
      {"damage_numbers", "Damage numbers",
       "Each hit's damage over the monster (Wilds, Rise or classic style, set in the game: START, third page, Mod "
       "Settings).",
       false}}},
    {"COMFORT",
     {{"quality_of_life", "Quality of life",
       "Heavenfall's comforts: World-style sharpness, upgrade stars on items, Hunting Horn songs, the Smithy's "
       "weapon tree. Each can be turned off in the game (START, third page, Mod Settings).",
       true},
      {"box_equipment", "Equipment at every item box", "Manage Equipment and Equipment Sets at any item box.", true},
      {"hold_a", "Hold A to keep gathering",
       "Held A keeps gathering and carving, and moves through the quest rewards.", false},
      {"fast_text", "Hold B for fast text", "Held B runs through conversations.", false},
      {"skip_intro", "Skip the prologue", "A new character starts in Val Habar (existing saves don't change).",
       false}}},
    {"EXTRAS",
     {{"layered_equipment", "Layered Gear",
       "Looks bought once and worn over your gear, which stays as it is (stats, skills).", false},
      {"rare_exchange", "Rare Materials Exchange",
       "The Wyporium trades common materials for a monster's rare ones (with Layered Gear).", false},
      {"jukebox", "Jukebox", "Your Housekeeper plays the music you choose in towns.", false}}},
};

constexpr const char* HUD_SCALES[] = {"100", "90", "80", "70", "60"};

struct Page {
    bool loaded = false;
    std::map<std::string, bool> flags;
    std::string hud_scale = "100";
    bool installed = false;
    bool changed = false;  ///< choices changed since it was made: Apply makes it again
    bool build_after = false;  ///< the job went well: the code library is built once its window closes
};

Page page;

/// A UTF-8 path as the file system's (Windows reads a plain std::string in its ANSI code page).
std::filesystem::path Utf8Path(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::filesystem::path Home() {
    return Utf8Path(Tools::UserDir()) / "mh4u-heavenfall";
}

bool IsInstalled() {
    std::error_code ec;
    return std::filesystem::is_regular_file(Utf8Path(Tools::UserDir()) / "mh4u-mods" / "Heavenfall" / "mod.json", ec);
}

void Load() {
    page = Page{};
    for (const auto& section : SECTIONS) {
        for (const auto& o : section.options) {
            page.flags[o.key] = o.fallback;
        }
    }
    std::ifstream file(Home() / "settings.json");
    if (file) {
        try {
            const Json j = Json::parse(file);
            for (auto& [key, value] : page.flags) {
                if (j.contains(key) && j[key].is_boolean()) {
                    value = j[key].get<bool>();
                }
            }
            if (j.contains("hud_scale") && j["hud_scale"].is_string()) {
                page.hud_scale = j["hud_scale"].get<std::string>();
            }
        } catch (const std::exception&) {
            // a damaged file: the defaults, written again on the next change
        }
    }
    page.installed = IsInstalled();
    page.loaded = true;
}

void Save() {
    Json j = Json::object();
    for (const auto& [key, value] : page.flags) {
        j[key] = value;
    }
    j["hud_scale"] = page.hud_scale;
    std::error_code ec;
    std::filesystem::create_directories(Home(), ec);
    std::ofstream(Home() / "settings.json") << j.dump(1);
    page.changed = true;
}

/// The tool in the job window; `then` once it finished well.
void StartTool(State& s, std::vector<std::string> args, const char* title, std::function<void()> then) {
    if (s.job.Active() || !s.ToolsReady()) {
        return;
    }
    auto process = std::make_unique<Process>();
    std::string error;
    const auto command = Tools::HeavenfallCommand(s.python, s.tools_dir, args);
    if (!process->Start(command, error, /*low_priority=*/true)) {
        s.Message(title, error);
        return;
    }
    LOG_INFO(Frontend, "launcher: heavenfall: {}", fmt::join(command, " "));
    s.job = Job{};
    s.job.kind = Job::Kind::Command;
    s.job.title = title;
    s.job.cancellable = true;
    s.job.process = std::move(process);
    s.job.on_done = [&s, then](int exit_code, const std::string& output) {
        page.installed = IsInstalled();
        if (exit_code == 0) {
            page.changed = false;
            s.job.summary = Tr("Done. Close this and the code library is built again with it.");
            then();
            return;
        }
        // the tool's last line: {"ok": false, "error": {"message": ...}}
        std::string why = output;
        const auto last = output.find_last_of('{') == std::string::npos ? std::string::npos
                                                                         : output.rfind("{\"ok\"");
        if (last != std::string::npos) {
            try {
                const Json j = Json::parse(output.substr(last));
                why = j.at("error").at("message").get<std::string>();
            } catch (const std::exception&) {
            }
        }
        s.job.summary = why;
        LOG_WARNING(Frontend, "launcher: heavenfall: {}", why);
    };
}

void Install(State& s) {
    if (s.game.state != Checks::Game::State::Ok || s.update.state != Checks::Update::State::Ok) {
        s.Message("Heavenfall", "It's made from your game and its update: set both up first (Setup).");
        return;
    }
    StartTool(s, {"install", "--rom", s.config.game, "--update", s.update.content},
              page.installed ? "Applying Heavenfall's changes" : "Installing Heavenfall", [] { page.build_after = true; });
}

void Remove(State& s) {
    StartTool(s, {"remove"}, "Removing Heavenfall", [] { page.build_after = true; });
}

void Group(const char* title) {
    ImGui::Spacing();
    Ui::PushFont(0.85f, true);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Ui::Color::Dim), "%s", Tr(title));
    Ui::PopFont();
}

} // namespace

void DrawHeavenfall(State& s) {
    if (!page.loaded || (!s.job.Active() && page.installed != IsInstalled())) {
        const bool build = page.build_after;
        Load();
        page.build_after = build;
    }
    if (page.build_after && !s.job.Active()) {
        // the game runs Heavenfall's code (or the game's alone again) only from a library built so
        page.build_after = false;
        s.StartBuild();
    }
    const float scale = Ui::Scale();
    ImGui::BeginChild("##heavenfall", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
    Ui::Heading("Heavenfall");
    Ui::TextDim("Heavenfall Beta, by popzol and BlitzWOLF (GPL-3.0), made from your own game for this port. Choose "
                "its parts here: only what is on goes into the game. Your save stays MH4U's own; what Heavenfall "
                "adds is kept in files beside it.");
    ImGui::Spacing();
    if (page.installed) {
        Ui::StatusLine(Ui::Status::Ok, Tr("Installed"),
                       page.changed ? Tr("You changed its parts: Apply makes it again with them.")
                                    : Tr("In the game with the parts below."));
    } else {
        Ui::StatusLine(Ui::Status::Off, Tr("Not installed"),
                       Tr("Install makes it from your game (a few minutes, once online to fetch Heavenfall), then "
                          "the code library is built again."));
    }
    ImGui::Spacing();

    for (const auto& section : SECTIONS) {
        Group(section.title);
        for (const auto& o : section.options) {
            bool on = page.flags[o.key];
            const std::string label = fmt::format("{}##{}", Tr(o.label), o.key);
            FocusNext(s);
            if (Ui::Toggle(label.c_str(), &on)) {
                page.flags[o.key] = on;
                Save();
            }
            ImGui::Indent(ImGui::GetFrameHeight() * 1.8f + ImGui::GetStyle().ItemInnerSpacing.x);
            Ui::PushFont(0.85f);
            Ui::TextDim(o.about);
            Ui::PopFont();
            ImGui::Unindent(ImGui::GetFrameHeight() * 1.8f + ImGui::GetStyle().ItemInnerSpacing.x);
        }
        if (std::string(section.title) == "SCREEN") {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Tr("HUD size"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f * scale);
            const std::string current = page.hud_scale + "%";
            if (ImGui::BeginCombo("##hud_scale", current.c_str())) {
                for (const char* v : HUD_SCALES) {
                    const std::string item = std::string(v) + "%";
                    if (ImGui::Selectable(item.c_str(), page.hud_scale == v)) {
                        page.hud_scale = v;
                        Save();
                    }
                }
                ImGui::EndCombo();
            }
            Ui::PushFont(0.85f);
            Ui::TextDim("The hunt's HUD smaller, each part towards its corner (100%: the game's own).");
            Ui::PopFont();
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();
    const bool ready = s.ToolsReady() && !s.job.Active();
    ImGui::BeginDisabled(!ready);
    if (Ui::AccentButton(page.installed ? "Apply" : "Install")) {
        Install(s);
    }
    if (page.installed) {
        ImGui::SameLine();
        if (ImGui::Button(Tr("Remove"))) {
            Remove(s);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Tr("Heavenfall's page"))) {
        SDL_OpenURL(PAGE);
    }
    if (!s.ToolsReady()) {
        Ui::TextDim("It needs the port's tools and Python 3.12 or newer (Setup).");
    }
    Ui::PushFont(0.85f);
    Ui::TextDim("The port does the frame rate, the field of view and the picture itself: Heavenfall's cheats stay "
                "off. Its other settings (damage numbers' style, the camera, the sharpness gauge...) are in the "
                "game: START, third page, Mod Settings.");
    Ui::PopFont();
    ImGui::EndChild();
}

} // namespace Launcher
