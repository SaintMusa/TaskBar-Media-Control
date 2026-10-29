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

// What the widget shows. Empty appId means no media session exists.
struct MediaInfo {
    std::wstring appId;   // e.g. "Spotify.exe" or a browser's AppUserModelID
    std::wstring appName; // e.g. "Spotify", "Helium"
    std::wstring title;
    std::wstring artist;
    std::shared_ptr<const MediaArt> art;     // null when the app gives none
    std::shared_ptr<const MediaArt> appIcon; // the playing app's own icon, null if not found
    bool playing = false;
    bool canPrevious = false;
    bool canPlayPause = false;
    bool canNext = false;
    int sessionCount = 0; // apps with media right now; 2+ shows the switch button

    // Timeline, exactly as the app last reported it. Apps report on play,
    // pause and seek only; PositionAt() extrapolates from the clock.
    bool hasTimeline = false; // false for live streams and apps that send none
    int64_t durationMs = 0;
    int64_t positionMs = 0;
    int64_t reportedAt = 0; // FILETIME units (100 ns since 1601), 0 if unknown
    double rate = 1.0;

    // Playback position in ms at `now` (a FILETIME value).
    int64_t PositionAt(int64_t now) const;

    bool operator==(const MediaInfo&) const = default; // art compares by pointer
};

enum class MediaCommand { None, Previous, PlayPause, Next };

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
    // Pixel size to decode cover art at; applies from the next track.
    void SetArtSize(UINT pixels) { artSize_ = pixels; }

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
    std::atomic<UINT> artSize_{32};
    HWND notifyWnd_ = nullptr;
    UINT changedMsg_ = 0;
    HANDLE stopEvent_ = nullptr;
    HANDLE wakeEvent_ = nullptr;
    std::mutex mutex_;
    MediaInfo latest_;
    std::thread thread_;
};
