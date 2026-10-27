// WebKitGTK implementation
class WebKitWebViewImpl : public AbstractView {
public:
    GtkWidget* webview;
    WebKitUserContentManager* manager;
    DecideNavigationCallback navigationCallback;
    WebviewEventHandler eventHandler;
    HandlePostMessage eventBridgeHandler;
    HandlePostMessage bunBridgeHandler;
    HandlePostMessage internalBridgeHandler;
    bool isSandboxed;
    std::string electrobunPreloadScript;
    std::string customPreloadScript;
    std::string partition;
    bool partitionContextReleased = false;
    bool isTransparent = false;
    bool isHidden = false;
    // 0 deny, 1 external, 2 popup
    int newWindowPolicy = 0;
    
    // Navigation state tracking
    bool lastNavigationWasBlocked = false;
    
    WebKitWebViewImpl(uint32_t webviewId,
                      GtkWidget* window,
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
                      bool sandbox,
                      bool startTransparent,
                      bool startPassthrough)
        : AbstractView(webviewId), navigationCallback(navigationCallback),
          eventHandler(webviewEventHandler), eventBridgeHandler(eventBridgeHandler),
          bunBridgeHandler(bunBridgeHandler),
          internalBridgeHandler(internalBridgeHandler), isSandboxed(sandbox),
          electrobunPreloadScript(electrobunPreloadScript ? electrobunPreloadScript : ""),
          customPreloadScript(customPreloadScript ? customPreloadScript : ""),
          partition(partitionIdentifier ? partitionIdentifier : "")
    {
        // Set initial state flags
        this->pendingStartTransparent = startTransparent;
        this->pendingStartPassthrough = startPassthrough;

        // This is the first WebKitGTK API path used during app webview
        // construction. Restore WebKitWebDriver's endpoint here: the main JSC
        // runtime is already initialized, while WebKit has not initialized yet.
        restoreWebKitAutomationInspectorServer();
        
        // Create the user content controller and manager
        manager = webkit_user_content_manager_new();
        if (!manager) {
            fprintf(stderr, "ERROR: Failed to create WebKit user content manager\n");
            throw std::runtime_error("Failed to create WebKit user content manager");
        }
        
        // Create WebKit settings
        WebKitSettings* settings = webkit_settings_new();
        if (!settings) {
            fprintf(stderr, "ERROR: Failed to create WebKit settings\n");
            throw std::runtime_error("Failed to create WebKit settings");
        }
        webkit_settings_set_enable_developer_extras(settings, TRUE);
        webkit_settings_set_enable_javascript(settings, TRUE);
        electrobun::renderAtDisplayRate(settings);
        webkit_settings_set_javascript_can_access_clipboard(settings, FALSE);
        webkit_settings_set_javascript_can_open_windows_automatically(settings, TRUE);
        webkit_settings_set_enable_back_forward_navigation_gestures(settings, TRUE);
        webkit_settings_set_enable_smooth_scrolling(settings, TRUE);

        // WebKitGTK's accelerated backing store disappears when a positioned
        // view is partially outside its toplevel. Software compositing keeps
        // the full view allocation while its GTK ancestors clip it correctly.
        if (!autoResize) {
            webkit_settings_set_hardware_acceleration_policy(
                settings,
                WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
        }
        
        // Enable media stream and WebRTC for camera/microphone access
        webkit_settings_set_enable_media_stream(settings, TRUE);
        webkit_settings_set_enable_webrtc(settings, TRUE);
        webkit_settings_set_enable_media(settings, TRUE);
        
        // Try to improve offscreen rendering without breaking stability
        // webkit_settings_set_enable_accelerated_2d_canvas is deprecated - removed

        // Get or create shared context for this partition
        WebKitWebContext* context = getContextForPartition(partition.empty() ? nullptr : partition.c_str());
        const bool isControlledByAutomation = selectWebKitAutomationContext(context);

        // Create webview with context and user content manager
        webview = GTK_WIDGET(g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "web-context", context,
            "user-content-manager", manager,
            "settings", settings,
            "is-controlled-by-automation", isControlledByAutomation ? TRUE : FALSE,
            NULL));
        if (!webview) {
            fprintf(stderr, "ERROR: Failed to create WebKit webview\n");
            throw std::runtime_error("Failed to create WebKit webview");
        }
        // Destroying the parent window destroys and finalizes this widget
        // without going through remove(). The weak pointer clears `webview`
        // when that happens so no later call touches a freed widget.
        g_object_add_weak_pointer(G_OBJECT(webview), reinterpret_cast<gpointer*>(&webview));
        g_object_set_data(
            G_OBJECT(webview),
            kWebviewIdDataKey,
            GUINT_TO_POINTER(webviewId));

        // Connect the session only after the controlled target exists. This
        // prevents WebKitWebDriver from requesting a browsing context during
        // the small gap between enabling the context and constructing the view.
        configureWebKitAutomation(
            context,
            WEBKIT_WEB_VIEW(webview),
            isControlledByAutomation);

        // A GTK size request is a minimum, not a one-time allocation. Full-size
        // host views fill their container through expand/fill, while positioned
        // child views still need their explicit initial allocation.
        gtk_widget_set_size_request(
            webview,
            autoResize ? -1 : (int)width,
            autoResize ? -1 : (int)height
        );
        
        // Check if parent window is transparent and apply transparency to webview
        GtkWidget* toplevel = gtk_widget_get_toplevel(window);
        if (GTK_IS_WINDOW(toplevel)) {
            // Check if window has RGBA visual (transparent)
            GdkScreen* screen = gtk_window_get_screen(GTK_WINDOW(toplevel));
            GdkVisual* visual = gtk_widget_get_visual(toplevel);
            if (visual && gdk_screen_get_rgba_visual(screen) == visual) {
                // Window is transparent, make webview transparent too
                GdkRGBA transparent_color = {0.0, 0.0, 0.0, 0.0};
                webkit_web_view_set_background_color(WEBKIT_WEB_VIEW(webview), &transparent_color);
                printf("GTK WebKit: Applied transparent background to webview\n");
            }
        }
        
        // Add preload scripts
        if (shouldForwardWebviewConsole(g_electrobunChannel)) {
            WebKitUserScript* consoleScript = webkit_user_script_new(
                webviewConsoleForwardingScript(),
                WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                nullptr,
                nullptr);
            webkit_user_content_manager_add_script(manager, consoleScript);
            webkit_user_script_unref(consoleScript);
            g_signal_connect(manager, "script-message-received::electrobunConsole",
                           G_CALLBACK(onConsoleMessage), this);
            webkit_user_content_manager_register_script_message_handler(manager, "electrobunConsole");
        }

        if (!this->electrobunPreloadScript.empty()) {
            addPreloadScriptToWebView(this->electrobunPreloadScript.c_str());
        }
        if (!this->customPreloadScript.empty()) {
            addPreloadScriptToWebView(this->customPreloadScript.c_str());
        }
        
        // Set up message handlers

        // eventBridge - event-only bridge (always set up for all webviews, including sandboxed)
        if (eventBridgeHandler) {
            g_signal_connect(manager, "script-message-received::eventBridge",
                           G_CALLBACK(onEventBridgeMessage), this);
            webkit_user_content_manager_register_script_message_handler(manager, "eventBridge");
        }

        // hostBridge/bunBridge aliases and internalBridge - RPC bridges (only for non-sandboxed webviews)
        if (!isSandboxed) {
            if (bunBridgeHandler) {
                g_signal_connect(manager, "script-message-received::hostBridge",
                               G_CALLBACK(onBunBridgeMessage), this);
                webkit_user_content_manager_register_script_message_handler(manager, "hostBridge");
                g_signal_connect(manager, "script-message-received::bunBridge",
                               G_CALLBACK(onBunBridgeMessage), this);
                webkit_user_content_manager_register_script_message_handler(manager, "bunBridge");
            }

            if (internalBridgeHandler) {
                g_signal_connect(manager, "script-message-received::internalBridge",
                               G_CALLBACK(onInternalBridgeMessage), this);
                webkit_user_content_manager_register_script_message_handler(manager, "internalBridge");
            }
        }
        
        // Connect navigation decision handler for both navigation callbacks AND navigation rules
        g_signal_connect(webview, "decide-policy", G_CALLBACK(onDecidePolicy), this);
        
        // Set up event handlers
        if (eventHandler) {
            g_signal_connect(webview, "load-changed", G_CALLBACK(onLoadChanged), this);
            g_signal_connect(webview, "load-failed", G_CALLBACK(onLoadFailed), this);
            g_signal_connect(webview, "web-process-terminated", G_CALLBACK(onWebProcessTerminated), this);
            g_signal_connect(webview, "create", G_CALLBACK(onCreateWebView), this);
#if WEBKIT_CHECK_VERSION(2, 34, 0)
            g_signal_connect(webview, "notify::is-web-process-responsive", G_CALLBACK(onWebProcessResponsiveChanged), this);
#endif
        }
        
        // Enable context menu (right-click menu)
        g_signal_connect(webview, "context-menu", G_CALLBACK(onContextMenu), this);
        
        // Debug scroll events
        g_signal_connect(webview, "scroll-event", G_CALLBACK(onScrollEvent), this);
        
        // Handle permission requests for getUserMedia
        g_signal_connect(webview, "permission-request", G_CALLBACK(onPermissionRequest), this);
        
        // Handle file chooser requests for <input type="file">
        g_signal_connect(webview, "run-file-chooser", G_CALLBACK(onRunFileChooser), this);

        // Handle downloads
        WebKitWebContext* defaultContext = webkit_web_view_get_context(WEBKIT_WEB_VIEW(webview));
        if (defaultContext) {
            g_signal_connect(defaultContext, "download-started", G_CALLBACK(onDownloadStarted), this);
        }

        // Note: Removed visibility override for stability
        
        this->widget = webview;
        
        // Ensure webview is visible for rendering
        gtk_widget_set_visible(webview, TRUE);
        
        // Widget will be realized after it's added to a container in addWebview()
        
        // Apply initial transparency if requested
        if (this->pendingStartTransparent) {
            this->setTransparent(true);
            this->pendingStartTransparent = false;
        }
        
        // Apply initial passthrough if requested
        if (this->pendingStartPassthrough) {
            this->setPassthrough(true);
            this->pendingStartPassthrough = false;
        }
        
        // Load URL if provided
        if (url && strlen(url) > 0) {
            loadURL(url);
        }
    }
    
