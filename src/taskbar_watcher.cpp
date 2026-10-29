#include "taskbar_watcher.h"
#include <ole2.h> // WIN32_LEAN_AND_MEAN leaves out the COM headers UIA expects
#include <uiautomation.h>
#include <wrl/client.h>

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
        ComPtr<IUIAutomationElement> start, widgets;

        // Finds the elements once per taskbar and subscribes to the events that
        // signal a layout change. Re-runs only after Explorer restarts or an element dies.
        auto subscribe = [&](HWND taskbar) {
            uia->RemoveAllEventHandlers();
            start.Reset();
            widgets.Reset();
            subscribedTo = nullptr;

            ComPtr<IUIAutomationElement> root;
            if (FAILED(uia->ElementFromHandle(taskbar, &root)) || !root) return;
            start = FindByAutomationId(uia.Get(), root.Get(), L"StartButton");
            widgets = FindByAutomationId(uia.Get(), root.Get(), L"WidgetsButton");

            // The centered icon group shifts the Start button when apps open or close.
            if (start) {
                PROPERTYID bounds = UIA_BoundingRectanglePropertyId;
                uia->AddPropertyChangedEventHandlerNativeArray(start.Get(), TreeScope_Element, nullptr,
                                                               handler.Get(), &bounds, 1);
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
