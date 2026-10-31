#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Which apps to leave out, and which kinds of app win when several play.
struct AppFilter {
    std::vector<std::wstring> ignored;  // exact app ID or name, ignoring case
    std::wstring pinned;                // same matching; shown whenever it's open
    std::vector<std::wstring> musicApps; // any of these as a whole word in the app ID or name
    std::vector<std::wstring> browsers;  // same
    bool preferMusicApps = true;
    bool preferNonBrowsers = false;

    bool Ignores(const std::wstring& appId, const std::wstring& appName) const;
    bool IsPinned(const std::wstring& appId, const std::wstring& appName) const;
    bool IsMusicApp(const std::wstring& appId, const std::wstring& appName) const;
    bool IsBrowser(const std::wstring& appId, const std::wstring& appName) const;
};

// What choosing a session needs to know about one, free of any Windows objects
// so the rules can be tested without real apps.
struct SessionCandidate {
    bool playing = false;
    uint64_t playingSince = 0; // higher = started playing more recently
    bool isMusic = false;
    bool isBrowser = false;
    bool isPinned = false;
};

struct SessionChoice {
    int index = -1;        // into the candidates; -1 = none of the rules apply
    bool keepPick = false; // the user's manual pick still holds
};

// `pick` is the index of the session the user switched to (-1 for none), and
// `pickedAt` the value of the play counter at that moment.
SessionChoice ChooseSession(const std::vector<SessionCandidate>& candidates, int pick, uint64_t pickedAt,
                            bool preferMusicApps, bool preferNonBrowsers);
