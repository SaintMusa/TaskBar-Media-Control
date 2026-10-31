#include "settings_window.h"
#include <windowsx.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>
#include "hotkey.h"
#include "list_text.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClass[] = L"TaskbarMedia.Settings";

// Layout, in DIPs (96 DPI pixels).
constexpr int kClientWidth = 640, kClientHeight = 560;
constexpr int kNavWidth = 176, kNavInset = 8, kNavTop = 20, kNavItem = 40, kNavGap = 4;
constexpr int kPad = 32;
constexpr int kTitleTop = 22, kTitleHeight = 36, kRowsTop = 76;
constexpr int kToggleRow = 56, kNumberRow = 56, kHotkeyRow = 56, kLinkRow = 48, kNoteRow = 40, kTextRow = 84;
constexpr int kBoxHeight = 32, kBoxRadius = 6, kBoxPad = 10, kEditHeight = 18;
constexpr int kToggleWidth = 40, kToggleHeight = 20;
constexpr int kNumberBoxWidth = 88, kHotkeyBoxWidth = 232, kClearSize = 24;
constexpr int kMaxHideSeconds = 86400;

constexpr int kEditIdFirst = 1000;

// Segoe Fluent Icons code points.
constexpr wchar_t kGlyphGeneral = 0xE713, kGlyphApps = 0xE71D, kGlyphKeyboard = 0xE765, kGlyphClear = 0xE711;

// DWM attributes newer than some SDK headers.
constexpr DWORD kUseImmersiveDarkMode = 20;
constexpr DWORD kBorderColor = 34;   // DWMWA_BORDER_COLOR
constexpr DWORD kCaptionColor = 35;  // DWMWA_CAPTION_COLOR
constexpr DWORD kCaptionTextColor = 36; // DWMWA_TEXT_COLOR

enum class Page { General, Apps, Hotkeys };
enum class Kind { Toggle, Number, Text, Hotkey, Link, Note };
enum class Field {
    None,
    StartWithWindows, StayVisible, HideAfter, ArtColor, OpenFile,
    Pinned, Ignored, PreferMusic, MusicApps, PreferNonBrowsers, Browsers,
    HotkeyNote, HkPlayPause, HkNext, HkPrevious, HkSwitchApp,
};

struct RowSpec {
    Field field;
    Kind kind;
    const wchar_t* title;
    const wchar_t* detail;
};

constexpr RowSpec kGeneralRows[] = {
    {Field::StartWithWindows, Kind::Toggle, L"Start with Windows", L"Run TaskbarMedia when you sign in."},
    {Field::StayVisible, Kind::Toggle, L"Stay visible when paused", L"Keep showing the last track instead of hiding."},
    {Field::HideAfter, Kind::Number, L"Hide after pausing for", L"Seconds. Used when the option above is off."},
    {Field::ArtColor, Kind::Toggle, L"Color the seekbar from the cover art", L"Use the cover's main color for the progress bar."},
    {Field::OpenFile, Kind::Link, L"Open settings file", nullptr},
};
constexpr RowSpec kAppsRows[] = {
    {Field::Pinned, Kind::Text, L"Pinned app", L"Always shown while it's open, over every other rule."},
    {Field::Ignored, Kind::Text, L"Hidden apps", L"Never shown. Names, exe names or app IDs, separated by ;"},
    {Field::PreferMusic, Kind::Toggle, L"Music apps first", L"Show a playing music app over anything else."},
    {Field::MusicApps, Kind::Text, L"Music apps", L"What counts as a music app, separated by ;"},
    {Field::PreferNonBrowsers, Kind::Toggle, L"Browsers last", L"Show a browser only while no other app is playing."},
    {Field::Browsers, Kind::Text, L"Browsers", L"What counts as a browser, separated by ;"},
};
constexpr RowSpec kHotkeyRows[] = {
    {Field::HotkeyNote, Kind::Note, L"Hotkeys work from any app. Each needs Ctrl, Alt, Shift or Win.", nullptr},
    {Field::HkPlayPause, Kind::Hotkey, L"Play / pause", nullptr},
    {Field::HkNext, Kind::Hotkey, L"Next track", nullptr},
    {Field::HkPrevious, Kind::Hotkey, L"Previous track", nullptr},
    {Field::HkSwitchApp, Kind::Hotkey, L"Switch app", nullptr},
};

struct NavSpec {
    Page page;
    const wchar_t* label;
    wchar_t glyph;
};
constexpr NavSpec kNav[] = {
    {Page::General, L"General", kGlyphGeneral},
    {Page::Apps, L"Apps", kGlyphApps},
    {Page::Hotkeys, L"Hotkeys", kGlyphKeyboard},
};

struct Item {
    const RowSpec* spec = nullptr;
    float top = 0, height = 0;
    D2D1_RECT_F box{};        // the toggle, the text box, or the hotkey box
    D2D1_RECT_F clear{};      // the hotkey box's clear button
    HWND edit = nullptr;
};

