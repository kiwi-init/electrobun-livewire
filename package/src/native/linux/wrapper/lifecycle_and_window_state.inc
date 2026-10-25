// Forward declaration - stopEventLoop is defined after startEventLoop
ELECTROBUN_EXPORT void stopEventLoop();

ELECTROBUN_EXPORT void startEventLoop(const char* identifier, const char* name, const char* channel) {
    // Store app identity before any native windows or renderer contexts are made.
    if (identifier && identifier[0]) {
        g_electrobunIdentifier = std::string(identifier);
    }
    if (name && name[0]) {
        g_electrobunName = std::string(name);
    }
    if (channel && channel[0]) {
        g_electrobunChannel = std::string(channel);
    }
    g_electrobunWindowClass = deriveLinuxWindowClass(
        g_electrobunName,
        g_electrobunChannel);

    // Linux uses runEventLoop instead
    runEventLoop();
}

ELECTROBUN_EXPORT void stopEventLoop() {
    if (g_eventLoopStopping.exchange(true)) {
        return;
    }
    g_shuttingDown.store(true);
    printf("[stopEventLoop] Initiating clean event loop exit\n");

    runOnMainThreadAsyncVoid([]() {
        wayland_screen_capture::shutdown();
        if (g_cefInitialized.load()) {
            beginCEFShutdownOnMainThread();
        } else {
            gtk_main_quit();
        }
    });
}

ELECTROBUN_EXPORT void killApp() {
    // Deprecated - delegates to stopEventLoop for backward compatibility
    stopEventLoop();
}

ELECTROBUN_EXPORT void waitForShutdownComplete(int timeoutMs) {
    int waited = 0;
    while (!g_shutdownComplete.load() && waited < timeoutMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waited += 10;
    }
}

ELECTROBUN_EXPORT void forceExit(int code) {
    _exit(code);
}

// C signal handler for SIGINT/SIGTERM - writes to self-pipe (async-signal-safe)
static void linux_signal_writer(int sig) {
    int saved_errno = errno;
    write(g_signal_pipe[1], &sig, sizeof(sig));
    errno = saved_errno;
}

// GLib IO watch callback - reads from self-pipe and dispatches quit logic
static gboolean linux_signal_pipe_read(GIOChannel* source, GIOCondition condition, gpointer data) {
    int sig;
    if (read(g_signal_pipe[0], &sig, sizeof(sig)) != sizeof(sig)) {
        return G_SOURCE_CONTINUE;
    }

    if (sig == SIGINT) {
        g_sigint_count++;
        if (g_sigint_count == 1) {
            if (g_quitRequestedHandler && !g_eventLoopStopping.load()) {
                g_quitRequestedHandler();
            } else {
                stopEventLoop();
            }
        } else {
            // Second Ctrl+C: force kill entire process group
            kill(0, SIGKILL);
        }
    } else if (sig == SIGTERM) {
        if (g_quitRequestedHandler && !g_eventLoopStopping.load()) {
            g_quitRequestedHandler();
        } else {
            stopEventLoop();
        }
    }

    return G_SOURCE_CONTINUE;
}

