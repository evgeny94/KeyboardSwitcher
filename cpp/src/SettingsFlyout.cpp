#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SettingsFlyout.h"
#include "Config.h"
#include "FeedbackLogger.h"
#include "LangAvail.h"
#include "Security.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace SettingsFlyout {

// ── Static data ──────────────────────────────────────────────

static const wchar_t kClass[] = L"KS_SettingsFlyout";

// Direction order (same as before the redesign).
static const char* const kFrom[6] = {"en","ru","en","he","ru","he"};
static const char* const kTo[6]   = {"ru","en","he","en","he","ru"};

// Factory defaults used by "Reset defaults" (mirror Config.cpp).
static constexpr float kFactoryShortConf    = 0.99f;
static constexpr int   kFactoryMinChars[6]  = {4, 4, 3, 4, 3, 4};
static constexpr float kFactoryFloor[6]     = {0.70f, 0.70f, 0.60f, 0.70f, 0.60f, 0.70f};

static constexpr float kConfMin = 0.50f, kConfMax = 1.00f;
static constexpr int   kCharsMin = 1, kCharsMax = 15;
static constexpr float kWidth = 400.0f;      // DIP

static const wchar_t* LangName(const std::string& l) {
    if (l == "en") return L"English";
    if (l == "he") return L"Hebrew";
    if (l == "ru") return L"Russian";
    return L"?";
}

// ── Theme ────────────────────────────────────────────────────

struct Theme {
    bool dark = false;
    D2D1_COLOR_F bg, card, cardStroke, text, text2, text3, ctrlFill, ctrlStroke,
                 ctrlHover, sep, accent, onAccent, thumb, track;
};

static D2D1_COLOR_F Rgb(UINT32 rgb, float a = 1.0f) {
    return D2D1::ColorF(rgb, a);
}

static DWORD RegDword(HKEY root, const wchar_t* key, const wchar_t* val, DWORD def) {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(root, key, val, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v;
    return def;
}

static bool SystemUsesDarkTheme() {
    return RegDword(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", 1) == 0;
}

// Accent as Windows 11 uses it on controls: a lighter shade in dark mode,
// a darker one in light mode (AccentPalette), falling back to the default blue.
static UINT32 AccentRgb(bool dark) {
    BYTE pal[32] = {};
    DWORD sz = sizeof(pal);
    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
            L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &sz) == ERROR_SUCCESS &&
        sz >= 32) {
        const BYTE* c = pal + (dark ? 1 : 4) * 4;            // Light2 / Dark1
        return (UINT32(c[0]) << 16) | (UINT32(c[1]) << 8) | c[2];
    }
    return dark ? 0x60CDFF : 0x005FB8;
}

// Mix |rgb| towards |base| (amount = share of rgb).
static UINT32 Blend(UINT32 base, UINT32 rgb, float amount) {
    auto ch = [&](int sh) {
        float b = (base >> sh) & 0xFF, c = (rgb >> sh) & 0xFF;
        return static_cast<UINT32>(b + (c - b) * amount + 0.5f) << sh;
    };
    return ch(16) | ch(8) | ch(0);
}

static Theme MakeTheme(bool dark) {
    Theme t;
    t.dark = dark;
    const UINT32 accent = AccentRgb(dark);
    t.accent = Rgb(accent);
    if (dark) {
        t.bg         = Rgb(Blend(0x1A1A1A, accent, 0.07f));   // accent-tinted, not flat grey
        t.card       = Rgb(0x2B2B2B);
        t.cardStroke = Rgb(0xFFFFFF, 0.08f);
        t.text       = Rgb(0xFFFFFF);
        t.text2      = Rgb(0xFFFFFF, 0.79f);
        t.text3      = Rgb(0xFFFFFF, 0.36f);
        t.ctrlFill   = Rgb(0xFFFFFF, 0.06f);
        t.ctrlHover  = Rgb(0xFFFFFF, 0.09f);
        t.ctrlStroke = Rgb(0xFFFFFF, 0.09f);
        t.sep        = Rgb(0xFFFFFF, 0.08f);
        t.onAccent   = Rgb(0x000000);
        t.thumb      = Rgb(0x454545);
        t.track      = Rgb(0xFFFFFF, 0.54f);
    } else {
        t.bg         = Rgb(Blend(0xF7F7F7, accent, 0.08f));   // accent-tinted, not flat grey
        t.card       = Rgb(0xFFFFFF);
        t.cardStroke = Rgb(0x000000, 0.08f);
        t.text       = Rgb(0x000000, 0.90f);
        t.text2      = Rgb(0x000000, 0.61f);
        t.text3      = Rgb(0x000000, 0.36f);
        t.ctrlFill   = Rgb(0xFFFFFF, 0.70f);
        t.ctrlHover  = Rgb(0xF9F9F9, 0.50f);
        t.ctrlStroke = Rgb(0x000000, 0.14f);
        t.sep        = Rgb(0x000000, 0.08f);
        t.onAccent   = Rgb(0xFFFFFF);
        t.thumb      = Rgb(0xFFFFFF);
        t.track      = Rgb(0x000000, 0.45f);
    }
    return t;
}