struct WindowState {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    bool light = false;
    SettingsHost host;
    Page page = Page::General;
    std::vector<Item> items;

    int hoveredNav = -1, hoveredItem = -1, pressedItem = -1;
    bool hoveredClear = false, pressedClear = false;
    bool trackingMouse = false;
    int capturing = -1;           // index of the hotkey item waiting for a key press
    std::wstring captureHint;     // what's wrong with the last key tried
    HWND focusedEdit = nullptr;

    HFONT font = nullptr;
    HBRUSH editBrush = nullptr;

    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<IDWriteTextFormat> titleFormat, rowFormat, detailFormat, iconFormat, boxFormat;
    HDC memDC = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    SIZE bitmapSize{};
} win;

float S(float dip) { return dip * win.dpi / 96.0f; }
int Scale(int dip) { return MulDiv(dip, static_cast<int>(win.dpi), 96); }

D2D1_COLOR_F Ink(float alpha) {
    return win.light ? D2D1::ColorF(0, 0, 0, alpha) : D2D1::ColorF(1, 1, 1, alpha);
}

// An opaque background. A translucent one (Mica) would blend with the native text
// boxes, which draw with GDI and so leave every pixel they touch transparent.
D2D1_COLOR_F WindowFill() { return win.light ? D2D1::ColorF(0.953f, 0.953f, 0.953f, 1) : D2D1::ColorF(0.125f, 0.125f, 0.125f, 1); }
COLORREF WindowFillRef() { return win.light ? RGB(243, 243, 243) : RGB(32, 32, 32); }

// The text boxes' color, which the native edit controls inside them must match.
D2D1_COLOR_F BoxFill() { return win.light ? D2D1::ColorF(1, 1, 1, 1) : D2D1::ColorF(0.176f, 0.176f, 0.176f, 1); }
COLORREF BoxFillRef() { return win.light ? RGB(255, 255, 255) : RGB(45, 45, 45); }
COLORREF EditTextRef() { return win.light ? RGB(0, 0, 0) : RGB(255, 255, 255); }

D2D1_COLOR_F Accent() {
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) {
        return D2D1::ColorF(((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, 1);
    }
    return D2D1::ColorF(0.0f, 0.47f, 0.84f, 1);
}

bool Contains(const D2D1_RECT_F& r, POINT pt) {
    return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
}

// --- The rows of each page --------------------------------------------------

const RowSpec* RowsOf(Page page, int& count) {
    switch (page) {
    case Page::General: count = static_cast<int>(std::size(kGeneralRows)); return kGeneralRows;
    case Page::Apps: count = static_cast<int>(std::size(kAppsRows)); return kAppsRows;
    case Page::Hotkeys: count = static_cast<int>(std::size(kHotkeyRows)); return kHotkeyRows;
    }
    count = 0;
    return nullptr;
}

int HeightOf(Kind kind) {
    switch (kind) {
    case Kind::Toggle: return kToggleRow;
    case Kind::Number: return kNumberRow;
    case Kind::Text: return kTextRow;
    case Kind::Hotkey: return kHotkeyRow;
    case Kind::Link: return kLinkRow;
    case Kind::Note: return kNoteRow;
    }
    return 0;
}

std::wstring* HotkeyOf(Settings& s, Field field) {
    switch (field) {
    case Field::HkPlayPause: return &s.hotkeyPlayPause;
    case Field::HkNext: return &s.hotkeyNext;
    case Field::HkPrevious: return &s.hotkeyPrevious;
    case Field::HkSwitchApp: return &s.hotkeySwitchApp;
    default: return nullptr;
    }
}

bool ToggleValue(Field field, const Settings& s) {
    switch (field) {
    case Field::StartWithWindows: return StartsWithWindows();
    case Field::StayVisible: return s.stayVisibleWhenPaused;
    case Field::ArtColor: return s.artColorSeekbar;
    case Field::PreferMusic: return s.preferMusicApps;
    case Field::PreferNonBrowsers: return s.preferNonBrowsers;
    default: return false;
    }
}

std::wstring TextValue(Field field, const Settings& s) {
    switch (field) {
    case Field::HideAfter: return std::to_wstring(s.hideAfterPausedSeconds);
    case Field::Pinned: return s.pinnedApp;
    case Field::Ignored: return JoinList(s.ignoredApps);
    case Field::MusicApps: return JoinList(s.musicApps);
    case Field::Browsers: return JoinList(s.browsers);
    default: return {};
    }
}

std::wstring Trimmed(const std::wstring& text) {
    const size_t first = text.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    return text.substr(first, text.find_last_not_of(L" \t") - first + 1);
}

void ApplyText(Field field, const std::wstring& text, Settings& s) {
    switch (field) {
    case Field::HideAfter: {
        const std::wstring digits = Trimmed(text);
        if (digits.empty()) return; // leave it as it was
        s.hideAfterPausedSeconds = static_cast<UINT>(std::clamp<unsigned long>(wcstoul(digits.c_str(), nullptr, 10), 1, kMaxHideSeconds));
        break;
    }
    case Field::Pinned: s.pinnedApp = Trimmed(text); break;
    case Field::Ignored: s.ignoredApps = SplitList(text); break;
    case Field::MusicApps: s.musicApps = SplitList(text); break;
    case Field::Browsers: s.browsers = SplitList(text); break;
    default: break;
    }
}

void FlipToggle(Field field, Settings& s) {
    switch (field) {
    case Field::StayVisible: s.stayVisibleWhenPaused = !s.stayVisibleWhenPaused; break;
    case Field::ArtColor: s.artColorSeekbar = !s.artColorSeekbar; break;
    case Field::PreferMusic: s.preferMusicApps = !s.preferMusicApps; break;
    case Field::PreferNonBrowsers: s.preferNonBrowsers = !s.preferNonBrowsers; break;
    default: break;
    }
}

void Redraw() {
    if (win.hwnd) InvalidateRect(win.hwnd, nullptr, FALSE);
}

// Saves a changed copy of the settings through the app, which applies it.
template <typename Change> void ChangeSettings(Change change) {
    Settings next = win.host.get();
    change(next);
    if (!(next == win.host.get())) win.host.apply(next);
    Redraw();
}

// --- Native text boxes ------------------------------------------------------

int ItemOfEdit(HWND edit) {
    for (size_t i = 0; i < win.items.size(); ++i) {
        if (win.items[i].edit == edit) return static_cast<int>(i);
    }
    return -1;
}

void CommitEdit(HWND edit) {
    const int index = ItemOfEdit(edit);
    if (index < 0) return;
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    const Field field = win.items[index].spec->field;
    ChangeSettings([&](Settings& s) { ApplyText(field, text, s); });
}

// Put back what the settings say, dropping what was typed.
void RevertEdit(HWND edit) {
    const int index = ItemOfEdit(edit);
    if (index < 0) return;
    SetWindowTextW(edit, TextValue(win.items[index].spec->field, win.host.get()).c_str());
}

LRESULT CALLBACK EditProc(HWND edit, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            CommitEdit(edit);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            RevertEdit(edit);
            SetFocus(win.hwnd); // leaving the box finds nothing left to commit
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == VK_RETURN || wp == VK_ESCAPE) return 0; // no beep
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(edit, EditProc, 1);
        break;
    }
    return DefSubclassProc(edit, msg, wp, lp);
}