    ~WebKitWebViewImpl() {
        releasePartitionContextIfNeeded();
        if (webview) {
            g_object_remove_weak_pointer(G_OBJECT(webview), reinterpret_cast<gpointer*>(&webview));
            webview = nullptr;
        }

        // Don't destroy widgets here - they should be destroyed in remove()
        // Just clean up the manager
        if (manager) {
            g_object_unref(manager);
        }
    }

    void releasePartitionContextIfNeeded() {
        if (partitionContextReleased) {
            return;
        }

        partitionContextReleased = true;
        releaseContextForPartition(partition);
    }

    void applyVisibilityState() {
        if (!webview) {
            return;
        }

        GtkWidget* wrapper = (GtkWidget*)g_object_get_data(G_OBJECT(webview), "wrapper");
        bool shouldShow = !isTransparent && !isHidden;

        if (shouldShow) {
            if (wrapper) {
                gtk_widget_show(wrapper);
            }
            gtk_widget_show(webview);
        } else {
            gtk_widget_hide(webview);
            if (wrapper) {
                gtk_widget_hide(wrapper);
            }
        }
    }
    
    void loadURL(const char* urlString) override {
        if (webview && urlString) {
            webkit_web_view_load_uri(WEBKIT_WEB_VIEW(webview), urlString);
        } else {
            fprintf(stderr, "ERROR: Cannot load URL - webview=%p, urlString=%s\n", webview, urlString ? urlString : "NULL");
        }
    }
    
