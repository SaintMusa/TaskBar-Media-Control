// TaskbarMedia: a media widget that lives on the Windows 11 taskbar.
//
// A small translucent box is attached as a child window of the taskbar
// (no injection, no hooks into Explorer) in the empty space left of the
// centered icons, and shows what's playing. Everything is event-driven:
// no polling timers.

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>
#include "app_info.h"
#include "media_watcher.h"
#include "settings.h"
#include "taskbar_watcher.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kControllerClass[] = L"TaskbarMedia.Controller";
constexpr wchar_t kWidgetClass[] = L"TaskbarMedia.Widget";

enum : UINT {
    WM_APP_GEOMETRY = WM_APP + 1, // watcher has fresh geometry
    WM_APP_TASKBAR_CHANGED,       // UIA says the layout moved; re-read soon
    WM_APP_MEDIA,                 // what's playing changed
    WM_APP_APPBAR,                // shell notifications (full screen apps)
};
enum : UINT_PTR {
    kTimerRelayout = 1,
    kTimerAttachRetry,
    kTimerProgress,
    kTimerIdleHide,
    kTimerWheelRest,
    kTimerMarquee,
    kTimerSettingsReload,
};
enum : UINT { kCmdExit = 1, kCmdStayVisible, kCmdStartWithWindows, kCmdOpenSettings };

constexpr UINT kRelayoutDelayMs = 120; // coalesces bursts while icons animate
constexpr UINT kAttachRetryMs = 1000;  // only while Explorer is (re)starting
constexpr UINT kWheelRestMs = 300;     // wheel quiet this long = the scroll gesture ended
constexpr UINT kSettingsSettleMs = 200; // editors save in several writes; reload once after
constexpr int kPreferredWidth = 300;   // all sizes in DIPs (96 DPI pixels)
constexpr int kMinWidth = 160;
constexpr int kMargin = 8;
constexpr int kVerticalInset = 4;
constexpr int kCornerRadius = 6;
constexpr int kInnerPadding = 4;       // box edge to art and buttons
constexpr int kGap = 8;                // art to text, text to buttons
constexpr int kButtonWidth = 28;
constexpr int kArtRadius = 4;
constexpr int kSeekbarThickness = 3;
constexpr UINT kMarqueePauseMs = 2000; // long text rests this long at each end
constexpr int kMarqueeSpeed = 30;      // DIPs per second
constexpr UINT kMarqueeFrameMs = 33;   // ~30 fps, only while text is actually moving
constexpr int kMarqueeFade = 12;       // DIPs of fade where text runs past the edge
constexpr float kTitleFontSize = 12.0f;
constexpr float kArtistFontSize = 11.0f;
constexpr float kIconFontSize = 14.0f;

// Segoe Fluent Icons code points.
constexpr wchar_t kGlyphPrevious = 0xE892;
constexpr wchar_t kGlyphNext = 0xE893;
constexpr wchar_t kGlyphPlay = 0xE768;
constexpr wchar_t kGlyphPause = 0xE769;
constexpr wchar_t kGlyphMusic = 0xE8D6;

constexpr wchar_t kGlyphSwitchApp = 0xE76C; // chevron, like Windows' own media flyout

// kButtonSwitchApp only exists while two or more apps have media.
enum Button { kButtonPrevious, kButtonPlayPause, kButtonNext, kButtonSwitchApp, kButtonCount, kNoButton = -1 };
constexpr int kPressedArt = 100; // g.pressed value while the cover art is held down

struct WidgetLayout {
    D2D1_RECT_F art;
    D2D1_RECT_F text;
    D2D1_RECT_F buttons[kButtonCount];
    D2D1_RECT_F seekbar;
};

// A line of text too long for its space, found while drawing.
struct OverflowLine {
    std::wstring text;
    ComPtr<IDWriteTextLayout> layout;
    D2D1_RECT_F rect;
    D2D1_COLOR_F color;
    float width;
};

// That line rendered once into its own pixels. Scrolling then only copies
// pixels at a new offset: no text layout or Direct2D work per frame.
struct Marquee {
    std::vector<uint32_t> pixels; // premultiplied BGRA, stripW x stripH
    int stripW = 0;
    int stripH = 0;
    RECT view{};    // where it shows inside the widget
    int travel = 0; // how far it slides: stripW minus the view's width
};

enum class Redraw {
    Full,    // something real changed: redraw everything
    Seekbar, // the bar moved a pixel: reuse the static parts
    Marquee, // long text slid: reuse everything, just move the text strips
};

struct State {
    HINSTANCE instance = nullptr;
    HWND controller = nullptr;
    HWND widget = nullptr;
    HWND taskbar = nullptr;
    UINT taskbarCreatedMsg = 0;
    TaskbarWatcher watcher;
    MediaWatcher media;
    MediaInfo nowPlaying;

    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<IDWriteTextFormat> titleFormat;
    ComPtr<IDWriteTextFormat> artistFormat;
    ComPtr<IDWriteTextFormat> iconFormat;
    UINT textFormatDpi = 0;
    ComPtr<ID2D1Bitmap> artBitmap;           // nowPlaying.art, uploaded for drawing
    const MediaArt* artBitmapSource = nullptr;
    ComPtr<ID2D1Bitmap> iconBitmap;          // nowPlaying.appIcon, uploaded for drawing
    const MediaArt* iconBitmapSource = nullptr;

    int hovered = kNoButton;
    int pressed = kNoButton;
    bool trackingMouse = false;
    bool wheelGesture = false; // a scroll is in progress and has already switched

    bool fits = false;       // the taskbar has room for us
    bool fullscreen = false; // a full screen app or game is in front
    bool idleHidden = false; // media has been paused for settings.hideAfterPausedSeconds
    Settings settings;

    HDC memDC = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    void* dibBits = nullptr;
    SIZE bitmapSize{};
    std::vector<BYTE> staticPixels; // last full draw minus the seekbar and scrolling text
    std::vector<BYTE> basePixels;   // staticPixels plus the seekbar
    std::vector<OverflowLine> overflowing;
    std::vector<Marquee> marquees;
    std::wstring marqueeKey;        // the scrolling texts; a change restarts the cycle
    ULONGLONG marqueeStart = 0;
    bool marqueeRunning = false;
    ComPtr<ID2D1RoundedRectangleGeometry> boxClip;
    D2D1_SIZE_F boxClipSize{};
    ComPtr<ID2D1Layer> clipLayer;

