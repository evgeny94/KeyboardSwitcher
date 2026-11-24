#pragma once

#include <string>
#include <atomic>
#include <windows.h>

// ============================================================
// Security — privacy / hardening settings and helpers.
//
// Secure-by-default: every option ships in its most restrictive
// state.  Power users / developers can relax individual options
// from the tray "Privacy & Security" submenu or by editing
//   %APPDATA%\KeyboardSwitcher\settings.json
// (kept separate from user_prefs.json so it survives version
// upgrades — user_prefs.json is discarded on every new version).
//
// settings.json keys (all optional, defaults shown):
//   "protect_secure_fields":    true   // never read keys in password fields
//   "redact_debug_log":         true   // debug log shows <len=N>, not text
//   "check_updates_on_startup": true   // silent GitHub version check
//   "require_verified_updates": true   // SHA-256 must match GitHub digest
//                                      // (JSON-only: not exposed in tray)
// ============================================================
namespace Security {

    // ── Settings ──────────────────────────────────────────────
    extern std::atomic<bool> ProtectSecureFields;
    extern std::atomic<bool> RedactDebugLog;
    extern std::atomic<bool> CheckUpdatesOnStartup;
    extern std::atomic<bool> RequireVerifiedUpdates;

    // Load settings.json (missing / corrupt file → secure defaults).
    void LoadSettings();
    // Persist current settings to settings.json.
    void SaveSettings();

    // ── Secure-field detection ────────────────────────────────
    // True while keyboard focus is on a password / protected field.
    // Updated from WinEvent focus notifications (UI thread).
    extern std::atomic<bool> SecureFieldFocused;

    // Evaluate the object named by an EVENT_OBJECT_FOCUS WinEvent.
    // Returns a short diagnostic (class / role / result — never text).
    std::string OnFocusEvent(HWND hwnd, LONG idObject, LONG idChild);

    // Background UI Automation worker that re-evaluates whatever has
    // keyboard focus (UIA IsPassword + Win32 ES_PASSWORD).  It is the
    // only place that turns protection OFF.  After each check it posts
    // |notifyMsg| to |notifyHwnd|; LastFocusCheck() describes the result.
    void StartFocusWorker(HWND notifyHwnd, UINT notifyMsg);
    void RequestFocusCheck();          // cheap; safe from any thread
    void StopFocusWorker();
    std::string LastFocusCheck();

    // ── Update verification helpers ───────────────────────────
    // Hosts the updater may talk to (exact match, HTTPS only).
    bool IsAllowedUpdateHost(const std::wstring& host);

    // Compute lowercase hex SHA-256 of a file.  Empty on error.
    std::string Sha256File(const std::wstring& path);
}
