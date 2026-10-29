#include "media_watcher.h"
#include "app_info.h"
#include <unknwn.h> // before C++/WinRT, so winrt::get_unknown interops with classic COM
#include <ole2.h>
#include <shcore.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <map>
#include <tuple>
#include <utility>

using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Media::Control;
using winrt::Windows::Storage::Streams::IRandomAccessStreamReference;
using Session = GlobalSystemMediaTransportControlsSession;
using Status = GlobalSystemMediaTransportControlsSessionPlaybackStatus;

namespace {

// Apps often fire several updates per track change (title, then artist, then
// playback state). Waiting this long after the last one evaluates them once.
constexpr DWORD kCoalesceMs = 50;

struct Tracked {
    Session session{nullptr};
    std::wstring appId;
    std::wstring appName;
    bool isMusic = false;
    bool isBrowser = false;
    std::wstring title;
    std::wstring artist;
    std::shared_ptr<const MediaArt> art;
    // Set from Windows' callback thread when title/artist/art change. Browsers
    // send playback updates far more often than metadata ones, so we only
    // re-read the (cross-process, async) media properties when this says so.
    std::shared_ptr<std::atomic<bool>> propertiesStale = std::make_shared<std::atomic<bool>>(true);
    // Whether this is the session on screen. Timeline updates from the others
    // are ignored without even waking the worker (live streams send many).
    std::shared_ptr<std::atomic<bool>> isShown = std::make_shared<std::atomic<bool>>(false);
    bool wasPlaying = false;
    bool canPrevious = false, canPlayPause = false, canNext = false;
    bool canShuffle = false, shuffle = false, canRepeat = false;
    RepeatMode repeat = RepeatMode::Off;
    double rate = 1.0;
    uint64_t playingSince = 0; // higher = started playing more recently
    Session::MediaPropertiesChanged_revoker propertiesChanged;
    Session::PlaybackInfoChanged_revoker playbackChanged;
    Session::TimelinePropertiesChanged_revoker timelineChanged;
};

// Reads play state and which buttons the app currently allows.
void ReadPlayback(Tracked& t) {
    t.wasPlaying = t.canPrevious = t.canPlayPause = t.canNext = false;
    t.canShuffle = t.shuffle = t.canRepeat = false;
    t.repeat = RepeatMode::Off;
    t.rate = 1.0;
    try {
        auto info = t.session.GetPlaybackInfo();
        if (!info) return;
        t.wasPlaying = info.PlaybackStatus() == Status::Playing;
        auto controls = info.Controls();
        t.canPrevious = controls.IsPreviousEnabled();
        t.canNext = controls.IsNextEnabled();
        t.canPlayPause = controls.IsPlayPauseToggleEnabled() || controls.IsPlayEnabled() || controls.IsPauseEnabled();
        if (auto rate = info.PlaybackRate(); rate && rate.Value() > 0) t.rate = rate.Value();
        t.canShuffle = controls.IsShuffleEnabled();
        t.canRepeat = controls.IsRepeatEnabled();
        if (auto shuffle = info.IsShuffleActive()) t.shuffle = shuffle.Value();
        if (auto repeat = info.AutoRepeatMode()) {
            using winrt::Windows::Media::MediaPlaybackAutoRepeatMode;
            t.repeat = repeat.Value() == MediaPlaybackAutoRepeatMode::Track  ? RepeatMode::One
                       : repeat.Value() == MediaPlaybackAutoRepeatMode::List ? RepeatMode::All
                                                                             : RepeatMode::Off;
        }
    } catch (...) {
    }
}

// Fills the timeline fields, leaving hasTimeline false for live streams or
// nonsense values (some apps report zeros or a negative end).
void ReadTimeline(const Session& session, MediaInfo& info) {
    try {
        auto timeline = session.GetTimelineProperties();
        using std::chrono::duration_cast;
        using std::chrono::milliseconds;
        const int64_t start = duration_cast<milliseconds>(timeline.StartTime()).count();
        const int64_t end = duration_cast<milliseconds>(timeline.EndTime()).count();
        const int64_t position = duration_cast<milliseconds>(timeline.Position()).count();
        if (end - start < 1000) return;
        info.hasTimeline = true;
        info.durationMs = end - start;
        info.positionMs = std::clamp<int64_t>(position - start, 0, info.durationMs);
        info.reportedAt = winrt::clock::to_file_time(timeline.LastUpdatedTime()).value;
    } catch (...) {
    }
}

// Scales an image so its short side is `size`, then center-crops it square.
std::shared_ptr<const MediaArt> ToArt(IWICImagingFactory* wic, IWICBitmapSource* source, UINT size) {
    UINT w = 0, h = 0;
    if (FAILED(source->GetSize(&w, &h)) || w == 0 || h == 0) return nullptr;

    const double scale = static_cast<double>(size) / std::min(w, h);
    const UINT scaledW = std::max(size, static_cast<UINT>(std::lround(w * scale)));
    const UINT scaledH = std::max(size, static_cast<UINT>(std::lround(h * scale)));
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(source, scaledW, scaledH, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom))) {
        return nullptr;
    }

    auto art = std::make_shared<MediaArt>();
    art->size = size;
    art->pixels.resize(static_cast<size_t>(size) * size * 4);
    WICRect crop{static_cast<INT>((scaledW - size) / 2), static_cast<INT>((scaledH - size) / 2),
                 static_cast<INT>(size), static_cast<INT>(size)};
    if (FAILED(converter->CopyPixels(&crop, size * 4, static_cast<UINT>(art->pixels.size()), art->pixels.data()))) {
        return nullptr;
    }
    return art;
}

