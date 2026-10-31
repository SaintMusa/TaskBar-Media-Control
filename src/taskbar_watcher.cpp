#include "taskbar_watcher.h"
#include <ole2.h> // WIN32_LEAN_AND_MEAN leaves out the COM headers UIA expects
#include <uiautomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <climits>
#include <cwchar>

using Microsoft::WRL::ComPtr;

// Receives UIA events on UIA's own threads and just pokes the UI thread.
class TaskbarWatcher::Handler final : public IUIAutomationPropertyChangedEventHandler,
                                      public IUIAutomationStructureChangedEventHandler {
public:
    Handler(HWND wnd, UINT msg) : wnd_(wnd), msg_(msg) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IUIAutomationPropertyChangedEventHandler)) {
            *out = static_cast<IUIAutomationPropertyChangedEventHandler*>(this);
        } else if (riid == __uuidof(IUIAutomationStructureChangedEventHandler)) {
            *out = static_cast<IUIAutomationStructureChangedEventHandler*>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }

    IFACEMETHODIMP HandlePropertyChangedEvent(IUIAutomationElement*, PROPERTYID, VARIANT) override {
        PostMessageW(wnd_, msg_, 0, 0);
        return S_OK;
    }
    IFACEMETHODIMP HandleStructureChangedEvent(IUIAutomationElement*, StructureChangeType, SAFEARRAY*) override {
        PostMessageW(wnd_, msg_, 0, 0);
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    HWND wnd_;
    UINT msg_;
};

namespace {

ComPtr<IUIAutomationElement> FindByAutomationId(IUIAutomation* uia, IUIAutomationElement* root, const wchar_t* id) {
    VARIANT value{};
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(id);
    ComPtr<IUIAutomationCondition> condition;
    ComPtr<IUIAutomationElement> found;
    if (SUCCEEDED(uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, value, &condition))) {
        root->FindFirst(TreeScope_Descendants, condition.Get(), &found);
    }
    VariantClear(&value);
    return found;
}

// Returns an empty rect when the element is gone, hidden or zero-sized.
RECT VisibleRect(IUIAutomationElement* element) {
    RECT rect{};
    BOOL offscreen = FALSE;
    if (!element || FAILED(element->get_CurrentBoundingRectangle(&rect)) ||
        FAILED(element->get_CurrentIsOffscreen(&offscreen)) || offscreen ||
        rect.right <= rect.left || rect.bottom <= rect.top) {
        return {};
    }
    return rect;
}

// Where the free space to the right of the icons ends: the system tray, or a
// window another app placed on the taskbar (TrafficMonitor does). Win32 child
// windows are checked as well as UIA because some of these have no UIA element.
LONG FirstObstacleRightOf(HWND taskbar, LONG from, const RECT& tray) {
    struct Search {
        LONG from;
        LONG nearest;
    } search{from, IsRectEmpty(&tray) ? LONG_MAX : tray.left};
    EnumChildWindows(
        taskbar,
        [](HWND child, LPARAM lp) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(lp);
            wchar_t name[64];
            // Our own widget; and the XAML host, which spans the whole taskbar.
            if (!GetClassNameW(child, name, ARRAYSIZE(name)) || wcscmp(name, L"TaskbarMedia.Widget") == 0 ||
                wcscmp(name, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0 ||
                !IsWindowVisible(child)) {
                return TRUE;
            }
            RECT rect;
            if (GetWindowRect(child, &rect) && rect.right > rect.left && rect.left >= s.from) {
                s.nearest = std::min(s.nearest, rect.left);
            }
            return TRUE; // keep looking: the nearest one counts
        },
        reinterpret_cast<LPARAM>(&search));
    return search.nearest;
}

} // namespace

bool TaskbarWatcher::Start(HWND notifyWnd, UINT geometryMsg, UINT changedMsg) {
    notifyWnd_ = notifyWnd;
    geometryMsg_ = geometryMsg;
    changedMsg_ = changedMsg;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    requestEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stopEvent_ || !requestEvent_) return false;
    thread_ = std::thread([this] { Run(); });
    return true;
}

void TaskbarWatcher::Stop() {
    if (thread_.joinable()) {
        SetEvent(stopEvent_);
        thread_.join();
    }
    if (stopEvent_) CloseHandle(stopEvent_);
    if (requestEvent_) CloseHandle(requestEvent_);
    stopEvent_ = requestEvent_ = nullptr;
}

void TaskbarWatcher::Refresh(HWND taskbar) {
    requested_ = taskbar;
    SetEvent(requestEvent_);
}

