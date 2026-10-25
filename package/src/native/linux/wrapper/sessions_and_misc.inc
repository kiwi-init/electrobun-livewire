// Store for partition-specific data managers (for cookie access)
// Session calls must go through the partition's own context, the one its
// views use: a second data manager on the same directories is a second
// network session, so cookies set through it never reach the views. The first
// Session call on a partition takes a reference, so an ephemeral partition
// keeps its cookies for views created after them.
static std::set<std::string> g_sessionPartitions;

static WebKitWebsiteDataManager* getDataManagerForPartition(const char* partitionIdentifier) {
    std::string partition = partitionIdentifier ? partitionIdentifier : "";
    if (partition.empty()) {
        return webkit_web_context_get_website_data_manager(webkit_web_context_get_default());
    }
    if (g_sessionPartitions.insert(partition).second) {
        return webkit_web_context_get_website_data_manager(getContextForPartition(partition.c_str()));
    }
    return webkit_web_context_get_website_data_manager(g_partitionContexts.at(partition));
}


// ELECTROBUN_COOKIE_DEBUG=1 logs cookies that WebKit didn't store or didn't
// confirm in time (T1199: a cookie once went missing under load on CI).
static bool cookieDebug() {
    static const bool enabled = g_getenv("ELECTROBUN_COOKIE_DEBUG") != nullptr;
    return enabled;
}

// Helper struct for async cookie operations
struct CookieCallbackData {
    std::string* result;
    bool* done;
    GMainLoop* loop;
};

// Callback for getting cookies
static void finishCookieList(CookieCallbackData* data, GList* cookies, GError* error) {
    std::ostringstream json;
    json << "[";

    if (!error && cookies) {
        GList* item = cookies;
        bool first = true;
        while (item) {
            SoupCookie* cookie = static_cast<SoupCookie*>(item->data);
            if (!first) json << ",";
            first = false;

            json << "{";
            json << "\"name\":\"" << (soup_cookie_get_name(cookie) ?: "") << "\",";
            json << "\"value\":\"" << (soup_cookie_get_value(cookie) ?: "") << "\",";
            json << "\"domain\":\"" << (soup_cookie_get_domain(cookie) ?: "") << "\",";
            json << "\"path\":\"" << (soup_cookie_get_path(cookie) ?: "") << "\",";
            json << "\"secure\":" << (soup_cookie_get_secure(cookie) ? "true" : "false") << ",";
            json << "\"httpOnly\":" << (soup_cookie_get_http_only(cookie) ? "true" : "false") << ",";
            SoupSameSitePolicy sameSite = soup_cookie_get_same_site_policy(cookie);
            json << "\"sameSite\":\""
                 << (sameSite == SOUP_SAME_SITE_POLICY_STRICT ? "strict" : sameSite == SOUP_SAME_SITE_POLICY_LAX ? "lax" : "no_restriction")
                 << "\"";

            GDateTime* expires = soup_cookie_get_expires(cookie);
            if (expires) {
                json << ",\"expirationDate\":" << g_date_time_to_unix(expires);
            }

            json << "}";

            item = item->next;
        }
        g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);
    }

    if (error) {
        g_error_free(error);
    }

    json << "]";
    *(data->result) = json.str();
    *(data->done) = true;

    if (data->loop) {
        g_main_loop_quit(data->loop);
    }
}

static void onGetCookiesFinished(GObject* source, GAsyncResult* result, gpointer user_data) {
    GError* error = nullptr;
    GList* cookies = webkit_cookie_manager_get_cookies_finish(WEBKIT_COOKIE_MANAGER(source), result, &error);
    finishCookieList(static_cast<CookieCallbackData*>(user_data), cookies, error);
}

#if WEBKIT_CHECK_VERSION(2, 42, 0)
static void onGetAllCookiesFinished(GObject* source, GAsyncResult* result, gpointer user_data) {
    GError* error = nullptr;
    GList* cookies = webkit_cookie_manager_get_all_cookies_finish(WEBKIT_COOKIE_MANAGER(source), result, &error);
    finishCookieList(static_cast<CookieCallbackData*>(user_data), cookies, error);
}
#endif

