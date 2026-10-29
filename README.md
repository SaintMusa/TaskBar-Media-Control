# TaskBar Media Control

A small media widget that lives on the Windows 11 taskbar. It shows what is
playing (cover art, title, artist and a seekbar) and gives you previous,
play/pause and next buttons, without opening any window.

It works with any app that reports its media to Windows, such as Spotify,
browsers (YouTube, Twitch) and media players.

> AI was used in making this project.

## Features

- Cover art with the playing app's icon on its corner; click it to open the app
- Title and artist, dimmed while paused; long text scrolls back and forth
- Previous / play-pause / next buttons, greyed out when the app does not allow them
- Seekbar along the bottom edge in the cover art's color, hidden for live streams
- Shift + scroll to seek 5 seconds at a time
- Cross-fade when the track changes
- Switch between apps that are playing with the **›** button or the mouse wheel
- Ctrl + scroll changes the playing app's own volume
- Music apps take priority over browsers and video players (can be turned off)
- Pin an app to keep it on the widget while others play; switching away still works
- Hide apps you never want to see, or make browsers show only when nothing else plays
- Hides itself when nothing is playing, or after a while paused (or stays, if you prefer)
- Right-click menu with shuffle, repeat, switching and all options
- Optional start with Windows
- Follows the system light/dark theme
- Comes back by itself if Explorer restarts

## Requirements

- Windows 11, with the taskbar icons centered (the widget uses the empty
  space left of them)
- To build: Visual Studio 2026 with the "Desktop development with C++"
  workload (includes CMake, Ninja and the Windows SDK)

## Build

From a *Developer PowerShell for VS*:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The result is a single file, `build\TaskbarMedia.exe`, with no extra runtime
to install.

## Use

- Run `TaskbarMedia.exe`. The widget appears on the left side of the taskbar.
- Click the buttons to control playback, or the cover art to open the app.
- Scroll over the widget, or click **›**, to switch to another app that has media.
  One scroll gesture switches one app.
- Ctrl + scroll to change that app's volume, Shift + scroll to seek.
- Rest the mouse on the widget to see the full title.
- Right-click for shuffle, repeat, switching, hiding apps, **Start with
  Windows** and **Exit TaskbarMedia**.

## Settings

Settings are in `settings.ini` next to `TaskbarMedia.exe` (right-click >
**Open settings file**). If that folder can't be written to, for example in
Program Files, they go to `%APPDATA%\TaskbarMedia\settings.ini` instead. Every
setting has a comment, and changes apply as soon as you save the file.

| Setting | What it does |
|---|---|
| `StayVisibleWhenPaused` | Keep showing the last track while paused |
| `HideAfterPausedSeconds` | Otherwise, hide after this many seconds paused |
| `ArtColorSeekbar` | Color the seekbar with the cover art's main color |
| `Ignore` | Apps never to show, separated by `;` (name, exe or app ID) |
| `Pinned` | An app to show whenever it's open; switching away lasts until you switch back or the pinned app plays |
| `PreferMusicApps` / `MusicApps` | Show a playing music app over anything else |
| `PreferNonBrowsers` / `Browsers` | Show a browser only while no other app plays |
| `[Hotkeys]` | Optional keys that work from any app, e.g. `PlayPause=Ctrl+Alt+Up` (off by default) |

## How it works

The widget is a normal child window of the taskbar (`Shell_TrayWnd`). Nothing
is injected into Explorer and no system files are changed.

| File | What it does |
|---|---|
| `src/main.cpp` | The widget window, layout, drawing (Direct2D in software mode), menu and tooltip. A hidden window listens for Windows broadcasts such as Explorer restarts and theme changes. |
| `src/taskbar_watcher.*` | Finds the Start and Widgets buttons with UI Automation, on its own thread, to know where the free space is. |
| `src/media_watcher.*` | Follows the Windows media sessions (`GlobalSystemMediaTransportControls`) on one worker thread, picks which one to show, and decodes cover art and app icons. |
| `src/app_info.*` | The app behind a session: its name, exe and windows (to open it). |
| `src/settings.*` | Reading and writing `settings.ini`, and the Start with Windows entry. |
| `src/volume.*` | Changing one app's volume through the Windows audio session API. |

The app is event-driven: it only wakes up when Windows reports a change.
The only recurring timers are the seekbar, which redraws once each time the
bar grows by a pixel, and scrolling text, which only copies a pre-drawn strip
of pixels while the text is moving. Both run only while something is playing
and the widget is visible. When idle it uses no CPU.

## Known limitations

- Only the main monitor's taskbar
- Needs the taskbar icons centered; with left-aligned icons there is no room
  and the widget stays hidden
- Browsers report one media session for the whole browser, so with several
  tabs playing, only the one the browser picks is shown
- Some apps do not report a playback position, so no seekbar is shown for them

## Credits

These open-source projects were studied for ideas. No code was copied from them.

- [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor): attaching a window to the taskbar
- [Deskband11Lib](https://github.com/airtaxi/Deskband11Lib): finding free taskbar space with UI Automation
- [minibar](https://github.com/AceisX/minibar-now-playing-overlay): lightweight media session handling and drawing
- [Taskbar Fluent Media Player](https://github.com/Salyts/Taskbar-Fluent-Media-Player): the original inspiration
