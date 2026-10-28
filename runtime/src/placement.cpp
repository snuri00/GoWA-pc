#include "runtime/placement.h"
#include "runtime/log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace placement {

namespace {

enum class Mode { OFF, IDEAL, CPUSET, MASK };

struct Rule {
    std::vector<int> lps;
    bool has_prio = false;
    int prio = 0;
};

struct Config {
    Mode mode = Mode::OFF;
    std::vector<std::vector<int>> cores;
    std::map<std::string, Rule> rules;
    std::map<int, ULONG> cpuset_ids;
};

std::vector<std::vector<int>> read_cores() {
    std::vector<std::pair<int, std::vector<int>>> found;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<uint8_t> buf(len);
    auto* p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    std::vector<std::vector<int>> cores;
    if (!len || !GetLogicalProcessorInformationEx(RelationProcessorCore, p, &len)) return cores;
    for (DWORD off = 0; off < len;) {
        auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount >= 1 && e->Processor.GroupMask[0].Group == 0) {
            std::vector<int> lps;
            KAFFINITY m = e->Processor.GroupMask[0].Mask;
            for (int i = 0; i < 64; i++) {
                if (m & (KAFFINITY(1) << i)) lps.push_back(i);
            }
            if (!lps.empty()) found.push_back({e->Processor.EfficiencyClass, lps});
        }
        off += e->Size;
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& f : found) cores.push_back(f.second);
    return cores;
}

std::map<int, ULONG> read_cpuset_ids() {
    std::map<int, ULONG> ids;
    ULONG len = 0;
    GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
    std::vector<uint8_t> buf(len);
    if (!len || !GetSystemCpuSetInformation(reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data()), len, &len, GetCurrentProcess(), 0)) return ids;
    for (ULONG off = 0; off < len;) {
        auto* e = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data() + off);
        if (e->Type == CpuSetInformation && e->CpuSet.Group == 0) ids[e->CpuSet.LogicalProcessorIndex] = e->CpuSet.Id;
        off += e->Size;
    }
    return ids;
}

void add_token(const Config& c, const std::string& t, std::vector<int>& out) {
    if (t.empty()) return;
    char kind = t[0];
    if (kind == 'c' || kind == 's' || kind == 'C') {
        size_t k = std::strtoul(t.c_str() + 1, nullptr, 10);
        if (k >= c.cores.size()) return;
        const auto& core = c.cores[k];
        if (kind == 'c') out.push_back(core[0]);
        else if (kind == 's') out.push_back(core.size() > 1 ? core[1] : core[0]);
        else out.insert(out.end(), core.begin(), core.end());
        return;
    }
    out.push_back(std::atoi(t.c_str()));
}

Rule parse_rule(const Config& c, const std::string& v) {
    Rule r;
    std::string set = v;
    size_t at = v.find('@');
    if (at != std::string::npos) {
        set = v.substr(0, at);
        r.has_prio = true;
        r.prio = std::atoi(v.c_str() + at + 1);
    }
    size_t pos = 0;
    while (pos <= set.size()) {
        size_t end = set.find('+', pos);
        if (end == std::string::npos) end = set.size();
        add_token(c, set.substr(pos, end - pos), r.lps);
        pos = end + 1;
    }
    return r;
}

std::string default_spec(const Config& c) {
    size_t n = c.cores.size();
    if (n < 4) return "";
    size_t rest = n - 2;
    std::vector<std::string> slots;
    size_t pairs = c.cores[2].size() > 1 && rest < 6 ? (std::min)(6 - rest, rest) : 0;
    for (size_t k = 0; k < pairs; k++) {
        slots.push_back("c" + std::to_string(2 + k));
        slots.push_back("s" + std::to_string(2 + k));
    }
    for (size_t k = pairs; slots.size() < 6; k++) slots.push_back("c" + std::to_string(2 + k % rest));
    std::string spec = "mode=mask;ppu:main_thread=c0;fifo=c1;spu=";
    for (size_t i = 0; i < slots.size(); i++) spec += (i ? "," : "") + slots[i];
    const char* ns = std::getenv("GOWA_NATIVE_SCHED");
    if (ns && *ns && *ns != '0') {
        const char* ap = std::getenv("GOWA_NATIVE_SCHED_PLACE");
        spec += ap ? std::string(",") + ap : c.cores[0].size() > 1 && c.cores[1].size() > 1 ? std::string(",s0,s1") : std::string(",c0,c1");
    }
    return spec;
}