void DestroyEdits() {
    for (auto& item : win.items) {
        if (item.edit) DestroyWindow(item.edit);
        item.edit = nullptr;
    }
    win.focusedEdit = nullptr;
}

void MakeFont() {
    if (win.font) DeleteObject(win.font);
    win.font = CreateFontW(-Scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
}

// Where the page's rows go, and the native text boxes inside their boxes.
void BuildPage() {
    DestroyEdits();
    win.items.clear();
    win.capturing = -1;
    int count = 0;
    const RowSpec* rows = RowsOf(win.page, count);
    const float left = S(kNavWidth + kPad), right = S(kClientWidth - kPad);
    float y = S(kRowsTop);
    for (int i = 0; i < count; ++i) {
        Item item;
        item.spec = &rows[i];
        item.top = y;
        item.height = S(static_cast<float>(HeightOf(rows[i].kind)));
        const float middle = item.top + item.height / 2;
        switch (rows[i].kind) {
        case Kind::Toggle:
            item.box = D2D1::RectF(right - S(kToggleWidth), middle - S(kToggleHeight) / 2, right,
                                   middle + S(kToggleHeight) / 2);
            break;
        case Kind::Number:
            item.box = D2D1::RectF(right - S(kNumberBoxWidth), middle - S(kBoxHeight) / 2, right,
                                   middle + S(kBoxHeight) / 2);
            break;
        case Kind::Hotkey:
            item.box = D2D1::RectF(right - S(kHotkeyBoxWidth), middle - S(kBoxHeight) / 2, right,
                                   middle + S(kBoxHeight) / 2);
            item.clear = D2D1::RectF(item.box.right - S(kClearSize) - S(4), middle - S(kClearSize) / 2,
                                     item.box.right - S(4), middle + S(kClearSize) / 2);
            break;
        case Kind::Text:
            item.box = D2D1::RectF(left, item.top + S(44), right, item.top + S(44) + S(kBoxHeight));
            break;
        case Kind::Link:
        case Kind::Note:
            item.box = D2D1::RectF(left, item.top, right, item.top + item.height);
            break;
        }
        win.items.push_back(item);
        y += item.height;
    }

    MakeFont();
    for (size_t i = 0; i < win.items.size(); ++i) {
        Item& item = win.items[i];
        if (item.spec->kind != Kind::Text && item.spec->kind != Kind::Number) continue;
        const D2D1_RECT_F& box = item.box;
        const int x = static_cast<int>(box.left + S(kBoxPad));
        const int height = Scale(kEditHeight);
        const int top = static_cast<int>((box.top + box.bottom) / 2) - height / 2;
        const int width = static_cast<int>(box.right - box.left - 2 * S(kBoxPad));
        const DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | (item.spec->kind == Kind::Number ? ES_NUMBER : 0);
        item.edit = CreateWindowExW(0, L"EDIT", L"", style, x, top, width, height, win.hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditIdFirst + i)),
                                    GetModuleHandleW(nullptr), nullptr);
        if (!item.edit) continue;
        SendMessageW(item.edit, WM_SETFONT, reinterpret_cast<WPARAM>(win.font), TRUE);
        SendMessageW(item.edit, EM_LIMITTEXT, item.spec->kind == Kind::Number ? 5 : 1000, 0);
        SetWindowSubclass(item.edit, EditProc, 1, 0);
    }
}