    void loadHTML(const char* htmlString) override {
        if (webview && htmlString) {
            webkit_web_view_load_html(WEBKIT_WEB_VIEW(webview), htmlString, nullptr);
        } else {
            fprintf(stderr, "ERROR: Cannot load HTML - webview=%p, htmlString=%s\n", webview, htmlString ? htmlString : "NULL");
        }
    }
    
    void goBack() override {
        if (webview) {
            webkit_web_view_go_back(WEBKIT_WEB_VIEW(webview));
        }
    }
    
    void goForward() override {
        if (webview) {
            webkit_web_view_go_forward(WEBKIT_WEB_VIEW(webview));
        }
    }
    
    void reload() override {
        if (webview) {
            webkit_web_view_reload(WEBKIT_WEB_VIEW(webview));
        }
    }
    
    void remove() override {
        // Remove from global webview map first
        {
            std::lock_guard<std::mutex> lock(g_webviewMapMutex);
            g_webviewMap.erase(webviewId);
        }
        
        // Mark as removed to prevent further operations
        isRemoved = true;
        
        if (webview) {
            // Non-null means still alive (weak pointer). Stop tracking it and
            // hold a reference until the deferred destroy below.
            GtkWidget* widget_to_destroy = webview;
            g_object_remove_weak_pointer(G_OBJECT(webview), reinterpret_cast<gpointer*>(&webview));
            webview = nullptr;  // Clear our reference immediately
            
            // The parent window may still destroy this widget before the idle
            // callback runs; the reference keeps its memory valid until then.
            g_object_ref(widget_to_destroy);
            g_idle_add([](gpointer data) -> gboolean {
                GtkWidget* widget = static_cast<GtkWidget*>(data);

                // Only try to unparent if the widget still has a live parent.
                GtkWidget* parent = gtk_widget_get_parent(widget);
                if (parent && GTK_IS_CONTAINER(parent)) {
                    gtk_container_remove(GTK_CONTAINER(parent), widget);
                }

                // Idempotent if the parent's teardown already destroyed it.
                gtk_widget_destroy(widget);
                g_object_unref(widget);

                return G_SOURCE_REMOVE;
            }, widget_to_destroy);
        }

        releasePartitionContextIfNeeded();
        
    }
    
    bool canGoBack() override {
        if (webview) {
            return webkit_web_view_can_go_back(WEBKIT_WEB_VIEW(webview));
        }
        return false;
    }

    bool terminateRenderer() override {
#if WEBKIT_CHECK_VERSION(2, 34, 0)
        if (webview) {
            webkit_web_view_terminate_web_process(WEBKIT_WEB_VIEW(webview));
            return true;
        }
#endif
        return false;
    }
    
    bool canGoForward() override {
        if (webview) {
            return webkit_web_view_can_go_forward(WEBKIT_WEB_VIEW(webview));
        }
        return false;
    }
    
    void evaluateJavaScriptWithNoCompletion(const char* jsString) override {
        if (webview && jsString) {
            webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(webview), jsString, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
        }
    }
    
    void callAsyncJavascript(const char* messageId, const char* jsString, uint32_t webviewId, uint32_t hostWebviewId, void* completionHandler) override {
        // TODO: Implement async JavaScript with completion handler
        evaluateJavaScriptWithNoCompletion(jsString);
    }
    
    void addPreloadScriptToWebView(const char* jsString) override {
        if (manager && jsString) {
            WebKitUserScript* script = webkit_user_script_new(jsString, 
                                                            WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                                            nullptr, nullptr);
            webkit_user_content_manager_add_script(manager, script);
            webkit_user_script_unref(script);
        }
    }
    
    void updateCustomPreloadScript(const char* jsString) override {
        customPreloadScript = jsString ? jsString : "";
        
        // Remove existing custom scripts and add new one
        if (manager) {
            // Remove all custom scripts (we'll track them with a prefix)
            webkit_user_content_manager_remove_all_scripts(manager);
            
            // Re-add electrobun preload script
            if (!electrobunPreloadScript.empty()) {
                WebKitUserScript* script = webkit_user_script_new(electrobunPreloadScript.c_str(), 
                                                                WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                                WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                                                nullptr, nullptr);
                webkit_user_content_manager_add_script(manager, script);
                webkit_user_script_unref(script);
            }
            
            // Add updated custom script
            if (!customPreloadScript.empty()) {
                WebKitUserScript* script = webkit_user_script_new(customPreloadScript.c_str(), 
                                                                WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                                WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                                                nullptr, nullptr);
                webkit_user_content_manager_add_script(manager, script);
                webkit_user_script_unref(script);
            }
        }
    }
    
