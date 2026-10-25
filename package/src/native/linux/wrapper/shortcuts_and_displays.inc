static GlobalShortcutCallback g_globalShortcutCallback = nullptr;

// Storage for registered shortcuts
struct ShortcutInfo {
    KeyCode keycode;
    unsigned int modifiers;
};
static std::map<std::string, ShortcutInfo> g_globalShortcuts;
static Display* g_shortcutDisplay = nullptr;
static std::thread g_shortcutThread;
static bool g_shortcutThreadRunning = false;

// On Wayland, shortcuts go through the GlobalShortcuts portal when a backend
// provides it; X11 key grabs (which under XWayland only see X11 windows) are
// the fallback. g_portalShortcuts lives on the GLib main context; the id set
// answers register/isRegistered from the caller's thread.
static bool g_shortcutsUsePortal = false;
static electrobun::xdg_portal::GlobalShortcuts* g_portalShortcuts = nullptr;
static std::mutex g_portalShortcutIdsMutex;
static std::set<std::string> g_portalShortcutIds;

// Helper to get X11 keysym from key string
static KeySym getKeySym(const std::string& key) {
    std::string lowerKey = key;
    std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);

    // Letters
    if (lowerKey.length() == 1 && lowerKey[0] >= 'a' && lowerKey[0] <= 'z') {
        return XK_a + (lowerKey[0] - 'a');
    }
    // Numbers
    if (lowerKey.length() == 1 && lowerKey[0] >= '0' && lowerKey[0] <= '9') {
        return XK_0 + (lowerKey[0] - '0');
    }
    // Function keys (F1-F24)
    if (lowerKey[0] == 'f' && lowerKey.length() >= 2) {
        int fNum = std::stoi(lowerKey.substr(1));
        if (fNum >= 1 && fNum <= 24) {
            if (fNum <= 12) return XK_F1 + (fNum - 1);
            else return XK_F13 + (fNum - 13);  // F13-F24
        }
    }
    // Special keys
    if (lowerKey == "space" || lowerKey == " ") return XK_space;
    if (lowerKey == "return" || lowerKey == "enter") return XK_Return;
    if (lowerKey == "tab") return XK_Tab;
    if (lowerKey == "escape" || lowerKey == "esc") return XK_Escape;
    if (lowerKey == "backspace") return XK_BackSpace;
    if (lowerKey == "delete") return XK_Delete;
    if (lowerKey == "insert") return XK_Insert;
    if (lowerKey == "up") return XK_Up;
    if (lowerKey == "down") return XK_Down;
    if (lowerKey == "left") return XK_Left;
    if (lowerKey == "right") return XK_Right;
    if (lowerKey == "home") return XK_Home;
    if (lowerKey == "end") return XK_End;
    if (lowerKey == "pageup") return XK_Page_Up;
    if (lowerKey == "pagedown") return XK_Page_Down;
    if (lowerKey == "print") return XK_Print;
    // Additional special keys
    if (lowerKey == "scrolllock") return XK_Scroll_Lock;
    if (lowerKey == "pause") return XK_Pause;
    if (lowerKey == "break") return XK_Break;
    if (lowerKey == "sysreq") return XK_Sys_Req;
    if (lowerKey == "numlock") return XK_Num_Lock;
    if (lowerKey == "capslock") return XK_Caps_Lock;
    if (lowerKey == "menu") return XK_Menu;
    if (lowerKey == "apps") return XK_Menu;  // Same as Menu
    if (lowerKey == "printscreen") return XK_Print;
    if (lowerKey == "cancel") return XK_Cancel;
    // Media keys (may not be available on all systems)
    if (lowerKey == "mediaselect") return XK_Select;  // Closest equivalent
    if (lowerKey == "calculator") return 0x1008ff1d;  // XF86Calculator
    if (lowerKey == "sleep") return 0x1008ff2f;  // XF86Sleep
    // Symbols
    if (lowerKey == "-") return XK_minus;
    if (lowerKey == "=") return XK_equal;
    if (lowerKey == "[") return XK_bracketleft;
    if (lowerKey == "]") return XK_bracketright;
    if (lowerKey == "\\") return XK_backslash;
    if (lowerKey == ";") return XK_semicolon;
    if (lowerKey == "'") return XK_apostrophe;
    if (lowerKey == ",") return XK_comma;
    if (lowerKey == ".") return XK_period;
    if (lowerKey == "/") return XK_slash;
    if (lowerKey == "`") return XK_grave;

    return NoSymbol;
}