// Puts what the settings say into the text boxes, except the one being typed in.
void SyncEdits() {
    const Settings settings = win.host.get();
    for (auto& item : win.items) {
        if (!item.edit) continue;
        if (item.edit != GetFocus()) {
            const std::wstring wanted = TextValue(item.spec->field, settings);
            const int length = GetWindowTextLengthW(item.edit);
            std::wstring now(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(item.edit, now.data(), length + 1);
            now.resize(static_cast<size_t>(length));
            if (now != wanted) SetWindowTextW(item.edit, wanted.c_str());
        }
        // The delay only matters when the widget hides after a pause.
        if (item.spec->field == Field::HideAfter) EnableWindow(item.edit, !settings.stayVisibleWhenPaused);
    }
}

// --- Hotkey capture ---------------------------------------------------------

void StopCapture() {
    if (win.capturing < 0) return;
    win.capturing = -1;
    win.captureHint.clear();
    if (win.host.pauseHotkeys) win.host.pauseHotkeys(false);
    Redraw();
}

void StartCapture(int index) {
    if (win.capturing == index) return;
    StopCapture();
    win.capturing = index;
    win.captureHint.clear();
    if (win.host.pauseHotkeys) win.host.pauseHotkeys(true); // so Ctrl+Alt+Up reaches us instead of firing
    SetFocus(win.hwnd);
    Redraw();
}

void OnCaptureKey(UINT key) {
    switch (key) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
        return; // wait for the key that goes with them
    case VK_ESCAPE:
        StopCapture();
        return;
    }
    UINT modifiers = 0;
    if (GetKeyState(VK_CONTROL) < 0) modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0) modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0) modifiers |= MOD_SHIFT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) modifiers |= MOD_WIN;

    const Field field = win.items[win.capturing].spec->field;
    if ((key == VK_BACK || key == VK_DELETE) && modifiers == 0) {
        ChangeSettings([&](Settings& s) { if (auto* hotkey = HotkeyOf(s, field)) hotkey->clear(); });
        StopCapture();
        return;
    }
    const std::wstring text = FormatHotkey(modifiers, key);
    if (text.empty()) {
        win.captureHint = L"That key can't be used";
    } else if (modifiers == 0) {
        win.captureHint = L"Add Ctrl, Alt, Shift or Win";
    } else {
        ChangeSettings([&](Settings& s) { if (auto* hotkey = HotkeyOf(s, field)) *hotkey = text; });
        StopCapture();
        return;
    }
    Redraw();
}

// --- Drawing ----------------------------------------------------------------

ComPtr<IDWriteTextFormat> MakeFormat(const wchar_t* family, DWRITE_FONT_WEIGHT weight, float sizeDip,
                                     DWRITE_TEXT_ALIGNMENT align, bool ellipsis) {
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(win.dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, S(sizeDip), L"", &format))) {
        return nullptr;
    }
    format->SetTextAlignment(align);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (ellipsis) {
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(win.dwrite->CreateEllipsisTrimmingSign(format.Get(), &sign))) {
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            format->SetTrimming(&trimming, sign.Get());
        }
    }
    return format;
}

void FreeFormats() {
    win.titleFormat.Reset();
    win.rowFormat.Reset();
    win.detailFormat.Reset();
    win.iconFormat.Reset();
    win.boxFormat.Reset();
}

void FreeGraphics() {
    if (win.memDC) {
        if (win.oldBitmap) SelectObject(win.memDC, win.oldBitmap);
        DeleteDC(win.memDC);
    }
    if (win.bitmap) DeleteObject(win.bitmap);
    win.memDC = nullptr;
    win.bitmap = nullptr;
    win.oldBitmap = nullptr;
    win.bitmapSize = {};
    FreeFormats();
    win.target.Reset();
    win.dwrite.Reset();
    win.d2d.Reset();
}

