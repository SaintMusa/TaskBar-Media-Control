#pragma once
#include <windows.h>
#include <string>
#include <vector>

// User settings, kept in %APPDATA%\TaskbarMedia\settings.ini. The file is
// plain text with a comment on every setting, so it can be edited by hand;
// the app watches it and applies changes as soon as it's saved.
struct Settings {
    // Keep showing the last track while paused instead of hiding after a delay.
    bool stayVisibleWhenPaused = false;
    UINT hideAfterPausedSeconds = 30;

    // Apps never to show, matched exactly (ignoring case) against the app's
    // name ("Spotify"), exe ("msedge.exe") or AppUserModelID.
    std::vector<std::wstring> ignoredApps;
    // Show a browser only while no other app is playing.
    bool preferNonBrowsers = false;
    // What counts as a browser: matched against the app's name or ID.
    std::vector<std::wstring> browsers;

    bool operator==(const Settings&) const = default;
};

std::wstring SettingsPath();
// Creates the file with defaults and comments on first run.
Settings LoadSettings();
void SaveSettings(const Settings& settings);

// "Start with Windows" lives in the registry (HKCU Run), not the file.
bool StartsWithWindows();
void SetStartWithWindows(bool enabled);