// Parse modifiers from accelerator string for X11 shortcuts using the
// shared cross-platform parser. Returns X11 modifier mask.
static unsigned int parseX11Modifiers(const std::string& accelerator, std::string& outKey) {
    auto parts = electrobun::parseAccelerator(accelerator);
    outKey = parts.key;

    unsigned int modifiers = 0;
    if (parts.commandOrControl || parts.command || parts.control) modifiers |= ControlMask;
    if (parts.alt)                                                modifiers |= Mod1Mask;
    if (parts.shift)                                              modifiers |= ShiftMask;
    if (parts.super)                                              modifiers |= Mod4Mask;
    return modifiers;
}

// X11 event loop for global shortcuts
static void shortcutEventLoop() {
    ensureXlibThreadSupport();
    g_shortcutDisplay = XOpenDisplay(nullptr);
    if (!g_shortcutDisplay) {
        fprintf(stderr, "ERROR: Failed to open X11 display for shortcuts\n");
        g_shortcutThreadRunning = false;
        return;
    }
    
    printf("GlobalShortcut: X11 display opened successfully for shortcuts\n");

    Window root = DefaultRootWindow(g_shortcutDisplay);

    while (g_shortcutThreadRunning) {
        while (XPending(g_shortcutDisplay)) {
            XEvent event;
            XNextEvent(g_shortcutDisplay, &event);

            if (event.type == KeyPress) {
                KeyCode keycode = event.xkey.keycode;
                unsigned int state = event.xkey.state & (ControlMask | ShiftMask | Mod1Mask | Mod4Mask);

                // Find matching shortcut
                for (const auto& pair : g_globalShortcuts) {
                    if (pair.second.keycode == keycode && pair.second.modifiers == state) {
                        if (g_globalShortcutCallback) {
                            g_globalShortcutCallback(pair.first.c_str());
                        }
                        break;
                    }
                }
            }
        }
        usleep(10000); // 10ms sleep
    }

    XCloseDisplay(g_shortcutDisplay);
    g_shortcutDisplay = nullptr;
}

// Set the callback for global shortcut events
ELECTROBUN_EXPORT void setGlobalShortcutCallback(GlobalShortcutCallback callback) {
    printf("GlobalShortcut: Setting callback (callback=%p)\n", callback);
    g_globalShortcutCallback = callback;

    if (g_shortcutsUsePortal) return;
    if (callback && !g_shortcutThreadRunning && g_getenv("WAYLAND_DISPLAY") &&
        electrobun::xdg_portal::GlobalShortcuts::available()) {
        g_shortcutsUsePortal = true;
        printf("GlobalShortcut: using the xdg-desktop-portal GlobalShortcuts interface\n");
        dispatch_async_main_void([]() {
            g_portalShortcuts = new electrobun::xdg_portal::GlobalShortcuts();
            g_portalShortcuts->start([](const std::string& id) {
                if (g_globalShortcutCallback) g_globalShortcutCallback(id.c_str());
            });
        });
        return;
    }

    // Start the event loop thread if not running
    if (!g_shortcutThreadRunning && callback) {
        printf("GlobalShortcut: Starting event loop thread\n");
        g_shortcutThreadRunning = true;
        g_shortcutThread = std::thread(shortcutEventLoop);
        // Wait for display to be opened
        int attempts = 0;
        while (!g_shortcutDisplay && g_shortcutThreadRunning && attempts < 100) {
            usleep(10000);
            attempts++;
        }
        if (g_shortcutDisplay) {
            printf("GlobalShortcut: Event loop ready\n");
        } else {
            fprintf(stderr, "ERROR: GlobalShortcut event loop failed to initialize\n");
        }
    }
}

