// CEF Response Filter for preload script injection (Mac-style clean approach)
class ElectrobunResponseFilter : public CefResponseFilter {
private:
    std::string electrobun_script_;
    std::string custom_script_;
    std::string buffer_;
    bool has_head_;
    bool injected_;
    
public:
    ElectrobunResponseFilter(const std::string& electrobunScript, const std::string& customScript)
        : electrobun_script_(electrobunScript), 
          custom_script_(customScript),
          has_head_(false), 
          injected_(false) {}
    
    virtual bool InitFilter() override {
        return true;
    }
    
    virtual FilterStatus Filter(void* data_in, size_t data_in_size,
                               size_t& data_in_read, void* data_out,
                               size_t data_out_size, size_t& data_out_written) override {
        
        // Add incoming data to buffer
        if (data_in_size > 0) {
            buffer_.append(static_cast<const char*>(data_in), data_in_size);
        }
        data_in_read = data_in_size;
        
        // Only inject once and if we have scripts to inject
        if (!injected_ && (!electrobun_script_.empty() || !custom_script_.empty())) {
            std::string combined_script = electrobun_script_;
            if (!custom_script_.empty()) {
                combined_script += "\n" + custom_script_;
            }
            
            std::string script_tag = "<script>\n" + combined_script + "\n</script>\n";
            
            // Look for injection points in order of preference
            size_t inject_pos = std::string::npos;
            
            // 1. Try to inject after <head>
            size_t head_pos = buffer_.find("<head>");
            if (head_pos != std::string::npos) {
                inject_pos = head_pos + 6; // After <head>
                has_head_ = true;
            }
            
            // 2. Try to inject after <html> with head wrapper
            if (inject_pos == std::string::npos) {
                size_t html_pos = buffer_.find("<html");
                if (html_pos != std::string::npos) {
                    // Find the end of the <html> tag
                    size_t html_end = buffer_.find(">", html_pos);
                    if (html_end != std::string::npos) {
                        inject_pos = html_end + 1;
                        script_tag = "<head>\n" + script_tag + "</head>\n";
                    }
                }
            }
            
            // 3. Fallback: inject at beginning
            if (inject_pos == std::string::npos) {
                inject_pos = 0;
                script_tag = "<html><head>\n" + script_tag + "</head><body>\n";
            }
            
            // Inject the script
            if (inject_pos <= buffer_.size()) {
                buffer_.insert(inject_pos, script_tag);
                injected_ = true;
            }
        }
        
        // Output buffered data
        size_t copy_size = std::min(data_out_size, buffer_.size());
        if (copy_size > 0) {
            std::memcpy(data_out, buffer_.data(), copy_size);
            buffer_.erase(0, copy_size);
        }
        data_out_written = copy_size;
        
        return buffer_.empty() ? RESPONSE_FILTER_DONE : RESPONSE_FILTER_NEED_MORE_DATA;
    }
    
    IMPLEMENT_REFCOUNTING(ElectrobunResponseFilter);
};

// CEF views:// scheme handler implementation
class ViewsResourceHandler : public CefResourceHandler {
public:
    explicit ViewsResourceHandler(uint32_t webviewId) : offset_(0), webviewId_(webviewId) {}
    
    bool Open(CefRefPtr<CefRequest> request, bool& handle_request, CefRefPtr<CefCallback> callback) override {
        std::string url = request->GetURL();
        const bool appData = url.rfind("appdata://", 0) == 0;
        if (!protocolAllowed(webviewId_, appData)) {
            handle_request = false;
            return false;
        }
        
        // Parse the URI to get everything after views://
        std::string fullPath = normalizeViewsRelativePath(url);
        if (appData) {
            if (!readContainedFile(appDataRoot(), fullPath, data_)) {
                handle_request = false;
                return false;
            }
            mimeType_ = getMimeTypeFromUrl(fullPath);
            handle_request = true;
            return true;
        }
        
        // Check if this is the internal HTML request
        if (fullPath == "internal/index.html") {
            // Use stored HTML content for this specific webview
            const char* htmlContent = getWebviewHTMLContent(webviewId_);
            if (htmlContent) {
                data_ = std::string(htmlContent);
                mimeType_ = "text/html";
                free((void*)htmlContent); // Free the strdup'd memory
                handle_request = true;
                return true;
            } else {
                data_ = "<html><body>No content set</body></html>";
                mimeType_ = "text/html";
                handle_request = true;
                return true;
            }
        }
        
        // Check if this webview has a custom viewsRoot
        std::string viewsRootPath;
        {
            std::lock_guard<std::mutex> lock(g_webviewViewsRootMutex);
            auto it = g_webviewViewsRoot.find(webviewId_);
            if (it != g_webviewViewsRoot.end()) {
                viewsRootPath = it->second;
            }
        }
        
        // If viewsRoot is set, try to read from that directory first
        if (!viewsRootPath.empty()) {
            if (readContainedFile(viewsRootPath, fullPath, data_)) {
                    // Determine MIME type
                    std::string mimeType = "application/octet-stream";
                    if (fullPath.find(".html") != std::string::npos) mimeType = "text/html";
                    else if (fullPath.find(".css") != std::string::npos) mimeType = "text/css";
                    else if (fullPath.find(".js") != std::string::npos) mimeType = "text/javascript";
                    else if (fullPath.find(".json") != std::string::npos) mimeType = "application/json";
                    else if (fullPath.find(".png") != std::string::npos) mimeType = "image/png";
                    else if (fullPath.find(".jpg") != std::string::npos || fullPath.find(".jpeg") != std::string::npos) mimeType = "image/jpeg";
                    else if (fullPath.find(".svg") != std::string::npos) mimeType = "image/svg+xml";
                    else if (fullPath.find(".woff") != std::string::npos) mimeType = "font/woff";
                    else if (fullPath.find(".woff2") != std::string::npos) mimeType = "font/woff2";
                    else if (fullPath.find(".ttf") != std::string::npos) mimeType = "font/ttf";
                    mimeType_ = mimeType;
                    
                    handle_request = true;
                    return true;
            }
        }

        // Build paths relative to current directory (bin)
        char* cwd = g_get_current_dir();
        gchar* resourcesDir = g_build_filename(cwd, "..", "Resources", nullptr);
        gchar* asarPath = g_build_filename(resourcesDir, "app.asar", nullptr);

        // Check if ASAR archive exists
        if (g_file_test(asarPath, G_FILE_TEST_EXISTS)) {
            // Thread-safe lazy-load ASAR archive on first use
            std::call_once(g_asarArchiveInitFlag, [asarPath]() {
                g_asarArchive = asar_open(asarPath);
                if (!g_asarArchive) {
                    printf("ERROR CEF loadViewsFile: Failed to open ASAR archive at %s\n", asarPath);
                }
            });

            // If ASAR archive is loaded, try to read from it
            if (g_asarArchive) {
                // The ASAR contains the entire app directory, so prepend "views/" to the path
                std::string asarFilePath = "views/" + fullPath;

                size_t fileSize = 0;
                const uint8_t* fileData = nullptr;
                
                // Protect ASAR read operations with mutex
                {
                    std::lock_guard<std::mutex> lock(g_asarReadMutex);
                    fileData = asar_read_file(g_asarArchive, asarFilePath.c_str(), &fileSize);
                    
                    if (fileData && fileSize > 0) {
                        // Create std::string that copies the buffer while holding the lock
                        data_ = std::string(reinterpret_cast<const char*>(fileData), fileSize);
                        // Free the ASAR buffer before releasing the lock
                        asar_free_buffer(fileData, fileSize);
                    }
                }

                if (!data_.empty()) {

                    // Determine MIME type
                    std::string mimeType = "application/octet-stream";
                    if (fullPath.find(".html") != std::string::npos) mimeType = "text/html";
                    else if (fullPath.find(".css") != std::string::npos) mimeType = "text/css";
                    else if (fullPath.find(".js") != std::string::npos) mimeType = "text/javascript";
                    else if (fullPath.find(".json") != std::string::npos) mimeType = "application/json";
                    else if (fullPath.find(".png") != std::string::npos) mimeType = "image/png";
                    else if (fullPath.find(".jpg") != std::string::npos || fullPath.find(".jpeg") != std::string::npos) mimeType = "image/jpeg";
                    else if (fullPath.find(".svg") != std::string::npos) mimeType = "image/svg+xml";
                    else if (fullPath.find(".woff") != std::string::npos) mimeType = "font/woff";
                    else if (fullPath.find(".woff2") != std::string::npos) mimeType = "font/woff2";
                    else if (fullPath.find(".ttf") != std::string::npos) mimeType = "font/ttf";
                    mimeType_ = mimeType;

                    g_free(cwd);
                    g_free(resourcesDir);
                    g_free(asarPath);

                    handle_request = true;
                    return true;
                } else {
                    // Fall through to flat file reading
                }
            }
        }

        // Fallback: Read from flat file system (for non-ASAR builds or missing files)
        gchar* viewsDir = g_build_filename(resourcesDir, "app", "views", nullptr);
        if (readContainedFile(viewsDir, fullPath, data_)) {
                mimeType_ = getMimeTypeFromUrl(fullPath);
                g_free(cwd);
                g_free(viewsDir);
                handle_request = true;
                return true;
        }
        
        g_free(cwd);
        g_free(viewsDir);
        
        handle_request = false;
        return false;
    }
    
