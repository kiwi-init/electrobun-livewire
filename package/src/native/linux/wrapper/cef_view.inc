// Create a CefRequestContext for partition isolation (CEF)
// Platform implementation for partition_context.h — builds the on-disk
// cache_path for a persistent partition under $HOME/.cache, creating any
// missing parent directories.
namespace electrobun {
std::string buildAndEnsurePartitionCachePath(const std::string& partitionName) {
    char* home = getenv("HOME");
    std::string basePath = home ? std::string(home) + "/.cache" : "/tmp";
    std::string cachePath = buildCEFPartitionPath(
        basePath, g_electrobunIdentifier, g_electrobunChannel, "CEF", partitionName);
    g_mkdir_with_parents(cachePath.c_str(), 0755);
    return cachePath;
}
} // namespace electrobun

CefRefPtr<CefRequestContext> CreateRequestContextForPartition(const char* partitionIdentifier,
                                                               uint32_t webviewId) {
    static CefRefPtr<ViewsSchemeHandlerFactory> schemeFactory = new ViewsSchemeHandlerFactory();
    return electrobun::getOrCreateRequestContextForPartition(
        partitionIdentifier, webviewId, schemeFactory);
}

// CEF WebView implementation
class CEFWebViewImpl : public AbstractView {
public:
    CefRefPtr<CefBrowser> browser;
    CefRefPtr<ElectrobunClient> client;
    CefFindSession findSession;
    DecideNavigationCallback navigationCallback;
    WebviewEventHandler eventHandler;
    HandlePostMessage eventBridgeHandler;
    HandlePostMessage bunBridgeHandler;
    HandlePostMessage internalBridgeHandler;
    bool isSandboxed;
    std::string electrobunPreloadScript;
    std::string customPreloadScript;
    std::string partition;
    
    // Public/browser-tag geometry remains in logical pixels until the X11
    // boundary. Full-size views are the explicit exception: ConfigureNotify
    // already reports physical pixels.
    LogicalRect logicalBounds = {};
    LogicalRect pendingFrame = {};
    bool hasPendingFrame = false;

    // Page zoom factor last requested through the API; 0 = none. Chromium
    // zoom is per-host and resets when a navigation commits, while the public
    // API (and WebKitGTK) is per-view, so it is re-applied after each commit.
    double requestedPageZoom = 0;

    void applyRequestedPageZoom() {
        if (browser && requestedPageZoom > 0) {
            // Chromium zoom levels are logarithmic: factor = 1.2^level.
            browser->GetHost()->SetZoomLevel(std::log(requestedPageZoom) / std::log(1.2));
        }
    }
    
    // For popup reparenting approach
    unsigned long parentXWindow = 0;
    Display* parentXDisplay = nullptr;
    CefRect targetBounds;
    double lastAppliedScaleFactor = 1.0;
    
    // For deferred browser creation
    GtkWidget* gtkWindow = nullptr;
    std::string deferredUrl;
    double deferredX = 0, deferredY = 0, deferredWidth = 0, deferredHeight = 0;
    
    // Track if parent window is transparent
    bool parentTransparent = false;
    
    // Transparent OSR input is forwarded by the shared X11 event source.
    uint32_t osr_window_id_ = 0;
    
    CEFWebViewImpl(uint32_t webviewId,
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
        // This must be known before SetAsChild computes its initial bounds.
        this->fullSize = autoResize;

        // Set initial state flags
        this->pendingStartTransparent = startTransparent;
        this->pendingStartPassthrough = startPassthrough;
        
        // Initialize CEF if not already done
        if (!g_cefInitialized && !initializeCEF()) {
            creationFailed = true;
            return;
        }
        
        createCEFBrowser(window, url, x, y, width, height);
        
        // Browser creation happens immediately in createCEFBrowser
    }
    
    ~CEFWebViewImpl() {
        // Native window teardown can release the last shared_ptr without going
        // through remove(). Detach every callback that captures this before the
        // owner storage becomes invalid.
        if (client) {
            client->DetachOwnerCallbacks();
        }
        if (!isRemoved && browser) {
            browser->GetHost()->CloseBrowser(false);
            browser = nullptr;
        }

        if (osr_window_id_) {
            registerOSRClientForWindow(osr_window_id_, nullptr);
            osr_window_id_ = 0;
        }
    }

    bool queryParentPhysicalBounds(LogicalRect& bounds) const {
        if (!parentXDisplay || !parentXWindow) return false;
        XWindowAttributes attributes = {};
        if (!XGetWindowAttributes(parentXDisplay, parentXWindow, &attributes)) {
            return false;
        }
        bounds = {
            0.0,
            0.0,
            static_cast<double>(attributes.width),
            static_cast<double>(attributes.height),
        };
        return true;
    }

