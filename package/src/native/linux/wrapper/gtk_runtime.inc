void initializeGTK() {
    {
        std::unique_lock<std::mutex> lock(g_gtkInitMutex);
        if (!g_gtkInitialized) {
            // Shared Display contract: XInitThreads must precede gtk_init/CEF
            // and every XOpenDisplay. Each X11Window owns its Display, ordinary
            // event/mutation work runs on the GLib main context, and OSR paint
            // and cursor callbacks are the deliberate cross-thread users. Xlib
            // serializes individual calls; Electrobun map locks only protect
            // C++ lifetimes and are never held across Xlib/CEF/application
            // callbacks. OSR's state mutex closes the parent/paint race.
            ensureXlibThreadSupport();

            // CEF renders through X11 (--ozone-platform=x11), so its windows
            // need GDK's X11 backend, on Wayland through XWayland. WebKitGTK
            // windows are plain GTK windows: leave GDK_BACKEND to the user,
            // and GTK opens Wayland when WAYLAND_DISPLAY is set, X11 if not.
            if (isCEFAvailable()) {
                setenv("GDK_BACKEND", "x11", 1);
            }

            // File dialogs go through the desktop's FileChooser portal when
            // one exists (GTK's own dialog stays the fallback).
            electrobun::xdg_portal::preferPortalFileChooser();

            // Disable setlocale before gtk_init to prevent CEF conflicts
            gtk_disable_setlocale();
            gtk_init(nullptr, nullptr);
            
            // Install X11 error handler for debugging
            XSetErrorHandler(x11_error_handler);
            
            g_gtkInitialized = true;
            
        }
    }
    // Notify all waiting threads that GTK is initialized
    g_gtkInitCondition.notify_all();
}

// Helper function to wait for GTK initialization
void waitForGTKInit() {
    std::unique_lock<std::mutex> lock(g_gtkInitMutex);
    g_gtkInitCondition.wait(lock, []{ return g_gtkInitialized; });
}

// Helper function to dispatch to main thread synchronously
template<typename Func>
auto dispatch_sync_main(Func&& func) -> decltype(func()) {
    using ReturnType = decltype(func());
    
    // If already on main thread, just execute
    if (isEventLoopThread()) {
        return func();
    }
    
    // Structure to hold the function and result
    struct DispatchData {
        Func func;
        ReturnType result;
        GMutex mutex;
        GCond cond;
        bool completed;
        std::exception_ptr exception;
        
        DispatchData(Func&& f) : func(std::forward<Func>(f)), completed(false) {
            g_mutex_init(&mutex);
            g_cond_init(&cond);
        }
        
        ~DispatchData() {
            g_mutex_clear(&mutex);
            g_cond_clear(&cond);
        }
    };
    
    auto data = std::make_unique<DispatchData>(std::forward<Func>(func));
    
    // Lambda to run on main thread
    auto callback = [](gpointer user_data) -> gboolean {
        auto* dispatch_data = static_cast<DispatchData*>(user_data);
        
        try {
            dispatch_data->result = dispatch_data->func();
        } catch (...) {
            dispatch_data->exception = std::current_exception();
        }
        
        g_mutex_lock(&dispatch_data->mutex);
        dispatch_data->completed = true;
        g_cond_signal(&dispatch_data->cond);
        g_mutex_unlock(&dispatch_data->mutex);
        
        return G_SOURCE_REMOVE;
    };
    
    // Schedule on main thread
    g_idle_add(callback, data.get());
    
    // Wait for completion
    g_mutex_lock(&data->mutex);
    while (!data->completed) {
        g_cond_wait(&data->cond, &data->mutex);
    }
    g_mutex_unlock(&data->mutex);
    
    // Rethrow any exception that occurred
    if (data->exception) {
        std::rethrow_exception(data->exception);
    }
    
    return data->result;
}