    void resize(const GdkRectangle& frame, const char* masksJson) override {
        if (webview) {
            // Check if this webview has a wrapper (OOPIF case)
            GtkWidget* wrapper = (GtkWidget*)g_object_get_data(G_OBJECT(webview), "wrapper");

            if (fullSize) {
                // Full-size views receive their allocation from GTK. Keeping
                // the request unset avoids turning every grow into a new
                // minimum window size.
                gtk_widget_set_size_request(webview, -1, -1);
            } else {
                // Positioned child views need an explicit widget allocation.
                gtk_widget_set_size_request(webview, frame.width, frame.height);
            }

            if (wrapper) {
                // For negative positions (scrolled out of view), we need to use
                // gtk_widget_set_margin_* with clamped values and offset the webview inside
                int clampedX = MAX(0, frame.x);
                int clampedY = MAX(0, frame.y);
                int offsetX = frame.x - clampedX;  // Will be negative if frame.x < 0
                int offsetY = frame.y - clampedY;  // Will be negative if frame.y < 0
                
                gtk_widget_set_size_request(wrapper, frame.width, frame.height);
                gtk_widget_set_margin_start(wrapper, clampedX);
                gtk_widget_set_margin_top(wrapper, clampedY);

                // Position the webview by the full clipped distance. GtkFixed
                // coordinates are already in logical pixels, just like the DOM
                // bounds used to construct frame.
                gtk_fixed_move(GTK_FIXED(wrapper), webview, offsetX, offsetY);

                // OOPIF positioned with coordinate adjustment
            } else if (!fullSize) {
                // For host webview, position directly with margins (can't be negative)
                gtk_widget_set_margin_start(webview, MAX(0, frame.x));
                gtk_widget_set_margin_top(webview, MAX(0, frame.y));
            }
            
            visualBounds = frame;
        }
        maskJSON = masksJson ? masksJson : "";
        
        // Store maskJSON for potential future use, but masking is not implemented for WebKit
        // See applyVisualMask() method for technical details on why WebKit masking isn't feasible
    }
    
    void applyVisualMask() override {
        // NOTE: WebKit masking is not implemented due to architectural limitations.
        // WebKit webviews in GTK have their own complex rendering and event handling
        // pipeline that doesn't support the "hole cutting" pattern used by CEF.
        // 
        // WebKit alternatives that were tried:
        // 1. X11 XShape masking - Conflicts with GTK rendering, causes visual artifacts
        // 2. GTK input shape masking - WebKit's internal event handling bypasses GTK input shapes
        // 3. CSS clipping - Would affect visual rendering, not just mouse events
        // 
        // Recommendation: Use CEF for webviews that require maskJSON functionality
        // on Linux, as CEF provides direct window management that supports masking.
    }
    
    void removeMasks() override {
        // NOTE: WebKit masking is not implemented - see applyVisualMask() for details
    }
    
    void toggleMirrorMode(bool enable) override {
        if (mirrorModeEnabled == enable) {
            return;
        }
        
        mirrorModeEnabled = enable;
        
        // With separate containers, mirror mode only affects input handling
        // No need to move webviews offscreen since OOPIFs are in non-sizing container
        if (webview) {
            if (enable) {
                // Disable input events for this webview
                gtk_widget_set_sensitive(webview, FALSE);
            } else {
                // Re-enable input events
                gtk_widget_set_sensitive(webview, TRUE);
            }
        }
    }
    
    void setHidden(bool hidden) override {
        isHidden = hidden;
        applyVisibilityState();
    }
    
    void setTransparent(bool transparent) override {
        isTransparent = transparent;
        applyVisibilityState();
    }
    
    void setPassthrough(bool enable) override {
        AbstractView::setPassthrough(enable); // Set the flag
        
        if (webview) {
            // For GTK WebKit, we use input shape to make it pass through mouse events
            GdkWindow* window = gtk_widget_get_window(webview);
            if (window) {
                if (enable) {
                    // Make the webview pass through mouse events
                    cairo_region_t* region = cairo_region_create();
                    gdk_window_input_shape_combine_region(window, region, 0, 0);
                    cairo_region_destroy(region);
                } else {
                    // Reset to receive mouse events normally
                    gdk_window_input_shape_combine_region(window, NULL, 0, 0);
                }
            }
        }
    }
    
    // Static callback functions

    static void onConsoleMessage(WebKitUserContentManager* manager, WebKitJavascriptResult* js_result, gpointer user_data) {
        WebKitWebViewImpl* impl = static_cast<WebKitWebViewImpl*>(user_data);
        if (!impl || !js_result) return;
        JSCValue* value = webkit_javascript_result_get_js_value(js_result);
        if (!value || !JSC_IS_VALUE(value) || !jsc_value_is_string(value)) return;
        gchar* message = jsc_value_to_string(value);
        if (!message) return;
        printWebviewConsoleMessage(impl->webviewId, message);
        g_free(message);
    }

    // eventBridge handler - event-only bridge for all webviews (including sandboxed)
    static void onEventBridgeMessage(WebKitUserContentManager* manager, WebKitJavascriptResult* js_result, gpointer user_data) {
        WebKitWebViewImpl* impl = static_cast<WebKitWebViewImpl*>(user_data);
        if (impl->eventBridgeHandler && js_result) {
            // Use the newer JSC API recommended by WebKit2GTK
            JSCValue* value = webkit_javascript_result_get_js_value(js_result);
            if (value && JSC_IS_VALUE(value) && jsc_value_is_string(value)) {
                gchar* str_value = jsc_value_to_string(value);
                if (str_value) {
                    impl->eventBridgeHandler(impl->webviewId, str_value);
                    g_free(str_value);
                }
            }
        }
    }

    static void onBunBridgeMessage(WebKitUserContentManager* manager, WebKitJavascriptResult* js_result, gpointer user_data) {
        WebKitWebViewImpl* impl = static_cast<WebKitWebViewImpl*>(user_data);
        if (impl->bunBridgeHandler && js_result) {
            // Use the newer JSC API recommended by WebKit2GTK
            JSCValue* value = webkit_javascript_result_get_js_value(js_result);
            if (value && JSC_IS_VALUE(value) && jsc_value_is_string(value)) {
                gchar* str_value = jsc_value_to_string(value);
                if (str_value) {
                    impl->bunBridgeHandler(impl->webviewId, str_value);
                    g_free(str_value);
                }
            }
        }
    }
    
