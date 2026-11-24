#ifndef UNICODE
#define UNICODE
#endif

#include "Security.h"
#include "FeedbackLogger.h"

#include <windows.h>
#include <oleacc.h>
#include <bcrypt.h>
#include <fstream>
#include <vector>
#include <nlohmann/json.hpp>

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

using json = nlohmann::json;

namespace Security {

std::atomic<bool> ProtectSecureFields{true};
std::atomic<bool> RedactDebugLog{true};
std::atomic<bool> CheckUpdatesOnStartup{true};
std::atomic<bool> RequireVerifiedUpdates{true};
std::atomic<bool> SecureFieldFocused{false};

// ── settings.json ────────────────────────────────────────────

static std::wstring SettingsPath() {
    std::wstring dir = Feedback::GetDataDir();
    if (dir.empty()) return L"";
    return dir + L"\\settings.json";
}

void LoadSettings() {
    std::wstring path = SettingsPath();
    if (path.empty()) return;
    std::ifstream in(path.c_str());
    if (!in.is_open()) { SaveSettings(); return; }  // write defaults once
    try {
        json j = json::parse(in);
        ProtectSecureFields.store(j.value("protect_secure_fields", true));
        RedactDebugLog.store(j.value("redact_debug_log", true));
        CheckUpdatesOnStartup.store(j.value("check_updates_on_startup", true));
        RequireVerifiedUpdates.store(j.value("require_verified_updates", true));
    } catch (...) {
        // Corrupt file — keep secure defaults
    }
}

void SaveSettings() {
    std::wstring path = SettingsPath();
    if (path.empty()) return;
    json j;
    j["protect_secure_fields"]    = ProtectSecureFields.load();
    j["redact_debug_log"]         = RedactDebugLog.load();
    j["check_updates_on_startup"] = CheckUpdatesOnStartup.load();
    j["require_verified_updates"] = RequireVerifiedUpdates.load();
    std::ofstream out(path.c_str(), std::ios::trunc);
    if (out.is_open()) out << j.dump(2);
}

// ── Secure-field detection ───────────────────────────────────

// Classic Win32 edit control with ES_PASSWORD.
static bool IsWin32PasswordEdit(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, _countof(cls));
    // "Edit", "RichEdit20W", "RICHEDIT50W", WinForms "WindowsForms10.EDIT..."
    if (!wcsstr(cls, L"Edit") && !wcsstr(cls, L"EDIT")) return false;
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    return (style & ES_PASSWORD) != 0;
}

// MSAA: browsers (Chrome/Edge/Firefox), Electron, WPF, WinForms and
// most UI frameworks expose password inputs with STATE_SYSTEM_PROTECTED.
static bool IsProtectedAccessible(HWND hwnd, LONG idObject, LONG idChild) {
    if (!hwnd) return false;
    IAccessible* acc = nullptr;
    VARIANT child;
    VariantInit(&child);
    if (FAILED(AccessibleObjectFromEvent(hwnd, idObject, idChild, &acc, &child)) || !acc)
        return false;
    bool isProtected = false;
    VARIANT state;
    VariantInit(&state);
    if (SUCCEEDED(acc->get_accState(child, &state)) && state.vt == VT_I4) {
        isProtected = (state.lVal & STATE_SYSTEM_PROTECTED) != 0;
    }
    VariantClear(&state);
    VariantClear(&child);
    acc->Release();
    return isProtected;
}

void OnFocusEvent(HWND hwnd, LONG idObject, LONG idChild) {
    if (!ProtectSecureFields.load()) {
        SecureFieldFocused.store(false);
        return;
    }
    bool secure = false;

    // Win32 focus HWND of the foreground thread.
    GUITHREADINFO gti = { sizeof(gti) };
    HWND fg = GetForegroundWindow();
    DWORD tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    if (tid && GetGUIThreadInfo(tid, &gti)) {
        secure = IsWin32PasswordEdit(gti.hwndFocus);
    }

    // Accessible object that just received focus.
    if (!secure && hwnd) {
        secure = IsProtectedAccessible(hwnd, idObject, idChild);
    }
    SecureFieldFocused.store(secure);
}

// ── Update verification ──────────────────────────────────────

bool IsAllowedUpdateHost(const std::wstring& host) {
    static const wchar_t* kAllowed[] = {
        L"api.github.com",
        L"github.com",
        L"objects.githubusercontent.com",
        L"release-assets.githubusercontent.com",
    };
    for (auto* h : kAllowed) {
        if (_wcsicmp(host.c_str(), h) == 0) return true;
    }
    return false;
}

std::string Sha256File(const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.is_open()) return "";

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        return "";
    if (!NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return "";
    }

    bool ok = true;
    std::vector<char> buf(64 * 1024);
    while (in) {
        in.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize n = in.gcount();
        if (n > 0 && !NT_SUCCESS(BCryptHashData(hHash, (PUCHAR)buf.data(), (ULONG)n, 0))) {
            ok = false;
            break;
        }
    }

    unsigned char digest[32] = {};
    if (ok && !NT_SUCCESS(BCryptFinishHash(hHash, digest, sizeof(digest), 0)))
        ok = false;
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (!ok) return "";

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char b : digest) {
        out += hex[b >> 4];
        out += hex[b & 0xF];
    }
    return out;
}

} // namespace Security