ELECTROBUN_EXPORT void setQuitRequestedHandler(QuitRequestedHandler handler) {
    g_quitRequestedHandler = handler;

    // Set up signal handling via self-pipe + GLib IO watch.
    // This MUST be done here (not in startEventLoop) because bun's Worker sets up
    // its own SIGINT handler via process.on("SIGINT") before calling this function.
    // By installing our sigaction handler here, we override bun's handler.
    // bun's handler can't forward signals to the Worker when the main thread is
    // blocked in gtk_main(), so we handle signals ourselves.
    if (g_signal_pipe[0] == -1) {
        if (pipe(g_signal_pipe) == 0) {
            fcntl(g_signal_pipe[0], F_SETFL, O_NONBLOCK);
            fcntl(g_signal_pipe[1], F_SETFL, O_NONBLOCK);

            GIOChannel* channel = g_io_channel_unix_new(g_signal_pipe[0]);
            g_io_add_watch(channel, G_IO_IN, linux_signal_pipe_read, nullptr);
            g_io_channel_unref(channel);
        }
    }

    // Install our signal handlers, overriding bun's
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = linux_signal_writer;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

ELECTROBUN_EXPORT void shutdownApplication() {
    // Deprecated - use stopEventLoop() instead
    stopEventLoop();
}

void* createNSRectWrapper(double x, double y, double width, double height) {
    // TODO: Return appropriate rectangle structure
    return nullptr;
}


// Helper function to clean up webviews when a window is closed
void cleanupWebviewsForWindow(uint32_t windowId) {
    // Check if we're shutting down to avoid cleanup races
    if (g_shuttingDown.load()) {
        return;
    }
    
    // Prevent double cleanup for the same window
    static std::set<uint32_t> s_cleaningWindows;
    static std::mutex s_cleanupMutex;
    
    {
        std::lock_guard<std::mutex> cleanup_lock(s_cleanupMutex);
        if (s_cleaningWindows.count(windowId) > 0) {
            return;
        }
        s_cleaningWindows.insert(windowId);
    }
    
    // Find and remove the container
    std::shared_ptr<ContainerView> container;
    {
        std::lock_guard<std::mutex> lock(g_containersMutex);
        auto it = g_containers.find(windowId);
        if (it != g_containers.end()) {
            container = it->second;
            g_containers.erase(it);
        }
    }
    
    if (container) {
        // Clean up all webviews in this container
        for (auto& webview : container->abstractViews) {
            if (webview) {
                g_pendingResizeQueue.remove(webview.get());
            }
        }

        dispatch_sync_main_void([container]() {
            for (auto& webview : container->abstractViews) {
                if (auto* webKitView = dynamic_cast<WebKitWebViewImpl*>(webview.get())) {
                    webKitView->releasePartitionContextIfNeeded();
                }
            }
        });

        {
            std::lock_guard<std::mutex> lock(g_webviewMapMutex);
            for (auto& webview : container->abstractViews) {
                if (webview && !dynamic_cast<WGPUViewImpl*>(webview.get())) {
                    g_webviewMap.erase(webview->webviewId);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_wgpuViewMapMutex);
            for (auto& webview : container->abstractViews) {
                if (webview && dynamic_cast<WGPUViewImpl*>(webview.get())) {
                    auto it = g_wgpuViewMap.find(webview->webviewId);
                    if (it != g_wgpuViewMap.end() &&
                        it->second.get() == webview.get()) {
                        g_wgpuViewMap.erase(it);
                    }
                }
            }
        }
    }
    
    // Mark cleanup as complete
    {
        std::lock_guard<std::mutex> cleanup_lock(s_cleanupMutex);
        s_cleaningWindows.erase(windowId);
    }
}

ELECTROBUN_EXPORT void closeWindow(void* window) {
    if (!window) return;
    
    // Check if we're shutting down
    if (g_shuttingDown.load()) {
        return;
    }
    
    // Prevent double-close for the same window pointer
    static std::set<void*> s_closingWindows;
    static std::mutex s_closeWindowMutex;
    
    {
        std::lock_guard<std::mutex> close_lock(s_closeWindowMutex);
        if (s_closingWindows.count(window) > 0) {
            return;
        }
        s_closingWindows.insert(window);
    }
    
    dispatch_sync_main_void([&]() {
        // Check if it's a GTK window first
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            
            // Find the container for this window to get the windowId and callback.
            // Hold a shared_ptr so the ContainerView stays alive through gtk_widget_destroy
            // (the "destroy" signal handler accesses it via raw pointer).
            uint32_t windowId = 0;
            WindowCloseCallback closeCallback = nullptr;
            std::shared_ptr<ContainerView> containerRef;
            {
                std::lock_guard<std::mutex> lock(g_containersMutex);
                for (auto& [id, container] : g_containers) {
                    if (container->window == gtkWindow) {
                        windowId = id;
                        closeCallback = container->closeCallback;
                        containerRef = container; // keep alive
                        break;
                    }
                }
            }
            
            // Call the close callback before destroying the widget.
            if (closeCallback && windowId > 0) {
                closeCallback(windowId);
            }
            
            // gtk_widget_destroy fires the "destroy" signal synchronously.
            // containerRef keeps the ContainerView alive so the signal handler's
            // raw ContainerView* user_data is valid for the duration of the call.
            gtk_widget_destroy(gtkWindow);
        } else {
            // It's an X11 window
            X11Window* x11win = static_cast<X11Window*>(window);
            
            // Validate the X11 window pointer and check if it's still in our maps
            bool window_valid = false;
            uint32_t windowId = 0;
            {
                std::lock_guard<std::mutex> lock(g_x11WindowsMutex);
                for (auto& [id, win] : g_x11_windows) {
                    if (win.get() == x11win && x11win->display && x11win->window) {
                        window_valid = true;
                        windowId = id;
                        break;
                    }
                }
            }
            
            if (window_valid) {
                // Store callback and window info before any cleanup
                auto callback = x11win->closeCallback;
                auto display = x11win->display;
                auto x11_window = x11win->window;
                
                // Call the close callback BEFORE removing from maps.
                if (callback) {
                    callback(windowId);
                }

                // Shared-core callers normally remove child views from their
                // close callback. Also cover native adapters and abnormal window
                // teardown before destroying the X11 parent.
                removeCEFViewsForParentWindow(x11_window);
                removeWGPUViewsForParentWindow(x11_window);
                
                // Remove the X11 window from global maps.
                {
                    std::lock_guard<std::mutex> lock(g_x11WindowsMutex);
                    for (auto it = g_x11_child_window_to_parent_id.begin(); it != g_x11_child_window_to_parent_id.end();) {
                        if (it->second == windowId) {
                            it = g_x11_child_window_to_parent_id.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    g_x11_window_to_id.erase(x11_window);
                    g_x11_windows.erase(windowId);
                }
                
                XDestroyWindow(display, x11_window);
                XFlush(display);

                // Note: Don't close display here as it might be shared
            }
        }
    });
    
    // Mark close as complete
    {
        std::lock_guard<std::mutex> close_lock(s_closeWindowMutex);
        s_closingWindows.erase(window);
    }
}

ELECTROBUN_EXPORT void requestWindowClose(void* window) {
    if (!window || g_shuttingDown.load()) return;

    dispatch_sync_main_void([&]() {
        if (GTK_IS_WIDGET(window)) {
            gtk_window_close(GTK_WINDOW(window));
            return;
        }

        X11Window* x11win = static_cast<X11Window*>(window);
        if (!x11win->display || !x11win->window) return;

        Atom wmProtocols = XInternAtom(x11win->display, "WM_PROTOCOLS", False);
        Atom wmDelete = XInternAtom(x11win->display, "WM_DELETE_WINDOW", False);
        XEvent event = {};
        event.xclient.type = ClientMessage;
        event.xclient.window = x11win->window;
        event.xclient.message_type = wmProtocols;
        event.xclient.format = 32;
        event.xclient.data.l[0] = wmDelete;
        event.xclient.data.l[1] = CurrentTime;
        XSendEvent(x11win->display, x11win->window, False, NoEventMask, &event);
        XFlush(x11win->display);
    });
}

ELECTROBUN_EXPORT void minimizeWindow(void* window) {
    if (!window) return;

    dispatch_sync_main_void([&]() {
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (GTK_IS_WINDOW(gtkWindow)) {
                gtk_window_iconify(GTK_WINDOW(gtkWindow));
                if (isWaylandWindow(gtkWindow)) {
                    setMinimizeRequested(gtkWindow, true);
                }
            }
        } else {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                XIconifyWindow(x11win->display, x11win->window, DefaultScreen(x11win->display));
                XFlush(x11win->display);
            }
        }
    });
}

ELECTROBUN_EXPORT void restoreWindow(void* window) {
    if (!window) return;

    dispatch_sync_main_void([&]() {
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (GTK_IS_WINDOW(gtkWindow)) {
                gtk_window_deiconify(GTK_WINDOW(gtkWindow));
                gtk_window_present(GTK_WINDOW(gtkWindow));
                setMinimizeRequested(gtkWindow, false);
            }
        } else {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                // First, map the window
                XMapWindow(x11win->display, x11win->window);
                
                // Send a client message to change the WM_STATE from IconicState to NormalState
                XEvent event;
                memset(&event, 0, sizeof(event));
                event.type = ClientMessage;
                event.xclient.window = x11win->window;
                event.xclient.message_type = XInternAtom(x11win->display, "WM_CHANGE_STATE", False);
                event.xclient.format = 32;
                event.xclient.data.l[0] = 1; // NormalState
                
                XSendEvent(x11win->display, DefaultRootWindow(x11win->display), False,
                          SubstructureNotifyMask | SubstructureRedirectMask, &event);
                
                // Also use _NET_WM_STATE to ensure the window is not minimized
                Atom wmState = XInternAtom(x11win->display, "_NET_WM_STATE", False);
                Atom wmStateHidden = XInternAtom(x11win->display, "_NET_WM_STATE_HIDDEN", False);
                
                XEvent xev;
                memset(&xev, 0, sizeof(xev));
                xev.type = ClientMessage;
                xev.xclient.window = x11win->window;
                xev.xclient.message_type = wmState;
                xev.xclient.format = 32;
                xev.xclient.data.l[0] = 0; // _NET_WM_STATE_REMOVE
                xev.xclient.data.l[1] = wmStateHidden;
                xev.xclient.data.l[2] = 0;
                
                XSendEvent(x11win->display, DefaultRootWindow(x11win->display), False,
                          SubstructureRedirectMask | SubstructureNotifyMask, &xev);

                // _NET_WM_STATE_HIDDEN is WM-owned (EWMH), so the removal
                // above is only advisory; EWMH WMs (Mutter included)
                // de-iconify on _NET_ACTIVE_WINDOW. Source indication 2
                // (pager) bypasses focus-stealing prevention for this
                // explicit restore.
                XEvent activate;
                memset(&activate, 0, sizeof(activate));
                activate.type = ClientMessage;
                activate.xclient.window = x11win->window;
                activate.xclient.message_type =
                    XInternAtom(x11win->display, "_NET_ACTIVE_WINDOW", False);
                activate.xclient.format = 32;
                activate.xclient.data.l[0] = 2;
                activate.xclient.data.l[1] = CurrentTime;
                activate.xclient.data.l[2] = 0;
                XSendEvent(x11win->display, DefaultRootWindow(x11win->display), False,
                          SubstructureRedirectMask | SubstructureNotifyMask, &activate);

                XFlush(x11win->display);
            }
        }
    });
}