// ── Layout model ─────────────────────────────────────────────

enum class Kind { LangChip, AutoExclude, DirToggle, Minus, Plus, Floor, ShortConf, Reset };

struct Item {
    Kind kind;
    int  index;           // direction (0-5) or language (0-2)
    D2D1_RECT_F r;        // DIP
    bool enabled;
    std::wstring tip;
};

enum class Font { Title, Body, BodyStrong, Caption, Section };

struct Label {
    std::wstring text;
    D2D1_RECT_F r;
    Font font;
    bool dim;             // greyed (disabled)
    bool secondary;
    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
};

struct Layout {
    std::vector<Item>        items;
    std::vector<Label>       labels;
    std::vector<D2D1_RECT_F> cards;
    std::vector<float>       seps;      // y of horizontal separators
    float height = 0;
};

static bool g_noPersist = false;   // preview / demo: never write settings

static bool LangShown(const std::string& l) {
    return !LangAvail::GetAutoExclude() || LangAvail::IsInstalled(l);
}

static bool DirShown(int i) { return LangShown(kFrom[i]) && LangShown(kTo[i]); }

static bool DirActive(int i) {
    return LangAvail::GetLangEnabled(kFrom[i]) && LangAvail::GetLangEnabled(kTo[i]) &&
           LangAvail::GetDirectionEnabled(kFrom[i], kTo[i]);
}

static int MinChars(int i) {
    auto it = Config::PairOverrides.find({kFrom[i], kTo[i]});
    return it != Config::PairOverrides.end() ? it->second.EarlyDetectionMinChars
                                             : kFactoryMinChars[i];
}

