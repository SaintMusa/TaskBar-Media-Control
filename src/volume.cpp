#include "volume.h"
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace {

std::wstring ExeNameOf(DWORD pid) {
    std::wstring name;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD length = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, path, &length)) {
            const wchar_t* slash = wcsrchr(path, L'\\');
            name = slash ? slash + 1 : path;
        }
        CloseHandle(process);
    }
    return name;
}

} // namespace

std::optional<float> ChangeAppVolume(const std::wstring& exeName, float delta) {
    if (exeName.empty()) return std::nullopt;
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDevice> output;
    ComPtr<IAudioSessionManager2> manager;
    ComPtr<IAudioSessionEnumerator> sessions;
    int count = 0;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devices))) ||
        FAILED(devices->GetDefaultAudioEndpoint(eRender, eMultimedia, &output)) ||
        FAILED(output->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr, &manager)) ||
        FAILED(manager->GetSessionEnumerator(&sessions)) || FAILED(sessions->GetCount(&count))) {
        return std::nullopt;
    }

    // Browsers can have several sessions (one per audio process); move them together.
    std::optional<float> level;
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        ComPtr<IAudioSessionControl2> control2;
        ComPtr<ISimpleAudioVolume> volume;
        DWORD pid = 0;
        if (FAILED(sessions->GetSession(i, &control)) || FAILED(control.As(&control2)) ||
            control2->GetProcessId(&pid) != S_OK || pid == 0 ||
            _wcsicmp(ExeNameOf(pid).c_str(), exeName.c_str()) != 0 || FAILED(control.As(&volume))) {
            continue;
        }
        float current = 0;
        if (FAILED(volume->GetMasterVolume(&current))) continue;
        const float next = std::clamp(current + delta, 0.0f, 1.0f);
        if (SUCCEEDED(volume->SetMasterVolume(next, nullptr)) && !level) level = next;
    }
    return level;
}
