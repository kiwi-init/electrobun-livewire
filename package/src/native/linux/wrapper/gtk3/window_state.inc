// GTK3 implementation of the toolkit window functions (wrapper/toolkit.inc):
// the GTK branch of each window export in lifecycle_and_window_state.inc,
// moved here unchanged. extern "C++" because this part sits inside the
// wrapper's extern "C" block, and toolkit:: functions have C++ linkage.
extern "C++" {
namespace toolkit {

void closeWindow(void* window) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);

    // Find the container for this window to get the windowId and callback.
    // Hold a shared_ptr so the ContainerView stays alive through gtk_widget_destroy
    // (the "destroy" signal handler accesses it via raw pointer).
    uint32_t windowId = 0;
    WindowCloseCallback closeCallback = nullptr;
    std::shared_ptr<ContainerView> containerRef;
    {
        std::lock_guard<std::mutex> lock(g_containersMutex);
        for (auto& [id, container] : g_containers) {
            if (container->window == gtkWindow) {
                windowId = id;
                closeCallback = container->closeCallback;
                containerRef = container; // keep alive
                break;
            }
        }
    }

    // Call the close callback before destroying the widget.
    if (closeCallback && windowId > 0) {
        closeCallback(windowId);
    }

    // gtk_widget_destroy fires the "destroy" signal synchronously.
    // containerRef keeps the ContainerView alive so the signal handler's
    // raw ContainerView* user_data is valid for the duration of the call.
    gtk_widget_destroy(gtkWindow);
}

void requestWindowClose(void* window) {
    gtk_window_close(GTK_WINDOW(window));
    return;
}

void minimizeWindow(void* window) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
    if (GTK_IS_WINDOW(gtkWindow)) {
        gtk_window_iconify(GTK_WINDOW(gtkWindow));
        if (isWaylandWindow(gtkWindow)) {
            setMinimizeRequested(gtkWindow, true);
        }
    }
}

void restoreWindow(void* window) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
    if (GTK_IS_WINDOW(gtkWindow)) {
        gtk_window_deiconify(GTK_WINDOW(gtkWindow));
        gtk_window_present(GTK_WINDOW(gtkWindow));
        setMinimizeRequested(gtkWindow, false);
    }
}

void isWindowMinimized(void* window, bool& result) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
    if (GTK_IS_WINDOW(gtkWindow)) {
        GdkWindow* gdkWindow = gtk_widget_get_window(gtkWindow);
        if (gdkWindow) {
            GdkWindowState state = gdk_window_get_state(gdkWindow);
            result = (state & GDK_WINDOW_STATE_ICONIFIED) != 0;
        }
        if (!result && isWaylandWindow(gtkWindow)) {
            result = minimizeRequested(gtkWindow);
        }
    }
}

void maximizeWindow(void* window) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
    if (GTK_IS_WINDOW(gtkWindow)) {
        gtk_window_maximize(GTK_WINDOW(gtkWindow));
    }
}

void unmaximizeWindow(void* window) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
    if (GTK_IS_WINDOW(gtkWindow)) {
        gtk_window_unmaximize(GTK_WINDOW(gtkWindow));
    }
}

void isWindowMaximized(void* window, bool& result) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
    if (GTK_IS_WINDOW(gtkWindow)) {
        result = gtk_window_is_maximized(GTK_WINDOW(gtkWindow));
    }
}

void setWindowFullScreen(void* window, bool fullScreen) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
    if (GTK_IS_WINDOW(gtkWindow)) {
        if (fullScreen) {
            gtk_window_fullscreen(GTK_WINDOW(gtkWindow));
        } else {
            gtk_window_unfullscreen(GTK_WINDOW(gtkWindow));
        }
    }
}

void isWindowFullScreen(void* window, bool& result) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
    if (GTK_IS_WINDOW(gtkWindow)) {
        GdkWindow* gdkWindow = gtk_widget_get_window(gtkWindow);
        if (gdkWindow) {
            GdkWindowState state = gdk_window_get_state(gdkWindow);
            result = (state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
        }
    }
}