    double parentDeviceScaleFactor() const {
        if (!parentXDisplay || !parentXWindow) return 1.0;

        XWindowAttributes attributes = {};
        int rootX = 0;
        int rootY = 0;
        Window child = 0;
        CefRefPtr<CefDisplay> display;
        if (XGetWindowAttributes(parentXDisplay, parentXWindow, &attributes) &&
            XTranslateCoordinates(
                parentXDisplay,
                parentXWindow,
                DefaultRootWindow(parentXDisplay),
                0,
                0,
                &rootX,
                &rootY,
                &child)) {
            // CEF display bounds are DIPs by default; `true` declares these
            // root coordinates as physical pixels, including negative origins.
            const CefRect physicalParentBounds(
                rootX, rootY, attributes.width, attributes.height);
            display = CefDisplay::GetDisplayMatchingBounds(
                physicalParentBounds, true);
        }
        if (!display) display = CefDisplay::GetPrimaryDisplay();
        return normalizeLinuxScaleFactor(
            display ? display->GetDeviceScaleFactor() : 1.0);
    }

    double x11BoundsScaleFactor() const {
        return fullSize ? 1.0 : parentDeviceScaleFactor();
    }

    LogicalRect clippedLogicalBounds(const LogicalRect& bounds) const {
        if (fullSize) {
            return {
                bounds.x,
                bounds.y,
                std::max(0.0, bounds.width),
                std::max(0.0, bounds.height),
            };
        }
        return clipLinuxLogicalRectToOrigin(bounds);
    }

    LinuxPhysicalRect toX11BoundsRect(const LogicalRect& bounds) const {
        return logicalToLinuxPhysicalRect(
            clippedLogicalBounds(bounds), x11BoundsScaleFactor());
    }

    void rememberLogicalBounds(const LogicalRect& bounds) {
        logicalBounds = bounds;
        const LogicalRect clipped = clippedLogicalBounds(bounds);
        visualBounds = {
            static_cast<int>(clipped.x),
            static_cast<int>(clipped.y),
            static_cast<int>(clipped.width),
            static_cast<int>(clipped.height),
        };
    }
    
