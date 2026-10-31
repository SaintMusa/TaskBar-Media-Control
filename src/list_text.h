#pragma once
#include <string>
#include <vector>

// Lists as written in settings.ini: items separated by ";".

// "A; B ;;C" -> {"A", "B", "C"}. Items are trimmed, and empty ones dropped.
std::vector<std::wstring> SplitList(const std::wstring& text);

// {"A", "B"} -> "A;B".
std::wstring JoinList(const std::vector<std::wstring>& items);
