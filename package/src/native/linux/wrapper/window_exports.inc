// Wayland compositors that negotiate server-side decorations but draw none.
// GTK3 then draws none either, so a framed window has no title or close
// button. Each entry is a name from XDG_CURRENT_DESKTOP and an env var the
// compositor sets for its clients.
struct DecorationlessCompositor {
    const char* desktop;
    const char* env;
};
static const DecorationlessCompositor kDecorationlessCompositors[] = {
    // Hyprland answers KDE server-decoration with mode SERVER and xdg-decoration
    // with SERVER_SIDE, whatever the client asks (src/protocols/
    // ServerDecorationKDE.cpp, XDGDecoration.cpp), and draws only a border.
    {"Hyprland", "HYPRLAND_INSTANCE_SIGNATURE"},
};

// Whether framed GTK windows draw their own header bar. ELECTROBUN_WAYLAND_CSD=1
// forces it on any Wayland compositor and =0 turns it off.
static bool shouldDrawClientDecorations() {
#ifdef GDK_WINDOWING_WAYLAND
    if (!GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default())) {
        return false;
    }
    const char* override = g_getenv("ELECTROBUN_WAYLAND_CSD");
    if (override && *override) {
        return strcmp(override, "0") != 0;
    }
    const char* desktop = g_getenv("XDG_CURRENT_DESKTOP");
    for (const auto& compositor : kDecorationlessCompositors) {
        if (g_getenv(compositor.env)) {
            return true;
        }
        if (desktop && strstr(desktop, compositor.desktop)) {
            return true;
        }
    }
    return false;
#else
    return false;
#endif
}