    void createCEFBrowser(GtkWidget* window, const char* url, double x, double y, double width, double height) {
        // NO GTK widget needed - CEF will be a direct child of the X11 window
        this->widget = nullptr;
        
        // window parameter is actually X11Window* cast as GtkWidget*
        X11Window* x11win = reinterpret_cast<X11Window*>(window);
        if (!x11win || !x11win->window) {
            printf("CEF: ERROR - X11 window is null or invalid\n");
            creationFailed = true;
            return;
        }
        
        // Store the parent X11 window handle for later window association
        this->parentXWindow = x11win->window;
        this->parentXDisplay = x11win->display;

        LogicalRect initialBounds = {x, y, width, height};
        if (fullSize) {
            // A WM may have replaced the requested startup size before CEF's
            // asynchronous child creation reaches this point.
            queryParentPhysicalBounds(initialBounds);
        }
        rememberLogicalBounds(initialBounds);
        
        // Store the parameters
        this->deferredUrl = url && url[0] ? url : "https://www.wikipedia.org";
        this->deferredX = initialBounds.x;
        this->deferredY = initialBounds.y;
        this->deferredWidth = initialBounds.width;
        this->deferredHeight = initialBounds.height;
        
        // Create CEF browser immediately as child of X11 window
        CefWindowInfo window_info;
        // Alloy style, matching Windows and macOS. Chrome style hosts an
        // external-parent child browser through CEF's Views
        // ChildWindowDelegate, which on Linux keeps a dangling platform
        // delegate after the browser closes (it is only cleared on Windows) and
        // crashes in OnWindowBoundsChanged when the child window then receives
        // a geometry update. DevTools windows keep Chrome style.
        window_info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
        
        const LinuxPhysicalRect physicalBounds = toX11BoundsRect(initialBounds);
        CefRect cef_rect(
            physicalBounds.x,
            physicalBounds.y,
            std::max(1, physicalBounds.width),
            std::max(1, physicalBounds.height));
        
        // Use SetAsChild with the X11 window
        window_info.SetAsChild(x11win->window, cef_rect);
        
        // Ensure X11 is synced before creating browser
        Display* display = parentXDisplay;
        if (display) {
            XSync(display, False);
        }

        // For transparent windows, use windowless/OSR mode like macOS and Windows
        if (x11win->transparent) {
            // Use windowless (off-screen) rendering for transparency
            window_info.SetAsWindowless(x11win->window);
            // printf("CEF: Using windowless (OSR) mode for transparency\n");
        }
        
        
        CefBrowserSettings browser_settings;
        
        // Check if the parent window is transparent
        if (parentXWindow && x11win->transparent) {
            // For OSR transparent windows, use fully transparent background
            browser_settings.background_color = CefColorSetARGB(0, 0, 0, 0); // Fully transparent
            this->parentTransparent = true;
            // printf("CEF: Using transparent background for OSR mode\n");
        }
        
        // Create client
        client = new ElectrobunClient(
            webviewId,
            eventBridgeHandler,
            bunBridgeHandler,
            internalBridgeHandler,
            eventHandler,
            navigationCallback,
            nullptr,  // No GTK window needed
            isSandboxed
        );
        
        // Set parent window handle for proper CEF window parenting
        client->SetParentWindowHandle(x11win->window, x11win->display);
        
        // Enable OSR for transparent windows
        if (x11win->transparent) {
            client->EnableOSR(
                x11win->window,
                x11win->display,
                std::max(1, physicalBounds.width),
                std::max(1, physicalBounds.height));
        }
        
        // Set up browser creation callback to notify CEFWebViewImpl when browser is ready
        client->SetBrowserCreatedCallback([this, x11win](CefRefPtr<CefBrowser> browser) {
            this->findSession.reset();
            this->browser = browser;
            
            CefWindowHandle handle = browser->GetHost()->GetWindowHandle();
            
            // Re-read full-size bounds after WM placement. Nested views retain
            // their latest fractional public frame. This callback runs after
            // OnAfterCreated's reparent step, so the final position is not reset.
            LogicalRect finalBounds = logicalBounds;
            if (fullSize) {
                queryParentPhysicalBounds(finalBounds);
            } else if (hasPendingFrame) {
                finalBounds = pendingFrame;
            }
            hasPendingFrame = false;
            resizeLogical(finalBounds, maskJSON.c_str());

            this->applyRequestedPageZoom();
            
            // Apply deferred initial transparent/passthrough state now that browser is ready
            // Don't apply transparency immediately - wait for page load to complete
            // (CEF navigation events will re-map the window, undoing our transparency)
            if (this->pendingStartPassthrough) {
                this->setPassthrough(true);
                this->pendingStartPassthrough = false;
                
                // Double-check that passthrough was applied
                CefWindowHandle handle = browser->GetHost()->GetWindowHandle();
                if (!handle) {
                    this->pendingStartPassthrough = true; // Keep pending
                }
            }
            
            // For transparent OSR windows, setup event handling
            if (this->parentTransparent && x11win && x11win->transparent) {
                this->osr_window_id_ = x11win->windowId;
                registerOSRClientForWindow(x11win->windowId, this->client);
                printf("CEF: Transparent window input handling enabled for window %u\n", x11win->windowId);
            }

            // Creation is asynchronous: loadURL/loadHTML may have replaced the
            // initial URL before this callback. Start only the latest request.
            browser->GetMainFrame()->LoadURL(CefString(this->deferredUrl));
            this->deferredUrl.clear();
        });
        
        // Set up browser close callback to clear browser reference
        client->SetBrowserCloseCallback([this]() {
            // Don't acquire the mutex here - OnBeforeClose already has it
            this->findSession.reset();
            this->browser = nullptr;
            printf("CEF: Browser reference cleared in CEFWebViewImpl\n");
        });
        
        // Set up load end callback for deferred transparency and passthrough application
        client->SetLoadCommitCallback([this]() {
            this->applyRequestedPageZoom();
        });

        client->SetLoadEndCallback([this]() {
            this->applyRequestedPageZoom();
            if (this->pendingStartTransparent) {
                this->setTransparent(true);
                this->pendingStartTransparent = false;
            }
            if (this->pendingStartPassthrough) {
                this->setPassthrough(true);
                this->pendingStartPassthrough = false;
            }
            
            // Also re-apply passthrough if it was already set (in case it got reset)
            if (this->isMousePassthroughEnabled && !this->pendingStartPassthrough) {
                this->setPassthrough(true);
            }
        });
        
        // Add preload scripts to the client
        if (!electrobunPreloadScript.empty()) {
            client->AddPreloadScript(electrobunPreloadScript);
        }
        if (!customPreloadScript.empty()) {
            client->UpdateCustomPreloadScript(customPreloadScript);
        }
        
        // Create the browser with partition-specific request context
        CefRefPtr<CefRequestContext> requestContext = CreateRequestContextForPartition(partition.c_str(), webviewId);
        if (!requestContext) {
            printf("CEF: ERROR - CreateRequestContextForPartition returned null\n");
            creationFailed = true;
            return;
        }

        // Pass sandbox flag to renderer process via extra_info
        CefRefPtr<CefDictionaryValue> extra_info = CefDictionaryValue::Create();
        extra_info->SetBool("sandbox", isSandboxed);

        client->MarkInitialBrowserCreationPending();
        // Navigate in the creation callback so an initial URL scheduled by CEF
        // cannot overwrite a loadURL request made while creation was pending.
        bool create_result = CefBrowserHost::CreateBrowser(window_info, client, "", browser_settings, extra_info, requestContext);
        
        if (!create_result) {
            client->ResolveInitialBrowserCreationPending();
            printf("CEF: CreateBrowser returned false\n");
            creationFailed = true;
        } else {
            // Add this webview to the X11 window's child list.
            // The actual handle is available asynchronously in OnAfterCreated.
            x11win->childWindows.push_back(0);
        }
    }
    
