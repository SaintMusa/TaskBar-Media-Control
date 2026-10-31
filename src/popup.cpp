#include "popup.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kPopupClass[] = L"TaskbarMedia.Popup";

// Layout, in DIPs (96 DPI pixels), top to bottom.
constexpr int kWidth = 320;
constexpr int kPad = 16;
constexpr int kHeaderTop = 14, kHeaderHeight = 20, kHeaderIcon = 16;
constexpr int kArtTop = 46, kArtSize = 176, kArtRadius = 8;
constexpr int kTitleTop = 234, kTitleHeight = 22;
constexpr int kArtistTop = 256, kArtistHeight = 18;
constexpr int kSeekCenter = 290, kSeekHitHeight = 20, kSeekThickness = 4, kSeekHoverThickness = 6, kThumb = 12;
constexpr int kTimesTop = 297, kTimesHeight = 14;
constexpr int kControlsTop = 316, kButton = 40, kPlayButton = 48, kButtonGap = 6, kButtonRadius = 6;
constexpr int kNowPlayingBottom = 372;
// The app list, only when two or more apps have media.
constexpr int kRowHeight = 48, kRowInset = 8, kRowArt = 32, kRowRadius = 6, kMaxRows = 5, kListPad = 8;
constexpr int kFooterHeight = 40, kGearSize = 32;
constexpr int kSlideDistance = 12;  // DIPs it rises while opening
constexpr ULONGLONG kSlideMs = 160;
constexpr UINT kSlideFrameMs = 10;
constexpr int kGapAboveTaskbar = 12; // like Windows' own flyouts

constexpr int64_t kWheelSeekMs = 5000;
constexpr UINT_PTR kTimerProgress = 1, kTimerSlide = 2;

// Segoe Fluent Icons code points.
constexpr wchar_t kGlyphShuffle = 0xE8B1;
constexpr wchar_t kGlyphPrevious = 0xE892;
constexpr wchar_t kGlyphPlay = 0xE768;
constexpr wchar_t kGlyphPause = 0xE769;
constexpr wchar_t kGlyphNext = 0xE893;
constexpr wchar_t kGlyphRepeatAll = 0xE8EE;
constexpr wchar_t kGlyphRepeatOne = 0xE8ED;
constexpr wchar_t kGlyphMusic = 0xE8D6;
constexpr wchar_t kGlyphSettings = 0xE713;

// DWM attributes newer than some SDK headers.
constexpr DWORD kUseImmersiveDarkMode = 20;  // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kWindowCornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kSystemBackdropType = 38;    // DWMWA_SYSTEMBACKDROP_TYPE
constexpr int kCornerRound = 2;              // DWMWCP_ROUND
constexpr int kBackdropTransientWindow = 3;  // DWMSBT_TRANSIENTWINDOW: acrylic, as in flyouts

// Everything the mouse can point at.
enum class Hit { None, Header, Art, Seek, Shuffle, Previous, PlayPause, Next, Repeat, Row, Options };
constexpr Hit kControls[] = {Hit::Shuffle, Hit::Previous, Hit::PlayPause, Hit::Next, Hit::Repeat};

struct PopupState {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    bool lightTheme = false;
    ULONGLONG closedAt = 0;
    PopupHost host;

    Hit hovered = Hit::None;
    Hit pressed = Hit::None;
    int hoveredRow = -1, pressedRow = -1; // with Hit::Row
    int focusedRow = -1;       // picked with the arrow keys, switched to with Enter
    ULONGLONG slideStart = 0;
    int bottom = 0; // screen y the pop-up's bottom edge stays at as it grows or shrinks
    bool trackingMouse = false;
    bool dragging = false;     // the seekbar thumb is held
    double dragFraction = 0;   // where it's held, 0..1

    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<IDWriteTextFormat> titleFormat, artistFormat, headerFormat, timeLeftFormat, timeRightFormat;
    ComPtr<IDWriteTextFormat> iconFormat, playIconFormat, placeholderFormat;
    ComPtr<ID2D1Bitmap> largeArt, smallArt, icon;
    std::vector<std::pair<std::shared_ptr<const MediaArt>, ComPtr<ID2D1Bitmap>>> rowArt; // the list's covers
    ComPtr<IDWriteTextFormat> rowTitleFormat, rowDetailFormat, rowIconFormat;
    std::shared_ptr<const MediaArt> largeArtSource, smallArtSource, iconSource;
    HDC memDC = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    SIZE bitmapSize{};
} p;

float S(float dip) { return dip * p.dpi / 96.0f; }
void Redraw(); // further down, with painting
int Scale(int dip) { return MulDiv(dip, static_cast<int>(p.dpi), 96); }

D2D1_COLOR_F Ink(float alpha) {
    return p.lightTheme ? D2D1::ColorF(0, 0, 0, alpha) : D2D1::ColorF(1, 1, 1, alpha);
}