    static void onInternalBridgeMessage(WebKitUserContentManager* manager, WebKitJavascriptResult* js_result, gpointer user_data) {
        WebKitWebViewImpl* impl = static_cast<WebKitWebViewImpl*>(user_data);
        if (impl->internalBridgeHandler && js_result) {
            // Use the newer JSC API recommended by WebKit2GTK
            JSCValue* value = webkit_javascript_result_get_js_value(js_result);
            if (value && JSC_IS_VALUE(value) && jsc_value_is_string(value)) {
                gchar* str_value = jsc_value_to_string(value);
                if (str_value) {
                    impl->internalBridgeHandler(impl->webviewId, str_value);
                    g_free(str_value);
                }
            }
        }
    }
    
    // Static debounce timestamp for ctrl+click handling
    static double lastCtrlClickTime;

    static gboolean onDecidePolicy(WebKitWebView* webview, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer user_data) {
        WebKitWebViewImpl* impl = static_cast<WebKitWebViewImpl*>(user_data);

        if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
            WebKitNavigationPolicyDecision* nav_decision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
            WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(nav_decision);
            WebKitURIRequest* request = webkit_navigation_action_get_request(action);
            const char* uri = webkit_uri_request_get_uri(request);

            // Check for Ctrl key using GDK (must use pointer device, not keyboard, for gdk_device_get_state)
            GdkDisplay* display = gdk_display_get_default();
            GdkSeat* seat = display ? gdk_display_get_default_seat(display) : nullptr;
            GdkDevice* pointer = seat ? gdk_seat_get_pointer(seat) : nullptr;
            GdkModifierType modifiers = (GdkModifierType)0;
            bool isCtrlHeld = false;

            if (pointer) {
                gdk_device_get_state(pointer, gdk_get_default_root_window(), NULL, &modifiers);
                isCtrlHeld = (modifiers & GDK_CONTROL_MASK) != 0;
            }

            if (isCtrlHeld && impl->eventHandler) {
                // Debounce: ignore ctrl+click navigations within 500ms
                double now = g_get_monotonic_time() / 1000000.0;

                if (now - lastCtrlClickTime >= 0.5) {
                    lastCtrlClickTime = now;

                    // Escape URL for JSON
                    std::string url = uri ? uri : "";
                    std::string escapedUrl;
                    for (char c : url) {
                        switch (c) {
                            case '"': escapedUrl += "\\\""; break;
                            case '\\': escapedUrl += "\\\\"; break;
                            default: escapedUrl += c; break;
                        }
                    }

                    std::string eventData = "{\"url\":\"" + escapedUrl +
                                           "\",\"isCmdClick\":true,\"modifierFlags\":0}";
                    // Use strdup to create persistent copies for the FFI callback
                    impl->eventHandler(impl->webviewId, strdup("new-window-open"), strdup(eventData.c_str()));

                    webkit_policy_decision_ignore(decision);
                    return TRUE;
                }
            }

            // Check navigation rules synchronously from native-stored rules
            std::string url = uri ? uri : "";
            bool shouldAllow = true;
            {
                std::lock_guard<std::mutex> lock(g_webviewMapMutex);
                auto it = g_webviewMap.find(impl->webviewId);
                if (it != g_webviewMap.end() && it->second != nullptr) {
                    shouldAllow = it->second->shouldAllowNavigationToURL(url);
                }
            }

            // Fire will-navigate event with allowed status
            if (impl->eventHandler) {
                // Escape URL for JSON
                std::string escapedUrl;
                for (char c : url) {
                    switch (c) {
                        case '"': escapedUrl += "\\\""; break;
                        case '\\': escapedUrl += "\\\\"; break;
                        default: escapedUrl += c; break;
                    }
                }
                std::string eventData = "{\"url\":\"" + escapedUrl + "\",\"allowed\":" +
                                       (shouldAllow ? "true" : "false") + "}";
                impl->eventHandler(impl->webviewId, strdup("will-navigate"), strdup(eventData.c_str()));
            }

            // Block navigation if not allowed
            if (!shouldAllow) {
                impl->lastNavigationWasBlocked = true;
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
            
            // Navigation is allowed, reset the flag
            impl->lastNavigationWasBlocked = false;
        }
        return FALSE;
    }
    
    // window.open and target=_blank. "popup" acts as "deny" here: WebKitGTK
    // builds a popup from the opener's page configuration, user content
    // manager included, so it would get Electrobun's preload and bridge.
    static GtkWidget* onCreateWebView(WebKitWebView* webview, WebKitNavigationAction* action, gpointer user_data) {
        (void)webview;
        WebKitWebViewImpl* impl = static_cast<WebKitWebViewImpl*>(user_data);
        const char* uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
        std::string url = uri ? uri : "";
        std::string policy = "deny";

        gchar* scheme = g_uri_parse_scheme(url.c_str());
        bool openable = scheme && (g_ascii_strcasecmp(scheme, "http") == 0 || g_ascii_strcasecmp(scheme, "https") == 0 ||
                                   g_ascii_strcasecmp(scheme, "mailto") == 0);
        g_free(scheme);

        if (impl->newWindowPolicy == 1 && openable) {
            GError* error = nullptr;
            if (g_app_info_launch_default_for_uri(url.c_str(), nullptr, &error)) {
                policy = "external";
            } else if (error) {
                g_error_free(error);
            }
        }

        if (impl->eventHandler && !impl->isRemoved) {
            std::string escapedUrl;
            for (char c : url) {
                switch (c) {
                    case '"': escapedUrl += "\\\""; break;
                    case '\\': escapedUrl += "\\\\"; break;
                    default: escapedUrl += c; break;
                }
            }
            std::string eventData = "{\"url\":\"" + escapedUrl + "\",\"isCmdClick\":false,\"modifierFlags\":0,\"policy\":\"" + policy + "\"}";
            impl->eventHandler(impl->webviewId, strdup("new-window-open"), strdup(eventData.c_str()));
        }
        return nullptr;
    }