    // Removed createCEFBrowserInX11Window and createCEFBrowserDeferred - functionality moved to createCEFBrowser
    
    void syncCEFPositionWithFrame(const LogicalRect& frame) {
        CefRefPtr<CefBrowser> browserRef;
        {
            std::lock_guard<std::mutex> lock(g_cefBrowserMutex);
            browserRef = browser;
        }
        if (!browserRef || !parentXDisplay) return;

        CefRefPtr<CefBrowserHost> host = browserRef->GetHost();
        if (!host) return;
        const CefWindowHandle cefWindow = host->GetWindowHandle();
        if (!cefWindow) return;

        XWindowAttributes current = {};
        if (!XGetWindowAttributes(
                parentXDisplay, static_cast<Window>(cefWindow), &current)) {
            // Asynchronous creation may not have installed the child XID yet;
            // pendingFrame is retried after OnAfterCreated/reparenting.
            return;
        }

        const LinuxPhysicalRect physicalBounds = toX11BoundsRect(frame);
        const int physicalWidth = std::max(1, physicalBounds.width);
        const int physicalHeight = std::max(1, physicalBounds.height);
        const bool moved =
            current.x != physicalBounds.x || current.y != physicalBounds.y;
        const bool resized =
            current.width != physicalWidth || current.height != physicalHeight;

        if (moved || resized) {
            XMoveResizeWindow(
                parentXDisplay,
                static_cast<Window>(cefWindow),
                physicalBounds.x,
                physicalBounds.y,
                physicalWidth,
                physicalHeight);
            if (resized) {
                // CEF may observe the child from a separate X connection.
                // Complete the server-side resize before notifying its host.
                XSync(parentXDisplay, False);
            } else {
                XFlush(parentXDisplay);
            }
        }

        lastAppliedScaleFactor = x11BoundsScaleFactor();
        if (resized) {
            // Notify only after X11 has accepted the new physical size. Pure
            // moves must not trigger a layout/repaint storm.
            host->WasResized();
        }
    }
    
    // Event handling will be implemented separately after global declarations
    
    void syncCEFPositionWithWidget() {
        if (!browser || !widget) {
            printf("CEF: Cannot sync - browser=%p, widget=%p\n", browser.get(), widget);
            return;
        }
        
        // Get the GTK widget's position relative to the parent window
        GtkWidget* parentWidget = gtk_widget_get_parent(widget);
        if (!parentWidget) {
            printf("CEF: Cannot sync - no parent widget\n");
            return;
        }
        
        // Get widget size first
        GtkAllocation allocation;
        gtk_widget_get_allocation(widget, &allocation);
        
        // Try to get absolute position on screen
        gint absX, absY;
        gdk_window_get_origin(gtk_widget_get_window(gtk_widget_get_toplevel(widget)), &absX, &absY);
        
        // Use the allocation position directly
        int finalX = allocation.x;
        int finalY = allocation.y;
        int finalWidth = MAX(allocation.width, 1);
        int finalHeight = MAX(allocation.height, 1);
        
        // Move the CEF browser window to match the widget position
        CefWindowHandle cefWindow = browser->GetHost()->GetWindowHandle();
        
        if (cefWindow && parentXDisplay) {
            Display* display = parentXDisplay;
            
            // Validate CEF window handle before using it
            XWindowAttributes attrs;
            int result = XGetWindowAttributes(display, cefWindow, &attrs);
            
            if (result == 0) {
                // Skip positioning for now - this is likely during initial creation
                return;
            }
            
            XMoveResizeWindow(display, cefWindow, finalX, finalY, finalWidth, finalHeight);
            XFlush(display);
            

        } else {
            printf("CEF: [SYNC_WIDGET] No CEF window handle available\n");
        }
    }
    
