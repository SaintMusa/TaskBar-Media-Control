#include "hotkey.h"
#include <cwchar>
#include <cwctype>

namespace {

struct NamedKey {
    const wchar_t* name;
    UINT key;
};

constexpr NamedKey kNamedKeys[] = {
    {L"Space", VK_SPACE}, {L"Left", VK_LEFT},   {L"Right", VK_RIGHT},   {L"Up", VK_UP},
    {L"Down", VK_DOWN},   {L"Home", VK_HOME},   {L"End", VK_END},       {L"PageUp", VK_PRIOR},
    {L"PageDown", VK_NEXT},
};

} // namespace

bool ParseHotkey(const std::wstring& text, UINT& modifiers, UINT& key) {
    modifiers = 0;
    key = 0;
    size_t start = 0;
    while (start <= text.size() && !text.empty()) {
        size_t end = text.find(L'+', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring part = text.substr(start, end - start);
        const size_t first = part.find_first_not_of(L" \t");
        if (first == std::wstring::npos) return false; // "Ctrl++Up" or a trailing "+"
        part = part.substr(first, part.find_last_not_of(L" \t") - first + 1);
        start = end + 1;
        auto is = [&](const wchar_t* name) { return _wcsicmp(part.c_str(), name) == 0; };
        if (is(L"Ctrl") || is(L"Control")) {
            modifiers |= MOD_CONTROL;
            continue;
        }
        if (is(L"Alt")) {
            modifiers |= MOD_ALT;
            continue;
        }
        if (is(L"Shift")) {
            modifiers |= MOD_SHIFT;
            continue;
        }
        if (is(L"Win") || is(L"Windows")) {
            modifiers |= MOD_WIN;
            continue;
        }
        if (key != 0) return false; // two keys
        bool named = false;
        for (const auto& candidate : kNamedKeys) {
            if (is(candidate.name)) {
                key = candidate.key;
                named = true;
            }
        }
        if (named) continue;
        if (part.size() == 1 && iswalnum(part[0]) && part[0] < 128) {
            key = static_cast<UINT>(towupper(part[0])); // A-Z and 0-9 share their VK codes
        } else if ((part[0] == L'F' || part[0] == L'f') && part.size() <= 3 && _wtoi(part.c_str() + 1) >= 1 &&
                   _wtoi(part.c_str() + 1) <= 24) {
            key = VK_F1 + _wtoi(part.c_str() + 1) - 1;
        } else {
            return false;
        }
    }
    return key != 0 && modifiers != 0;
}

std::wstring FormatHotkey(UINT modifiers, UINT key) {
    std::wstring name;
    for (const auto& candidate : kNamedKeys) {
        if (candidate.key == key) name = candidate.name;
    }
    if (name.empty() && ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))) name = std::wstring(1, static_cast<wchar_t>(key));
    if (name.empty() && key >= VK_F1 && key <= VK_F24) name = L"F" + std::to_wstring(key - VK_F1 + 1);
    if (name.empty()) return {};

    std::wstring text;
    if (modifiers & MOD_CONTROL) text += L"Ctrl+";
    if (modifiers & MOD_ALT) text += L"Alt+";
    if (modifiers & MOD_SHIFT) text += L"Shift+";
    if (modifiers & MOD_WIN) text += L"Win+";
    return text + name;
}
