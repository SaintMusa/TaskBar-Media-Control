#include "app_info.h"
#include <ole2.h>
#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ProcessExePath(DWORD pid) {
    std::wstring path;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buffer[MAX_PATH];
        DWORD length = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, buffer, &length)) path.assign(buffer, length);
        CloseHandle(process);
    }
    return path;
}

// The AppUserModelID a window is grouped under on the taskbar, if it sets one.
std::wstring WindowAppId(HWND hwnd) {
    std::wstring id;
    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
            id = value.pwszVal;
        }
        PropVariantClear(&value);
    }
    return id;
}

// The app's main windows in z-order, so the most recently used one comes first.
HWND FindAppWindow(const std::wstring& appId) {
    struct Search {
        const std::wstring* appId;
        bool byExe;
        HWND found;
    } search{&appId, IsExeName(appId), nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(lp);
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) ||
                (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) || GetWindowTextLengthW(hwnd) == 0) {
                return TRUE;
            }
            bool match;
            if (s.byExe) {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                match = _wcsicmp(FileNameOf(ProcessExePath(pid)).c_str(), s.appId->c_str()) == 0;
            } else {
                match = _wcsicmp(WindowAppId(hwnd).c_str(), s.appId->c_str()) == 0;
            }
            if (match) s.found = hwnd;
            return !match;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// "Spotify" from the exe's own description, rather than "Spotify.exe".
std::wstring ExeDescription(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    struct Translation {
        WORD language, codePage;
    }* translations = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length) ||
        length < sizeof(Translation)) {
        return {};
    }
    wchar_t key[64];
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[0].language,
               translations[0].codePage);
    wchar_t* description = nullptr;
    if (!VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&description), &length) || length <= 1) return {};
    return description;
}

} // namespace

bool IsExeName(const std::wstring& appId) {
    return appId.size() > 4 && _wcsicmp(appId.c_str() + appId.size() - 4, L".exe") == 0;
}

std::wstring RunningExePath(const std::wstring& exeName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return {};
    std::wstring path;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && path.empty(); ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, exeName.c_str()) == 0) path = ProcessExePath(entry.th32ProcessID);
    }
    CloseHandle(snapshot);
    return path;
}

std::wstring AppShellName(const std::wstring& appId) {
    if (appId.empty()) return {};
    return IsExeName(appId) ? RunningExePath(appId) : L"shell:AppsFolder\\" + appId;
}

std::wstring AppDisplayName(const std::wstring& appId) {
    if (IsExeName(appId)) {
        std::wstring name = ExeDescription(RunningExePath(appId));
        return name.empty() ? appId.substr(0, appId.size() - 4) : name;
    }
    ComPtr<IShellItem> item;
    wchar_t* name = nullptr;
    std::wstring result = appId;
    if (SUCCEEDED(SHCreateItemFromParsingName(AppShellName(appId).c_str(), nullptr, IID_PPV_ARGS(&item))) &&
        SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name))) {
        result = name;
        CoTaskMemFree(name);
    }
    return result;
}

std::wstring AppExeName(const std::wstring& appId) {
    if (IsExeName(appId)) return appId;
    HWND window = FindAppWindow(appId);
    if (!window) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    return FileNameOf(ProcessExePath(pid));
}

bool ActivateApp(const std::wstring& appId) {
    if (HWND window = FindAppWindow(appId)) {
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        return SetForegroundWindow(window) != FALSE;
    }
    const std::wstring target = AppShellName(appId);
    return !target.empty() &&
           reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