    void retryPositioning() {
        if (browser && widget) {
            // Try to sync position with the current widget allocation
            GtkAllocation allocation;
            gtk_widget_get_allocation(widget, &allocation);
            
            LogicalRect frame = {
                static_cast<double>(allocation.x),
                static_cast<double>(allocation.y),
                static_cast<double>(allocation.width),
                static_cast<double>(allocation.height),
            };
            
            syncCEFPositionWithFrame(frame);
        }
    }
    
    void loadURL(const char* urlString) override {
        OperationGuard guard;
        if (!guard.isValid() || !urlString) return;
        
        std::lock_guard<std::mutex> lock(g_cefBrowserMutex);
        if (browser) {
            browser->GetMainFrame()->LoadURL(CefString(urlString));
        } else {
            deferredUrl = urlString;
        }
    }
    
    void loadHTML(const char* htmlString) override {
        if (!htmlString) return;
        // Use the same pending-navigation path as loadURL during creation.
        std::string dataUri = "data:text/html;charset=utf-8,";
        dataUri += htmlString;
        loadURL(dataUri.c_str());
    }
    
    void goBack() override {
        OperationGuard guard;
        if (!guard.isValid()) return;
        
        std::lock_guard<std::mutex> lock(g_cefBrowserMutex);
        if (browser) {
            browser->GoBack();
        }
    }
    
    void goForward() override {
        OperationGuard guard;
        if (!guard.isValid()) return;
        
        std::lock_guard<std::mutex> lock(g_cefBrowserMutex);
        if (browser) {
            browser->GoForward();
        }
    }
    
    void reload() override {
        OperationGuard guard;
        if (!guard.isValid()) return;
        
        std::lock_guard<std::mutex> lock(g_cefBrowserMutex);
        if (browser) {
            browser->Reload();
        }
    }
    
    void removeInternal(bool parentIsBeingDestroyed) {
        if (isRemoved) return;
        // Teardown must remain valid after stopEventLoop sets g_shuttingDown;
        // OperationGuard intentionally rejects new work in that state, but a
        // parent XID may not be destroyed until paint/input callbacks detach.
        isRemoved = true;

        // Remove from global webview map first
        {
            std::lock_guard<std::mutex> lock(g_webviewMapMutex);
            g_webviewMap.erase(webviewId);
        }

        CefRefPtr<CefBrowser> browser_to_close;
        GtkWidget* widget_to_destroy = nullptr;
        
        {
            std::lock_guard<std::mutex> lock(g_cefBrowserMutex);
            
            browser_to_close = browser;
            widget_to_destroy = widget;
            
            // Clear our references immediately
            browser = nullptr;
            widget = nullptr;
        }

        // The client may outlive this view while asynchronous creation, load, or
        // close callbacks are still pending. It also closes a browser that is
        // created after this view has already been removed.
        if (client) {
            client->DetachOwnerCallbacks();
        }

        // Parent destruction and global shutdown cannot leave a close request
        // queued behind the XDestroyWindow that invalidates the child XID.
        if (browser_to_close) {
            if (parentIsBeingDestroyed || g_shuttingDown.load()) {
                browser_to_close->GetHost()->CloseBrowser(true);
            } else {
                g_idle_add([](gpointer data) -> gboolean {
                    auto* browser_ref =
                        static_cast<CefRefPtr<CefBrowser>*>(data);
                    if (*browser_ref) {
                        (*browser_ref)->GetHost()->CloseBrowser(false);
                    }
                    delete browser_ref;
                    return G_SOURCE_REMOVE;
                }, new CefRefPtr<CefBrowser>(browser_to_close));
            }
        }
        
        // Destroy widget asynchronously. The parent window may destroy this
        // widget via gtk_widget_destroy cascade first; the reference keeps its
        // memory valid until this callback runs (destroy is then idempotent).
        if (widget_to_destroy) {
            g_object_ref(widget_to_destroy);
            g_idle_add([](gpointer data) -> gboolean {
                GtkWidget* widget = static_cast<GtkWidget*>(data);
                gtk_widget_hide(widget);
                gtk_widget_destroy(widget);
                g_object_unref(widget);
                return G_SOURCE_REMOVE;
            }, widget_to_destroy);
        }
    }

    void removeForParentDestruction() {
        removeInternal(true);
    }

    void remove() override {
        removeInternal(false);
    }
    
