#include "LangAvail.h"

#include <atomic>
#include <vector>
#include <nlohmann/json.hpp>

namespace LangAvail {

const char* const kLangs[kNumLangs] = { "en", "he", "ru" };
static const WORD kPrimary[kNumLangs] = { LANG_ENGLISH, LANG_HEBREW, LANG_RUSSIAN };
static const LANGID kPreferred[kNumLangs] = { 0x0409, 0x040D, 0x0419 };

// Bit i  = language kLangs[i]
static std::atomic<unsigned> g_installed{0x7};   // optimistic until first Refresh
static std::atomic<unsigned> g_userLangs{0x7};
// Bit (from*3 + to)
static std::atomic<unsigned> g_userDirs{0x1FF};
static std::atomic<bool>     g_autoExclude{true};
static std::atomic<DWORD>    g_lastRefresh{0};
static std::atomic<bool>     g_refreshed{false};
static std::atomic<bool>     g_frozen{false};      // OverrideInstalled() (preview)

int IndexOf(const std::string& lang) {
    for (int i = 0; i < kNumLangs; ++i)
        if (lang == kLangs[i]) return i;
    return -1;
}

static std::vector<HKL> InstalledLayouts() {
    int n = GetKeyboardLayoutList(0, nullptr);
    std::vector<HKL> v(n > 0 ? n : 0);
    if (n > 0) v.resize(GetKeyboardLayoutList(n, v.data()));
    return v;
}

void Refresh() {
    if (g_frozen.load()) return;
    unsigned mask = 0;
    for (HKL h : InstalledLayouts()) {
        WORD prim = PRIMARYLANGID(LOWORD(reinterpret_cast<DWORD_PTR>(h)));
        for (int i = 0; i < kNumLangs; ++i)
            if (prim == kPrimary[i]) mask |= 1u << i;
    }
    // An empty list means the call failed, not that nothing is installed.
    if (mask != 0) g_installed.store(mask);
    g_lastRefresh.store(GetTickCount());
    g_refreshed.store(true);
}

void OverrideInstalled(unsigned mask) {
    g_installed.store(mask);
    g_frozen.store(true);
}

static void MaybeRefresh() {
    if (g_frozen.load()) return;
    if (!g_refreshed.load() || GetTickCount() - g_lastRefresh.load() > 2000)
        Refresh();
}

bool IsInstalled(const std::string& lang) {
    int i = IndexOf(lang);
    if (i < 0) return false;
    MaybeRefresh();
    return (g_installed.load() >> i) & 1u;
}

bool IsLangUsable(const std::string& lang) {
    int i = IndexOf(lang);
    if (i < 0) return false;
    if (!((g_userLangs.load() >> i) & 1u)) return false;
    return !g_autoExclude.load() || IsInstalled(lang);
}

bool IsDirectionUsable(const std::string& from, const std::string& to) {
    int f = IndexOf(from), t = IndexOf(to);
    if (f < 0 || t < 0 || f == t) return false;
    if (!((g_userDirs.load() >> (f * kNumLangs + t)) & 1u)) return false;
    return IsLangUsable(from) && IsLangUsable(to);
}

std::set<std::string> ExcludedTargets(const std::string& currentLang) {
    std::set<std::string> out;
    for (const char* l : kLangs)
        if (currentLang != l && !IsDirectionUsable(currentLang, l))
            out.insert(l);
    return out;
}

HKL InstalledHKL(const std::string& lang) {
    int i = IndexOf(lang);
    if (i < 0) return nullptr;
    HKL samePrimary = nullptr;
    for (HKL h : InstalledLayouts()) {
        LANGID id = LOWORD(reinterpret_cast<DWORD_PTR>(h));
        if (id == kPreferred[i]) return h;
        if (!samePrimary && PRIMARYLANGID(id) == kPrimary[i]) samePrimary = h;
    }
    return samePrimary;
}

// ── User preferences ─────────────────────────────────────────

static void SetBit(std::atomic<unsigned>& a, int bit, bool on) {
    unsigned m = 1u << bit;
    if (on) a.fetch_or(m); else a.fetch_and(~m);
}

bool GetAutoExclude()          { return g_autoExclude.load(); }
void SetAutoExclude(bool on)   { g_autoExclude.store(on); }

bool GetLangEnabled(const std::string& lang) {
    int i = IndexOf(lang);
    return i >= 0 && ((g_userLangs.load() >> i) & 1u);
}
void SetLangEnabled(const std::string& lang, bool on) {
    int i = IndexOf(lang);
    if (i >= 0) SetBit(g_userLangs, i, on);
}

bool GetDirectionEnabled(const std::string& from, const std::string& to) {
    int f = IndexOf(from), t = IndexOf(to);
    return f >= 0 && t >= 0 && ((g_userDirs.load() >> (f * kNumLangs + t)) & 1u);
}
void SetDirectionEnabled(const std::string& from, const std::string& to, bool on) {
    int f = IndexOf(from), t = IndexOf(to);
    if (f >= 0 && t >= 0 && f != t) SetBit(g_userDirs, f * kNumLangs + t, on);
}

static std::string DirKey(int f, int t) {
    return std::string(kLangs[f]) + "->" + kLangs[t];
}

// A missing or wrong-typed value means "enabled" (the default).
static bool BoolOr(const nlohmann::json& obj, const std::string& key, bool def) {
    auto it = obj.find(key);
    return (it != obj.end() && it->is_boolean()) ? it->get<bool>() : def;
}

void FromJson(const nlohmann::json& j) {
    if (!j.is_object()) return;
    g_autoExclude.store(BoolOr(j, "auto_exclude_missing_layouts", true));
    auto langs = j.find("languages");
    if (langs != j.end() && langs->is_object())
        for (int i = 0; i < kNumLangs; ++i)
            SetBit(g_userLangs, i, BoolOr(*langs, kLangs[i], true));
    auto dirs = j.find("directions");
    if (dirs != j.end() && dirs->is_object())
        for (int f = 0; f < kNumLangs; ++f)
            for (int t = 0; t < kNumLangs; ++t)
                if (f != t) SetBit(g_userDirs, f * kNumLangs + t, BoolOr(*dirs, DirKey(f, t), true));
}

void ToJson(nlohmann::json& j) {
    j["auto_exclude_missing_layouts"] = g_autoExclude.load();
    nlohmann::json langs = nlohmann::json::object();
    for (int i = 0; i < kNumLangs; ++i)
        langs[kLangs[i]] = GetLangEnabled(kLangs[i]);
    j["languages"] = langs;
    nlohmann::json dirs = nlohmann::json::object();
    for (int f = 0; f < kNumLangs; ++f)
        for (int t = 0; t < kNumLangs; ++t)
            if (f != t) dirs[DirKey(f, t)] = GetDirectionEnabled(kLangs[f], kLangs[t]);
    j["directions"] = dirs;
}

} // namespace LangAvail
