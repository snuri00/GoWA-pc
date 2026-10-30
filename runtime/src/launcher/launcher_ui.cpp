#include "launcher_ui.h"

#include "runtime/settings.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <stb_image.h>

namespace launcher::ui {

namespace {

namespace fs = std::filesystem;

const ImVec4 kGold{0.86f, 0.69f, 0.36f, 1.0f};
const ImVec4 kBronze{0.55f, 0.40f, 0.21f, 1.0f};
const ImVec4 kRed{0.60f, 0.09f, 0.07f, 1.0f};
const ImVec4 kRedHot{0.74f, 0.13f, 0.09f, 1.0f};
const ImVec4 kRedActive{0.86f, 0.18f, 0.11f, 1.0f};
const ImVec4 kText{0.93f, 0.89f, 0.82f, 1.0f};
const ImVec4 kMuted{0.62f, 0.56f, 0.50f, 1.0f};

ImFont* g_body = nullptr;
ImFont* g_title = nullptr;
ImFont* g_subtitle = nullptr;
Image g_background;
Image g_icon;
std::map<std::string, std::array<char, 512>> g_text;
std::string g_status;
bool g_dirty = false;

const char* first_existing(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        std::error_code ec;
        if (fs::exists(p, ec)) return p;
    }
    return nullptr;
}

ImFont* load_font(std::initializer_list<const char*> paths, float size) {
    if (const char* p = first_existing(paths)) return ImGui::GetIO().Fonts->AddFontFromFileTTF(p, size);
    return nullptr;
}

void theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 3.0f;
    s.TabRounding = 3.0f;
    s.GrabRounding = 3.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 9);
    s.WindowPadding = ImVec2(18, 16);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.03f, 0.03f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.07f, 0.04f, 0.035f, 0.86f);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.05f, 0.045f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(kBronze.x, kBronze.y, kBronze.z, 0.55f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.09f, 0.075f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.13f, 0.10f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.32f, 0.15f, 0.11f, 1.0f);
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_SliderGrab] = kGold;
    c[ImGuiCol_SliderGrabActive] = kRedActive;
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.10f, 0.08f, 1.0f);
    c[ImGuiCol_ButtonHovered] = kRedHot;
    c[ImGuiCol_ButtonActive] = kRedActive;
    c[ImGuiCol_Header] = ImVec4(kRed.x, kRed.y, kRed.z, 0.55f);
    c[ImGuiCol_HeaderHovered] = kRedHot;
    c[ImGuiCol_HeaderActive] = kRedActive;
    c[ImGuiCol_Tab] = ImVec4(0.16f, 0.08f, 0.065f, 1.0f);
    c[ImGuiCol_TabHovered] = kRedHot;
    c[ImGuiCol_TabSelected] = kRed;
    c[ImGuiCol_TabSelectedOverline] = kGold;
    c[ImGuiCol_Separator] = ImVec4(kBronze.x, kBronze.y, kBronze.z, 0.45f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.2f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(kBronze.x, kBronze.y, kBronze.z, 0.6f);
}

Image load_image(Upload upload, const std::string& path) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) return {};
    Image img = upload(px, w, h);
    stbi_image_free(px);
    return img;
}

std::string disc_dir() {
    for (const settings::Option& o : settings::options()) {
        if (std::strcmp(o.key, "disc_dir") == 0) {
            std::string v = settings::get(o);
            if (!v.empty()) return v;
        }
    }
    if (const char* e = std::getenv("GOWA_RPCS3_DIR")) return std::string(e) + "/games/BCES01741";
    std::error_code ec;
    if (fs::exists("rpcs3/games/BCES01741", ec)) return "rpcs3/games/BCES01741";
#ifdef _WIN32
    return "D:/Games/rpcs3/games/BCES01741";
#else
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/rpcs3/games/BCES01741";
#endif
}

bool path_ok(const char* key, const std::string& p) {
    if (p.empty()) return true;
    std::error_code ec;
    if (std::strcmp(key, "disc_dir") == 0) return fs::exists(p + "/PS3_GAME/PARAM.SFO", ec);
    if (std::strcmp(key, "patch_dir") == 0) return fs::exists(p + "/PARAM.SFO", ec);
    if (std::strcmp(key, "data_root") == 0) return fs::exists(p + "/bin", ec) && fs::exists(p + "/fw", ec);
    return fs::is_directory(p, ec);
}

std::vector<std::pair<std::string, std::string>> parse_choices(const char* c) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string s = c ? c : "";
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t end = s.find('|', pos);
        if (end == std::string::npos) end = s.size();
        std::string item = s.substr(pos, end - pos);
        size_t eq = item.find('=');
        out.push_back(eq == std::string::npos ? std::make_pair(item, item) : std::make_pair(item.substr(0, eq), item.substr(eq + 1)));
        pos = end + 1;
    }
    return out;
}

