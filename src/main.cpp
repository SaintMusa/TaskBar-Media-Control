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
#include "media_watcher.h"
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
enum : UINT_PTR { kTimerRelayout = 1, kTimerAttachRetry, kTimerProgress, kTimerIdleHide };
enum : UINT { kCmdExit = 1 };

constexpr UINT kRelayoutDelayMs = 120; // coalesces bursts while icons animate
constexpr UINT kAttachRetryMs = 1000;  // only while Explorer is (re)starting
constexpr UINT kIdleHideMs = 30000;    // paused this long: get out of the way
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

struct WidgetLayout {
    D2D1_RECT_F art;
    D2D1_RECT_F text;
    D2D1_RECT_F buttons[kButtonCount];
    D2D1_RECT_F seekbar;
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
    int wheelDelta = 0; // leftover from high-resolution wheels and touchpads

    bool fits = false;       // the taskbar has room for us
    bool fullscreen = false; // a full screen app or game is in front
    bool idleHidden = false; // media has been paused for kIdleHideMs

    HDC memDC = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    void* dibBits = nullptr;
    SIZE bitmapSize{};
    std::vector<BYTE> staticPixels; // last full draw minus the seekbar
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
    int64_t drawTicks = 0;       // Direct2D, QueryPerformanceCounter units
    int64_t seekbarTicks = 0;
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

    const std::wstring& title = media.appId.empty() ? std::wstring(L"Nothing playing")
                                : media.title.empty() ? std::wstring(L"Untitled")
                                                      : media.title;
    const auto& text = layout.text;
    if (media.artist.empty()) {
        g.target->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()), g.titleFormat.Get(), text,
                            titleInk.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    } else {
        g.target->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()), g.titleFormat.Get(),
                            D2D1::RectF(text.left, h * 0.08f, text.right, h * 0.54f), titleInk.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_CLIP);
        g.target->DrawTextW(media.artist.c_str(), static_cast<UINT32>(media.artist.size()), g.artistFormat.Get(),
                            D2D1::RectF(text.left, h * 0.5f, text.right, h * 0.92f), artistInk.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_CLIP);
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

// Draws and presents the widget. The static parts are kept in a copy, so a
// seekbar tick (seekbarOnly) pastes that copy and draws just the bar.
void Render(bool seekbarOnly = false) {
    if (!g.widget || !IsWindowVisible(g.widget)) return;
    SIZE size{g.placed.right - g.placed.left, g.placed.bottom - g.placed.top};
    if (size.cx <= 0 || size.cy <= 0 || !EnsureGraphics() || !EnsureBitmap(size)) return;
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);

    RECT bounds{0, 0, size.cx, size.cy};
    if (FAILED(g.target->BindDC(g.memDC, &bounds))) return;
    const float w = static_cast<float>(size.cx), h = static_cast<float>(size.cy);
    const WidgetLayout layout = ComputeLayout(w, h);
    const size_t bytes = static_cast<size_t>(size.cx) * size.cy * 4;

    HRESULT hr = S_OK;
    if (seekbarOnly && g.staticPixels.size() == bytes) {
        memcpy(g.dibBits, g.staticPixels.data(), bytes);
    } else {
        g.target->BeginDraw();
        DrawStatic(layout, w, h);
        hr = g.target->EndDraw();
        GdiFlush();
        const auto* bits = static_cast<const BYTE*>(g.dibBits);
        g.staticPixels.assign(bits, bits + bytes);
    }
    if (SUCCEEDED(hr) && g.nowPlaying.hasTimeline) {
        g.target->BeginDraw();
        DrawSeekbar(layout, w, h);
        hr = g.target->EndDraw();
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
        return;
    }

    QueryPerformanceCounter(&t1);
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (!UpdateLayeredWindow(g.widget, nullptr, nullptr, &size, g.memDC, &source, 0, &blend, ULW_ALPHA)) {
        Log(L"UpdateLayeredWindow failed: %lu", GetLastError());
    }
    QueryPerformanceCounter(&t2);
    ++(seekbarOnly ? g.seekbarRenders : g.renders);
    (seekbarOnly ? g.seekbarTicks : g.drawTicks) += t1.QuadPart - t0.QuadPart;
    g.presentTicks += t2.QuadPart - t1.QuadPart;
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
    // Paused for a while hides the widget; playing brings it straight back.
    if (m.playing) {
        KillTimer(g.controller, kTimerIdleHide);
        g.idleHidden = false;
    } else if (!g.idleHidden && (previous.playing || m.appId != previous.appId)) {
        SetTimer(g.controller, kTimerIdleHide, kIdleHideMs, nullptr);
    }
    UpdateVisibility();

    // Apps like Spotify re-report their position every few seconds; when that's
    // all that changed, only the seekbar needs drawing.
    MediaInfo sameTimeline = m;
    sameTimeline.positionMs = previous.positionMs;
    sameTimeline.reportedAt = previous.reportedAt;
    Render(/*seekbarOnly=*/sameTimeline == previous);
}

void ShowMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit TaskbarMedia");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g.controller); // so the menu closes when clicking elsewhere
    UINT cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g.controller,
                              nullptr);
    PostMessageW(g.controller, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd == kCmdExit) DestroyWindow(g.controller);
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
    case WM_LBUTTONDOWN:
        g.pressed = HitButton({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (g.pressed != kNoButton) {
            SetCapture(hwnd);
            Render();
        }
        return 0;
    case WM_LBUTTONUP: {
        int released = HitButton({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        int pressedButton = g.pressed;
        g.pressed = kNoButton;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (pressedButton != kNoButton && released == pressedButton && ButtonEnabled(released)) {
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
        // One notch = one app. Down goes to the next session, up to the previous.
        g.wheelDelta += GET_WHEEL_DELTA_WPARAM(wp);
        while (std::abs(g.wheelDelta) >= WHEEL_DELTA) {
            const int direction = g.wheelDelta > 0 ? -1 : 1;
            g.media.SwitchSession(direction);
            g.wheelDelta += direction * WHEEL_DELTA;
        }
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
            Render(/*seekbarOnly=*/true);
            ScheduleProgress();
        } else if (wp == kTimerIdleHide) {
            g.idleHidden = true;
            UpdateVisibility();
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
        if (g.renders + g.seekbarRenders) {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            const double ms = 1000.0 / freq.QuadPart;
            Log(L"%u full renders at %.2f ms, %u seekbar-only at %.2f ms, presenting %.2f ms on average", g.renders,
                g.renders ? ms * g.drawTicks / g.renders : 0.0, g.seekbarRenders,
                g.seekbarRenders ? ms * g.seekbarTicks / g.seekbarRenders : 0.0,
                ms * g.presentTicks / (g.renders + g.seekbarRenders));
        }
        for (UINT_PTR timer : {kTimerRelayout, kTimerAttachRetry, kTimerProgress, kTimerIdleHide}) {
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

    OpenLog();
    g.instance = instance;
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

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

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
    return 0;
}
