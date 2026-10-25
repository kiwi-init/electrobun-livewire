// Ensure the exported functions have appropriate visibility
#define ELECTROBUN_EXPORT __attribute__((visibility("default")))

static std::once_flag g_xlibThreadsOnce;
static std::atomic<bool> g_xlibThreadsInitialized{false};

static void ensureXlibThreadSupport() {
    std::call_once(g_xlibThreadsOnce, []() {
        g_xlibThreadsInitialized.store(XInitThreads() != 0);
    });
    if (!g_xlibThreadsInitialized.load()) {
        fprintf(stderr, "FATAL: XInitThreads failed before X11 initialization\n");
        abort();
    }
}

// X11 Error Handler (non-fatal errors are common in WebKit/GTK)
static int x11_error_handler(Display* display, XErrorEvent* error) {
    // Only log severe errors, ignore common ones like BadWindow for destroyed widgets
    if (error->error_code != BadWindow && error->error_code != BadDrawable) {
        char error_text[256];
        XGetErrorText(display, error->error_code, error_text, sizeof(error_text));
        fprintf(stderr, "X11 Error: %s (code %d)\n", error_text, error->error_code);
    }
    return 0; // Continue execution
}

// Helper macros
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// OOPIF positioning - GTK uses separate containers instead of offscreen positioning

// Forward declare callback types
typedef void (*WindowCloseCallback)(uint32_t windowId);
typedef void (*WindowMoveCallback)(uint32_t windowId, double x, double y);
typedef void (*WindowResizeCallback)(uint32_t windowId, double x, double y, double width, double height);
typedef void (*WindowFocusCallback)(uint32_t windowId);
typedef void (*WindowBlurCallback)(uint32_t windowId);

// Forward declaration for WebKit scheme handler
static void handleViewsURIScheme(WebKitURISchemeRequest* request, gpointer user_data);

// Forward declaration for partition context management
static WebKitWebContext* getContextForPartition(const char* partitionIdentifier);
static void releaseContextForPartition(const std::string& partition);


// Webview and tray callback types are defined in shared/callbacks.h
// Platform-specific alias
typedef StatusItemHandler ZigStatusItemHandler;

// Menu item structure
struct MenuItemData {
    uint32_t menuId;
    std::string action;
    std::string type;
    ZigStatusItemHandler clickHandler;
};

// Global menu item counter and storage
static uint32_t g_nextMenuId = 1;
static std::map<uint32_t, std::shared_ptr<MenuItemData>> g_menuItems;
static std::mutex g_menuItemsMutex;

// Global application menu storage
static std::string g_applicationMenuConfig;
static ZigStatusItemHandler g_applicationMenuHandler = nullptr;

// Webview content storage (replaces JSCallback approach)
static std::map<uint32_t, std::string> webviewHTMLContent;
static std::mutex webviewHTMLMutex;

// Global variables for CEF cache path isolation
static std::string g_electrobunChannel = "";
static std::string g_electrobunIdentifier = "";
static std::string g_electrobunName = "";
static std::string g_electrobunWindowClass = "Electrobun";

static std::string deriveLinuxWindowClass(
    const std::string& name,
    const std::string& channel) {
    std::string result;
    result.reserve(name.size() + channel.size() + 1);
    for (char character : name) {
        if (character != ' ') {
            result.push_back(character);
        }
    }
    if (result.empty()) {
        result = "Electrobun";
    }
    if (!channel.empty() && channel != "stable") {
        // version.json.name is already the channel-qualified bundle name.
        // Also accept unqualified names from callers supplying AppInfo directly.
        const std::string suffix = "-" + channel;
        if (result.size() < suffix.size() ||
            result.compare(result.size() - suffix.size(), suffix.size(), suffix) != 0) {
            result += suffix;
        }
    }
    return result;
}

// The launcher sets this private marker only for the exact `--automation`
// opt-in. WebKitGTK permits automation on one context per process, so the first
// context used by an Electrobun WebKit view becomes the sole automation context.
static constexpr const char* kWebKitAutomationEnvironment = "ELECTROBUN_WEBKIT_AUTOMATION";
static constexpr const char* kWebKitAutomationInspectorServerEnvironment =
    "ELECTROBUN_WEBKIT_AUTOMATION_INSPECTOR_SERVER";
static WebKitWebContext* g_webKitAutomationContext = nullptr;
static WebKitWebView* g_webKitAutomationTarget = nullptr;
static bool g_webKitAutomationConfigured = false;
static bool g_webKitAutomationInspectorServerRestored = false;

