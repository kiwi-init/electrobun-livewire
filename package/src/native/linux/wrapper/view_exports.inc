// Global flags set by setNextWebviewFlags, consumed by initWebview
static struct {
    bool startTransparent;
    bool startPassthrough;
} g_nextWebviewFlags = {false, false};
static AllowedProtocols g_nextAllowedProtocols = {true, false};

AbstractView* initGTKWebkitWebview(uint32_t webviewId,
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
    
    // Flags are passed from the main initWebview function
    
    AbstractView* result = dispatch_sync_main([&]() -> AbstractView* {
        try {
            
            auto webview = std::make_shared<WebKitWebViewImpl>(
                webviewId, GTK_WIDGET(window),
                url, x, y, width, height, autoResize,
                partitionIdentifier, navigationCallback, webviewEventHandler,
                eventBridgeHandler, bunBridgeHandler, internalBridgeHandler,
                electrobunPreloadScript, customPreloadScript, sandbox,
                startTransparent, startPassthrough
            );
            
            // Set fullSize flag for auto-resize functionality
            webview->fullSize = autoResize;
            
            // Store the viewsRoot for views:// protocol resolution
            if (viewsRoot && strlen(viewsRoot) > 0) {
                webview->viewsRoot = std::string(viewsRoot);
                // Also store in the separate map for access by handlers
                {
                    std::lock_guard<std::mutex> lock(g_webviewViewsRootMutex);
                    g_webviewViewsRoot[webviewId] = std::string(viewsRoot);
                }
            }
            
            // Store the webview in global map to keep it alive and for navigation rules
            {
                std::lock_guard<std::mutex> lock(g_webviewMapMutex);
                g_webviewMap[webviewId] = webview;
            }
            
            // Webview created successfully
            
            {
                std::lock_guard<std::mutex> lock(g_containersMutex);
                for (auto& [id, container] : g_containers) {
                    if (container->window == GTK_WIDGET(window)) {
                        container->addWebview(webview, x, y);
                        break;
                    }
                }
            }

            // Flags are now set in the constructor

            return webview.get();
        } catch (const std::exception& e) {
            return nullptr;
        }
    });

    return result;
}

ELECTROBUN_EXPORT void setNextWebviewFlags(bool startTransparent, bool startPassthrough) {
    g_nextWebviewFlags.startTransparent = startTransparent;
    g_nextWebviewFlags.startPassthrough = startPassthrough;
}

ELECTROBUN_EXPORT void setNextWebviewAllowedProtocols(bool allowViews, bool allowAppData) {
    g_nextAllowedProtocols = {allowViews, allowAppData};
}

ELECTROBUN_EXPORT AbstractView* initWebview(uint32_t webviewId,
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
                         bool transparent,
                         bool sandbox) {
    // Read and clear pre-set flags
    bool startTransparent = g_nextWebviewFlags.startTransparent;
    bool startPassthrough = g_nextWebviewFlags.startPassthrough;
    g_nextWebviewFlags = {false, false};
    const AllowedProtocols allowedProtocols = g_nextAllowedProtocols;
    g_nextAllowedProtocols = {true, false};
    {
        std::lock_guard<std::mutex> lock(g_allowedProtocolsMutex);
        g_allowedProtocols[webviewId] = allowedProtocols;
    }

    // TODO: Implement transparent handling for Linux

    // Null pointer checks
    if (!window) {
        fprintf(stderr, "ERROR: initWebview called with null window pointer\n");
        return nullptr;
    }

    // Wait for GTK initialization to complete before creating any webviews
    waitForGTKInit();

    AbstractView* view = nullptr;
    if (isCEFAvailable()) {
        view = initCEFWebview(webviewId, window, renderer, url, x, y, width, height, autoResize,
                              partitionIdentifier, navigationCallback, webviewEventHandler,
                              eventBridgeHandler, bunBridgeHandler, internalBridgeHandler,
                              electrobunPreloadScript, customPreloadScript, viewsRoot, sandbox,
                              startTransparent, startPassthrough);
    } else {
        view = initGTKWebkitWebview(webviewId, window, renderer, url, x, y, width, height, autoResize,
                                    partitionIdentifier, navigationCallback, webviewEventHandler,
                                    eventBridgeHandler, bunBridgeHandler, internalBridgeHandler,
                                    electrobunPreloadScript, customPreloadScript, viewsRoot, sandbox,
                                    startTransparent, startPassthrough);
    }

    // Flags are now set in the constructor, no need to set them here

    return view;

}