    RECT placed{}; // widget rect, relative to the taskbar
    UINT dpi = 96;
    bool lightTheme = false;

    FILE* log = nullptr;
    unsigned layoutEvents = 0;
    unsigned renders = 0;        // for measuring: count and time spent drawing
    unsigned seekbarRenders = 0; // the cheap seekbar-only kind
    unsigned marqueeRenders = 0; // the cheapest: scrolling text frames
    int64_t drawTicks = 0;       // Direct2D, QueryPerformanceCounter units
    int64_t seekbarTicks = 0;
    int64_t marqueeTicks = 0;
    int64_t presentTicks = 0;    // UpdateLayeredWindow
} g;

void Log(const wchar_t* format, ...) {
    if (!g.log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(g.log, L"%02u:%02u:%02u.%03u  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, format);
    vfwprintf(g.log, format, args);
    va_end(args);
    fputwc(L'\n', g.log);
    fflush(g.log);
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    wchar_t* dot = wcsrchr(path, L'.');
    if (!dot) return;
    wcscpy_s(dot, MAX_PATH - (dot - path), L".log");
    g.log = _wfsopen(path, L"w, ccs=UTF-8", _SH_DENYNO); // readable while we run
}

int Scale(int dip) { return MulDiv(dip, static_cast<int>(g.dpi), 96); }

// Same clock and units as the media timeline's report times.
int64_t NowFileTime() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

bool ReadLightTheme() {
    DWORD value = 0, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value != 0;
}

// ---------------------------------------------------------------------------
// Drawing: Direct2D in software mode into a DIB, handed to the window with
// UpdateLayeredWindow for per-pixel alpha. No GPU device, no swap chain.

void FreeBitmap() {
    if (g.memDC) {
        if (g.oldBitmap) SelectObject(g.memDC, g.oldBitmap);
        DeleteDC(g.memDC);
    }
    if (g.bitmap) DeleteObject(g.bitmap);
    g.memDC = nullptr;
    g.bitmap = nullptr;
    g.oldBitmap = nullptr;
    g.dibBits = nullptr;
    g.bitmapSize = {};
    g.staticPixels.clear();
}

bool EnsureBitmap(SIZE size) {
    if (g.bitmap && g.bitmapSize.cx == size.cx && g.bitmapSize.cy == size.cy) return true;
    FreeBitmap();
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size.cx;
    info.bmiHeader.biHeight = -size.cy; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    g.memDC = CreateCompatibleDC(nullptr);
    g.bitmap = CreateDIBSection(g.memDC, &info, DIB_RGB_COLORS, &g.dibBits, nullptr, 0);
    if (!g.memDC || !g.bitmap) {
        FreeBitmap();
        return false;
    }
    g.oldBitmap = SelectObject(g.memDC, g.bitmap);
    g.bitmapSize = size;
    return true;
}

// One line of left-aligned text that ends in "…" when it doesn't fit.
ComPtr<IDWriteTextFormat> CreateLineFormat(DWRITE_FONT_WEIGHT weight, float sizeDip) {
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(g.dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, sizeDip * g.dpi / 96.0f, L"", &format))) {
        return nullptr;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(g.dwrite->CreateEllipsisTrimmingSign(format.Get(), &ellipsis))) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        format->SetTrimming(&trimming, ellipsis.Get());
    }
    return format;
}

bool EnsureGraphics() {
    if (!g.d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g.d2d.GetAddressOf()))) return false;
    if (!g.dwrite && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                reinterpret_cast<IUnknown**>(g.dwrite.GetAddressOf())))) {
        return false;
    }
    if (!g.target) {
        // 96 DPI so one DIP is one pixel; we scale sizes ourselves from g.dpi.
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(g.d2d->CreateDCRenderTarget(&props, &g.target))) return false;
        // ClearType needs an opaque background; grayscale works on transparent pixels.
        g.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    if (!g.titleFormat || !g.artistFormat || !g.iconFormat || g.textFormatDpi != g.dpi) {
        g.titleFormat = CreateLineFormat(DWRITE_FONT_WEIGHT_SEMI_BOLD, kTitleFontSize);
        g.artistFormat = CreateLineFormat(DWRITE_FONT_WEIGHT_NORMAL, kArtistFontSize);
        g.iconFormat.Reset();
        if (SUCCEEDED(g.dwrite->CreateTextFormat(L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                 kIconFontSize * g.dpi / 96.0f, L"", &g.iconFormat))) {
            g.iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            g.iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (!g.titleFormat || !g.artistFormat || !g.iconFormat) return false;
        g.textFormatDpi = g.dpi;
    }
    return true;
}

// Art on the left, the buttons on the right, text in between. The switch-app
// button takes room from the text only while there's another app to switch to.
WidgetLayout ComputeLayout(float w, float h) {
    const float pad = static_cast<float>(Scale(kInnerPadding));
    const float gap = static_cast<float>(Scale(kGap));
    const float buttonW = static_cast<float>(Scale(kButtonWidth));
    const int shown = g.nowPlaying.sessionCount > 1 ? kButtonCount : kButtonSwitchApp;
    WidgetLayout layout{};
    layout.art = D2D1::RectF(pad, pad, pad + (h - 2 * pad), h - pad);
    float x = w - pad - buttonW * static_cast<float>(shown);
    for (int i = 0; i < shown; ++i) {
        layout.buttons[i] = D2D1::RectF(x, pad, x + buttonW, h - pad);
        x += buttonW;
    }
    layout.text = D2D1::RectF(layout.art.right + gap, 0, layout.buttons[0].left - gap / 2, h);
    // Full width along the bottom edge; DrawSeekbar clips it to the rounded corners.
    const float thickness = static_cast<float>(Scale(kSeekbarThickness));
    layout.seekbar = D2D1::RectF(0, h - thickness, w, h);
    return layout;
}

bool ButtonEnabled(int button) {
    switch (button) {
    case kButtonPrevious: return g.nowPlaying.canPrevious;
    case kButtonPlayPause: return g.nowPlaying.canPlayPause;
    case kButtonNext: return g.nowPlaying.canNext;
    case kButtonSwitchApp: return g.nowPlaying.sessionCount > 1;
    }
    return false;
}

