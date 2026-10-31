// Tests for which app the widget shows. Run with: ctest --test-dir build
#include "app_filter.h"
#include <cstdio>

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("FAILED line %d: %s\n", __LINE__, #condition);       \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

SessionCandidate Playing(uint64_t since) {
    SessionCandidate c;
    c.playing = true;
    c.playingSince = since;
    return c;
}

SessionCandidate Paused(uint64_t since = 0) {
    SessionCandidate c = Playing(since);
    c.playing = false;
    return c;
}

SessionCandidate Music(SessionCandidate c) {
    c.isMusic = true;
    return c;
}

SessionCandidate Browser(SessionCandidate c) {
    c.isBrowser = true;
    return c;
}

SessionCandidate Pinned(SessionCandidate c) {
    c.isPinned = true;
    return c;
}

int Chosen(const std::vector<SessionCandidate>& candidates, int pick = -1, uint64_t pickedAt = 0,
           bool preferMusic = false, bool preferNonBrowsers = false) {
    return ChooseSession(candidates, pick, pickedAt, preferMusic, preferNonBrowsers).index;
}

void TestPlayingRules() {
    CHECK(Chosen({}) == -1);
    CHECK(Chosen({Paused(), Paused()}) == -1); // the caller keeps what it showed
    CHECK(Chosen({Paused(), Playing(1)}) == 1);
    CHECK(Chosen({Playing(1), Playing(2)}) == 1); // the newest wins
}

void TestPreferences() {
    const std::vector<SessionCandidate> musicAndNewerVideo = {Music(Playing(1)), Playing(2)};
    CHECK(Chosen(musicAndNewerVideo, -1, 0, true, false) == 0);
    CHECK(Chosen(musicAndNewerVideo, -1, 0, false, false) == 1); // the setting is off

    const std::vector<SessionCandidate> appAndNewerBrowser = {Playing(1), Browser(Playing(2))};
    CHECK(Chosen(appAndNewerBrowser, -1, 0, false, true) == 0);
    CHECK(Chosen(appAndNewerBrowser, -1, 0, false, false) == 1);

    // Music outranks non-browser when the two disagree.
    const std::vector<SessionCandidate> musicInBrowser = {Playing(2), Browser(Music(Playing(1)))};
    CHECK(Chosen(musicInBrowser, -1, 0, true, true) == 1);
}

void TestManualPick() {
    const std::vector<SessionCandidate> sessions = {Playing(1), Paused(), Playing(2)};
    const SessionChoice held = ChooseSession(sessions, 1, 2, false, false);
    CHECK(held.index == 1);
    CHECK(held.keepPick);

    // Something started playing after the pick, so the pick ends.
    const SessionChoice ended = ChooseSession(sessions, 1, 1, false, false);
    CHECK(ended.index == 2);
    CHECK(!ended.keepPick);
}

void TestPinning() {
    CHECK(Chosen({Playing(1), Pinned(Paused())}) == 1); // beats playing apps, even paused

    const std::vector<SessionCandidate> sessions = {Playing(1), Pinned(Playing(2))};
    const SessionChoice held = ChooseSession(sessions, 0, 2, false, false);
    CHECK(held.index == 0); // a swap away from it holds...
    CHECK(held.keepPick);

    const SessionChoice ended = ChooseSession(sessions, 0, 1, false, false);
    CHECK(ended.index == 1); // ...until the pinned app starts playing
    CHECK(!ended.keepPick);

    // Another app starting doesn't end the swap; only the pinned one does.
    const std::vector<SessionCandidate> other = {Playing(1), Pinned(Paused(1)), Playing(5)};
    CHECK(Chosen(other, 0, 2) == 0);

    CHECK(ChooseSession(sessions, 1, 2, false, false).keepPick == false); // picking it isn't a swap
}

void TestWordMatching() {
    AppFilter filter;
    filter.browsers = {L"Arc", L"Edge", L"Chrome"};
    CHECK(filter.IsBrowser(L"Arc.exe", L""));
    CHECK(filter.IsBrowser(L"TheBrowserCompany.Arc_ttt1ap7aakyb4!Arc", L""));
    CHECK(filter.IsBrowser(L"chrome.exe", L"Google Chrome")); // ignores case
    CHECK(filter.IsBrowser(L"MSEdge", L"Microsoft Edge"));    // by its name, not its ID
    CHECK(!filter.IsBrowser(L"Search.exe", L"Search"));
    CHECK(!filter.IsBrowser(L"Arcade.exe", L"Arcade"));
    CHECK(!filter.IsBrowser(L"MSEdge", L"MSEdge"));

    filter.browsers = {L"Arc", L""}; // a stray ";" in the settings file makes an empty entry
    CHECK(!filter.IsBrowser(L"anything", L"anything"));

    // A later match still counts when the first is part of a longer word.
    filter.browsers = {L"Arc"};
    CHECK(filter.IsBrowser(L"Search Arc", L""));
}

void TestExactMatching() {
    AppFilter filter;
    filter.ignored = {L"Spotify", L"msedge.exe"};
    filter.pinned = L"Spotify";
    CHECK(filter.Ignores(L"Spotify.exe", L"spotify"));
    CHECK(filter.Ignores(L"MSEDGE.EXE", L"Microsoft Edge"));
    CHECK(!filter.Ignores(L"SpotifyLauncher.exe", L"Spotify Launcher")); // exact, not whole-word
    CHECK(filter.IsPinned(L"Spotify.exe", L"Spotify"));
    filter.pinned.clear();
    CHECK(!filter.IsPinned(L"", L"")); // nothing pinned must not match an empty name
}

} // namespace

int main() {
    TestPlayingRules();
    TestPreferences();
    TestManualPick();
    TestPinning();
    TestWordMatching();
    TestExactMatching();
    std::printf(failures ? "%d check(s) failed\n" : "All checks passed\n", failures);
    return failures ? 1 : 0;
}
