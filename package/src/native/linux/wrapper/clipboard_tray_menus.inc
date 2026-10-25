// ============================================================================
// Clipboard API
// ============================================================================

// clipboardReadText - Read text from the system clipboard
// Returns: UTF-8 string (caller must free) or NULL if no text available
ELECTROBUN_EXPORT const char* clipboardReadText() {
    return dispatch_sync_main([&]() -> const char* {
        GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gchar* text = gtk_clipboard_wait_for_text(clipboard);
        if (text) {
            const char* result = strdup(text);
            g_free(text);
            return result;
        }
        return nullptr;
    });
}

// clipboardWriteText - Write text to the system clipboard
ELECTROBUN_EXPORT void clipboardWriteText(const char* text) {
    if (!text) return;

    // Make a copy of the text since we need it to persist
    std::string textCopy(text);

    dispatch_sync_main_void([&]() {
        GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(clipboard, textCopy.c_str(), -1);
        // Store the clipboard data so it persists after the app exits
        gtk_clipboard_store(clipboard);
    });
}

// clipboardReadImage - Read image from clipboard as PNG data
// Returns: PNG data (caller must free) and sets outSize, or NULL if no image
const uint8_t* clipboardReadImage(size_t* outSize) {
    return dispatch_sync_main([&]() -> const uint8_t* {
        if (outSize) *outSize = 0;

        GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        GdkPixbuf* pixbuf = gtk_clipboard_wait_for_image(clipboard);

        if (!pixbuf) {
            return nullptr;
        }

        // Save pixbuf to PNG in memory
        gchar* buffer = nullptr;
        gsize bufferSize = 0;
        GError* error = nullptr;

        gboolean success = gdk_pixbuf_save_to_buffer(
            pixbuf, &buffer, &bufferSize, "png", &error, NULL
        );

        g_object_unref(pixbuf);

        if (!success || !buffer) {
            if (error) g_error_free(error);
            return nullptr;
        }

        // Copy to malloc'd buffer (caller will free)
        uint8_t* result = static_cast<uint8_t*>(malloc(bufferSize));
        memcpy(result, buffer, bufferSize);
        g_free(buffer);

        if (outSize) *outSize = bufferSize;
        return result;
    });
}

// clipboardWriteImage - Write PNG image data to clipboard
ELECTROBUN_EXPORT void clipboardWriteImage(const uint8_t* pngData, size_t size) {
    if (!pngData || size == 0) return;

    // Copy the data since we need it to persist
    std::vector<uint8_t> dataCopy(pngData, pngData + size);

    dispatch_sync_main_void([&]() {
        // Load PNG data into a GdkPixbuf
        GInputStream* stream = g_memory_input_stream_new_from_data(
            dataCopy.data(), dataCopy.size(), nullptr
        );

        GError* error = nullptr;
        GdkPixbuf* pixbuf = gdk_pixbuf_new_from_stream(stream, nullptr, &error);
        g_object_unref(stream);

        if (!pixbuf) {
            if (error) {
                std::cerr << "Failed to load PNG: " << error->message << std::endl;
                g_error_free(error);
            }
            return;
        }

        GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_image(clipboard, pixbuf);
        gtk_clipboard_store(clipboard);

        g_object_unref(pixbuf);
    });
}

// clipboardClear - Clear the clipboard
ELECTROBUN_EXPORT void clipboardClear() {
    dispatch_sync_main_void([&]() {
        GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_clear(clipboard);
    });
}

// clipboardAvailableFormats - Get available formats in clipboard
// Returns: comma-separated list of formats (caller must free)
ELECTROBUN_EXPORT const char* clipboardAvailableFormats() {
    return dispatch_sync_main([&]() -> const char* {
        GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        std::vector<std::string> formats;

        // Check for text
        if (gtk_clipboard_wait_is_text_available(clipboard)) {
            formats.push_back("text");
        }

        // Check for image
        if (gtk_clipboard_wait_is_image_available(clipboard)) {
            formats.push_back("image");
        }

        // Check for URIs (files)
        if (gtk_clipboard_wait_is_uris_available(clipboard)) {
            formats.push_back("files");
        }

        // Join formats with comma
        std::string result;
        for (size_t i = 0; i < formats.size(); i++) {
            if (i > 0) result += ",";
            result += formats[i];
        }

        return strdup(result.c_str());
    });
}

// NOTE: Removed deferred tray creation code - now creating TrayItem synchronously
// The TrayItem constructor handles deferred AppIndicator creation internally