    void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t& response_length, CefString& redirectUrl) override {
        response->SetStatus(200);
        response->SetMimeType(mimeType_);
        response->SetStatusText("OK");
        CefResponse::HeaderMap headers;
        headers.emplace("Access-Control-Allow-Origin", "*");
        headers.emplace("X-Content-Type-Options", "nosniff");
        std::string contentSecurityPolicy = electrobun::viewContentSecurityPolicy(webviewId_);
        if (!contentSecurityPolicy.empty()) {
            headers.emplace("Content-Security-Policy", contentSecurityPolicy);
        }
        response->SetHeaderMap(headers);
        response_length = data_.length();
    }
    
    bool Read(void* data_out, int bytes_to_read, int& bytes_read, CefRefPtr<CefResourceReadCallback> callback) override {
        bool has_data = false;
        bytes_read = 0;
        
        if (offset_ < data_.length()) {
            int transfer_size = std::min(bytes_to_read, static_cast<int>(data_.length() - offset_));
            memcpy(data_out, data_.c_str() + offset_, transfer_size);
            offset_ += transfer_size;
            bytes_read = transfer_size;
            has_data = true;
        }
        
        return has_data;
    }
    
    void Cancel() override {
        // Nothing to cancel
    }
    
private:
    std::string data_;
    std::string mimeType_;
    size_t offset_;
    uint32_t webviewId_;
    
    IMPLEMENT_REFCOUNTING(ViewsResourceHandler);
};

// CEF views:// scheme handler factory
class ViewsSchemeHandlerFactory : public CefSchemeHandlerFactory {
public:
    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, 
                                       const CefString& scheme_name, CefRefPtr<CefRequest> request) override {
        // Resolve the webviewId for the browser making this request
        uint32_t webviewId = 0;
        if (browser) {
            std::lock_guard<std::mutex> lock(g_browserIdToWebviewIdMutex);
            auto it = g_browserIdToWebviewId.find(browser->GetIdentifier());
            if (it != g_browserIdToWebviewId.end()) {
                webviewId = it->second;
            }
        }
        return new ViewsResourceHandler(webviewId);
    }
    
private:
    IMPLEMENT_REFCOUNTING(ViewsSchemeHandlerFactory);
};

// V8 Handler for postMessage functions
class V8MessageHandler : public CefV8Handler {
public:
    V8MessageHandler(CefRefPtr<CefBrowser> browser, const CefString& messageName)
        : browser_(browser), message_name_(messageName) {}

    virtual bool Execute(const CefString& name,
                       CefRefPtr<CefV8Value> object,
                       const CefV8ValueList& arguments,
                       CefRefPtr<CefV8Value>& retval,
                       CefString& exception) override {
        
        if (arguments.size() > 0 && arguments[0]->IsString()) {
            std::string msgContent = arguments[0]->GetStringValue();
            
            // Create and send process message to the browser process
            CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(message_name_);
            message->GetArgumentList()->SetString(0, msgContent);
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, message);
            return true;
        }
        return false;
    }

private:
    CefRefPtr<CefBrowser> browser_;
    CefString message_name_;
    IMPLEMENT_REFCOUNTING(V8MessageHandler);
};

// ElectrobunApp implementation for Linux
class ElectrobunApp : public CefApp,
                     public CefBrowserProcessHandler,
                     public CefRenderProcessHandler {
public:
    ElectrobunApp() {}
    
    void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command_line) override {
        command_line->AppendSwitchWithValue("custom-scheme", "views,appdata");

        // Linux default flags — can be overridden via chromiumFlags in config
        // GPU acceleration disabled by default for VM compatibility;
        // skip with e.g. chromiumFlags: { "disable-gpu": false }
        static const std::vector<electrobun::DefaultFlag> defaults = {
            {"use-mock-keychain", ""},
            {"disable-gpu", ""},
            {"disable-gpu-compositing", ""},
            {"disable-gpu-sandbox", ""},
            {"disable-accelerated-video-decode", ""},
            {"disable-accelerated-video-encode", ""},
            {"disable-dev-shm-usage", ""},
            {"disable-extensions", ""},
            {"disable-plugins", ""},
            {"disable-web-security", ""},
            {"no-sandbox", ""},
            {"ozone-platform", "x11"},
            {"use-x11", ""},
        };
        electrobun::applyDefaultFlags(defaults, g_userChromiumFlags.skip, command_line);

        // Apply user-defined chromium flags from build.json
        electrobun::applyChromiumFlags(g_userChromiumFlags, command_line);
    }
    
    void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
        registrar->AddCustomScheme("views", 
            CEF_SCHEME_OPTION_STANDARD | 
            CEF_SCHEME_OPTION_CORS_ENABLED |
            CEF_SCHEME_OPTION_SECURE |
            CEF_SCHEME_OPTION_CSP_BYPASSING |
            CEF_SCHEME_OPTION_FETCH_ENABLED);
        registrar->AddCustomScheme("appdata",
            CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_CORS_ENABLED |
            CEF_SCHEME_OPTION_SECURE | CEF_SCHEME_OPTION_FETCH_ENABLED);
    }
    
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }
    
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
        // In multi-process mode, return nullptr so render process uses the helper
        return nullptr;
    }
    
    
    void OnContextInitialized() override {
        CefRegisterSchemeHandlerFactory("views", "", new ViewsSchemeHandlerFactory());
        CefRegisterSchemeHandlerFactory("appdata", "", new ViewsSchemeHandlerFactory());
    }
    
    // Render process handler methods
    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override {
        
        // Enter the context
        context->Enter();
        
        // Get the global object
        CefRefPtr<CefV8Value> global = context->GetGlobal();
        
        // Create hostBridge/bunBridge object with postMessage method
        CefRefPtr<CefV8Value> bunBridge = CefV8Value::CreateObject(nullptr, nullptr);
        CefRefPtr<CefV8Handler> bunHandler = new V8MessageHandler(browser, "BunBridgeMessage");
        CefRefPtr<CefV8Value> bunPostMessage = CefV8Value::CreateFunction("postMessage", bunHandler);
        bunBridge->SetValue("postMessage", bunPostMessage, V8_PROPERTY_ATTRIBUTE_NONE);
        global->SetValue("hostBridge", bunBridge, V8_PROPERTY_ATTRIBUTE_NONE);
        global->SetValue("bunBridge", bunBridge, V8_PROPERTY_ATTRIBUTE_NONE);
        
        // Create internalBridge object with postMessage method
        CefRefPtr<CefV8Value> internalBridge = CefV8Value::CreateObject(nullptr, nullptr);
        CefRefPtr<CefV8Handler> internalHandler = new V8MessageHandler(browser, "internalMessage");
        CefRefPtr<CefV8Value> internalPostMessage = CefV8Value::CreateFunction("postMessage", internalHandler);
        internalBridge->SetValue("postMessage", internalPostMessage, V8_PROPERTY_ATTRIBUTE_NONE);
        global->SetValue("internalBridge", internalBridge, V8_PROPERTY_ATTRIBUTE_NONE);
        
        
        // Exit the context
        context->Exit();
    }

private:
    IMPLEMENT_REFCOUNTING(ElectrobunApp);
    DISALLOW_COPY_AND_ASSIGN(ElectrobunApp);
};


