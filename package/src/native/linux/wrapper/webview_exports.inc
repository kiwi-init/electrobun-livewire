ELECTROBUN_EXPORT void loadHTMLInWebView(AbstractView* abstractView, const char* htmlString) {
    if (abstractView && htmlString) {
        std::string htmlStr(htmlString);  // Copy the string to ensure it survives
        dispatch_sync_main_void([abstractView, htmlStr]() {  // Capture by value
            abstractView->loadHTML(htmlStr.c_str());
        });
    }
}

ELECTROBUN_EXPORT void webviewGoBack(AbstractView* abstractView) {
    if (abstractView) {
        dispatch_sync_main_void([&]() {
            abstractView->goBack();
        });
    }
}

ELECTROBUN_EXPORT void webviewGoForward(AbstractView* abstractView) {
    if (abstractView) {
        dispatch_sync_main_void([&]() {
            abstractView->goForward();
        });
    }
}

ELECTROBUN_EXPORT void webviewReload(AbstractView* abstractView) {
    if (abstractView) {
        dispatch_sync_main_void([&]() {
            abstractView->reload();
        });
    }
}

ELECTROBUN_EXPORT void webviewRemove(AbstractView* abstractView) {
    if (abstractView) {
        // Get the webview ID before scheduling async removal
        uint32_t webviewId = abstractView->webviewId;
        {
            std::lock_guard<std::mutex> lock(g_allowedProtocolsMutex);
            g_allowedProtocols.erase(webviewId);
        }
        {
            std::lock_guard<std::mutex> lock(g_webviewViewsRootMutex);
            g_webviewViewsRoot.erase(webviewId);
        }
        
        // Find the shared_ptr for this view to keep it alive during async removal
        std::shared_ptr<AbstractView> viewPtr;
        {
            std::lock_guard<std::mutex> lock(g_webviewMapMutex);
            auto it = g_webviewMap.find(webviewId);
            if (it != g_webviewMap.end() && it->second.get() == abstractView) {
                viewPtr = it->second;
            } else {
                return;
            }
        }

        g_pendingResizeQueue.remove(viewPtr.get());
        
        // Use g_idle_add to remove the webview asynchronously on the main thread
        // Pass the shared_ptr to keep the object alive
        struct RemoveData {
            std::shared_ptr<AbstractView> view;
        };
        
        RemoveData* data = new RemoveData{viewPtr};
        
        g_idle_add([](gpointer user_data) -> gboolean {
            RemoveData* data = static_cast<RemoveData*>(user_data);
            
            if (data && data->view) {
                data->view->remove();
            }
            
            delete data;
            return G_SOURCE_REMOVE; // Only run once
        }, data);
    }
}

ELECTROBUN_EXPORT bool webviewCanGoBack(AbstractView* abstractView) {
    if (abstractView) {
        return abstractView->canGoBack();
    }
    return false;
}

ELECTROBUN_EXPORT bool webviewTerminateRenderer(AbstractView* abstractView) {
    if (!abstractView) {
        return false;
    }
    return dispatch_sync_main([&]() -> bool {
        return abstractView->terminateRenderer();
    });
}

ELECTROBUN_EXPORT bool webviewCanGoForward(AbstractView* abstractView) {
    if (abstractView) {
        return abstractView->canGoForward();
    }
    return false;
}

void updateActiveWebviewForMousePosition(uint32_t windowId, int mouseX, int mouseY) {
    // Find the container for this window
    auto containerIt = g_containers.find(windowId);
    if (containerIt == g_containers.end()) {
        return;
    }
    
    auto container = containerIt->second;
    
    // Iterate through webviews in reverse order (topmost webview first)
    for (auto it = container->abstractViews.rbegin(); it != container->abstractViews.rend(); ++it) {
        auto webview = *it;
        
        // Skip removed webviews
        if (webview->isRemoved) {
            continue;
        }
        
        // Check if mouse is within the webview bounds
        if (mouseX >= webview->visualBounds.x && 
            mouseX < webview->visualBounds.x + webview->visualBounds.width &&
            mouseY >= webview->visualBounds.y && 
            mouseY < webview->visualBounds.y + webview->visualBounds.height) {
            
            // Check if the mouse is in a masked area
            if (!webview->maskJSON.empty()) {
                std::vector<MaskRect> masks = parseMaskJson(webview->maskJSON);
                
                // Convert mouse position to webview-relative coordinates
                int relativeX = mouseX - webview->visualBounds.x;
                int relativeY = mouseY - webview->visualBounds.y;
                
                if (isPointInMask(relativeX, relativeY, masks)) {
                    // Mouse is in a masked area, continue to next webview
                    continue;
                }
            }
            
            // This webview should be active
            if (container->activeWebView != webview.get()) {
                // Disable input for all webviews first
                for (auto& view : container->abstractViews) {
                    view->toggleMirrorMode(true);
                }
                
                // Enable input for this webview
                webview->toggleMirrorMode(false);
                container->activeWebView = webview.get();
            }
            return;
        }
    }
    
    // Mouse is not over any webview, disable input for all
    for (auto& view : container->abstractViews) {
        view->toggleMirrorMode(true);
    }
    container->activeWebView = nullptr;
}