static Layout BuildLayout() {
    Layout L;
    const float pad = 16, cw = kWidth - 2 * pad;
    float y = 14;

    L.labels.push_back({L"Detection settings", {pad, y, pad + cw, y + 28}, Font::Title, false, false});
    y += 28;
    L.labels.push_back({L"Fix text typed in the wrong keyboard layout.",
                        {pad, y, pad + cw, y + 18}, Font::Caption, false, true});
    y += 30;

    // ── Languages (card) ──
    L.labels.push_back({L"LANGUAGES", {pad, y, pad + cw, y + 16}, Font::Section, false, true});
    y += 22;
    {
        const float ip = 12, x0 = pad + ip, iw = cw - 2 * ip;
        const float gap = 8, w = (iw - 2 * gap) / 3;
        const float top = y;
        y += ip;
        std::wstring missing;
        for (int i = 0; i < LangAvail::kNumLangs; ++i) {
            const std::string l = LangAvail::kLangs[i];
            bool installed = LangAvail::IsInstalled(l);
            bool shown     = LangShown(l);
            D2D1_RECT_F r  = {x0 + i * (w + gap), y, x0 + i * (w + gap) + w, y + 32};
            std::wstring tip = shown
                ? std::wstring(L"Include ") + LangName(l) +
                  L" in detection. When off, text is never converted to or from " + LangName(l) + L"."
                : std::wstring(LangName(l)) +
                  L" has no keyboard layout installed, so it is excluded automatically. "
                  L"Add it in Windows Settings > Time & language > Language & region.";
            L.items.push_back({Kind::LangChip, i, r, shown, tip});
            if (!installed) {
                if (!missing.empty()) missing += L", ";
                missing += LangName(l);
            }
        }
        y += 32 + 10;
        L.labels.push_back({missing.empty() ? std::wstring(L"All supported layouts are installed.")
                                            : missing + L": no layout installed",
                            {x0, y, x0 + iw - 150, y + 20}, Font::Caption, false, true});
        // Auto-exclude toggle at the right of the caption line.
        L.labels.push_back({L"Skip missing", {x0 + iw - 150, y, x0 + iw - 50, y + 20},
                            Font::Caption, false, true, DWRITE_TEXT_ALIGNMENT_TRAILING});
        L.items.push_back({Kind::AutoExclude, 0, {x0 + iw - 40, y, x0 + iw, y + 20}, true,
                           L"Automatically exclude languages whose keyboard layout is not installed "
                           L"(recommended). Turn off only if you add layouts on the fly."});
        y += 20 + ip;
        L.cards.push_back({pad, top, pad + cw, y});
        y += 16;
    }

    // ── Directions (one card each) ──
    L.labels.push_back({L"DIRECTIONS", {pad, y, pad + cw, y + 16}, Font::Section, false, true});
    y += 22;
    int shownDirs = 0;
    for (int i = 0; i < 6; ++i) {
        if (!DirShown(i)) continue;
        ++shownDirs;
        const bool langsOn = LangAvail::GetLangEnabled(kFrom[i]) && LangAvail::GetLangEnabled(kTo[i]);
        const bool active  = DirActive(i);
        const float h = 92;
        L.cards.push_back({pad, y, pad + cw, y + h});
        const float ip = 14, x0 = pad + ip, x1 = pad + cw - ip;

        std::wstring name = std::wstring(LangName(kFrom[i])) + L"  →  " + LangName(kTo[i]);
        L.labels.push_back({name, {x0, y + 12, x1 - 50, y + 32}, Font::BodyStrong, !langsOn, false});
        L.items.push_back({Kind::DirToggle, i, {x1 - 40, y + 12, x1, y + 32}, langsOn,
                           std::wstring(L"Correct text typed on the ") + LangName(kFrom[i]) +
                           L" layout that was meant to be " + LangName(kTo[i]) + L"."});

        // Captions
        L.labels.push_back({L"Min chars", {x0, y + 40, x0 + 100, y + 56}, Font::Caption, !active, true});
        const float sx0 = x0 + 116, sx1 = x1 - 44;
        L.labels.push_back({L"Confidence floor", {sx0, y + 40, x1, y + 56}, Font::Caption, !active, true});

        // Stepper  [−] n [+]
        const float by = y + 58, bs = 26;
        const std::wstring stepTip =
            L"Letters to type before detection may fire. Lower reacts sooner but risks "
            L"correcting short words that were typed on purpose.";
        L.items.push_back({Kind::Minus, i, {x0, by, x0 + bs, by + bs},
                           active && MinChars(i) > kCharsMin, stepTip});
        L.labels.push_back({std::to_wstring(MinChars(i)), {x0 + bs, by, x0 + 2 * bs + 4, by + bs},
                            Font::BodyStrong, !active, false, DWRITE_TEXT_ALIGNMENT_CENTER});
        L.items.push_back({Kind::Plus, i, {x0 + 2 * bs + 4, by, x0 + 3 * bs + 4, by + bs},
                           active && MinChars(i) < kCharsMax, stepTip});

        // Slider + value
        L.items.push_back({Kind::Floor, i, {sx0, by, sx1, by + bs}, active,
                           L"Lowest model confidence needed once a word is long (15+ letters). "
                           L"Lower = more corrections and more false positives. Calibration may "
                           L"adjust it slightly on top of this value."});
        wchar_t buf[16];
        swprintf(buf, 16, L"%.2f", Feedback::GetBaseConfFloor(kFrom[i], kTo[i]));
        L.labels.push_back({buf, {sx1 + 4, by, x1, by + bs}, Font::BodyStrong, !active, false,
                            DWRITE_TEXT_ALIGNMENT_TRAILING});
        y += h + 8;
    }
    if (shownDirs == 0) {
        L.labels.push_back({L"Install a second supported keyboard layout to enable switching.",
                            {pad, y, pad + cw, y + 18}, Font::Caption, false, true});
        y += 26;
    }

    // ── Short words (card) ──
    y += 8;
    L.labels.push_back({L"SHORT WORDS", {pad, y, pad + cw, y + 16}, Font::Section, false, true});
    y += 22;
    {
        const float ip = 14, x0 = pad + ip, x1 = pad + cw - ip;
        const float top = y;
        L.labels.push_back({L"Confidence required at minimum length (all directions)",
                            {x0, y + 10, x1, y + 28}, Font::Caption, false, true});
        const float sy = y + 32;
        L.items.push_back({Kind::ShortConf, 0, {x0, sy, x1 - 48, sy + 26}, true,
                           L"Confidence needed when only the minimum number of letters has been "
                           L"typed. It relaxes linearly towards each direction's floor."});
        wchar_t buf[16];
        swprintf(buf, 16, L"%.2f", Config::DefaultParams.ConfidenceAtMinChars);
        L.labels.push_back({buf, {x1 - 44, sy, x1, sy + 26}, Font::BodyStrong, false, false,
                            DWRITE_TEXT_ALIGNMENT_TRAILING});
        y = sy + 26 + 10;
        L.cards.push_back({pad, top, pad + cw, y});
        y += 16;
    }

    // ── Footer ──
    L.items.push_back({Kind::Reset, 0, {pad + cw - 128, y, pad + cw, y + 32}, true,
                       L"Restore factory thresholds, re-enable all languages and directions, and "
                       L"clear learned calibration."});
    L.labels.push_back({std::wstring(L"v") + Config::VERSION, {pad, y, pad + 120, y + 32},
                        Font::Caption, false, true});
    y += 32 + 16;
    L.height = y;
    return L;
}