bool HitArt(POINT pt) {
    const float w = static_cast<float>(g.placed.right - g.placed.left);
    const float h = static_cast<float>(g.placed.bottom - g.placed.top);
    const auto r = ComputeLayout(w, h).art;
    return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
}

int HitButton(POINT pt) {
    const float w = static_cast<float>(g.placed.right - g.placed.left);
    const float h = static_cast<float>(g.placed.bottom - g.placed.top);
    WidgetLayout layout = ComputeLayout(w, h);
    for (int i = 0; i < kButtonCount; ++i) {
        const auto& r = layout.buttons[i];
        if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) return i;
    }
    return kNoButton;
}

// Uploads an image to a Direct2D bitmap once, when it changes (new track or app).
ID2D1Bitmap* Uploaded(const MediaArt* image, ComPtr<ID2D1Bitmap>& bitmap, const MediaArt*& uploadedFrom) {
    if (image != uploadedFrom) {
        bitmap.Reset();
        uploadedFrom = image;
        if (image) {
            auto props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            g.target->CreateBitmap(D2D1::SizeU(image->size, image->size), image->pixels.data(), image->size * 4,
                                   props, &bitmap);
        }
    }
    return bitmap.Get();
}

ID2D1Bitmap* ArtBitmap() { return Uploaded(g.nowPlaying.art.get(), g.artBitmap, g.artBitmapSource); }
ID2D1Bitmap* IconBitmap() { return Uploaded(g.nowPlaying.appIcon.get(), g.iconBitmap, g.iconBitmapSource); }

D2D1_COLOR_F InkAt(float alpha) {
    return g.lightTheme ? D2D1::ColorF(0, 0, 0, alpha) : D2D1::ColorF(1, 1, 1, alpha);
}

// Draws a line of text that fits. One that doesn't is left out and queued in
// g.overflowing, to be drawn as a scrolling strip instead.
void DrawLine(const std::wstring& text, IDWriteTextFormat* format, D2D1_RECT_F rect, ID2D1SolidColorBrush* ink) {
    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(g.dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 100000.0f,
                                          rect.bottom - rect.top, &layout)) ||
        FAILED(layout->GetMetrics(&metrics))) {
        return;
    }
    if (metrics.width <= rect.right - rect.left) {
        g.target->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), ink, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        return;
    }
    g.overflowing.push_back({text, layout, rect, ink->GetColor(), metrics.width});
}

// Box, art, text and buttons: everything except the seekbar.
void DrawStatic(const WidgetLayout& layout, float w, float h) {
    const MediaInfo& media = g.nowPlaying;
    // Paused media is drawn dimmer so the state reads at a glance.
    const float textAlpha = media.playing ? 0.92f : 0.6f;
    ComPtr<ID2D1SolidColorBrush> fill, border, titleInk, artistInk, paint;
    g.target->CreateSolidColorBrush(InkAt(0.08f), &fill);
    g.target->CreateSolidColorBrush(InkAt(0.12f), &border);
    g.target->CreateSolidColorBrush(InkAt(textAlpha), &titleInk);
    g.target->CreateSolidColorBrush(InkAt(textAlpha * 0.7f), &artistInk);
    g.target->CreateSolidColorBrush(InkAt(1.0f), &paint); // recolored per use

    g.target->Clear(D2D1::ColorF(0, 0, 0, 0));
    const float radius = static_cast<float>(Scale(kCornerRadius));
    auto box = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), radius, radius);
    g.target->FillRoundedRectangle(box, fill.Get());
    g.target->DrawRoundedRectangle(box, border.Get(), 1.0f);

    // Cover art with rounded corners, or a music note when the app sends none.
    const float artRadius = static_cast<float>(Scale(kArtRadius));
    const auto artBox = D2D1::RoundedRect(layout.art, artRadius, artRadius);
    ComPtr<ID2D1BitmapBrush> artBrush;
    if (ID2D1Bitmap* bitmap = ArtBitmap(); bitmap && SUCCEEDED(g.target->CreateBitmapBrush(bitmap, &artBrush))) {
        const float side = layout.art.right - layout.art.left;
        const auto source = bitmap->GetSize();
        artBrush->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        artBrush->SetTransform(D2D1::Matrix3x2F::Scale(side / source.width, side / source.height) *
                               D2D1::Matrix3x2F::Translation(layout.art.left, layout.art.top));
        artBrush->SetOpacity(media.playing ? 1.0f : 0.7f);
        g.target->FillRoundedRectangle(artBox, artBrush.Get());
    } else {
        g.target->FillRoundedRectangle(artBox, fill.Get());
        g.target->DrawTextW(&kGlyphMusic, 1, g.iconFormat.Get(), layout.art, artistInk.Get());
    }

    // The playing app's icon as a badge on the art's bottom-right corner, in its
    // own shape (round for Spotify, rounded square for Helium), like Windows' media flyout.
    if (ID2D1Bitmap* icon = IconBitmap()) {
        const auto size = icon->GetSize();
        const auto badge = D2D1::RectF(layout.art.right - size.width, layout.art.bottom - size.height,
                                       layout.art.right, layout.art.bottom);
        g.target->DrawBitmap(icon, badge, media.playing ? 1.0f : 0.7f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    const std::wstring title = media.appId.empty() ? L"Nothing playing"
                               : media.title.empty() ? L"Untitled"
                                                     : media.title;
    const auto& text = layout.text;
    g.overflowing.clear();
    if (media.artist.empty()) {
        DrawLine(title, g.titleFormat.Get(), text, titleInk.Get());
    } else {
        DrawLine(title, g.titleFormat.Get(), D2D1::RectF(text.left, h * 0.08f, text.right, h * 0.54f), titleInk.Get());
        DrawLine(media.artist, g.artistFormat.Get(), D2D1::RectF(text.left, h * 0.5f, text.right, h * 0.92f),
                 artistInk.Get());
    }

    // Buttons: a soft highlight on hover, dimmer while pressed, faint when the app disallows them.
    const wchar_t glyphs[kButtonCount] = {kGlyphPrevious, media.playing ? kGlyphPause : kGlyphPlay, kGlyphNext,
                                          kGlyphSwitchApp};
    const float buttonRadius = static_cast<float>(Scale(kArtRadius));
    for (int i = 0; i < kButtonCount; ++i) {
        if (i == kButtonSwitchApp && media.sessionCount < 2) continue;
        const bool enabled = ButtonEnabled(i);
        const bool isPressed = enabled && g.pressed == i;
        if (enabled && g.hovered == i) {
            paint->SetColor(InkAt(isPressed ? 0.05f : 0.09f));
            g.target->FillRoundedRectangle(D2D1::RoundedRect(layout.buttons[i], buttonRadius, buttonRadius),
                                           paint.Get());
        }
        paint->SetColor(InkAt(!enabled ? 0.3f : isPressed ? 0.7f : 0.92f));
        g.target->DrawTextW(&glyphs[i], 1, g.iconFormat.Get(), layout.buttons[i], paint.Get());
    }
}

// Seekbar along the bottom edge, clipped to the box's rounded outline so its
// ends follow the corner curves. Display only. The filled part is at full
// brightness so it reads at a glance against the faint track.
void DrawSeekbar(const WidgetLayout& layout, float w, float h) {
    const MediaInfo& media = g.nowPlaying;
    if (!media.hasTimeline) return;
    if (!g.boxClip || g.boxClipSize.width != w || g.boxClipSize.height != h) {
        const float radius = static_cast<float>(Scale(kCornerRadius));
        g.boxClip.Reset();
        g.d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), radius, radius),
                                              &g.boxClip);
        g.boxClipSize = D2D1::SizeF(w, h);
    }
    if (!g.clipLayer) g.target->CreateLayer(&g.clipLayer);
    ComPtr<ID2D1SolidColorBrush> paint;
    g.target->CreateSolidColorBrush(InkAt(0.18f), &paint);
    if (!g.boxClip || !g.clipLayer || !paint) return;

    g.target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g.boxClip.Get()), g.clipLayer.Get());
    const auto& track = layout.seekbar;
    g.target->FillRectangle(track, paint.Get());
    auto filled = track;
    const float progress = static_cast<float>(media.PositionAt(NowFileTime())) / media.durationMs;
    filled.right = track.left + (track.right - track.left) * progress;
    paint->SetColor(InkAt(media.playing ? 1.0f : 0.65f));
    g.target->FillRectangle(filled, paint.Get());
    g.target->PopLayer();
}

