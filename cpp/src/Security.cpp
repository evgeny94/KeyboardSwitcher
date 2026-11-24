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
        // Per key: a missing or wrong-typed value keeps the secure default.
        auto flag = [&](const char* k, std::atomic<bool>& dst) {
            auto it = j.find(k);
            if (it != j.end() && it->is_boolean()) dst.store(it->get<bool>());
        };
        flag("protect_secure_fields",    ProtectSecureFields);
        flag("redact_debug_log",         RedactDebugLog);
        flag("check_updates_on_startup", CheckUpdatesOnStartup);
        flag("require_verified_updates", RequireVerifiedUpdates);
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
    // Write a temp file and swap it in: a crash never leaves a truncated
    // settings.json (which would silently reset everything to defaults).
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp.c_str(), std::ios::trunc);
        if (!out.is_open()) return;
        out << j.dump(2);
        if (!out.good()) return;
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// ── Secure-field detection ───────────────────────────────────
//
// All cross-process queries (UIA and MSAA) run on a dedicated MTA worker
// thread, never on the message-loop thread that also services the
// low-level keyboard / mouse hooks (a hung target app must not be able
// to stall input system-wide).  To keep zero latency, every focus change
// opens a "check pending" window during which the field is treated as
// protected (fail closed); the worker closes it with its verdict.  A
// generation counter guarantees that a verdict computed before the
// latest focus change can never switch protection off.

// Classic Win32 edit controls with ES_PASSWORD.  Exact class names: the
// ES_PASSWORD bit (0x20) is only meaningful for edit-type classes.
static bool IsWin32PasswordEdit(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t cls[64] = {};
    if (!GetClassNameW(hwnd, cls, _countof(cls))) return false;
    static const wchar_t* kEdit[] = { L"Edit", L"RichEdit20A", L"RichEdit20W",
                                      L"RICHEDIT50W", L"TEdit" };
    bool isEdit = _wcsnicmp(cls, L"WindowsForms10.EDIT", 19) == 0;
    for (auto* e : kEdit) isEdit = isEdit || _wcsicmp(cls, e) == 0;
    return isEdit && (GetWindowLongW(hwnd, GWL_STYLE) & ES_PASSWORD) != 0;
}

// GUIDs defined locally (avoids depending on toolchain uuid libs).
static const CLSID kCLSID_CUIAutomation =
    {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
static const IID kIID_IUIAutomation =
    {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
static const IID kIID_IUIAutomation2 =
    {0x34723aff, 0x0c9d, 0x49d0, {0x98, 0x96, 0x7a, 0xb5, 0x2d, 0xf8, 0xcd, 0x8a}};

static std::thread           g_worker;
static HANDLE                g_wakeEvent  = nullptr;
static HANDLE                g_doneEvent  = nullptr;   // worker exited (MinGW's std::thread has no HANDLE)
static std::atomic<bool>     g_stop{false};
static HWND                  g_notifyHwnd = nullptr;
static UINT                  g_notifyMsg  = 0;
static std::atomic<unsigned> g_requested{0};   // bumped on every focus change
static std::atomic<unsigned> g_checked{0};     // generation the last verdict covers

// Latest EVENT_OBJECT_FOCUS target, for the worker's MSAA check.
static std::mutex g_evMutex;
static HWND       g_evHwnd   = nullptr;
static LONG       g_evObject = 0;
static LONG       g_evChild  = 0;

bool IsSecureFieldActive() {
    if (!ProtectSecureFields.load()) return false;
    return SecureFieldFocused.load() || g_checked.load() != g_requested.load();
}

void RequestFocusCheck() {
    if (!g_wakeEvent) return;
    g_requested.fetch_add(1);
    SetEvent(g_wakeEvent);
}

void OnFocusEvent(HWND hwnd, LONG idObject, LONG idChild) {
    {
        std::lock_guard<std::mutex> lk(g_evMutex);
        g_evHwnd = hwnd; g_evObject = idObject; g_evChild = idChild;
    }
    RequestFocusCheck();
}

// Verdicts: 1 = password, 0 = not a password, -1 = could not tell.

// MSAA: STATE_SYSTEM_PROTECTED on the object named by the focus event.
static int MsaaIsProtected() {
    HWND hwnd; LONG obj, child;
    {
        std::lock_guard<std::mutex> lk(g_evMutex);
        hwnd = g_evHwnd; obj = g_evObject; child = g_evChild;
    }
    if (!hwnd || !IsWindow(hwnd)) return -1;
    IAccessible* acc = nullptr;
    VARIANT vChild;
    VariantInit(&vChild);
    int result = -1;
    if (SUCCEEDED(AccessibleObjectFromEvent(hwnd, obj, child, &acc, &vChild)) && acc) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(acc->get_accState(vChild, &v)) && v.vt == VT_I4)
            result = (v.lVal & STATE_SYSTEM_PROTECTED) ? 1 : 0;
        VariantClear(&v);
        acc->Release();
    }
    VariantClear(&vChild);
    return result;
}