// Helper for void functions
template<typename Func>
typename std::enable_if<std::is_void<decltype(std::declval<Func>()())>::value>::type
dispatch_sync_main_void(Func&& func) {
    if (isEventLoopThread()) {
        func();
        return;
    }
    
    struct DispatchData {
        Func func;
        GMutex mutex;
        GCond cond;
        bool completed;
        std::exception_ptr exception;
        
        DispatchData(Func&& f) : func(std::forward<Func>(f)), completed(false) {
            g_mutex_init(&mutex);
            g_cond_init(&cond);
        }
        
        ~DispatchData() {
            g_mutex_clear(&mutex);
            g_cond_clear(&cond);
        }
    };
    
    auto data = std::make_unique<DispatchData>(std::forward<Func>(func));
    
    auto callback = [](gpointer user_data) -> gboolean {
        auto* dispatch_data = static_cast<DispatchData*>(user_data);
        
        try {
            dispatch_data->func();
        } catch (...) {
            dispatch_data->exception = std::current_exception();
        }
        
        g_mutex_lock(&dispatch_data->mutex);
        dispatch_data->completed = true;
        g_cond_signal(&dispatch_data->cond);
        g_mutex_unlock(&dispatch_data->mutex);
        
        return G_SOURCE_REMOVE;
    };
    
    g_idle_add(callback, data.get());
    
    g_mutex_lock(&data->mutex);
    while (!data->completed) {
        g_cond_wait(&data->cond, &data->mutex);
    }
    g_mutex_unlock(&data->mutex);
    
    if (data->exception) {
        std::rethrow_exception(data->exception);
    }
}

template<typename Func>
void dispatch_async_main_void(Func&& func) {
    if (isEventLoopThread()) {
        func();
        return;
    }

    using FuncType = typename std::decay<Func>::type;
    struct DispatchData {
        FuncType func;
        explicit DispatchData(Func&& f) : func(std::forward<Func>(f)) {}
    };

    auto data = new DispatchData(std::forward<Func>(func));
    g_idle_add([](gpointer user_data) -> gboolean {
        std::unique_ptr<DispatchData> dispatch_data(static_cast<DispatchData*>(user_data));
        dispatch_data->func();
        return G_SOURCE_REMOVE;
    }, data);
}

// Store for partition-specific contexts (for session storage synchronization)
static std::map<std::string, WebKitWebContext*> g_partitionContexts;
// Non-persistent WebKit partitions should share while live, then reset after the last webview closes.
static std::map<std::string, size_t> g_ephemeralPartitionContextRefCounts;

static bool isPersistentWebKitPartition(const std::string& partition) {
    return partition.compare(0, 8, "persist:") == 0;
}

// Helper function to automatically set window icon from standard location
static void autoSetWindowIcon(void* window) {
    if (!window) return;
    
    // Standard icon location: Resources/appIcon.png
    const char* iconPath = "Resources/appIcon.png";
    
    // Check if icon exists
    struct stat buffer;
    if (stat(iconPath, &buffer) != 0) {
        // Icon doesn't exist, nothing to do
        return;
    }
    
    GError* error = nullptr;
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(iconPath, &error);
    
    if (pixbuf) {
        if (GTK_IS_WIDGET(window)) {
            gtk_window_set_icon(GTK_WINDOW(window), pixbuf);
        } else {
            // For X11/CEF windows
            X11Window* x11win = static_cast<X11Window*>(window);
            if (x11win && x11win->display && x11win->window) {
                setX11WindowIcon(x11win, pixbuf);
            }
        }
        g_object_unref(pixbuf);
    } else {
        if (error) {
            g_error_free(error);
        }
    }
}

// Helper function to set X11 window icon from GdkPixbuf
static void setX11WindowIcon(X11Window* x11win, GdkPixbuf* pixbuf) {
    if (!x11win || !x11win->display || !x11win->window || !pixbuf) return;
    
    // Get pixel data
    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    int channels = gdk_pixbuf_get_n_channels(pixbuf);
    guchar* pixels = gdk_pixbuf_get_pixels(pixbuf);
    int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    
    // Convert to ARGB format for X11
    std::vector<unsigned long> icon_data;
    icon_data.push_back(width);
    icon_data.push_back(height);
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            guchar* pixel = pixels + y * rowstride + x * channels;
            unsigned long argb = 0;
            
            if (channels == 4) {
                // RGBA
                argb = ((unsigned long)pixel[3] << 24) | // A
                       ((unsigned long)pixel[0] << 16) | // R
                       ((unsigned long)pixel[1] << 8)  | // G
                       ((unsigned long)pixel[2]);        // B
            } else if (channels == 3) {
                // RGB (no alpha)
                argb = (0xFFUL << 24) |                  // A (opaque)
                       ((unsigned long)pixel[0] << 16) | // R
                       ((unsigned long)pixel[1] << 8)  | // G
                       ((unsigned long)pixel[2]);        // B
            }
            
            icon_data.push_back(argb);
        }
    }
    
    // Set _NET_WM_ICON property
    Atom net_wm_icon = XInternAtom(x11win->display, "_NET_WM_ICON", False);
    XChangeProperty(x11win->display, x11win->window, net_wm_icon,
                  XA_CARDINAL, 32, PropModeReplace,
                  (unsigned char*)icon_data.data(), icon_data.size());
    
    XFlush(x11win->display);
}