// Renders each overflowing line once into its own strip of pixels.
void BuildMarquees() {
    std::wstring key;
    for (const auto& line : g.overflowing) key += line.text + L'\n';
    if (key != g.marqueeKey) {
        g.marqueeKey = key;
        g.marqueeStart = GetTickCount64(); // new text starts from the beginning
    }
    g.marquees.clear();
    for (const auto& line : g.overflowing) {
        Marquee m;
        m.view = {static_cast<LONG>(std::ceil(line.rect.left)), static_cast<LONG>(std::floor(line.rect.top)),
                  static_cast<LONG>(std::floor(line.rect.right)), static_cast<LONG>(std::ceil(line.rect.bottom))};
        m.stripW = static_cast<int>(std::ceil(line.width)) + 2;
        m.stripH = m.view.bottom - m.view.top;
        m.travel = m.stripW - (m.view.right - m.view.left);
        if (m.stripH <= 0 || m.travel <= 0) continue;

        BITMAPINFO info{};
        info.bmiHeader = {sizeof(info.bmiHeader), m.stripW, -m.stripH, 1, 32, BI_RGB};
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP strip = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = strip ? SelectObject(dc, strip) : nullptr;
        RECT bounds{0, 0, m.stripW, m.stripH};
        ComPtr<ID2D1SolidColorBrush> ink;
        if (strip && SUCCEEDED(g.target->BindDC(dc, &bounds)) &&
            SUCCEEDED(g.target->CreateSolidColorBrush(line.color, &ink))) {
            g.target->BeginDraw();
            g.target->Clear(D2D1::ColorF(0, 0, 0, 0));
            g.target->DrawTextLayout(D2D1::Point2F(0, line.rect.top - m.view.top), line.layout.Get(), ink.Get());
            if (SUCCEEDED(g.target->EndDraw())) {
                GdiFlush();
                const auto* px = static_cast<const uint32_t*>(bits);
                m.pixels.assign(px, px + static_cast<size_t>(m.stripW) * m.stripH);
                g.marquees.push_back(std::move(m));
            }
        }
        if (old) SelectObject(dc, old);
        if (strip) DeleteObject(strip);
        DeleteDC(dc);
    }
}

// Back and forth: rest, slide to the end, rest, slide back. Also reports how
// long until the next frame is needed (0 = it's sliding, so draw soon).
int MarqueeOffset(const Marquee& m, ULONGLONG elapsed, ULONGLONG* wait) {
    const double pxPerMs = Scale(kMarqueeSpeed) / 1000.0;
    const ULONGLONG slide = static_cast<ULONGLONG>(m.travel / pxPerMs);
    ULONGLONG t = elapsed % (2 * (kMarqueePauseMs + slide));
    *wait = 0;
    if (t < kMarqueePauseMs) {
        *wait = kMarqueePauseMs - t;
        return 0;
    }
    t -= kMarqueePauseMs;
    if (t < slide) return static_cast<int>(t * pxPerMs);
    t -= slide;
    if (t < kMarqueePauseMs) {
        *wait = kMarqueePauseMs - t;
        return m.travel;
    }
    t -= kMarqueePauseMs;
    return m.travel - static_cast<int>(t * pxPerMs);
}

// Premultiplied "source over destination" for one BGRA pixel.
uint32_t Over(uint32_t s, uint32_t d) {
    const uint32_t keep = 255 - (s >> 24);
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const uint32_t c = ((s >> shift) & 255) + (((d >> shift) & 255) * keep + 127) / 255;
        out |= std::min<uint32_t>(c, 255) << shift;
    }
    return out;
}

uint32_t Faded(uint32_t s, int coverage /* 0..256 */) {
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) out |= (((s >> shift) & 255) * coverage >> 8) << shift;
    return out;
}