ELECTROBUN_EXPORT void* createGTKWindow(uint32_t windowId, double x, double y, double width, double height, const char* title,
                   WindowCloseCallback closeCallback, WindowMoveCallback moveCallback, WindowResizeCallback resizeCallback, WindowFocusCallback focusCallback, WindowBlurCallback blurCallback, WindowKeyHandler keyCallback, WindowShouldCloseHandler shouldCloseCallback,
                   const char* titleBarStyle = nullptr, bool transparent = false) {
    
   
    
    void* result = dispatch_sync_main([&]() -> void* {
      
  
        
        
        GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
       
        gtk_window_set_title(GTK_WINDOW(window), title);
        
        // Match the channel-specific StartupWMClass emitted by Hutch.
        gtk_window_set_wmclass(
            GTK_WINDOW(window),
            g_electrobunWindowClass.c_str(),
            g_electrobunWindowClass.c_str());
        
        gtk_window_set_default_size(GTK_WINDOW(window), (int)width, (int)height);
       
        if (x >= 0 && y >= 0) {
            gtk_window_move(GTK_WINDOW(window), (int)x, (int)y);
        }
        
        // Handle titleBarStyle for custom titlebars
        if (titleBarStyle && (strcmp(titleBarStyle, "hidden") == 0 ||
                             strcmp(titleBarStyle, "hiddenInset") == 0)) {
            // Both custom-titlebar styles need client resize handles when
            // server decorations are disabled. In particular, existing apps
            // using hidden must not need a style change to become resizable.
            gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
            // An empty titlebar makes GTK draw the window itself: on KWin it
            // asks for client-side decorations (request_mode 1) instead of a
            // server title bar over the app's own.
            gtk_window_set_titlebar(GTK_WINDOW(window), gtk_fixed_new());
            attachGtkWindowResizeHandles(window);
        } else if (shouldDrawClientDecorations()) {
            // GtkWindow keeps a header bar titlebar's title in sync with
            // gtk_window_set_title, and gtk_window_get_size leaves it out,
            // so frames still mean content size.
            GtkWidget* headerBar = gtk_header_bar_new();
            gtk_header_bar_set_title(GTK_HEADER_BAR(headerBar), title);
            gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(headerBar), TRUE);
            gtk_widget_show(headerBar);
            gtk_window_set_titlebar(GTK_WINDOW(window), headerBar);
        }
        
        // Handle transparency
        if (transparent) {
            // Enable RGBA visual for transparency
            GdkScreen* screen = gtk_window_get_screen(GTK_WINDOW(window));
            GdkVisual* visual = gdk_screen_get_rgba_visual(screen);
            
            if (visual && gdk_screen_is_composited(screen)) {
                gtk_widget_set_visual(window, visual);
                gtk_widget_set_app_paintable(window, TRUE);
                
                // Connect to draw signal to paint transparent background
                g_signal_connect(window, "draw", G_CALLBACK(+[](GtkWidget* widget, cairo_t* cr, gpointer data) -> gboolean {
                    // Clear the window with transparent background
                    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
                    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
                    cairo_paint(cr);
                    
                    // Let child widgets draw themselves
                    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
                    return FALSE;  // Continue with default drawing
                }), nullptr);
                
                printf("GTK: Created transparent window\n");
            } else {
                printf("GTK WARNING: Transparency not supported (no RGBA visual or compositor)\n");
            }
        }
        
        // Create container with callbacks
        auto container = std::make_shared<ContainerView>(window, windowId, closeCallback, shouldCloseCallback, moveCallback, resizeCallback, focusCallback, blurCallback, keyCallback);
      
        {
            std::lock_guard<std::mutex> lock(g_containersMutex);
            g_containers[windowId] = container;
        }

        // Apply application menu to new window if one is configured
        applyApplicationMenuToWindow(window);

        // Connect window delete event to handle X button clicks properly
        g_signal_connect(window, "delete-event", G_CALLBACK(onWindowDeleteEvent), container.get());

        // Connect destroy signal to clean up the container.
        // Note: cleanupWebviewsForWindow() may have already erased the container from
        // g_containers. We guard with windowId > 0 and hold g_containersMutex so the
        // erase is safe and idempotent.
        g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget* widget, gpointer user_data) {
            ContainerView* container = static_cast<ContainerView*>(user_data);
            if (container && container->windowId > 0) {
                std::lock_guard<std::mutex> lock(g_containersMutex);
                g_containers.erase(container->windowId);
            }
        }), container.get());

        // Connect window focus event
        g_signal_connect(window, "focus-in-event", G_CALLBACK(+[](GtkWidget* widget, GdkEventFocus* event, gpointer user_data) -> gboolean {
            reportWindowFocus(static_cast<ContainerView*>(user_data), true);
            return FALSE; // Allow event to propagate
        }), container.get());

        g_signal_connect(window, "focus-out-event", G_CALLBACK(+[](GtkWidget* widget, GdkEventFocus* event, gpointer user_data) -> gboolean {
            reportWindowFocus(static_cast<ContainerView*>(user_data), false);
            return FALSE; // Allow event to propagate
        }), container.get());

        g_signal_connect(window, "window-state-event", G_CALLBACK(+[](GtkWidget* widget, GdkEventWindowState* event, gpointer user_data) -> gboolean {
            if (event->changed_mask & GDK_WINDOW_STATE_FOCUSED) {
                reportWindowFocus(static_cast<ContainerView*>(user_data),
                                  (event->new_window_state & GDK_WINDOW_STATE_FOCUSED) != 0);
            }
            return FALSE;
        }), container.get());

        // Note: Removed gtk_main_quit as default behavior - let the app decide whether to exit


        // Connect window resize signal for auto-resize functionality
        g_signal_connect(window, "configure-event", G_CALLBACK(onWindowConfigure), container.get());
      
        
        // Connect mouse motion event for debugging
        gtk_widget_add_events(window, GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
        g_signal_connect(window, "motion-notify-event", G_CALLBACK(onMouseMove), container.get());
        
        // Connect keyboard events
        g_signal_connect(window, "key-press-event", G_CALLBACK(+[](GtkWidget* widget, GdkEventKey* event, gpointer user_data) -> gboolean {
            ContainerView* container = static_cast<ContainerView*>(user_data);
            if (container && container->keyCallback) {
                // Convert GDK modifiers to our format
                uint32_t modifiers = 0;
                if (event->state & GDK_SHIFT_MASK) modifiers |= (1 << 0);
                if (event->state & GDK_CONTROL_MASK) modifiers |= (1 << 1);
                if (event->state & GDK_MOD1_MASK) modifiers |= (1 << 2); // Alt
                if (event->state & GDK_SUPER_MASK) modifiers |= (1 << 3); // Super/Windows key
                
                // GDK uses hardware keycodes which should match X11 keycodes
                container->keyCallback(container->windowId, event->hardware_keycode, modifiers, 1u, 0u);
            }
            return FALSE; // Allow event to propagate
        }), container.get());
        
        g_signal_connect(window, "key-release-event", G_CALLBACK(+[](GtkWidget* widget, GdkEventKey* event, gpointer user_data) -> gboolean {
            ContainerView* container = static_cast<ContainerView*>(user_data);
            if (container && container->keyCallback) {
                // Convert GDK modifiers to our format
                uint32_t modifiers = 0;
                if (event->state & GDK_SHIFT_MASK) modifiers |= (1 << 0);
                if (event->state & GDK_CONTROL_MASK) modifiers |= (1 << 1);
                if (event->state & GDK_MOD1_MASK) modifiers |= (1 << 2); // Alt
                if (event->state & GDK_SUPER_MASK) modifiers |= (1 << 3); // Super/Windows key
                
                // GDK uses hardware keycodes which should match X11 keycodes
                container->keyCallback(container->windowId, event->hardware_keycode, modifiers, 0u, 0u);
            }
            return FALSE; // Allow event to propagate
        }), container.get());
   
        
        return (void*)window;
   
        
    });
    
  
    
    return result;
}

