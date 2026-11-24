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
    // Diagnostics that survive restarts (both default off):
    //   "debug_log":        write ks_debug.log from startup
    //   "collect_feedback": append outcomes to feedback.jsonl
    extern std::atomic<bool> DebugLogOnStartup;
    extern std::atomic<bool> CollectFeedback;

    // Load settings.json (missing / corrupt file → secure defaults).
    void LoadSettings();
    // Persist current settings to settings.json.
    void SaveSettings();

    // ── Secure-field detection ────────────────────────────────
    // Verdict of the last focus check (written by the focus worker).
    extern std::atomic<bool> SecureFieldFocused;

    // True while keystrokes must be ignored: protection is on AND either
    // a password field has focus or a focus check is still pending (fail
    // closed between a focus change and the worker's verdict).
    bool IsSecureFieldActive();

    // EVENT_OBJECT_FOCUS: remember the target and schedule a check.
    // Cheap, no cross-process calls (safe on the hook thread).
    void OnFocusEvent(HWND hwnd, LONG idObject, LONG idChild);

    // Background worker (MTA) that evaluates the focused element with
    // UIA IsPassword, MSAA STATE_SYSTEM_PROTECTED and Win32 ES_PASSWORD.
    // When the verdict changes it posts |notifyMsg| (wParam = 1 secure,
    // 0 not) to |notifyHwnd|.
    void StartFocusWorker(HWND notifyHwnd, UINT notifyMsg);
    void RequestFocusCheck();          // cheap; safe from any thread
    void StopFocusWorker();

    // ── Update verification helpers ───────────────────────────
    // Hosts the updater may talk to (exact match, HTTPS only).
    bool IsAllowedUpdateHost(const std::wstring& host);

    // Compute lowercase hex SHA-256 of a file.  Empty on error.
    std::string Sha256File(const std::wstring& path);
}