ELECTROBUN_EXPORT void resizeWebview(AbstractView* abstractView, double x, double y, double width, double height, const char* masksJson) {
    if (!abstractView) {
        return;
    }
    
    if (abstractView->isRemoved) {
        return;
    }

    LogicalRect frame = {x, y, width, height};
    abstractView->storePendingResize(frame, masksJson);
    g_pendingResizeQueue.enqueue(abstractView);
    schedulePendingResizeDrain();
}

ELECTROBUN_EXPORT void evaluateJavaScriptWithNoCompletion(AbstractView* abstractView, const char* js) {
    if (abstractView && js) {
        std::string jsString(js);  // Copy the string to ensure it survives
        dispatch_sync_main_void([abstractView, jsString]() {  // Capture by value
            
            // Verify the abstractView is still valid
            if (abstractView) {
                abstractView->evaluateJavaScriptWithNoCompletion(jsString.c_str());
            } else {
                printf("evaluateJavaScriptWithNoCompletion: abstractView became NULL in dispatch!\n");
            }
        });
    } else {
        printf("evaluateJavaScriptWithNoCompletion: FFI entry, abstractView=%p, js=%p\n", abstractView, js);
    }
}

void webviewSetTransparent(AbstractView* abstractView, bool transparent) {
    if (abstractView) {
        dispatch_sync_main_void([&]() {
            abstractView->setTransparent(transparent);
        });
    }
}

void webviewSetPassthrough(AbstractView* abstractView, bool enablePassthrough) {
    if (abstractView) {
        dispatch_sync_main_void([&]() {
            abstractView->setPassthrough(enablePassthrough);
        });
    }
}

void webviewSetHidden(AbstractView* abstractView, bool hidden) {
    if (abstractView) {
        dispatch_sync_main_void([&]() {
            abstractView->setHidden(hidden);
        });
    }
}

ELECTROBUN_EXPORT void setWebviewContentSecurityPolicy(uint32_t webviewId, const char* policy) {
    electrobun::setViewContentSecurityPolicy(webviewId, policy);
}

ELECTROBUN_EXPORT void webviewSetNewWindowPolicy(AbstractView* abstractView, int32_t policy) {
    auto* view = dynamic_cast<WebKitWebViewImpl*>(abstractView);
    if (view) {
        dispatch_sync_main_void([view, policy]() {
            view->newWindowPolicy = policy;
        });
    }
}

ELECTROBUN_EXPORT bool webviewSetSpellCheck(AbstractView* abstractView, bool enabled) {
    (void)abstractView;
    (void)enabled;
    // This option intentionally targets macOS WKWebView, not WebKitGTK or CEF.
    return false;
}

// No native bridge-origin check here yet; the preload still gives the bridge
// only to pages on the bridge origins.
ELECTROBUN_EXPORT void setWebviewBridgeOrigins(AbstractView* abstractView, const char* originsJson) {
    (void)abstractView;
    (void)originsJson;
}

ELECTROBUN_EXPORT void setWebviewNavigationRules(AbstractView* abstractView, const char* rulesJson) {
    if (abstractView) {
        std::string rulesStr(rulesJson ? rulesJson : "");  // Copy the string to ensure it survives
        dispatch_sync_main_void([abstractView, rulesStr]() {
            abstractView->setNavigationRulesFromJSON(rulesStr.c_str());
        });
    }
}

ELECTROBUN_EXPORT void webviewFindInPage(AbstractView* abstractView, const char* searchText, bool forward, bool matchCase) {
    if (abstractView) {
        std::string text(searchText ? searchText : "");
        dispatch_sync_main_void([abstractView, text, forward, matchCase]() {
            abstractView->findInPage(text.c_str(), forward, matchCase);
        });
    }
}

ELECTROBUN_EXPORT void webviewStopFind(AbstractView* abstractView) {
    if (abstractView) {
        dispatch_sync_main_void([abstractView]() {
            abstractView->stopFindInPage();
        });
    }
}

ELECTROBUN_EXPORT void webviewOpenDevTools(AbstractView* abstractView) {
    if (abstractView) {
        dispatch_sync_main_void([abstractView]() {
            abstractView->openDevTools();
        });
    }
}