// The portal binds shortcuts asynchronously (and may ask the user), so this
// accepts any accelerator with a known key, like the X11 path does.
static bool registerPortalShortcut(const std::string& accelerator) {
    auto parts = electrobun::parseAccelerator(accelerator);
    KeySym keysym = getKeySym(parts.key);
    std::string trigger = electrobun::xdg_portal::shortcutTrigger(
        parts, keysym == NoSymbol ? nullptr : XKeysymToString(keysym));
    if (trigger.empty()) {
        fprintf(stderr, "ERROR: Unknown key: %s\n", parts.key.c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_portalShortcutIdsMutex);
        if (!g_portalShortcutIds.insert(accelerator).second) {
            fprintf(stderr, "GlobalShortcut already registered: %s\n", accelerator.c_str());
            return false;
        }
    }
    dispatch_async_main_void([accelerator, trigger]() {
        if (g_portalShortcuts) g_portalShortcuts->set(accelerator, accelerator, trigger);
    });
    printf("GlobalShortcut registered with the portal: %s (preferred trigger %s)\n",
           accelerator.c_str(), trigger.c_str());
    return true;
}

// Register a global keyboard shortcut
ELECTROBUN_EXPORT bool registerGlobalShortcut(const char* accelerator) {
    printf("GlobalShortcut: registerGlobalShortcut called for '%s'\n", accelerator ? accelerator : "(null)");
    
    if (!accelerator) {
        fprintf(stderr, "ERROR: Cannot register shortcut - accelerator is null\n");
        return false;
    }

    if (g_shortcutsUsePortal) return registerPortalShortcut(accelerator);
    
    if (!g_shortcutDisplay) {
        fprintf(stderr, "ERROR: Cannot register shortcut '%s' - display not ready (g_shortcutDisplay=%p)\n", 
                accelerator, g_shortcutDisplay);
        return false;
    }

    std::string accelStr(accelerator);

    // Check if already registered
    if (g_globalShortcuts.find(accelStr) != g_globalShortcuts.end()) {
        fprintf(stderr, "GlobalShortcut already registered: %s\n", accelerator);
        return false;
    }

    // Parse the accelerator
    std::string key;
    unsigned int modifiers = parseX11Modifiers(accelStr, key);
    KeySym keysym = getKeySym(key);

    if (keysym == NoSymbol) {
        fprintf(stderr, "ERROR: Unknown key: %s\n", key.c_str());
        return false;
    }

    KeyCode keycode = XKeysymToKeycode(g_shortcutDisplay, keysym);
    if (keycode == 0) {
        fprintf(stderr, "ERROR: Failed to get keycode for key: %s\n", key.c_str());
        return false;
    }

    Window root = DefaultRootWindow(g_shortcutDisplay);

    // Grab key with various modifier combinations (to handle NumLock, CapsLock, etc.)
    unsigned int modifierVariants[] = {
        modifiers,
        modifiers | Mod2Mask,  // NumLock
        modifiers | LockMask,  // CapsLock
        modifiers | Mod2Mask | LockMask
    };

    // Just try to grab the key - if it fails, XGrabKey will generate an X11 error
    // but won't crash the program. We'll optimistically assume success.
    for (unsigned int mods : modifierVariants) {
        XGrabKey(g_shortcutDisplay, keycode, mods, root, True, GrabModeAsync, GrabModeAsync);
    }
    XFlush(g_shortcutDisplay);

    // Since we can't easily detect if XGrabKey failed without complex error handling,
    // we'll assume success and let the user know if the shortcut doesn't work

    ShortcutInfo info;
    info.keycode = keycode;
    info.modifiers = modifiers;
    g_globalShortcuts[accelStr] = info;

    printf("GlobalShortcut registered: %s (keycode: %d, modifiers: 0x%X)\n",
           accelerator, keycode, modifiers);
    return true;
}

