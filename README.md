# TaskBar Media Control

A small media widget that lives on the Windows 11 taskbar. It shows what is
playing (cover art, title, artist and a seekbar) and gives you previous,
play/pause and next buttons, without opening any window.

It works with any app that reports its media to Windows, such as Spotify,
browsers (YouTube, Twitch) and media players.

> AI was used in making this project.

## Features

- Cover art with the playing app's icon on its corner
- Title and artist, dimmed while paused
- Previous / play-pause / next buttons, greyed out when the app does not allow them
- Seekbar along the bottom edge (display only), hidden for live streams
- Switch between apps that are playing with the **›** button or the mouse wheel
- Hides itself when nothing is playing, or after 30 seconds paused
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
- Click the buttons to control playback.
- Scroll over the widget, or click **›**, to switch to another app that has media.
- Right-click the widget and choose **Exit TaskbarMedia** to close it.

It does not start with Windows yet; run it again after a reboot.

## How it works

The widget is a normal child window of the taskbar (`Shell_TrayWnd`). Nothing
is injected into Explorer and no system files are changed.

| File | What it does |
|---|---|
| `src/main.cpp` | The widget window, layout and drawing (Direct2D in software mode). A hidden window listens for Windows broadcasts such as Explorer restarts and theme changes. |
| `src/taskbar_watcher.*` | Finds the Start and Widgets buttons with UI Automation, on its own thread, to know where the free space is. |
| `src/media_watcher.*` | Follows the Windows media sessions (`GlobalSystemMediaTransportControls`) on one worker thread, and decodes cover art and app icons. |

The app is event-driven: it only wakes up when Windows reports a change.
The one recurring timer is the seekbar, which redraws once each time the bar
grows by a pixel, and only while something is playing and the widget is
visible. When idle it uses no CPU.

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