TaskbarGeometry TaskbarWatcher::Latest() {
    std::lock_guard lock(mutex_);
    return latest_;
}

void TaskbarWatcher::Run() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
    {
        ComPtr<IUIAutomation> uia;
        if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia)))) {
            CoUninitialize();
            return;
        }
        ComPtr<Handler> handler;
        handler.Attach(new Handler(notifyWnd_, changedMsg_));

        HWND subscribedTo = nullptr;
        ComPtr<IUIAutomationElement> start, widgets, repeater, tray;

        // Finds the elements once per taskbar and subscribes to the events that
        // signal a layout change. Re-runs only after Explorer restarts or an element dies.
        auto subscribe = [&](HWND taskbar) {
            uia->RemoveAllEventHandlers();
            start.Reset();
            widgets.Reset();
            repeater.Reset();
            tray.Reset();
            subscribedTo = nullptr;

            ComPtr<IUIAutomationElement> root;
            if (FAILED(uia->ElementFromHandle(taskbar, &root)) || !root) return;
            start = FindByAutomationId(uia.Get(), root.Get(), L"StartButton");
            widgets = FindByAutomationId(uia.Get(), root.Get(), L"WidgetsButton");
            // The group holding every icon. It's a layout container that UIA hides from
            // searches, so ask for Start's parent in the raw tree and check what we got.
            if (start) {
                ComPtr<IUIAutomationTreeWalker> raw;
                ComPtr<IUIAutomationElement> parent;
                BSTR id = nullptr;
                if (SUCCEEDED(uia->get_RawViewWalker(&raw)) && SUCCEEDED(raw->GetParentElement(start.Get(), &parent)) &&
                    parent && SUCCEEDED(parent->get_CurrentAutomationId(&id)) && id &&
                    wcscmp(id, L"TaskbarFrameRepeater") == 0) {
                    repeater = parent;
                }
                SysFreeString(id);
            }
            tray =FindByAutomationId(uia.Get(), root.Get(), L"SystemTrayIcon");           // the first tray button

            // A centered icon group shifts the Start button when apps open or close; a
            // left-aligned one keeps Start put and grows, so its own size is watched too.
            PROPERTYID bounds = UIA_BoundingRectanglePropertyId;
            for (IUIAutomationElement* watched : {start.Get(), repeater.Get()}) {
                if (watched) {
                    uia->AddPropertyChangedEventHandlerNativeArray(watched, TreeScope_Element, nullptr,
                                                                   handler.Get(), &bounds, 1);
                }
            }
            // Buttons being added or removed. Only the XAML frame: our own window
            // sits outside it, so moving ourselves can't trigger a feedback loop.
            if (auto frame = FindByAutomationId(uia.Get(), root.Get(), L"TaskbarFrame")) {
                uia->AddStructureChangedEventHandler(frame.Get(), TreeScope_Subtree, nullptr, handler.Get());
            }
            subscribedTo = taskbar;
        };

        HANDLE waits[] = {stopEvent_, requestEvent_};
        while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            HWND taskbar = requested_;
            if (!IsWindow(taskbar)) continue;

            if (taskbar != subscribedTo || !start) subscribe(taskbar);
            TaskbarGeometry geometry;
            geometry.taskbar = taskbar;
            GetWindowRect(taskbar, &geometry.taskbarRect);
            geometry.startButton = VisibleRect(start.Get());
            if (IsRectEmpty(&geometry.startButton) && start) {
                // Cached element went stale; look it up again once.
                subscribe(taskbar);
                geometry.startButton = VisibleRect(start.Get());
            }
            geometry.widgetsButton = VisibleRect(widgets.Get());
            geometry.icons = VisibleRect(repeater.Get());
            if (!IsRectEmpty(&geometry.icons)) {
                const RECT trayRect = VisibleRect(tray.Get());
                geometry.rightObstacleLeft = FirstObstacleRightOf(taskbar, geometry.icons.right, trayRect);
                // The Widgets button sits right of the icons in some layouts.
                if (geometry.widgetsButton.left >= geometry.icons.right) {
                    geometry.rightObstacleLeft = std::min(geometry.rightObstacleLeft, geometry.widgetsButton.left);
                }
            }

            {
                std::lock_guard lock(mutex_);
                latest_ = geometry;
            }
            PostMessageW(notifyWnd_, geometryMsg_, 0, 0);
        }
        uia->RemoveAllEventHandlers();
    }
    CoUninitialize();
}