// Decodes the thumbnail, scaled so its short side is `size`, center-cropped square.
std::shared_ptr<const MediaArt> DecodeThumbnail(IWICImagingFactory* wic, const IRandomAccessStreamReference& ref,
                                                UINT size) {
    if (!wic || !ref || size == 0) return nullptr;
    try {
        auto stream = ref.OpenReadAsync().get();
        ComPtr<IStream> source;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(CreateStreamOverRandomAccessStream(winrt::get_unknown(stream), IID_PPV_ARGS(&source))) ||
            FAILED(wic->CreateDecoderFromStream(source.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame))) {
            return nullptr;
        }
        return ToArt(wic, frame.Get(), size);
    } catch (...) {
        return nullptr;
    }
}

// The app's own icon, as Windows shows it in the Start menu. Store and
// packaged apps (and browsers like Helium) report an AppUserModelID that the
// Apps folder knows; plain desktop apps report their exe name ("Spotify.exe"),
// so for those the running exe's icon is used.
std::shared_ptr<const MediaArt> LoadAppIcon(IWICImagingFactory* wic, const std::wstring& appId, UINT size) {
    if (!wic || appId.empty() || size == 0) return nullptr;
    const std::wstring parsingName = AppShellName(appId);
    ComPtr<IShellItemImageFactory> factory;
    HBITMAP icon = nullptr;
    if (parsingName.empty() ||
        FAILED(SHCreateItemFromParsingName(parsingName.c_str(), nullptr, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->GetImage(SIZE{static_cast<LONG>(size), static_cast<LONG>(size)},
                                 SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &icon))) {
        return nullptr;
    }
    ComPtr<IWICBitmap> bitmap;
    std::shared_ptr<const MediaArt> result;
    // The shell hands back straight (not premultiplied) alpha; ToArt converts.
    if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(icon, nullptr, WICBitmapUseAlpha, &bitmap))) {
        result = ToArt(wic, bitmap.Get(), size);
    }
    DeleteObject(icon);
    return result;
}

void RunCommand(const Session& session, MediaCommand command) {
    using Repeat = winrt::Windows::Media::MediaPlaybackAutoRepeatMode;
    try {
        switch (command) {
        case MediaCommand::Previous: session.TrySkipPreviousAsync().get(); break;
        case MediaCommand::PlayPause: session.TryTogglePlayPauseAsync().get(); break;
        case MediaCommand::Next: session.TrySkipNextAsync().get(); break;
        case MediaCommand::ShuffleOn: session.TryChangeShuffleActiveAsync(true).get(); break;
        case MediaCommand::ShuffleOff: session.TryChangeShuffleActiveAsync(false).get(); break;
        case MediaCommand::RepeatOff: session.TryChangeAutoRepeatModeAsync(Repeat::None).get(); break;
        case MediaCommand::RepeatOne: session.TryChangeAutoRepeatModeAsync(Repeat::Track).get(); break;
        case MediaCommand::RepeatAll: session.TryChangeAutoRepeatModeAsync(Repeat::List).get(); break;
        case MediaCommand::None: break;
        }
    } catch (...) {
        // The app closed or refused; its next update tells us the real state.
    }
}

} // namespace