// Get cookies for a partition (WebKit2GTK)
ELECTROBUN_EXPORT const char* sessionGetCookies(const char* partitionIdentifier, const char* filterJson) {
    // Copy arguments before dispatching to main thread
    std::string partitionStr = partitionIdentifier ? partitionIdentifier : "";
    std::string filterStr = filterJson ? filterJson : "{}";

    return dispatch_sync_main([partitionStr, filterStr]() -> const char* {
        WebKitWebsiteDataManager* dataManager = getDataManagerForPartition(partitionStr.c_str());
        if (!dataManager) {
            return strdup("[]");
        }

        WebKitCookieManager* cookieManager = webkit_website_data_manager_get_cookie_manager(dataManager);
        if (!cookieManager) {
            return strdup("[]");
        }

        // Parse filter for URL
        std::string filterUrl;

        size_t urlPos = filterStr.find("\"url\"");
        if (urlPos != std::string::npos) {
            size_t colonPos = filterStr.find(':', urlPos);
            size_t quoteStart = filterStr.find('"', colonPos);
            size_t quoteEnd = filterStr.find('"', quoteStart + 1);
            if (quoteStart != std::string::npos && quoteEnd != std::string::npos) {
                filterUrl = filterStr.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
            }
        }

        std::string result = "[]";
        bool done = false;

        CookieCallbackData callbackData;
        callbackData.result = &result;
        callbackData.done = &done;
        callbackData.loop = g_main_loop_new(NULL, FALSE);

        if (filterUrl.empty()) {
#if WEBKIT_CHECK_VERSION(2, 42, 0)
            webkit_cookie_manager_get_all_cookies(cookieManager, nullptr, onGetAllCookiesFinished, &callbackData);
#else
            webkit_cookie_manager_get_cookies(cookieManager, "https://localhost", nullptr, onGetCookiesFinished, &callbackData);
#endif
        } else {
            webkit_cookie_manager_get_cookies(cookieManager, filterUrl.c_str(), nullptr, onGetCookiesFinished, &callbackData);
        }

        // Run main loop until done or timeout
        GSource* timeout = g_timeout_source_new(5000);
        g_source_set_callback(timeout, [](gpointer data) -> gboolean {
            g_main_loop_quit(static_cast<GMainLoop*>(data));
            return G_SOURCE_REMOVE;
        }, callbackData.loop, nullptr);
        g_source_attach(timeout, g_main_loop_get_context(callbackData.loop));

        g_main_loop_run(callbackData.loop);
        g_source_destroy(timeout);
        g_source_unref(timeout);
        g_main_loop_unref(callbackData.loop);

        return strdup(result.c_str());
    });
}

// Callback for setting cookie
static void onSetCookieFinished(GObject* source, GAsyncResult* result, gpointer user_data) {
    CookieCallbackData* data = static_cast<CookieCallbackData*>(user_data);
    GError* error = nullptr;
    gboolean success = webkit_cookie_manager_add_cookie_finish(
        WEBKIT_COOKIE_MANAGER(source), result, &error);

    *(data->result) = success ? "true" : "false";
    *(data->done) = true;

    if (error) {
        if (cookieDebug()) {
            fprintf(stderr, "Session: cookie not stored: %s\n", error->message);
        }
        g_error_free(error);
    }

    if (data->loop) {
        g_main_loop_quit(data->loop);
    }
}

