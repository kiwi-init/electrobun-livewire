static uint32_t webviewIdForSchemeRequest(WebKitURISchemeRequest* request) {
    WebKitWebView* requestingWebView = webkit_uri_scheme_request_get_web_view(request);
    if (!requestingWebView) return 0;
    const uint32_t storedWebviewId = GPOINTER_TO_UINT(
        g_object_get_data(G_OBJECT(requestingWebView), kWebviewIdDataKey));
    if (storedWebviewId != 0) return storedWebviewId;
    std::lock_guard<std::mutex> lock(g_webviewMapMutex);
    for (auto& [id, view] : g_webviewMap) {
        auto* wkImpl = dynamic_cast<WebKitWebViewImpl*>(view.get());
        if (wkImpl && wkImpl->webview == GTK_WIDGET(requestingWebView)) return id;
    }
    return 0;
}

static void finishSchemeResponse(WebKitURISchemeRequest* request,
                                 uint32_t webviewId,
                                 gchar* contents,
                                 gsize size,
                                 const char* mimeType) {
    GInputStream* stream = g_memory_input_stream_new_from_data(contents, size, g_free);
#if WEBKIT_CHECK_VERSION(2, 36, 0)
    WebKitURISchemeResponse* response = webkit_uri_scheme_response_new(stream, size);
    webkit_uri_scheme_response_set_content_type(response, mimeType);
    SoupMessageHeaders* headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
    // Older WebKitGTK takes the MIME type and length from these headers once
    // any are set, so they have to be here too, not only on the response.
    soup_message_headers_set_content_type(headers, mimeType, nullptr);
    soup_message_headers_set_content_length(headers, static_cast<goffset>(size));
    soup_message_headers_append(headers, "Access-Control-Allow-Origin", "*");
    soup_message_headers_append(headers, "X-Content-Type-Options", "nosniff");
    std::string contentSecurityPolicy = electrobun::viewContentSecurityPolicy(webviewId);
    if (!contentSecurityPolicy.empty()) {
        soup_message_headers_append(headers, "Content-Security-Policy", contentSecurityPolicy.c_str());
    }
    webkit_uri_scheme_response_set_http_headers(response, headers);
    webkit_uri_scheme_request_finish_with_response(request, response);
    g_object_unref(response);
#else
    webkit_uri_scheme_request_finish(request, stream, size, mimeType);
#endif
    g_object_unref(stream);
}

static void handleAppDataURIScheme(WebKitURISchemeRequest* request, gpointer user_data) {
    const uint32_t webviewId = webviewIdForSchemeRequest(request);
    if (!protocolAllowed(webviewId, true)) {
        GError* error = g_error_new(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "appdata:// is not enabled");
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }
    const char* uri = webkit_uri_scheme_request_get_uri(request);
    const std::string relative = normalizeViewsRelativePath(uri ? uri : "");
    std::string data;
    if (!readContainedFile(appDataRoot(), relative, data)) {
        GError* error = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "File not found");
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }
    gchar* contents = static_cast<gchar*>(g_memdup2(data.data(), data.size()));
    finishSchemeResponse(request, webviewId, contents, data.size(), getMimeTypeFromUrl(relative).c_str());
}