int64_t MediaInfo::PositionAt(int64_t now) const {
    if (!hasTimeline) return 0;
    int64_t position = positionMs;
    // Extrapolate only from a sane report time; some apps send year 1601.
    const int64_t elapsedMs = (now - reportedAt) / 10000;
    if (playing && reportedAt > 0 && elapsedMs >= 0 && elapsedMs < 24LL * 3600 * 1000) {
        position += static_cast<int64_t>(elapsedMs * rate);
    }
    return std::clamp<int64_t>(position, 0, durationMs);
}

bool MediaWatcher::Start(HWND notifyWnd, UINT changedMsg) {
    notifyWnd_ = notifyWnd;
    changedMsg_ = changedMsg;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wakeEvent_ = CreateEventW(nullptr, FALSE, TRUE, nullptr); // signaled: evaluate once at start
    if (!stopEvent_ || !wakeEvent_) return false;
    thread_ = std::thread([this] { Run(); });
    return true;
}

void MediaWatcher::Stop() {
    if (thread_.joinable()) {
        SetEvent(stopEvent_);
        thread_.join();
    }
    if (stopEvent_) CloseHandle(stopEvent_);
    if (wakeEvent_) CloseHandle(wakeEvent_);
    stopEvent_ = wakeEvent_ = nullptr;
}

MediaInfo MediaWatcher::Latest() {
    std::lock_guard lock(mutex_);
    return latest_;
}

void MediaWatcher::Send(MediaCommand command) {
    pendingCommand_ = static_cast<int>(command);
    SetEvent(wakeEvent_);
}

void MediaWatcher::ShowSession(const std::wstring& appId) {
    {
        std::lock_guard lock(mutex_);
        pendingPick_ = appId;
    }
    needsFullEvaluation_ = true;
    SetEvent(wakeEvent_);
}

void MediaWatcher::SetFilter(AppFilter filter) {
    {
        std::lock_guard lock(mutex_);
        filter_ = std::move(filter);
    }
    needsFullEvaluation_ = true;
    SetEvent(wakeEvent_);
}

bool AppFilter::Ignores(const std::wstring& appId, const std::wstring& appName) const {
    for (const auto& entry : ignored) {
        if (_wcsicmp(entry.c_str(), appId.c_str()) == 0 || _wcsicmp(entry.c_str(), appName.c_str()) == 0) return true;
    }
    return false;
}

namespace {
bool AnyWithin(const std::vector<std::wstring>& parts, const std::wstring& appId, const std::wstring& appName) {
    auto contains = [](const std::wstring& text, const std::wstring& part) {
        return !part.empty() && std::search(text.begin(), text.end(), part.begin(), part.end(), [](wchar_t a, wchar_t b) {
                                    return towlower(a) == towlower(b);
                                }) != text.end();
    };
    for (const auto& part : parts) {
        if (contains(appId, part) || contains(appName, part)) return true;
    }
    return false;
}
} // namespace

bool AppFilter::IsMusicApp(const std::wstring& appId, const std::wstring& appName) const {
    return AnyWithin(musicApps, appId, appName);
}

bool AppFilter::IsBrowser(const std::wstring& appId, const std::wstring& appName) const {
    return AnyWithin(browsers, appId, appName);
}

void MediaWatcher::SwitchSession(int direction) {
    pendingSwitch_ += direction;
    needsFullEvaluation_ = true;
    SetEvent(wakeEvent_);
}