ELECTROBUN_EXPORT void* createTray(uint32_t trayId, const char* title, const char* pathToImage, bool isTemplate, uint32_t width, uint32_t height, void* clickHandler) {
    // NOTE: width and height are ignored on Linux: SNI hosts and AppIndicator
    // size the icon to the panel. They're here for FFI consistency with macOS
    // and Windows.

    // Wait for GTK initialization to complete
    waitForGTKInit();

    return dispatch_sync_main([&]() -> void* {
        // Create the TrayItem on main thread
        try {
            auto tray = std::make_unique<TrayItem>(
                trayId,
                title ? title : "",
                pathToImage ? pathToImage : "",
                isTemplate,
                reinterpret_cast<ZigStatusItemHandler>(clickHandler)
            );
            if (!tray->valid()) {
                fprintf(stderr, "Tray: no tray icon: no session bus and no AppIndicator\n");
                return nullptr;
            }

            TrayItem* trayPtr = tray.get();
            g_trays[trayId] = std::move(tray);
            
            return trayPtr;
        } catch (const std::exception& e) {
            return nullptr;
        } catch (...) {
            return nullptr;
        }
    });
}

ELECTROBUN_EXPORT void setTrayTitle(void* statusItem, const char* title) {
    dispatch_sync_main_void([&]() {
        // Find the tray by statusItem pointer
        for (auto& [id, tray] : g_trays) {
            if (tray.get() == statusItem) {
                tray->setTitle(title);
                break;
            }
        }
    });
}

ELECTROBUN_EXPORT void setTrayImage(void* statusItem, const char* image) {
    dispatch_sync_main_void([&]() {
        // Find the tray by statusItem pointer
        for (auto& [id, tray] : g_trays) {
            if (tray.get() == statusItem) {
                tray->setImage(image);
                break;
            }
        }
    });
}

ELECTROBUN_EXPORT void setTrayMenuFromJSON(void* statusItem, const char* jsonString) {
    dispatch_sync_main_void([&]() {
        // Find the tray by statusItem pointer
        for (auto& [id, tray] : g_trays) {
            if (tray.get() == statusItem) {
                tray->setMenu(jsonString);
                break;
            }
        }
    });
}

ELECTROBUN_EXPORT void setTrayMenu(void* statusItem, const char* menuConfig) {
    setTrayMenuFromJSON(statusItem, menuConfig);
}

ELECTROBUN_EXPORT void removeTray(void* statusItem) {
    dispatch_sync_main_void([&]() {
        // Find the tray by statusItem pointer and remove it
        for (auto it = g_trays.begin(); it != g_trays.end(); ++it) {
            if (it->second.get() == statusItem) {
                g_trays.erase(it);
                break;
            }
        }
    });
}

ELECTROBUN_EXPORT const char* getTrayBounds(void* statusItem) {
    (void)statusItem;
    return strdup("{\"x\":0,\"y\":0,\"width\":0,\"height\":0}");
}

// Whether an SNI host can show a tray icon: a StatusNotifierWatcher is on the
// session bus and reports a registered host. Apps check it before hiding a
// window to the tray, so it doesn't vanish with no way back.
ELECTROBUN_EXPORT bool isTrayHostAvailable() {
    GDBusConnection* bus = traySessionBus();
    return bus && sni::hostAvailable(bus);
}

typedef void (*TrayHostHandler)(bool available);
static TrayHostHandler g_trayHostHandler = nullptr;
static guint g_trayHostWatch = 0;

// Calls handler(available) when isTrayHostAvailable() changes (a bar starts,
// quits or reloads). Starts watching on the GTK main loop; null stops it.
ELECTROBUN_EXPORT void setTrayHostHandler(TrayHostHandler handler) {
    g_trayHostHandler = handler;
    g_idle_add([](gpointer) -> gboolean {
        GDBusConnection* bus = traySessionBus();
        if (g_trayHostHandler && !g_trayHostWatch && bus) {
            g_trayHostWatch = sni::watchHost(bus, [](bool available) {
                if (g_trayHostHandler) g_trayHostHandler(available);
            });
        } else if (!g_trayHostHandler && g_trayHostWatch) {
            sni::unwatchHost(g_trayHostWatch);
            g_trayHostWatch = 0;
        }
        return G_SOURCE_REMOVE;
    }, nullptr);
}

ELECTROBUN_EXPORT void setApplicationMenu(const char* jsonString, void* applicationMenuHandler) {
    if (!jsonString || strlen(jsonString) == 0) {
        return;
    }
    
    // Wait for GTK initialization to complete
    waitForGTKInit();
    
    dispatch_sync_main_void([&]() {
        try {
            // Store the menu config globally so it can be applied to future windows
            g_applicationMenuConfig = std::string(jsonString);
            g_applicationMenuHandler = reinterpret_cast<ZigStatusItemHandler>(applicationMenuHandler);
            
            std::vector<MenuJsonValue> menuItems = parseMenuJson(g_applicationMenuConfig);
            
            // Apply menu to all existing GTK windows  
            for (auto& containerPair : g_containers) {
                auto container = containerPair.second;
                if (container && container->window) {
                    applyApplicationMenuToWindow(container->window);
                }
            }
            
            // Apply menu to all existing X11 windows
            for (auto& x11Pair : g_x11_windows) {
                auto x11win = x11Pair.second;
                if (x11win) {
                    applyApplicationMenuToX11Window(x11win.get());
                }
            }
        } catch (const std::exception& e) {
            // Handle exception silently
        }
    });
}

