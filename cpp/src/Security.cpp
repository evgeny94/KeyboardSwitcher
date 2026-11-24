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
#include <thread>
#include <mutex>
#include <uiautomation.h>
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
        RequestFocusCheck();
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
    // Fast path only ever turns protection ON (no latency before the
    // first keystroke).  Turning it OFF is left to the UIA worker, so
    // stray events (e.g. the shell's ForegroundStaging window) can't
    // clear it while a password field still has focus.
    if (secure) SecureFieldFocused.store(true);
    RequestFocusCheck();
    return Describe("focus-event/msaa", hwnd, role, secure);
}

// ── UI Automation focus worker ───────────────────────────────
// UIA reliably reports IsPassword for browsers (Chrome/Edge/Firefox),
// Electron, WPF, UWP/WinUI and Win32.  Queries are cross-process and can
// block on a hung app, so they run on a dedicated MTA thread — never on
// the keyboard-hook thread.

// GUIDs defined locally (avoids depending on toolchain uuid libs).
static const CLSID kCLSID_CUIAutomation =
    {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
static const IID kIID_IUIAutomation =
    {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};

static std::thread       g_worker;
static HANDLE            g_wakeEvent  = nullptr;
static std::atomic<bool> g_stop{false};
static HWND              g_notifyHwnd = nullptr;
static UINT              g_notifyMsg  = 0;
static std::mutex        g_descMutex;
static std::string       g_lastDesc;

static std::string Narrow(const wchar_t* w) {
    std::string s;
    if (w) for (const wchar_t* p = w; *p; ++p) s += (*p < 128) ? (char)*p : '?';
    return s;
}

// Win32 fallback: focused HWND of the foreground thread.
static bool Win32FocusIsPassword(std::string& cls) {
    HWND fg = GetForegroundWindow();
    GUITHREADINFO gti = { sizeof(gti) };
    DWORD tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    HWND focus = (tid && GetGUIThreadInfo(tid, &gti) && gti.hwndFocus) ? gti.hwndFocus : fg;
    cls = ClassOf(focus);
    return IsWin32PasswordEdit(focus);
}

static void WorkerMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    CoCreateInstance(kCLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                     kIID_IUIAutomation, (void**)&uia);

    while (!g_stop.load()) {
        if (WaitForSingleObject(g_wakeEvent, INFINITE) != WAIT_OBJECT_0) break;
        if (g_stop.load()) break;
        Sleep(30);                    // debounce bursts of focus events
        ResetEvent(g_wakeEvent);      // coalesce requests made meanwhile

        std::string desc;
        bool secure = false;
        if (!ProtectSecureFields.load()) {
            desc = "disabled";
        } else {
            bool decided = false;
            if (uia) {
                IUIAutomationElement* el = nullptr;
                if (SUCCEEDED(uia->GetFocusedElement(&el)) && el) {
                    BOOL isPwd = FALSE;
                    CONTROLTYPEID ct = 0;
                    BSTR cls = nullptr;
                    int pid = 0;
                    if (SUCCEEDED(el->get_CurrentIsPassword(&isPwd))) decided = true;
                    el->get_CurrentControlType(&ct);
                    el->get_CurrentClassName(&cls);
                    el->get_CurrentProcessId(&pid);
                    secure = decided && isPwd;
                    desc = "uia ctrl=" + std::to_string(ct) +
                           " cls=" + Narrow(cls) +
                           " pwd=" + (isPwd ? "1" : "0");
                    if (cls) SysFreeString(cls);
                    el->Release();
                }
            }
            std::string wcls;
            if (Win32FocusIsPassword(wcls)) {      // belt and braces
                secure = true; decided = true;
                desc += " win32-pwd cls=" + wcls;
            }
            if (!decided) desc = "uia-unavailable cls=" + wcls;
        }
        SecureFieldFocused.store(secure);
        {
            std::lock_guard<std::mutex> lk(g_descMutex);
            g_lastDesc = desc + " secure=" + (secure ? "1" : "0");
        }
        if (g_notifyHwnd) PostMessageW(g_notifyHwnd, g_notifyMsg, 0, 0);
    }
    if (uia) uia->Release();
    CoUninitialize();
}

void StartFocusWorker(HWND notifyHwnd, UINT notifyMsg) {
    if (g_worker.joinable()) return;
    g_notifyHwnd = notifyHwnd;
    g_notifyMsg  = notifyMsg;
    g_wakeEvent  = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual reset
    g_stop.store(false);
    g_worker = std::thread(WorkerMain);
}

void RequestFocusCheck() {
    if (g_wakeEvent) SetEvent(g_wakeEvent);
}

void StopFocusWorker() {
    if (!g_worker.joinable()) return;
    g_stop.store(true);
    SetEvent(g_wakeEvent);
    // Don't hang shutdown on a UIA call stuck in a hung app.
    if (WaitForSingleObject(g_worker.native_handle(), 2000) == WAIT_OBJECT_0)
        g_worker.join();
    else
        g_worker.detach();
}

std::string LastFocusCheck() {
    std::lock_guard<std::mutex> lk(g_descMutex);
    return g_lastDesc;
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