void MediaWatcher::Run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
        ComPtr<IWICImagingFactory> wic;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));

        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        // Every Windows callback just wakes this thread.
        auto wake = [this](auto&&, auto&&) {
            needsFullEvaluation_ = true;
            SetEvent(wakeEvent_);
        };
        auto sessionsChanged = manager.SessionsChanged(winrt::auto_revoke, wake);

        std::vector<Tracked> tracked;
        uint64_t playCounter = 0;
        std::wstring shownAppId;
        Session shown{nullptr};
        // Set by scrolling. Holds until it closes or anything starts playing after the pick.
        Session picked{nullptr};
        uint64_t pickedAt = 0;
        // App icons are looked up once per app (and size), then reused. Null = none found.
        std::map<std::pair<std::wstring, UINT>, std::shared_ptr<const MediaArt>> appIcons;
        std::map<std::wstring, std::wstring> appNames; // same: looked up once per app
        auto nameOf = [&](const std::wstring& appId) {
            auto name = appNames.find(appId);
            if (name == appNames.end()) name = appNames.emplace(appId, AppDisplayName(appId)).first;
            return name->second;
        };

        HANDLE waits[] = {stopEvent_, wakeEvent_};
        while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            // A click or scroll should feel instant; only plain updates wait for the burst to settle.
            if (pendingCommand_ == 0 && pendingSwitch_ == 0) {
                while (WaitForSingleObject(wakeEvent_, kCoalesceMs) == WAIT_OBJECT_0) {
                }
            }
            if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) break;
            ++evaluations_;

            if (auto command = static_cast<MediaCommand>(pendingCommand_.exchange(0));
                command != MediaCommand::None && shown) {
                RunCommand(shown, command); // the app's update event re-wakes us with the new state
                continue;
            }

            if (!needsFullEvaluation_.exchange(false) && shown) {
                ++timelineOnly_;
                MediaInfo info = Latest();
                info.hasTimeline = false;
                info.durationMs = info.positionMs = info.reportedAt = 0;
                ReadTimeline(shown, info);
                bool changed;
                {
                    std::lock_guard lock(mutex_);
                    changed = !(info == latest_);
                    latest_ = std::move(info);
                }
                if (changed) PostMessageW(notifyWnd_, changedMsg_, 0, 0);
                continue;
            }

            AppFilter filter;
            {
                std::lock_guard lock(mutex_);
                filter = filter_;
            }

            // Keep one subscription per live session; drop the ones that closed
            // and the ones the user chose to ignore.
            std::vector<Tracked> next;
            for (const auto& session : manager.GetSessions()) {
                auto it = std::find_if(tracked.begin(), tracked.end(),
                                       [&](const Tracked& t) { return t.session == session; });
                if (it != tracked.end()) {
                    if (filter.Ignores(it->appId, it->appName)) continue;
                    it->isMusic = filter.IsMusicApp(it->appId, it->appName);
                    it->isBrowser = filter.IsBrowser(it->appId, it->appName);
                    next.push_back(std::move(*it));
                    continue;
                }
                Tracked t;
                t.session = session;
                t.appId = session.SourceAppUserModelId();
                t.appName = nameOf(t.appId);
                if (filter.Ignores(t.appId, t.appName)) continue;
                t.isMusic = filter.IsMusicApp(t.appId, t.appName);
                t.isBrowser = filter.IsBrowser(t.appId, t.appName);
                t.propertiesChanged = session.MediaPropertiesChanged(
                    winrt::auto_revoke, [this, stale = t.propertiesStale](auto&&, auto&&) {
                        *stale = true;
                        needsFullEvaluation_ = true;
                        SetEvent(wakeEvent_);
                    });
                t.playbackChanged = session.PlaybackInfoChanged(winrt::auto_revoke, wake);
                t.timelineChanged = session.TimelinePropertiesChanged(
                    winrt::auto_revoke, [this, shownFlag = t.isShown](auto&&, auto&&) {
                        if (*shownFlag) SetEvent(wakeEvent_);
                    });
                next.push_back(std::move(t));
            }
            tracked = std::move(next);

            // Scrolling steps through sessions in a stable order, wrapping around.
            if (int steps = pendingSwitch_.exchange(0); steps != 0 && tracked.size() > 1) {
                auto it = std::find_if(tracked.begin(), tracked.end(),
                                       [&](const Tracked& t) { return shown && t.session == shown; });
                const int count = static_cast<int>(tracked.size());
                const int from = it == tracked.end() ? 0 : static_cast<int>(it - tracked.begin());
                picked = tracked[((from + steps) % count + count) % count].session;
                pickedAt = playCounter;
            }
            // Or a specific app chosen from the menu.
            std::wstring pickApp;
            {
                std::lock_guard lock(mutex_);
                pickApp = std::exchange(pendingPick_, std::wstring{});
            }
            for (const auto& t : tracked) {
                if (!pickApp.empty() && t.appId == pickApp) {
                    picked = t.session;
                    pickedAt = playCounter;
                }
            }

            // Among playing apps: a music app first (if preferred), then a
            // non-browser (if preferred), then whichever started playing last.
            Tracked* best = nullptr;
            Tracked* pick = nullptr;
            uint64_t newestStart = 0;
            auto rank = [&](const Tracked& t) {
                return std::make_tuple(filter.preferMusicApps && t.isMusic, filter.preferNonBrowsers && !t.isBrowser,
                                       t.playingSince);
            };
            for (auto& t : tracked) {
                bool wasPlaying = t.wasPlaying;
                ReadPlayback(t);
                if (t.wasPlaying && !wasPlaying) t.playingSince = ++playCounter;
                if (t.wasPlaying && (!best || rank(t) > rank(*best))) best = &t;
                if (t.wasPlaying) newestStart = std::max(newestStart, t.playingSince);
                if (picked && t.session == picked) pick = &t;
            }
            if (pick && newestStart <= pickedAt) {
                best = pick;
            } else {
                picked = nullptr;
            }
            // Nothing playing: keep showing what we showed, paused.
            for (auto& t : tracked) {
                if (!best && t.appId == shownAppId) best = &t;
            }
            if (!best && !tracked.empty()) {
                auto current = manager.GetCurrentSession();
                for (auto& t : tracked) {
                    if (current && t.session == current) best = &t;
                }
                if (!best) best = &tracked.front();
            }

            MediaInfo info;
            if (best) {
                if (best->propertiesStale->exchange(false)) {
                    try {
                        if (auto props = best->session.TryGetMediaPropertiesAsync().get()) {
                            best->title = props.Title();
                            best->artist = props.Artist();
                            best->art = DecodeThumbnail(wic.Get(), props.Thumbnail(), artSize_);
                        }
                    } catch (...) {
                        // The app closed mid-query; the next SessionsChanged sorts it out.
                    }
                }
                info.appId = best->appId;
                info.title = best->title;
                info.artist = best->artist;
                info.art = best->art;
                const UINT iconSize = std::max(12u, artSize_ * 7 / 16); // a badge, under half the art
                auto key = std::make_pair(best->appId, iconSize);
                auto icon = appIcons.find(key);
                if (icon == appIcons.end()) {
                    icon = appIcons.emplace(key, LoadAppIcon(wic.Get(), best->appId, iconSize)).first;
                }
                info.appIcon = icon->second;
                info.appName = best->appName;
                info.playing = best->wasPlaying;
                info.canPrevious = best->canPrevious;
                info.canPlayPause = best->canPlayPause;
                info.canNext = best->canNext;
                info.rate = best->rate;
                info.sessionCount = static_cast<int>(tracked.size());
                info.canShuffle = best->canShuffle;
                info.shuffle = best->shuffle;
                info.canRepeat = best->canRepeat;
                info.repeat = best->repeat;
                for (const auto& t : tracked) info.sessions.push_back({t.appId, t.appName, t.title, t.wasPlaying});
                ReadTimeline(best->session, info);
                shownAppId = best->appId;
                shown = best->session;
            } else {
                shownAppId.clear();
                shown = nullptr;
            }
            for (auto& t : tracked) *t.isShown = &t == best;

            bool changed;
            {
                std::lock_guard lock(mutex_);
                changed = !(info == latest_);
                latest_ = std::move(info);
            }
            if (changed) PostMessageW(notifyWnd_, changedMsg_, 0, 0);
        }
    } catch (const winrt::hresult_error&) {
        // Media sessions unavailable; the widget just shows nothing.
    }
    winrt::uninit_apartment();
}