ELECTROBUN_EXPORT bool isWindowMinimized(void* window) {
    if (!window) return false;

    bool result = false;
    dispatch_sync_main_void([&]() {
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (GTK_IS_WINDOW(gtkWindow)) {
                GdkWindow* gdkWindow = gtk_widget_get_window(gtkWindow);
                if (gdkWindow) {
                    GdkWindowState state = gdk_window_get_state(gdkWindow);
                    result = (state & GDK_WINDOW_STATE_ICONIFIED) != 0;
                }
                if (!result && isWaylandWindow(gtkWindow)) {
                    result = minimizeRequested(gtkWindow);
                }
            }
        } else {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                Atom wmState = XInternAtom(x11win->display, "WM_STATE", True);
                if (wmState != None) {
                    Atom actualType;
                    int actualFormat;
                    unsigned long nItems, bytesAfter;
                    unsigned char* propData = nullptr;

                    if (XGetWindowProperty(x11win->display, x11win->window, wmState,
                            0, 2, False, wmState, &actualType, &actualFormat,
                            &nItems, &bytesAfter, &propData) == Success && propData) {
                        // WM_STATE first element: WithdrawnState=0, NormalState=1, IconicState=3
                        if (nItems > 0) {
                            result = (propData[0] == 3); // IconicState
                        }
                        XFree(propData);
                    }
                    
                    // Also check _NET_WM_STATE_HIDDEN as a fallback
                    if (!result) {
                        Atom netWmState = XInternAtom(x11win->display, "_NET_WM_STATE", False);
                        Atom netWmStateHidden = XInternAtom(x11win->display, "_NET_WM_STATE_HIDDEN", False);
                        
                        if (netWmState != None) {
                            Atom actualType2;
                            int actualFormat2;
                            unsigned long nItems2, bytesAfter2;
                            unsigned char* propData2 = nullptr;
                            
                            if (XGetWindowProperty(x11win->display, x11win->window, netWmState,
                                    0, 1024, False, XA_ATOM, &actualType2, &actualFormat2,
                                    &nItems2, &bytesAfter2, &propData2) == Success && propData2) {
                                Atom* atoms = (Atom*)propData2;
                                for (unsigned long i = 0; i < nItems2; i++) {
                                    if (atoms[i] == netWmStateHidden) {
                                        result = true;
                                        break;
                                    }
                                }
                                XFree(propData2);
                            }
                        }
                    }
                }
            }
        }
    });
    return result;
}