const Config& config() {
    static const Config c = [] {
        Config c;
        c.cores = read_cores();
        c.cpuset_ids = read_cpuset_ids();
        const char* e = std::getenv("GOWA_PLACE");
        std::string spec = e ? e : default_spec(c);
        if (spec.empty() || spec == "0" || spec == "off") return c;
        size_t pos = 0;
        while (pos < spec.size()) {
            size_t end = spec.find(';', pos);
            if (end == std::string::npos) end = spec.size();
            std::string item = spec.substr(pos, end - pos);
            pos = end + 1;
            size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            std::string k = item.substr(0, eq), v = item.substr(eq + 1);
            if (k == "mode") {
                c.mode = v == "ideal" ? Mode::IDEAL : v == "cpuset" ? Mode::CPUSET : v == "mask" ? Mode::MASK : Mode::OFF;
            } else if (k == "spu") {
                size_t p = 0;
                for (int slot = 0; p <= v.size(); slot++) {
                    size_t q = v.find(',', p);
                    if (q == std::string::npos) q = v.size();
                    c.rules["spu" + std::to_string(slot)] = parse_rule(c, v.substr(p, q - p));
                    p = q + 1;
                }
            } else {
                c.rules[k] = parse_rule(c, v);
            }
        }
        std::string topo;
        for (const auto& core : c.cores) {
            topo += " [";
            for (size_t i = 0; i < core.size(); i++) topo += (i ? "," : "") + std::to_string(core[i]);
            topo += "]";
        }
        LOG_INFO("place", "thread placement '%s', cores%s", spec.c_str(), topo.c_str());
        return c;
    }();
    return c;
}

}

void apply(const std::string& role) {
    const Config& c = config();
    if (c.mode == Mode::OFF && c.rules.empty()) return;
    auto it = c.rules.find(role);
    if (it == c.rules.end()) it = c.rules.find(role.substr(0, role.find(':')));
    if (it == c.rules.end()) return;
    const Rule& r = it->second;
    HANDLE h = GetCurrentThread();
    bool ok = true;
    if (!r.lps.empty()) {
        if (c.mode == Mode::IDEAL) {
            PROCESSOR_NUMBER pn{};
            pn.Number = static_cast<BYTE>(r.lps[0]);
            ok = SetThreadIdealProcessorEx(h, &pn, nullptr) != 0;
        } else if (c.mode == Mode::CPUSET) {
            std::vector<ULONG> ids;
            for (int lp : r.lps) {
                auto f = c.cpuset_ids.find(lp);
                if (f != c.cpuset_ids.end()) ids.push_back(f->second);
            }
            ok = !ids.empty() && SetThreadSelectedCpuSets(h, ids.data(), static_cast<ULONG>(ids.size()));
        } else if (c.mode == Mode::MASK) {
            DWORD_PTR m = 0;
            for (int lp : r.lps) m |= DWORD_PTR(1) << lp;
            ok = SetThreadAffinityMask(h, m) != 0;
        }
    }
    if (r.has_prio) ok = SetThreadPriority(h, r.prio) && ok;
    std::string lps;
    for (int lp : r.lps) lps += " " + std::to_string(lp);
    LOG_INFO("place", "%s ->%s%s%s", role.c_str(), lps.c_str(), r.has_prio ? " prio " : "", r.has_prio ? std::to_string(r.prio).c_str() : "");
    if (!ok) LOG_WARN("place", "%s placement failed (%lu)", role.c_str(), GetLastError());
}

std::vector<int> role_lps(const std::string& role) {
    const Config& c = config();
    auto it = c.rules.find(role);
    if (it == c.rules.end()) return {};
    return it->second.lps;
}

int logical_cpus() {
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) return 0;
    int n = 0;
    for (DWORD_PTR m = proc; m; m &= m - 1) n++;
    return n;
}

}
