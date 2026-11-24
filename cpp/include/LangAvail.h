#pragma once

#include <string>
#include <set>
#include <windows.h>
#include <nlohmann/json_fwd.hpp>

// ============================================================
// LangAvail — which languages / directions the detector may use.
//
// A language is USABLE when its keyboard layout is installed
// (GetKeyboardLayoutList) — unless the user turned off
// "auto_exclude_missing_layouts" — AND the user has not disabled it.
// A direction from→to is usable when both ends are usable and the
// user has not disabled that direction.
//
// Unusable targets are removed from detection entirely: no layout
// conversion variant is generated, and their softmax mass is dropped
// (scores renormalised) so they no longer dilute top-1 confidence,
// the top-1/top-2 margin, or the incumbent signal on the remaining
// pairs.  See TypoResilientDetect().
//
// All state is held in atomics: read from the keyboard hook and the
// correction thread, written from the UI thread.
//
// settings.json keys (all optional):
//   "auto_exclude_missing_layouts": true
//   "languages":  { "en": true, "he": true, "ru": true }
//   "directions": { "en->he": true, "he->en": true, ... }
// ============================================================
namespace LangAvail {

    constexpr int kNumLangs = 3;
    // Index order used throughout this module.
    extern const char* const kLangs[kNumLangs];   // "en", "he", "ru"

    int  IndexOf(const std::string& lang);         // -1 if unknown

    // Re-read installed layouts.  Cheap; called on focus change, from
    // the 60 s tray timer and lazily from IsUsable* (at most every 2 s).
    void Refresh();

    bool IsInstalled(const std::string& lang);
    bool IsLangUsable(const std::string& lang);
    bool IsDirectionUsable(const std::string& from, const std::string& to);

    // Languages that must NOT be considered as targets when typing on
    // |currentLang| (union of unusable languages and disabled directions).
    std::set<std::string> ExcludedTargets(const std::string& currentLang);

    // Installed HKL for |lang|, or nullptr when no such layout is installed.
    // Prefers an exact LANGID match, then the same primary language
    // (e.g. en-GB when en-US is absent).
    HKL InstalledHKL(const std::string& lang);

    // ── User preferences ───────────────────────────────────────
    bool GetAutoExclude();
    void SetAutoExclude(bool on);
    bool GetLangEnabled(const std::string& lang);
    void SetLangEnabled(const std::string& lang, bool on);
    bool GetDirectionEnabled(const std::string& from, const std::string& to);
    void SetDirectionEnabled(const std::string& from, const std::string& to, bool on);

    // settings.json (de)serialisation — called by Security::Load/SaveSettings.
    void FromJson(const nlohmann::json& j);
    void ToJson(nlohmann::json& j);
}