// Copies each strip into the frame at its current offset, fading the edge
// where the text continues out of view.
void BlendMarquees() {
    auto* frame = static_cast<uint32_t*>(g.dibBits);
    const int stride = g.bitmapSize.cx;
    const int fade = std::max(1, Scale(kMarqueeFade));
    const ULONGLONG elapsed = GetTickCount64() - g.marqueeStart;
    for (const auto& m : g.marquees) {
        ULONGLONG wait;
        const int offset = g.marqueeRunning ? MarqueeOffset(m, elapsed, &wait) : 0;
        const int viewW = m.view.right - m.view.left;
        for (int y = 0; y < m.stripH; ++y) {
            const int fy = m.view.top + y;
            if (fy < 0 || fy >= g.bitmapSize.cy) continue;
            for (int x = 0; x < viewW && x + offset < m.stripW; ++x) {
                uint32_t s = m.pixels[static_cast<size_t>(y) * m.stripW + x + offset];
                if (!s) continue;
                int coverage = 256;
                if (offset > 0 && x < fade) coverage = coverage * x / fade;
                if (offset < m.travel && x >= viewW - fade) coverage = coverage * (viewW - 1 - x) / fade;
                if (coverage < 256) s = Faded(s, coverage);
                uint32_t& d = frame[static_cast<size_t>(fy) * stride + m.view.left + x];
                d = Over(s, d);
            }
        }
    }
}

// Ticks only while long text is actually sliding; during the rests at each
// end it sleeps until the next slide. Stopped while paused, hidden or full screen.
void ScheduleMarquee() {
    KillTimer(g.controller, kTimerMarquee);
    const bool run = g.widget && IsWindowVisible(g.widget) && g.nowPlaying.playing && !g.fullscreen &&
                     !g.marquees.empty();
    if (!run) {
        g.marqueeRunning = false;
        return;
    }
    if (!g.marqueeRunning) {
        g.marqueeRunning = true;
        g.marqueeStart = GetTickCount64();
    }
    ULONGLONG next = ~0ULL;
    const ULONGLONG elapsed = GetTickCount64() - g.marqueeStart;
    for (const auto& m : g.marquees) {
        ULONGLONG wait;
        MarqueeOffset(m, elapsed, &wait);
        next = std::min(next, wait == 0 ? kMarqueeFrameMs : wait);
    }
    SetTimer(g.controller, kTimerMarquee, static_cast<UINT>(std::max<ULONGLONG>(next, kMarqueeFrameMs)), nullptr);
}

// Draws and presents the widget, reusing as much of the last frame as `what`
// allows: the static parts are kept, then the seekbar on top of them.
void Render(Redraw what = Redraw::Full) {
    if (!g.widget || !IsWindowVisible(g.widget)) return;
    SIZE size{g.placed.right - g.placed.left, g.placed.bottom - g.placed.top};
    if (size.cx <= 0 || size.cy <= 0 || !EnsureGraphics() || !EnsureBitmap(size)) return;
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);

    RECT bounds{0, 0, size.cx, size.cy};
    const float w = static_cast<float>(size.cx), h = static_cast<float>(size.cy);
    const WidgetLayout layout = ComputeLayout(w, h);
    const size_t bytes = static_cast<size_t>(size.cx) * size.cy * 4;
    auto* bits = static_cast<BYTE*>(g.dibBits);
    if (g.staticPixels.size() != bytes) what = Redraw::Full;
    if (what == Redraw::Marquee && g.basePixels.size() != bytes) what = Redraw::Seekbar;
    const Redraw measured = what;

    HRESULT hr = S_OK;
    if (what == Redraw::Full) {
        if (FAILED(g.target->BindDC(g.memDC, &bounds))) return;
        g.target->BeginDraw();
        DrawStatic(layout, w, h);
        hr = g.target->EndDraw();
        GdiFlush();
        g.staticPixels.assign(bits, bits + bytes);
        if (SUCCEEDED(hr)) BuildMarquees(); // borrows the render target, so rebind below
    } else if (what == Redraw::Seekbar) {
        memcpy(bits, g.staticPixels.data(), bytes);
    }
    if (SUCCEEDED(hr) && what != Redraw::Marquee) {
        if (g.nowPlaying.hasTimeline && SUCCEEDED(g.target->BindDC(g.memDC, &bounds))) {
            g.target->BeginDraw();
            DrawSeekbar(layout, w, h);
            hr = g.target->EndDraw();
            GdiFlush();
        }
        g.basePixels.assign(bits, bits + bytes);
    } else if (what == Redraw::Marquee) {
        memcpy(bits, g.basePixels.data(), bytes);
    }
    if (hr == D2DERR_RECREATE_TARGET) {
        // These belong to the target; rebuild them on the next draw.
        g.target.Reset();
        g.clipLayer.Reset();
        g.artBitmap.Reset();
        g.artBitmapSource = nullptr;
        g.iconBitmap.Reset();
        g.iconBitmapSource = nullptr;
        g.staticPixels.clear();
        g.basePixels.clear();
        return;
    }
    BlendMarquees();

    QueryPerformanceCounter(&t1);
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (!UpdateLayeredWindow(g.widget, nullptr, nullptr, &size, g.memDC, &source, 0, &blend, ULW_ALPHA)) {
        Log(L"UpdateLayeredWindow failed: %lu", GetLastError());
    }
    QueryPerformanceCounter(&t2);
    const int64_t spent = t1.QuadPart - t0.QuadPart;
    switch (measured) {
    case Redraw::Full: ++g.renders, g.drawTicks += spent; break;
    case Redraw::Seekbar: ++g.seekbarRenders, g.seekbarTicks += spent; break;
    case Redraw::Marquee: ++g.marqueeRenders, g.marqueeTicks += spent; break;
    }
    g.presentTicks += t2.QuadPart - t1.QuadPart;
    if (measured != Redraw::Marquee) ScheduleMarquee();
}

void OnMarqueeTick() {
    Render(Redraw::Marquee);
    ScheduleMarquee();
}

// ---------------------------------------------------------------------------
// Attaching to the taskbar and laying out.

void ScheduleRelayout() { SetTimer(g.controller, kTimerRelayout, kRelayoutDelayMs, nullptr); }

void Detach() {
    if (g.widget && IsWindow(g.widget)) DestroyWindow(g.widget);
    g.widget = nullptr;
    g.taskbar = nullptr;
    g.placed = {};
    g.fits = false;
}