ELECTROBUN_EXPORT void webviewCloseDevTools(AbstractView* abstractView) {
    if (abstractView) {
        dispatch_sync_main_void([abstractView]() {
            abstractView->closeDevTools();
        });
    }
}

ELECTROBUN_EXPORT void webviewToggleDevTools(AbstractView* abstractView) {
    if (abstractView) {
        dispatch_sync_main_void([abstractView]() {
            abstractView->toggleDevTools();
        });
    }
}

ELECTROBUN_EXPORT void webviewSetPageZoom(AbstractView* abstractView, double zoomLevel) {
    if (!abstractView) return;

    dispatch_sync_main_void([abstractView, zoomLevel]() {
        auto* webKitView = dynamic_cast<WebKitWebViewImpl*>(abstractView);
        if (webKitView && webKitView->webview) {
            webkit_web_view_set_zoom_level(WEBKIT_WEB_VIEW(webKitView->webview), zoomLevel);
            return;
        }
        auto* cefView = dynamic_cast<CEFWebViewImpl*>(abstractView);
        if (cefView && zoomLevel > 0) {
            // The browser is created asynchronously and navigations reset
            // Chromium's zoom, so keep the request and apply it when possible.
            cefView->requestedPageZoom = zoomLevel;
            cefView->applyRequestedPageZoom();
        }
    });
}

ELECTROBUN_EXPORT double webviewGetPageZoom(AbstractView* abstractView) {
    if (!abstractView) return 1.0;

    double zoomLevel = 1.0;
    dispatch_sync_main_void([abstractView, &zoomLevel]() {
        auto* webKitView = dynamic_cast<WebKitWebViewImpl*>(abstractView);
        if (webKitView && webKitView->webview) {
            zoomLevel = webkit_web_view_get_zoom_level(WEBKIT_WEB_VIEW(webKitView->webview));
            return;
        }
        auto* cefView = dynamic_cast<CEFWebViewImpl*>(abstractView);
        if (cefView && cefView->browser) {
            zoomLevel = std::pow(1.2, cefView->browser->GetHost()->GetZoomLevel());
        } else if (cefView && cefView->requestedPageZoom > 0) {
            zoomLevel = cefView->requestedPageZoom;
        }
    });
    return zoomLevel;
}

ELECTROBUN_EXPORT void updatePreloadScriptToWebView(AbstractView* abstractView, const char* scriptIdentifier, const char* scriptContent, bool forMainFrameOnly) {
    if (abstractView) {
        dispatch_sync_main_void([&]() {
            abstractView->updateCustomPreloadScript(scriptContent);
        });
    }
}

// Forward declaration
void stopWindowMove();

// Window drag motion handler
static gboolean onWindowDragMotion(GtkWidget* widget, GdkEventMotion* event, gpointer user_data) {
    if (!g_draggedWindow || widget != g_draggedWindow || !event || !event->device) {
        return FALSE;
    }
    
    // Validate widget and its window
    GdkWindow* gdkWindow = gtk_widget_get_window(widget);
    if (!gdkWindow) {
        return FALSE;
    }
    
    // Get the current mouse position using the event data directly (more reliable)
    gint rootX = (gint)event->x_root;
    gint rootY = (gint)event->y_root;
    
    // Calculate new window position
    gint newX = rootX - g_dragStartX;
    gint newY = rootY - g_dragStartY;
    
    // Move the window
    gtk_window_move(GTK_WINDOW(widget), newX, newY);
    
    return FALSE; // Let other handlers process the event
}

// Window drag button release handler
static gboolean onWindowDragButtonRelease(GtkWidget* widget, GdkEventButton* event, gpointer user_data) {
    if (!event) {
        return FALSE;
    }
    
    if (event->button == 1) { // Left mouse button
        printf("Button release detected, stopping window move\n");
        fflush(stdout);
        stopWindowMove();
    }
    return FALSE; // Let other handlers process the event
}