ELECTROBUN_EXPORT AbstractView* initWGPUView(uint32_t webviewId,
                         void* window,
                         double x, double y,
                         double width, double height,
                         bool autoResize,
                         bool startTransparent,
                         bool startPassthrough) {
    if (!window) {
        fprintf(stderr, "ERROR: initWGPUView called with null window pointer\n");
        return nullptr;
    }

    waitForGTKInit();

    auto view = std::make_shared<WGPUViewImpl>(webviewId);
    view->fullSize = autoResize;
    view->pendingStartTransparent = startTransparent;
    view->pendingStartPassthrough = startPassthrough;

    dispatch_sync_main_void([&]() {
        GdkRectangle frame = {(int)x, (int)y, (int)width, (int)height};
        if (isCEFAvailable()) {
            X11Window* x11win = static_cast<X11Window*>(window);
            if (!x11win || !x11win->display || !x11win->window) {
                fprintf(stderr, "ERROR: Failed to create X11-backed WGPUView\n");
                view->creationFailed = true;
                return;
            }

            if (!view->createX11Child(
                x11win->display,
                x11win->window,
                x,
                y,
                width,
                height,
                "CEF"
            )) {
                view->creationFailed = true;
                return;
            }
            if (!view->createX11InputChild(
                x11win->display,
                x11win->window,
                x,
                y,
                width,
                height,
                "CEF"
            )) {
                fprintf(stderr, "WARNING: WGPUView will use drawing-window input for CEF\n");
            }

            {
                std::lock_guard<std::mutex> lock(g_x11WindowsMutex);
                g_x11_child_window_to_parent_id[view->xWindow] = x11win->windowId;
                if (view->inputXWindow) {
                    g_x11_child_window_to_parent_id[view->inputXWindow] = x11win->windowId;
                }
            }

            XMapRaised(x11win->display, view->xWindow);
            if (view->inputXWindow) {
                XMapRaised(x11win->display, view->inputXWindow);
            }
            view->resize(frame, "");

            if (startPassthrough) {
                view->setPassthrough(true);
                view->pendingStartPassthrough = false;
            }
            XFlush(x11win->display);
            return;
        }

        GtkWidget* windowWidget = static_cast<GtkWidget*>(window);
        auto container = getContainerViewForWindow(windowWidget);
        if (!container) {
            fprintf(stderr, "ERROR: Failed to create container for WGPUView\n");
            view->creationFailed = true;
            return;
        }

        // GTK transparent toplevels use an RGBA visual, while Dawn/Vulkan expects
        // the WGPU surface window to support opaque alpha. Use a default-visual
        // X11 child for every GTK WGPU view so transparent and non-transparent
        // Linux renderers share the same masking/passthrough behavior.
        if (!gtk_widget_get_realized(windowWidget)) {
            gtk_widget_realize(windowWidget);
        }

        GdkWindow* parentGdkWindow = gtk_widget_get_window(windowWidget);
        if (parentGdkWindow) {
            Display* display = gdk_x11_display_get_xdisplay(gdk_window_get_display(parentGdkWindow));
            Window parentXWindow = GDK_WINDOW_XID(parentGdkWindow);

            if (view->createX11Child(display, parentXWindow, x, y, width, height, "GTK")) {
                if (!view->createX11InputChild(display, parentXWindow, x, y, width, height, "GTK")) {
                    fprintf(stderr, "WARNING: WGPUView will use drawing-window input for GTK\n");
                }
                container->abstractViews.insert(container->abstractViews.begin(), view);
                XMapRaised(display, view->xWindow);
                if (view->inputXWindow) {
                    XMapRaised(display, view->inputXWindow);
                }
                view->resize(frame, "");

                if (startTransparent) {
                    view->setTransparent(true);
                    view->pendingStartTransparent = false;
                }
                if (startPassthrough) {
                    view->setPassthrough(true);
                    view->pendingStartPassthrough = false;
                }
                XFlush(display);
                return;
            }
        }

        if (parentGdkWindow) {
            fprintf(stderr, "WARNING: Falling back to GtkDrawingArea-backed WGPUView\n");
        } else {
            fprintf(stderr, "WARNING: Failed to get GTK parent window for WGPUView; falling back to GtkDrawingArea\n");
        }

        view->xDisplay = nullptr;
        view->parentXWindow = 0;
        view->xWindow = 0;
        view->viewWidget = gtk_drawing_area_new();
        view->widget = view->viewWidget;

        gtk_widget_set_size_request(
            view->viewWidget,
            autoResize ? -1 : (int)width,
            autoResize ? -1 : (int)height
        );
        container->addWebview(view, x, y);
        view->resize(frame, "");

        if (startTransparent) {
            view->setTransparent(true);
            view->pendingStartTransparent = false;
        }
        if (startPassthrough) {
            view->setPassthrough(true);
            view->pendingStartPassthrough = false;
        }
    });

    if (view->creationFailed) {
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_wgpuViewMapMutex);
        g_wgpuViewMap[webviewId] = view;
    }

    return view.get();
}