bool EnsureGraphics(SIZE size) {
    if (!win.d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, win.d2d.GetAddressOf()))) return false;
    if (!win.dwrite && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                  reinterpret_cast<IUnknown**>(win.dwrite.GetAddressOf())))) {
        return false;
    }
    if (!win.target) {
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(win.d2d->CreateDCRenderTarget(&props, &win.target))) return false;
        win.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    if (!win.titleFormat) {
        constexpr wchar_t text[] = L"Segoe UI Variable Text", icons[] = L"Segoe Fluent Icons";
        win.titleFormat = MakeFormat(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 24, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        win.rowFormat = MakeFormat(text, DWRITE_FONT_WEIGHT_NORMAL, 14, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        win.detailFormat = MakeFormat(text, DWRITE_FONT_WEIGHT_NORMAL, 12, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        win.iconFormat = MakeFormat(icons, DWRITE_FONT_WEIGHT_NORMAL, 16, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        win.boxFormat = MakeFormat(text, DWRITE_FONT_WEIGHT_NORMAL, 14, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    }
    if (!win.bitmap || win.bitmapSize.cx != size.cx || win.bitmapSize.cy != size.cy) {
        if (win.memDC) {
            SelectObject(win.memDC, win.oldBitmap);
            DeleteDC(win.memDC);
        }
        if (win.bitmap) DeleteObject(win.bitmap);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(info.bmiHeader), size.cx, -size.cy, 1, 32, BI_RGB};
        void* bits = nullptr;
        win.memDC = CreateCompatibleDC(nullptr);
        win.bitmap = CreateDIBSection(win.memDC, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!win.memDC || !win.bitmap) return false;
        win.oldBitmap = SelectObject(win.memDC, win.bitmap);
        win.bitmapSize = size;
    }
    return win.titleFormat && win.rowFormat && win.detailFormat && win.iconFormat && win.boxFormat;
}

void Text(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, ID2D1SolidColorBrush* brush) {
    win.target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush,
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Glyph(wchar_t glyph, const D2D1_RECT_F& rect, ID2D1SolidColorBrush* brush) {
    win.target->DrawTextW(&glyph, 1, win.iconFormat.Get(), rect, brush);
}

void RoundedBox(const D2D1_RECT_F& rect, bool focused, ID2D1SolidColorBrush* brush) {
    const auto shape = D2D1::RoundedRect(D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f, rect.right - 0.5f, rect.bottom - 0.5f),
                                         S(kBoxRadius), S(kBoxRadius));
    brush->SetColor(BoxFill());
    win.target->FillRoundedRectangle(shape, brush);
    brush->SetColor(focused ? Accent() : Ink(0.14f));
    win.target->DrawRoundedRectangle(shape, brush, focused ? 1.5f : 1.0f);
}

void DrawNav(ID2D1SolidColorBrush* brush) {
    for (int i = 0; i < static_cast<int>(std::size(kNav)); ++i) {
        const float top = S(kNavTop) + i * S(kNavItem + kNavGap);
        const auto rect = D2D1::RectF(S(kNavInset), top, S(kNavWidth - kNavInset), top + S(kNavItem));
        const bool selected = kNav[i].page == win.page;
        if (selected || win.hoveredNav == i) {
            brush->SetColor(Ink(selected ? 0.08f : 0.05f));
            win.target->FillRoundedRectangle(D2D1::RoundedRect(rect, S(kBoxRadius), S(kBoxRadius)), brush);
        }
        if (selected) {
            const float mid = (rect.top + rect.bottom) / 2;
            brush->SetColor(Accent());
            win.target->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(rect.left, mid - S(8), rect.left + S(3), mid + S(8)), S(1.5f), S(1.5f)), brush);
        }
        brush->SetColor(Ink(0.95f));
        Glyph(kNav[i].glyph, D2D1::RectF(rect.left + S(8), rect.top, rect.left + S(36), rect.bottom), brush);
        Text(kNav[i].label, win.rowFormat.Get(), D2D1::RectF(rect.left + S(44), rect.top, rect.right - S(8), rect.bottom), brush);
    }
}

void DrawToggle(const D2D1_RECT_F& box, bool on, bool hovered, ID2D1SolidColorBrush* brush) {
    const float radius = (box.bottom - box.top) / 2;
    const auto pill = D2D1::RoundedRect(D2D1::RectF(box.left + 0.5f, box.top + 0.5f, box.right - 0.5f, box.bottom - 0.5f),
                                        radius, radius);
    const float mid = (box.top + box.bottom) / 2;
    if (on) {
        brush->SetColor(Accent());
        win.target->FillRoundedRectangle(pill, brush);
        brush->SetColor(win.light ? D2D1::ColorF(1, 1, 1, 1) : D2D1::ColorF(0, 0, 0, 0.9f));
        win.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(box.right - radius, mid), S(hovered ? 7.0f : 6.0f), S(hovered ? 7.0f : 6.0f)), brush);
    } else {
        brush->SetColor(Ink(hovered ? 0.8f : 0.6f));
        win.target->DrawRoundedRectangle(pill, brush, 1.0f);
        win.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(box.left + radius, mid), S(hovered ? 7.0f : 6.0f), S(hovered ? 7.0f : 6.0f)), brush);
    }
}