// Unregister a global keyboard shortcut
ELECTROBUN_EXPORT bool unregisterGlobalShortcut(const char* accelerator) {
    if (accelerator && g_shortcutsUsePortal) {
        std::string accelStr(accelerator);
        {
            std::lock_guard<std::mutex> lock(g_portalShortcutIdsMutex);
            if (!g_portalShortcutIds.erase(accelStr)) return false;
        }
        dispatch_async_main_void([accelStr]() {
            if (g_portalShortcuts) g_portalShortcuts->remove(accelStr);
        });
        return true;
    }
    if (!accelerator || !g_shortcutDisplay) return false;

    std::string accelStr(accelerator);
    auto it = g_globalShortcuts.find(accelStr);
    if (it != g_globalShortcuts.end()) {
        Window root = DefaultRootWindow(g_shortcutDisplay);
        KeyCode keycode = it->second.keycode;
        unsigned int modifiers = it->second.modifiers;

        // Ungrab with same modifier variants
        unsigned int modifierVariants[] = {
            modifiers,
            modifiers | Mod2Mask,
            modifiers | LockMask,
            modifiers | Mod2Mask | LockMask
        };

        for (unsigned int mods : modifierVariants) {
            XUngrabKey(g_shortcutDisplay, keycode, mods, root);
        }
        XFlush(g_shortcutDisplay);

        g_globalShortcuts.erase(it);
        printf("GlobalShortcut unregistered: %s\n", accelerator);
        return true;
    }

    return false;
}

// Unregister all global keyboard shortcuts
ELECTROBUN_EXPORT void unregisterAllGlobalShortcuts() {
    if (g_shortcutsUsePortal) {
        {
            std::lock_guard<std::mutex> lock(g_portalShortcutIdsMutex);
            g_portalShortcutIds.clear();
        }
        dispatch_async_main_void([]() {
            if (g_portalShortcuts) g_portalShortcuts->clear();
        });
        return;
    }
    if (!g_shortcutDisplay) return;

    Window root = DefaultRootWindow(g_shortcutDisplay);

    for (const auto& pair : g_globalShortcuts) {
        KeyCode keycode = pair.second.keycode;
        unsigned int modifiers = pair.second.modifiers;

        unsigned int modifierVariants[] = {
            modifiers,
            modifiers | Mod2Mask,
            modifiers | LockMask,
            modifiers | Mod2Mask | LockMask
        };

        for (unsigned int mods : modifierVariants) {
            XUngrabKey(g_shortcutDisplay, keycode, mods, root);
        }
    }
    XFlush(g_shortcutDisplay);

    g_globalShortcuts.clear();
    printf("GlobalShortcut: Unregistered all shortcuts\n");
}

// Check if a shortcut is registered
ELECTROBUN_EXPORT bool isGlobalShortcutRegistered(const char* accelerator) {
    if (!accelerator) return false;
    if (g_shortcutsUsePortal) {
        std::lock_guard<std::mutex> lock(g_portalShortcutIdsMutex);
        return g_portalShortcutIds.count(accelerator) > 0;
    }
    return g_globalShortcuts.find(std::string(accelerator)) != g_globalShortcuts.end();
}

/*
 * =============================================================================
 * SCREEN API
 * =============================================================================
 */