void option_row(const settings::Option& o) {
    std::string value = settings::get(o);
    bool locked = o.env && std::getenv(o.env) && value == std::getenv(o.env) && value != o.def;
    ImGui::PushID(o.key);
    ImGui::BeginDisabled(locked);
    float label_w = 230.0f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(o.label);
    if (ImGui::IsItemHovered() && o.help) ImGui::SetTooltip("%s", o.help);
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-1);
    std::string next = value;
    switch (o.kind) {
    case settings::Kind::Bool: {
        bool v = value == "1";
        if (ImGui::Checkbox("##v", &v)) next = v ? "1" : "0";
        break;
    }
    case settings::Kind::Int: {
        int v = std::atoi(value.c_str());
        if (ImGui::InputInt("##v", &v, 10, 50)) next = std::to_string(std::max(v, 0));
        break;
    }
    case settings::Kind::Choice: {
        auto choices = parse_choices(o.choices);
        const char* preview = value.c_str();
        for (auto& [k, l] : choices) {
            if (k == value) preview = l.c_str();
        }
        if (ImGui::BeginCombo("##v", preview)) {
            for (auto& [k, l] : choices) {
                if (ImGui::Selectable(l.c_str(), k == value)) next = k;
            }
            ImGui::EndCombo();
        }
        break;
    }
    default: {
        auto& buf = g_text[o.key];
        if (std::strcmp(buf.data(), value.c_str()) != 0 && !ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) std::snprintf(buf.data(), buf.size(), "%s", value.c_str());
        if (ImGui::InputTextWithHint("##v", o.kind == settings::Kind::Path ? "default location" : "", buf.data(), buf.size())) next = buf.data();
        break;
    }
    }
    if (o.kind == settings::Kind::Path) {
        bool ok = path_ok(o.key, next);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + label_w - ImGui::GetStyle().WindowPadding.x);
        ImGui::TextColored(ok ? ImVec4(0.55f, 0.75f, 0.45f, 1.0f) : ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "%s", next.empty() ? "using the default location" : ok ? "found" : "not found");
    }
    ImGui::EndDisabled();
    if (locked) {
        ImGui::SameLine();
        ImGui::TextDisabled("(set by %s)", o.env);
    }
    if (next != value) {
        settings::set(o, next);
        g_dirty = true;
    }
    ImGui::PopID();
}

void section(const char* name) {
    for (const settings::Option& o : settings::options()) {
#ifndef _WIN32
        if (o.windows_only) continue;
#endif
        if (std::strcmp(o.section, name) == 0) option_row(o);
    }
}

void clear_shader_cache() {
    std::error_code ec;
    uintmax_t n = fs::remove_all("shader_cache", ec);
    g_status = ec ? "could not clear the shader cache" : "shader cache cleared (" + std::to_string(n) + " files)";
}

bool big_button(const char* label, ImVec2 size, bool primary) {
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, kRed);
        ImGui::PushStyleColor(ImGuiCol_Border, kGold);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
    }
    bool r = ImGui::Button(label, size);
    if (primary) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
    return r;
}

}