// Forward declarations for HTML content management
extern "C" ELECTROBUN_EXPORT const char* getWebviewHTMLContent(uint32_t webviewId);
extern "C" ELECTROBUN_EXPORT void setWebviewHTMLContent(uint32_t webviewId, const char* htmlContent);

// MIME type detection function is in shared/mime_types.h
// Permission cache types and functions are in shared/permissions.h

// Linux-specific permission request helper
std::string getOriginFromPermissionRequest(WebKitPermissionRequest* request) {
    // For views:// scheme, use a constant origin since these are local files
    // For other schemes, you would use webkit_permission_request_get_requesting_origin() when available
    return "views://";
}

// Menu JSON structure is now defined in shared/json_menu_parser.h
// Alias for backward compatibility with existing code
using MenuJsonValue = MenuItemJson;

// Forward declarations
class ContainerView;
class CEFWebViewImpl;
std::string getExecutableDir();
std::string getExecutableBaseName();
GtkWidget* getContainerViewOverlay(GtkWidget* window);
GtkWidget* createMenuFromParsedItems(const std::vector<MenuJsonValue>& items, ZigStatusItemHandler clickHandler, uint32_t trayId);
GtkWidget* createApplicationMenuBar(const std::vector<MenuJsonValue>& items, ZigStatusItemHandler clickHandler);
void applyApplicationMenuToWindow(GtkWidget* window);
void initializeGTK();

// X11 Window structure to replace GTK windows
struct X11Window {
    Display* display;
    Window window;
    uint32_t windowId;
    double x, y, width, height;
    std::string title;
    WindowCloseCallback closeCallback;
    WindowShouldCloseHandler shouldCloseCallback;
    WindowMoveCallback moveCallback;
    WindowResizeCallback resizeCallback;
    WindowFocusCallback focusCallback;
    WindowBlurCallback blurCallback;
    WindowKeyHandler keyCallback;
    std::vector<Window> childWindows;  // For managing webviews
    ContainerView* containerView = nullptr;  // Associated container for webview management
    bool transparent = false;  // Track if window is transparent

    // The WM applies move/resize requests asynchronously, so an immediate
    // XGetWindowAttributes still reports the old geometry. Report the latest
    // request until a ConfigureNotify confirms it (or a WM adjustment wins).
    struct RequestedGeometry {
        bool hasPosition = false;
        bool hasSize = false;
        double x = 0, y = 0, width = 0, height = 0;
        gint64 requestedAtUs = 0;
    } requestedGeometry;

    void requestPosition(double newX, double newY) {
        requestedGeometry.hasPosition = true;
        requestedGeometry.x = newX;
        requestedGeometry.y = newY;
        requestedGeometry.requestedAtUs = g_get_monotonic_time();
    }

    void requestSize(double newWidth, double newHeight) {
        requestedGeometry.hasSize = true;
        requestedGeometry.width = newWidth;
        requestedGeometry.height = newHeight;
        requestedGeometry.requestedAtUs = g_get_monotonic_time();
    }

    // Called with WM-confirmed geometry from ConfigureNotify.
    void settleRequestedGeometry(double actualX, double actualY, double actualWidth, double actualHeight) {
        auto near = [](double a, double b) { return std::abs(a - b) <= 1.0; };
        const bool expired =
            g_get_monotonic_time() - requestedGeometry.requestedAtUs > 1000000;
        if (requestedGeometry.hasPosition &&
            (expired || (near(actualX, requestedGeometry.x) && near(actualY, requestedGeometry.y)))) {
            requestedGeometry.hasPosition = false;
        }
        if (requestedGeometry.hasSize &&
            (expired || (near(actualWidth, requestedGeometry.width) && near(actualHeight, requestedGeometry.height)))) {
            requestedGeometry.hasSize = false;
        }
    }

    X11Window() : display(nullptr), window(0), windowId(0), x(0), y(0), width(800), height(600), closeCallback(nullptr), shouldCloseCallback(nullptr), moveCallback(nullptr), resizeCallback(nullptr), focusCallback(nullptr), blurCallback(nullptr), keyCallback(nullptr), transparent(false) {}
};

// Forward declarations for icon management
static void autoSetWindowIcon(void* window);
static void setX11WindowIcon(X11Window* x11win, GdkPixbuf* pixbuf);
extern "C" void stopWgpuTestForWindow(Window window);

// Forward declaration for X11 menu function
void applyApplicationMenuToX11Window(X11Window* x11win);