void DrawRowText(const Item& item, float rightLimit, bool enabled, ID2D1SolidColorBrush* brush) {
    const float left = S(kNavWidth + kPad);
    const bool tall = item.spec->kind == Kind::Text;
    const float titleTop = tall ? item.top + S(2) : item.top + (item.height - S(item.spec->detail ? 38.0f : 20.0f)) / 2;
    brush->SetColor(Ink(enabled ? 0.95f : 0.4f));
    Text(item.spec->title, win.rowFormat.Get(), D2D1::RectF(left, titleTop, rightLimit, titleTop + S(20)), brush);
    if (item.spec->detail) {
        brush->SetColor(Ink(enabled ? 0.62f : 0.32f));
        Text(item.spec->detail, win.detailFormat.Get(), D2D1::RectF(left, titleTop + S(20), rightLimit, titleTop + S(38)), brush);
    }
}

void DrawPage(ID2D1SolidColorBrush* brush) {
    const Settings settings = win.host.get();
    const float left = S(kNavWidth + kPad), right = S(kClientWidth - kPad);
    const wchar_t* title = win.page == Page::General ? L"General" : win.page == Page::Apps ? L"Apps" : L"Hotkeys";
    brush->SetColor(Ink(0.95f));
    Text(title, win.titleFormat.Get(), D2D1::RectF(left, S(kTitleTop), right, S(kTitleTop + kTitleHeight)), brush);

    for (int i = 0; i < static_cast<int>(win.items.size()); ++i) {
        const Item& item = win.items[i];
        const bool hovered = win.hoveredItem == i;
        switch (item.spec->kind) {
        case Kind::Toggle: {
            if (hovered) { // the whole row is the button
                brush->SetColor(Ink(win.pressedItem == i ? 0.03f : 0.05f));
                win.target->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(left - S(12), item.top + S(2), right + S(12), item.top + item.height - S(2)),
                                      S(kBoxRadius), S(kBoxRadius)), brush);
            }
            DrawRowText(item, item.box.left - S(16), true, brush);
            DrawToggle(item.box, ToggleValue(item.spec->field, settings), hovered, brush);
            break;
        }
        case Kind::Number: {
            const bool enabled = !settings.stayVisibleWhenPaused;
            DrawRowText(item, item.box.left - S(16), enabled, brush);
            RoundedBox(item.box, item.edit && item.edit == win.focusedEdit, brush);
            break;
        }
        case Kind::Text:
            DrawRowText(item, right, true, brush);
            RoundedBox(item.box, item.edit && item.edit == win.focusedEdit, brush);
            break;
        case Kind::Hotkey: {
            DrawRowText(item, item.box.left - S(16), true, brush);
            const bool capturing = win.capturing == i;
            RoundedBox(item.box, capturing, brush);
            const std::wstring* text = nullptr;
            Settings copy = settings;
            text = HotkeyOf(copy, item.spec->field);
            const bool set = text && !text->empty();
            const std::wstring shown = capturing ? (win.captureHint.empty() ? L"Press a key combination" : win.captureHint)
                                       : set     ? *text
                                                 : L"Not set";
            brush->SetColor(capturing ? (win.captureHint.empty() ? Accent() : D2D1::ColorF(1.0f, 0.55f, 0.45f, 1))
                                      : Ink(set ? 0.95f : 0.5f));
            Text(shown, win.boxFormat.Get(),
                 D2D1::RectF(item.box.left + S(kBoxPad), item.box.top, item.clear.left, item.box.bottom), brush);
            if (set && !capturing) {
                if (win.hoveredItem == i && win.hoveredClear) {
                    brush->SetColor(Ink(win.pressedClear ? 0.05f : 0.1f));
                    win.target->FillRoundedRectangle(D2D1::RoundedRect(item.clear, S(4), S(4)), brush);
                }
                brush->SetColor(Ink(0.8f));
                Glyph(kGlyphClear, item.clear, brush);
            }
            break;
        }
        case Kind::Link:
            brush->SetColor(hovered ? Ink(0.95f) : Accent());
            Text(item.spec->title, win.rowFormat.Get(), D2D1::RectF(left, item.top, right, item.top + item.height), brush);
            break;
        case Kind::Note:
            brush->SetColor(Ink(0.62f));
            Text(item.spec->title, win.detailFormat.Get(), D2D1::RectF(left, item.top, right, item.top + item.height), brush);
            break;
        }
    }
}

