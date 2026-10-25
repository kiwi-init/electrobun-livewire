// AbstractView base class declaration
class AbstractView {
public:
    uint32_t webviewId;
    GtkWidget* widget = nullptr;
    bool isMousePassthroughEnabled = false;
    bool mirrorModeEnabled = false;
    bool fullSize = false;
    bool pendingStartTransparent = false;
    bool pendingStartPassthrough = false;
    bool isReceivingInput = true;
    bool isRemoved = false;  // Flag to prevent operations on removed webviews
    std::string maskJSON;
    GdkRectangle visualBounds = {};
    bool creationFailed = false;

    // Pending resize state (cross-thread)
    std::mutex pendingResizeMutex;
    std::atomic<uint64_t> pendingResizeGeneration{0};
    uint64_t appliedResizeGeneration = 0;
    bool hasPendingResize = false;
    LogicalRect pendingResizeFrame = {};
    std::string pendingResizeMasks;

    // Navigation rules for URL filtering
    std::vector<std::string> navigationRules;
    
    // Root directory for views:// protocol resolution
    std::string viewsRoot;

    AbstractView(uint32_t webviewId) : webviewId(webviewId) {}
    virtual ~AbstractView() {}

    // Set navigation rules from JSON array string
    void setNavigationRulesFromJSON(const char* rulesJson) {
        navigationRules.clear();
        if (!rulesJson || strlen(rulesJson) == 0) {
            return;
        }

        // Simple JSON array parser for string arrays: ["rule1", "rule2", ...]
        std::string json(rulesJson);
        size_t pos = json.find('[');
        if (pos == std::string::npos) return;

        pos++;
        while (pos < json.length()) {
            // Find start of string
            size_t strStart = json.find('"', pos);
            if (strStart == std::string::npos) break;

            // Find end of string (handle escaped quotes)
            size_t strEnd = strStart + 1;
            while (strEnd < json.length()) {
                if (json[strEnd] == '"' && json[strEnd - 1] != '\\') break;
                strEnd++;
            }
            if (strEnd >= json.length()) break;

            // Extract string value
            std::string rule = json.substr(strStart + 1, strEnd - strStart - 1);
            navigationRules.push_back(rule);

            pos = strEnd + 1;
        }
    }

    // Check if URL should be allowed based on navigation rules
    bool shouldAllowNavigationToURL(const std::string& url) {
        if (navigationRules.empty()) {
            return true; // Default allow if no rules
        }

        bool allowed = true; // Default allow if no rules match

        for (const std::string& rule : navigationRules) {
            bool isBlockRule = !rule.empty() && rule[0] == '^';
            std::string pattern = isBlockRule ? rule.substr(1) : rule;

            if (electrobun::globMatch(pattern, url)) {
                allowed = !isBlockRule; // Last match wins
            }
        }

        return allowed;
    }
    
    // Pure virtual methods that must be implemented by derived classes
    virtual void loadURL(const char* urlString) = 0;
    virtual void loadHTML(const char* htmlString) = 0;
    virtual void goBack() = 0;
    virtual void goForward() = 0;
    virtual void reload() = 0;
    virtual void remove() = 0;
    virtual bool canGoBack() = 0;
    virtual bool canGoForward() = 0;
    // Kill the renderer process; false where the renderer can't be killed.
    virtual bool terminateRenderer() { return false; }
    virtual void evaluateJavaScriptWithNoCompletion(const char* jsString) = 0;
    virtual void callAsyncJavascript(const char* messageId, const char* jsString, uint32_t webviewId, uint32_t hostWebviewId, void* completionHandler) = 0;
    virtual void addPreloadScriptToWebView(const char* jsString) = 0;
    virtual void updateCustomPreloadScript(const char* jsString) = 0;
    virtual void resize(const GdkRectangle& frame, const char* masksJson) = 0;
    virtual void resizeLogical(const LogicalRect& frame, const char* masksJson) {
        const GdkRectangle integerFrame = {
            static_cast<int>(frame.x),
            static_cast<int>(frame.y),
            static_cast<int>(frame.width),
            static_cast<int>(frame.height),
        };
        resize(integerFrame, masksJson);
    }
    virtual void applyVisualMask() = 0;
    virtual void removeMasks() = 0;
    virtual void toggleMirrorMode(bool enable) = 0;
    
    // Common methods with default implementation
    virtual void setTransparent(bool transparent) {}
    virtual void setPassthrough(bool enable) { isMousePassthroughEnabled = enable; }
    virtual void setHidden(bool hidden) {}

    // Find in page methods
    virtual void findInPage(const char* searchText, bool forward, bool matchCase) = 0;
    virtual void stopFindInPage() = 0;

    // Developer tools methods
    virtual void openDevTools() = 0;
    virtual void closeDevTools() = 0;
    virtual void toggleDevTools() = 0;

    void storePendingResize(const LogicalRect& frame, const char* masksJson) {
        std::lock_guard<std::mutex> lock(pendingResizeMutex);
        pendingResizeFrame = frame;
        pendingResizeMasks = masksJson ? masksJson : "";
        hasPendingResize = true;
        pendingResizeGeneration++;
    }

