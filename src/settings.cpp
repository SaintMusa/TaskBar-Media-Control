#include "settings.h"
#include <shlobj.h>
#include <cstdio>

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"TaskbarMedia";
constexpr wchar_t kDefaultMusicApps[] =
    L"Spotify;Apple Music;iTunes;Amazon Music;TIDAL;Deezer;YouTube Music;foobar2000;MusicBee;AIMP;Winamp";
constexpr wchar_t kDefaultBrowsers[] = L"Helium;Chrome;Edge;Firefox;Brave;Opera;Vivaldi;Zen;Arc";

// UTF-16 with a byte-order mark: the profile APIs then read and write
// Unicode, so app names in any language survive.
constexpr wchar_t kTemplate[] =
    L"\xFEFF" L"; TaskbarMedia settings. Changes apply as soon as you save this file.\r\n"
    L"\r\n"
    L"[General]\r\n"
    L"; 1 = keep showing the last track while paused.\r\n"
    L"; 0 = hide after HideAfterPausedSeconds.\r\n"
    L"StayVisibleWhenPaused=0\r\n"
    L"HideAfterPausedSeconds=30\r\n"
    L"\r\n"
    L"[Apps]\r\n"
    L"; Apps never to show, separated by ;  Use the name from the right-click\r\n"
    L"; menu (Spotify), an exe name (msedge.exe) or an app ID.\r\n"
    L"Ignore=\r\n"
    L"; An app to keep on the widget whenever it's open, even while others play.\r\n"
    L"; Switching away lasts until you switch back or the pinned app plays.\r\n"
    L"Pinned=\r\n"
    L"; 1 = while a music app is playing, show it over any other app.\r\n"
    L"PreferMusicApps=1\r\n"
    L"; What counts as a music app, separated by ;\r\n"
    L"MusicApps=Spotify;Apple Music;iTunes;Amazon Music;TIDAL;Deezer;YouTube Music;foobar2000;MusicBee;AIMP;Winamp\r\n"
    L"; 1 = show a browser only while no other app is playing.\r\n"
    L"PreferNonBrowsers=0\r\n"
    L"; What counts as a browser, separated by ;\r\n"
    L"Browsers=Helium;Chrome;Edge;Firefox;Brave;Opera;Vivaldi;Zen;Arc\r\n"
    L"\r\n"
    L"[Hotkeys]\r\n"
    L"; Off unless filled in. They work from any app: Ctrl, Alt, Shift and Win plus\r\n"
    L"; one key, joined with + (Space, Left, Right, Up, Down, A-Z, 0-9, F1-F24).\r\n"
    L"; For example: PlayPause=Ctrl+Alt+Up  Next=Ctrl+Alt+Right  Previous=Ctrl+Alt+Left\r\n"
    L"PlayPause=\r\n"
    L"Next=\r\n"
    L"Previous=\r\n"
    L"SwitchApp=\r\n";

std::wstring ReadString(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
    wchar_t buffer[2048];
    GetPrivateProfileStringW(section, key, fallback, buffer, ARRAYSIZE(buffer), SettingsPath().c_str());
    return buffer;
}

std::vector<std::wstring> SplitList(const std::wstring& text) {
    std::vector<std::wstring> items;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L';', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring item = text.substr(start, end - start);
        const size_t first = item.find_first_not_of(L" \t");
        const size_t last = item.find_last_not_of(L" \t");
        if (first != std::wstring::npos) items.push_back(item.substr(first, last - first + 1));
        start = end + 1;
    }
    return items;
}

std::wstring JoinList(const std::vector<std::wstring>& items) {
    std::wstring text;
    for (const auto& item : items) text += (text.empty() ? L"" : L";") + item;
    return text;
}

} // namespace

std::wstring AppDataSettingsPath() {
    PWSTR appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) dir = appData;
    CoTaskMemFree(appData);
    return dir + L"\\TaskbarMedia\\settings.ini";
}

