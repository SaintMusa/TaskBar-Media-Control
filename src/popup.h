#pragma once
#include <windows.h>

// The pop-up player that opens above the widget: its own top-level window
// with Windows 11's acrylic backdrop and rounded corners. It exists only while
// open, so it costs nothing while closed.

struct PopupPlacement {
    RECT anchor;        // the widget, in screen pixels; the pop-up sits above it
    RECT taskbar;       // the taskbar, so the pop-up clears it
    UINT dpi = 96;
    bool lightTheme = false;
};

void OpenPopup(HINSTANCE instance, HWND owner, const PopupPlacement& placement);
void ClosePopup();
bool PopupIsOpen();

// When the pop-up last closed (GetTickCount64). Clicking the widget while the
// pop-up is open first closes it by taking focus away; this lets that same
// click be recognized instead of reopening it straight away.
ULONGLONG PopupClosedAt();
