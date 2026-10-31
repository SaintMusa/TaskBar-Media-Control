#pragma once
#include <windows.h>
#include <functional>
#include "settings.h"

// The settings window: three pages (General, Apps, Hotkeys) that change the
// same settings.ini the file editor does. A change applies and saves at once,
// and edits made to the file by hand show up here while it's open. Like the
// pop-up, it exists only while open.

// What the window needs from the rest of the app.
struct SettingsHost {
    std::function<Settings()> get;                   // the settings in effect now
    std::function<void(const Settings&)> apply;     // save and apply new ones
    std::function<bool()> lightTheme;               // the system theme
    std::function<void()> openFile;                 // open settings.ini in an editor
    std::function<void(bool paused)> pauseHotkeys;  // so a key press can be captured
                                                     // even if it's already a hotkey
};

// Opens the window, or brings it forward if it's already open.
void OpenSettingsWindow(HINSTANCE instance, UINT dpi, SettingsHost host);
void CloseSettingsWindow();

// The settings (or the theme) changed somewhere else: redraw to match.
void SettingsWindowRefresh();