    bool consumePendingResize(LogicalRect& outFrame, std::string& outMasks) {
        std::lock_guard<std::mutex> lock(pendingResizeMutex);
        if (!hasPendingResize) return false;
        uint64_t gen = pendingResizeGeneration.load();
        if (gen == appliedResizeGeneration) return false;
        outFrame = pendingResizeFrame;
        outMasks = pendingResizeMasks;
        appliedResizeGeneration = gen;
        hasPendingResize = false;
        return true;
    }
};

// Pending resize queue (cross-thread)
static PendingResizeQueue g_pendingResizeQueue;
static std::atomic<bool> g_pendingResizeScheduled{false};

static void drainPendingResizes() {
    g_pendingResizeScheduled.store(false);
    auto items = g_pendingResizeQueue.drain();
    for (void* item : items) {
        AbstractView* view = static_cast<AbstractView*>(item);
        if (!view) continue;
        LogicalRect frame = {};
        std::string masks;
        if (view->consumePendingResize(frame, masks)) {
            view->resizeLogical(frame, masks.c_str());
        }
    }
}

static void schedulePendingResizeDrain() {
    if (g_pendingResizeScheduled.exchange(true)) return;
    g_idle_add([](gpointer) -> gboolean {
        drainPendingResizes();
        return G_SOURCE_REMOVE;
    }, nullptr);
}

// Helper function implementation - calls AbstractView's navigation rules method
bool checkNavigationRules(std::shared_ptr<AbstractView> view, const std::string& url) {
    return view->shouldAllowNavigationToURL(url);
}

static bool isWebKitAutomationRequested() {
    const char* value = getenv(kWebKitAutomationEnvironment);
    return value && strcmp(value, "1") == 0;
}

static void restoreWebKitAutomationInspectorServer() {
    if (!isWebKitAutomationRequested()) {
        return;
    }

    const char* server = getenv(kWebKitAutomationInspectorServerEnvironment);
    if (!server || server[0] == '\0') {
        return;
    }

    setenv("WEBKIT_INSPECTOR_SERVER", server, 1);
    unsetenv(kWebKitAutomationInspectorServerEnvironment);
    g_webKitAutomationInspectorServerRestored = true;
}

static void onWebKitAutomationContextDestroyed(gpointer, GObject*) {
    g_webKitAutomationContext = nullptr;
    g_webKitAutomationConfigured = false;
}

static bool selectWebKitAutomationContext(WebKitWebContext* context) {
    if (!context || !isWebKitAutomationRequested()) {
        return false;
    }

    if (!g_webKitAutomationContext) {
        g_webKitAutomationContext = context;
        g_object_weak_ref(
            G_OBJECT(context),
            onWebKitAutomationContextDestroyed,
            nullptr);
    }

    return context == g_webKitAutomationContext;
}

static WebKitWebView* onWebKitAutomationCreateWebView(
    WebKitAutomationSession*,
    gpointer) {
    if (!g_webKitAutomationTarget ||
        !webkit_web_view_is_controlled_by_automation(g_webKitAutomationTarget)) {
        fprintf(stderr, "ERROR: WebKitGTK automation has no controlled app view\n");
        return nullptr;
    }

    printf("[WebKit] W3C automation attached to the primary app view\n");
    fflush(stdout);
    return g_webKitAutomationTarget;
}

static void onWebKitAutomationStarted(
    WebKitWebContext*,
    WebKitAutomationSession* session,
    gpointer) {
    printf("[WebKit] W3C automation session requested\n");
    fflush(stdout);

    WebKitApplicationInfo* info = webkit_application_info_new();
    webkit_application_info_set_name(
        info,
        g_electrobunIdentifier.empty()
            ? "Electrobun"
            : g_electrobunIdentifier.c_str());
    webkit_automation_session_set_application_info(session, info);
    webkit_application_info_unref(info);

    g_signal_connect(
        session,
        "create-web-view",
        G_CALLBACK(onWebKitAutomationCreateWebView),
        nullptr);
}

static void configureWebKitAutomation(
    WebKitWebContext* context,
    WebKitWebView* target,
    bool isControlledByAutomation) {
    if (!isControlledByAutomation || context != g_webKitAutomationContext) {
        return;
    }

    if (!g_webKitAutomationTarget) {
        g_webKitAutomationTarget = target;
        g_object_add_weak_pointer(
            G_OBJECT(target),
            reinterpret_cast<gpointer*>(&g_webKitAutomationTarget));
    }

    if (g_webKitAutomationConfigured) {
        return;
    }

    g_signal_connect(
        context,
        "automation-started",
        G_CALLBACK(onWebKitAutomationStarted),
        nullptr);
    webkit_web_context_set_automation_allowed(context, TRUE);
    g_webKitAutomationConfigured =
        webkit_web_context_is_automation_allowed(context) == TRUE;

    // Give WebKitGTK's remote-inspector server one main-loop turn to consume
    // the endpoint, then hide it from JavaScriptCore VMs created later.
    if (g_webKitAutomationInspectorServerRestored) {
        g_webKitAutomationInspectorServerRestored = false;
        g_timeout_add(1000, [](gpointer) -> gboolean {
            unsetenv("WEBKIT_INSPECTOR_SERVER");
            return G_SOURCE_REMOVE;
        }, nullptr);
    }

    if (g_webKitAutomationConfigured) {
        printf("[WebKit] W3C automation enabled for the primary app view\n");
        fflush(stdout);
    } else {
        fprintf(stderr, "ERROR: WebKitGTK automation could not be enabled\n");
    }
}