    bool canGoBack() override {
        OperationGuard guard;
        if (!guard.isValid()) return false;
        
        std::lock_guard<std::mutex> lock(g_cefBrowserMutex);
        return browser ? browser->CanGoBack() : false;
    }
    
    bool canGoForward() override {
        OperationGuard guard;
        if (!guard.isValid()) return false;
        
        std::lock_guard<std::mutex> lock(g_cefBrowserMutex);
        return browser ? browser->CanGoForward() : false;
    }
    
    void evaluateJavaScriptWithNoCompletion(const char* jsString) override {
        OperationGuard guard;
        if (!guard.isValid() || !jsString || strlen(jsString) == 0) {
            if (jsString && strlen(jsString) == 0) {
                printf("CEF: evaluateJavaScriptWithNoCompletion called with empty jsString\n");
            }
            return;
        }
        
        // Get browser reference without holding lock for JS execution
        CefRefPtr<CefBrowser> browserRef;
        {
            std::lock_guard<std::mutex> lock(g_cefBrowserMutex);
            browserRef = browser;
        }
        
        if (!browserRef) {
            return;
        }
        
        CefRefPtr<CefFrame> frame = browserRef->GetMainFrame();
        if (!frame) {
            printf("CEF: evaluateJavaScriptWithNoCompletion - GetMainFrame returned NULL\n");
            return;
        }
        
        frame->ExecuteJavaScript(CefString(jsString), CefString(""), 0);
    }
    
    void callAsyncJavascript(const char* messageId, const char* jsString, uint32_t webviewId, uint32_t hostWebviewId, void* completionHandler) override {
        // TODO: Implement async javascript execution with completion handler
        evaluateJavaScriptWithNoCompletion(jsString);
    }
    
    void addPreloadScriptToWebView(const char* jsString) override {
        electrobunPreloadScript = jsString ? jsString : "";
        if (client) {
            client->AddPreloadScript(electrobunPreloadScript);
        }
    }
    
    void updateCustomPreloadScript(const char* jsString) override {
        customPreloadScript = jsString ? jsString : "";
        if (client) {
            client->UpdateCustomPreloadScript(customPreloadScript);
        }
    }
    
    void resize(const GdkRectangle& frame, const char* masksJson) override {
        resizeLogical(
            {
                static_cast<double>(frame.x),
                static_cast<double>(frame.y),
                static_cast<double>(frame.width),
                static_cast<double>(frame.height),
            },
            masksJson);
    }

    void resizeLogical(const LogicalRect& frame, const char* masksJson) override {
        OperationGuard guard;
        if (!guard.isValid()) return;

        // Internal relayouts may pass maskJSON.c_str(); copy before assigning
        // the destination string so that aliased storage cannot be invalidated.
        const std::string nextMasks = masksJson ? masksJson : "";
        rememberLogicalBounds(frame);
        maskJSON = nextMasks;

        CefRefPtr<CefBrowser> browserRef;
        {
            std::lock_guard<std::mutex> lock(g_cefBrowserMutex);
            browserRef = browser;
        }
        if (!browserRef) {
            pendingFrame = frame;
            hasPendingFrame = true;
            return;
        }

        if (parentTransparent && client) {
            const LinuxPhysicalRect physicalBounds = toX11BoundsRect(frame);
            client->UpdateOSRSize(
                std::max(1, physicalBounds.width),
                std::max(1, physicalBounds.height));
            lastAppliedScaleFactor = x11BoundsScaleFactor();
            browserRef->GetHost()->WasResized();
            browserRef->GetHost()->Invalidate(PET_VIEW);
        } else {
            syncCEFPositionWithFrame(frame);
        }

        // Apply visual mask if maskJSON is provided
        // Check if masksJson is nullptr, empty, or just "[]" (empty array)
        if (!maskJSON.empty() && maskJSON != "[]") {
            applyVisualMask();
        } else {
            // If no masks, remove any existing masks
            removeMasks();
        }
    }

    void handleParentGeometryChanged(
        bool moved,
        bool resized,
        int width,
        int height
    ) {
        CefRefPtr<CefBrowser> browserRef;
        {
            std::lock_guard<std::mutex> lock(g_cefBrowserMutex);
            browserRef = browser;
        }
        if (!browserRef) return;

        CefRefPtr<CefBrowserHost> host = browserRef->GetHost();
        if (!host) return;

        if (moved || resized) {
            // Update CEF's display/DPR model before any scale-driven resize.
            host->NotifyScreenInfoChanged();
            host->NotifyMoveOrResizeStarted();
        }

        if (fullSize && resized) {
            LogicalRect actualBounds = {
                0.0, 0.0, static_cast<double>(width), static_cast<double>(height)};
            queryParentPhysicalBounds(actualBounds);
            resizeLogical(actualBounds, maskJSON.c_str());
        } else if (!fullSize) {
            const double nextScaleFactor = parentDeviceScaleFactor();
            const bool scaleChanged =
                std::abs(nextScaleFactor - lastAppliedScaleFactor) > 0.0001;
            if (scaleChanged) {
                resizeLogical(logicalBounds, maskJSON.c_str());
            }
        }

    }
    