// UIA: IsPassword of the focused element.
static int UiaIsPassword(IUIAutomation* uia) {
    if (!uia) return -1;
    IUIAutomationElement* el = nullptr;
    int result = -1;
    if (SUCCEEDED(uia->GetFocusedElement(&el)) && el) {
        BOOL isPwd = FALSE;
        if (SUCCEEDED(el->get_CurrentIsPassword(&isPwd))) result = isPwd ? 1 : 0;
        el->Release();
    }
    return result;
}

// Win32: focused HWND of the foreground thread.
static bool Win32FocusIsPassword() {
    HWND fg = GetForegroundWindow();
    GUITHREADINFO gti = { sizeof(gti) };
    DWORD tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    HWND focus = (tid && GetGUIThreadInfo(tid, &gti) && gti.hwndFocus) ? gti.hwndFocus : fg;
    return IsWin32PasswordEdit(focus);
}

static void WorkerMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    CoCreateInstance(kCLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                     kIID_IUIAutomation, (void**)&uia);
    // Bound how long a hung target app can keep a check pending.
    IUIAutomation2* uia2 = nullptr;
    if (uia && SUCCEEDED(uia->QueryInterface(kIID_IUIAutomation2, (void**)&uia2)) && uia2) {
        uia2->put_ConnectionTimeout(1000);
        uia2->put_TransactionTimeout(1000);
        uia2->Release();
    }

    while (!g_stop.load()) {
        if (WaitForSingleObject(g_wakeEvent, INFINITE) != WAIT_OBJECT_0) break;
        if (g_stop.load()) break;
        Sleep(30);                    // debounce bursts of focus events
        ResetEvent(g_wakeEvent);      // coalesce requests made meanwhile
        const unsigned gen = g_requested.load();

        const bool prev = SecureFieldFocused.load();
        bool secure = prev;
        if (!ProtectSecureFields.load()) {
            secure = false;
        } else {
            int u = UiaIsPassword(uia);
            int m = MsaaIsProtected();
            bool w = Win32FocusIsPassword();
            if (u == 1 || m == 1 || w) secure = true;
            else if (u == 0 || m == 0) secure = false;
            // else nobody could tell: keep the previous verdict.
        }
        SecureFieldFocused.store(secure);
        g_checked.store(gen);         // a newer request keeps the check pending
        if (g_notifyHwnd && secure != prev)
            PostMessageW(g_notifyHwnd, g_notifyMsg, secure ? 1 : 0, 0);
    }
    if (uia) uia->Release();
    CoUninitialize();
    SetEvent(g_doneEvent);
}

void StartFocusWorker(HWND notifyHwnd, UINT notifyMsg) {
    if (g_worker.joinable()) return;
    g_notifyHwnd = notifyHwnd;
    g_notifyMsg  = notifyMsg;
    g_wakeEvent  = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual reset
    g_doneEvent  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_stop.store(false);
    g_worker = std::thread(WorkerMain);
}

void StopFocusWorker() {
    if (!g_worker.joinable()) return;
    g_stop.store(true);
    SetEvent(g_wakeEvent);
    // Don't hang shutdown on a UIA call stuck in a hung app.  On timeout
    // the thread is detached and the event handles are deliberately
    // leaked (it may still use them).  The worker touches only atomics,
    // those handles, g_evMutex and its own locals; the process is exiting anyway.
    if (WaitForSingleObject(g_doneEvent, 2000) == WAIT_OBJECT_0) {
        g_worker.join();
        CloseHandle(g_wakeEvent); g_wakeEvent = nullptr;
        CloseHandle(g_doneEvent); g_doneEvent = nullptr;
    } else {
        g_worker.detach();
    }
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