// Set a cookie (WebKit2GTK)
ELECTROBUN_EXPORT bool sessionSetCookie(const char* partitionIdentifier, const char* cookieJson) {
    // Copy arguments before dispatching to main thread
    std::string partitionStr = partitionIdentifier ? partitionIdentifier : "";
    std::string jsonStr = cookieJson ? cookieJson : "{}";

    return dispatch_sync_main([partitionStr, jsonStr]() -> bool {
        WebKitWebsiteDataManager* dataManager = getDataManagerForPartition(partitionStr.c_str());
        if (!dataManager) {
            return false;
        }

        WebKitCookieManager* cookieManager = webkit_website_data_manager_get_cookie_manager(dataManager);
        if (!cookieManager) {
            return false;
        }

        // Parse JSON (simple parsing)
        auto extractString = [&jsonStr](const std::string& key) -> std::string {
            std::string searchKey = "\"" + key + "\"";
            size_t pos = jsonStr.find(searchKey);
            if (pos == std::string::npos) return "";
            size_t colonPos = jsonStr.find(':', pos);
            size_t quoteStart = jsonStr.find('"', colonPos);
            size_t quoteEnd = jsonStr.find('"', quoteStart + 1);
            if (quoteStart != std::string::npos && quoteEnd != std::string::npos) {
                return jsonStr.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
            }
            return "";
        };

        auto extractBool = [&jsonStr](const std::string& key) -> bool {
            std::string searchKey = "\"" + key + "\"";
            size_t pos = jsonStr.find(searchKey);
            if (pos == std::string::npos) return false;
            size_t commaPos = jsonStr.find(',', pos);
            size_t truePos = jsonStr.find("true", pos);
            return truePos != std::string::npos && (commaPos == std::string::npos || truePos < commaPos);
        };

        auto extractDouble = [&jsonStr](const std::string& key) -> double {
            std::string searchKey = "\"" + key + "\"";
            size_t pos = jsonStr.find(searchKey);
            if (pos == std::string::npos) return 0;
            size_t colonPos = jsonStr.find(':', pos);
            size_t numStart = colonPos + 1;
            while (numStart < jsonStr.size() && (jsonStr[numStart] == ' ' || jsonStr[numStart] == '\t')) numStart++;
            try {
                return std::stod(jsonStr.substr(numStart));
            } catch (...) {
                return 0;
            }
        };

        std::string name = extractString("name");
        std::string value = extractString("value");
        std::string domain = extractString("domain");
        std::string path = extractString("path");
        std::string url = extractString("url");
        bool secure = extractBool("secure");
        bool httpOnly = extractBool("httpOnly");
        std::string sameSite = extractString("sameSite");
        double expirationDate = extractDouble("expirationDate");

        if (name.empty()) {
            return false;
        }

        // Derive domain from URL if not provided
        if (domain.empty() && !url.empty()) {
            size_t start = url.find("://");
            if (start != std::string::npos) {
                start += 3;
                size_t end = url.find('/', start);
                domain = url.substr(start, end - start);
            }
        }

        if (domain.empty()) {
            return false;
        }

        if (path.empty()) path = "/";

        // Create SoupCookie
        SoupCookie* cookie = soup_cookie_new(name.c_str(), value.c_str(), domain.c_str(), path.c_str(), -1);
        if (!cookie) {
            return false;
        }

        soup_cookie_set_secure(cookie, secure);
        soup_cookie_set_http_only(cookie, httpOnly);
        if (g_ascii_strcasecmp(sameSite.c_str(), "strict") == 0) {
            soup_cookie_set_same_site_policy(cookie, SOUP_SAME_SITE_POLICY_STRICT);
        } else if (g_ascii_strcasecmp(sameSite.c_str(), "lax") == 0) {
            soup_cookie_set_same_site_policy(cookie, SOUP_SAME_SITE_POLICY_LAX);
        } else if (g_ascii_strcasecmp(sameSite.c_str(), "no_restriction") == 0) {
            soup_cookie_set_same_site_policy(cookie, SOUP_SAME_SITE_POLICY_NONE);
        }

        if (expirationDate > 0) {
            GDateTime* expires = g_date_time_new_from_unix_utc((gint64)expirationDate);
            soup_cookie_set_expires(cookie, expires);
            g_date_time_unref(expires);
        }

        std::string result = "false";
        bool done = false;

        CookieCallbackData callbackData;
        callbackData.result = &result;
        callbackData.done = &done;
        callbackData.loop = g_main_loop_new(NULL, FALSE);

        webkit_cookie_manager_add_cookie(cookieManager, cookie, nullptr, onSetCookieFinished, &callbackData);

        // Run main loop until done or timeout
        GSource* timeout = g_timeout_source_new(5000);
        g_source_set_callback(timeout, [](gpointer data) -> gboolean {
            g_main_loop_quit(static_cast<GMainLoop*>(data));
            return G_SOURCE_REMOVE;
        }, callbackData.loop, nullptr);
        g_source_attach(timeout, g_main_loop_get_context(callbackData.loop));

        g_main_loop_run(callbackData.loop);
        g_source_destroy(timeout);
        g_source_unref(timeout);
        g_main_loop_unref(callbackData.loop);
        if (!done && cookieDebug()) {
            fprintf(stderr, "Session: cookie %s not confirmed within 5 s\n", name.c_str());
        }

        soup_cookie_free(cookie);

        return result == "true";
    });
}

// Callback for deleting cookie
static void onDeleteCookieFinished(GObject* source, GAsyncResult* result, gpointer user_data) {
    CookieCallbackData* data = static_cast<CookieCallbackData*>(user_data);
    GError* error = nullptr;
    gboolean success = webkit_cookie_manager_delete_cookie_finish(
        WEBKIT_COOKIE_MANAGER(source), result, &error);

    *(data->result) = success ? "true" : "false";
    *(data->done) = true;

    if (error) {
        g_error_free(error);
    }

    if (data->loop) {
        g_main_loop_quit(data->loop);
    }
}

