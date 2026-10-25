extern "C" {

// Constructor to run when library is loaded
__attribute__((constructor))
void on_library_load() {
    // Library constructors run before callers can initialize GTK/CEF or open a
    // Display, satisfying Xlib's ordering requirement for thread support.
    ensureXlibThreadSupport();
}

static bool cefBrowsersFinishedClosing() {
    if (g_pendingCefBrowserCreations.load() != 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_liveCefBrowsersMutex);
    return g_liveCefBrowsers.empty();
}

static gboolean drainCEFForShutdown(gpointer) {
    // OnBeforeClose removes the browser from g_liveCefBrowsers. Pending
    // CreateBrowser calls are also part of the barrier because OnAfterCreated
    // must get a chance to immediately close them during shutdown.
    if (!cefBrowsersFinishedClosing()) {
        return G_SOURCE_CONTINUE;
    }

    printf("[stopEventLoop] All CEF browsers closed\n");
    CefQuitMessageLoop();
    return G_SOURCE_REMOVE;
}

static void beginCEFShutdownOnMainThread() {
    std::vector<CefRefPtr<CefBrowser>> browsers;
    {
        std::lock_guard<std::mutex> lock(g_liveCefBrowsersMutex);
        browsers.reserve(g_liveCefBrowsers.size());
        for (const auto& [browser_id, browser] : g_liveCefBrowsers) {
            (void)browser_id;
            if (browser) {
                browsers.push_back(browser);
            }
        }
    }

    // Force-close is appropriate for application shutdown: a beforeunload
    // handler must not leave the native event-loop thread stuck indefinitely.
    // Never hold the registry mutex while invoking CEF callbacks.
    for (const auto& browser : browsers) {
        browser->GetHost()->CloseBrowser(true);
    }

    // CefRunMessageLoop owns the GLib context. Quit only after every successful
    // CreateBrowser request has resolved and every live browser has reached
    // OnBeforeClose.
    g_timeout_add(10, drainCEFForShutdown, nullptr);
}

// Auto-resize webviews in a specific window
void resizeAutoSizingWebviewsInWindow(uint32_t windowId, int width, int height) {
    OperationGuard guard;
    if (!guard.isValid()) return;

    // Find the X11 window handle for this window ID
    Window x11WindowHandle;
    {
        std::lock_guard<std::mutex> lock(g_x11WindowsMutex);
        auto windowIt = g_x11_windows.find(windowId);
        if (windowIt == g_x11_windows.end()) {
            return;
        }
        x11WindowHandle = windowIt->second->window;
    }
    
    // Find all auto-sized WGPU views that belong to this window.
    std::vector<std::shared_ptr<AbstractView>> fullSizeWebviews;
    {
        std::lock_guard<std::mutex> lock(g_wgpuViewMapMutex);
        for (auto& [webviewId, webview] : g_wgpuViewMap) {
            (void)webviewId;
            if (webview) {
                WGPUViewImpl* wgpuView = dynamic_cast<WGPUViewImpl*>(webview.get());
                // CEF has a separate geometry hook because pure parent moves
                // may change monitor scale without changing parent size.
                if (wgpuView && wgpuView->parentXWindow == x11WindowHandle &&
                    webview->fullSize) {
                    fullSizeWebviews.push_back(webview);
                }
            }
        }
    }
    
    // Process fullSize webviews - resize them to fill the window
    for (const auto& webview : fullSizeWebviews) {
        if (webview && webview->fullSize) {
            // Check if the webview is already the right size to avoid infinite resize loops
            GdkRectangle currentBounds = webview->visualBounds;
            if (currentBounds.width == width && currentBounds.height == height) {
                continue;
            }
            
            // For auto-resize, typically want to fill the entire window starting from (0,0)
            GdkRectangle frame = { 0, 0, width, height };
            webview->resize(frame, "");
        }
    }
}

void syncCEFViewsForParentGeometry(
    Window parentWindow,
    bool moved,
    bool resized,
    int width,
    int height
) {
    std::vector<std::shared_ptr<CEFWebViewImpl>> cefViews;
    {
        std::lock_guard<std::mutex> lock(g_webviewMapMutex);
        for (const auto& [webviewId, webview] : g_webviewMap) {
            (void)webviewId;
            auto cefView = std::dynamic_pointer_cast<CEFWebViewImpl>(webview);
            if (cefView && cefView->parentXWindow == parentWindow &&
                !cefView->isRemoved) {
                cefViews.push_back(std::move(cefView));
            }
        }
    }

    // Never call CEF/Xlib while holding the webview registry mutex.
    for (const auto& cefView : cefViews) {
        cefView->handleParentGeometryChanged(moved, resized, width, height);
    }
}

// X11 event processing function
gboolean process_x11_events(gpointer data) {
    OperationGuard guard;
    if (!guard.isValid()) {
        return G_SOURCE_REMOVE;
    }
    
    // Collect windows to process with proper synchronization
    std::vector<std::pair<uint32_t, std::shared_ptr<X11Window>>> windows_to_process;
    
    {
        std::lock_guard<std::mutex> lock(g_x11WindowsMutex);
        for (auto& [windowId, x11win] : g_x11_windows) {
            if (x11win && x11win->display) {
                windows_to_process.push_back({windowId, x11win});
            }
        }
    }
    
    // Process events for all X11 windows safely
    std::vector<uint32_t> windows_to_close;
    
    for (auto& [windowId, x11win] : windows_to_process) {
        if (!x11win || !x11win->display) continue;
        LinuxX11GeometryReducer geometryReducer({
            x11win->x,
            x11win->y,
            x11win->width,
            x11win->height,
        });
        
        // Check if we're still valid during processing
        if (g_shuttingDown.load()) {
            break;
        }
        
        while (XPending(x11win->display)) {
            XEvent event;
            XNextEvent(x11win->display, &event);

            std::shared_ptr<X11Window> childParentWindow;
            {
                std::lock_guard<std::mutex> lock(g_x11WindowsMutex);
                auto childIt = g_x11_child_window_to_parent_id.find(event.xany.window);
                if (childIt != g_x11_child_window_to_parent_id.end()) {
                    auto parentIt = g_x11_windows.find(childIt->second);
                    if (parentIt != g_x11_windows.end()) {
                        childParentWindow = parentIt->second;
                    }
                }
            }

            if (childParentWindow) {
                if (event.type == ButtonPress || event.type == FocusIn) {
                    focusX11Window(childParentWindow->display, childParentWindow->window);
                    if (childParentWindow->focusCallback) {
                        childParentWindow->focusCallback(childParentWindow->windowId);
                    }
                }
                continue;
            }
            
            // Validate window still exists in maps
            bool window_valid = false;
            {
                std::lock_guard<std::mutex> lock(g_x11WindowsMutex);
                auto it = g_x11_window_to_id.find(event.xany.window);
                if (it != g_x11_window_to_id.end() && it->second == windowId) {
                    auto winIt = g_x11_windows.find(windowId);
                    window_valid = (winIt != g_x11_windows.end() && winIt->second.get() == x11win.get());
                }
            }
            
            if (!window_valid) continue;
            
            // CRITICAL FIX: Only process events from actual main windows, not CEF child windows
            // CEF child windows should NEVER be in g_x11_window_to_id, but if they are, ignore them
            if (event.xany.window != x11win->window) {
                continue;
            }

            if (x11win->transparent) {
                CefRefPtr<ElectrobunClient> osrClient = getOSRClientForWindow(windowId);
                if (osrClient) {
                    forwardX11EventToOSRClient(event, x11win->display, x11win->window, osrClient);
                }
            }
            
            switch (event.type) {
                case ClientMessage:
                    if (event.xclient.data.l[0] == (long)XInternAtom(x11win->display, "WM_DELETE_WINDOW", False)) {
                        if (x11win->shouldCloseCallback) {
                            x11win->shouldCloseCallback(x11win->windowId);
                        } else {
                            if (x11win->closeCallback) {
                                x11win->closeCallback(x11win->windowId);
                            }

                            // Mark for safe cleanup after event processing
                            windows_to_close.push_back(windowId);
                        }
                    }
                    break;
                    
                case ConfigureNotify:
                    // Keep only the WM's latest parent geometry in this drain.
                    // Child ConfigureNotify events were filtered above.
                    geometryReducer.observe(
                        event.xconfigure.x,
                        event.xconfigure.y,
                        event.xconfigure.width,
                        event.xconfigure.height);
                    break;
                    
                case Expose:
                    // Handle expose events if needed
                    break;

                case FocusIn:
                    // Window received focus
                    if (x11win->focusCallback) {
                        x11win->focusCallback(x11win->windowId);
                    }
                    break;

                case FocusOut:
                    // Window received focus
                    if (x11win->blurCallback) {
                        x11win->blurCallback(x11win->windowId);
                   }
                   break;
                case KeyPress:
                case KeyRelease:
                    if (x11win->keyCallback) {
                        x11win->keyCallback(
                            x11win->windowId,
                            static_cast<uint32_t>(event.xkey.keycode),
                            modifiersFromX11State(event.xkey.state),
                            event.type == KeyPress ? 1u : 0u,
                            0u
                        );
                    }
                    break;
            }
        }

        const auto windowIsStillRegistered = [&]() {
            std::lock_guard<std::mutex> lock(g_x11WindowsMutex);
            auto current = g_x11_windows.find(windowId);
            return current != g_x11_windows.end() &&
                current->second.get() == x11win.get();
        };

        const bool closeQueued =
            std::find(windows_to_close.begin(), windows_to_close.end(), windowId) !=
            windows_to_close.end();
        const LinuxX11GeometryChange geometryChange = geometryReducer.result();
        if (geometryChange.hasConfigure && !closeQueued &&
            windowIsStillRegistered()) {
            const bool moved = geometryChange.moved;
            const bool resized = geometryChange.resized;

            x11win->x = geometryChange.geometry.x;
            x11win->y = geometryChange.geometry.y;
            x11win->width = geometryChange.geometry.width;
            x11win->height = geometryChange.geometry.height;
            x11win->settleRequestedGeometry(
                x11win->x, x11win->y, x11win->width, x11win->height);

            if (moved && x11win->moveCallback) {
                x11win->moveCallback(
                    x11win->windowId, x11win->x, x11win->y);
            }
            if (!windowIsStillRegistered()) continue;
            if (resized && x11win->resizeCallback) {
                x11win->resizeCallback(
                    x11win->windowId,
                    x11win->x,
                    x11win->y,
                    x11win->width,
                    x11win->height);
            }
            if (!windowIsStillRegistered()) continue;

            if (moved || resized) {
                // This is intentionally separate from public resize delivery:
                // a same-size monitor move can still change CEF's DPR.
                syncCEFViewsForParentGeometry(
                    x11win->window,
                    moved,
                    resized,
                    static_cast<int>(x11win->width),
                    static_cast<int>(x11win->height));
            }
            if (resized) {
                resizeAutoSizingWebviewsInWindow(
                    x11win->windowId,
                    static_cast<int>(x11win->width),
                    static_cast<int>(x11win->height));
            }
        }
    }
    
    // Safely clean up windows that requested closure
    for (uint32_t windowId : windows_to_close) {
        std::shared_ptr<X11Window> closingWindow;
        {
            std::lock_guard<std::mutex> lock(g_x11WindowsMutex);
            auto winIt = g_x11_windows.find(windowId);
            if (winIt != g_x11_windows.end()) {
                closingWindow = winIt->second;
                if (closingWindow) {
                    for (auto childIt = g_x11_child_window_to_parent_id.begin();
                         childIt != g_x11_child_window_to_parent_id.end();) {
                        if (childIt->second == windowId) {
                            childIt = g_x11_child_window_to_parent_id.erase(childIt);
                        } else {
                            ++childIt;
                        }
                    }
                    g_x11_window_to_id.erase(closingWindow->window);
                }
                g_x11_windows.erase(winIt);
            }
        }

        if (closingWindow && closingWindow->display && closingWindow->window) {
            // Disable/detach paint callbacks before invalidating their XID.
            // This function takes registry locks internally, so it must remain
            // outside g_x11WindowsMutex.
            removeCEFViewsForParentWindow(closingWindow->window);
            removeWGPUViewsForParentWindow(closingWindow->window);
            XDestroyWindow(closingWindow->display, closingWindow->window);
            XFlush(closingWindow->display);
        }
    }
    
    return G_SOURCE_CONTINUE;
}

void runCEFEventLoop() {
    // initializeCEF initializes GTK on this thread after disabling locale, and
    // Xlib thread support was already installed by the library constructor.
    ensureXlibThreadSupport();
    printf("=== ELECTROBUN NATIVE WRAPPER VERSION 1.0.2 === CEF EVENT LOOP STARTED ===\n");
    fflush(stdout);

    // Chromium's Linux message pump installs a GLib source that only blocks
    // correctly while CefRunMessageLoop owns it. Driving that source from
    // gtk_main plus a fixed CefDoMessageLoopWork timer leaves it perpetually
    // ready and spins an idle CPU core.
    if (!initializeCEF()) {
        fprintf(stderr, "CEF: initialization failed; running GTK loop for clean shutdown\n");
        initializeGTK();
        gtk_main();
        g_shutdownComplete.store(true);
        return;
    }
    
    // Set up X11 event processing
    g_timeout_add(10, process_x11_events, nullptr); // Process X11 events every 10ms

    CefRunMessageLoop();
    
    // Cleanup CEF on shutdown

    if (g_cefInitialized.exchange(false)) {
        CefShutdown();
    }
    g_shutdownComplete.store(true);
}

void runGTKEventLoop() {
    // Initialize GTK on the main thread (this MUST be done here)
    initializeGTK();
    printf("=== ELECTROBUN NATIVE WRAPPER VERSION 1.0.2 === GTK EVENT LOOP STARTED ===\n");

    gtk_main();
    g_shutdownComplete.store(true);
}

void runEventLoop() {    
    g_eventLoopThreadId.store(std::this_thread::get_id(), std::memory_order_release);
    if (isCEFAvailable()) {      
        runCEFEventLoop();
    } else {  
        runGTKEventLoop();
    }
}


// Forward declarations
void showWindow(void* window, bool activate);

void* createX11Window(uint32_t windowId, double x, double y, double width, double height, const char* title,
                   WindowCloseCallback closeCallback, WindowMoveCallback moveCallback, WindowResizeCallback resizeCallback, WindowFocusCallback focusCallback, WindowBlurCallback blurCallback, WindowKeyHandler keyCallback, WindowShouldCloseHandler shouldCloseCallback,
                   const char* titleBarStyle = nullptr, bool transparent = false) {
    
    void* result = dispatch_sync_main([&]() -> void* {
        
            // CEF mode - create pure X11 window
            
            // Create X11 window
            ensureXlibThreadSupport();
            Display* display = XOpenDisplay(nullptr);
            if (!display) {
                printf("ERROR: Failed to open X11 display\n");
                return nullptr;
            }
            
            int screen = DefaultScreen(display);
            Window root = RootWindow(display, screen);
            
            // Create window attributes
            XSetWindowAttributes attrs;
            attrs.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | 
                              ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                              FocusChangeMask | StructureNotifyMask | SubstructureNotifyMask |
                              EnterWindowMask | LeaveWindowMask;
            
            unsigned long attr_mask = CWEventMask;
            Visual* visual = DefaultVisual(display, screen);
            int depth = DefaultDepth(display, screen);
            
            // For transparent windows, use ARGB visual for true transparency
            if (transparent) {
                // Find ARGB visual for transparency  
                XVisualInfo vinfo;
                if (XMatchVisualInfo(display, screen, 32, TrueColor, &vinfo)) {
                    visual = vinfo.visual;
                    depth = vinfo.depth;
                    attrs.colormap = XCreateColormap(display, root, visual, AllocNone);
                    attr_mask |= CWColormap;
                    // Use transparent background pixel
                    attrs.background_pixel = 0x00000000;  // Fully transparent
                    attr_mask |= CWBackPixel;
                    attrs.border_pixel = 0;
                    attr_mask |= CWBorderPixel;
                    printf("X11: Created transparent window with 32-bit ARGB visual\n");
                } else {
                    printf("WARNING: 32-bit visual not available, using dark background fallback\n");
                    attrs.background_pixel = 0x101010;  // Very dark gray fallback
                    attrs.border_pixel = 0;
                    attrs.colormap = DefaultColormap(display, screen);
                    attr_mask |= CWBackPixel | CWBorderPixel | CWColormap;
                }
            } else {
                attrs.background_pixel = WhitePixel(display, screen);
                attrs.border_pixel = BlackPixel(display, screen);
                attrs.colormap = DefaultColormap(display, screen);
                attr_mask |= CWBackPixel | CWBorderPixel | CWColormap;
            }
            
            // Create the main window
            Window x11_window = XCreateWindow(
                display, root,
                (int)x, (int)y, (int)width, (int)height, 0,
                depth, InputOutput,
                visual,
                attr_mask,
                &attrs
            );
            
            // Window created successfully
            
            // Note: For Linux, transparent windows are handled as borderless windows
            
            if (!x11_window) {
                printf("ERROR: Failed to create X11 window\n");
                XCloseDisplay(display);
                return nullptr;
            }
            
            // Set window title
            XStoreName(display, x11_window, title);
            
            // Set WM_CLASS for proper taskbar icon matching
            XClassHint class_hint;
            class_hint.res_name = const_cast<char*>(g_electrobunWindowClass.c_str());
            class_hint.res_class = const_cast<char*>(g_electrobunWindowClass.c_str());
            XSetClassHint(display, x11_window, &class_hint);
            
            // Set window protocols for close button
            Atom wmDelete = XInternAtom(display, "WM_DELETE_WINDOW", False);
            XSetWMProtocols(display, x11_window, &wmDelete, 1);
            
            // Select input events for interaction
            long event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | 
                             ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                             FocusChangeMask | EnterWindowMask | LeaveWindowMask |
                             StructureNotifyMask;
            XSelectInput(display, x11_window, event_mask);
            
            // Handle window decorations based on titleBarStyle
            if (titleBarStyle && strcmp(titleBarStyle, "hidden") == 0) {
                // Remove window decorations for borderless windows
                Atom wmHints = XInternAtom(display, "_MOTIF_WM_HINTS", False);
                struct {
                    unsigned long flags;
                    unsigned long functions;
                    unsigned long decorations;
                    long inputMode;
                    unsigned long status;
                } hints = { 2, 0, 0, 0, 0 };  // MWM_HINTS_DECORATIONS = 2, no decorations
                
                XChangeProperty(display, x11_window, wmHints, wmHints, 32,
                               PropModeReplace, (unsigned char*)&hints, 5);
            }
            
            // Set window type for better compositor handling
            if (transparent || (titleBarStyle && strcmp(titleBarStyle, "hidden") == 0)) {
                Atom wmWindowType = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
                Atom wmWindowTypeNormal = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NORMAL", False);
                XChangeProperty(display, x11_window, wmWindowType, XA_ATOM, 32,
                               PropModeReplace, (unsigned char*)&wmWindowTypeNormal, 1);
            }
            
            // Set size and position hints to ensure window manager honors our positioning
            XSizeHints* sizeHints = XAllocSizeHints();
            if (sizeHints) {
                sizeHints->flags = PPosition | PSize;
                sizeHints->x = (int)x;
                sizeHints->y = (int)y;
                sizeHints->width = (int)width;
                sizeHints->height = (int)height;
                XSetWMNormalHints(display, x11_window, sizeHints);
                XFree(sizeHints);
            }
            
            // Create X11Window structure
            auto x11win = std::make_shared<X11Window>();
            x11win->display = display;
            x11win->window = x11_window;
            x11win->windowId = windowId;
            x11win->x = x;
            x11win->y = y;
            x11win->width = width;
            x11win->height = height;
            x11win->title = title;
            x11win->closeCallback = closeCallback;
            x11win->shouldCloseCallback = shouldCloseCallback;
            x11win->moveCallback = moveCallback;
            x11win->resizeCallback = resizeCallback;
            x11win->focusCallback = focusCallback;
            x11win->blurCallback = blurCallback;
            x11win->keyCallback = keyCallback;
            x11win->transparent = transparent;

            // Store in global maps
            {
                std::lock_guard<std::mutex> lock(g_x11WindowsMutex);
                g_x11_windows[windowId] = x11win;
                g_x11_window_to_id[x11_window] = windowId;
            }
            
            // X11/CEF mode doesn't need GTK containers - CEF manages its own windows
            // CEF webviews will be direct children of the X11 window
            
            // Apply application menu if one has been set
            applyApplicationMenuToX11Window(x11win.get());
            
            return (void*)x11win.get();
        
    });
    
    
    
    return result;
}

static bool isWaylandWindow(GtkWidget* window) {
#ifdef GDK_WINDOWING_WAYLAND
    return GDK_IS_WAYLAND_DISPLAY(gtk_widget_get_display(window));
#else
    (void)window;
    return false;
#endif
}

// Wayland tells a window it was minimized never: xdg-shell has no such state,
// and GTK3 never sets ICONIFIED there. Electrobun remembers the request until
// the window is restored or becomes active again.
static constexpr const char* kMinimizeRequestedKey = "electrobun-minimize-requested";

static void setMinimizeRequested(GtkWidget* window, bool requested) {
    g_object_set_data(G_OBJECT(window), kMinimizeRequestedKey, GINT_TO_POINTER(requested ? 1 : 0));
}

static bool minimizeRequested(GtkWidget* window) {
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(window), kMinimizeRequestedKey)) != 0;
}

// Focus reaches a window two ways: keyboard focus-in/out, and the compositor's
// activated state (GDK_WINDOW_STATE_FOCUSED), which on Wayland arrives even
// with no keyboard. Report each change once, whichever comes first.
static constexpr const char* kReportedFocusKey = "electrobun-reported-focus";

static void reportWindowFocus(ContainerView* container, bool focused) {
    if (!container || !container->window) return;
    GObject* object = G_OBJECT(container->window);
    // 0 = nothing reported yet, 1 = focused, 2 = blurred.
    const int previous = GPOINTER_TO_INT(g_object_get_data(object, kReportedFocusKey));
    const int next = focused ? 1 : 2;
    if (previous == next) return;
    g_object_set_data(object, kReportedFocusKey, GINT_TO_POINTER(next));
    if (focused) {
        setMinimizeRequested(container->window, false);
        if (container->focusCallback) container->focusCallback(container->windowId);
    } else if (container->blurCallback) {
        container->blurCallback(container->windowId);
    }
}