int64_t NowFileTime() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::wstring FormatTime(int64_t ms) {
    const int64_t total = std::max<int64_t>(ms, 0) / 1000;
    wchar_t text[32];
    if (total >= 3600) {
        swprintf_s(text, L"%lld:%02lld:%02lld", total / 3600, total / 60 % 60, total % 60);
    } else {
        swprintf_s(text, L"%lld:%02lld", total / 60, total % 60);
    }
    return text;
}

// --- Layout ----------------------------------------------------------------

D2D1_RECT_F ArtRect() {
    const float left = (S(kWidth) - S(kArtSize)) / 2;
    return D2D1::RectF(left, S(kArtTop), left + S(kArtSize), S(kArtTop) + S(kArtSize));
}

D2D1_RECT_F HeaderRect() {
    return D2D1::RectF(S(kPad), S(kHeaderTop), S(kWidth) - S(kPad), S(kHeaderTop) + S(kHeaderHeight));
}

D2D1_RECT_F SeekTrack() {
    return D2D1::RectF(S(kPad), S(kSeekCenter) - S(kSeekThickness) / 2, S(kWidth) - S(kPad),
                       S(kSeekCenter) + S(kSeekThickness) / 2);
}

D2D1_RECT_F SeekHitRect() {
    const auto track = SeekTrack();
    return D2D1::RectF(track.left - S(6), S(kSeekCenter) - S(kSeekHitHeight) / 2, track.right + S(6),
                       S(kSeekCenter) + S(kSeekHitHeight) / 2);
}

D2D1_RECT_F ControlRect(Hit control) {
    const float total = S(kButton) * 4 + S(kPlayButton) + S(kButtonGap) * 4;
    float x = (S(kWidth) - total) / 2;
    for (Hit c : kControls) {
        const float size = c == Hit::PlayPause ? S(kPlayButton) : S(kButton);
        const float top = S(kControlsTop) + (S(kPlayButton) - size) / 2;
        if (c == control) return D2D1::RectF(x, top, x + size, top + size);
        x += size + S(kButtonGap);
    }
    return {};
}

bool Contains(const D2D1_RECT_F& r, POINT pt) {
    return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
}

bool ControlEnabled(Hit control, const MediaInfo& m) {
    switch (control) {
    case Hit::Shuffle: return m.canShuffle;
    case Hit::Previous: return m.canPrevious;
    case Hit::PlayPause: return m.canPlayPause;
    case Hit::Next: return m.canNext;
    case Hit::Repeat: return m.canRepeat;
    default: return false;
    }
}

bool CanSeek(const MediaInfo& m) { return m.hasTimeline && m.canSeek && m.durationMs > 0; }

int RowCount(const MediaInfo& m) {
    return m.sessions.size() < 2 ? 0 : std::min<int>(static_cast<int>(m.sessions.size()), kMaxRows);
}

int FooterTop(const MediaInfo& m) {
    const int rows = RowCount(m);
    return kNowPlayingBottom + (rows ? 1 + kListPad * 2 + rows * kRowHeight : 0);
}

int ContentHeight(const MediaInfo& m) { return FooterTop(m) + 1 + kFooterHeight; }

D2D1_RECT_F GearRect(const MediaInfo& m) {
    const float top = S(static_cast<float>(FooterTop(m))) + S(1) + (S(kFooterHeight) - S(kGearSize)) / 2;
    const float right = S(kWidth) - S(kRowInset);
    return D2D1::RectF(right - S(kGearSize), top, right, top + S(kGearSize));
}

D2D1_RECT_F RowRect(int index) {
    const float top = S(kNowPlayingBottom) + S(1) + S(kListPad) + S(kRowHeight) * index;
    return D2D1::RectF(S(kRowInset), top, S(kWidth) - S(kRowInset), top + S(kRowHeight));
}

Hit HitTest(POINT pt) {
    const MediaInfo& m = p.host.media();
    if (Contains(HeaderRect(), pt)) return Hit::Header;
    if (Contains(GearRect(m), pt)) return Hit::Options;
    if (Contains(ArtRect(), pt)) return Hit::Art;
    if (m.hasTimeline && Contains(SeekHitRect(), pt)) return Hit::Seek;
    for (Hit c : kControls) {
        if (Contains(ControlRect(c), pt)) return c;
    }
    return Hit::None;
}

// Which row of the app list is under the point, or -1.
int RowAt(POINT pt) {
    const int rows = RowCount(p.host.media());
    for (int i = 0; i < rows; ++i) {
        if (Contains(RowRect(i), pt)) return i;
    }
    return -1;
}

double FractionAt(int x) {
    const auto track = SeekTrack();
    return std::clamp((x - track.left) / (track.right - track.left), 0.0f, 1.0f);
}

// --- Drawing resources -----------------------------------------------------

ComPtr<IDWriteTextFormat> MakeFormat(const wchar_t* family, DWRITE_FONT_WEIGHT weight, float sizeDip,
                                     DWRITE_TEXT_ALIGNMENT align, bool ellipsis) {
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(p.dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, S(sizeDip), L"", &format))) {
        return nullptr;
    }
    format->SetTextAlignment(align);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (ellipsis) {
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(p.dwrite->CreateEllipsisTrimmingSign(format.Get(), &sign))) {
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            format->SetTrimming(&trimming, sign.Get());
        }
    }
    return format;
}

