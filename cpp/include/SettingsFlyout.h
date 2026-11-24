#pragma once

#include <string>
#include <windows.h>

// ============================================================
// SettingsFlyout — detection settings popup (left-click on tray).
//
// Custom-drawn with Direct2D / DirectWrite (system DLLs only):
// follows the system light/dark theme and accent colour, uses Win11
// rounded corners + acrylic backdrop via DWM where available, Segoe UI
// Variable, and per-monitor-v2 DPI scaling (set per thread, so the rest
// of the process keeps its DPI mode).
//
// Edits the same values as before: global short-text confidence
// (ConfidenceAtMinChars), per-direction min chars and the USER BASE
// confidence floor (calibration delta stays invisible), plus the
// language / direction toggles from LangAvail.
// ============================================================
namespace SettingsFlyout {

    // Open the flyout above the cursor, or close it if already open.
    void Toggle(HWND owner);

    // Smallest EarlyDetectionMinChars across all pairs → DefaultParams
    // (fast-path gate in the keyboard hook).
    void SyncGlobalEarlyOut();

    // Developer / CI helpers (command line, see WinMain):
    //   --render-flyout <out.bmp> [--theme light|dark] [--assume-installed en,he,ru]
    // renders the flyout off-screen to a BMP; --flyout-demo shows the live
    // window without starting the switcher.  Neither persists any change.
    bool RenderToBitmapFile(const std::wstring& path, bool dark, float scale);
    void RunDemo();
}
