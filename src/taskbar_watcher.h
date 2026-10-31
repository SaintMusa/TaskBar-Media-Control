#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <thread>

// Positions of the taskbar parts we lay out around, in physical screen pixels.
// An empty rect means that part wasn't found or isn't visible.
struct TaskbarGeometry {
    HWND taskbar = nullptr;
    RECT taskbarRect{};
    RECT startButton{};
    RECT widgetsButton{};
    RECT icons{};              // the whole strip of taskbar buttons, Start included
    LONG rightObstacleLeft = LONG_MAX; // left edge of the first thing right of the icons (tray, another app's window)
};

// Reads taskbar button positions with UI Automation on its own thread, and
// listens for UIA events that mean the taskbar layout changed.
//
// UIA runs off the UI thread on purpose: our widget window lives inside the
// taskbar tree that UIA walks, so querying from the thread that owns it would
// stall on our own window.
class TaskbarWatcher {
public:
    // geometryMsg is posted to notifyWnd when Latest() has new data;
    // changedMsg is posted when a UIA event says the layout moved.
    bool Start(HWND notifyWnd, UINT geometryMsg, UINT changedMsg);
    void Stop();

    // Asks the worker to (re)read the geometry of this taskbar. Cheap; coalesces.
    void Refresh(HWND taskbar);
    TaskbarGeometry Latest();
    DWORD ThreadId() { return GetThreadId(thread_.native_handle()); }

private:
    class Handler;
    void Run();

    HWND notifyWnd_ = nullptr;
    UINT geometryMsg_ = 0;
    UINT changedMsg_ = 0;
    HANDLE stopEvent_ = nullptr;
    HANDLE requestEvent_ = nullptr;
    std::atomic<HWND> requested_{nullptr};
    std::mutex mutex_;
    TaskbarGeometry latest_;
    std::thread thread_;
};