ELECTROBUN_EXPORT void maximizeWindow(void* window) {
    if (!window) return;

    dispatch_sync_main_void([&]() {
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (GTK_IS_WINDOW(gtkWindow)) {
                gtk_window_maximize(GTK_WINDOW(gtkWindow));
            }
        } else {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                Atom wmState = XInternAtom(x11win->display, "_NET_WM_STATE", False);
                Atom maxH = XInternAtom(x11win->display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
                Atom maxV = XInternAtom(x11win->display, "_NET_WM_STATE_MAXIMIZED_VERT", False);

                XEvent xev = {};
                xev.type = ClientMessage;
                xev.xclient.window = x11win->window;
                xev.xclient.message_type = wmState;
                xev.xclient.format = 32;
                xev.xclient.data.l[0] = 1; // _NET_WM_STATE_ADD
                xev.xclient.data.l[1] = maxH;
                xev.xclient.data.l[2] = maxV;
                xev.xclient.data.l[3] = 0;

                XSendEvent(x11win->display, DefaultRootWindow(x11win->display), False,
                    SubstructureRedirectMask | SubstructureNotifyMask, &xev);
                XFlush(x11win->display);
            }
        }
    });
}

ELECTROBUN_EXPORT void unmaximizeWindow(void* window) {
    if (!window) return;

    dispatch_sync_main_void([&]() {
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (GTK_IS_WINDOW(gtkWindow)) {
                gtk_window_unmaximize(GTK_WINDOW(gtkWindow));
            }
        } else {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                Atom wmState = XInternAtom(x11win->display, "_NET_WM_STATE", False);
                Atom maxH = XInternAtom(x11win->display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
                Atom maxV = XInternAtom(x11win->display, "_NET_WM_STATE_MAXIMIZED_VERT", False);

                XEvent xev = {};
                xev.type = ClientMessage;
                xev.xclient.window = x11win->window;
                xev.xclient.message_type = wmState;
                xev.xclient.format = 32;
                xev.xclient.data.l[0] = 0; // _NET_WM_STATE_REMOVE
                xev.xclient.data.l[1] = maxH;
                xev.xclient.data.l[2] = maxV;
                xev.xclient.data.l[3] = 0;

                XSendEvent(x11win->display, DefaultRootWindow(x11win->display), False,
                    SubstructureRedirectMask | SubstructureNotifyMask, &xev);
                XFlush(x11win->display);
            }
        }
    });
}