void setup(Upload upload) {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
#ifdef _WIN32
    g_body = load_font({"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"}, 18.0f);
    g_title = load_font({"C:/Windows/Fonts/georgiab.ttf", "C:/Windows/Fonts/timesbd.ttf"}, 54.0f);
    g_subtitle = load_font({"C:/Windows/Fonts/georgia.ttf", "C:/Windows/Fonts/times.ttf"}, 30.0f);
#else
    g_body = load_font({"/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}, 18.0f);
    g_title = load_font({"/usr/share/fonts/noto/NotoSerif-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSerif-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf"}, 54.0f);
    g_subtitle = load_font({"/usr/share/fonts/noto/NotoSerif-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSerif.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf"}, 30.0f);
#endif
    if (!g_body) g_body = io.Fonts->AddFontDefault();
    theme();
    std::string disc = disc_dir();
    g_background = load_image(upload, disc + "/PS3_GAME/PIC1.PNG");
    if (!g_background.id) g_background = load_image(upload, disc + "/PS3_GAME/PIC0.PNG");
    g_icon = load_image(upload, disc + "/PS3_GAME/ICON0.PNG");
}

bool frame(Result& result) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 size = io.DisplaySize;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), size, IM_COL32(12, 7, 6, 255));
    if (g_background.id && g_background.w && g_background.h) {
        float s = std::max(size.x / g_background.w, size.y / g_background.h);
        ImVec2 im(g_background.w * s, g_background.h * s);
        ImVec2 p0((size.x - im.x) * 0.5f, (size.y - im.y) * 0.5f);
        bg->AddImage(g_background.id, p0, ImVec2(p0.x + im.x, p0.y + im.y));
    }
    bg->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(10, 4, 3, 235), IM_COL32(10, 4, 3, 120), IM_COL32(10, 4, 3, 170), IM_COL32(10, 4, 3, 245));
    bg->AddRectFilled(ImVec2(0, size.y - 3), size, IM_COL32(150, 24, 18, 255));

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    if (g_body) ImGui::PushFont(g_body);

    float left = std::min(420.0f, size.x * 0.38f);
    ImGui::SetCursorPos(ImVec2(36, 40));
    ImGui::BeginGroup();
    if (g_icon.id) {
        float iw = left - 20.0f, ih = iw * g_icon.h / std::max(1, g_icon.w);
        ImGui::Image(g_icon.id, ImVec2(iw, ih));
    } else {
        if (g_title) ImGui::PushFont(g_title);
        ImGui::TextColored(kGold, "GOD OF WAR");
        if (g_title) ImGui::PopFont();
        if (g_subtitle) ImGui::PushFont(g_subtitle);
        ImGui::TextColored(ImVec4(0.80f, 0.16f, 0.11f, 1.0f), "A S C E N S I O N");
        if (g_subtitle) ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(0, 10));
    ImVec2 line = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(line, ImVec2(line.x + left - 20.0f, line.y), ImGui::GetColorU32(kBronze), 1.0f);
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PushTextWrapPos(36.0f + left - 20.0f);
    ImGui::TextColored(kGold, "PC port");
    ImGui::TextColored(kMuted, "Settings are saved to %s", settings::path().c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();

    float panel_x = left + 60.0f;
    float panel_w = size.x - panel_x - 36.0f;
    float bottom_h = 70.0f;
    ImGui::SetCursorPos(ImVec2(panel_x, 40));
    ImGui::BeginChild("panel", ImVec2(panel_w, size.y - 40 - bottom_h - 24), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("tabs")) {
        struct Tab {
            const char* name;
            const char* section;
        };
        static const Tab tabs[] = {{"Game", "game"}, {"Graphics", "graphics"}, {"Audio", "audio"}, {"Performance", "performance"}, {"Advanced", "advanced"}};
        for (const Tab& t : tabs) {
            if (ImGui::BeginTabItem(t.name)) {
                ImGui::Dummy(ImVec2(0, 6));
                section(t.section);
                if (std::strcmp(t.section, "advanced") == 0) {
                    section("launcher");
                    ImGui::Dummy(ImVec2(0, 8));
                    if (ImGui::Button("Clear shader cache")) clear_shader_cache();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Deletes compiled shaders and the pipeline cache; saves are kept.");
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(panel_x, size.y - bottom_h));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kMuted, "%s", !g_status.empty() ? g_status.c_str() : g_dirty ? "unsaved changes" : "");
    float bw = 130.0f, pw = 190.0f, bh = 44.0f;
    bool done = false;
    ImGui::SetCursorPos(ImVec2(size.x - 36 - pw - 2 * (bw + 12), size.y - bottom_h - 4));
    if (big_button("Quit", ImVec2(bw, bh), false)) {
        result = Result::Quit;
        done = true;
    }
    ImGui::SameLine(0, 12);
    if (big_button("Save", ImVec2(bw, bh), false)) {
        g_status = settings::save() ? "saved" : "could not write the settings file";
        g_dirty = false;
    }
    ImGui::SameLine(0, 12);
    if (big_button("PLAY", ImVec2(pw, bh), true) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
        if (g_dirty) settings::save();
        result = Result::Play;
        done = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        result = Result::Quit;
        done = true;
    }
    if (g_body) ImGui::PopFont();
    ImGui::End();
    return done;
}

void shutdown() {
    g_background = {};
    g_icon = {};
    g_body = g_title = g_subtitle = nullptr;
}

}

namespace launcher {

bool wanted(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--launcher") == 0) return true;
        if (std::strcmp(argv[i], "--no-launcher") == 0) return false;
    }
    if (std::getenv("GOWA_NO_LAUNCHER") || std::getenv("GOWA_HEADLESS")) return false;
    for (const settings::Option& o : settings::options()) {
        if (std::strcmp(o.section, "launcher") == 0 && std::strcmp(o.key, "show_at_start") == 0) return settings::get(o) != "0";
    }
    return true;
}

}