// Use parseMenuJson from shared/json_menu_parser.h
using electrobun::parseMenuJson;

// Mask rectangle structure for X11 regions
struct MaskRect {
    double x, y, width, height;
};

// Parse maskJSON string into rectangles
std::vector<MaskRect> parseMaskJson(const std::string& jsonStr) {
    std::vector<MaskRect> rects;
    
    
    if (jsonStr.empty()) {
        return rects;
    }
    
    // Handle double-escaped JSON by unescaping quotes
    std::string unescapedJson = jsonStr;
    size_t pos = 0;
    while ((pos = unescapedJson.find("\\\"", pos)) != std::string::npos) {
        unescapedJson.replace(pos, 2, "\"");
        pos += 1;
    }
    
    // Simple JSON parser for rectangle arrays
    // Looking for patterns like [{"x":10,"y":20,"width":100,"height":50}]
    size_t parsePos = 0;
    while (parsePos < unescapedJson.length()) {
        size_t objStart = unescapedJson.find("{", parsePos);
        if (objStart == std::string::npos) break;
        
        size_t objEnd = unescapedJson.find("}", objStart);
        if (objEnd == std::string::npos) break;
        
        std::string obj = unescapedJson.substr(objStart, objEnd - objStart + 1);
        
        MaskRect rect = {};
        
        // Geometry originates in getBoundingClientRect(), so retain fractional
        // DIPs until the final X11 conversion.
        // Parse x
        size_t xPos = obj.find("\"x\":");
        if (xPos != std::string::npos) {
            size_t valueStart = obj.find_first_of("0123456789-.", xPos + 4);
            if (valueStart != std::string::npos) {
                rect.x = strtod(obj.c_str() + valueStart, nullptr);
            }
        }
        
        // Parse y
        size_t yPos = obj.find("\"y\":");
        if (yPos != std::string::npos) {
            size_t valueStart = obj.find_first_of("0123456789-.", yPos + 4);
            if (valueStart != std::string::npos) {
                rect.y = strtod(obj.c_str() + valueStart, nullptr);
            }
        }
        
        // Parse width
        size_t widthPos = obj.find("\"width\":");
        if (widthPos != std::string::npos) {
            size_t valueStart = obj.find_first_of("0123456789-.", widthPos + 8);
            if (valueStart != std::string::npos) {
                rect.width = strtod(obj.c_str() + valueStart, nullptr);
            }
        }
        
        // Parse height
        size_t heightPos = obj.find("\"height\":");
        if (heightPos != std::string::npos) {
            size_t valueStart = obj.find_first_of("0123456789-.", heightPos + 9);
            if (valueStart != std::string::npos) {
                rect.height = strtod(obj.c_str() + valueStart, nullptr);
            }
        }
        
        rects.push_back(rect);
        parsePos = objEnd + 1;
    }
    
    return rects;
}

// Check if a point is within any of the mask rectangles
bool isPointInMask(double x, double y, const std::vector<MaskRect>& masks) {
    for (const auto& mask : masks) {
        if (x >= mask.x && x < mask.x + mask.width &&
            y >= mask.y && y < mask.y + mask.height) {
            return true;
        }
    }
    return false;
}

// Note: X11 window extraction removed - WebKit now uses GTK-native input shape masking

// Forward declarations
class AbstractView;

// Helper function to check navigation rules - defined after AbstractView class
bool checkNavigationRules(std::shared_ptr<AbstractView> view, const std::string& url);

// CEF globals and implementation
static std::atomic<bool> g_cefInitialized{false};
static std::atomic<bool> g_useCEF{false};
static std::atomic<bool> g_checkedForCEF{false};

// Browser views and WGPU views have independent ID allocators. Keep their
// ownership registries separate so equal IDs cannot replace one another.
static std::map<uint32_t, std::shared_ptr<AbstractView>> g_webviewMap;
static std::mutex g_webviewMapMutex;
static std::map<uint32_t, std::shared_ptr<AbstractView>> g_wgpuViewMap;
static std::mutex g_wgpuViewMapMutex;
static std::map<uint32_t, std::string> g_webviewViewsRoot;
static std::mutex g_webviewViewsRootMutex;
static constexpr const char* kWebviewIdDataKey = "electrobun-webview-id";
struct AllowedProtocols { bool views = true; bool appData = false; };
static std::map<uint32_t, AllowedProtocols> g_allowedProtocols;
static std::mutex g_allowedProtocolsMutex;