    static void onWebProcessTerminated(WebKitWebView* webview, WebKitWebProcessTerminationReason reason, gpointer user_data) {
        (void)webview;
        WebKitWebViewImpl* impl = static_cast<WebKitWebViewImpl*>(user_data);
        if (!impl->eventHandler || impl->isRemoved) {
            return;
        }
        const char* name = "crashed";
        if (reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT) {
            name = "oom";
        }
#if WEBKIT_CHECK_VERSION(2, 34, 0)
        if (reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API) {
            name = "killed";
        }
#endif
        std::string detail = std::string("{\"reason\":\"") + name + "\"}";
        impl->eventHandler(impl->webviewId, strdup("render-process-gone"), strdup(detail.c_str()));
    }

#if WEBKIT_CHECK_VERSION(2, 34, 0)
    static void onWebProcessResponsiveChanged(GObject* object, GParamSpec* pspec, gpointer user_data) {
        (void)pspec;
        WebKitWebViewImpl* impl = static_cast<WebKitWebViewImpl*>(user_data);
        if (!impl->eventHandler || impl->isRemoved) {
            return;
        }
        bool responsive = webkit_web_view_get_is_web_process_responsive(WEBKIT_WEB_VIEW(object));
        impl->eventHandler(impl->webviewId, strdup(responsive ? "responsive" : "unresponsive"), strdup(""));
    }
#endif

    static void onLoadChanged(WebKitWebView* webview, WebKitLoadEvent event, gpointer user_data) {
        WebKitWebViewImpl* impl = static_cast<WebKitWebViewImpl*>(user_data);
        if (impl->eventHandler) {
            const char* uri = webkit_web_view_get_uri(webview);
            switch (event) {
                case WEBKIT_LOAD_STARTED:
                    impl->eventHandler(impl->webviewId, "load-started", uri);
                    break;
                case WEBKIT_LOAD_REDIRECTED:
                    impl->eventHandler(impl->webviewId, "load-redirected", uri);
                    break;
                case WEBKIT_LOAD_COMMITTED:
                    impl->eventHandler(impl->webviewId, "load-committed", uri);
                    impl->eventHandler(impl->webviewId, "did-commit-navigation", uri);
                    break;
                case WEBKIT_LOAD_FINISHED:
                    impl->eventHandler(impl->webviewId, "load-finished", uri);
                    // Only fire did-navigate event if navigation wasn't blocked
                    if (!impl->lastNavigationWasBlocked) {
                        impl->eventHandler(impl->webviewId, "did-navigate", uri);
                    }
                    break;
            }
        }
    }
    
    static gboolean onLoadFailed(WebKitWebView* webview, WebKitLoadEvent event, gchar* uri, GError* error, gpointer user_data) {
        WebKitWebViewImpl* impl = static_cast<WebKitWebViewImpl*>(user_data);
        if (impl->eventHandler) {
            impl->eventHandler(impl->webviewId, "load-failed", uri);
        }
        return FALSE;
    }
    
    static gboolean onContextMenu(WebKitWebView* webview, WebKitContextMenu* context_menu, GdkEvent* event, WebKitHitTestResult* hit_test_result, gpointer user_data) {
        // Allow the default context menu to be shown
        return FALSE;
    }
    
    static gboolean onScrollEvent(GtkWidget* widget, GdkEventScroll* event, gpointer user_data) {
        WebKitWebViewImpl* impl = static_cast<WebKitWebViewImpl*>(user_data);
        fflush(stdout);
        return FALSE; // Allow scroll to continue
    }
    