ELECTROBUN_EXPORT void startWindowMove(void *window) {
  dispatch_sync_main_void([&]() {
    if (isCEFAvailable()) {
      // CEF is always forced to X11 mode (--ozone-platform=x11 / --use-x11),
      // so _NET_WM_MOVERESIZE works even on Wayland via XWayland.
      X11Window *x11win = static_cast<X11Window *>(window);
      if (!x11win || !x11win->display || !x11win->window)
        return;

      Window root, child;
      int rootX, rootY, winX, winY;
      unsigned int mask;
      XQueryPointer(x11win->display, x11win->window, &root, &child, &rootX,
                    &rootY, &winX, &winY, &mask);

      XEvent xev;
      memset(&xev, 0, sizeof(xev));
      xev.xclient.type = ClientMessage;
      xev.xclient.window = x11win->window;
      xev.xclient.message_type =
          XInternAtom(x11win->display, "_NET_WM_MOVERESIZE", False);
      xev.xclient.format = 32;
      xev.xclient.data.l[0] = rootX;
      xev.xclient.data.l[1] = rootY;
      xev.xclient.data.l[2] = 8; // _NET_WM_MOVERESIZE_MOVE
      xev.xclient.data.l[3] = Button1;
      xev.xclient.data.l[4] = 1;

      XSendEvent(x11win->display, DefaultRootWindow(x11win->display), False,
                 SubstructureRedirectMask | SubstructureNotifyMask, &xev);
      XFlush(x11win->display);
    } else {
      // GTK path: delegate to the compositor/WM on both X11 and Wayland.
      // gtk_window_begin_move_drag works natively on both — no manual grab
      // needed.
      GtkWidget *gtkWindow = GTK_WIDGET(window);
      if (!gtkWindow || !GTK_IS_WINDOW(gtkWindow)) {
        fprintf(stderr, "startWindowMove: invalid GTK window\n");
        return;
      }

      if (!gtk_widget_get_realized(gtkWindow)) {
        fprintf(stderr, "startWindowMove: window not realized\n");
        return;
      }

      GdkDisplay *display = gdk_display_get_default();
      GdkSeat *seat = display ? gdk_display_get_default_seat(display) : nullptr;
      GdkDevice *device = seat ? gdk_seat_get_pointer(seat) : nullptr;

      if (!device) {
        fprintf(stderr, "startWindowMove: no pointer device\n");
        return;
      }

      gint rootX, rootY;
      gdk_device_get_position(device, nullptr, &rootX, &rootY);

      gtk_window_begin_move_drag(GTK_WINDOW(gtkWindow), GDK_BUTTON_PRIMARY,
                                 rootX, rootY, gtk_get_current_event_time());

      printf("Window drag started\n");
      fflush(stdout);
    }
  });
}

ELECTROBUN_EXPORT void stopWindowMove() {
  // gtk_window_begin_move_drag is handled entirely by the WM/compositor —
  // there's nothing to clean up on our side.
  printf("stopWindowMove called\n");
  fflush(stdout);
}

ELECTROBUN_EXPORT void addPreloadScriptToWebView(AbstractView* abstractView, const char* scriptContent, bool forMainFrameOnly) {
    if (abstractView) {
        dispatch_sync_main_void([&]() {
            abstractView->addPreloadScriptToWebView(scriptContent);
        });
    }
}

ELECTROBUN_EXPORT void callAsyncJavaScript(const char* messageId, const char* jsString, uint32_t webviewId, uint32_t hostWebviewId, void* completionHandler) {
    std::shared_ptr<AbstractView> browserView;
    {
        std::lock_guard<std::mutex> lock(g_webviewMapMutex);
        auto it = g_webviewMap.find(webviewId);
        if (it != g_webviewMap.end()) {
            browserView = it->second;
        }
    }

    if (browserView) {
        browserView->callAsyncJavascript(
            messageId, jsString, webviewId, hostWebviewId, completionHandler);
    }
}

void* addScriptMessageHandlerWithReply(void* webView, uint32_t webviewId, const char* name, void* callback) {
    // TODO: Implement script message handler with reply
    return nullptr;
}

void testFFI(void* ptr) {
    // Test function for FFI
}

void testFFI2(void (*completionHandler)()) {
    printf("testFFI2 called from FFI! Callback pointer: %p\n", completionHandler);
    fflush(stdout);
    
    // Write to log file as well
    FILE* logFile = fopen("/tmp/tray_debug.log", "a");
    if (logFile) {
        fprintf(logFile, "testFFI2 called from FFI! Callback pointer: %p\n", completionHandler);
        fflush(logFile);
        fclose(logFile);
    }
    
    if (completionHandler) {
        completionHandler();
    }
}

ELECTROBUN_EXPORT int simpleTest() {
    printf("simpleTest called successfully\n");
    fflush(stdout);
    return 42;
}

ELECTROBUN_EXPORT const char* getUrlFromNavigationAction(void* navigationAction) {
    // TODO: Implement URL extraction from navigation action
    return nullptr;
}

ELECTROBUN_EXPORT const char* getBodyFromScriptMessage(void* message) {
    // TODO: Implement body extraction from script message
    return nullptr;
}

void invokeDecisionHandler(void* decisionHandler, uint32_t policy) {
    // TODO: Implement decision handler invocation
}