// Get or create a WebKit context for a partition
static WebKitWebContext* getContextForPartition(const char* partitionIdentifier) {
    std::string partition = partitionIdentifier ? partitionIdentifier : "";
    bool isEphemeralPartition = !partition.empty() && !isPersistentWebKitPartition(partition);

    auto it = g_partitionContexts.find(partition);
    if (it != g_partitionContexts.end()) {
        if (isEphemeralPartition) {
            g_ephemeralPartitionContextRefCounts[partition]++;
        }
        return it->second;
    }

    WebKitWebContext* context = nullptr;

    if (partition.empty()) {
        // Default: use default context
        context = webkit_web_context_get_default();
    } else {
        if (isPersistentWebKitPartition(partition)) {
            std::string partitionName = partition.substr(8);

            // Build paths with identifier/channel structure (consistent with CLI and updater)
            char* home = getenv("HOME");
            std::string homeStr = home ? std::string(home) : "/tmp";
            std::string dataPath = buildPartitionPath(homeStr + "/.local/share", g_electrobunIdentifier, g_electrobunChannel, "WebKit", partitionName);
            std::string cachePath = buildPartitionPath(homeStr + "/.cache", g_electrobunIdentifier, g_electrobunChannel, "WebKit", partitionName);

            g_mkdir_with_parents(dataPath.c_str(), 0755);
            g_mkdir_with_parents(cachePath.c_str(), 0755);

            WebKitWebsiteDataManager* dataManager = webkit_website_data_manager_new(
                "base-data-directory", dataPath.c_str(),
                "base-cache-directory", cachePath.c_str(),
                NULL
            );

            // Enable persistent cookie storage (SQLite-backed)
            WebKitCookieManager* cookieManager = webkit_website_data_manager_get_cookie_manager(dataManager);
            if (cookieManager) {
                std::string cookiePath = dataPath + "/cookies.sqlite";
                webkit_cookie_manager_set_persistent_storage(
                    cookieManager,
                    cookiePath.c_str(),
                    WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE
                );
            }

            context = webkit_web_context_new_with_website_data_manager(dataManager);
            g_object_unref(dataManager);
        } else {
            WebKitWebsiteDataManager* dataManager = webkit_website_data_manager_new_ephemeral();
            context = webkit_web_context_new_with_website_data_manager(dataManager);
            g_object_unref(dataManager);
        }

        g_partitionContexts[partition] = context;
        if (isEphemeralPartition) {
            g_ephemeralPartitionContextRefCounts[partition] = 1;
        }
    }

    // Mark each context after registration so the default context and custom
    // partition contexts both receive the handler exactly once.
    static const char* viewsSchemeRegisteredKey =
        "electrobun-views-scheme-registered";
    if (!g_object_get_data(G_OBJECT(context), viewsSchemeRegisteredKey)) {
        WebKitSecurityManager* securityManager =
            webkit_web_context_get_security_manager(context);
        webkit_security_manager_register_uri_scheme_as_secure(securityManager, "views");
        webkit_security_manager_register_uri_scheme_as_cors_enabled(securityManager, "views");
        webkit_security_manager_register_uri_scheme_as_secure(securityManager, "appdata");
        webkit_security_manager_register_uri_scheme_as_cors_enabled(securityManager, "appdata");
        webkit_web_context_register_uri_scheme(
            context,
            "views",
            handleViewsURIScheme,
            nullptr,
            nullptr);
        webkit_web_context_register_uri_scheme(
            context,
            "appdata",
            handleAppDataURIScheme,
            nullptr,
            nullptr);
        g_object_set_data(
            G_OBJECT(context),
            viewsSchemeRegisteredKey,
            GINT_TO_POINTER(1));
    }

    return context;
}

static void releaseContextForPartition(const std::string& partition) {
    if (partition.empty() || isPersistentWebKitPartition(partition)) {
        return;
    }

    auto refIt = g_ephemeralPartitionContextRefCounts.find(partition);
    if (refIt == g_ephemeralPartitionContextRefCounts.end()) {
        return;
    }

    if (refIt->second > 1) {
        refIt->second--;
        return;
    }

    g_ephemeralPartitionContextRefCounts.erase(refIt);

    auto contextIt = g_partitionContexts.find(partition);
    if (contextIt != g_partitionContexts.end()) {
        WebKitWebContext* context = contextIt->second;
        g_partitionContexts.erase(contextIt);
        if (context) {
            g_object_unref(context);
        }
    }
}