// Remove a specific cookie (WebKit2GTK)
ELECTROBUN_EXPORT bool sessionRemoveCookie(const char* partitionIdentifier, const char* urlStr, const char* cookieName) {
    if (!urlStr || !cookieName) return false;

    // Copy arguments before dispatching to main thread
    std::string partitionStr = partitionIdentifier ? partitionIdentifier : "";
    std::string urlString = urlStr;
    std::string nameString = cookieName;

    return dispatch_sync_main([partitionStr, urlString, nameString]() -> bool {
        WebKitWebsiteDataManager* dataManager = getDataManagerForPartition(partitionStr.c_str());
        if (!dataManager) {
            return false;
        }

        WebKitCookieManager* cookieManager = webkit_website_data_manager_get_cookie_manager(dataManager);
        if (!cookieManager) {
            return false;
        }

        // First get all cookies for the URL, then delete the matching one
        std::string result = "[]";
        bool done = false;

        CookieCallbackData callbackData;
        callbackData.result = &result;
        callbackData.done = &done;
        callbackData.loop = g_main_loop_new(NULL, FALSE);

        // Get cookies first
        webkit_cookie_manager_get_cookies(cookieManager, urlString.c_str(), nullptr,
            [](GObject* source, GAsyncResult* result, gpointer user_data) {
                CookieCallbackData* data = static_cast<CookieCallbackData*>(user_data);
                GError* error = nullptr;
                GList* cookies = webkit_cookie_manager_get_cookies_finish(
                    WEBKIT_COOKIE_MANAGER(source), result, &error);

                if (cookies) {
                    // Store cookies list in result for now
                    *(data->result) = std::to_string(reinterpret_cast<uintptr_t>(cookies));
                } else {
                    *(data->result) = "0";
                }
                *(data->done) = true;

                if (error) {
                    g_error_free(error);
                }

                if (data->loop) {
                    g_main_loop_quit(data->loop);
                }
            }, &callbackData);

        GSource* timeout = g_timeout_source_new(5000);
        g_source_set_callback(timeout, [](gpointer data) -> gboolean {
            g_main_loop_quit(static_cast<GMainLoop*>(data));
            return G_SOURCE_REMOVE;
        }, callbackData.loop, nullptr);
        g_source_attach(timeout, g_main_loop_get_context(callbackData.loop));

        g_main_loop_run(callbackData.loop);
        g_source_destroy(timeout);
        g_source_unref(timeout);
        g_main_loop_unref(callbackData.loop);

        // Parse the cookies list pointer
        GList* cookies = reinterpret_cast<GList*>(std::stoull(result));
        bool found = false;

        if (cookies) {
            GList* item = cookies;
            while (item) {
                SoupCookie* cookie = static_cast<SoupCookie*>(item->data);
                if (std::string(soup_cookie_get_name(cookie)) == nameString) {
                    // Delete this cookie
                    done = false;
                    result = "false";
                    callbackData.loop = g_main_loop_new(NULL, FALSE);

                    webkit_cookie_manager_delete_cookie(cookieManager, cookie, nullptr, onDeleteCookieFinished, &callbackData);

                    timeout = g_timeout_source_new(5000);
                    g_source_set_callback(timeout, [](gpointer data) -> gboolean {
                        g_main_loop_quit(static_cast<GMainLoop*>(data));
                        return G_SOURCE_REMOVE;
                    }, callbackData.loop, nullptr);
                    g_source_attach(timeout, g_main_loop_get_context(callbackData.loop));

                    g_main_loop_run(callbackData.loop);
                    g_source_destroy(timeout);
                    g_source_unref(timeout);
                    g_main_loop_unref(callbackData.loop);

                    found = (result == "true");
                    break;
                }
                item = item->next;
            }
            g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);
        }

        return found;
    });
}

// Clear all cookies (WebKit2GTK)
// Clear all cookies (WebKit2GTK) - STUB implementation to prevent crashes
ELECTROBUN_EXPORT void sessionClearStorageData(const char* partitionIdentifier, const char* storageTypesJson);

// Remove every cookie in the partition, and wait until WebKit has.
ELECTROBUN_EXPORT void sessionClearCookies(const char* partitionIdentifier) {
    sessionClearStorageData(partitionIdentifier, "[\"cookies\"]");
}