void FreeGraphics() {
    if (p.memDC) {
        if (p.oldBitmap) SelectObject(p.memDC, p.oldBitmap);
        DeleteDC(p.memDC);
    }
    if (p.bitmap) DeleteObject(p.bitmap);
    p.memDC = nullptr;
    p.bitmap = nullptr;
    p.oldBitmap = nullptr;
    p.bitmapSize = {};
    p.largeArt.Reset();
    p.smallArt.Reset();
    p.icon.Reset();
    p.rowArt.clear();
    p.largeArtSource = p.smallArtSource = p.iconSource = nullptr;
    p.rowTitleFormat.Reset();
    p.rowDetailFormat.Reset();
    p.rowIconFormat.Reset();
    p.titleFormat.Reset();
    p.artistFormat.Reset();
    p.headerFormat.Reset();
    p.timeLeftFormat.Reset();
    p.timeRightFormat.Reset();
    p.iconFormat.Reset();
    p.playIconFormat.Reset();
    p.placeholderFormat.Reset();
    p.target.Reset();
    p.dwrite.Reset();
    p.d2d.Reset();
}

bool EnsureGraphics(SIZE size) {
    if (!p.d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, p.d2d.GetAddressOf()))) return false;
    if (!p.dwrite && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                reinterpret_cast<IUnknown**>(p.dwrite.GetAddressOf())))) {
        return false;
    }
    if (!p.target) {
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(p.d2d->CreateDCRenderTarget(&props, &p.target))) return false;
        p.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    if (!p.titleFormat) {
        constexpr wchar_t text[] = L"Segoe UI Variable Text", icons[] = L"Segoe Fluent Icons";
        p.titleFormat = MakeFormat(text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 16, DWRITE_TEXT_ALIGNMENT_CENTER, true);
        p.artistFormat = MakeFormat(text, DWRITE_FONT_WEIGHT_NORMAL, 13, DWRITE_TEXT_ALIGNMENT_CENTER, true);
        p.headerFormat = MakeFormat(text, DWRITE_FONT_WEIGHT_NORMAL, 12, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        p.timeLeftFormat = MakeFormat(text, DWRITE_FONT_WEIGHT_NORMAL, 11, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        p.timeRightFormat = MakeFormat(text, DWRITE_FONT_WEIGHT_NORMAL, 11, DWRITE_TEXT_ALIGNMENT_TRAILING, false);
        p.iconFormat = MakeFormat(icons, DWRITE_FONT_WEIGHT_NORMAL, 16, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        p.playIconFormat = MakeFormat(icons, DWRITE_FONT_WEIGHT_NORMAL, 22, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        p.placeholderFormat = MakeFormat(icons, DWRITE_FONT_WEIGHT_NORMAL, 48, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        p.rowTitleFormat = MakeFormat(text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        p.rowDetailFormat = MakeFormat(text, DWRITE_FONT_WEIGHT_NORMAL, 12, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        p.rowIconFormat = MakeFormat(icons, DWRITE_FONT_WEIGHT_NORMAL, 12, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    }
    if (!p.bitmap || p.bitmapSize.cx != size.cx || p.bitmapSize.cy != size.cy) {
        if (p.memDC) {
            SelectObject(p.memDC, p.oldBitmap);
            DeleteDC(p.memDC);
        }
        if (p.bitmap) DeleteObject(p.bitmap);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(info.bmiHeader), size.cx, -size.cy, 1, 32, BI_RGB};
        void* bits = nullptr;
        p.memDC = CreateCompatibleDC(nullptr);
        p.bitmap = CreateDIBSection(p.memDC, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!p.memDC || !p.bitmap) return false;
        p.oldBitmap = SelectObject(p.memDC, p.bitmap);
        p.bitmapSize = size;
    }
    return p.titleFormat && p.artistFormat && p.headerFormat && p.timeLeftFormat && p.timeRightFormat &&
           p.iconFormat && p.playIconFormat && p.placeholderFormat && p.rowTitleFormat && p.rowDetailFormat &&
           p.rowIconFormat;
}

// Uploads an image to a Direct2D bitmap once, when it changes. uploadedFrom keeps
// the image alive, so a new cover can't reuse its address and look unchanged.
ID2D1Bitmap* Uploaded(const std::shared_ptr<const MediaArt>& image, ComPtr<ID2D1Bitmap>& bitmap,
                      std::shared_ptr<const MediaArt>& uploadedFrom) {
    if (image != uploadedFrom) {
        bitmap.Reset();
        uploadedFrom = image;
        if (image) {
            auto props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            p.target->CreateBitmap(D2D1::SizeU(image->size, image->size), image->pixels.data(), image->size * 4,
                                   props, &bitmap);
        }
    }
    return bitmap.Get();
}

// --- Drawing ---------------------------------------------------------------

// Fills a rounded rect with an image, scaled to fit it.
void FillWithImage(ID2D1Bitmap* image, const D2D1_RECT_F& rect, float radius) {
    ComPtr<ID2D1BitmapBrush> brush;
    if (FAILED(p.target->CreateBitmapBrush(image, &brush))) return;
    const auto source = image->GetSize();
    brush->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    brush->SetTransform(D2D1::Matrix3x2F::Scale((rect.right - rect.left) / source.width,
                                                (rect.bottom - rect.top) / source.height) *
                        D2D1::Matrix3x2F::Translation(rect.left, rect.top));
    p.target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
}

void DrawText(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect,
              ID2D1SolidColorBrush* brush) {
    p.target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DrawNowPlaying(const MediaInfo& m, ID2D1SolidColorBrush* paint) {
    // Header: the app's icon and name, so it's clear where the sound comes from.
    const auto header = HeaderRect();
    float textLeft = header.left;
    if (ID2D1Bitmap* icon = Uploaded(m.appIcon, p.icon, p.iconSource)) {
        const float top = header.top + (header.bottom - header.top - S(kHeaderIcon)) / 2;
        p.target->DrawBitmap(icon, D2D1::RectF(header.left, top, header.left + S(kHeaderIcon), top + S(kHeaderIcon)),
                             1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        textLeft += S(kHeaderIcon) + S(8);
    }
    paint->SetColor(Ink(p.hovered == Hit::Header ? 0.92f : 0.68f));
    DrawText(m.appName.empty() ? m.appId : m.appName, p.headerFormat.Get(),
             D2D1::RectF(textLeft, header.top, header.right, header.bottom), paint);

    // The cover, big. While the full-size decode is still on its way, the
    // widget's small copy stands in rather than an empty square.
    const auto art = ArtRect();
    const float radius = S(kArtRadius);
    ID2D1Bitmap* cover = Uploaded(m.largeArt, p.largeArt, p.largeArtSource);
    if (!cover) cover = Uploaded(m.art, p.smallArt, p.smallArtSource);
    if (cover) {
        FillWithImage(cover, art, radius);
    } else {
        paint->SetColor(Ink(0.08f));
        p.target->FillRoundedRectangle(D2D1::RoundedRect(art, radius, radius), paint);
        paint->SetColor(Ink(0.4f));
        p.target->DrawTextW(&kGlyphMusic, 1, p.placeholderFormat.Get(), art, paint);
    }
    if (p.hovered == Hit::Art) { // a soft lift says "click me"
        paint->SetColor(D2D1::ColorF(1, 1, 1, 0.08f));
        p.target->FillRoundedRectangle(D2D1::RoundedRect(art, radius, radius), paint);
    }

    const float textAlpha = m.playing ? 0.95f : 0.7f;
    paint->SetColor(Ink(textAlpha));
    DrawText(m.title.empty() ? L"Untitled" : m.title, p.titleFormat.Get(),
             D2D1::RectF(S(kPad), S(kTitleTop), S(kWidth) - S(kPad), S(kTitleTop) + S(kTitleHeight)), paint);
    paint->SetColor(Ink(textAlpha * 0.65f));
    DrawText(m.artist, p.artistFormat.Get(),
             D2D1::RectF(S(kPad), S(kArtistTop), S(kWidth) - S(kPad), S(kArtistTop) + S(kArtistHeight)), paint);

    // Seekbar with times. Live streams and apps that report no length skip it.
    if (m.hasTimeline && m.durationMs > 0) {
        const bool active = p.dragging || (p.hovered == Hit::Seek && CanSeek(m));
        const double fraction = p.dragging ? p.dragFraction
                                           : static_cast<double>(m.PositionAt(NowFileTime())) / m.durationMs;
        auto track = SeekTrack();
        if (active) {
            const float grow = (S(kSeekHoverThickness) - S(kSeekThickness)) / 2;
            track.top -= grow;
            track.bottom += grow;
        }
        const float round = (track.bottom - track.top) / 2;
        paint->SetColor(Ink(0.18f));
        p.target->FillRoundedRectangle(D2D1::RoundedRect(track, round, round), paint);
        auto filled = track;
        filled.right = track.left + static_cast<float>((track.right - track.left) * fraction);
        D2D1_COLOR_F color = p.host.accent();
        paint->SetColor(color);
        p.target->FillRoundedRectangle(D2D1::RoundedRect(filled, round, round), paint);
        if (active) {
            paint->SetColor(Ink(1.0f));
            p.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(filled.right, S(kSeekCenter)), S(kThumb) / 2,
                                                S(kThumb) / 2),
                                  paint);
        }
        const int64_t shownMs = static_cast<int64_t>(fraction * m.durationMs);
        const auto times = D2D1::RectF(S(kPad), S(kTimesTop), S(kWidth) - S(kPad), S(kTimesTop) + S(kTimesHeight));
        paint->SetColor(Ink(0.58f));
        DrawText(FormatTime(shownMs), p.timeLeftFormat.Get(), times, paint);
        DrawText(FormatTime(m.durationMs), p.timeRightFormat.Get(), times, paint);
    }

    // Controls. Shuffle and repeat light up in the cover's color when on, and
    // everything the app doesn't allow is drawn faint.
    for (Hit c : kControls) {
        const auto rect = ControlRect(c);
        const bool enabled = ControlEnabled(c, m);
        const bool on = (c == Hit::Shuffle && m.shuffle) || (c == Hit::Repeat && m.repeat != RepeatMode::Off);
        if (enabled && p.hovered == c) {
            paint->SetColor(Ink(p.pressed == c ? 0.05f : 0.08f));
            p.target->FillRoundedRectangle(D2D1::RoundedRect(rect, S(kButtonRadius), S(kButtonRadius)), paint);
        }
        wchar_t glyph = 0;
        switch (c) {
        case Hit::Shuffle: glyph = kGlyphShuffle; break;
        case Hit::Previous: glyph = kGlyphPrevious; break;
        case Hit::PlayPause: glyph = m.playing ? kGlyphPause : kGlyphPlay; break;
        case Hit::Next: glyph = kGlyphNext; break;
        case Hit::Repeat: glyph = m.repeat == RepeatMode::One ? kGlyphRepeatOne : kGlyphRepeatAll; break;
        default: break;
        }
        if (!enabled) paint->SetColor(Ink(0.32f));
        else if (on) paint->SetColor(p.host.accent());
        else paint->SetColor(Ink(p.pressed == c ? 0.7f : 0.95f));
        p.target->DrawTextW(&glyph, 1, c == Hit::PlayPause ? p.playIconFormat.Get() : p.iconFormat.Get(), rect,
                            paint);
        if (enabled && on) { // a small dot under it, so "on" doesn't rely on color alone
            const float cx = (rect.left + rect.right) / 2;
            p.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, rect.bottom - S(5)), S(1.5f), S(1.5f)), paint);
        }
    }
}

// A cover for the list, uploaded once per image and reused while it stays.
ID2D1Bitmap* RowArt(const std::shared_ptr<const MediaArt>& image) {
    if (!image) return nullptr;
    for (auto& [source, bitmap] : p.rowArt) {
        if (source == image) return bitmap.Get();
    }
    ComPtr<ID2D1Bitmap> bitmap;
    std::shared_ptr<const MediaArt> uploadedFrom;
    Uploaded(image, bitmap, uploadedFrom);
    p.rowArt.emplace_back(image, bitmap);
    return bitmap.Get();
}

// Every app with media: the one on screen is highlighted with a bar in the
// cover's color, like Windows' own media flyout; playing ones get a small play mark.
void DrawAppList(const MediaInfo& m, ID2D1SolidColorBrush* paint) {
    const int rows = RowCount(m);
    if (!rows) return;
    // Forget covers no longer listed, so they don't pile up while open.
    std::erase_if(p.rowArt, [&](const auto& entry) {
        return std::none_of(m.sessions.begin(), m.sessions.end(),
                            [&](const SessionSummary& s) { return s.art == entry.first; });
    });

    paint->SetColor(Ink(0.08f));
    p.target->FillRectangle(D2D1::RectF(0, S(kNowPlayingBottom), S(kWidth), S(kNowPlayingBottom) + S(1)), paint);
    for (int i = 0; i < rows; ++i) {
        const SessionSummary& s = m.sessions[i];
        const auto row = RowRect(i);
        const bool current = s.appId == m.appId;
        if (current || p.hoveredRow == i || p.focusedRow == i) {
            paint->SetColor(Ink(current ? 0.08f : (p.pressedRow == i ? 0.04f : 0.05f)));
            p.target->FillRoundedRectangle(D2D1::RoundedRect(row, S(kRowRadius), S(kRowRadius)), paint);
        }
        if (current) {
            const float mid = (row.top + row.bottom) / 2;
            paint->SetColor(p.host.accent());
            p.target->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(row.left, mid - S(9), row.left + S(3), mid + S(9)), S(1.5f), S(1.5f)),
                paint);
        }

        const float artLeft = row.left + S(8), artTop = row.top + (row.bottom - row.top - S(kRowArt)) / 2;
        const auto art = D2D1::RectF(artLeft, artTop, artLeft + S(kRowArt), artTop + S(kRowArt));
        if (ID2D1Bitmap* cover = RowArt(s.art)) {
            FillWithImage(cover, art, S(4));
        } else {
            paint->SetColor(Ink(0.08f));
            p.target->FillRoundedRectangle(D2D1::RoundedRect(art, S(4), S(4)), paint);
            paint->SetColor(Ink(0.4f));
            p.target->DrawTextW(&kGlyphMusic, 1, p.rowIconFormat.Get(), art, paint);
        }

        const float textLeft = art.right + S(10), textRight = row.right - S(28);
        const std::wstring name = s.appName.empty() ? s.appId : s.appName;
        const std::wstring title = s.title.empty() ? name : s.title;
        std::wstring detail = s.artist;
        if (!s.title.empty()) detail += (detail.empty() ? L"" : L" \u00B7 ") + name;
        const float mid = (row.top + row.bottom) / 2;
        paint->SetColor(Ink(0.95f));
        DrawText(title, p.rowTitleFormat.Get(),
                 D2D1::RectF(textLeft, detail.empty() ? row.top : mid - S(18), textRight,
                             detail.empty() ? row.bottom : mid + S(1)),
                 paint);
        if (!detail.empty()) {
            paint->SetColor(Ink(0.6f));
            DrawText(detail, p.rowDetailFormat.Get(), D2D1::RectF(textLeft, mid, textRight, mid + S(17)), paint);
        }
        if (s.playing) {
            paint->SetColor(Ink(0.6f));
            p.target->DrawTextW(&kGlyphPlay, 1, p.rowIconFormat.Get(),
                                D2D1::RectF(row.right - S(28), row.top, row.right - S(4), row.bottom), paint);
        }
    }
}

// The footer: a gear for the options menu, until there's a settings window.
void DrawFooter(const MediaInfo& m, ID2D1SolidColorBrush* paint) {
    const float top = S(static_cast<float>(FooterTop(m)));
    paint->SetColor(Ink(0.08f));
    p.target->FillRectangle(D2D1::RectF(0, top, S(kWidth), top + S(1)), paint);
    const auto gear = GearRect(m);
    if (p.hovered == Hit::Options) {
        paint->SetColor(Ink(p.pressed == Hit::Options ? 0.05f : 0.08f));
        p.target->FillRoundedRectangle(D2D1::RoundedRect(gear, S(kButtonRadius), S(kButtonRadius)), paint);
    }
    paint->SetColor(Ink(p.hovered == Hit::Options ? 0.95f : 0.72f));
    p.target->DrawTextW(&kGlyphSettings, 1, p.iconFormat.Get(), gear, paint);
}

// Slides up into place on open, like Windows' flyouts, unless animations are off.
void SlideStep() {
    if (!p.hwnd) return;
    const double t = std::min(1.0, static_cast<double>(GetTickCount64() - p.slideStart) / kSlideMs);
    const double eased = 1 - (1 - t) * (1 - t) * (1 - t); // ease out: quick, then settles
    RECT window;
    GetWindowRect(p.hwnd, &window);
    const int height = window.bottom - window.top;
    const int offset = static_cast<int>(std::lround(Scale(kSlideDistance) * (1 - eased)));
    SetWindowPos(p.hwnd, nullptr, window.left, p.bottom - height + offset, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (t >= 1) KillTimer(p.hwnd, kTimerSlide);
}

// Keyboard, while the pop-up has focus.
void OnKey(WPARAM key) {
    const MediaInfo& m = p.host.media();
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    const int rows = RowCount(m);
    switch (key) {
    case VK_ESCAPE: ClosePopup(); return;
    case VK_SPACE:
        if (m.canPlayPause) p.host.send(MediaCommand::PlayPause);
        return;
    case VK_LEFT:
    case VK_RIGHT: {
        const int direction = key == VK_RIGHT ? 1 : -1;
        if (shift) {
            if (CanSeek(m)) {
                p.host.seek(std::clamp<int64_t>(m.PositionAt(NowFileTime()) + direction * kWheelSeekMs, 0,
                                                m.durationMs));
            }
        } else if (direction > 0 ? m.canNext : m.canPrevious) {
            p.host.send(direction > 0 ? MediaCommand::Next : MediaCommand::Previous);
        }
        return;
    }
    case VK_UP:
    case VK_DOWN: {
        if (!rows) return;
        int current = p.focusedRow;
        if (current < 0) { // start from the app on screen
            for (int i = 0; i < rows; ++i) {
                if (m.sessions[i].appId == m.appId) current = i;
            }
        }
        p.focusedRow = std::clamp(current + (key == VK_DOWN ? 1 : -1), 0, rows - 1);
        Redraw();
        return;
    }
    case VK_RETURN:
        if (p.focusedRow >= 0 && p.focusedRow < rows) p.host.pick(m.sessions[p.focusedRow].appId);
        return;
    }
}

// Keeps the window as tall as its content, growing upward from a fixed bottom.
void FitHeight() {
    if (!p.hwnd) return;
    RECT window;
    GetWindowRect(p.hwnd, &window);
    const int height = Scale(ContentHeight(p.host.media()));
    if (window.bottom - window.top == height) return;
    SetWindowPos(p.hwnd, nullptr, window.left, p.bottom - height, window.right - window.left, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

// Draws into a premultiplied 32-bit bitmap and copies it to the window. With
// the frame extended over the whole window, DWM treats transparent pixels as
// "show the acrylic backdrop here", so the content floats on the blur.
void Paint(HDC windowDC) {
    RECT client;
    GetClientRect(p.hwnd, &client);
    SIZE size{client.right, client.bottom};
    if (size.cx <= 0 || size.cy <= 0 || !EnsureGraphics(size)) return;
    RECT bounds{0, 0, size.cx, size.cy};
    if (FAILED(p.target->BindDC(p.memDC, &bounds))) return;

    ComPtr<ID2D1SolidColorBrush> paint;
    p.target->CreateSolidColorBrush(Ink(1.0f), &paint);
    p.target->BeginDraw();
    p.target->Clear(D2D1::ColorF(0, 0, 0, 0));
    if (paint) {
        DrawNowPlaying(p.host.media(), paint.Get());
        DrawAppList(p.host.media(), paint.Get());
        DrawFooter(p.host.media(), paint.Get());
    }
    if (p.target->EndDraw() == D2DERR_RECREATE_TARGET) {
        p.target.Reset();
        p.largeArt.Reset();
        p.smallArt.Reset();
        p.icon.Reset();
        p.rowArt.clear();
        p.largeArtSource = p.smallArtSource = p.iconSource = nullptr;
    }
    BitBlt(windowDC, 0, 0, size.cx, size.cy, p.memDC, 0, 0, SRCCOPY);
}

void Redraw() {
    if (p.hwnd) InvalidateRect(p.hwnd, nullptr, FALSE);
}

// While open and playing, redraws each time the seekbar grows by a pixel,
// exactly like the widget's bar. Paused, dragging or no timeline: no timer.
void ScheduleProgress() {
    if (!p.hwnd) return;
    KillTimer(p.hwnd, kTimerProgress);
    const MediaInfo& m = p.host.media();
    if (!m.playing || !m.hasTimeline || m.durationMs <= 0 || p.dragging) return;
    const auto track = SeekTrack();
    const double barPx = track.right - track.left;
    const double position = static_cast<double>(m.PositionAt(NowFileTime()));
    const double nextPixelAt = (std::floor(position * barPx / m.durationMs) + 1) * m.durationMs / barPx;
    const double delayMs = (nextPixelAt - position) / m.rate;
    SetTimer(p.hwnd, kTimerProgress, static_cast<UINT>(std::clamp(delayMs + 1, 100.0, 60000.0)), nullptr);
}

void Activate(Hit hit) {
    const MediaInfo& m = p.host.media();
    switch (hit) {
    case Hit::Header:
    case Hit::Art: p.host.openApp(); break; // the app taking focus closes the pop-up
    case Hit::Shuffle: p.host.send(m.shuffle ? MediaCommand::ShuffleOff : MediaCommand::ShuffleOn); break;
    case Hit::Previous: p.host.send(MediaCommand::Previous); break;
    case Hit::PlayPause: p.host.send(MediaCommand::PlayPause); break;
    case Hit::Next: p.host.send(MediaCommand::Next); break;
    case Hit::Options: p.host.options(); break; // the menu taking focus closes the pop-up
    case Hit::Repeat: // off -> all -> one -> off, like most players
        p.host.send(m.repeat == RepeatMode::Off   ? MediaCommand::RepeatAll
                    : m.repeat == RepeatMode::All ? MediaCommand::RepeatOne
                                                  : MediaCommand::RepeatOff);
        break;
    default: break;
    }
}

bool Clickable(Hit hit, const MediaInfo& m) {
    switch (hit) {
    case Hit::Header:
    case Hit::Art:
    case Hit::Row:
    case Hit::Options: return true;
    case Hit::Seek: return CanSeek(m);
    case Hit::None: return false;
    default: return ControlEnabled(hit, m);
    }
}

LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ACTIVATE:
        // Clicking anywhere else takes focus away: that's the cue to close.
        if (LOWORD(wp) == WA_INACTIVE) ClosePopup();
        return 0;
    case WM_KEYDOWN:
        OnKey(wp);
        return 0;
    case WM_NCCALCSIZE:
        // The window has a frame style only so DWM gives it a backdrop; this
        // makes all of it client area, so no frame is actually drawn.
        if (wp) return 0;
        break;
    case WM_NCHITTEST:
        return HTCLIENT; // no resizing from the edges the frame style would add
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            const bool hand = Clickable(RowAt(pt) >= 0 ? Hit::Row : HitTest(pt), p.host.media()) || p.dragging;
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        if (!p.trackingMouse) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            p.trackingMouse = TrackMouseEvent(&track);
        }
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (p.dragging) {
            p.dragFraction = FractionAt(pt.x);
            Redraw();
            return 0;
        }
        const int row = RowAt(pt);
        const Hit hit = row >= 0 ? Hit::Row : HitTest(pt);
        if (hit != p.hovered || row != p.hoveredRow) {
            p.hovered = hit;
            p.hoveredRow = row;
            Redraw(); // only when what's under the mouse changes
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        p.trackingMouse = false;
        if (p.hovered != Hit::None && !p.dragging) {
            p.hovered = Hit::None;
            p.hoveredRow = -1;
            Redraw();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int row = RowAt(pt);
        const Hit hit = row >= 0 ? Hit::Row : HitTest(pt);
        const MediaInfo& m = p.host.media();
        if (!Clickable(hit, m)) return 0;
        SetCapture(hwnd);
        p.pressedRow = row;
        if (hit == Hit::Seek) {
            p.dragging = true;
            p.dragFraction = FractionAt(pt.x);
            KillTimer(hwnd, kTimerProgress);
        } else {
            p.pressed = hit;
        }
        Redraw();
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        // Take the press state first: releasing capture sends WM_CAPTURECHANGED
        // right away, which clears it.
        const bool wasDragging = p.dragging;
        const Hit pressed = p.pressed;
        const int pressedRow = p.pressedRow;
        p.dragging = false;
        p.pressed = Hit::None;
        p.pressedRow = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        const MediaInfo& m = p.host.media();
        if (wasDragging) {
            p.host.seek(static_cast<int64_t>(FractionAt(pt.x) * m.durationMs));
        } else if (pressed == Hit::Row) {
            const int row = RowAt(pt);
            if (row >= 0 && row == pressedRow && row < static_cast<int>(m.sessions.size())) {
                p.host.pick(m.sessions[row].appId); // the pop-up stays open on the new app
            }
        } else if (pressed != Hit::None && HitTest(pt) == pressed) {
            Activate(pressed);
        }
        if (p.hwnd) {
            Redraw();
            ScheduleProgress();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (p.dragging || p.pressed != Hit::None) {
            p.dragging = false;
            p.pressed = Hit::None;
            Redraw();
            ScheduleProgress();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        // Over the seekbar the wheel seeks: up is forward, like the widget's Shift+scroll.
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        const MediaInfo& m = p.host.media();
        if (HitTest(pt) == Hit::Seek && CanSeek(m)) {
            const int64_t step = kWheelSeekMs * GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            p.host.seek(std::clamp<int64_t>(m.PositionAt(NowFileTime()) + step, 0, m.durationMs));
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerProgress) {
            Redraw();
            ScheduleProgress();
        } else if (wp == kTimerSlide) {
            SlideStep();
        }
        return 0;
    case WM_ERASEBKGND:
        return 1; // Paint covers everything
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (hwnd == p.hwnd) {
            KillTimer(hwnd, kTimerProgress);
            KillTimer(hwnd, kTimerSlide);
            p.hwnd = nullptr;
            p.closedAt = GetTickCount64();
            p.hovered = p.pressed = Hit::None;
            p.hoveredRow = p.pressedRow = p.focusedRow = -1;
            p.dragging = p.trackingMouse = false;
            FreeGraphics(); // everything the pop-up used goes with it
            if (p.host.closed) p.host.closed();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void OpenPopup(HINSTANCE instance, HWND owner, const PopupPlacement& placement, PopupHost host) {
    if (p.hwnd) return;
    static bool registered = [instance] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpfnWndProc = PopupProc;
        wc.lpszClassName = kPopupClass;
        return RegisterClassExW(&wc) != 0;
    }();
    if (!registered) return;

    p.host = std::move(host);
    p.dpi = placement.dpi;
    p.lightTheme = placement.lightTheme;
    const int width = Scale(kWidth), height = Scale(ContentHeight(p.host.media()));

    // Above the taskbar, lined up with the widget's left edge, kept on screen.
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&placement.anchor, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT& area = monitor.rcWork;
    const int x = std::clamp<int>(placement.anchor.left, area.left + Scale(kGapAboveTaskbar),
                                  area.right - width - Scale(kGapAboveTaskbar));
    p.bottom = placement.taskbar.top - Scale(kGapAboveTaskbar);
    const int y = std::max<int>(area.top, p.bottom - height);

    // WS_THICKFRAME: system backdrops (acrylic) are drawn in a window's frame,
    // and a bare WS_POPUP has none. WM_NCCALCSIZE then hides that frame.
    p.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPopupClass, L"TaskbarMedia player",
                             WS_POPUP | WS_THICKFRAME, x, y, width, height, owner, nullptr, instance, nullptr);
    if (!p.hwnd) return;

    const BOOL dark = !p.lightTheme;
    const int corner = kCornerRound, backdrop = kBackdropTransientWindow;
    DwmSetWindowAttribute(p.hwnd, kUseImmersiveDarkMode, &dark, sizeof(dark));
    DwmSetWindowAttribute(p.hwnd, kWindowCornerPreference, &corner, sizeof(corner));
    DwmSetWindowAttribute(p.hwnd, kSystemBackdropType, &backdrop, sizeof(backdrop));
    const MARGINS whole{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(p.hwnd, &whole);

    BOOL animate = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
    if (animate) { // start a little low and rise into place
        SetWindowPos(p.hwnd, nullptr, x, y + Scale(kSlideDistance), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        p.slideStart = GetTickCount64();
        SetTimer(p.hwnd, kTimerSlide, kSlideFrameMs, nullptr);
    }
    ShowWindow(p.hwnd, SW_SHOW);
    SetForegroundWindow(p.hwnd); // so a click anywhere else closes it
    ScheduleProgress();
}

void ClosePopup() {
    if (p.hwnd) DestroyWindow(p.hwnd);
}

bool PopupIsOpen() { return p.hwnd != nullptr; }

void PopupRefresh() {
    if (!p.hwnd || p.dragging) return; // a drag in progress keeps its own position
    FitHeight(); // apps can come and go while it's open
    Redraw();
    ScheduleProgress();
}

UINT PopupArtSize(UINT dpi) { return static_cast<UINT>(MulDiv(kArtSize, static_cast<int>(dpi), 96)); }

ULONGLONG PopupClosedAt() { return p.closedAt; }