ELECTROBUN_EXPORT bool isWindowMaximized(void* window) {
    if (!window) return false;

    bool result = false;
    dispatch_sync_main_void([&]() {
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (GTK_IS_WINDOW(gtkWindow)) {
                result = gtk_window_is_maximized(GTK_WINDOW(gtkWindow));
            }
        } else {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                Atom wmState = XInternAtom(x11win->display, "_NET_WM_STATE", False);
                Atom maxH = XInternAtom(x11win->display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
                Atom maxV = XInternAtom(x11win->display, "_NET_WM_STATE_MAXIMIZED_VERT", False);

                Atom actualType;
                int actualFormat;
                unsigned long nItems, bytesAfter;
                unsigned char* propData = nullptr;

                if (XGetWindowProperty(x11win->display, x11win->window, wmState,
                        0, (~0L), False, XA_ATOM, &actualType, &actualFormat,
                        &nItems, &bytesAfter, &propData) == Success && propData) {
                    Atom* atoms = reinterpret_cast<Atom*>(propData);
                    bool hasMaxH = false, hasMaxV = false;
                    for (unsigned long i = 0; i < nItems; i++) {
                        if (atoms[i] == maxH) hasMaxH = true;
                        if (atoms[i] == maxV) hasMaxV = true;
                    }
                    result = hasMaxH && hasMaxV;
                    XFree(propData);
                }
            }
        }
    });
    return result;
}

ELECTROBUN_EXPORT void setWindowFullScreen(void* window, bool fullScreen) {
    if (!window) return;

    dispatch_sync_main_void([&]() {
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (GTK_IS_WINDOW(gtkWindow)) {
                if (fullScreen) {
                    gtk_window_fullscreen(GTK_WINDOW(gtkWindow));
                } else {
                    gtk_window_unfullscreen(GTK_WINDOW(gtkWindow));
                }
            }
        } else {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                Atom wmState = XInternAtom(x11win->display, "_NET_WM_STATE", False);
                Atom fullscreenAtom = XInternAtom(x11win->display, "_NET_WM_STATE_FULLSCREEN", False);

                XEvent xev = {};
                xev.type = ClientMessage;
                xev.xclient.window = x11win->window;
                xev.xclient.message_type = wmState;
                xev.xclient.format = 32;
                xev.xclient.data.l[0] = fullScreen ? 1 : 0; // _NET_WM_STATE_ADD or REMOVE
                xev.xclient.data.l[1] = fullscreenAtom;
                xev.xclient.data.l[2] = 0;
                xev.xclient.data.l[3] = 0;

                XSendEvent(x11win->display, DefaultRootWindow(x11win->display), False,
                    SubstructureRedirectMask | SubstructureNotifyMask, &xev);
                XFlush(x11win->display);
            }
        }
    });
}

ELECTROBUN_EXPORT bool isWindowFullScreen(void* window) {
    if (!window) return false;

    bool result = false;
    dispatch_sync_main_void([&]() {
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (GTK_IS_WINDOW(gtkWindow)) {
                GdkWindow* gdkWindow = gtk_widget_get_window(gtkWindow);
                if (gdkWindow) {
                    GdkWindowState state = gdk_window_get_state(gdkWindow);
                    result = (state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
                }
            }
        } else {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                Atom wmState = XInternAtom(x11win->display, "_NET_WM_STATE", False);
                Atom fullscreenAtom = XInternAtom(x11win->display, "_NET_WM_STATE_FULLSCREEN", False);

                Atom actualType;
                int actualFormat;
                unsigned long nItems, bytesAfter;
                unsigned char* propData = nullptr;

                if (XGetWindowProperty(x11win->display, x11win->window, wmState,
                        0, (~0L), False, XA_ATOM, &actualType, &actualFormat,
                        &nItems, &bytesAfter, &propData) == Success && propData) {
                    Atom* atoms = reinterpret_cast<Atom*>(propData);
                    for (unsigned long i = 0; i < nItems; i++) {
                        if (atoms[i] == fullscreenAtom) {
                            result = true;
                            break;
                        }
                    }
                    XFree(propData);
                }
            }
        }
    });
    return result;
}

ELECTROBUN_EXPORT void setWindowAlwaysOnTop(void* window, bool alwaysOnTop) {
    if (!window) return;

    dispatch_sync_main_void([&]() {
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (GTK_IS_WINDOW(gtkWindow)) {
                gtk_window_set_keep_above(GTK_WINDOW(gtkWindow), alwaysOnTop ? TRUE : FALSE);
                // Focus the window when setting always on top to ensure visibility
                if (alwaysOnTop) {
                    gtk_window_present(GTK_WINDOW(gtkWindow));
                }
            }
        } else {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                Atom wmState = XInternAtom(x11win->display, "_NET_WM_STATE", False);
                Atom aboveAtom = XInternAtom(x11win->display, "_NET_WM_STATE_ABOVE", False);

                XEvent xev = {};
                xev.type = ClientMessage;
                xev.xclient.window = x11win->window;
                xev.xclient.message_type = wmState;
                xev.xclient.format = 32;
                xev.xclient.data.l[0] = alwaysOnTop ? 1 : 0; // _NET_WM_STATE_ADD or REMOVE
                xev.xclient.data.l[1] = aboveAtom;
                xev.xclient.data.l[2] = 0;
                xev.xclient.data.l[3] = 0;

                XSendEvent(x11win->display, DefaultRootWindow(x11win->display), False,
                    SubstructureRedirectMask | SubstructureNotifyMask, &xev);
                XFlush(x11win->display);
            }
        }
    });
}