// Mac-compatible function for Linux
ELECTROBUN_EXPORT void* createWindowWithFrameAndStyleFromWorker(uint32_t windowId, double x, double y, double width, double height,
                                             uint32_t styleMask, const char* titleBarStyle, bool transparent,
                                             double trafficLightOffsetX, double trafficLightOffsetY,
                                             WindowCloseCallback closeCallback, WindowMoveCallback moveCallback, WindowResizeCallback resizeCallback, WindowFocusCallback focusCallback, WindowBlurCallback blurCallback, WindowKeyHandler keyCallback, WindowShouldCloseHandler shouldCloseCallback) {
    (void)trafficLightOffsetX;
    (void)trafficLightOffsetY;

    // CEF supports custom frames and transparency, GTK doesn't
    if (isCEFAvailable()) {
        return createX11Window(windowId, x, y, width, height, "Window", closeCallback, moveCallback, resizeCallback, focusCallback, blurCallback, keyCallback, shouldCloseCallback, titleBarStyle, transparent);
    } else {
        // Pass titleBarStyle and transparent to GTK window creation
        return createGTKWindow(windowId, x, y, width, height, "Window", closeCallback, moveCallback, resizeCallback, focusCallback, blurCallback, keyCallback, shouldCloseCallback, titleBarStyle, transparent);
    }

}

void setX11WindowTitle(void* window, const char* title) {
    dispatch_sync_main_void([&]() {
        X11Window* x11win = static_cast<X11Window*>(window);
        if (x11win && x11win->display && x11win->window) {
            XStoreName(x11win->display, x11win->window, title);
            XFlush(x11win->display);
            x11win->title = title;
        }
    });
}

void setGTKWindowTitle(void* window, const char* title) {
    dispatch_sync_main_void([&]() {
        gtk_window_set_title(GTK_WINDOW(window), title);
    });
}

// Cross-platform compatible function for Linux
ELECTROBUN_EXPORT void setWindowTitle(void* window, const char* title) {
    if (isCEFAvailable()) {
        setX11WindowTitle(window, title);
    } else {
        setGTKWindowTitle(window, title);
    }
}

