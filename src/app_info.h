#pragma once
#include <windows.h>
#include <string>

// Finding the app behind a media session: its name, its exe and its windows.
//
// A session's app ID is either an AppUserModelID, which Store apps and
// browsers like Helium use ("Helium.BT6SRN..."), or, for plain desktop apps,
// just an exe name ("Spotify.exe").

bool IsExeName(const std::wstring& appId);

// Full path of a running process with this exe name, or empty.
std::wstring RunningExePath(const std::wstring& exeName);

// What to hand the shell to get the app's icon or to launch it: the exe's
// path, or "shell:AppsFolder\<id>". Empty if the exe isn't running.
std::wstring AppShellName(const std::wstring& appId);

// The name people know the app by ("Spotify", "Helium"); the ID if unknown.
std::wstring AppDisplayName(const std::wstring& appId);

// The exe name the app's audio plays from ("helium.exe"), used to find its
// volume control. Empty if it can't be worked out.
std::wstring AppExeName(const std::wstring& appId);

// Brings the app's window to the front, restoring it if minimized. If no
// window is found (say it's in the tray), launches it, which for single-
// instance apps like Spotify brings the running one forward.
bool ActivateApp(const std::wstring& appId);