ELECTROBUN_EXPORT bool isWindowAlwaysOnTop(void* window) {
    if (!window) return false;

    bool result = false;
    dispatch_sync_main_void([&]() {
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (GTK_IS_WINDOW(gtkWindow)) {
                GdkWindow* gdkWindow = gtk_widget_get_window(gtkWindow);
                if (gdkWindow) {
                    GdkWindowState state = gdk_window_get_state(gdkWindow);
                    result = (state & GDK_WINDOW_STATE_ABOVE) != 0;
                }
            }
        } else {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                Atom wmState = XInternAtom(x11win->display, "_NET_WM_STATE", False);
                Atom aboveAtom = XInternAtom(x11win->display, "_NET_WM_STATE_ABOVE", False);

                Atom actualType;
                int actualFormat;
                unsigned long nItems, bytesAfter;
                unsigned char* propData = nullptr;

                if (XGetWindowProperty(x11win->display, x11win->window, wmState,
                        0, (~0L), False, XA_ATOM, &actualType, &actualFormat,
                        &nItems, &bytesAfter, &propData) == Success && propData) {
                    Atom* atoms = reinterpret_cast<Atom*>(propData);
                    for (unsigned long i = 0; i < nItems; i++) {
                        if (atoms[i] == aboveAtom) {
                            result = true;
                            break;
                        }
                    }
                    XFree(propData);
                }
            }
        }
    });
    return result;
}

ELECTROBUN_EXPORT void setWindowVisibleOnAllWorkspaces(void* window, bool visible) {
    // Not applicable on Linux - no-op
}

ELECTROBUN_EXPORT bool isWindowVisibleOnAllWorkspaces(void* window) {
    // Not applicable on Linux
    return false;
}

ELECTROBUN_EXPORT void setWindowPosition(void* window, double x, double y) {
    if (!window) return;

    dispatch_sync_main_void([=]() {
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (GTK_IS_WINDOW(gtkWindow)) {
                gtk_window_move(GTK_WINDOW(gtkWindow), (int)x, (int)y);
            }
        } else {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                // Set window position, accounting for window manager
                XMoveWindow(x11win->display, x11win->window, (int)x, (int)y);
                x11win->requestPosition(x, y);
                
                // Also send a ConfigureRequest event to ensure window manager compliance
                XEvent event;
                memset(&event, 0, sizeof(event));
                event.xconfigure.type = ConfigureNotify;
                event.xconfigure.window = x11win->window;
                event.xconfigure.x = (int)x;
                event.xconfigure.y = (int)y;
                XSendEvent(x11win->display, x11win->window, False, StructureNotifyMask, &event);
                
                XFlush(x11win->display);
            }
        }
    });
}

// Wayland has no primary monitor (GDK returns none there), so the first
// monitor stands in, as it does on X11 without a primary set.
static GdkMonitor* primaryOrFirstMonitor(GdkDisplay* display) {
    GdkMonitor* monitor = gdk_display_get_primary_monitor(display);
    if (!monitor && gdk_display_get_n_monitors(display) > 0) {
        monitor = gdk_display_get_monitor(display, 0);
    }
    return monitor;
}

ELECTROBUN_EXPORT void centerWindow(void* window) {
    if (!window) return;

    dispatch_sync_main_void([=]() {
        GdkDisplay* gdkDisplay = gdk_display_get_default();
        if (!gdkDisplay) return;
        GdkMonitor* monitor = primaryOrFirstMonitor(gdkDisplay);
        if (!monitor) return;

        GdkRectangle workarea{};
        gdk_monitor_get_workarea(monitor, &workarea);

        if (isCEFAvailable()) {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (!x11win || !x11win->display || !x11win->window) return;
            XWindowAttributes attributes{};
            if (!XGetWindowAttributes(x11win->display, x11win->window, &attributes)) return;
            const int x = workarea.x + std::max(0, (workarea.width - attributes.width) / 2);
            const int y = workarea.y + std::max(0, (workarea.height - attributes.height) / 2);
            XMoveWindow(x11win->display, x11win->window, x, y);
            x11win->requestPosition(x, y);
            XFlush(x11win->display);
            return;
        }

        if (GTK_IS_WINDOW(window)) {
            int width = 0;
            int height = 0;
            gtk_window_get_size(GTK_WINDOW(window), &width, &height);
            const int x = workarea.x + std::max(0, (workarea.width - width) / 2);
            const int y = workarea.y + std::max(0, (workarea.height - height) / 2);
            gtk_window_move(GTK_WINDOW(window), x, y);
        }
    });
}

