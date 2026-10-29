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
    L"Browsers=Helium;Chrome;Edge;Firefox;Brave;Opera;Vivaldi;Zen;Arc\r\n";

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

std::wstring SettingsPath() {
    static const std::wstring path = [] {
        PWSTR appData = nullptr;
        std::wstring dir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) dir = appData;
        CoTaskMemFree(appData);
        dir += L"\\TaskbarMedia";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\settings.ini";
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
