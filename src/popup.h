#pragma once
#include <windows.h>
#include <d2d1.h>
#include <functional>
#include "media_watcher.h"

// The pop-up player that opens above the widget: its own top-level window
// with Windows 11's acrylic backdrop and rounded corners. It exists only while
// open, so it costs nothing while closed.

struct PopupPlacement {
    RECT anchor;        // the widget, in screen pixels; the pop-up sits above it
    RECT taskbar;       // the taskbar, so the pop-up clears it
    UINT dpi = 96;
    bool lightTheme = false;
};

// What the pop-up needs from the rest of the app.
struct PopupHost {
    std::function<const MediaInfo&()> media;  // what's playing right now
    std::function<D2D1_COLOR_F()> accent;     // the cover's color, for the seekbar
    std::function<void(MediaCommand)> send;
    std::function<void(int64_t positionMs)> seek; // from the start of the track
    std::function<void()> openApp;
    std::function<void()> closed;             // the pop-up has just closed
};

void OpenPopup(HINSTANCE instance, HWND owner, const PopupPlacement& placement, PopupHost host);
void ClosePopup();
bool PopupIsOpen();

// What's playing changed: redraw with host.media().
void PopupRefresh();

// The pixel size the pop-up draws the cover at, to decode it at exactly that.
UINT PopupArtSize(UINT dpi);

// When the pop-up last closed (GetTickCount64). Clicking the widget while the
// pop-up is open first closes it by taking focus away; this lets that same
// click be recognized instead of reopening it straight away.
ULONGLONG PopupClosedAt();
