#include "popup.h"
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kPopupClass[] = L"TaskbarMedia.Popup";
constexpr int kWidth = 320;        // DIPs
constexpr int kHeight = 200;       // step 1: a fixed test size
constexpr int kGapAboveTaskbar = 12; // like Windows' own flyouts

// DWM attributes newer than some SDK headers.
constexpr DWORD kUseImmersiveDarkMode = 20;  // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kWindowCornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kSystemBackdropType = 38;    // DWMWA_SYSTEMBACKDROP_TYPE
constexpr int kCornerRound = 2;              // DWMWCP_ROUND
constexpr int kBackdropTransientWindow = 3;  // DWMSBT_TRANSIENTWINDOW: acrylic, as in flyouts

struct PopupState {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    bool lightTheme = false;
    ULONGLONG closedAt = 0;

    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<IDWriteTextFormat> textFormat;
    HDC memDC = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    SIZE bitmapSize{};
} p;

int Scale(int dip) { return MulDiv(dip, static_cast<int>(p.dpi), 96); }

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
    p.textFormat.Reset();
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
    if (!p.textFormat) {
        if (FAILED(p.dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              16.0f * p.dpi / 96.0f, L"", &p.textFormat))) {
            return false;
        }
        p.textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        p.textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
    return true;
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

    const auto ink = p.lightTheme ? D2D1::ColorF(0, 0, 0, 0.9f) : D2D1::ColorF(1, 1, 1, 0.92f);
    ComPtr<ID2D1SolidColorBrush> text;
    p.target->CreateSolidColorBrush(ink, &text);
    p.target->BeginDraw();
    p.target->Clear(D2D1::ColorF(0, 0, 0, 0));
    constexpr wchar_t label[] = L"Pop-up test";
    p.target->DrawTextW(label, static_cast<UINT32>(wcslen(label)), p.textFormat.Get(),
                        D2D1::RectF(0, 0, static_cast<float>(size.cx), static_cast<float>(size.cy)), text.Get());
    if (p.target->EndDraw() == D2DERR_RECREATE_TARGET) p.target.Reset();
    BitBlt(windowDC, 0, 0, size.cx, size.cy, p.memDC, 0, 0, SRCCOPY);
}

LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ACTIVATE:
        // Clicking anywhere else takes focus away: that's the cue to close.
        if (LOWORD(wp) == WA_INACTIVE) ClosePopup();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) ClosePopup();
        return 0;
    case WM_NCCALCSIZE:
        // The window has a frame style only so DWM gives it a backdrop; this
        // makes all of it client area, so no frame is actually drawn.
        if (wp) return 0;
        break;
    case WM_NCHITTEST:
        return HTCLIENT; // no resizing from the edges the frame style would add
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
            p.hwnd = nullptr;
            p.closedAt = GetTickCount64();
            FreeGraphics(); // everything the pop-up used goes with it
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void OpenPopup(HINSTANCE instance, HWND owner, const PopupPlacement& placement) {
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

    p.dpi = placement.dpi;
    p.lightTheme = placement.lightTheme;
    const int width = Scale(kWidth), height = Scale(kHeight);

    // Above the taskbar, lined up with the widget's left edge, kept on screen.
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&placement.anchor, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT& area = monitor.rcWork;
    const int x = std::clamp<int>(placement.anchor.left, area.left + Scale(kGapAboveTaskbar),
                                  area.right - width - Scale(kGapAboveTaskbar));
    const int y = std::max<int>(area.top, placement.taskbar.top - height - Scale(kGapAboveTaskbar));

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

    ShowWindow(p.hwnd, SW_SHOW);
    SetForegroundWindow(p.hwnd); // so a click anywhere else closes it
}

void ClosePopup() {
    if (p.hwnd) DestroyWindow(p.hwnd);
}

bool PopupIsOpen() { return p.hwnd != nullptr; }

ULONGLONG PopupClosedAt() { return p.closedAt; }