// views:// URI scheme handler callback
static void handleViewsURIScheme(WebKitURISchemeRequest* request, gpointer user_data) {
    const uint32_t requestingWebviewId = webviewIdForSchemeRequest(request);
    if (!protocolAllowed(requestingWebviewId, false)) {
        GError* error = g_error_new(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "views:// is not enabled");
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }
    const char* uri = webkit_uri_scheme_request_get_uri(request);
    
    // Parse the full URI to get everything after views://
    // For views://webviewtag/index.html, we want "webviewtag/index.html"
    std::string fullPathString = normalizeViewsRelativePath(uri ? std::string(uri) : std::string());
    const char* fullPath = fullPathString.c_str();
    
    // Check if this is the internal HTML request
    if (strcmp(fullPath, "internal/index.html") == 0) {
        fflush(stdout);
        
        // Resolve the webviewId by matching the requesting WebKitWebView to g_webviewMap
        uint32_t webviewId = 0;
        WebKitWebView* requestingWebView = webkit_uri_scheme_request_get_web_view(request);
        if (requestingWebView) {
            std::lock_guard<std::mutex> lock(g_webviewMapMutex);
            for (auto& [id, view] : g_webviewMap) {
                auto* wkImpl = dynamic_cast<WebKitWebViewImpl*>(view.get());
                if (wkImpl && wkImpl->webview == GTK_WIDGET(requestingWebView)) {
                    webviewId = id;
                    break;
                }
            }
        }
        
        // Use stored HTML content for this specific webview
        const char* htmlContent = getWebviewHTMLContent(webviewId);
        if (htmlContent) {
            finishSchemeResponse(request, webviewId, g_strdup(htmlContent), strlen(htmlContent), "text/html");
            free((void*)htmlContent); // Free the strdup'd memory
            fflush(stdout);
            return;
        } else {
            fflush(stdout);
            const char* fallbackHTML = "<html><body>No content set</body></html>";
            finishSchemeResponse(request, webviewId, g_strdup(fallbackHTML), strlen(fallbackHTML), "text/html");
            return;
        }
    }
    
    // Check if this webview has a custom viewsRoot
    std::string viewsRootPath;
    {
        // First get the webviewId for the requesting WebKitWebView
        uint32_t webviewId = 0;
        WebKitWebView* requestingWebView = webkit_uri_scheme_request_get_web_view(request);
        if (requestingWebView) {
            std::lock_guard<std::mutex> lock(g_webviewMapMutex);
            for (auto& [id, view] : g_webviewMap) {
                auto* wkImpl = dynamic_cast<WebKitWebViewImpl*>(view.get());
                if (wkImpl && wkImpl->webview == GTK_WIDGET(requestingWebView)) {
                    webviewId = id;
                    break;
                }
            }
        }
        
        // Now check if this webview has a custom viewsRoot
        if (webviewId > 0) {
            std::lock_guard<std::mutex> lock(g_webviewViewsRootMutex);
            auto it = g_webviewViewsRoot.find(webviewId);
            if (it != g_webviewViewsRoot.end()) {
                viewsRootPath = it->second;
            }
        }
    }
    
    gchar* fileContents = nullptr;
    gsize fileSize = 0;
    bool foundFile = false;
    
    // If viewsRoot is set, try to read from that directory first
    if (!viewsRootPath.empty()) {
        std::string contents;
        if (readContainedFile(viewsRootPath, fullPath, contents)) {
            fileContents = static_cast<gchar*>(g_memdup2(contents.data(), contents.size()));
            fileSize = contents.size();
            foundFile = true;
        }
    }
    
    // Build paths relative to current directory (bin)
    char* cwd = g_get_current_dir();
    gchar* resourcesDir = g_build_filename(cwd, "..", "Resources", nullptr);
    gchar* asarPath = g_build_filename(resourcesDir, "app.asar", nullptr);

    // Check if ASAR archive exists (only if file not found in viewsRoot)
    if (!foundFile && g_file_test(asarPath, G_FILE_TEST_EXISTS)) {
        // Thread-safe lazy-load ASAR archive on first use
        std::call_once(g_asarArchiveInitFlag, [asarPath]() {
            g_asarArchive = asar_open(asarPath);
            if (g_asarArchive) {
                fflush(stdout);
            } else {
                printf("ERROR WebKit loadViewsFile: Failed to open ASAR archive at %s\n", asarPath);
                fflush(stdout);
            }
        });

        // If ASAR archive is loaded, try to read from it
        if (g_asarArchive) {
            // The ASAR contains the entire app directory, so prepend "views/" to the path
            std::string asarFilePath = "views/" + std::string(fullPath);

            size_t asarFileSize = 0;
            const uint8_t* fileData = nullptr;
            
            // Protect ASAR read operations with mutex
            {
                std::lock_guard<std::mutex> lock(g_asarReadMutex);
                fileData = asar_read_file(g_asarArchive, asarFilePath.c_str(), &asarFileSize);
                
                if (fileData && asarFileSize > 0) {
                    fflush(stdout);
                    // Copy the data (glib will free it)
                    fileContents = (gchar*)g_memdup2(fileData, asarFileSize);
                    fileSize = asarFileSize;
                    foundFile = true;
                    // Free the ASAR buffer before releasing the lock
                    asar_free_buffer(fileData, asarFileSize);
                }
            }
            
            if (!foundFile) {
                fflush(stdout);
                // Fall through to flat file reading
            }
        }
    }

    // Fallback: Read from flat file system (for non-ASAR builds or missing files)
    if (!foundFile) {
        gchar* viewsDir = g_build_filename(resourcesDir, "app", "views", nullptr);
        std::string contents;
        if (readContainedFile(viewsDir, fullPath, contents)) {
            fileContents = static_cast<gchar*>(g_memdup2(contents.data(), contents.size()));
            fileSize = contents.size();
            foundFile = true;
        }
        g_free(viewsDir);
    }

    // Send response if file was found
    if (foundFile && fileContents) {
        // Determine MIME type using shared function
        std::string mimeTypeStr = getMimeTypeFromUrl(fullPath);
        const char* mimeType = mimeTypeStr.c_str();

        finishSchemeResponse(request, requestingWebviewId, fileContents, fileSize, mimeType);
    } else {
        // Return 404 error
        GError* responseError = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "File not found: %s", fullPath);
        webkit_uri_scheme_request_finish_error(request, responseError);
        g_error_free(responseError);
    }

    // Cleanup
    g_free(cwd);
    g_free(resourcesDir);
    g_free(asarPath);
}