ELECTROBUN_EXPORT void loadURLInWebView(AbstractView* abstractView, const char* urlString) {
    if (abstractView && urlString) {
        std::string urlStr(urlString);  // Copy the string to ensure it survives
        dispatch_sync_main_void([abstractView, urlStr]() {  // Capture by value
            abstractView->loadURL(urlStr.c_str());
        });
    }
}

ELECTROBUN_EXPORT void wgpuViewSetFrame(AbstractView* abstractView, double x, double y, double width, double height) {
    if (!abstractView) return;
    LogicalRect frame = {x, y, width, height};
    abstractView->storePendingResize(frame, "");
    g_pendingResizeQueue.enqueue(abstractView);
    schedulePendingResizeDrain();
}

ELECTROBUN_EXPORT void wgpuViewSetTransparent(AbstractView* abstractView, bool transparent) {
    if (!abstractView) return;
    dispatch_sync_main_void([&]() {
        abstractView->setTransparent(transparent);
    });
}

ELECTROBUN_EXPORT void wgpuViewSetPassthrough(AbstractView* abstractView, bool enablePassthrough) {
    if (!abstractView) return;
    dispatch_sync_main_void([&]() {
        abstractView->setPassthrough(enablePassthrough);
    });
}

ELECTROBUN_EXPORT void wgpuViewSetHidden(AbstractView* abstractView, bool hidden) {
    if (!abstractView) return;
    dispatch_sync_main_void([&]() {
        abstractView->setHidden(hidden);
    });
}

ELECTROBUN_EXPORT void wgpuViewRemove(AbstractView* abstractView) {
    if (!abstractView) return;
    dispatch_sync_main_void([abstractView]() {
        std::shared_ptr<AbstractView> retainedView;
        {
            std::lock_guard<std::mutex> lock(g_wgpuViewMapMutex);
            auto it = std::find_if(
                g_wgpuViewMap.begin(),
                g_wgpuViewMap.end(),
                [abstractView](const auto& entry) {
                    return entry.second.get() == abstractView;
                });
            if (it == g_wgpuViewMap.end()) {
                return;
            }
            retainedView = it->second;
            g_wgpuViewMap.erase(it);
        }

        g_pendingResizeQueue.remove(retainedView.get());

        WGPUViewImpl* view = dynamic_cast<WGPUViewImpl*>(retainedView.get());
        if (view && view->xWindow) {
            std::lock_guard<std::mutex> lock(g_x11WindowsMutex);
            g_x11_child_window_to_parent_id.erase(view->xWindow);
            if (view->inputXWindow) {
                g_x11_child_window_to_parent_id.erase(view->inputXWindow);
            }
        }
        retainedView->remove();
    });
}

ELECTROBUN_EXPORT void* wgpuViewGetNativeHandle(AbstractView* abstractView) {
    if (!abstractView) return nullptr;
    WGPUViewImpl* view = dynamic_cast<WGPUViewImpl*>(abstractView);
    if (!view) return nullptr;
    if (view->xWindow) {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(view->xWindow));
    }
    if (view->viewWidget) {
        GdkWindow* gdkWindow = gtk_widget_get_window(view->viewWidget);
        if (gdkWindow) {
            return reinterpret_cast<void*>(static_cast<uintptr_t>(GDK_WINDOW_XID(gdkWindow)));
        }
    }
    return nullptr;
}