void setWindowAlwaysOnTop(void* window, bool alwaysOnTop) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
    if (GTK_IS_WINDOW(gtkWindow)) {
        gtk_window_set_keep_above(GTK_WINDOW(gtkWindow), alwaysOnTop ? TRUE : FALSE);
        // Focus the window when setting always on top to ensure visibility
        if (alwaysOnTop) {
            gtk_window_present(GTK_WINDOW(gtkWindow));
        }
    }
}

void isWindowAlwaysOnTop(void* window, bool& result) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
    if (GTK_IS_WINDOW(gtkWindow)) {
        GdkWindow* gdkWindow = gtk_widget_get_window(gtkWindow);
        if (gdkWindow) {
            GdkWindowState state = gdk_window_get_state(gdkWindow);
            result = (state & GDK_WINDOW_STATE_ABOVE) != 0;
        }
    }
}

void setWindowPosition(void* window, double x, double y) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
    if (GTK_IS_WINDOW(gtkWindow)) {
        gtk_window_move(GTK_WINDOW(gtkWindow), (int)x, (int)y);
    }
}

void setWindowSize(void* window, double width, double height) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
    if (GTK_IS_WINDOW(gtkWindow)) {
        gtk_window_resize(GTK_WINDOW(gtkWindow), (int)width, (int)height);
    }
}

void setWindowFrame(void* window, double x, double y, double width, double height) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
    if (GTK_IS_WINDOW(gtkWindow)) {
        gtk_window_move(GTK_WINDOW(gtkWindow), (int)x, (int)y);
        gtk_window_resize(GTK_WINDOW(gtkWindow), (int)width, (int)height);
    }
}

void getWindowFrame(void* window, double* outX, double* outY, double* outWidth, double* outHeight) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
    if (GTK_IS_WINDOW(gtkWindow)) {
        gint wx, wy, ww, wh;
        gtk_window_get_position(GTK_WINDOW(gtkWindow), &wx, &wy);
        gtk_window_get_size(GTK_WINDOW(gtkWindow), &ww, &wh);
        *outX = (double)wx;
        *outY = (double)wy;
        *outWidth = (double)ww;
        *outHeight = (double)wh;
    }
}

void getWindowContentOrigin(void* window, double* outX, double* outY) {
    GtkWidget* gtkWindow = static_cast<GtkWidget*>(window);
    if (!GTK_IS_WINDOW(gtkWindow)) return;

    GdkWindow* contentWindow = gtk_widget_get_window(gtkWindow);
    if (contentWindow) {
        gint contentX = 0;
        gint contentY = 0;
        gdk_window_get_origin(contentWindow, &contentX, &contentY);
        *outX = static_cast<double>(contentX);
        *outY = static_cast<double>(contentY);
        return;
    }

    // A not-yet-realized GTK window has no client GdkWindow. Preserve
    // the best available position until it is realized.
    gint windowX = 0;
    gint windowY = 0;
    gtk_window_get_position(GTK_WINDOW(gtkWindow), &windowX, &windowY);
    *outX = static_cast<double>(windowX);
    *outY = static_cast<double>(windowY);
    return;
}

void centerWindow(void* window, const GdkRectangle& workarea) {
    int width = 0;
    int height = 0;
    gtk_window_get_size(GTK_WINDOW(window), &width, &height);
    const int x = workarea.x + std::max(0, (workarea.width - width) / 2);
    const int y = workarea.y + std::max(0, (workarea.height - height) / 2);
    gtk_window_move(GTK_WINDOW(window), x, y);
}

void setWindowIcon(void* window, GdkPixbuf* pixbuf) {
    gtk_window_set_icon(GTK_WINDOW(window), pixbuf);
}

} // namespace toolkit
} // extern "C++"
