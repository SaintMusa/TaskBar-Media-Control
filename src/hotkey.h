#pragma once
#include <windows.h>
#include <string>

// Hotkeys as written in settings.ini: modifiers and one key joined with "+",
// like "Ctrl+Alt+Up". Ctrl, Alt, Shift and Win; Space, the arrows, Home, End,
// PageUp, PageDown, A-Z, 0-9 and F1-F24.

// "Ctrl+Alt+Up" -> RegisterHotKey modifiers (MOD_*) and a virtual key. Returns
// false if it can't be read, or has no modifier (a bare key would swallow typing).
bool ParseHotkey(const std::wstring& text, UINT& modifiers, UINT& key);

// The reverse: the text for a key combination, or empty when the key has no name
// ParseHotkey understands (punctuation, for one).
std::wstring FormatHotkey(UINT modifiers, UINT key);