void showX11Window(void* window) {
    dispatch_sync_main_void([&]() {
        X11Window* x11win = static_cast<X11Window*>(window);
        if (x11win && x11win->display && x11win->window) {
            // Automatically set icon from standard location
            autoSetWindowIcon(window);
            XMapWindow(x11win->display, x11win->window);
            
            // Raise the window to the front
            XRaiseWindow(x11win->display, x11win->window);
            
            // Set input focus to the window
            XSetInputFocus(x11win->display, x11win->window, RevertToParent, CurrentTime);
            
            XFlush(x11win->display);
            
            // Apply application menu when window is shown
            applyApplicationMenuToX11Window(x11win);
        }
    });
}

void showX11WindowWithoutActivating(void* window) {
    dispatch_sync_main_void([&]() {
        X11Window* x11win = static_cast<X11Window*>(window);
        if (x11win && x11win->display && x11win->window) {
            autoSetWindowIcon(window);
            XMapWindow(x11win->display, x11win->window);
            XRaiseWindow(x11win->display, x11win->window);
            XFlush(x11win->display);
            applyApplicationMenuToX11Window(x11win);
        }
    });
}

void activateX11Window(void* window) {
    dispatch_sync_main_void([&]() {
        X11Window* x11win = static_cast<X11Window*>(window);
        if (x11win && x11win->display && x11win->window) {
            XWindowAttributes attrs;
            if (XGetWindowAttributes(x11win->display, x11win->window, &attrs) == 0 ||
                attrs.map_state == IsUnmapped) {
                return;
            }

            XRaiseWindow(x11win->display, x11win->window);
            XSetInputFocus(x11win->display, x11win->window, RevertToParent, CurrentTime);
            XFlush(x11win->display);
        }
    });
}

void showGTKWindow(void* window) {
    dispatch_sync_main_void([&]() {
        // Automatically set icon from standard location
        autoSetWindowIcon(window);
        gtk_widget_show_all(GTK_WIDGET(window));
        
        // Bring the window to the front and give it focus
        gtk_window_present(GTK_WINDOW(window));
    });
}

void showGTKWindowWithoutActivating(void* window) {
    dispatch_sync_main_void([&]() {
        autoSetWindowIcon(window);
        gtk_widget_show_all(GTK_WIDGET(window));
    });
}

void activateGTKWindow(void* window) {
    dispatch_sync_main_void([&]() {
        if (!gtk_widget_get_visible(GTK_WIDGET(window))) {
            return;
        }
        gtk_window_present(GTK_WINDOW(window));
    });
}

ELECTROBUN_EXPORT void showWindow(void* window, bool activate) {
    if (isCEFAvailable()) {
        if (activate) {
            showX11Window(window);
        } else {
            showX11WindowWithoutActivating(window);
        }
    } else {
        if (activate) {
            showGTKWindow(window);
        } else {
            showGTKWindowWithoutActivating(window);
        }
    }
}

ELECTROBUN_EXPORT void activateWindow(void* window) {
    if (isCEFAvailable()) {
        activateX11Window(window);
    } else {
        activateGTKWindow(window);
    }
}

void hideX11Window(void* window) {
    dispatch_sync_main_void([&]() {
        X11Window* x11win = static_cast<X11Window*>(window);
        if (x11win && x11win->display && x11win->window) {
            XUnmapWindow(x11win->display, x11win->window);
            XFlush(x11win->display);
        }
    });
}

void hideGTKWindow(void* window) {
    dispatch_sync_main_void([&]() {
        gtk_widget_hide(GTK_WIDGET(window));
    });
}

ELECTROBUN_EXPORT void hideWindow(void* window) {
    if (isCEFAvailable()) {
        hideX11Window(window);
    } else {
        hideGTKWindow(window);
    }
}

bool isX11WindowVisible(void* window) {
    return dispatch_sync_main([&]() -> bool {
        X11Window* x11win = static_cast<X11Window*>(window);
        if (!x11win || !x11win->display || !x11win->window) {
            return false;
        }

        XWindowAttributes attributes = {};
        return XGetWindowAttributes(x11win->display, x11win->window, &attributes) != 0 &&
               attributes.map_state != IsUnmapped;
    });
}