    void applyVisualMask() override {
        
        if (!browser || maskJSON.empty()) {
            return;
        }
        
        // Parse mask rectangles from JSON
        std::vector<MaskRect> masks = parseMaskJson(maskJSON);
        if (masks.empty()) {
            return;
        }
        
        // Get the CEF browser's X11 window
        CefWindowHandle window = browser->GetHost()->GetWindowHandle();
        if (!window) {
            return;
        }
        
        Display* display = parentXDisplay;
        if (!display) return;

        const LogicalRect clippedBounds = clippedLogicalBounds(logicalBounds);
        const double scaleFactor = x11BoundsScaleFactor();
        
        // Convert local mask edges against the original view origin, then
        // express them relative to the clipped child origin. This keeps holes
        // aligned at fractional scale and while a child is scrolled offscreen.
        std::vector<XRectangle> xrects;
        for (const auto& mask : masks) {
            const LinuxPhysicalRect physicalMask =
                logicalSubrectToLinuxPhysicalRect(
                    clippedBounds.x,
                    clippedBounds.y,
                    logicalBounds.x + mask.x - clippedBounds.x,
                    logicalBounds.y + mask.y - clippedBounds.y,
                    mask.width,
                    mask.height,
                    scaleFactor);
            const LinuxXRectangleFields fields =
                linuxPhysicalRectToXRectangleFields(physicalMask);
            if (fields.width == 0 || fields.height == 0) continue;
            XRectangle rect = {fields.x, fields.y, fields.width, fields.height};
            xrects.push_back(rect);
        }
        
        // Apply the shape mask to the X11 window. Rebuild the base even when
        // every parsed hole clips to zero so an older shape cannot linger.
        {
            // First, create the base shape (full window rectangle)
            LinuxPhysicalRect physicalBase = toX11BoundsRect(logicalBounds);
            physicalBase.x = 0;
            physicalBase.y = 0;
            const LinuxXRectangleFields baseFields =
                linuxPhysicalRectToXRectangleFields(physicalBase);
            XRectangle baseRect = {
                baseFields.x,
                baseFields.y,
                baseFields.width,
                baseFields.height};
            
            // Set the base shape to the full window
            XShapeCombineRectangles(display, window, ShapeBounding, 0, 0,
                                   &baseRect, 1, ShapeSet, YXBanded);
            if (!isMousePassthroughEnabled) {
                XShapeCombineRectangles(display, window, ShapeInput, 0, 0,
                                       &baseRect, 1, ShapeSet, YXBanded);
            }
            
            // Subtract each mask rectangle individually
            for (size_t i = 0; i < xrects.size(); i++) {
                XShapeCombineRectangles(display, window, ShapeBounding, 0, 0,
                                       &xrects[i], 1, ShapeSubtract, YXBanded);
                if (!isMousePassthroughEnabled) {
                    XShapeCombineRectangles(display, window, ShapeInput, 0, 0,
                                           &xrects[i], 1, ShapeSubtract, YXBanded);
                }
            }
            
            XFlush(display);
        }
    }
    
    void removeMasks() override {
        if (!browser) {
            return;
        }
        
        // Get the CEF browser's X11 window
        CefWindowHandle window = browser->GetHost()->GetWindowHandle();
        if (!window) {
            return;
        }
        
        Display* display = parentXDisplay;
        if (!display) return;
        
        // Reset the window shape to be fully opaque/visible
        // This removes any existing shape mask
        XShapeCombineMask(display, window, ShapeBounding, 0, 0, None, ShapeSet);
        if (!isMousePassthroughEnabled) {
            XShapeCombineMask(display, window, ShapeInput, 0, 0, None, ShapeSet);
        }
        XFlush(display);
        
        // Clear the mask JSON
        maskJSON.clear();
    }
    
    void toggleMirrorMode(bool enable) override {
        mirrorModeEnabled = enable;
        // TODO: Implement mirror mode for CEF
    }
    
