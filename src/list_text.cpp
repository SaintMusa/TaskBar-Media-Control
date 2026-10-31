#include "list_text.h"

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
