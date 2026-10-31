#include "runtime/session.h"
#include "runtime/log.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>

namespace session {

namespace {

bool g_active = false;
std::string g_dir;

void set_default(const char* name, const std::string& value) {
    if (std::getenv(name)) return;
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

}

bool begin() {
    const char* e = std::getenv("GOWA_SESSION");
    if (!e || !*e || e[0] == '0') return false;
    if (const char* d = std::getenv("GOWA_SESSION_DIR")) {
        g_dir = d;
    } else {
        std::time_t now = std::time(nullptr);
        std::tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &lt);
        g_dir = std::string("sessions/") + stamp;
    }
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
    if (ec) {
        LOG_WARN("session", "cannot create %s", g_dir.c_str());
        return false;
    }
    g_active = true;
    set_default("GOWA_CRITPATH", "1");
    set_default("GOWA_CRITPATH_FILE", g_dir + "/critpath.bin");
    set_default("GOWA_THREAD_CPU", "1");
    set_default("GOWA_HITCH_DUMP_MS", "150");
    logging::mirror_to((g_dir + "/gowa.log").c_str());
    LOG_INFO("session", "recording to %s", std::filesystem::absolute(g_dir, ec).string().c_str());
    return true;
}

bool active() { return g_active; }

const std::string& dir() { return g_dir; }

FILE* open(const char* name) {
    if (!g_active) return nullptr;
    return std::fopen((g_dir + "/" + name).c_str(), "wb");
}

}
