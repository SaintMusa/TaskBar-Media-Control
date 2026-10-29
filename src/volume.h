#pragma once
#include <windows.h>
#include <optional>
#include <string>

// Changes the volume of one app only (as in the Windows volume mixer), not
// the whole system. Finds the app's audio sessions on the default output
// device by exe name and moves them all by `delta` (-1..1). Returns the new
// level (0..1), or nothing if the app has no audio session right now.
std::optional<float> ChangeAppVolume(const std::wstring& exeName, float delta);