void Paint(HDC windowDC) {
    RECT client;
    GetClientRect(win.hwnd, &client);
    const SIZE size{client.right, client.bottom};
    if (size.cx <= 0 || size.cy <= 0 || !EnsureGraphics(size)) return;
    RECT bounds{0, 0, size.cx, size.cy};
    if (FAILED(win.target->BindDC(win.memDC, &bounds))) return;

    ComPtr<ID2D1SolidColorBrush> brush;
    win.target->CreateSolidColorBrush(Ink(1.0f), &brush);
    win.target->BeginDraw();
    win.target->Clear(WindowFill());
    if (brush) {
        DrawNav(brush.Get());
        DrawPage(brush.Get());
    }
    if (win.target->EndDraw() == D2DERR_RECREATE_TARGET) win.target.Reset();
    BitBlt(windowDC, 0, 0, size.cx, size.cy, win.memDC, 0, 0, SRCCOPY);
}

// --- Mouse and keyboard -----------------------------------------------------

int NavAt(POINT pt) {
    for (int i = 0; i < static_cast<int>(std::size(kNav)); ++i) {
        const float top = S(kNavTop) + i * S(kNavItem + kNavGap);
        if (Contains(D2D1::RectF(S(kNavInset), top, S(kNavWidth - kNavInset), top + S(kNavItem)), pt)) return i;
    }
    return -1;
}

// The row under the point, if it's one that reacts to clicks.
int ItemAt(POINT pt, bool& onClear) {
    onClear = false;
    for (int i = 0; i < static_cast<int>(win.items.size()); ++i) {
        const Item& item = win.items[i];
        switch (item.spec->kind) {
        case Kind::Toggle:
        case Kind::Link:
            if (pt.y >= item.top && pt.y < item.top + item.height && pt.x >= S(kNavWidth)) return i;
            break;
        case Kind::Hotkey:
            if (Contains(item.box, pt)) {
                onClear = Contains(item.clear, pt);
                return i;
            }
            break;
        default: break;
        }
    }
    return -1;
}

void SetPage(Page page) {
    if (win.capturing >= 0) StopCapture();
    win.page = page;
    BuildPage();
    SyncEdits();
    Redraw();
}