ELECTROBUN_EXPORT void setWindowButtonPosition(void* window, double x, double y) {
    (void)window;
    (void)x;
    (void)y;
    // Not applicable on Linux - no-op
}

ELECTROBUN_EXPORT void getWindowButtonPosition(void* window, double* x, double* y) {
    (void)window;
    if (x) *x = 0;
    if (y) *y = 0;
}

ELECTROBUN_EXPORT void setWindowSize(void* window, double width, double height) {
    if (!window) return;

    dispatch_sync_main_void([=]() {
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (GTK_IS_WINDOW(gtkWindow)) {
                gtk_window_resize(GTK_WINDOW(gtkWindow), (int)width, (int)height);
            }
        } else {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                XResizeWindow(x11win->display, x11win->window, (unsigned int)width, (unsigned int)height);
                x11win->requestSize(width, height);
                XFlush(x11win->display);
            }
        }
    });
}

ELECTROBUN_EXPORT void setWindowFrame(void* window, double x, double y, double width, double height) {
    if (!window) return;

    dispatch_sync_main_void([=]() {
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (GTK_IS_WINDOW(gtkWindow)) {
                gtk_window_move(GTK_WINDOW(gtkWindow), (int)x, (int)y);
                gtk_window_resize(GTK_WINDOW(gtkWindow), (int)width, (int)height);
            }
        } else {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                XMoveResizeWindow(x11win->display, x11win->window, (int)x, (int)y, (unsigned int)width, (unsigned int)height);
                x11win->requestPosition(x, y);
                x11win->requestSize(width, height);
                XFlush(x11win->display);
            }
        }
    });
}

ELECTROBUN_EXPORT void getWindowFrame(void* window, double* outX, double* outY, double* outWidth, double* outHeight) {
    if (!window) {
        *outX = 0;
        *outY = 0;
        *outWidth = 0;
        *outHeight = 0;
        return;
    }

    dispatch_sync_main_void([&]() {
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (GTK_IS_WINDOW(gtkWindow)) {
                gint wx, wy, ww, wh;
                gtk_window_get_position(GTK_WINDOW(gtkWindow), &wx, &wy);
                gtk_window_get_size(GTK_WINDOW(gtkWindow), &ww, &wh);
                *outX = (double)wx;
                *outY = (double)wy;
                *outWidth = (double)ww;
                *outHeight = (double)wh;
            }
        } else {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                XWindowAttributes attrs;
                if (XGetWindowAttributes(x11win->display, x11win->window, &attrs)) {
                    // Get the absolute position of the window
                    int abs_x = 0, abs_y = 0;
                    Window child;
                    
                    // Translate from window coordinates (0,0) to root window coordinates
                    XTranslateCoordinates(x11win->display, x11win->window,
                        DefaultRootWindow(x11win->display), 
                        0, 0, &abs_x, &abs_y, &child);
                    
                    // For windows with decorations, we need to get the frame extents
                    Atom actualType;
                    int actualFormat;
                    unsigned long nItems, bytesAfter;
                    unsigned char* data = nullptr;
                    
                    Atom frameExtents = XInternAtom(x11win->display, "_NET_FRAME_EXTENTS", False);
                    if (frameExtents != None) {
                        if (XGetWindowProperty(x11win->display, x11win->window, frameExtents,
                                             0, 4, False, XA_CARDINAL,
                                             &actualType, &actualFormat, &nItems, &bytesAfter,
                                             &data) == Success && data) {
                            if (nItems == 4 && actualFormat == 32) {
                                long* extents = (long*)data;
                                // Adjust position by left and top frame extents
                                abs_x -= extents[0]; // left
                                abs_y -= extents[2]; // top
                            }
                            XFree(data);
                        }
                    }
                    
                    *outX = (double)abs_x;
                    *outY = (double)abs_y;
                    *outWidth = (double)attrs.width;
                    *outHeight = (double)attrs.height;
                    const auto& requested = x11win->requestedGeometry;
                    if (requested.hasPosition) {
                        *outX = requested.x;
                        *outY = requested.y;
                    }
                    if (requested.hasSize) {
                        *outWidth = requested.width;
                        *outHeight = requested.height;
                    }
                } else {
                    *outX = 0;
                    *outY = 0;
                    *outWidth = 0;
                    *outHeight = 0;
                }
            }
        }
    });
}