    void setHidden(bool hidden) override {
        if (browser) {
            // Use X11 APIs to show/hide the CEF window
            CefWindowHandle window = browser->GetHost()->GetWindowHandle();
            if (window && parentXDisplay) {
                Display* display = parentXDisplay;
                if (hidden) {
                    XUnmapWindow(display, window);
                } else {
                    XMapWindow(display, window);
                }
                XFlush(display);
            }
        }
    }
    
    void setTransparent(bool transparent) override {
        // Use the same approach as setHidden: XUnmapWindow/XMapWindow
        if (browser) {
            CefWindowHandle window = browser->GetHost()->GetWindowHandle();
            if (window && parentXDisplay) {
                Display* display = parentXDisplay;
                if (transparent) {
                    XUnmapWindow(display, window);
                } else {
                    XMapWindow(display, window);
                    
                    // When making visible again, sync position to ensure it's in the right place
                    // This is needed because unmapped windows don't receive position updates
                    if (logicalBounds.width && logicalBounds.height) {
                        syncCEFPositionWithFrame(logicalBounds);
                    }
                }
                XFlush(display);
            }
        }
    }
    
    void setPassthrough(bool enable) override {
        AbstractView::setPassthrough(enable); // Set the flag

        if (browser) {
            // Use X11 input shape extension for mouse passthrough
            CefWindowHandle window = browser->GetHost()->GetWindowHandle();
            if (window && parentXDisplay) {
                Display* display = parentXDisplay;
                if (enable) {
                    // Make window invisible to mouse events
                    XRectangle rect = {0, 0, 0, 0}; // Empty rectangle
                    XShapeCombineRectangles(display, window, ShapeInput, 0, 0,
                                           &rect, 1, ShapeSet, YXBanded);
                } else {
                    if (!maskJSON.empty() && maskJSON != "[]") {
                        applyVisualMask();
                    } else {
                        XShapeCombineMask(
                            display, window, ShapeInput, 0, 0, None, ShapeSet);
                    }
                }
                XFlush(display);
            }
        }
    }

    void findInPage(const char* searchText, bool forward, bool matchCase) override {
        if (!searchText || strlen(searchText) == 0) {
            findSession.reset();
            if (!browser) return;

            CefRefPtr<CefBrowserHost> host = browser->GetHost();
            if (host) {
                host->StopFinding(true);
            }
            return;
        }

        if (!browser) return;

        CefRefPtr<CefBrowserHost> host = browser->GetHost();
        if (!host) return;

        const bool findNext = findSession.begin(searchText, matchCase);
        if (!findNext) {
            host->StopFinding(true);
        }

        host->Find(CefString(searchText), forward, matchCase, findNext);
    }

    void stopFindInPage() override {
        findSession.reset();
        if (!browser) return;

        CefRefPtr<CefBrowserHost> host = browser->GetHost();
        if (host) {
            host->StopFinding(true);
        }
    }

    void openDevTools() override {
        if (!browser) return;

        CefRefPtr<CefBrowserHost> host = browser->GetHost();
        if (host) {
            CefWindowInfo devToolsInfo;
            devToolsInfo.runtime_style = CEF_RUNTIME_STYLE_CHROME;
            host->ShowDevTools(devToolsInfo, nullptr, CefBrowserSettings(), CefPoint());
        }
    }

    void closeDevTools() override {
        if (!browser) return;

        CefRefPtr<CefBrowserHost> host = browser->GetHost();
        if (host) {
            host->CloseDevTools();
        }
    }

    void toggleDevTools() override {
        if (!browser) return;

        CefRefPtr<CefBrowserHost> host = browser->GetHost();
        if (host) {
            if (host->HasDevTools()) {
                host->CloseDevTools();
            } else {
                CefWindowInfo devToolsInfo;
                devToolsInfo.runtime_style = CEF_RUNTIME_STYLE_CHROME;
                host->ShowDevTools(devToolsInfo, nullptr, CefBrowserSettings(), CefPoint());
            }
        }
    }
};

static void removeCEFViewsForParentWindow(Window parent_window) {
    std::vector<std::shared_ptr<CEFWebViewImpl>> views_to_remove;
    {
        std::lock_guard<std::mutex> lock(g_webviewMapMutex);
        for (const auto& [id, view] : g_webviewMap) {
            (void)id;
            auto cef_view = std::dynamic_pointer_cast<CEFWebViewImpl>(view);
            if (cef_view && cef_view->parentXWindow == parent_window && !cef_view->isRemoved) {
                views_to_remove.push_back(std::move(cef_view));
            }
        }
    }

    // Teardown erases g_webviewMap; never invoke it while holding the map lock.
    // The parent-specific path is unconditional during global shutdown.
    for (const auto& view : views_to_remove) {
        view->removeForParentDestruction();
    }
}

