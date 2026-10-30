#include "runtime/settings.h"
#include "runtime/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace settings {

namespace {

std::mutex g_lock;
std::map<std::string, std::string> g_values;
std::set<std::string> g_external;
bool g_loaded = false;

std::string id(const Option& o) { return std::string(o.section) + "." + o.key; }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return n ? std::filesystem::path(std::wstring(buf, n)).parent_path().string() : std::string(".");
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string(".") : p.parent_path().string();
#endif
}

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

}

const std::vector<Option>& options() {
    static const std::vector<Option> o = {
        {"game", "data_root", "GOWA_ROOT", Kind::Path, "", "Generated data folder", "Folder that contains bin/ and fw/ produced by tools/setup.py.", nullptr},
        {"game", "disc_dir", "GOWA_DISC_DIR", Kind::Path, "", "Game disc folder", "Extracted disc, the folder that contains PS3_GAME.", nullptr},
        {"game", "patch_dir", "GOWA_PATCH_DIR", Kind::Path, "", "Update folder", "Installed update BCES01741 (PARAM.SFO + USRDIR).", nullptr},
        {"game", "fast_startup", "GOWA_TURBO", Kind::Choice, "3@15", "Fast startup", "Runs the logo and legal screens three times faster during the first 15 seconds (no sound meanwhile).",
         "1=Off|3@15=On"},
        {"game", "flash_dir", "GOWA_FLASH_DIR", Kind::Path, "", "dev_flash folder", "Optional: a PS3 dev_flash copy (fonts, system files).", nullptr},
        {"graphics", "resolution_scale", "GOWA_RES_SCALE", Kind::Choice, "1", "Internal resolution", "Render targets are scaled by this factor.",
         "1=720p (native)|1.5=1080p|2=1440p|3=2160p"},
        {"graphics", "fullscreen", "GOWA_FULLSCREEN", Kind::Bool, "0", "Fullscreen", "Borderless fullscreen; F11 or Alt+Enter switches while playing.", nullptr},
        {"graphics", "fps_limit", "GOWA_FPS_UNLOCK", Kind::Choice, "1", "Frame rate", "Unlocked modes keep game speed correct between 15 and the target.",
         "0=Original (game pacing)|1=Match display|30=30 FPS|60=60 FPS|120=120 FPS|144=144 FPS"},
        {"graphics", "show_fps", "GOWA_FPS_OSD", Kind::Bool, "1", "Show FPS counter", "Small frame rate overlay in the corner.", nullptr},
        {"graphics", "motion_blur", "GOWA_MOTION_BLUR", Kind::Bool, "1", "Motion blur", "The game's camera and attack motion blur pass.", nullptr},
        {"graphics", "async_shaders", "GOWA_ASYNC_SHADERS", Kind::Bool, "1", "Asynchronous shaders",
         "Compile new shaders in the background: no stutter, but objects can appear a moment late the first time.", nullptr},
        {"graphics", "dlss_frame_generation", "GOWA_DLSSG", Kind::Bool, "0", "DLSS frame generation", "Requires an RTX 40/50 GPU and the Streamline files.", nullptr, true},
        {"audio", "enabled", "GOWA_AUDIO", Kind::Bool, "1", "Sound", "Disable to run without an audio device.", nullptr},
        {"audio", "latency_ms", "GOWA_AUDIO_LATENCY_MS", Kind::Int, "100", "Audio buffer (ms)", "Lower values reduce delay, higher values avoid crackling.", nullptr},
        {"performance", "thread_placement", "GOWA_PLACE", Kind::Choice, "", "Thread placement", "Pins game threads to CPU cores.", "=Automatic|off=Operating system"},
        {"performance", "spu_park", "GOWA_SPU_PARK", Kind::Bool, "0", "Park idle SPU threads", "Lower CPU use and heat; can cost a few FPS.", nullptr},
        {"advanced", "native_video", "GOWA_VDEC_HLE", Kind::Bool, "1", "Native video decoding", "Decode cutscene videos on the host instead of emulated SPUs.", nullptr},
        {"advanced", "log_level", "GOWA_LOG", Kind::Choice, "", "Log level", "More detail in gowa.log for bug reports.", "=Normal|debug=Debug|trace=Trace"},
        {"launcher", "show_at_start", nullptr, Kind::Bool, "1", "Show this launcher at start", "Start the game with --launcher to open it anyway.", nullptr},
    };
    return o;
}

std::string user_dir() {
    if (const char* e = std::getenv("GOWA_USER_DIR")) return e;
    return exe_dir() + "/user";
}

std::string path() { return user_dir() + "/settings.ini"; }

bool load() {
    std::lock_guard lk(g_lock);
    if (!g_loaded) {
        for (const Option& o : options()) {
            if (o.env && std::getenv(o.env)) g_external.insert(o.env);
        }
        g_loaded = true;
    }
    std::ifstream f(path());
    if (!f) return false;
    std::string line, section;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        g_values[section + "." + trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return true;
}

bool save() {
    std::lock_guard lk(g_lock);
    std::error_code ec;
    std::filesystem::create_directories(user_dir(), ec);
    std::string tmp = path() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        f << "# God of War Ascension PC settings. Environment variables of the same name override these.\n";
        std::string section;
        for (const Option& o : options()) {
            if (section != o.section) {
                section = o.section;
                f << "\n[" << section << "]\n";
            }
            f << "# " << o.label << ": " << o.help;
            if (o.env) f << " (" << o.env << ")";
            f << "\n";
            auto it = g_values.find(id(o));
            f << o.key << " = " << (it != g_values.end() ? it->second : std::string(o.def)) << "\n";
        }
    }
    std::filesystem::rename(tmp, path(), ec);
    return !ec;
}

std::string get(const Option& o) {
    std::lock_guard lk(g_lock);
    if (o.env && g_external.count(o.env)) {
        const char* v = std::getenv(o.env);
        return v ? v : "";
    }
    auto it = g_values.find(id(o));
    return it != g_values.end() ? it->second : std::string(o.def);
}

void set(const Option& o, const std::string& value) {
    std::lock_guard lk(g_lock);
    g_values[id(o)] = value;
}

void apply_env() {
    std::lock_guard lk(g_lock);
    std::string applied;
    for (const Option& o : options()) {
        if (!o.env || g_external.count(o.env)) continue;
        auto it = g_values.find(id(o));
        const std::string& v = it != g_values.end() ? it->second : std::string(o.def);
        if (v.empty()) continue;
        set_env(o.env, v);
        applied += std::string(" ") + o.env + "=" + v;
    }
    LOG_INFO("settings", "applied:%s", applied.c_str());
    if (!g_external.empty()) {
        std::string list;
        for (const std::string& e : g_external) list += " " + e;
        LOG_INFO("settings", "environment overrides settings.ini:%s", list.c_str());
    }
}

}