void OnLeftUp(POINT pt) {
    const int pressed = win.pressedItem;
    const bool pressedClear = win.pressedClear;
    win.pressedItem = -1;
    win.pressedClear = false;
    if (GetCapture() == win.hwnd) ReleaseCapture();
    if (pressed >= 0 && pressed < static_cast<int>(win.items.size())) {
        bool onClear = false;
        if (ItemAt(pt, onClear) == pressed && onClear == pressedClear) {
            const Field field = win.items[pressed].spec->field;
            switch (win.items[pressed].spec->kind) {
            case Kind::Toggle:
                if (field == Field::StartWithWindows) {
                    SetStartWithWindows(!StartsWithWindows());
                } else {
                    ChangeSettings([&](Settings& s) { FlipToggle(field, s); });
                }
                break;
            case Kind::Link:
                if (win.host.openFile) win.host.openFile();
                break;
            case Kind::Hotkey:
                if (pressedClear) {
                    ChangeSettings([&](Settings& s) { if (auto* hotkey = HotkeyOf(s, field)) hotkey->clear(); });
                } else {
                    StartCapture(pressed);
                }
                break;
            default: break;
            }
        }
    }
    SyncEdits();
    Redraw();
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; // Paint covers everything
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!win.trackingMouse) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            win.trackingMouse = TrackMouseEvent(&track);
        }
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        bool onClear = false;
        const int item = ItemAt(pt, onClear), nav = NavAt(pt);
        if (item != win.hoveredItem || nav != win.hoveredNav || onClear != win.hoveredClear) {
            win.hoveredItem = item;
            win.hoveredNav = nav;
            win.hoveredClear = onClear;
            Redraw();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        win.trackingMouse = false;
        win.hoveredItem = win.hoveredNav = -1;
        win.hoveredClear = false;
        Redraw();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            bool onClear = false;
            const bool hand = ItemAt(pt, onClear) >= 0 || NavAt(pt) >= 0;
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetFocus(hwnd); // takes focus from a text box, which commits what was typed
        if (const int nav = NavAt(pt); nav >= 0) {
            if (kNav[nav].page != win.page) SetPage(kNav[nav].page);
            return 0;
        }
        bool onClear = false;
        const int item = ItemAt(pt, onClear);
        if (win.capturing >= 0 && item != win.capturing) StopCapture();
        if (item >= 0) {
            win.pressedItem = item;
            win.pressedClear = onClear;
            SetCapture(hwnd);
        }
        Redraw();
        return 0;
    }
    case WM_LBUTTONUP:
        OnLeftUp({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    case WM_CAPTURECHANGED:
        if (win.pressedItem >= 0) {
            win.pressedItem = -1;
            win.pressedClear = false;
            Redraw();
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (win.capturing >= 0) {
            OnCaptureKey(static_cast<UINT>(wp));
            return 0;
        }
        if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_SYSCHAR:
    case WM_CHAR:
        if (win.capturing >= 0) return 0; // no beep for Alt+key
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU && win.capturing >= 0) return 0; // Alt alone must not open the menu
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) StopCapture(); // global hotkeys come back
        return 0;
    case WM_KILLFOCUS:
        StopCapture();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) >= kEditIdFirst) {
            auto* edit = reinterpret_cast<HWND>(lp);
            if (HIWORD(wp) == EN_SETFOCUS) {
                win.focusedEdit = edit;
                StopCapture();
                Redraw();
            } else if (HIWORD(wp) == EN_KILLFOCUS) {
                if (win.focusedEdit == edit) win.focusedEdit = nullptr;
                CommitEdit(edit);
                Redraw();
            }
        }
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, msg == WM_CTLCOLORSTATIC ? (win.light ? RGB(130, 130, 130) : RGB(140, 140, 140)) : EditTextRef());
        SetBkColor(dc, BoxFillRef());
        if (!win.editBrush) win.editBrush = CreateSolidBrush(BoxFillRef());
        return reinterpret_cast<LRESULT>(win.editBrush);
    }
    case WM_DPICHANGED: {
        win.dpi = HIWORD(wp);
        FreeFormats();
        const RECT& suggested = *reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                     suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
        BuildPage();
        SyncEdits();
        Redraw();
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (hwnd == win.hwnd) {
            StopCapture();
            DestroyEdits();
            if (win.font) DeleteObject(win.font);
            if (win.editBrush) DeleteObject(win.editBrush);
            win.font = nullptr;
            win.editBrush = nullptr;
            win.hwnd = nullptr;
            win.items.clear();
            win.hoveredItem = win.hoveredNav = win.pressedItem = -1;
            win.trackingMouse = false;
            FreeGraphics();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ApplyTheme() {
    const BOOL dark = !win.light;
    DwmSetWindowAttribute(win.hwnd, kUseImmersiveDarkMode, &dark, sizeof(dark));
    // The title bar in the window's own color, so there's no seam under it.
    const COLORREF fill = WindowFillRef(), text = win.light ? RGB(0, 0, 0) : RGB(255, 255, 255);
    DwmSetWindowAttribute(win.hwnd, kCaptionColor, &fill, sizeof(fill));
    DwmSetWindowAttribute(win.hwnd, kBorderColor, &fill, sizeof(fill));
    DwmSetWindowAttribute(win.hwnd, kCaptionTextColor, &text, sizeof(text));
    if (win.editBrush) {
        DeleteObject(win.editBrush);
        win.editBrush = nullptr; // rebuilt in the new color
    }
    for (auto& item : win.items) {
        if (item.edit) InvalidateRect(item.edit, nullptr, TRUE);
    }
}

} // namespace

void OpenSettingsWindow(HINSTANCE instance, UINT dpi, SettingsHost host) {
    if (win.hwnd) {
        if (IsIconic(win.hwnd)) ShowWindow(win.hwnd, SW_RESTORE);
        SetForegroundWindow(win.hwnd);
        return;
    }
    static bool registered = [instance] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpfnWndProc = SettingsProc;
        wc.lpszClassName = kClass;
        return RegisterClassExW(&wc) != 0;
    }();
    if (!registered) return;

    win.host = std::move(host);
    win.dpi = dpi;
    win.light = win.host.lightTheme ? win.host.lightTheme() : false;
    win.page = Page::General;

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT frame{0, 0, Scale(kClientWidth), Scale(kClientHeight)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const int width = frame.right - frame.left, height = frame.bottom - frame.top;
    const int x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
    const int y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
    win.hwnd = CreateWindowExW(0, kClass, L"TaskbarMedia settings", style, x, y, width, height, nullptr, nullptr, instance,
                               nullptr);
    if (!win.hwnd) return;

    ApplyTheme();

    BuildPage();
    SyncEdits();
    ShowWindow(win.hwnd, SW_SHOW);
    SetForegroundWindow(win.hwnd);
}

void CloseSettingsWindow() {
    if (win.hwnd) DestroyWindow(win.hwnd);
}

void SettingsWindowRefresh() {
    if (!win.hwnd) return;
    const bool light = win.host.lightTheme ? win.host.lightTheme() : win.light;
    if (light != win.light) {
        win.light = light;
        ApplyTheme();
        InvalidateRect(win.hwnd, nullptr, FALSE);
    }
    SyncEdits();
    Redraw();
}