// Get all displays as JSON array
ELECTROBUN_EXPORT const char* getAllDisplays() {
    waitForGTKInit();
    return dispatch_sync_main([]() -> const char* {
        GdkDisplay* display = gdk_display_get_default();
        if (!display) {
            return strdup("[]");
        }

        int numMonitors = gdk_display_get_n_monitors(display);
        GdkMonitor* primaryMonitor = primaryOrFirstMonitor(display);

        std::ostringstream result;
        result << "[";

        for (int i = 0; i < numMonitors; i++) {
            GdkMonitor* monitor = gdk_display_get_monitor(display, i);
            if (!monitor) continue;

            if (i > 0) result << ",";

            // Get geometry (full bounds)
            GdkRectangle geometry;
            gdk_monitor_get_geometry(monitor, &geometry);

            // Get work area (excludes panels/taskbars)
            GdkRectangle workarea;
            gdk_monitor_get_workarea(monitor, &workarea);

            // Get scale factor
            int scaleFactor = gdk_monitor_get_scale_factor(monitor);

            // Check if primary
            bool isPrimary = (monitor == primaryMonitor);

            // Use monitor index as ID (GdkMonitor doesn't have a persistent ID)
            result << "{";
            result << "\"id\":" << i << ",";
            result << "\"bounds\":{";
            result << "\"x\":" << geometry.x << ",";
            result << "\"y\":" << geometry.y << ",";
            result << "\"width\":" << geometry.width << ",";
            result << "\"height\":" << geometry.height;
            result << "},";
            result << "\"workArea\":{";
            result << "\"x\":" << workarea.x << ",";
            result << "\"y\":" << workarea.y << ",";
            result << "\"width\":" << workarea.width << ",";
            result << "\"height\":" << workarea.height;
            result << "},";
            result << "\"scaleFactor\":" << scaleFactor << ",";
            result << "\"isPrimary\":" << (isPrimary ? "true" : "false");
            result << "}";
        }

        result << "]";
        return strdup(result.str().c_str());
    });
}

// Get primary display as JSON
ELECTROBUN_EXPORT const char* getPrimaryDisplay() {
    waitForGTKInit();
    return dispatch_sync_main([]() -> const char* {
        GdkDisplay* display = gdk_display_get_default();
        if (!display) {
            return strdup("{}");
        }

        GdkMonitor* monitor = primaryOrFirstMonitor(display);
        if (!monitor) {
            return strdup("{}");
        }

        // Get geometry (full bounds)
        GdkRectangle geometry;
        gdk_monitor_get_geometry(monitor, &geometry);

        // Get work area (excludes panels/taskbars)
        GdkRectangle workarea;
        gdk_monitor_get_workarea(monitor, &workarea);

        // Get scale factor
        int scaleFactor = gdk_monitor_get_scale_factor(monitor);

        std::ostringstream result;
        result << "{";
        result << "\"id\":0,";
        result << "\"bounds\":{";
        result << "\"x\":" << geometry.x << ",";
        result << "\"y\":" << geometry.y << ",";
        result << "\"width\":" << geometry.width << ",";
        result << "\"height\":" << geometry.height;
        result << "},";
        result << "\"workArea\":{";
        result << "\"x\":" << workarea.x << ",";
        result << "\"y\":" << workarea.y << ",";
        result << "\"width\":" << workarea.width << ",";
        result << "\"height\":" << workarea.height;
        result << "},";
        result << "\"scaleFactor\":" << scaleFactor << ",";
        result << "\"isPrimary\":true";
        result << "}";

        return strdup(result.str().c_str());
    });
}

// Get current cursor position as JSON: {"x": 123, "y": 456}
ELECTROBUN_EXPORT const char* getCursorScreenPoint() {
    static thread_local std::string resultStorage;

    resultStorage = dispatch_sync_main([&]() -> std::string {
        if (wayland_screen_capture::isWaylandSession()) {
            double portalX = 0;
            double portalY = 0;
            if (wayland_screen_capture::getCursorScreenPoint(
                    &portalX, &portalY)) {
                std::ostringstream result;
                result << "{\"x\":" << portalX << ",\"y\":" << portalY << "}";
                return result.str();
            }
        }

        GdkDisplay* display = gdk_display_get_default();
        if (!display) {
            return "{\"x\":0,\"y\":0}";
        }

        GdkSeat* seat = gdk_display_get_default_seat(display);
        if (!seat) {
            return "{\"x\":0,\"y\":0}";
        }

        GdkDevice* pointer = gdk_seat_get_pointer(seat);
        if (!pointer) {
            return "{\"x\":0,\"y\":0}";
        }

        int x = 0;
        int y = 0;
        gdk_device_get_position(pointer, NULL, &x, &y);

        std::ostringstream result;
        result << "{\"x\":" << x << ",\"y\":" << y << "}";
        return result.str();
    });

    return resultStorage.c_str();
}