// ── Actions ──────────────────────────────────────────────────

void SyncGlobalEarlyOut() {
    int m = 15;
    for (const auto& [pair, p] : Config::PairOverrides)
        m = std::min(m, p.EarlyDetectionMinChars);
    Config::DefaultParams.EarlyDetectionMinChars = m;
}

static void SaveLangSettings() {
    if (!g_noPersist) Security::SaveSettings();
}

static void SetShortConf(float v) {
    Config::DefaultParams.ConfidenceAtMinChars = v;
    for (auto& [pair, p] : Config::PairOverrides) p.ConfidenceAtMinChars = v;
}

static void SetMinChars(int i, int v) {
    auto it = Config::PairOverrides.find({kFrom[i], kTo[i]});
    if (it == Config::PairOverrides.end()) return;
    it->second.EarlyDetectionMinChars = std::clamp(v, kCharsMin, kCharsMax);
    SyncGlobalEarlyOut();
}

static void ResetDefaults() {
    SetShortConf(kFactoryShortConf);
    for (int i = 0; i < 6; ++i) {
        SetMinChars(i, kFactoryMinChars[i]);
        // Base → factory AND wipe the invisible calibration delta.
        Feedback::ResetPairToFactory(kFrom[i], kTo[i], kFactoryFloor[i]);
        LangAvail::SetDirectionEnabled(kFrom[i], kTo[i], true);
    }
    for (const char* l : LangAvail::kLangs) LangAvail::SetLangEnabled(l, true);
    LangAvail::SetAutoExclude(true);
    SyncGlobalEarlyOut();
    if (!g_noPersist) Feedback::SavePrefs();
    SaveLangSettings();
}

static float SliderValue(const Item& it) {
    return it.kind == Kind::ShortConf ? Config::DefaultParams.ConfidenceAtMinChars
                                      : Feedback::GetBaseConfFloor(kFrom[it.index], kTo[it.index]);
}

static void SetSliderFromX(const Item& it, float x) {
    const float pad = 10;   // thumb radius inset
    float t = (x - (it.r.left + pad)) / ((it.r.right - pad) - (it.r.left + pad));
    t = std::clamp(t, 0.0f, 1.0f);
    float v = std::round((kConfMin + t * (kConfMax - kConfMin)) * 100.0f) / 100.0f;
    if (it.kind == Kind::ShortConf) {
        SetShortConf(v);
    } else if (std::fabs(v - SliderValue(it)) > 0.001f) {
        // Routed to the calibration controller as the USER BASE: effective =
        // clamp(base + delta) is applied there.  Persisted on close.
        Feedback::SetBaseConfFloor(kFrom[it.index], kTo[it.index], v);
    }
}

static void Activate(const Item& it) {
    switch (it.kind) {
    case Kind::LangChip: {
        const char* l = LangAvail::kLangs[it.index];
        LangAvail::SetLangEnabled(l, !LangAvail::GetLangEnabled(l));
        SaveLangSettings();
        break;
    }
    case Kind::AutoExclude:
        LangAvail::SetAutoExclude(!LangAvail::GetAutoExclude());
        SaveLangSettings();
        break;
    case Kind::DirToggle:
        LangAvail::SetDirectionEnabled(kFrom[it.index], kTo[it.index],
            !LangAvail::GetDirectionEnabled(kFrom[it.index], kTo[it.index]));
        SaveLangSettings();
        break;
    case Kind::Minus: SetMinChars(it.index, MinChars(it.index) - 1); break;
    case Kind::Plus:  SetMinChars(it.index, MinChars(it.index) + 1); break;
    case Kind::Reset: ResetDefaults(); break;
    default: break;
    }
}