static bool protocolAllowed(uint32_t webviewId, bool appData) {
    std::lock_guard<std::mutex> lock(g_allowedProtocolsMutex);
    auto it = g_allowedProtocols.find(webviewId);
    return it != g_allowedProtocols.end() && (appData ? it->second.appData : it->second.views);
}

static std::filesystem::path appDataRoot() {
    const char* xdg = g_getenv("XDG_DATA_HOME");
    std::string base;
    if (xdg && *xdg) base = xdg;
    else if (const char* home = g_get_home_dir()) base = std::string(home) + "/.local/share";
    // Only userData/appdata is served: the rest of userData stays private.
    return buildAppDataPath(base, g_electrobunIdentifier, g_electrobunChannel, "appdata");
}

static bool readContainedFile(const std::filesystem::path& rootPath,
                              const std::string& relative,
                              std::string& data) {
    if (relative.empty()) return false;
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(rootPath, ec);
    if (ec) return false;
    const auto target = std::filesystem::weakly_canonical(root / relative, ec);
    if (ec) return false;
    auto rootIt = root.begin(), targetIt = target.begin();
    for (; rootIt != root.end(); ++rootIt, ++targetIt) {
        if (targetIt == target.end() || *rootIt != *targetIt) return false;
    }
    if (!std::filesystem::is_regular_file(target, ec) || ec) return false;
    std::ifstream stream(target, std::ios::binary);
    if (!stream) return false;
    data.assign(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
    return true;
}

// CefShutdown requires every browser to have completed OnBeforeClose first.
// Track browsers independently of g_webviewMap because a removed view can keep
// closing asynchronously after its owner has been erased from that map.
static std::map<int, CefRefPtr<CefBrowser>> g_liveCefBrowsers;
static std::mutex g_liveCefBrowsersMutex;
static std::atomic<int> g_pendingCefBrowserCreations{0};

// Global map to store preload scripts by browser ID (for multi-process CEF)
static std::map<int, std::string> g_preloadScripts;

CefRefPtr<class ElectrobunApp> g_app;
electrobun::ChromiumFlagConfig g_userChromiumFlags;

static bool IsPortAvailable(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));

    const int result = bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    close(sock);
    return result == 0;
}


// Get the directory of the current executable
std::string getExecutableDir() {
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len != -1) {
        path[len] = '\0';
        std::string exePath(path);
        size_t lastSlash = exePath.find_last_of('/');
        if (lastSlash != std::string::npos) {
            return exePath.substr(0, lastSlash);
        }
    }
    return "."; // fallback to current directory
}

std::string getExecutableBaseName() {
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len != -1) {
        path[len] = '\0';
        std::string exePath(path);
        size_t lastSlash = exePath.find_last_of('/');
        std::string fileName = lastSlash != std::string::npos
            ? exePath.substr(lastSlash + 1)
            : exePath;
        if (!fileName.empty()) {
            return fileName;
        }
    }
    return "bun";
}

static bool x11GetWindowAttributesSafe(Display* display, Window window, XWindowAttributes* outAttrs) {
    if (!display || !window || !outAttrs) return false;
    // The process-wide handler installed by initializeGTK ignores BadWindow.
    // Do not swap Xlib's global handler here: another Display may be painting
    // concurrently on CEF's UI thread.
    return XGetWindowAttributes(display, window, outAttrs) != 0;
}

static void x11DestroyWindowSafe(Display* display, Window window) {
    if (!display || !window) return;
    XDestroyWindow(display, window);
    XSync(display, False);
}

// CEF availability check - runtime check for CEF files in app bundle
bool isCEFAvailable() {
    // Return cached result if we've already checked
    if (g_checkedForCEF) {
        return g_useCEF;
    }
    
    // Perform the check once and cache the result
    // Get the directory where the executable is located
    std::string execDir = getExecutableDir();
    
    // Check for CEF shared library in cef subdirectory (where it's bundled)
    std::string cefLibPath = execDir + "/cef/libcef.so";
   
    // Check if the CEF library file exists
    if (access(cefLibPath.c_str(), F_OK) == 0) {
        g_useCEF = true;
    } else {
        // Also check for CEF in main directory (fallback)
        cefLibPath = execDir + "/libcef.so";
        if (access(cefLibPath.c_str(), F_OK) == 0) {
            g_useCEF = true;
        } else {
            g_useCEF = false;
        }
    }
    
    // Mark that we've performed the check
    g_checkedForCEF = true;
    
    return g_useCEF;
}