ELECTROBUN_EXPORT bool captureScreenRegion(
    double x,
    double y,
    uint32_t width,
    uint32_t height,
    uint8_t* out_rgba,
    uint64_t out_len
) {
    if (!out_rgba || width == 0 || height == 0 ||
        !std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }

    const uint64_t logicalWidth = static_cast<uint64_t>(width);
    const uint64_t logicalHeight = static_cast<uint64_t>(height);
    if (logicalWidth > std::numeric_limits<uint64_t>::max() / logicalHeight) {
        return false;
    }
    const uint64_t logicalPixels = logicalWidth * logicalHeight;
    if (logicalPixels > std::numeric_limits<uint64_t>::max() / 4) {
        return false;
    }
    const uint64_t requiredOutputBytes = logicalPixels * 4;
    if (out_len != requiredOutputBytes ||
        requiredOutputBytes > std::numeric_limits<size_t>::max() ||
        width > static_cast<uint32_t>(G_MAXINT) ||
        height > static_cast<uint32_t>(G_MAXINT)) {
        return false;
    }

    // Keep the conversion well-defined even for hostile FFI inputs. Screen
    // coordinates ultimately have gint-sized GDK bounds; use int64_t while
    // validating before narrowing them for the GDK call.
    const long double roundedX = std::round(static_cast<long double>(x));
    const long double roundedY = std::round(static_cast<long double>(y));
    if (roundedX < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        roundedX > static_cast<long double>(std::numeric_limits<int64_t>::max()) ||
        roundedY < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        roundedY > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        return false;
    }
    const int64_t requestedLeft = static_cast<int64_t>(roundedX);
    const int64_t requestedTop = static_cast<int64_t>(roundedY);

    return dispatch_sync_main([=]() -> bool {
        if (wayland_screen_capture::isWaylandSession()) {
            return wayland_screen_capture::captureRegion(
                static_cast<double>(requestedLeft),
                static_cast<double>(requestedTop),
                width,
                height,
                out_rgba,
                out_len);
        }

        GdkDisplay* display = gdk_display_get_default();
        if (!display || !GDK_IS_X11_DISPLAY(display)) {
            // A native Wayland display has no root window to read, and the
            // portal capture above didn't apply: don't pretend capture
            // succeeded.
            return false;
        }

        GdkWindow* root = gdk_get_default_root_window();
        if (!root || gdk_window_is_destroyed(root)) {
            return false;
        }

        const int rootWidth = gdk_window_get_width(root);
        const int rootHeight = gdk_window_get_height(root);
        if (rootWidth <= 0 || rootHeight <= 0) {
            return false;
        }

        const int64_t requestedWidth = static_cast<int64_t>(width);
        const int64_t requestedHeight = static_cast<int64_t>(height);
        // Screen.getCursorScreenPoint(), display bounds, and GDK's root
        // drawable share this coordinate space. Negative/clipped XRandR CRTC
        // coordinates are outside the drawable and therefore fail cleanly.
        if (requestedLeft < 0 || requestedTop < 0 ||
            requestedLeft > rootWidth || requestedTop > rootHeight ||
            requestedWidth > static_cast<int64_t>(rootWidth) - requestedLeft ||
            requestedHeight > static_cast<int64_t>(rootHeight) - requestedTop) {
            return false;
        }

        GdkPixbuf* pixbuf = gdk_pixbuf_get_from_window(
            root,
            static_cast<int>(requestedLeft),
            static_cast<int>(requestedTop),
            static_cast<int>(width),
            static_cast<int>(height));
        if (!pixbuf) {
            return false;
        }

        const int pixbufWidth = gdk_pixbuf_get_width(pixbuf);
        const int pixbufHeight = gdk_pixbuf_get_height(pixbuf);
        const int channels = gdk_pixbuf_get_n_channels(pixbuf);
        const int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
        const bool validFormat =
            gdk_pixbuf_get_colorspace(pixbuf) == GDK_COLORSPACE_RGB &&
            gdk_pixbuf_get_bits_per_sample(pixbuf) == 8 &&
            channels >= 3 &&
            pixbufWidth > 0 && pixbufHeight > 0 &&
            pixbufWidth % static_cast<int>(width) == 0 &&
            pixbufHeight % static_cast<int>(height) == 0;
        if (!validFormat) {
            g_object_unref(pixbuf);
            return false;
        }

        const int scaleX = pixbufWidth / static_cast<int>(width);
        const int scaleY = pixbufHeight / static_cast<int>(height);
        if (scaleX <= 0 || scaleX != scaleY) {
            g_object_unref(pixbuf);
            return false;
        }

        const uint64_t packedRowBytes =
            static_cast<uint64_t>(pixbufWidth) * static_cast<uint64_t>(channels);
        if (rowstride <= 0 || static_cast<uint64_t>(rowstride) < packedRowBytes) {
            g_object_unref(pixbuf);
            return false;
        }

        guint sourceLength = 0;
        const guchar* source =
            gdk_pixbuf_get_pixels_with_length(pixbuf, &sourceLength);
        const uint64_t requiredSourceBytes =
            static_cast<uint64_t>(pixbufHeight - 1) *
                static_cast<uint64_t>(rowstride) +
            packedRowBytes;
        if (!source || requiredSourceBytes > static_cast<uint64_t>(sourceLength)) {
            g_object_unref(pixbuf);
            return false;
        }

        // The pixbuf is device-resolution (logical dimensions multiplied by
        // the root window's integer scale). Sample the center device pixel for
        // each requested logical pixel and always expose opaque RGBA.
        const int sampleOffset = scaleX / 2;
        for (uint32_t destinationY = 0; destinationY < height; ++destinationY) {
            const int sourceY =
                static_cast<int>(destinationY) * scaleY + sampleOffset;
            const uint64_t sourceRow =
                static_cast<uint64_t>(sourceY) * static_cast<uint64_t>(rowstride);
            const uint64_t destinationRow =
                static_cast<uint64_t>(destinationY) * logicalWidth * 4;

            for (uint32_t destinationX = 0; destinationX < width; ++destinationX) {
                const int sourceX =
                    static_cast<int>(destinationX) * scaleX + sampleOffset;
                const uint64_t sourceOffset =
                    sourceRow + static_cast<uint64_t>(sourceX) *
                        static_cast<uint64_t>(channels);
                const uint64_t destinationOffset =
                    destinationRow + static_cast<uint64_t>(destinationX) * 4;

                out_rgba[destinationOffset] = source[sourceOffset];
                out_rgba[destinationOffset + 1] = source[sourceOffset + 1];
                out_rgba[destinationOffset + 2] = source[sourceOffset + 2];
                out_rgba[destinationOffset + 3] = 255;
            }
        }

        g_object_unref(pixbuf);
        return true;
    });
}

ELECTROBUN_EXPORT uint64_t getMouseButtons() {
    return dispatch_sync_main([&]() -> uint64_t {
        GdkDisplay* display = gdk_display_get_default();
        if (!display) {
            return 0;
        }

        GdkSeat* seat = gdk_display_get_default_seat(display);
        if (!seat) {
            return 0;
        }

        GdkDevice* pointer = gdk_seat_get_pointer(seat);
        if (!pointer) {
            return 0;
        }

        GdkWindow* root = gdk_get_default_root_window();
        if (!root) {
            return 0;
        }

        GdkModifierType modifiers = (GdkModifierType)0;
        gdk_device_get_state(pointer, root, NULL, &modifiers);
        return mouseButtonsFromGdkModifiers(modifiers);
    });
}

/*
 * =============================================================================
 * COOKIE MANAGEMENT API
 * =============================================================================
 */