// ElectrobunClient implementation for Linux
class ElectrobunClient : public CefClient,
                        public CefLoadHandler,
                        public CefRequestHandler,
                        public CefContextMenuHandler,
                        public CefKeyboardHandler,
                        public CefDisplayHandler,
                        public CefResourceRequestHandler,
                        public CefLifeSpanHandler,
                        public CefPermissionHandler,
                        public CefDialogHandler,
                        public CefDownloadHandler,
                        public CefRenderHandler {
private:
    uint32_t webview_id_;
    HandlePostMessage event_bridge_handler_;
    HandlePostMessage bun_bridge_handler_;
    HandlePostMessage webview_tag_handler_;
    WebviewEventHandler webview_event_handler_;
    DecideNavigationCallback navigation_callback_;
    bool is_sandboxed_;

    std::string electrobun_script_;
    std::string custom_script_;
    CefRefPtr<CefBrowser> browser_;
    
    GtkWidget* gtk_widget_;
    std::function<void()> positioning_callback_;
    std::function<void(CefRefPtr<CefBrowser>)> browser_created_callback_;
    std::function<void()> browser_close_callback_;  // Callback to clear parent webview browser
    std::function<void()> load_end_callback_;  // Callback for page load completion
    std::function<void()> load_commit_callback_;  // Callback after a main-frame commit
    std::atomic<bool> owner_detached_{false};
    std::atomic<bool> initial_browser_creation_pending_{false};
    guint layout_interval_source_id_ = 0;
    
    // OSR (Off-Screen Rendering) members for transparency
    Window x11_window_;
    Display* display_;
    bool osr_enabled_;
    int osr_width_, osr_height_;
    Cursor osr_cursor_;
    std::mutex osr_state_mutex_;
    
    // Parent window handle for proper CEF window parenting
    Window parent_window_handle_;
    Display* parent_display_;

public:
    ElectrobunClient(uint32_t webviewId,
                     HandlePostMessage eventBridgeHandler,
                     HandlePostMessage bunBridgeHandler,
                     HandlePostMessage internalBridgeHandler,
                     WebviewEventHandler webviewEventHandler,
                     DecideNavigationCallback navigationCallback,
                     GtkWidget* gtkWidget,
                     bool sandbox)
        : webview_id_(webviewId)
        , event_bridge_handler_(eventBridgeHandler)
        , bun_bridge_handler_(bunBridgeHandler)
        , webview_tag_handler_(internalBridgeHandler)
        , webview_event_handler_(webviewEventHandler)
        , navigation_callback_(navigationCallback)
        , is_sandboxed_(sandbox)
        , gtk_widget_(gtkWidget)
        , x11_window_(0)
        , display_(nullptr)
        , osr_enabled_(false)
        , osr_width_(0)
        , osr_height_(0)
        , osr_cursor_(0)
        , parent_window_handle_(0)
        , parent_display_(nullptr) {}

    void AddPreloadScript(const std::string& script, bool mainFrameOnly = false) {
        electrobun_script_ = script;
    }

    void UpdateCustomPreloadScript(const std::string& script) {
        custom_script_ = script;
    }
    
    std::string GetCombinedScript() {
        std::string combined_script = electrobun_script_;
        if (!custom_script_.empty()) {
            combined_script += "\n" + custom_script_;
        }
        return combined_script;
    }
    
    void SetBrowser(CefRefPtr<CefBrowser> browser) {
        browser_ = browser;
    }
    
    void SetParentWindowHandle(Window parent_window, Display* parent_display) {
        parent_window_handle_ = parent_window;
        parent_display_ = parent_display;
    }
    
    CefRefPtr<CefBrowser> GetBrowser() {
        return browser_;
    }

    void MarkInitialBrowserCreationPending() {
        bool expected = false;
        if (initial_browser_creation_pending_.compare_exchange_strong(expected, true)) {
            g_pendingCefBrowserCreations.fetch_add(1);
        }
    }

    void ResolveInitialBrowserCreationPending() {
        if (initial_browser_creation_pending_.exchange(false)) {
            g_pendingCefBrowserCreations.fetch_sub(1);
        }
    }
    
    void SetBrowserCreatedCallback(std::function<void(CefRefPtr<CefBrowser>)> callback) {
        if (owner_detached_.load()) return;
        browser_created_callback_ = callback;
    }
    
    void SetBrowserCloseCallback(std::function<void()> callback) {
        if (owner_detached_.load()) return;
        browser_close_callback_ = callback;
    }
    
    void SetLoadEndCallback(std::function<void()> callback) {
        if (owner_detached_.load()) return;
        load_end_callback_ = callback;
    }

    void SetLoadCommitCallback(std::function<void()> callback) {
        if (owner_detached_.load()) return;
        load_commit_callback_ = callback;
    }

    void CancelLayoutInterval() {
        const guint sourceId = layout_interval_source_id_;
        layout_interval_source_id_ = 0;
        if (sourceId) {
            g_source_remove(sourceId);
        }
    }

    void DetachOwnerCallbacks() {
        owner_detached_.store(true);
        CancelLayoutInterval();
        DisableOSR();
        browser_created_callback_ = nullptr;
        browser_close_callback_ = nullptr;
        load_end_callback_ = nullptr;
        load_commit_callback_ = nullptr;
        positioning_callback_ = nullptr;
    }
    
    void SetBrowserPreloadScript(int browserId, const std::string& script) {
        g_preloadScripts[browserId] = script;
    }
    
    void SetPositioningCallback(std::function<void()> callback) {
        positioning_callback_ = callback;
    }
    
    void EnableOSR(Window x11_window, Display* display, int width, int height) {
        std::lock_guard<std::mutex> lock(osr_state_mutex_);
        x11_window_ = x11_window;
        display_ = display;
        osr_enabled_ = true;
        osr_width_ = std::max(1, width);
        osr_height_ = std::max(1, height);
        printf("CEF: OSR enabled for window %lu, size %dx%d\n", x11_window, width, height);
    }

    void DisableOSR() {
        std::lock_guard<std::mutex> lock(osr_state_mutex_);
        osr_enabled_ = false;
        if (display_ && osr_cursor_) {
            XFreeCursor(display_, osr_cursor_);
        }
        osr_cursor_ = 0;
        x11_window_ = 0;
        display_ = nullptr;
    }

    void UpdateOSRSize(int width, int height) {
        std::lock_guard<std::mutex> lock(osr_state_mutex_);
        if (!osr_enabled_) return;
        osr_width_ = std::max(1, width);
        osr_height_ = std::max(1, height);
    }
    
    void SendMouseEvent(const CefMouseEvent& event, bool mouse_down, int click_count) {
        CefRefPtr<CefBrowser> browser;
        {
            std::lock_guard<std::mutex> lock(osr_state_mutex_);
            if (!browser_ || !osr_enabled_) return;
            browser = browser_;
        }
        browser->GetHost()->SendMouseMoveEvent(event, false);
        if (mouse_down) {
            browser->GetHost()->SendMouseClickEvent(
                event, MBT_LEFT, false, click_count);
        }
    }
    
    void SendKeyEvent(const CefKeyEvent& event) {
        CefRefPtr<CefBrowser> browser;
        {
            std::lock_guard<std::mutex> lock(osr_state_mutex_);
            if (!browser_ || !osr_enabled_) return;
            browser = browser_;
        }
        browser->GetHost()->SendKeyEvent(event);
    }

    virtual CefRefPtr<CefLoadHandler> GetLoadHandler() override {
        return this;
    }

    virtual CefRefPtr<CefRequestHandler> GetRequestHandler() override {
        return this;
    }

    virtual CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override {
        return this;
    }

    virtual CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override {
        return this;
    }

    virtual CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
        return this;
    }

    virtual CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }
    
    virtual CefRefPtr<CefPermissionHandler> GetPermissionHandler() override {
        return this;
    }
    
    virtual CefRefPtr<CefDialogHandler> GetDialogHandler() override {
        return this;
    }

    virtual CefRefPtr<CefDownloadHandler> GetDownloadHandler() override {
        return this;
    }

    virtual CefRefPtr<CefRenderHandler> GetRenderHandler() override {
        return this;
    }

    bool OnCursorChange(CefRefPtr<CefBrowser> browser,
                        CefCursorHandle cursor,
                        cef_cursor_type_t type,
                        const CefCursorInfo& custom_cursor_info) override {
        (void)browser;
        (void)cursor;
        (void)custom_cursor_info;

        std::lock_guard<std::mutex> lock(osr_state_mutex_);
        if (!osr_enabled_) {
            return false;
        }

        Display* display = display_ ? display_ : parent_display_;
        Window window = x11_window_ ? x11_window_ : parent_window_handle_;
        if (!display || !window) {
            return false;
        }

        unsigned int cursorShape = XC_left_ptr;
        switch (type) {
            case CT_IBEAM:
            case CT_VERTICALTEXT:
                cursorShape = XC_xterm;
                break;
            case CT_HAND:
                cursorShape = XC_hand2;
                break;
            case CT_CROSS:
                cursorShape = XC_crosshair;
                break;
            case CT_WAIT:
            case CT_PROGRESS:
                cursorShape = XC_watch;
                break;
            case CT_HELP:
                cursorShape = XC_question_arrow;
                break;
            case CT_MOVE:
            case CT_GRAB:
            case CT_GRABBING:
                cursorShape = XC_fleur;
                break;
            case CT_EASTRESIZE:
            case CT_WESTRESIZE:
            case CT_EASTWESTRESIZE:
            case CT_COLUMNRESIZE:
                cursorShape = XC_sb_h_double_arrow;
                break;
            case CT_NORTHRESIZE:
            case CT_SOUTHRESIZE:
            case CT_NORTHSOUTHRESIZE:
            case CT_ROWRESIZE:
                cursorShape = XC_sb_v_double_arrow;
                break;
            case CT_NORTHEASTRESIZE:
            case CT_SOUTHWESTRESIZE:
            case CT_NORTHEASTSOUTHWESTRESIZE:
                cursorShape = XC_top_right_corner;
                break;
            case CT_NORTHWESTRESIZE:
            case CT_SOUTHEASTRESIZE:
            case CT_NORTHWESTSOUTHEASTRESIZE:
                cursorShape = XC_top_left_corner;
                break;
            case CT_NOTALLOWED:
            case CT_NODROP:
            case CT_DND_NONE:
                cursorShape = XC_X_cursor;
                break;
            default:
                cursorShape = XC_left_ptr;
                break;
        }

        Cursor nextCursor = XCreateFontCursor(display, cursorShape);
        if (!nextCursor) {
            return false;
        }

        XDefineCursor(display, window, nextCursor);
        XFlush(display);

        if (osr_cursor_) {
            XFreeCursor(display, osr_cursor_);
        }
        osr_cursor_ = nextCursor;
        return true;
    }

    // Static debounce timestamp for ctrl+click handling
    static double lastCtrlClickTime;

    // Handle navigation requests
    bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       CefRefPtr<CefRequest> request,
                       bool user_gesture,
                       bool is_redirect) override {
        std::string url = request->GetURL().ToString();

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

        if (isCtrlHeld && !is_redirect && webview_event_handler_) {
            // Debounce: ignore ctrl+click navigations within 500ms
            double now = g_get_monotonic_time() / 1000000.0;

            if (now - lastCtrlClickTime >= 0.5) {
                lastCtrlClickTime = now;

                // Escape URL for JSON
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
                webview_event_handler_(webview_id_, strdup("new-window-open"), strdup(eventData.c_str()));
                return true;  // Cancel navigation
            }
        }

        // Check navigation rules synchronously from native-stored rules  
        // Note: This mirrors the same logic in WebKit policy handler
        bool shouldAllow = true;
        {
            std::lock_guard<std::mutex> lock(g_webviewMapMutex);
            auto it = g_webviewMap.find(webview_id_);
            if (it != g_webviewMap.end() && it->second != nullptr) {
                // Forward to the navigation rules check method (defined later in this file)
                shouldAllow = checkNavigationRules(it->second, url);
            }
        }

        // Fire will-navigate event with allowed status
        if (webview_event_handler_) {
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
            webview_event_handler_(webview_id_, strdup("will-navigate"), strdup(eventData.c_str()));
        }

        return !shouldAllow;  // Return true to cancel navigation
    }

    virtual CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefRequest> request,
        bool is_navigation,
        bool is_download,
        const CefString& request_initiator,
        bool& disable_default_handling) override {
        return this;
    }
    
    // Response filter for preload script injection (Mac-style clean approach)
    virtual CefRefPtr<CefResponseFilter> GetResourceResponseFilter(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefRequest> request,
        CefRefPtr<CefResponse> response) override {
        
        // Only inject scripts into HTML responses in main frame
        if (frame->IsMain() && 
            response->GetMimeType().ToString().find("html") != std::string::npos) {
            
            std::string combined_script = GetCombinedScript();
            if (!combined_script.empty()) {
                return new ElectrobunResponseFilter(electrobun_script_, custom_script_);
            }
        }
        return nullptr;
    }


    void OnLoadStart(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     TransitionType transition_type) override {
        if (frame->IsMain() && webview_event_handler_) {
            std::string url = frame->GetURL().ToString();
            webview_event_handler_(webview_id_, strdup("did-commit-navigation"), strdup(url.c_str()));
        }
        if (frame->IsMain() && !owner_detached_.load() && load_commit_callback_) {
            load_commit_callback_();
        }
    }

    void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                  CefRefPtr<CefFrame> frame,
                  int httpStatusCode) override {
        if (frame->IsMain() && webview_event_handler_) {
            std::string url = frame->GetURL().ToString();
            webview_event_handler_(webview_id_, strdup("did-navigate"), strdup(url.c_str()));
        }
        
        // Call load end callback for deferred operations (like transparency)
        if (frame->IsMain() && !owner_detached_.load() && load_end_callback_) {
            load_end_callback_();
        }
        
    }

    // Context menu handler with DevTools option
    void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            CefRefPtr<CefContextMenuParams> params,
                            CefRefPtr<CefMenuModel> model) override {
        printf("CEF: Creating context menu with DevTools options\n");
        
        // Don't clear the default menu - let CEF show its default items
        // model->Clear();
        
        // Add DevTools option to existing menu
        if (model->GetCount() > 0) {
            model->AddSeparator();
        }
        model->AddItem(26501, "Inspect Element");
        model->AddItem(26502, "Open DevTools");
        
        printf("CEF: Context menu now has %zu items\n", model->GetCount());
    }
    
    // Handle context menu display
    bool RunContextMenu(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       CefRefPtr<CefContextMenuParams> params,
                       CefRefPtr<CefMenuModel> model,
                       CefRefPtr<CefRunContextMenuCallback> callback) override {
        printf("CEF: RunContextMenu called - using CEF's default implementation\n");
        
        // Return false to let CEF handle the context menu with its native implementation
        // CEF will create its own X11 window for the menu that works properly
        return false;
    }
    
    // Handle context menu commands
    bool OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
                             CefRefPtr<CefFrame> frame,
                             CefRefPtr<CefContextMenuParams> params,
                             int command_id,
                             EventFlags event_flags) override {
        printf("CEF: Context menu command selected: %d\n", command_id);
        
        switch (command_id) {
            case 26501: // Inspect Element
            case 26502: // Open DevTools
                printf("CEF: Opening DevTools...\n");
                {
                    CefWindowInfo devToolsInfo;
                    devToolsInfo.runtime_style = CEF_RUNTIME_STYLE_CHROME;
                    browser->GetHost()->ShowDevTools(devToolsInfo, this, CefBrowserSettings(), CefPoint());
                }
                return true;
                
            case 26503: // Reload
                printf("CEF: Reloading page...\n");
                browser->Reload();
                return true;
                
            case 26504: // View Source
                printf("CEF: Viewing source...\n");
                frame->ViewSource();
                return true;
                
            default:
                return false;
        }
    }
    
    // Handle keyboard shortcuts (F12 for DevTools)
    bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                       const CefKeyEvent& event,
                       CefEventHandle os_event,
                       bool* is_keyboard_shortcut) override {
        if (event.type == KEYEVENT_KEYDOWN && event.windows_key_code == 123) { // F12 key
            printf("CEF: F12 pressed - opening DevTools\n");
            {
                CefWindowInfo devToolsInfo;
                devToolsInfo.runtime_style = CEF_RUNTIME_STYLE_CHROME;
                browser->GetHost()->ShowDevTools(devToolsInfo, this, CefBrowserSettings(), CefPoint());
            }
            return true; // Consume the event
        }
        return false;
    }

    // CefLifeSpanHandler methods
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      int popup_id,
                      const CefString& target_url,
                      const CefString& target_frame_name,
                      CefLifeSpanHandler::WindowOpenDisposition target_disposition,
                      bool user_gesture,
                      const CefPopupFeatures& popupFeatures,
                      CefWindowInfo& windowInfo,
                      CefRefPtr<CefClient>& client,
                      CefBrowserSettings& settings,
                      CefRefPtr<CefDictionaryValue>& extra_info,
                      bool* no_javascript_access) override {
        // Prevent all popup windows - this stops CEF from creating new top-level windows
        return true;  // Return true to cancel the popup
    }
    
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        
        // Set the browser reference
        SetBrowser(browser);
        ResolveInitialBrowserCreationPending();

        // Keep a strong reference until OnBeforeClose. A view may already have
        // been removed from g_webviewMap while its close is still in flight.
        {
            std::lock_guard<std::mutex> lock(g_liveCefBrowsersMutex);
            g_liveCefBrowsers[browser->GetIdentifier()] = browser;
        }
        
        // Register browser ID → webviewId so scheme handlers can look up content
        {
            std::lock_guard<std::mutex> lock(g_browserIdToWebviewIdMutex);
            g_browserIdToWebviewId[browser->GetIdentifier()] = webview_id_;
        }
        
        // A view can be removed before asynchronous browser creation finishes.
        // In that case the client owns the only safe reference and must close
        // the new browser instead of leaving it orphaned.
        if (g_shuttingDown.load() || owner_detached_.load()) {
            browser->GetHost()->CloseBrowser(g_shuttingDown.load());
            return;
        }

        // Schedule rapid interval to trigger OOPIF positioning
        // Runs every 5ms for up to 1 second to ensure OOPIFs are positioned correctly
        struct LayoutIntervalData {
            CefRefPtr<ElectrobunClient> owner;
            CefRefPtr<CefBrowser> browser;
            Display* display;
            gint64 start_time;
        };

        CancelLayoutInterval();
        auto* interval_data = new LayoutIntervalData{
            this,
            browser,
            parent_display_,
            g_get_monotonic_time(), // microseconds since arbitrary point
        };

        layout_interval_source_id_ = g_timeout_add_full(
            G_PRIORITY_DEFAULT,
            5,
            [](gpointer data) -> gboolean {
            auto* lid = static_cast<LayoutIntervalData*>(data);

            const auto finish = [lid]() {
                if (lid->owner) {
                    lid->owner->layout_interval_source_id_ = 0;
                }
                return G_SOURCE_REMOVE;
            };

            // Check if 1 second has elapsed
            gint64 elapsed = g_get_monotonic_time() - lid->start_time;
            if (elapsed > 1000000) { // 1 second in microseconds
                return finish();
            }

            if (g_shuttingDown.load() || !lid->owner ||
                lid->owner->owner_detached_.load()) {
                return finish();
            }

            if (lid->browser) {
                CefWindowHandle cefWindow = lid->browser->GetHost()->GetWindowHandle();
                
                if (cefWindow && cefWindow != 0x1) {
                    
                    
                    // Get window dimensions for mouse event coordinates
                    Display* display = lid->display;
                    XWindowAttributes attrs;
                    if (display && XGetWindowAttributes(display, (Window)cefWindow, &attrs) != 0) {
                        // Send mouse move event to trigger layout recalculation
                        CefMouseEvent moveEvent;
                        moveEvent.x = attrs.width / 2;
                        moveEvent.y = attrs.height / 2;
                        lid->browser->GetHost()->SendMouseMoveEvent(moveEvent, false);
                        
                        // Send minimal scroll event
                        CefMouseEvent scrollEvent;
                        scrollEvent.x = attrs.width / 2;
                        scrollEvent.y = attrs.height / 2;
                        lid->browser->GetHost()->SendMouseWheelEvent(scrollEvent, 0, 1);
                        lid->browser->GetHost()->SendMouseWheelEvent(scrollEvent, 0, -1);
                    }
                    
                }
            }

            return G_SOURCE_CONTINUE; // Continue interval
        }, interval_data, [](gpointer data) {
            delete static_cast<LayoutIntervalData*>(data);
        });
        
        // The CEF browser window is now fully created
        CefWindowHandle cefWindow = browser->GetHost()->GetWindowHandle();
        
        // printf("CEF: [BROWSER_CREATED] Browser created, window handle: 0x%lx\n", (unsigned long)cefWindow);
        
        // Validate the CEF window handle and ensure proper parenting
        if (cefWindow) {
            Display* display = parent_display_;
            if (!display) {
                fprintf(stderr, "CEF: parent X11 Display is unavailable\n");
            } else {
            
            // Try to get window attributes to validate the handle
            XWindowAttributes attrs;
            int result = XGetWindowAttributes(display, (Window)cefWindow, &attrs);
            
            // Ensure the CEF window is properly parented to the main window
            if (parent_window_handle_) {
                if (cefWindow != 0x1 && result != 0) {
                    XReparentWindow(display, cefWindow, parent_window_handle_, 0, 0);
                    XMapRaised(display, cefWindow);
                    XFlush(display);
                } else {
                    // printf("CEF: [BROWSER_CREATED] Skipping reparenting due to invalid window handle\n");
                }
            } else {
                printf("CEF: [BROWSER_CREATED] No parent window handle set\n");
            }
                
                // Get window geometry to verify positioning
                XWindowAttributes parent_attrs, cef_attrs;
                if (XGetWindowAttributes(display, parent_window_handle_, &parent_attrs) &&
                    XGetWindowAttributes(display, cefWindow, &cef_attrs)) {
                    
                }
                            
            XWindowAttributes tree_attrs;
            if (XGetWindowAttributes(display, cefWindow, &tree_attrs) == 0) {
                Window root, parent;
                Window* children;
                unsigned int nchildren;
                
                // Get the root window
                Window rootWindow = DefaultRootWindow(display);
                if (XQueryTree(display, rootWindow, &root, &parent, &children, &nchildren) != 0) {
                    for (unsigned int i = 0; i < nchildren; i++) {
                        XWindowAttributes childAttrs;
                        if (XGetWindowAttributes(display, children[i], &childAttrs) != 0) {
                            // printf("CEF: Valid window found: 0x%lx (decimal: %lu)\n", 
                            //        (unsigned long)children[i], (unsigned long)children[i]);
                        }
                    }
                    XFree(children);
                }
            } else {
                       
                // Try positioning callback if window is ready
                if (!owner_detached_.load() && positioning_callback_) {
                    positioning_callback_();
                }
            }
            }
        }

        // Reparenting above resets child coordinates to 0,0. Notify the owning
        // view only after that step so its final WM/DPI-aware bounds win.
        if (!owner_detached_.load() && browser_created_callback_) {
            browser_created_callback_(browser);
        }
    }

    // Critical: Handle browser cleanup to prevent use-after-free
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        printf("CEF: OnBeforeClose called for browser %d\n", browser->GetIdentifier());
        CancelLayoutInterval();

        // This is the shutdown barrier: only after this erase may the main
        // loop observe that every CEF browser has finished closing.
        {
            std::lock_guard<std::mutex> lock(g_liveCefBrowsersMutex);
            g_liveCefBrowsers.erase(browser->GetIdentifier());
        }
        
        // Remove browser ID → webviewId mapping
        {
            std::lock_guard<std::mutex> lock(g_browserIdToWebviewIdMutex);
            g_browserIdToWebviewId.erase(browser->GetIdentifier());
        }
        
        // Clear browser reference to prevent use-after-free
        std::lock_guard<std::mutex> lock(g_cefBrowserMutex);
        
        if (browser_ && browser_->IsSame(browser)) {
            browser_ = nullptr;
            
            // Notify parent webview to clear its browser reference too
            if (!owner_detached_.load() && browser_close_callback_) {
                browser_close_callback_();
            }
        }
    }

    // Try using OnLoadingStateChange to detect when CEF is fully ready
    void OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                            bool isLoading,
                            bool canGoBack,
                            bool canGoForward) override {
        if (!isLoading) {
            // Notify browser of resize when loading completes to ensure OOPIFs are positioned
            browser->GetHost()->WasResized();
            
            // Check if CEF window handle is valid now
            CefWindowHandle cefWindow = browser->GetHost()->GetWindowHandle();
            
            if (cefWindow) {
   
                
                Display* display = parent_display_;
                if (!display) return;
                XWindowAttributes attrs = {};
                int result = XGetWindowAttributes(display, cefWindow, &attrs);
                
                if (result == 0) {
                    printf("CEF: [LOADING_STATE] ERROR - Cannot get window attributes\n");
                } else {
                    
                    // Check window class hint
                    XClassHint class_hint;
                    if (XGetClassHint(display, cefWindow, &class_hint) != 0) {
                       
                        // Analyze toolkit based on class names
                        // if (class_hint.res_class) {
                        //     if (strstr(class_hint.res_class, "Gtk") || strstr(class_hint.res_class, "gtk")) {
                        //         // printf("CEF: TOOLKIT DETECTED: GTK (based on class name)\n");
                        //     } else if (strstr(class_hint.res_class, "Qt") || strstr(class_hint.res_class, "qt")) {
                        //         // printf("CEF: TOOLKIT DETECTED: Qt (based on class name)\n");
                        //     } else {
                        //         // printf("CEF: TOOLKIT: Likely native X11 (class: %s)\n", class_hint.res_class);
                        //     }
                        // }
                        
                        if (class_hint.res_class) XFree(class_hint.res_class);
                        if (class_hint.res_name) XFree(class_hint.res_name);
                    }
                    
                    // Check window manager name
                    char *wm_name = nullptr;
                    if (XFetchName(display, cefWindow, &wm_name) && wm_name) {
                        XFree(wm_name);
                    }
                    
                    // Check parent window
                    Window root, parent;
                    Window* children;
                    unsigned int nchildren;
                    if (XQueryTree(display, cefWindow, &root, &parent, &children, &nchildren) != 0) {
                        if (children) XFree(children);
                    }
                    
                    // Check window type
                    Atom actual_type;
                    int actual_format;
                    unsigned long nitems, bytes_after;
                    unsigned char* prop_data = nullptr;
                    Atom window_type_atom = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
                    Atom atom_type = XInternAtom(display, "ATOM", False);
                    if (XGetWindowProperty(display, cefWindow, window_type_atom, 0, 1, False,
                                         atom_type, &actual_type, &actual_format, &nitems,
                                         &bytes_after, &prop_data) == Success && prop_data) {
                        Atom window_type = *(Atom*)prop_data;
                        char* type_name = XGetAtomName(display, window_type);
                        if (type_name) XFree(type_name);
                        XFree(prop_data);
                    }
                    // Additional toolkit detection via window properties
                    
                    // Check for GTK-specific properties
                    Atom gtk_atom = XInternAtom(display, "_GTK_THEME_VARIANT", True);
                    if (gtk_atom != None) {
                        unsigned char* gtk_data = nullptr;
                        if (XGetWindowProperty(display, cefWindow, gtk_atom, 0, 1, False,
                                             AnyPropertyType, &actual_type, &actual_format, &nitems,
                                             &bytes_after, &gtk_data) == Success && gtk_data) {
                            XFree(gtk_data);
                        }
                    }
                    
                    // Check for Qt-specific properties  
                    Atom qt_atom = XInternAtom(display, "_QT_SELECTION", True);
                    if (qt_atom != None) {
                        unsigned char* qt_data = nullptr;
                        if (XGetWindowProperty(display, cefWindow, qt_atom, 0, 1, False,
                                             AnyPropertyType, &actual_type, &actual_format, &nitems,
                                             &bytes_after, &qt_data) == Success && qt_data) {
                            XFree(qt_data);
                        }
                    }
                    
                    // if (attrs.all_event_masks & 0x400000) {
                    //     // printf("CEF: Event mask suggests modern toolkit (GTK3+ or Qt5+)\n");
                    // }
                }
            }
        }
    }

    // Handle process messages from render process
    virtual bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                        CefRefPtr<CefFrame> frame,
                                        CefProcessId source_process,
                                        CefRefPtr<CefProcessMessage> message) override {
        std::string messageName = message->GetName().ToString();
        std::string messageContent = message->GetArgumentList()->GetString(0).ToString();
        
        
        bool result = false;

        // eventBridge - event-only bridge (always process for all webviews, including sandboxed)
        if (messageName == "EventBridgeMessage") {
            event_bridge_handler_(webview_id_, messageContent.c_str());
            result = true;
        }
        // bunBridge and internalBridge - RPC bridges (only for non-sandboxed webviews)
        else if (!is_sandboxed_) {
            if (messageName == "BunBridgeMessage") {
                // printf("CEF: Forwarding BunBridgeMessage to handler\n");
                bun_bridge_handler_(webview_id_, messageContent.c_str());
                result = true;
            } else if (messageName == "internalMessage") {
                // printf("CEF: Forwarding internalMessage to handler\n");
                webview_tag_handler_(webview_id_, messageContent.c_str());
                result = true;
            }
        }
        
        return result;
    }
    
    // Permission Handler methods for CEF
    virtual bool OnRequestMediaAccessPermission(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        const CefString& requesting_origin,
        uint32_t requested_permissions,
        CefRefPtr<CefMediaAccessCallback> callback) override {
        
        std::string origin = requesting_origin.ToString();
        printf("CEF: Media access permission requested for %s (permissions: %u)\n", origin.c_str(), requested_permissions);

        // views:// is the app's own bundled-asset shell — always trusted, never prompt.
        if (origin.find("views://") == 0) {
            callback->Continue(requested_permissions);
            return true;
        }

        // Check cache first
        PermissionStatus cachedStatus = getPermissionFromCache(origin, PermissionType::USER_MEDIA);
        
        if (cachedStatus == PermissionStatus::ALLOWED) {
            printf("CEF: Using cached permission: User previously allowed media access for %s\n", origin.c_str());
            callback->Continue(requested_permissions); // Allow all requested permissions
            return true;
        } else if (cachedStatus == PermissionStatus::DENIED) {
            printf("CEF: Using cached permission: User previously blocked media access for %s\n", origin.c_str());
            callback->Cancel();
            return true;
        }
        
        // No cached permission, show dialog
        printf("CEF: No cached permission found for %s, showing dialog\n", origin.c_str());
        
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
            callback->Continue(requested_permissions); // Allow all requested permissions
            cachePermission(origin, PermissionType::USER_MEDIA, PermissionStatus::ALLOWED);
            printf("CEF: User allowed media access for %s (cached)\n", origin.c_str());
        } else {
            callback->Cancel();
            cachePermission(origin, PermissionType::USER_MEDIA, PermissionStatus::DENIED);
            printf("CEF: User blocked media access for %s (cached)\n", origin.c_str());
        }
        
        return true; // We handled the permission request
    }
    
    virtual bool OnShowPermissionPrompt(
        CefRefPtr<CefBrowser> browser,
        uint64_t prompt_id,
        const CefString& requesting_origin,
        uint32_t requested_permissions,
        CefRefPtr<CefPermissionPromptCallback> callback) override {
        
        std::string origin = requesting_origin.ToString();
        printf("CEF: Permission prompt requested for %s (permissions: %u)\n", origin.c_str(), requested_permissions);

        // views:// is the app's own bundled-asset shell — always trusted, never prompt.
        // This also covers Chromium's new Loopback/Local Network Access gate triggered
        // by the per-webview RPC websocket to ws://localhost:<port>.
        if (origin.find("views://") == 0) {
            callback->Continue(CEF_PERMISSION_RESULT_ACCEPT);
            return true;
        }

        // Handle different permission types
        PermissionType permType = PermissionType::OTHER;
        std::string message;
        std::string title = "Permission Request";

        // Check for specific permission types
        if (requested_permissions & CEF_PERMISSION_TYPE_CAMERA_STREAM ||
            requested_permissions & CEF_PERMISSION_TYPE_MIC_STREAM) {
            permType = PermissionType::USER_MEDIA;
            message = "This page wants to access your camera and/or microphone.\n\nDo you want to allow this?";
            title = "Camera & Microphone Access";
        } else if (requested_permissions & CEF_PERMISSION_TYPE_GEOLOCATION) {
            permType = PermissionType::GEOLOCATION;
            message = "This page wants to access your location.\n\nDo you want to allow this?";
            title = "Location Access";
        } else if (requested_permissions & CEF_PERMISSION_TYPE_NOTIFICATIONS) {
            permType = PermissionType::NOTIFICATIONS;
            message = "This page wants to show notifications.\n\nDo you want to allow this?";
            title = "Notification Permission";
        } else {
            // Unrecognized permission type — name what's being requested instead of
            // a generic "additional permissions" dialog so the user can decide.
            message = "This page is requesting permission for: " +
                      electrobun::describeCefPermissions(requested_permissions) +
                      ".\n\nDo you want to allow this?";
        }

        // Check cache first
        PermissionStatus cachedStatus = getPermissionFromCache(origin, permType);
        
        if (cachedStatus == PermissionStatus::ALLOWED) {
            printf("CEF: Using cached permission: User previously allowed %s for %s\n", title.c_str(), origin.c_str());
            callback->Continue(CEF_PERMISSION_RESULT_ACCEPT);
            return true;
        } else if (cachedStatus == PermissionStatus::DENIED) {
            printf("CEF: Using cached permission: User previously blocked %s for %s\n", title.c_str(), origin.c_str());
            callback->Continue(CEF_PERMISSION_RESULT_DENY);
            return true;
        }
        
        // No cached permission, show dialog
        printf("CEF: No cached permission found for %s, showing dialog\n", origin.c_str());
        
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
            callback->Continue(CEF_PERMISSION_RESULT_ACCEPT);
            cachePermission(origin, permType, PermissionStatus::ALLOWED);
            printf("CEF: User allowed %s for %s (cached)\n", title.c_str(), origin.c_str());
        } else {
            callback->Continue(CEF_PERMISSION_RESULT_DENY);
            cachePermission(origin, permType, PermissionStatus::DENIED);
            printf("CEF: User blocked %s for %s (cached)\n", title.c_str(), origin.c_str());
        }
        
        return true; // We handled the permission request
    }
    
    virtual void OnDismissPermissionPrompt(
        CefRefPtr<CefBrowser> browser,
        uint64_t prompt_id,
        cef_permission_request_result_t result) override {
        
        printf("CEF: Permission prompt dismissed with result %d\n", result);
        // Optional: Handle prompt dismissal if needed
    }
    
    // CefDialogHandler methods
    virtual bool OnFileDialog(CefRefPtr<CefBrowser> browser,
                            FileDialogMode mode,
                            const CefString& title,
                            const CefString& default_file_path,
                            const std::vector<CefString>& accept_filters,
                            const std::vector<CefString>& accept_extensions,
                            const std::vector<CefString>& accept_descriptions,
                            CefRefPtr<CefFileDialogCallback> callback) override {
        
        printf("CEF Linux: File dialog requested - mode: %d\n", static_cast<int>(mode));
        
        // Run the file dialog using the desktop-native GTK portal integration.
        GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_OPEN;
        const char* buttonText = "_Open";
        
        // Configure dialog based on mode
        switch (mode) {
            case FILE_DIALOG_OPEN:
                action = GTK_FILE_CHOOSER_ACTION_OPEN;
                buttonText = "_Open";
                break;
            case FILE_DIALOG_OPEN_MULTIPLE:
                action = GTK_FILE_CHOOSER_ACTION_OPEN;
                buttonText = "_Open";
                break;
            case FILE_DIALOG_OPEN_FOLDER:
                action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
                buttonText = "_Select";
                break;
            case FILE_DIALOG_SAVE:
                action = GTK_FILE_CHOOSER_ACTION_SAVE;
                buttonText = "_Save";
                break;
        }
        
        const std::string dialogTitle = title.empty() ? "Select File" : title.ToString();
        LinuxNativeFileDialog dialog(dialogTitle.c_str(), nullptr, action, buttonText);
        if (!dialog.valid()) {
            callback->Continue(std::vector<CefString>{});
            return true;
        }
        
        // Set multiple selection for OPEN_MULTIPLE mode
        if (mode == FILE_DIALOG_OPEN_MULTIPLE) {
            dialog.setSelectMultiple(true);
        }
        
        // Set default file path if provided
        if (!default_file_path.empty()) {
            std::string path = default_file_path.ToString();
            if (mode == FILE_DIALOG_SAVE) {
                // For save dialogs, set the filename
                dialog.setCurrentName(path.c_str());
            } else {
                // For open dialogs, set the folder
                dialog.setCurrentFolder(path.c_str());
            }
        }
        
        // Set file filters
        if (!accept_filters.empty()) {
            for (const auto& filter : accept_filters) {
                std::string filterStr = filter.ToString();
                
                GtkFileFilter* gtkFilter = gtk_file_filter_new();
                gtk_file_filter_set_name(gtkFilter, filterStr.c_str());
                
                // Handle common patterns
                if (filterStr == "*.*" || filterStr == "*") {
                    gtk_file_filter_add_pattern(gtkFilter, "*");
                } else if (filterStr.find("*.") == 0) {
                    gtk_file_filter_add_pattern(gtkFilter, filterStr.c_str());
                } else {
                    // Assume it's a file extension
                    std::string pattern = "*." + filterStr;
                    gtk_file_filter_add_pattern(gtkFilter, pattern.c_str());
                }
                
                dialog.addFilter(gtkFilter);
            }
            
            // Always add an "All files" filter
            GtkFileFilter* allFilter = gtk_file_filter_new();
            gtk_file_filter_set_name(allFilter, "All files");
            gtk_file_filter_add_pattern(allFilter, "*");
            dialog.addFilter(allFilter);
        }
        
        // Show the dialog
        gint response = dialog.run();
        
        std::vector<CefString> file_paths;
        if (response == GTK_RESPONSE_ACCEPT) {
            for (const auto& path : dialog.selectedPaths()) {
                file_paths.emplace_back(path);
            }
        }
        
        // Call the callback with results
        callback->Continue(file_paths);
        
        printf("CEF Linux: File dialog completed with %zu files selected\n", file_paths.size());
        return true; // We handled the dialog
    }

    // CefDownloadHandler methods
    bool OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefDownloadItem> download_item,
                          const CefString& suggested_name,
                          CefRefPtr<CefBeforeDownloadCallback> callback) override {
        printf("CEF Linux: OnBeforeDownload for %s\n", suggested_name.ToString().c_str());

        // Get the Downloads folder using GLib
        const gchar* downloadsDir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
        if (!downloadsDir) {
            // Fallback to home directory + Downloads
            const gchar* homeDir = g_get_home_dir();
            if (homeDir) {
                gchar* fallbackDir = g_build_filename(homeDir, "Downloads", nullptr);
                downloadsDir = fallbackDir;
            }
        }

        if (downloadsDir) {
            std::string suggestedStr = suggested_name.ToString();
            gchar* destinationPath = g_build_filename(downloadsDir, suggestedStr.c_str(), nullptr);

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

            printf("CEF Linux: Downloading to %s\n", destinationPath);

            // Continue the download to the specified path without showing a dialog
            callback->Continue(destinationPath, false);

            g_free(basePath);
            g_free(extension);
            g_free(destinationPath);
        } else {
            printf("CEF Linux ERROR: Could not determine Downloads directory, using default behavior\n");
            callback->Continue("", false);
        }

        return true;  // We handled it
    }

    void OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefDownloadItem> download_item,
                           CefRefPtr<CefDownloadItemCallback> callback) override {
        if (download_item->IsComplete()) {
            printf("CEF Linux: Download complete - %s\n", download_item->GetFullPath().ToString().c_str());
        } else if (download_item->IsCanceled()) {
            printf("CEF Linux: Download canceled\n");
        } else if (download_item->IsInProgress()) {
            int percent = download_item->GetPercentComplete();
            if (percent >= 0 && percent % 25 == 0) {  // Log at 0%, 25%, 50%, 75%, 100%
                printf("CEF Linux: Download progress %d%%\n", percent);
            }
        }
    }

    // CefRenderHandler methods for OSR (Off-Screen Rendering)
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override {
        std::lock_guard<std::mutex> lock(osr_state_mutex_);
        if (osr_enabled_) {
            rect.Set(0, 0, osr_width_, osr_height_);
            // printf("CEF OSR GetViewRect: returning %dx%d\n", osr_width_, osr_height_);
        } else {
            rect.Set(0, 0, 800, 600); // Default fallback
            // printf("CEF OSR GetViewRect: fallback 800x600\n");
        }
    }

    void OnPaint(CefRefPtr<CefBrowser> browser,
                 PaintElementType type,
                 const RectList& dirtyRects,
                 const void* buffer,
                 int width,
                 int height) override {
        std::lock_guard<std::mutex> lock(osr_state_mutex_);
        if (!osr_enabled_ || !display_ || !x11_window_ || type != PET_VIEW) {
            printf("CEF OSR OnPaint: skipping (enabled=%d, display=%p, window=%lu, type=%d)\n", 
                   osr_enabled_, display_, x11_window_, type);
            return;
        }
        

        // Convert BGRA to ARGB format for X11
        const uint32_t* src = static_cast<const uint32_t*>(buffer);
        std::vector<uint32_t> converted_buffer(width * height);
        
        for (int i = 0; i < width * height; i++) {
            uint32_t bgra = src[i];
            uint32_t b = (bgra >> 0) & 0xFF;
            uint32_t g = (bgra >> 8) & 0xFF;
            uint32_t r = (bgra >> 16) & 0xFF;
            uint32_t a = (bgra >> 24) & 0xFF;
            
            // Convert BGRA to ARGB
            converted_buffer[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        
        // Get window attributes to ensure we have the right visual
        XWindowAttributes win_attrs = {};
        if (!XGetWindowAttributes(display_, x11_window_, &win_attrs)) {
            return;
        }
        
        // Create XImage with the window's visual for proper transparency support
        XImage* image = XCreateImage(display_,
                                   win_attrs.visual,
                                   win_attrs.depth, // Use window's depth
                                   ZPixmap,
                                   0,
                                   reinterpret_cast<char*>(converted_buffer.data()),
                                   width,
                                   height,
                                   32, // bitmap_pad
                                   width * 4); // bytes_per_line
        
        if (image) {
            // Create a GC compatible with the window's visual
            GC gc = XCreateGC(display_, x11_window_, 0, nullptr);
            
            // Draw the image to the window
            XPutImage(display_, x11_window_, gc,
                     image, 0, 0, 0, 0, width, height);
            
            XFlush(display_);
            XFreeGC(display_, gc);
            
            // Clean up (don't free the data since it's from converted_buffer)
            image->data = nullptr;
            XDestroyImage(image);
            
            
        }
        
       
    }

private:
    IMPLEMENT_REFCOUNTING(ElectrobunClient);
    DISALLOW_COPY_AND_ASSIGN(ElectrobunClient);
};

// Initialize static debounce timestamp for ctrl+click handling
double ElectrobunClient::lastCtrlClickTime = 0;

static std::mutex g_osrClientsMutex;
static std::map<uint32_t, CefRefPtr<ElectrobunClient>> g_osrClientsByWindowId;

static void registerOSRClientForWindow(uint32_t windowId, CefRefPtr<ElectrobunClient> client) {
    if (!windowId) return;
    std::lock_guard<std::mutex> lock(g_osrClientsMutex);
    if (client) {
        g_osrClientsByWindowId[windowId] = client;
    } else {
        g_osrClientsByWindowId.erase(windowId);
    }
}

static CefRefPtr<ElectrobunClient> getOSRClientForWindow(uint32_t windowId) {
    std::lock_guard<std::mutex> lock(g_osrClientsMutex);
    auto it = g_osrClientsByWindowId.find(windowId);
    return it != g_osrClientsByWindowId.end() ? it->second : nullptr;
}

static int cefEventModifiersFromXState(unsigned int state) {
    int modifiers = EVENTFLAG_NONE;
    if (state & ShiftMask) modifiers |= EVENTFLAG_SHIFT_DOWN;
    if (state & ControlMask) modifiers |= EVENTFLAG_CONTROL_DOWN;
    if (state & Mod1Mask) modifiers |= EVENTFLAG_ALT_DOWN;
    if (state & Button1Mask) modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (state & Button2Mask) modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    if (state & Button3Mask) modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    return modifiers;
}

static int cefButtonModifier(unsigned int button) {
    if (button == Button1) return EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (button == Button2) return EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    if (button == Button3) return EVENTFLAG_RIGHT_MOUSE_BUTTON;
    return EVENTFLAG_NONE;
}

struct OSRClickState {
    Window window = 0;
    unsigned int button = 0;
    Time time = 0;
    int x = 0;
    int y = 0;
    int clickCount = 0;
};

static std::map<Window, OSRClickState> g_osrClickStates;

static int osrClickCountForEvent(Window window, const XButtonEvent& buttonEvent, bool pressed) {
    auto& state = g_osrClickStates[window];
    if (!pressed) {
        return std::max(1, state.clickCount);
    }

    const Time doubleClickMs = 500;
    bool sameButton = state.window == window && state.button == buttonEvent.button;
    bool closeInTime = state.time != 0 && buttonEvent.time >= state.time && (buttonEvent.time - state.time) <= doubleClickMs;
    bool closeInSpace = std::abs(buttonEvent.x - state.x) <= 4 && std::abs(buttonEvent.y - state.y) <= 4;

    int clickCount = (sameButton && closeInTime && closeInSpace) ? std::min(state.clickCount + 1, 3) : 1;
    state.window = window;
    state.button = buttonEvent.button;
    state.time = buttonEvent.time;
    state.x = buttonEvent.x;
    state.y = buttonEvent.y;
    state.clickCount = clickCount;
    return clickCount;
}

static void forwardX11EventToOSRClient(const XEvent& event, Display* display, Window window, CefRefPtr<ElectrobunClient> client) {
    if (!client) return;

    CefRefPtr<CefBrowser> browser;
    {
        std::lock_guard<std::mutex> lock(g_cefBrowserMutex);
        browser = client->GetBrowser();
    }
    if (!browser) return;

    auto host = browser->GetHost();

    switch (event.type) {
        case ButtonPress:
        case ButtonRelease: {
            CefMouseEvent mouse_event;
            mouse_event.x = event.xbutton.x;
            mouse_event.y = event.xbutton.y;
            mouse_event.modifiers = cefEventModifiersFromXState(event.xbutton.state);

            if (event.xbutton.button == Button4 || event.xbutton.button == Button5) {
                if (event.type == ButtonPress) {
                    int deltaY = event.xbutton.button == Button4 ? 120 : -120;
                    host->SendMouseWheelEvent(mouse_event, 0, deltaY);
                }
                break;
            }

            cef_mouse_button_type_t button_type = MBT_LEFT;
            if (event.xbutton.button == Button3) button_type = MBT_RIGHT;
            else if (event.xbutton.button == Button2) button_type = MBT_MIDDLE;

            bool mouseUp = event.type == ButtonRelease;
            if (!mouseUp) {
                mouse_event.modifiers |= cefButtonModifier(event.xbutton.button);
            }
            int clickCount = osrClickCountForEvent(window, event.xbutton, !mouseUp);
            host->SendMouseClickEvent(mouse_event, button_type, mouseUp, clickCount);
            break;
        }
        case MotionNotify: {
            CefMouseEvent mouse_event;
            mouse_event.x = event.xmotion.x;
            mouse_event.y = event.xmotion.y;
            mouse_event.modifiers = cefEventModifiersFromXState(event.xmotion.state);
            host->SendMouseMoveEvent(mouse_event, false);
            break;
        }
        case FocusIn:
        case FocusOut:
            host->SetFocus(event.type == FocusIn);
            break;
        case EnterNotify:
            host->SetFocus(true);
            if (display && window) {
                XSetInputFocus(display, window, RevertToParent, CurrentTime);
            }
            break;
        case Expose:
            host->Invalidate(PET_VIEW);
            break;
    }
}

// Initialize CEF for Linux
bool initializeCEF() {
    if (g_cefInitialized) return true;
    
    if (!isCEFAvailable()) {
        printf("CEF not available in app bundle\n");
        return false;
    }
    
    // CRITICAL: Call gtk_disable_setlocale before any CEF operations
    // CEF internally calls GTK initialization, so we must do this first
    gtk_disable_setlocale();
    
    // Get command line arguments
    int argc = 0;
    char** argv = nullptr;
    
    // Read /proc/self/cmdline for arguments
    FILE* cmdline = fopen("/proc/self/cmdline", "r");
    if (cmdline) {
        fseek(cmdline, 0, SEEK_END);
        long size = ftell(cmdline);
        fseek(cmdline, 0, SEEK_SET);
        
        char* buffer = (char*)malloc(size + 1);
        fread(buffer, 1, size, cmdline);
        buffer[size] = '\0';
        fclose(cmdline);
        
        // Count arguments
        for (long i = 0; i < size; i++) {
            if (buffer[i] == '\0') argc++;
        }
        
        argv = (char**)malloc(sizeof(char*) * argc);
        int argIndex = 0;
        char* start = buffer;
        
        for (long i = 0; i <= size; i++) {
            if (buffer[i] == '\0') {
                argv[argIndex++] = strdup(start);
                start = buffer + i + 1;
            }
        }
        free(buffer);
    }
    
    CefMainArgs main_args(argc, argv);
    g_app = new ElectrobunApp();

    // Read user-defined chromium flags from build.json
    std::string buildJsonPath = getExecutableDir() + "/../Resources/build.json";
    std::string buildJsonContent = electrobun::readFileToString(buildJsonPath);
    if (!buildJsonContent.empty()) {
        g_userChromiumFlags = electrobun::parseChromiumFlags(buildJsonContent);
    }

    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;  // Required for OSR/transparent windows
    settings.log_severity = LOGSEVERITY_ERROR;  // Change to WARNING to see more CEF logs

    const auto remoteDebugging = electrobun::resolveRemoteDebugging(
        buildJsonContent,
        g_userChromiumFlags,
        getenv(electrobun::kRemoteDebuggingPortEnvironment));
    const int selectedPort = electrobun::selectRemoteDebuggingPort(
        remoteDebugging,
        IsPortAvailable);
    if (selectedPort != 0) {
        settings.remote_debugging_port = selectedPort;
        std::cout << "[CEF] Remote debugging enabled on 127.0.0.1:"
                  << selectedPort << " ("
                  << electrobun::remoteDebuggingSourceName(remoteDebugging.source)
                  << ")" << std::endl;
    } else if (remoteDebugging.enabled()) {
        std::cout << "[CEF] Remote debugging disabled: no free port in "
                  << electrobun::kDefaultRemoteDebuggingPort << "-"
                  << electrobun::kLastAutomaticRemoteDebuggingPort << std::endl;
    } else if (remoteDebugging.source == electrobun::RemoteDebuggingSource::invalid_configuration ||
               remoteDebugging.source == electrobun::RemoteDebuggingSource::invalid_environment) {
        std::cout << "[CEF] Remote debugging disabled: "
                  << electrobun::remoteDebuggingSourceName(remoteDebugging.source)
                  << std::endl;
    }
    
    // Use centralized GTK initialization to ensure proper setlocale handling
    initializeGTK();
    
    // Set the resource directory to where CEF files are located
    std::string execDir = getExecutableDir();
    // Resources are in the same directory as the executable for Linux
    CefString(&settings.resources_dir_path) = execDir;
    CefString(&settings.locales_dir_path) = execDir + "/locales";
    
    // Match the helper name to the actual host executable (Cottontail or native main).
    CefString(&settings.browser_subprocess_path) =
        execDir + "/" + getExecutableBaseName() + " Helper";
    
    // Set cache path with identifier/channel structure (consistent with CLI and updater)
    // Use ~/.cache/identifier/channel/CEF
    char* home = getenv("HOME");
    if (home) {
        std::string basePath = std::string(home) + "/.cache";
        std::string cachePath = buildAppDataPath(basePath, g_electrobunIdentifier, g_electrobunChannel, "CEF");
        std::cout << "[CEF] Using path: " << cachePath << std::endl;

        // One-shot wipe if Electrobun's cache format version has been bumped
        // since the user's last launch. See cache_migration.h.
        electrobun::migrateCacheFolderIfNeeded(cachePath);

        CefString(&settings.root_cache_path) = cachePath;
    }
    
    // Set language
    CefString(&settings.accept_language_list) = "en-US,en";
    
    bool result = CefInitialize(main_args, settings, g_app.get(), nullptr);

    // Cleanup
    if (argv) {
        for (int i = 0; i < argc; i++) {
            free(argv[i]);
        }
        free(argv);
    }
    
    if (!result) {
        printf("CEF: [INIT] ERROR - CefInitialize failed\n");
        return false;
    } 
    
    g_cefInitialized = true;
    // printf("CEF initialized successfully\n");
    return true;
}

CefRefPtr<CefClient> create_default_handler() {
  class SimpleClient : public CefClient, public CefLifeSpanHandler {
  public:
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    void OnAfterCreated(CefRefPtr<CefBrowser>) override {}
    IMPLEMENT_REFCOUNTING(SimpleClient);
  };
  return new SimpleClient();
}


