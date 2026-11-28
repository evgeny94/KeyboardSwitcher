#pragma once

#include <functional>
#include <string>
#include <vector>
#include <windows.h>

// ============================================================
// SettingsFlyout — detection settings popup (left-click on tray).
//
// Custom-drawn with Direct2D / DirectWrite (system DLLs only):
// follows the system light/dark theme and accent colour, uses Win11
// solid background with raised cards, Win11 rounded corners via DWM, Segoe UI
// Variable, and per-monitor-v2 DPI scaling (set per thread, so the rest
// of the process keeps its DPI mode).
//
// Edits the same values as before: global short-text confidence
// (ConfidenceAtMinChars), per-direction min chars and the USER BASE
// confidence floor (calibration delta stays invisible), plus the
// language / direction toggles from LangAvail.
// ============================================================
//
// The gear at the top right switches to the "Settings" page: the general
// on/off options (the same list the tray right-click menu shows), each
// with a short explanation.
// ============================================================
namespace SettingsFlyout {

    // One on/off option.  main.cpp owns the list; the tray menu and the
    // flyout's Settings page both read and change options through it, so
    // the two can never disagree.
    struct Option {
        UINT                              id;       // tray-menu command id
        const wchar_t*                    group;    // section heading
        const wchar_t*                    title;    // short name (also the menu text)
        const wchar_t*                    desc;     // one-two line explanation
        std::function<bool()>             get;
        std::function<void(bool, HWND)>   set;      // owner HWND for any confirm box
        std::function<bool()>             enabled;  // optional: greyed when false
    };
    void SetOptions(std::vector<Option> opts);
    const std::vector<Option>& Options();
    // Footer button on the Settings page.
    void SetOpenDataFolder(std::function<void()> fn);

    enum class Page { Detection, Settings };

    // Open the flyout above the cursor, or close it if already open.
    void Toggle(HWND owner);
    // Open (or switch an open flyout to) a given page.
    void Show(HWND owner, Page page);

    // Smallest EarlyDetectionMinChars across all pairs → DefaultParams
    // (fast-path gate in the keyboard hook).
    void SyncGlobalEarlyOut();

    // Developer / CI helpers (command line, see WinMain):
    //   --render-flyout <out.bmp> [--theme light|dark] [--page settings]
    //                   [--assume-installed en,he,ru]
    // renders the flyout off-screen to a BMP; --flyout-demo shows the live
    // window without starting the switcher.  Neither persists any change.
    bool RenderToBitmapFile(const std::wstring& path, bool dark, float scale,
                            Page page = Page::Detection);
    void RunDemo();
}