// Whether we may create files in this folder (Program Files, for one, says no).
bool CanWriteTo(const std::wstring& dir) {
    HANDLE probe = CreateFileW((dir + L"\\.taskbarmedia-write-test").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (probe == INVALID_HANDLE_VALUE) return false;
    CloseHandle(probe);
    return true;
}

// Next to the exe, like a portable app, so it's easy to find and opens in any
// editor. (The Store version of Notepad can't open files in AppData folders
// made by other apps.) Falls back to AppData where the exe's folder is read-only.
std::wstring SettingsPath() {
    static const std::wstring path = [] {
        wchar_t exe[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe, length > 0 && length < MAX_PATH ? length : 0);
        dir = dir.substr(0, dir.find_last_of(L'\\'));
        if (!dir.empty() && CanWriteTo(dir)) {
            const std::wstring portable = dir + L"\\settings.ini";
            // Earlier versions kept the file in AppData; bring it along once.
            const std::wstring old = AppDataSettingsPath();
            if (GetFileAttributesW(portable.c_str()) == INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES) {
                CopyFileW(old.c_str(), portable.c_str(), TRUE);
            }
            return portable;
        }
        const std::wstring fallback = AppDataSettingsPath();
        CreateDirectoryW(fallback.substr(0, fallback.find_last_of(L'\\')).c_str(), nullptr);
        return fallback;
    }();
    return path;
}

Settings LoadSettings() {
    const std::wstring path = SettingsPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"wb") == 0 && file) {
            fwrite(kTemplate, sizeof(wchar_t), ARRAYSIZE(kTemplate) - 1, file);
            fclose(file);
        }
    }
    Settings s;
    s.stayVisibleWhenPaused = GetPrivateProfileIntW(L"General", L"StayVisibleWhenPaused", 0, path.c_str()) != 0;
    s.hideAfterPausedSeconds = GetPrivateProfileIntW(L"General", L"HideAfterPausedSeconds", 30, path.c_str());
    s.ignoredApps = SplitList(ReadString(L"Apps", L"Ignore", L""));
    s.pinnedApp = ReadString(L"Apps", L"Pinned", L"");
    s.preferMusicApps = GetPrivateProfileIntW(L"Apps", L"PreferMusicApps", 1, path.c_str()) != 0;
    s.musicApps = SplitList(ReadString(L"Apps", L"MusicApps", kDefaultMusicApps));
    s.preferNonBrowsers = GetPrivateProfileIntW(L"Apps", L"PreferNonBrowsers", 0, path.c_str()) != 0;
    s.browsers = SplitList(ReadString(L"Apps", L"Browsers", kDefaultBrowsers));
    const Settings defaults;
    s.hotkeyPlayPause = ReadString(L"Hotkeys", L"PlayPause", defaults.hotkeyPlayPause.c_str());
    s.hotkeyNext = ReadString(L"Hotkeys", L"Next", defaults.hotkeyNext.c_str());
    s.hotkeyPrevious = ReadString(L"Hotkeys", L"Previous", defaults.hotkeyPrevious.c_str());
    s.hotkeySwitchApp = ReadString(L"Hotkeys", L"SwitchApp", defaults.hotkeySwitchApp.c_str());
    return s;
}

// WritePrivateProfileString updates values in place, keeping the comments.
void SaveSettings(const Settings& s) {
    const std::wstring path = SettingsPath();
    WritePrivateProfileStringW(L"General", L"StayVisibleWhenPaused", s.stayVisibleWhenPaused ? L"1" : L"0",
                               path.c_str());
    WritePrivateProfileStringW(L"General", L"HideAfterPausedSeconds",
                               std::to_wstring(s.hideAfterPausedSeconds).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Apps", L"Ignore", JoinList(s.ignoredApps).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Apps", L"Pinned", s.pinnedApp.c_str(), path.c_str());
    WritePrivateProfileStringW(L"Apps", L"PreferMusicApps", s.preferMusicApps ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Apps", L"MusicApps", JoinList(s.musicApps).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Apps", L"PreferNonBrowsers", s.preferNonBrowsers ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Apps", L"Browsers", JoinList(s.browsers).c_str(), path.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"PlayPause", s.hotkeyPlayPause.c_str(), path.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"Next", s.hotkeyNext.c_str(), path.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"Previous", s.hotkeyPrevious.c_str(), path.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"SwitchApp", s.hotkeySwitchApp.c_str(), path.c_str());
}

bool StartsWithWindows() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) ==
           ERROR_SUCCESS;
}

void SetStartWithWindows(bool enabled) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enabled) {
        wchar_t exe[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            const std::wstring command = L"\"" + std::wstring(exe) + L"\"";
            RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}