// NOTE: Context menu behavior on Linux is limited compared to macOS.
// On macOS, you can programmatically show a custom menu at the current mouse position.
// On Linux/GTK, context menus are typically triggered by right-click events rather than
// programmatic calls. This function is not supported on Linux.
ELECTROBUN_EXPORT void showContextMenu(const char* jsonString, void* contextMenuHandler) {
    printf("showContextMenu is not supported on Linux. Use application menus or system tray menus instead.\n");
    fflush(stdout);
}

struct WebviewSnapshotRequest {
    uint32_t requestId;
    SnapshotCallback callback;
};

static cairo_status_t appendSnapshotPngBytes(void* closure, const unsigned char* data, unsigned int length) {
    auto* png = static_cast<std::vector<unsigned char>*>(closure);
    png->insert(png->end(), data, data + length);
    return CAIRO_STATUS_SUCCESS;
}

static void onWebviewSnapshotReady(GObject* source, GAsyncResult* result, gpointer data) {
    std::unique_ptr<WebviewSnapshotRequest> request(static_cast<WebviewSnapshotRequest*>(data));
    GError* error = nullptr;
    cairo_surface_t* surface = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), result, &error);
    std::vector<unsigned char> png;
    if (surface) {
        if (cairo_surface_write_to_png_stream(surface, appendSnapshotPngBytes, &png) != CAIRO_STATUS_SUCCESS) {
            png.clear();
        }
        cairo_surface_destroy(surface);
    }
    if (png.empty()) {
        request->callback(request->requestId, nullptr, strdup(error ? error->message : "WebKitGTK returned no image"));
        if (error) g_error_free(error);
        return;
    }
    gchar* base64 = g_base64_encode(png.data(), png.size());
    std::string dataUrl = std::string("data:image/png;base64,") + base64;
    g_free(base64);
    request->callback(request->requestId, strdup(dataUrl.c_str()), nullptr);
}

ELECTROBUN_EXPORT bool getWebviewSnapshot(AbstractView* abstractView, uint32_t requestId, SnapshotCallback callback) {
    if (!abstractView || !callback) {
        return false;
    }
    std::shared_ptr<WebKitWebViewImpl> view;
    {
        std::lock_guard<std::mutex> lock(g_webviewMapMutex);
        for (const auto& [id, candidate] : g_webviewMap) {
            (void)id;
            if (candidate.get() == abstractView) {
                view = std::dynamic_pointer_cast<WebKitWebViewImpl>(candidate);
                break;
            }
        }
    }
    if (!view) {
        return false;
    }
    dispatch_async_main_void([view, requestId, callback]() {
        if (view->isRemoved || !view->webview) {
            callback(requestId, nullptr, strdup("the webview was removed"));
            return;
        }
        webkit_web_view_get_snapshot(WEBKIT_WEB_VIEW(view->webview), WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                     WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr, onWebviewSnapshotReady,
                                     new WebviewSnapshotRequest{requestId, callback});
    });
    return true;
}

void setJSUtils(void* getMimeType, void* getHTMLForWebviewSync) {
    printf("setJSUtils called but using map-based approach instead of callbacks\n");
    fflush(stdout);
}

// MARK: - Webview HTML Content Management (replaces JSCallback approach)

extern "C" void setWebviewHTMLContent(uint32_t webviewId, const char* htmlContent) {
    std::lock_guard<std::mutex> lock(webviewHTMLMutex);
    if (htmlContent) {
        webviewHTMLContent[webviewId] = std::string(htmlContent);
        printf("setWebviewHTMLContent: Set HTML for webview %u\n", webviewId);
    } else {
        webviewHTMLContent.erase(webviewId);
        printf("setWebviewHTMLContent: Cleared HTML for webview %u\n", webviewId);
    }
    fflush(stdout);
}

const char* getWebviewHTMLContent(uint32_t webviewId) {
    std::lock_guard<std::mutex> lock(webviewHTMLMutex);
    auto it = webviewHTMLContent.find(webviewId);
    if (it != webviewHTMLContent.end()) {
        char* result = strdup(it->second.c_str());
        printf("getWebviewHTMLContent: Retrieved HTML for webview %u\n", webviewId);
        fflush(stdout);
        return result;
    } else {
        printf("getWebviewHTMLContent: No HTML found for webview %u\n", webviewId);
        fflush(stdout);
        return nullptr;
    }
}