// ── Rendering (Direct2D / DirectWrite) ───────────────────────

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Gfx {
    ID2D1Factory*        d2d   = nullptr;
    IDWriteFactory*      dw    = nullptr;
    IDWriteTextFormat*   fmt[5] = {};
    float                fmtScale = 0;

    bool Init() {
        if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                             __uuidof(ID2D1Factory), nullptr,
                                             reinterpret_cast<void**>(&d2d))))
            return false;
        if (!dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                              reinterpret_cast<IUnknown**>(&dw))))
            return false;
        return true;
    }

    bool HasFamily(const wchar_t* name) {
        IDWriteFontCollection* fc = nullptr;
        if (FAILED(dw->GetSystemFontCollection(&fc, FALSE)) || !fc) return false;
        UINT32 idx = 0; BOOL exists = FALSE;
        fc->FindFamilyName(name, &idx, &exists);
        fc->Release();
        return exists;
    }

    // Text formats are created in DIP; the render target transform scales.
    void EnsureFormats() {
        if (fmt[0]) return;
        const wchar_t* display = HasFamily(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
        const wchar_t* text    = HasFamily(L"Segoe UI Variable Text")    ? L"Segoe UI Variable Text"    : L"Segoe UI";
        const wchar_t* small   = HasFamily(L"Segoe UI Variable Small")   ? L"Segoe UI Variable Small"   : L"Segoe UI";
        struct { const wchar_t* fam; DWRITE_FONT_WEIGHT w; float size; } spec[5] = {
            { display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 20.0f },   // Title
            { text,    DWRITE_FONT_WEIGHT_REGULAR,   14.0f },   // Body
            { text,    DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f },   // BodyStrong
            { small,   DWRITE_FONT_WEIGHT_REGULAR,   12.0f },   // Caption
            { small,   DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.0f },   // Section
        };
        for (int i = 0; i < 5; ++i) {
            dw->CreateTextFormat(spec[i].fam, nullptr, spec[i].w, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, spec[i].size, L"en-us", &fmt[i]);
            if (fmt[i]) {
                fmt[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                fmt[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                fmt[i]->SetTrimming(&trim, nullptr);
            }
        }
    }
};
static Gfx g_gfx;

struct PaintState {
    int  hot     = -1;
    int  pressed = -1;
};

static void Draw(ID2D1RenderTarget* rt, const Layout& L, const Theme& th,
                 const PaintState& ps, float scale) {
    rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->Clear(th.bg);

    ID2D1SolidColorBrush* br = nullptr;
    rt->CreateSolidColorBrush(th.text, &br);
    if (!br) return;
    auto fill = [&](const D2D1_RECT_F& r, float rad, const D2D1_COLOR_F& c) {
        br->SetColor(c);
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
    };
    auto stroke = [&](const D2D1_RECT_F& r, float rad, const D2D1_COLOR_F& c, float w = 1.0f) {
        br->SetColor(c);
        D2D1_RECT_F in = {r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2};
        rt->DrawRoundedRectangle(D2D1::RoundedRect(in, rad, rad), br, w);
    };
    auto circle = [&](float cx, float cy, float rad, const D2D1_COLOR_F& c) {
        br->SetColor(c);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), br);
    };
    auto text = [&](const std::wstring& s, const D2D1_RECT_F& r, Font f, const D2D1_COLOR_F& c,
                    DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING) {
        IDWriteTextFormat* tf = g_gfx.fmt[static_cast<int>(f)];
        if (!tf) return;
        tf->SetTextAlignment(a);
        br->SetColor(c);
        rt->DrawText(s.c_str(), static_cast<UINT32>(s.size()), tf, r, br,
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    for (const auto& c : L.cards) {
        const float sh = th.dark ? 0.35f : 0.06f;
        fill({c.left, c.top + 2, c.right, c.bottom + 2}, 8, D2D1::ColorF(0x000000, sh * 0.6f));
        fill({c.left, c.top + 1, c.right, c.bottom + 1}, 8, D2D1::ColorF(0x000000, sh));
        fill(c, 8, th.card);
        stroke(c, 8, th.cardStroke);
    }
    for (float y : L.seps) {
        br->SetColor(th.sep);
        rt->DrawLine(D2D1::Point2F(16, y), D2D1::Point2F(kWidth - 16, y), br, 1.0f);
    }
    for (const auto& lb : L.labels)
        text(lb.text, lb.r, lb.font, lb.dim ? th.text3 : (lb.secondary ? th.text2 : th.text), lb.align);

    for (size_t n = 0; n < L.items.size(); ++n) {
        const Item& it = L.items[n];
        const bool hot = static_cast<int>(n) == ps.hot && it.enabled;
        const bool down = static_cast<int>(n) == ps.pressed && it.enabled;
        const D2D1_RECT_F& r = it.r;
        const float cy = (r.top + r.bottom) / 2;

        switch (it.kind) {
        case Kind::AutoExclude:
        case Kind::DirToggle: {
            bool on = it.kind == Kind::AutoExclude
                ? LangAvail::GetAutoExclude()
                : LangAvail::GetDirectionEnabled(kFrom[it.index], kTo[it.index]);
            const float h = r.bottom - r.top, rad = h / 2;
            if (!it.enabled) {
                stroke(r, rad, th.text3);
                circle(on ? r.right - rad : r.left + rad, cy, 5, th.text3);
            } else if (on) {
                fill(r, rad, th.accent);
                circle(r.right - rad, cy, hot ? 7.0f : 6.0f, th.onAccent);
            } else {
                if (hot) fill(r, rad, th.ctrlHover);
                stroke(r, rad, th.text2);
                circle(r.left + rad, cy, hot ? 6.0f : 5.0f, th.text2);
            }
            break;
        }
        case Kind::LangChip: {
            const char* l = LangAvail::kLangs[it.index];
            bool on = LangAvail::GetLangEnabled(l);
            const float rad = (r.bottom - r.top) / 2;
            std::wstring name = LangName(l);
            if (!it.enabled) {
                stroke(r, rad, th.ctrlStroke);
                text(name + L" · n/a", r, Font::Body, th.text3, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (on) {
                fill(r, rad, th.accent);
                if (hot) fill(r, rad, D2D1::ColorF(th.dark ? 0x000000 : 0xFFFFFF, 0.10f));
                text(L"✓  " + name, r, Font::BodyStrong, th.onAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                fill(r, rad, hot ? th.ctrlHover : th.ctrlFill);
                stroke(r, rad, th.ctrlStroke);
                text(name, r, Font::Body, th.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            break;
        }
        case Kind::Minus:
        case Kind::Plus:
        case Kind::Reset: {
            fill(r, 4, down ? th.ctrlFill : (hot ? th.ctrlHover : th.ctrlFill));
            if (hot && !down) fill(r, 4, D2D1::ColorF(th.dark ? 0xFFFFFF : 0x000000, 0.03f));
            stroke(r, 4, th.ctrlStroke);
            const wchar_t* glyph = it.kind == Kind::Minus ? L"−"
                                 : it.kind == Kind::Plus  ? L"+" : L"Reset defaults";
            text(glyph, r, Font::Body, it.enabled ? (down ? th.text2 : th.text) : th.text3,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        }
        case Kind::Floor:
        case Kind::ShortConf: {
            const float inset = 10;
            const float x0 = r.left + inset, x1 = r.right - inset;
            const float t  = std::clamp((SliderValue(it) - kConfMin) / (kConfMax - kConfMin), 0.0f, 1.0f);
            const float tx = x0 + t * (x1 - x0);
            const D2D1_COLOR_F fillC = it.enabled ? th.accent : th.text3;
            fill({x0, cy - 2, x1, cy + 2}, 2, it.enabled ? th.track : th.ctrlStroke);
            fill({x0, cy - 2, tx, cy + 2}, 2, fillC);
            circle(tx, cy, 10, th.thumb);
            br->SetColor(th.ctrlStroke);
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(tx, cy), 10, 10), br, 1.0f);
            circle(tx, cy, down ? 5.0f : (hot ? 7.0f : 6.0f), fillC);
            break;
        }
        }
    }
    br->Release();
}

// Render the layout into a 32-bpp premultiplied DIB selected into |memDC|.
static bool RenderToDC(HDC memDC, int w, int h, const Layout& L, const Theme& th,
                       const PaintState& ps, float scale) {
    if (!g_gfx.Init()) return false;
    g_gfx.EnsureFormats();
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ID2D1DCRenderTarget* rt = nullptr;
    if (FAILED(g_gfx.d2d->CreateDCRenderTarget(&props, &rt)) || !rt) return false;
    RECT rc = {0, 0, w, h};
    bool ok = false;
    if (SUCCEEDED(rt->BindDC(memDC, &rc))) {
        rt->BeginDraw();
        Draw(rt, L, th, ps, scale);
        ok = SUCCEEDED(rt->EndDraw());
    }
    rt->Release();
    return ok;
}

static HBITMAP CreateDib(HDC dc, int w, int h, void** bits) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;     // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

// ── Window ───────────────────────────────────────────────────

struct FlyoutState {
    HWND       hwnd     = nullptr;
    HWND       tip      = nullptr;
    float      scale    = 1.0f;
    Theme      theme;
    Layout     layout;
    PaintState ps;
    int        drag     = -1;
    bool       tracking = false;
};
static FlyoutState g_fly;

using GetDpiForWindowFn = UINT (WINAPI*)(HWND);
using SetThreadDpiCtxFn = HANDLE (WINAPI*)(HANDLE);

static UINT WindowDpi(HWND hwnd) {
    static auto fn = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    UINT dpi = fn ? fn(hwnd) : 0;
    if (!dpi) { HDC dc = GetDC(hwnd); dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(hwnd, dc); }
    return dpi ? dpi : 96;
}

static int HitTest(float x, float y) {
    const auto& items = g_fly.layout.items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& r = items[i].r;
        if (x >= r.left - 2 && x <= r.right + 2 && y >= r.top - 2 && y <= r.bottom + 2)
            return static_cast<int>(i);
    }
    return -1;
}

static void RebuildTooltips() {
    HWND hwnd = g_fly.hwnd;
    if (g_fly.tip) DestroyWindow(g_fly.tip);
    g_fly.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, hwnd, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!g_fly.tip) return;
    SendMessageW(g_fly.tip, TTM_SETMAXTIPWIDTH, 0, static_cast<LPARAM>(320 * g_fly.scale));
    SendMessageW(g_fly.tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 15000);
    const float s = g_fly.scale;
    for (size_t i = 0; i < g_fly.layout.items.size(); ++i) {
        const Item& it = g_fly.layout.items[i];
        if (it.tip.empty()) continue;
        TOOLINFOW ti = {};
        ti.cbSize   = TTTOOLINFOW_V1_SIZE;
        ti.uFlags   = TTF_SUBCLASS;
        ti.hwnd     = hwnd;
        ti.uId      = i + 1;
        ti.rect     = { static_cast<LONG>(it.r.left * s), static_cast<LONG>(it.r.top * s),
                        static_cast<LONG>(it.r.right * s), static_cast<LONG>(it.r.bottom * s) };
        ti.lpszText = const_cast<wchar_t*>(it.tip.c_str());
        SendMessageW(g_fly.tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

static void ApplyDwmStyle(HWND hwnd) {
    const bool dark = SystemUsesDarkTheme();
    BOOL darkFlag = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkFlag, sizeof(darkFlag));
    int corner = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));

    // Solid background by design (an acrylic backdrop looked washed out).
    g_fly.theme = MakeTheme(dark);
}

// Resize to the current layout; the bottom edge stays anchored (the
// flyout opens above the taskbar).
static void Relayout(bool keepBottom) {
    g_fly.layout = BuildLayout();
    HWND hwnd = g_fly.hwnd;
    RECT wr; GetWindowRect(hwnd, &wr);
    int w = static_cast<int>(std::ceil(kWidth * g_fly.scale));
    int h = static_cast<int>(std::ceil(g_fly.layout.height * g_fly.scale));
    int top = keepBottom ? wr.bottom - h : wr.top;
    SetWindowPos(hwnd, nullptr, wr.left, top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    RebuildTooltips();
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void RefreshLabels() {
    // Values changed but structure did not: rebuild labels/enabled flags only.
    g_fly.layout = BuildLayout();
    InvalidateRect(g_fly.hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wParam) return 0;   // client area = whole window (frame kept for DWM shadow/corners)
        break;
    case WM_NCHITTEST:
        return HTCLIENT;        // no resizing via the thick frame
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;
        HDC mem = CreateCompatibleDC(dc);
        void* bits = nullptr;
        HBITMAP bmp = CreateDib(dc, w, h, &bits);
        if (mem && bmp) {
            HGDIOBJ old = SelectObject(mem, bmp);
            RenderToDC(mem, w, h, g_fly.layout, g_fly.theme, g_fly.ps, g_fly.scale);
            BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
        }
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        float x = GET_X_LPARAM(lParam) / g_fly.scale, y = GET_Y_LPARAM(lParam) / g_fly.scale;
        if (!g_fly.tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            g_fly.tracking = TrackMouseEvent(&tme) != FALSE;
        }
        if (g_fly.drag >= 0) {
            SetSliderFromX(g_fly.layout.items[g_fly.drag], x);
            RefreshLabels();
            return 0;
        }
        int hot = HitTest(x, y);
        if (hot != g_fly.ps.hot) {
            g_fly.ps.hot = hot;
            bool hand = hot >= 0 && g_fly.layout.items[hot].enabled;
            SetCursor(LoadCursor(nullptr, hand ? IDC_HAND : IDC_ARROW));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            int hot = g_fly.ps.hot;
            bool hand = hot >= 0 && hot < (int)g_fly.layout.items.size() && g_fly.layout.items[hot].enabled;
            SetCursor(LoadCursor(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSELEAVE:
        g_fly.tracking = false;
        if (g_fly.drag < 0) { g_fly.ps.hot = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;

    case WM_LBUTTONDOWN: {
        float x = GET_X_LPARAM(lParam) / g_fly.scale, y = GET_Y_LPARAM(lParam) / g_fly.scale;
        int idx = HitTest(x, y);
        if (idx < 0 || !g_fly.layout.items[idx].enabled) return 0;
        g_fly.ps.pressed = idx;
        SetCapture(hwnd);
        Kind k = g_fly.layout.items[idx].kind;
        if (k == Kind::Floor || k == Kind::ShortConf) {
            g_fly.drag = idx;
            SetSliderFromX(g_fly.layout.items[idx], x);
            RefreshLabels();
        } else {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        float x = GET_X_LPARAM(lParam) / g_fly.scale, y = GET_Y_LPARAM(lParam) / g_fly.scale;
        int pressed = g_fly.ps.pressed;
        bool wasDrag = g_fly.drag >= 0;
        g_fly.drag = -1;
        g_fly.ps.pressed = -1;
        ReleaseCapture();
        if (!wasDrag && pressed >= 0 && pressed == HitTest(x, y)) {
            Item it = g_fly.layout.items[pressed];
            Activate(it);
            bool structural = it.kind == Kind::LangChip || it.kind == Kind::AutoExclude ||
                              it.kind == Kind::Reset;
            if (structural) Relayout(true); else { RefreshLabels(); RebuildTooltips(); }
        } else {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        g_fly.drag = -1;
        g_fly.ps.pressed = -1;
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) DestroyWindow(hwnd);   // light-dismiss
        return 0;

    case WM_DPICHANGED: {
        g_fly.scale = HIWORD(wParam) / 96.0f;
        const RECT* s = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, s->left, s->top, s->right - s->left, s->bottom - s->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Relayout(false);
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        ApplyDwmStyle(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_DESTROY:
        // Persist user-base floor edits (SetBaseConfFloor batches writes here).
        if (!g_noPersist) Feedback::SavePrefs();
        if (g_fly.tip) DestroyWindow(g_fly.tip);
        g_fly = {};
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void Open(HWND owner) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    LangAvail::Refresh();

    // Per-monitor-v2 DPI for this window only (thread context while creating).
    static auto setCtx = reinterpret_cast<SetThreadDpiCtxFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    HANDLE prev = setCtx ? setCtx(reinterpret_cast<HANDLE>(-4)) : nullptr;   // PER_MONITOR_AWARE_V2

    POINT pt; GetCursorPos(&pt);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"Detection settings",
                                WS_POPUP | WS_THICKFRAME, pt.x, pt.y, 10, 10,
                                owner, nullptr, hInst, nullptr);
    if (setCtx && prev) setCtx(prev);
    if (!hwnd) return;

    g_fly.hwnd  = hwnd;
    g_fly.scale = WindowDpi(hwnd) / 96.0f;
    ApplyDwmStyle(hwnd);
    g_fly.layout = BuildLayout();

    const int w = static_cast<int>(std::ceil(kWidth * g_fly.scale));
    const int h = static_cast<int>(std::ceil(g_fly.layout.height * g_fly.scale));
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    const int gap = static_cast<int>(12 * g_fly.scale);
    int x = std::clamp(static_cast<int>(pt.x - w / 2), static_cast<int>(wa.left + gap),
                       static_cast<int>(wa.right - w - gap));
    int y = std::clamp(static_cast<int>(wa.bottom - h - gap), static_cast<int>(wa.top + gap),
                       static_cast<int>(wa.bottom - h));
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    // Trigger WM_NCCALCSIZE so the frame is removed before first paint.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    RebuildTooltips();
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

void Toggle(HWND owner) {
    if (g_fly.hwnd) { DestroyWindow(g_fly.hwnd); return; }
    Open(owner);
}

// ── Developer / CI helpers ───────────────────────────────────

bool RenderToBitmapFile(const std::wstring& path, bool dark, float scale) {
    g_noPersist = true;
    Layout L = BuildLayout();
    Theme th = MakeTheme(dark);
    int w = static_cast<int>(std::ceil(kWidth * scale));
    int h = static_cast<int>(std::ceil(L.height * scale));
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bmp = CreateDib(screen, w, h, &bits);
    ReleaseDC(nullptr, screen);
    bool ok = false;
    if (mem && bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        PaintState ps;
        ok = RenderToDC(mem, w, h, L, th, ps, scale);
        GdiFlush();
        SelectObject(mem, old);
        if (ok) {
            BITMAPFILEHEADER fh = {};
            BITMAPINFOHEADER ih = {};
            ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = -h; ih.biPlanes = 1;
            ih.biBitCount = 32; ih.biCompression = BI_RGB;
            DWORD dataSize = static_cast<DWORD>(w) * h * 4;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + dataSize;
            FILE* f = _wfopen(path.c_str(), L"wb");
            ok = f != nullptr;
            if (f) {
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                fwrite(bits, 1, dataSize, f);
                fclose(f);
            }
        }
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    return ok;
}

void RunDemo() {
    g_noPersist = true;
    Open(nullptr);
    MSG m;
    while (g_fly.hwnd && GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

} // namespace SettingsFlyout