    static gboolean onPermissionRequest(WebKitWebView* webview, WebKitPermissionRequest* request, gpointer user_data) {
        WebKitWebViewImpl* impl = static_cast<WebKitWebViewImpl*>(user_data);
        
        // Check if this is a user media permission request (camera/microphone)
        if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request)) {
            std::string origin = getOriginFromPermissionRequest(request);
            
            // Check cache first
            PermissionStatus cachedStatus = getPermissionFromCache(origin, PermissionType::USER_MEDIA);
            
            if (cachedStatus == PermissionStatus::ALLOWED) {
                printf("Using cached permission: User previously allowed camera/microphone access for %s\n", origin.c_str());
                webkit_permission_request_allow(request);
                return TRUE;
            } else if (cachedStatus == PermissionStatus::DENIED) {
                printf("Using cached permission: User previously blocked camera/microphone access for %s\n", origin.c_str());
                webkit_permission_request_deny(request);
                return TRUE;
            }
            
            // No cached permission, show dialog
            printf("No cached permission found for %s, showing dialog\n", origin.c_str());
            
            // Create camera/microphone permission dialog
            std::string message = "This page wants to access your camera and/or microphone.\n\nDo you want to allow this?";
            std::string title = "Camera & Microphone Access";
            
            // Create permission dialog with custom buttons
            GtkWidget* dialog = gtk_dialog_new_with_buttons(
                title.c_str(),
                nullptr,
                GTK_DIALOG_MODAL,
                "Allow", GTK_RESPONSE_YES,
                "Block", GTK_RESPONSE_NO,
                nullptr
            );
            
            // Add message label
            GtkWidget* content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
            GtkWidget* label = gtk_label_new(message.c_str());
            gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
            gtk_widget_set_margin_top(label, 10);
            gtk_widget_set_margin_bottom(label, 10);
            gtk_widget_set_margin_start(label, 10);
            gtk_widget_set_margin_end(label, 10);
            gtk_container_add(GTK_CONTAINER(content_area), label);
            gtk_widget_show_all(dialog);
            
            gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
            
            // Show dialog and get response
            gint response = gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            
            // Handle response and cache the decision
            if (response == GTK_RESPONSE_YES) {
                webkit_permission_request_allow(request);
                cachePermission(origin, PermissionType::USER_MEDIA, PermissionStatus::ALLOWED);
                printf("User allowed camera/microphone access for %s (cached)\n", origin.c_str());
            } else {
                webkit_permission_request_deny(request);
                cachePermission(origin, PermissionType::USER_MEDIA, PermissionStatus::DENIED);
                printf("User blocked camera/microphone access for %s (cached)\n", origin.c_str());
            }
            
            return TRUE;
        }
        
        // For other permission types (geolocation, notifications, etc.)
        std::string message = "This page is requesting additional permissions.\n\nDo you want to allow this?";
        std::string title = "Permission Request";
        
        // Create permission dialog with custom buttons
        GtkWidget* dialog = gtk_dialog_new_with_buttons(
            title.c_str(),
            nullptr,
            GTK_DIALOG_MODAL,
            "Allow", GTK_RESPONSE_YES,
            "Block", GTK_RESPONSE_NO,
            nullptr
        );
        
        // Add message label
        GtkWidget* content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        GtkWidget* label = gtk_label_new(message.c_str());
        gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
        gtk_widget_set_margin_top(label, 10);
        gtk_widget_set_margin_bottom(label, 10);
        gtk_widget_set_margin_start(label, 10);
        gtk_widget_set_margin_end(label, 10);
        gtk_container_add(GTK_CONTAINER(content_area), label);
        gtk_widget_show_all(dialog);
        
        gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
        
        gint response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        
        if (response == GTK_RESPONSE_YES) {
            webkit_permission_request_allow(request);
            printf("User allowed permission request\n");
        } else {
            webkit_permission_request_deny(request);
            printf("User blocked permission request\n");
        }
        
        return TRUE;
    }
    
    static gboolean onRunFileChooser(WebKitWebView* webview, WebKitFileChooserRequest* request, gpointer user_data) {
        WebKitWebViewImpl* impl = static_cast<WebKitWebViewImpl*>(user_data);
        
        // Get file chooser details
        gboolean allowsMultipleSelection = webkit_file_chooser_request_get_select_multiple(request);
        const gchar* const* acceptedMimeTypes = webkit_file_chooser_request_get_mime_types(request);
        
        LinuxNativeFileDialog dialog(
            "Select File(s)",
            nullptr, // No parent window for now
            GTK_FILE_CHOOSER_ACTION_OPEN,
            "_Open"
        );
        if (!dialog.valid()) {
            webkit_file_chooser_request_cancel(request);
            return TRUE;
        }
        
        // Set multiple selection
        dialog.setSelectMultiple(allowsMultipleSelection);
        
        // Set up MIME type filters if provided
        if (acceptedMimeTypes && acceptedMimeTypes[0] != nullptr) {
            GtkFileFilter* filter = gtk_file_filter_new();
            gtk_file_filter_set_name(filter, "Allowed file types");
            
            for (int i = 0; acceptedMimeTypes[i] != nullptr; i++) {
                const char* mimeType = acceptedMimeTypes[i];
                
                // Add MIME type to filter
                if (strlen(mimeType) > 0) {
                    gtk_file_filter_add_mime_type(filter, mimeType);
                    
                    // Also add common patterns for known MIME types
                    if (strcmp(mimeType, "image/*") == 0) {
                        gtk_file_filter_add_pattern(filter, "*.jpg");
                        gtk_file_filter_add_pattern(filter, "*.jpeg");
                        gtk_file_filter_add_pattern(filter, "*.png");
                        gtk_file_filter_add_pattern(filter, "*.gif");
                        gtk_file_filter_add_pattern(filter, "*.bmp");
                        gtk_file_filter_add_pattern(filter, "*.webp");
                    } else if (strcmp(mimeType, "text/*") == 0) {
                        gtk_file_filter_add_pattern(filter, "*.txt");
                        gtk_file_filter_add_pattern(filter, "*.html");
                        gtk_file_filter_add_pattern(filter, "*.css");
                        gtk_file_filter_add_pattern(filter, "*.js");
                        gtk_file_filter_add_pattern(filter, "*.json");
                    }
                }
            }
            
            dialog.addFilter(filter);
        }
        
        // Always add "All files" filter as fallback
        GtkFileFilter* allFilter = gtk_file_filter_new();
        gtk_file_filter_set_name(allFilter, "All files");
        gtk_file_filter_add_pattern(allFilter, "*");
        dialog.addFilter(allFilter);
        
        // Run the dialog and handle the response
        gint response = dialog.run();
        
        if (response == GTK_RESPONSE_ACCEPT) {
            const auto filenames = dialog.selectedPaths();
            std::vector<const gchar*> files;
            files.reserve(filenames.size() + 1);
            for (const auto& filename : filenames) {
                files.push_back(filename.c_str());
            }
            files.push_back(nullptr);
            webkit_file_chooser_request_select_files(request, files.data());
        } else {
            webkit_file_chooser_request_cancel(request);
        }
        return TRUE; // We handled the request
    }

    // Download handling callbacks
    static gboolean onDecideDestination(WebKitDownload* download, gchar* suggested_filename, gpointer user_data) {
        WebKitWebViewImpl* impl = static_cast<WebKitWebViewImpl*>(user_data);
        fprintf(stderr, "WebKit2GTK: Deciding destination for download: %s\n", suggested_filename);

        // Get the Downloads directory
        const gchar* downloadsDir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
        if (!downloadsDir) {
            // Fallback to home directory + Downloads
            downloadsDir = g_get_home_dir();
            if (downloadsDir) {
                gchar* fallbackDir = g_build_filename(downloadsDir, "Downloads", nullptr);
                downloadsDir = fallbackDir;
            }
        }

        if (downloadsDir) {
            gchar* destinationPath = g_build_filename(downloadsDir, suggested_filename, nullptr);

            // Handle duplicate filenames
            gchar* basePath = g_strdup(destinationPath);
            gchar* extension = nullptr;
            gchar* dot = g_strrstr(basePath, ".");
            if (dot && dot != basePath) {
                // Check if dot is in filename (not in path)
                gchar* lastSlash = g_strrstr(basePath, "/");
                if (!lastSlash || dot > lastSlash) {
                    extension = g_strdup(dot);
                    *dot = '\0';
                }
            }

            int counter = 1;
            while (g_file_test(destinationPath, G_FILE_TEST_EXISTS)) {
                g_free(destinationPath);
                if (extension) {
                    destinationPath = g_strdup_printf("%s (%d)%s", basePath, counter, extension);
                } else {
                    destinationPath = g_strdup_printf("%s (%d)", basePath, counter);
                }
                counter++;
            }

            g_free(basePath);
            g_free(extension);

            // Convert path to URI
            gchar* destinationUri = g_filename_to_uri(destinationPath, nullptr, nullptr);
            if (destinationUri) {
                fprintf(stderr, "WebKit2GTK: Downloading to %s\n", destinationPath);
                webkit_download_set_destination(download, destinationUri);
                g_free(destinationUri);
            } else {
                fprintf(stderr, "WebKit2GTK ERROR: Could not convert path to URI: %s\n", destinationPath);
            }

            g_free(destinationPath);
        } else {
            fprintf(stderr, "WebKit2GTK ERROR: Could not determine Downloads directory\n");
        }

        return TRUE; // We handled the signal
    }

    static void onDownloadFinished(WebKitDownload* download, gpointer user_data) {
        const gchar* destination = webkit_download_get_destination(download);
        fprintf(stderr, "WebKit2GTK: Download finished - %s\n", destination ? destination : "unknown");
    }

    static void onDownloadFailed(WebKitDownload* download, GError* error, gpointer user_data) {
        fprintf(stderr, "WebKit2GTK ERROR: Download failed - %s\n", error ? error->message : "unknown error");
    }

    static void onDownloadStarted(WebKitWebContext* context, WebKitDownload* download, gpointer user_data) {
        WebKitWebViewImpl* impl = static_cast<WebKitWebViewImpl*>(user_data);
        WebKitURIRequest* request = webkit_download_get_request(download);
        const gchar* uri = webkit_uri_request_get_uri(request);
        fprintf(stderr, "WebKit2GTK: Download started for %s\n", uri);

        // Connect to decide-destination signal
        g_signal_connect(download, "decide-destination", G_CALLBACK(onDecideDestination), user_data);

        // Connect to finished/failed signals for logging
        g_signal_connect(download, "finished", G_CALLBACK(onDownloadFinished), user_data);
        g_signal_connect(download, "failed", G_CALLBACK(onDownloadFailed), user_data);
    }

    void findInPage(const char* searchText, bool forward, bool matchCase) override {
        if (!WEBKIT_IS_WEB_VIEW(webview)) return;

        WebKitFindController* findController = webkit_web_view_get_find_controller(WEBKIT_WEB_VIEW(webview));
        if (!findController) return;

        if (!searchText || strlen(searchText) == 0) {
            webkit_find_controller_search_finish(findController);
            return;
        }

        guint32 findOptions = WEBKIT_FIND_OPTIONS_WRAP_AROUND;
        if (!matchCase) {
            findOptions |= WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE;
        }
        if (!forward) {
            findOptions |= WEBKIT_FIND_OPTIONS_BACKWARDS;
        }

        webkit_find_controller_search(findController, searchText, findOptions, G_MAXUINT);
    }

    void stopFindInPage() override {
        if (!WEBKIT_IS_WEB_VIEW(webview)) return;

        WebKitFindController* findController = webkit_web_view_get_find_controller(WEBKIT_WEB_VIEW(webview));
        if (findController) {
            webkit_find_controller_search_finish(findController);
        }
    }

    void openDevTools() override {
        if (!WEBKIT_IS_WEB_VIEW(webview)) return;
        
        WebKitWebInspector* inspector = webkit_web_view_get_inspector(WEBKIT_WEB_VIEW(webview));
        if (inspector) {
            webkit_web_inspector_show(inspector);
        }
    }

    void closeDevTools() override {
        if (!WEBKIT_IS_WEB_VIEW(webview)) return;
        
        WebKitWebInspector* inspector = webkit_web_view_get_inspector(WEBKIT_WEB_VIEW(webview));
        if (inspector) {
            webkit_web_inspector_close(inspector);
        }
    }

    void toggleDevTools() override {
        if (!WEBKIT_IS_WEB_VIEW(webview)) return;
        
        WebKitWebInspector* inspector = webkit_web_view_get_inspector(WEBKIT_WEB_VIEW(webview));
        if (inspector) {
            if (webkit_web_inspector_is_attached(inspector)) {
                webkit_web_inspector_close(inspector);
            } else {
                webkit_web_inspector_show(inspector);
            }
        }
    }

};

