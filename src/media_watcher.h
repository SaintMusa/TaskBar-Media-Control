#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Cover art (once per track) or an app icon (once per app), decoded at the
// size the widget draws it. Premultiplied BGRA, size x size pixels, square.
struct MediaArt {
    UINT size = 0;
    std::vector<BYTE> pixels;
};

// One app with media, for the "Switch to" menu.
struct SessionSummary {
    std::wstring appId;
    std::wstring appName;
    std::wstring title;  // empty until known: shown once, or listed while the pop-up is open
    std::wstring artist;
    std::shared_ptr<const MediaArt> art;
    bool playing = false;

    bool operator==(const SessionSummary&) const = default;
};

enum class RepeatMode { Off, One, All };

// What the widget shows. Empty appId means no media session exists.
struct MediaInfo {
    std::wstring appId;   // e.g. "Spotify.exe" or a browser's AppUserModelID
    std::wstring appName; // e.g. "Spotify", "Helium"
    std::wstring title;
    std::wstring artist;
    std::shared_ptr<const MediaArt> art;     // null when the app gives none
    std::shared_ptr<const MediaArt> appIcon; // the playing app's own icon, null if not found
    std::shared_ptr<const MediaArt> largeArt; // the cover at pop-up size, only while it's open
    // The cover's main color as hue (0-360) and saturation (0-1), for tinting.
    // Absent for grey or black-and-white covers and when there's no art.
    bool hasAccent = false;
    float accentHue = 0;
    float accentSaturation = 0;
    bool playing = false;
    bool canPrevious = false;
    bool canPlayPause = false;
    bool canNext = false;
    int sessionCount = 0; // apps with media right now; 2+ shows the switch button
    std::vector<SessionSummary> sessions; // all of them, in switching order

    // Only some apps let others change these (Spotify often doesn't, browsers rarely).
    bool canShuffle = false;
    bool shuffle = false;
    bool canRepeat = false;
    RepeatMode repeat = RepeatMode::Off;

    // Timeline, exactly as the app last reported it. Apps report on play,
    // pause and seek only; PositionAt() extrapolates from the clock.
    bool hasTimeline = false; // false for live streams and apps that send none
    int64_t durationMs = 0;
    int64_t positionMs = 0;
    int64_t reportedAt = 0; // FILETIME units (100 ns since 1601), 0 if unknown
    int64_t startMs = 0;    // the timeline's own start; positionMs counts from here
    bool canSeek = false;   // the app lets others change the position
    double rate = 1.0;

    // Playback position in ms at `now` (a FILETIME value).
    int64_t PositionAt(int64_t now) const;

    bool operator==(const MediaInfo&) const = default; // art compares by pointer
};

enum class MediaCommand { None, Previous, PlayPause, Next, ShuffleOn, ShuffleOff, RepeatOff, RepeatOne, RepeatAll };

// Which apps to leave out, and which kinds of app win when several play.
struct AppFilter {
    std::vector<std::wstring> ignored;  // exact app ID or name, ignoring case
    std::wstring pinned;                // same matching; shown whenever it's open
    std::vector<std::wstring> musicApps; // any of these within the app ID or name
    std::vector<std::wstring> browsers;  // same
    bool preferMusicApps = true;
    bool preferNonBrowsers = false;

    bool Ignores(const std::wstring& appId, const std::wstring& appName) const;
    bool IsPinned(const std::wstring& appId, const std::wstring& appName) const;
    bool IsMusicApp(const std::wstring& appId, const std::wstring& appName) const;
    bool IsBrowser(const std::wstring& appId, const std::wstring& appName) const;
};

// Follows the Windows media sessions (GlobalSystemMediaTransportControls).
//
// All decisions run on one worker thread: Windows' event callbacks only wake
// it, so there are no races and no polling. When the shown media changes,
// `changedMsg` is posted to `notifyWnd`.
//
// Which session is shown: the one that most recently started playing. When
// nothing is playing, the last shown session stays (paused). A session picked
// with SwitchSession() stays until it closes or another app starts playing.
class MediaWatcher {
public:
    bool Start(HWND notifyWnd, UINT changedMsg);
    void Stop();
    MediaInfo Latest();

    // Sends a command to the shown session. Returns immediately.
    void Send(MediaCommand command);
    // Shows the next (+1) or previous (-1) media session. Returns immediately.
    void SwitchSession(int direction);
    // Jumps the shown session to this position (ms from the start of the track).
    void SeekTo(int64_t positionMs);
    // Shows this app's session, as a manual pick (like SwitchSession).
    void ShowSession(const std::wstring& appId);
    // Applies new ignore/browser rules and re-picks the shown session.
    void SetFilter(AppFilter filter);
    // Pixel size to decode cover art at; applies from the next track.
    void SetArtSize(UINT pixels) { artSize_ = pixels; }
    // Also decode the cover at this size (for the pop-up), and fetch title and
    // art for every listed app; 0 drops the big cover again.
    void SetLargeArtSize(UINT pixels);

    // How many times the worker woke up, and how many of those were the cheap
    // timeline-only kind (for measuring).
    unsigned Evaluations() const { return evaluations_; }
    unsigned TimelineOnlyEvaluations() const { return timelineOnly_; }
    DWORD ThreadId() { return GetThreadId(thread_.native_handle()); }

private:
    void Run();

    std::atomic<unsigned> evaluations_{0};
    std::atomic<unsigned> timelineOnly_{0};
    // False when the only news is the shown app's timeline: then one cheap
    // re-read is enough instead of re-checking every session.
    std::atomic<bool> needsFullEvaluation_{true};
    std::atomic<int> pendingCommand_{0};
    std::atomic<int> pendingSwitch_{0};
    std::atomic<int64_t> pendingSeekMs_{-1}; // -1 = no seek waiting
    std::atomic<UINT> artSize_{32};
    std::atomic<UINT> largeArtSize_{0};
    HWND notifyWnd_ = nullptr;
    UINT changedMsg_ = 0;
    HANDLE stopEvent_ = nullptr;
    HANDLE wakeEvent_ = nullptr;
    std::mutex mutex_;
    MediaInfo latest_;
    AppFilter filter_;
    std::wstring pendingPick_; // app ID from ShowSession, guarded by mutex_
    std::thread thread_;
};