void Attach() {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) {
        Log(L"Taskbar not found yet, retrying");
        SetTimer(g.controller, kTimerAttachRetry, kAttachRetryMs, nullptr);
        return;
    }
    if (g.widget && IsWindow(g.widget) && GetParent(g.widget) == taskbar) {
        g.watcher.Refresh(taskbar);
        return;
    }
    Detach();

    // A plain child of the taskbar window. Explorer keeps it in place, shows and
    // hides it with the taskbar, and destroys it if Explorer exits.
    g.widget = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kWidgetClass, L"TaskbarMedia",
                               WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, taskbar, nullptr, g.instance, nullptr);
    if (!g.widget) {
        Log(L"CreateWindowEx on taskbar failed: %lu, retrying", GetLastError());
        SetTimer(g.controller, kTimerAttachRetry, kAttachRetryMs, nullptr);
        return;
    }
    g.taskbar = taskbar;
    g.dpi = GetDpiForWindow(taskbar);
    Log(L"Attached to taskbar %p at %u DPI", taskbar, g.dpi);
    g.watcher.Refresh(taskbar);
}

// The seekbar's only cost: one redraw each time the filled part grows by a
// whole pixel (about every 1-2 s for a song), and only while it can be seen
// moving. Paused, hidden, full screen or live stream: no timer at all.
void ScheduleProgress() {
    KillTimer(g.controller, kTimerProgress);
    const MediaInfo& m = g.nowPlaying;
    if (!g.widget || !IsWindowVisible(g.widget) || g.fullscreen || !m.playing || !m.hasTimeline ||
        m.durationMs <= 0) {
        return;
    }

    const float w = static_cast<float>(g.placed.right - g.placed.left);
    const float h = static_cast<float>(g.placed.bottom - g.placed.top);
    const WidgetLayout layout = ComputeLayout(w, h);
    const double barPx = layout.seekbar.right - layout.seekbar.left;
    if (barPx < 1) return;

    const double position = static_cast<double>(m.PositionAt(NowFileTime()));
    const double nextPixelAt = (std::floor(position * barPx / m.durationMs) + 1) * m.durationMs / barPx;
    const double delayMs = (nextPixelAt - position) / m.rate;
    SetTimer(g.controller, kTimerProgress, static_cast<UINT>(std::clamp(delayMs + 1, 100.0, 60000.0)), nullptr);
}