// Return the drawable client area's screen-space origin. Unlike
// getWindowFrame(), this intentionally excludes window-manager decorations so
// screen-space pointer coordinates can be translated into content coordinates.
ELECTROBUN_EXPORT void getWindowContentOrigin(void* window, double* outX, double* outY) {
    if (!outX || !outY) return;

    *outX = 0;
    *outY = 0;
    if (!window) return;

    dispatch_sync_main_void([&]() {
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (!GTK_IS_WINDOW(gtkWindow)) return;

            GdkWindow* contentWindow = gtk_widget_get_window(gtkWindow);
            if (contentWindow) {
                gint contentX = 0;
                gint contentY = 0;
                gdk_window_get_origin(contentWindow, &contentX, &contentY);
                *outX = static_cast<double>(contentX);
                *outY = static_cast<double>(contentY);
                return;
            }

            // A not-yet-realized GTK window has no client GdkWindow. Preserve
            // the best available position until it is realized.
            gint windowX = 0;
            gint windowY = 0;
            gtk_window_get_position(GTK_WINDOW(gtkWindow), &windowX, &windowY);
            *outX = static_cast<double>(windowX);
            *outY = static_cast<double>(windowY);
            return;
        }

        X11Window* x11win = static_cast<X11Window*>(window);
        if (!x11win || !x11win->display || !x11win->window) return;

        int contentX = 0;
        int contentY = 0;
        Window child = None;
        if (XTranslateCoordinates(
                x11win->display,
                x11win->window,
                DefaultRootWindow(x11win->display),
                0,
                0,
                &contentX,
                &contentY,
                &child)) {
            *outX = static_cast<double>(contentX);
            *outY = static_cast<double>(contentY);
        }
    });
}

ELECTROBUN_EXPORT void getWindowPosition(void* window, double* outX, double* outY) {
    double width, height;
    getWindowFrame(window, outX, outY, &width, &height);
}

ELECTROBUN_EXPORT void getWindowSize(void* window, double* outWidth, double* outHeight) {
    double x, y;
    getWindowFrame(window, &x, &y, outWidth, outHeight);
}

ELECTROBUN_EXPORT void setWindowIcon(void* window, const char* iconPath) {
    if (!window || !iconPath) return;

    dispatch_sync_main_void([=]() {
        std::string actualPath(iconPath);
        
        // Handle views:// protocol
        if (actualPath.substr(0, 8) == "views://") {
            std::string viewPath = normalizeViewsRelativePath(actualPath);
            
            // Try to load from ASAR archive first if available
            if (g_asarArchive) {
                size_t fileSize = 0;
                const uint8_t* fileData = nullptr;
                
                // Protect ASAR read operations with mutex
                {
                    std::lock_guard<std::mutex> lock(g_asarReadMutex);
                    fileData = asar_read_file(g_asarArchive, 
                        ("views/" + viewPath).c_str(), &fileSize);
                }
                
                if (fileData && fileSize > 0) {
                    // Create pixbuf from memory
                    GError* error = nullptr;
                    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
                    
                    if (gdk_pixbuf_loader_write(loader, fileData, fileSize, &error)) {
                        gdk_pixbuf_loader_close(loader, nullptr);
                        GdkPixbuf* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
                        
                        if (pixbuf) {
                            g_object_ref(pixbuf); // Keep a reference
                            
                            if (GTK_IS_WIDGET(window)) {
                                gtk_window_set_icon(GTK_WINDOW(window), pixbuf);
                            } else {
                                // Handle X11 window icon setting (moved to separate function)
                                setX11WindowIcon(static_cast<X11Window*>(window), pixbuf);
                            }
                            
                            g_object_unref(pixbuf);
                        }
                    }
                    
                    g_object_unref(loader);
                    
                    // Free ASAR buffer with mutex protection
                    {
                        std::lock_guard<std::mutex> lock(g_asarReadMutex);
                        asar_free_buffer(fileData, fileSize);
                    }
                    
                    if (error) g_error_free(error);
                    return;
                }
            }
            
            // Fallback to file system
            actualPath = "Resources/app/views/" + viewPath;
        }
        
        if (GTK_IS_WIDGET(window)) {
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
            if (GTK_IS_WINDOW(gtkWindow)) {
                GError* error = nullptr;
                
                // Load icon from file
                GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(actualPath.c_str(), &error);
                if (pixbuf) {
                    gtk_window_set_icon(GTK_WINDOW(gtkWindow), pixbuf);
                    g_object_unref(pixbuf);
                } else {
                    fprintf(stderr, "Failed to load icon from %s: %s\n", actualPath.c_str(), 
                            error ? error->message : "Unknown error");
                    if (error) g_error_free(error);
                }
            }
        } else {
            // For X11/CEF windows
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                GError* error = nullptr;
                GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(actualPath.c_str(), &error);
                
                if (pixbuf) {
                    setX11WindowIcon(x11win, pixbuf);
                    g_object_unref(pixbuf);
                } else {
                    fprintf(stderr, "Failed to load icon from %s: %s\n", actualPath.c_str(), 
                            error ? error->message : "Unknown error");
                    if (error) g_error_free(error);
                }
            }
        }
    });
}

/*
 * =============================================================================
 * GLOBAL KEYBOARD SHORTCUTS
 * =============================================================================
 */

// Callback type for global shortcut triggers
typedef void (*GlobalShortcutCallback)(const char* accelerator);
