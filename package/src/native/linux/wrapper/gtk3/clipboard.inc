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

