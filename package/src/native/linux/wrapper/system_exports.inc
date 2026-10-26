ELECTROBUN_EXPORT bool moveToTrash(char* pathString) {
    if (!pathString) return false;
    
    // Use GIO to move file to trash
    GFile* file = g_file_new_for_path(pathString);
    GError* error = nullptr;
    
    gboolean result = g_file_trash(file, nullptr, &error);
    
    if (error) {
        fprintf(stderr, "Failed to move to trash: %s\n", error->message);
        g_error_free(error);
    }
    
    g_object_unref(file);
    return result == TRUE;
}

void showItemInFolder(char* path) {
    if (!path) return;
    
    // Check if path exists
    struct stat sb;
    if (stat(path, &sb) != 0) {
        fprintf(stderr, "Path does not exist: %s\n", path);
        return;
    }
    
    // Get the parent directory if it's a file
    gchar* parentDir = nullptr;
    if (S_ISREG(sb.st_mode)) {
        parentDir = g_path_get_dirname(path);
    } else {
        parentDir = g_strdup(path);
    }
    
    // Try to open with the default file manager
    // Most Linux desktop environments support xdg-open
    gchar* uri = g_filename_to_uri(parentDir, nullptr, nullptr);
    if (uri) {
        // xdg-open works across desktop environments; gio open if it fails.
        if (!electrobun::spawn_fallbacks::xdgOpen(uri) && !electrobun::spawn_fallbacks::gioOpen(uri)) {
            fprintf(stderr, "Failed to open file manager for: %s\n", path);
        }
        g_free(uri);
    }
    
    g_free(parentDir);
}

// Open a URL in the default browser or appropriate application
ELECTROBUN_EXPORT bool openExternal(const char* urlString) {
    if (!urlString) {
        fprintf(stderr, "ERROR: NULL URL passed to openExternal\n");
        return false;
    }

    std::string url(urlString);
    if (url.empty()) {
        fprintf(stderr, "ERROR: Empty URL passed to openExternal\n");
        return false;
    }

    GError* error = nullptr;

    // Use g_app_info_launch_default_for_uri to open the URL with default app
    gboolean result = g_app_info_launch_default_for_uri(urlString, nullptr, &error);

    if (error) {
        fprintf(stderr, "GIO failed to open URL: %s - trying xdg-open\n", error->message);
        g_error_free(error);

        // Fallback to xdg-open
        if (!electrobun::spawn_fallbacks::xdgOpen(url)) {
            fprintf(stderr, "ERROR: Failed to open external URL: %s\n", urlString);
            return false;
        }
        return true;
    }

    return result == TRUE;
}

// Open a file or folder with the default application
ELECTROBUN_EXPORT bool openPath(const char* pathString) {
    if (!pathString) {
        fprintf(stderr, "ERROR: NULL path passed to openPath\n");
        return false;
    }

    std::string path(pathString);
    if (path.empty()) {
        fprintf(stderr, "ERROR: Empty path passed to openPath\n");
        return false;
    }

    // Convert path to URI
    gchar* uri = g_filename_to_uri(pathString, nullptr, nullptr);
    if (!uri) {
        fprintf(stderr, "ERROR: Failed to convert path to URI: %s\n", pathString);
        return false;
    }

    GError* error = nullptr;

    // Use g_app_info_launch_default_for_uri to open with default app
    gboolean result = g_app_info_launch_default_for_uri(uri, nullptr, &error);

    if (error) {
        fprintf(stderr, "GIO failed to open path: %s - trying xdg-open\n", error->message);
        g_error_free(error);

        // Fallback to xdg-open
        const bool opened = electrobun::spawn_fallbacks::xdgOpen(uri);
        g_free(uri);

        if (!opened) {
            fprintf(stderr, "ERROR: Failed to open path: %s\n", pathString);
            return false;
        }
        return true;
    }

    g_free(uri);
    return result == TRUE;
}

// Show a native desktop notification: the Notification portal in a sandbox,
// else org.freedesktop.Notifications, else notify-send.
void showNotification(const char* title, const char* body, const char* subtitle, bool silent) {
    if (!title) {
        fprintf(stderr, "ERROR: NULL title passed to showNotification\n");
        return;
    }

    std::string titleStr(title);
    std::string bodyStr;

    // Combine subtitle and body if both exist
    if (subtitle && strlen(subtitle) > 0) {
        bodyStr = std::string(subtitle);
        if (body && strlen(body) > 0) {
            bodyStr += "\n" + std::string(body);
        }
    } else if (body) {
        bodyStr = std::string(body);
    }

    // Execute asynchronously to not block
    std::thread([titleStr, bodyStr, silent]() {
        namespace portal = electrobun::xdg_portal;
        if (portal::sandboxed() && portal::interfaceVersion(portal::kNotificationInterface) > 0 &&
            portal::addNotification(portal::newToken(), titleStr, bodyStr, silent)) {
            return;
        }
        const char* appName = g_get_application_name();
        if (portal::notifyFreedesktop(appName ? appName : "Electrobun", titleStr, bodyStr, silent)) {
            return;
        }
        electrobun::spawn_fallbacks::notifySend(titleStr, bodyStr, silent);
    }).detach();
}