// ABN_FULLSCREENAPP also fires for screen overlays and the desktop, so check
// that the front window really covers the taskbar's monitor.
bool ForegroundCoversTaskbar() {
    HWND front = GetForegroundWindow();
    RECT frontRect;
    MONITORINFO monitor{sizeof(monitor)};
    if (!front || !g.taskbar || !GetWindowRect(front, &frontRect) ||
        !GetMonitorInfoW(MonitorFromWindow(g.taskbar, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
        return false;
    }
    return frontRect.left <= monitor.rcMonitor.left && frontRect.top <= monitor.rcMonitor.top &&
           frontRect.right >= monitor.rcMonitor.right && frontRect.bottom >= monitor.rcMonitor.bottom;
}

// Shown only when there is media, it hasn't been paused for long, and there's
// room on the taskbar. A real full screen app covers the taskbar and us with
// it, so that case only stops the seekbar timer (see ScheduleProgress).
void UpdateVisibility() {
    if (!g.widget) return;
    const wchar_t* hiddenBecause = !g.fits                     ? L"no room"
                                   : g.nowPlaying.appId.empty() ? L"no media"
                                   : g.idleHidden               ? L"paused for a while"
                                                                : nullptr;
    const bool show = hiddenBecause == nullptr;
    if (show != (IsWindowVisible(g.widget) != FALSE)) {
        ShowWindow(g.widget, show ? SW_SHOWNA : SW_HIDE);
        Log(show ? L"Shown" : L"Hidden: %s", hiddenBecause);
        if (show) Render();
    }
    ScheduleProgress();
    ScheduleMarquee();
}

// Free space on a centered taskbar: between the left edge (or the Widgets
// button) and the Start button, which leads the centered icon group.
void ApplyGeometry() {
    TaskbarGeometry geo = g.watcher.Latest();
    if (!g.widget || geo.taskbar != g.taskbar) return;
    if (IsRectEmpty(&geo.startButton)) {
        Log(L"Start button not found");
        g.fits = false;
        UpdateVisibility();
        return;
    }

    const RECT& bar = geo.taskbarRect;
    int gapLeft = IsRectEmpty(&geo.widgetsButton) ? bar.left : geo.widgetsButton.right;
    int gapRight = geo.startButton.left;
    int available = gapRight - gapLeft - 2 * Scale(kMargin);
    int width = std::min(Scale(kPreferredWidth), available);
    if (width < Scale(kMinWidth)) {
        Log(L"Only %d px free left of Start", available);
        g.fits = false;
        UpdateVisibility();
        return;
    }

    int barHeight = bar.bottom - bar.top;
    int height = barHeight - 2 * Scale(kVerticalInset);
    RECT next{gapLeft + Scale(kMargin) - bar.left, (barHeight - height) / 2, 0, 0};
    next.right = next.left + width;
    next.bottom = next.top + height;

    // Decode cover art at exactly the size it's drawn.
    g.media.SetArtSize(static_cast<UINT>(height - 2 * Scale(kInnerPadding)));

    g.fits = true;
    if (!EqualRect(&next, &g.placed)) {
        g.placed = next;
        SetWindowPos(g.widget, HWND_TOP, next.left, next.top, width, height, SWP_NOACTIVATE);
        Log(L"Placed at x=%ld w=%d h=%d (free %d px, Start at %ld)", next.left, width, height, available,
            geo.startButton.left);
        Render();
    }
    UpdateVisibility();
}

// An appbar that reserves no space: the documented way to hear when an app
// goes full screen (ABN_FULLSCREENAPP), instead of polling for it.
void RegisterAppBar() {
    APPBARDATA data{sizeof(data)};
    data.hWnd = g.controller;
    data.uCallbackMessage = WM_APP_APPBAR;
    SHAppBarMessage(ABM_NEW, &data);
}

void UnregisterAppBar() {
    APPBARDATA data{sizeof(data)};
    data.hWnd = g.controller;
    SHAppBarMessage(ABM_REMOVE, &data);
}

void OnMediaChanged() {
    const MediaInfo previous = g.nowPlaying;
    g.nowPlaying = g.media.Latest();
    const MediaInfo& m = g.nowPlaying;
    if (m.appId != previous.appId || m.title != previous.title || m.playing != previous.playing) {
        Log(L"Media: [%s] %s — %s (%s%s)", m.appId.c_str(), m.title.c_str(), m.artist.c_str(),
            m.playing ? L"playing" : L"paused", m.hasTimeline ? L", has timeline" : L"");
    }
    // Paused for a while hides the widget (unless the settings say stay);
    // playing brings it straight back.
    if (m.playing || g.settings.stayVisibleWhenPaused) {
        KillTimer(g.controller, kTimerIdleHide);
        g.idleHidden = false;
    } else if (!g.idleHidden && (previous.playing || m.appId != previous.appId)) {
        SetTimer(g.controller, kTimerIdleHide, g.settings.hideAfterPausedSeconds * 1000, nullptr);
    }
    UpdateVisibility();

    // Apps like Spotify re-report their position every few seconds; when that's
    // all that changed, only the seekbar needs drawing.
    MediaInfo sameTimeline = m;
    sameTimeline.positionMs = previous.positionMs;
    sameTimeline.reportedAt = previous.reportedAt;
    Render(sameTimeline == previous ? Redraw::Seekbar : Redraw::Full);
}

// Takes new settings (from the menu or an edit of the file) into effect.
void ApplySettings(const Settings& next) {
    g.settings = next;
    if (g.settings.stayVisibleWhenPaused) {
        KillTimer(g.controller, kTimerIdleHide);
        g.idleHidden = false;
    } else if (!g.nowPlaying.playing && !g.idleHidden && !g.nowPlaying.appId.empty()) {
        SetTimer(g.controller, kTimerIdleHide, g.settings.hideAfterPausedSeconds * 1000, nullptr);
    }
    UpdateVisibility();
}

void OpenSettingsFile() {
    ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + SettingsPath() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

void ShowMenu() {
    HMENU menu = CreatePopupMenu();
    const auto checked = [](bool on) { return MF_STRING | (on ? MF_CHECKED : MF_UNCHECKED); };
    AppendMenuW(menu, checked(g.settings.stayVisibleWhenPaused), kCmdStayVisible, L"Stay visible when paused");
    AppendMenuW(menu, checked(StartsWithWindows()), kCmdStartWithWindows, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, kCmdOpenSettings, L"Open settings file");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit TaskbarMedia");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g.controller); // so the menu closes when clicking elsewhere
    UINT cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g.controller,
                              nullptr);
    PostMessageW(g.controller, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
    case kCmdStayVisible: {
        Settings next = g.settings;
        next.stayVisibleWhenPaused = !next.stayVisibleWhenPaused;
        SaveSettings(next);
        ApplySettings(next);
        break;
    }
    case kCmdStartWithWindows: SetStartWithWindows(!StartsWithWindows()); break;
    case kCmdOpenSettings: OpenSettingsFile(); break;
    case kCmdExit: DestroyWindow(g.controller); break;
    }
}

// ---------------------------------------------------------------------------
// Window procedures.

LRESULT CALLBACK WidgetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE; // never steal focus from the user's app
    case WM_MOUSEMOVE: {
        if (!g.trackingMouse) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            g.trackingMouse = TrackMouseEvent(&track);
        }
        int hit = HitButton({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (hit != g.hovered) {
            g.hovered = hit;
            Render(); // redraw only when the highlighted button changes
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g.trackingMouse = false;
        if (g.hovered != kNoButton) {
            g.hovered = kNoButton;
            Render();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g.pressed = HitButton(pt);
        if (g.pressed == kNoButton && HitArt(pt) && !g.nowPlaying.appId.empty()) g.pressed = kPressedArt;
        if (g.pressed != kNoButton) {
            SetCapture(hwnd);
            Render();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int released = HitButton(pt);
        int pressedButton = g.pressed;
        g.pressed = kNoButton;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (pressedButton == kPressedArt && HitArt(pt)) {
            // Clicking the cover jumps to the app that's playing.
            if (!ActivateApp(g.nowPlaying.appId)) Log(L"Couldn't bring %s forward", g.nowPlaying.appId.c_str());
        } else if (pressedButton != kNoButton && released == pressedButton && ButtonEnabled(released)) {
            if (released == kButtonSwitchApp) {
                g.media.SwitchSession(+1);
            } else {
                static constexpr MediaCommand commands[] = {MediaCommand::Previous, MediaCommand::PlayPause,
                                                            MediaCommand::Next};
                g.media.Send(commands[released]);
            }
        }
        Render();
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (g.pressed != kNoButton) {
            g.pressed = kNoButton;
            Render();
        }
        return 0;
    case WM_MOUSEWHEEL:
        // One scroll gesture = one app, however many notches it has: switch on
        // the first notch, then ignore the wheel until it rests for a moment.
        // Down goes to the next session, up to the previous.
        if (!g.wheelGesture) {
            g.wheelGesture = true;
            g.media.SwitchSession(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
        }
        SetTimer(g.controller, kTimerWheelRest, kWheelRestMs, nullptr); // re-armed while scrolling continues
        return 0;
    case WM_RBUTTONUP:
        ShowMenu();
        return 0;
    case WM_DPICHANGED_AFTERPARENT:
        g.dpi = GetDpiForWindow(hwnd);
        ScheduleRelayout();
        return 0;
    case WM_DESTROY:
        if (hwnd == g.widget) {
            // Explorer went away and took our window with it. TaskbarCreated
            // normally brings us back; the retry covers a missed broadcast.
            Log(L"Widget destroyed");
            g.widget = nullptr;
            g.taskbar = nullptr;
            g.placed = {};
            g.fits = false;
            if (g.controller) SetTimer(g.controller, kTimerAttachRetry, kAttachRetryMs, nullptr);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Hidden top-level window. Broadcasts like TaskbarCreated and WM_SETTINGCHANGE
// only reach top-level windows, so the controller outlives the widget.
LRESULT CALLBACK ControllerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g.taskbarCreatedMsg && msg != 0) {
        Log(L"TaskbarCreated: Explorer restarted");
        UnregisterAppBar();
        RegisterAppBar();
        g.fullscreen = false;
        Detach();
        Attach();
        return 0;
    }
    switch (msg) {
    case WM_APP_APPBAR:
        if (wp == ABN_FULLSCREENAPP) {
            g.fullscreen = lp != 0 && ForegroundCoversTaskbar();
            Log(L"Full screen notification: %s (%s)", lp ? L"opened" : L"closed",
                g.fullscreen ? L"confirmed" : L"not covering the taskbar");
            ScheduleProgress();
            ScheduleMarquee();
        }
        return 0;
    case WM_APP_TASKBAR_CHANGED:
        if (++g.layoutEvents % 20 == 1) Log(L"Layout event #%u", g.layoutEvents);
        ScheduleRelayout(); // re-arming the timer debounces bursts
        return 0;
    case WM_APP_GEOMETRY:
        ApplyGeometry();
        return 0;
    case WM_APP_MEDIA:
        OnMediaChanged();
        return 0;
    case WM_TIMER:
        KillTimer(hwnd, wp);
        if (wp == kTimerRelayout && g.taskbar) {
            g.watcher.Refresh(g.taskbar);
        } else if (wp == kTimerAttachRetry) {
            Attach();
        } else if (wp == kTimerProgress) {
            Render(Redraw::Seekbar);
            ScheduleProgress();
        } else if (wp == kTimerIdleHide) {
            g.idleHidden = true;
            UpdateVisibility();
        } else if (wp == kTimerWheelRest) {
            g.wheelGesture = false;
        } else if (wp == kTimerMarquee) {
            OnMarqueeTick();
        } else if (wp == kTimerSettingsReload) {
            if (Settings next = LoadSettings(); !(next == g.settings)) {
                Log(L"Settings file changed; applying");
                ApplySettings(next);
            }
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
            bool light = ReadLightTheme();
            if (light != g.lightTheme) {
                g.lightTheme = light;
                Log(L"Theme changed: %s", light ? L"light" : L"dark");
                Render();
            }
        }
        ScheduleRelayout(); // taskbar alignment or Widgets may have changed
        return 0;
    case WM_DISPLAYCHANGE:
        ScheduleRelayout();
        return 0;
    case WM_DESTROY:
        Log(L"Exiting (%u layout events, %u media evaluations, %u of them timeline-only)", g.layoutEvents,
            g.media.Evaluations(), g.media.TimelineOnlyEvaluations());
        if (const unsigned total = g.renders + g.seekbarRenders + g.marqueeRenders) {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            const double ms = 1000.0 / freq.QuadPart;
            auto average = [&](int64_t ticks, unsigned count) { return count ? ms * ticks / count : 0.0; };
            Log(L"Renders: %u full at %.2f ms, %u seekbar at %.2f ms, %u marquee at %.3f ms; presenting %.2f ms",
                g.renders, average(g.drawTicks, g.renders), g.seekbarRenders,
                average(g.seekbarTicks, g.seekbarRenders), g.marqueeRenders,
                average(g.marqueeTicks, g.marqueeRenders), average(g.presentTicks, total));
        }
        for (UINT_PTR timer : {kTimerRelayout, kTimerAttachRetry, kTimerProgress, kTimerIdleHide, kTimerWheelRest,
                               kTimerMarquee, kTimerSettingsReload}) {
            KillTimer(hwnd, timer);
        }
        UnregisterAppBar();
        g.controller = nullptr;
        Detach();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"TaskbarMedia.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // The UI thread uses the shell (app windows, launching) and Core Audio (volume).
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    OpenLog();
    g.instance = instance;
    g.settings = LoadSettings();
    g.lightTheme = ReadLightTheme();
    g.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = ControllerProc;
    wc.lpszClassName = kControllerClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = WidgetProc;
    wc.lpszClassName = kWidgetClass;
    RegisterClassExW(&wc);

    // Top-level but never shown: receives broadcasts, owns the menu.
    g.controller = CreateWindowExW(WS_EX_TOOLWINDOW, kControllerClass, L"TaskbarMedia", WS_POPUP, 0, 0, 0, 0,
                                   nullptr, nullptr, instance, nullptr);
    // --no-media: diagnostic switch to measure the cost of media sessions alone.
    const bool mediaEnabled = !wcsstr(GetCommandLineW(), L"--no-media");
    if (!g.controller || !g.watcher.Start(g.controller, WM_APP_GEOMETRY, WM_APP_TASKBAR_CHANGED) ||
        (mediaEnabled && !g.media.Start(g.controller, WM_APP_MEDIA))) {
        return 1;
    }
    Log(L"Started (theme %s; threads: ui %lu, taskbar %lu, media %lu)", g.lightTheme ? L"light" : L"dark",
        GetCurrentThreadId(), g.watcher.ThreadId(), g.media.ThreadId());
    RegisterAppBar();
    Attach();

    // Windows signals this handle when the settings folder changes, so hand
    // edits apply on save without polling the file.
    const std::wstring settingsPath = SettingsPath();
    HANDLE settingsChanged = FindFirstChangeNotificationW(
        settingsPath.substr(0, settingsPath.find_last_of(L'\\')).c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (settingsChanged == INVALID_HANDLE_VALUE) settingsChanged = nullptr;

    MSG msg;
    for (bool running = true; running;) {
        const DWORD woke = MsgWaitForMultipleObjectsEx(settingsChanged ? 1 : 0, &settingsChanged, INFINITE,
                                                       QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (settingsChanged && woke == WAIT_OBJECT_0) {
            FindNextChangeNotification(settingsChanged);
            SetTimer(g.controller, kTimerSettingsReload, kSettingsSettleMs, nullptr);
            continue;
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (settingsChanged) FindCloseChangeNotification(settingsChanged);

    g.media.Stop();
    g.watcher.Stop();
    g.titleFormat.Reset();
    g.artistFormat.Reset();
    g.target.Reset();
    g.dwrite.Reset();
    g.d2d.Reset();
    FreeBitmap();
    if (g.log) fclose(g.log);
    if (singleInstance) CloseHandle(singleInstance);
    if (SUCCEEDED(comInit)) CoUninitialize();
    return 0;
}