// Clear storage data (WebKit2GTK)
ELECTROBUN_EXPORT void sessionClearStorageData(const char* partitionIdentifier, const char* storageTypesJson) {
    // Copy arguments before dispatching to main thread
    std::string partitionStr = partitionIdentifier ? partitionIdentifier : "";
    std::string typesStr = storageTypesJson ? storageTypesJson : "";

    dispatch_sync_main_void([partitionStr, typesStr]() {
        WebKitWebsiteDataManager* dataManager = getDataManagerForPartition(partitionStr.c_str());
        if (!dataManager) {
            return;
        }

        unsigned int typesFlags = 0;

        if (typesStr.length() > 2) {
            if (typesStr.find("cookies") != std::string::npos) {
                typesFlags |= WEBKIT_WEBSITE_DATA_COOKIES;
            }
            if (typesStr.find("localStorage") != std::string::npos) {
                typesFlags |= WEBKIT_WEBSITE_DATA_LOCAL_STORAGE;
            }
            if (typesStr.find("indexedDB") != std::string::npos) {
                typesFlags |= WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES;
            }
            if (typesStr.find("cache") != std::string::npos) {
                typesFlags |= WEBKIT_WEBSITE_DATA_DISK_CACHE;
                typesFlags |= WEBKIT_WEBSITE_DATA_MEMORY_CACHE;
            }
            if (typesStr.find("serviceWorkers") != std::string::npos) {
                typesFlags |= WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS;
            }
        } else {
            // Clear all
            typesFlags = WEBKIT_WEBSITE_DATA_ALL;
        }

        if (typesFlags == 0) {
            return;
        }

        WebKitWebsiteDataTypes types = static_cast<WebKitWebsiteDataTypes>(typesFlags);

        GMainLoop* loop = g_main_loop_new(NULL, FALSE);

        webkit_website_data_manager_clear(dataManager, types, 0, nullptr,
            [](GObject* source, GAsyncResult* result, gpointer user_data) {
                GMainLoop* loop = static_cast<GMainLoop*>(user_data);
                GError* error = nullptr;
                webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), result, &error);
                if (error) {
                    g_error_free(error);
                }
                g_main_loop_quit(loop);
            }, loop);

        GSource* timeout = g_timeout_source_new(10000);
        g_source_set_callback(timeout, [](gpointer data) -> gboolean {
            g_main_loop_quit(static_cast<GMainLoop*>(data));
            return G_SOURCE_REMOVE;
        }, loop, nullptr);
        g_source_attach(timeout, g_main_loop_get_context(loop));

        g_main_loop_run(loop);
        g_source_destroy(timeout);
        g_source_unref(timeout);
        g_main_loop_unref(loop);
    });
}

ELECTROBUN_EXPORT void setURLOpenHandler(void (*callback)(const char*)) {
    (void)callback;
    // Not supported on Linux - stub to prevent dlopen failure
    // Linux URL protocol handling is done via desktop file associations
}

ELECTROBUN_EXPORT void setAppReopenHandler(void (*callback)()) {
    (void)callback;
    // Not supported on Linux - stub to prevent dlopen failure
}

ELECTROBUN_EXPORT void setAppWakeHandler(void (*callback)()) {
    (void)callback;
    // Not supported on Linux yet - stub to prevent dlopen failure
}

ELECTROBUN_EXPORT void setNotificationResponseHandler(void (*handler)(char*)) {
    (void)handler;
    // Not supported on Linux yet - stub to prevent dlopen failure
}

ELECTROBUN_EXPORT void freeNativeString(char *value) {
    free(value);
}

// Bun only calls this on macOS; other platforms use showNotification.
ELECTROBUN_EXPORT void showNotificationWithOptions(const char *optionsJson) {
    (void)optionsJson;
}

ELECTROBUN_EXPORT void setDockBadge(const char *label) {
    (void)label;
    // Not supported on Linux yet - stub to prevent dlopen failure
}

ELECTROBUN_EXPORT char *getDockBadge() {
    return nullptr;
}

ELECTROBUN_EXPORT void setDockProgress(double progress) {
    (void)progress;
    // Not supported on Linux yet - stub to prevent dlopen failure
}

ELECTROBUN_EXPORT double getDockProgress() {
    return -1;
}

ELECTROBUN_EXPORT void setDockIconVisible(bool visible) {
    (void)visible;
    // Not supported on Linux - stub to prevent dlopen failure
}

ELECTROBUN_EXPORT bool isDockIconVisible() {
    // Not supported on Linux
    return true;
}

// Graceful shutdown function to coordinate cleanup
ELECTROBUN_EXPORT void shutdownNativeWrapper() {
    printf("Starting graceful shutdown of native wrapper...\n");

    // Use the same coordinated path as Utils.quit. Calling CefShutdown here
    // while browser close callbacks are pending can tear down profile services
    // that those browsers still reference.
    stopEventLoop();
}

}