bool isGTKWindowVisible(void* window) {
    return dispatch_sync_main([&]() -> bool {
        return window && gtk_widget_get_visible(GTK_WIDGET(window));
    });
}

ELECTROBUN_EXPORT bool isWindowVisible(void* window) {
    return isCEFAvailable() ? isX11WindowVisible(window) : isGTKWindowVisible(window);
}

// Cross-platform compatible function for Linux - return dummy style mask
ELECTROBUN_EXPORT uint32_t getWindowStyle(bool borderless, bool titled, bool closable, bool miniaturizable,
                        bool resizable, bool unifiedTitleAndToolbar, bool fullScreen,
                        bool fullSizeContentView, bool utilityWindow, bool docModalWindow,
                        bool nonactivatingPanel, bool hudWindow) {
    // Linux doesn't use style masks like macOS, so just return a dummy value
    // The actual window styling is handled in createWindow
    return 0;
}



// Webview functions


AbstractView* initCEFWebview(uint32_t webviewId,
                         void* window,
                         const char* renderer,
                         const char* url,
                         double x, double y,
                         double width, double height,
                         bool autoResize,
                         const char* partitionIdentifier,
                         DecideNavigationCallback navigationCallback,
                         WebviewEventHandler webviewEventHandler,
                         HandlePostMessage eventBridgeHandler,
                         HandlePostMessage bunBridgeHandler,
                         HandlePostMessage internalBridgeHandler,
                         const char* electrobunPreloadScript,
                         const char* customPreloadScript,
                         const char* viewsRoot,
                         bool sandbox,
                         bool startTransparent,
                         bool startPassthrough) {
    
    AbstractView* result = dispatch_sync_main([&]() -> AbstractView* {
        try {
            std::shared_ptr<AbstractView> webview;
            
            webview = std::make_shared<CEFWebViewImpl>(
                webviewId, (GtkWidget*)window,  // window is now X11Window* cast to void*
                url, x, y, width, height, autoResize,
                partitionIdentifier, navigationCallback, webviewEventHandler,
                eventBridgeHandler, bunBridgeHandler, internalBridgeHandler,
                electrobunPreloadScript, customPreloadScript, sandbox,
                startTransparent, startPassthrough
            );
            
            if (webview->creationFailed) {
                printf("CEF webview creation failed\n");
                fflush(stdout);
                return nullptr;
            }
            
            // Store the viewsRoot for views:// protocol resolution
            if (viewsRoot && strlen(viewsRoot) > 0) {
                webview->viewsRoot = std::string(viewsRoot);
                // Also store in the separate map for access by handlers
                {
                    std::lock_guard<std::mutex> lock(g_webviewViewsRootMutex);
                    g_webviewViewsRoot[webviewId] = std::string(viewsRoot);
                }
            }
        
            // For CEF, we need to manually trigger position sync since there's no container       
            CEFWebViewImpl* cefView = dynamic_cast<CEFWebViewImpl*>(webview.get());
            if (cefView) {
                // Defer positioning until CEF window is ready
                LogicalRect frame = {x, y, width, height};
                if (cefView->fullSize) {
                    cefView->queryParentPhysicalBounds(frame);
                }
                
                // Store the frame for later positioning
                cefView->pendingFrame = frame;
                cefView->hasPendingFrame = true;
                
                // Try immediate positioning, but if it fails, the OnAfterCreated callback will retry
                cefView->syncCEFPositionWithFrame(frame);   
            }
           
            
            // Store the webview in global map to keep it alive
            {
                std::lock_guard<std::mutex> lock(g_webviewMapMutex);
                g_webviewMap[webviewId] = webview;
            }

            // Apply deferred initial transparent/passthrough state now that view is ready
            if (webview->pendingStartTransparent) {
                webview->setTransparent(true);
            }
            if (webview->pendingStartPassthrough) {
                webview->setPassthrough(true);
            }

            return webview.get();
        } catch (const std::exception& e) {
            printf("ERROR: Failed to create webview: %s\n", e.what());
            fflush(stdout);
            return nullptr;
        }
    });

    return result;
}

