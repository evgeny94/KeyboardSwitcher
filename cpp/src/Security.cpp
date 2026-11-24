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

static std::string ClassOf(HWND hwnd) {
    wchar_t cls[64] = {};
    if (hwnd) GetClassNameW(hwnd, cls, _countof(cls));
    std::string s;
    for (wchar_t* p = cls; *p; ++p) s += (*p < 128) ? (char)*p : '?';
    return s;
}

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

// Read MSAA state/role of (acc, child).  Returns true if PROTECTED.
static bool AccIsProtected(IAccessible* acc, VARIANT child, long* roleOut) {
    bool isProtected = false;
    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(acc->get_accState(child, &v)) && v.vt == VT_I4)
        isProtected = (v.lVal & STATE_SYSTEM_PROTECTED) != 0;
    VariantClear(&v);
    if (roleOut) {
        *roleOut = 0;
        VariantInit(&v);
        if (SUCCEEDED(acc->get_accRole(child, &v)) && v.vt == VT_I4)
            *roleOut = v.lVal;
        VariantClear(&v);
    }
    return isProtected;
}

static std::string Describe(const char* src, HWND hwnd, long role, bool secure) {
    return std::string(src) + " cls=" + ClassOf(hwnd) +
           " role=" + std::to_string(role) +
           " secure=" + (secure ? "1" : "0");
}

std::string OnFocusEvent(HWND hwnd, LONG idObject, LONG idChild) {
    if (!ProtectSecureFields.load()) { SecureFieldFocused.store(false); return "disabled"; }
    if (IsWin32PasswordEdit(hwnd)) {
        SecureFieldFocused.store(true);
        return Describe("focus-event/win32", hwnd, 0, true);
    }
    bool secure = false;
    long role = 0;
    IAccessible* acc = nullptr;
    VARIANT child;
    VariantInit(&child);
    if (hwnd && SUCCEEDED(AccessibleObjectFromEvent(hwnd, idObject, idChild, &acc, &child)) && acc) {
        secure = AccIsProtected(acc, child, &role);
        acc->Release();
    }
    VariantClear(&child);
    SecureFieldFocused.store(secure);
    return Describe("focus-event/msaa", hwnd, role, secure);
}

std::string RefreshFocus() {
    if (!ProtectSecureFields.load()) { SecureFieldFocused.store(false); return "disabled"; }

    HWND fg = GetForegroundWindow();
    HWND focus = fg;
    GUITHREADINFO gti = { sizeof(gti) };
    DWORD tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    if (tid && GetGUIThreadInfo(tid, &gti) && gti.hwndFocus)
        focus = gti.hwndFocus;
    if (!focus) { SecureFieldFocused.store(false); return "refresh no-focus"; }

    if (IsWin32PasswordEdit(focus)) {
        SecureFieldFocused.store(true);
        return Describe("refresh/win32", focus, 0, true);
    }

    // MSAA: walk accFocus from the focus window's client object down to
    // the focused leaf (browsers report the focused DOM node this way).
    bool secure = false;
    long role = 0;
    IAccessible* acc = nullptr;
    if (SUCCEEDED(AccessibleObjectFromWindow(focus, (DWORD)OBJID_CLIENT,
                                             IID_IAccessible, (void**)&acc)) && acc) {
        for (int depth = 0; depth < 16 && acc; ++depth) {
            VARIANT f;
            VariantInit(&f);
            if (FAILED(acc->get_accFocus(&f)) || f.vt == VT_EMPTY) {
                VARIANT self; VariantInit(&self); self.vt = VT_I4; self.lVal = CHILDID_SELF;
                secure = AccIsProtected(acc, self, &role);
                VariantClear(&f);
                break;
            }
            if (f.vt == VT_DISPATCH && f.pdispVal) {
                IAccessible* next = nullptr;
                f.pdispVal->QueryInterface(IID_IAccessible, (void**)&next);
                VariantClear(&f);
                if (!next || next == acc) { if (next) next->Release(); break; }
                acc->Release();
                acc = next;
                continue;
            }
            if (f.vt == VT_I4) {
                secure = AccIsProtected(acc, f, &role);   // child id (or SELF)
                VariantClear(&f);
                break;
            }
            VariantClear(&f);
            break;
        }
        if (acc) acc->Release();
    }
    SecureFieldFocused.store(secure);
    return Describe("refresh/msaa", focus, role, secure);
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
