#include "app_filter.h"
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <tuple>

namespace {

// A whole word, ignoring case, so "Arc" finds "Arc.exe" but not "Search".
bool ContainsWord(const std::wstring& text, const std::wstring& word) {
    if (word.empty()) return false;
    auto next = [&](std::wstring::const_iterator from) {
        return std::search(from, text.end(), word.begin(), word.end(),
                           [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); });
    };
    for (auto found = next(text.begin()); found != text.end(); found = next(found + 1)) {
        const auto end = found + word.size();
        const bool startsWord = found == text.begin() || !iswalnum(*(found - 1));
        const bool endsWord = end == text.end() || !iswalnum(*end);
        if (startsWord && endsWord) return true;
    }
    return false;
}

bool AnyWithin(const std::vector<std::wstring>& words, const std::wstring& appId, const std::wstring& appName) {
    for (const auto& word : words) {
        if (ContainsWord(appId, word) || ContainsWord(appName, word)) return true;
    }
    return false;
}

} // namespace

bool AppFilter::IsPinned(const std::wstring& appId, const std::wstring& appName) const {
    return !pinned.empty() &&
           (_wcsicmp(pinned.c_str(), appId.c_str()) == 0 || _wcsicmp(pinned.c_str(), appName.c_str()) == 0);
}

bool AppFilter::Ignores(const std::wstring& appId, const std::wstring& appName) const {
    for (const auto& entry : ignored) {
        if (_wcsicmp(entry.c_str(), appId.c_str()) == 0 || _wcsicmp(entry.c_str(), appName.c_str()) == 0) return true;
    }
    return false;
}

bool AppFilter::IsMusicApp(const std::wstring& appId, const std::wstring& appName) const {
    return AnyWithin(musicApps, appId, appName);
}

bool AppFilter::IsBrowser(const std::wstring& appId, const std::wstring& appName) const {
    return AnyWithin(browsers, appId, appName);
}

SessionChoice ChooseSession(const std::vector<SessionCandidate>& candidates, int pick, uint64_t pickedAt,
                            bool preferMusicApps, bool preferNonBrowsers) {
    // Among playing apps: a music app first (if preferred), then a non-browser
    // (if preferred), then whichever started playing last.
    auto rank = [&](const SessionCandidate& c) {
        return std::make_tuple(preferMusicApps && c.isMusic, preferNonBrowsers && !c.isBrowser, c.playingSince);
    };
    int best = -1, pinned = -1;
    uint64_t newestStart = 0;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        const SessionCandidate& c = candidates[i];
        if (c.playing && (best < 0 || rank(c) > rank(candidates[best]))) best = i;
        if (c.playing) newestStart = std::max(newestStart, c.playingSince);
        if (c.isPinned) pinned = i;
    }
    // A pinned app beats all of the above whenever it's open. Switching away
    // from it still works and stays, playing or paused, until you switch again,
    // that app closes, or the pinned app starts playing.
    if (pinned >= 0) {
        const bool swapHolds = pick >= 0 && pick != pinned && candidates[pinned].playingSince <= pickedAt;
        return {swapHolds ? pick : pinned, swapHolds};
    }
    if (pick >= 0 && newestStart <= pickedAt) return {pick, true};
    return {best, false};
}
