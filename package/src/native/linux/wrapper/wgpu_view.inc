// WGPUView implementation (non-webview rendering surface)
class WGPUViewImpl : public AbstractView {
public:
    GtkWidget* viewWidget = nullptr;
    Display* xDisplay = nullptr;
    Window parentXWindow = 0;
    Window xWindow = 0;
    Window inputXWindow = 0;
    Cursor inputCursor = 0;

    WGPUViewImpl(uint32_t webviewId)
        : AbstractView(webviewId) {}

    bool createX11Child(Display* display, Window parentWindow, double x, double y, double width, double height, const char* context) {
        if (!display || !parentWindow) {
            fprintf(stderr, "ERROR: Failed to create X11-backed WGPUView (%s): invalid parent\n", context);
            return false;
        }

        int screen = DefaultScreen(display);
        Visual* visual = DefaultVisual(display, screen);
        int depth = DefaultDepth(display, screen);

        XSetWindowAttributes attrs = {};
        attrs.border_pixel = 0;
        attrs.background_pixel = 0;
        attrs.colormap = DefaultColormap(display, screen);
        attrs.event_mask = StructureNotifyMask | ExposureMask | ButtonPressMask | FocusChangeMask;

        xDisplay = display;
        parentXWindow = parentWindow;
        xWindow = XCreateWindow(
            display,
            parentWindow,
            (int)x,
            (int)y,
            std::max(1, (int)width),
            std::max(1, (int)height),
            0,
            depth,
            InputOutput,
            visual,
            CWBorderPixel | CWBackPixel | CWColormap | CWEventMask,
            &attrs
        );

        if (!xWindow) {
            fprintf(stderr, "ERROR: XCreateWindow failed for WGPUView (%s)\n", context);
            xDisplay = nullptr;
            parentXWindow = 0;
            return false;
        }

        return true;
    }

    bool createX11InputChild(Display* display, Window parentWindow, double x, double y, double width, double height, const char* context) {
        if (!display || !parentWindow) {
            fprintf(stderr, "ERROR: Failed to create X11 input layer for WGPUView (%s): invalid parent\n", context);
            return false;
        }

        XSetWindowAttributes attrs = {};
        attrs.event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                           EnterWindowMask | LeaveWindowMask | FocusChangeMask |
                           StructureNotifyMask;

        unsigned long attrMask = CWEventMask;
        inputCursor = XCreateFontCursor(display, XC_left_ptr);
        if (inputCursor) {
            attrs.cursor = inputCursor;
            attrMask |= CWCursor;
        }

        inputXWindow = XCreateWindow(
            display,
            parentWindow,
            (int)x,
            (int)y,
            std::max(1, (int)width),
            std::max(1, (int)height),
            0,
            0,
            InputOnly,
            (Visual*)CopyFromParent,
            attrMask,
            &attrs
        );

        if (!inputXWindow) {
            fprintf(stderr, "ERROR: XCreateWindow failed for WGPUView input layer (%s)\n", context);
            if (inputCursor) {
                XFreeCursor(display, inputCursor);
                inputCursor = 0;
            }
            return false;
        }

        return true;
    }

    bool resolveX11DrawingSurface(Display*& display, Window& window) {
        display = nullptr;
        window = 0;

        if (xDisplay && xWindow) {
            display = xDisplay;
            window = xWindow;
            return true;
        }

        if (viewWidget) {
            GdkWindow* gdkWindow = gtk_widget_get_window(viewWidget);
            if (gdkWindow) {
                display = gdk_x11_display_get_xdisplay(gdk_window_get_display(gdkWindow));
                window = GDK_WINDOW_XID(gdkWindow);
                return display && window;
            }
        }

        return false;
    }

    void setInputShape(Display* display, Window window, const std::vector<MaskRect>& masks) {
        if (!display || !window) {
            return;
        }

        if (isMousePassthroughEnabled) {
            XRectangle emptyRect = {0, 0, 0, 0};
            XShapeCombineRectangles(display, window, ShapeInput, 0, 0, &emptyRect, 1, ShapeSet, YXBanded);
            return;
        }

        const LinuxXRectangleFields baseFields =
            linuxPhysicalRectToXRectangleFields({
                0,
                0,
                std::max(1, visualBounds.width),
                std::max(1, visualBounds.height),
            });
        XRectangle baseRect = {
            baseFields.x,
            baseFields.y,
            baseFields.width,
            baseFields.height,
        };
        XShapeCombineRectangles(display, window, ShapeInput, 0, 0, &baseRect, 1, ShapeSet, YXBanded);

        for (const auto& mask : masks) {
            const LinuxXRectangleFields fields =
                linuxPhysicalRectToXRectangleFields(
                    logicalToLinuxPhysicalRect(
                        mask.x, mask.y, mask.width, mask.height, 1.0));
            if (fields.width == 0 || fields.height == 0) continue;
            XRectangle rect = {
                fields.x,
                fields.y,
                fields.width,
                fields.height,
            };
            XShapeCombineRectangles(display, window, ShapeInput, 0, 0, &rect, 1, ShapeSubtract, YXBanded);
        }
    }

    void clearInputShape(Display* display, Window window) {
        if (!display || !window) {
            return;
        }

        XRectangle emptyRect = {0, 0, 0, 0};
        XShapeCombineRectangles(display, window, ShapeInput, 0, 0, &emptyRect, 1, ShapeSet, YXBanded);
    }

    void applyCurrentInputShape() {
        Display* display = nullptr;
        Window drawingWindow = 0;
        if (!resolveX11DrawingSurface(display, drawingWindow)) {
            return;
        }

        if (inputXWindow) {
            clearInputShape(display, drawingWindow);
            XRaiseWindow(display, inputXWindow);
        }

        std::vector<MaskRect> masks;
        if (!maskJSON.empty() && maskJSON != "[]") {
            masks = parseMaskJson(maskJSON);
        }

        setInputShape(display, inputXWindow ? inputXWindow : drawingWindow, masks);
        XFlush(display);
    }

    void loadURL(const char* urlString) override {}
    void loadHTML(const char* htmlString) override {}
    void goBack() override {}
    void goForward() override {}
    void reload() override {}
    void evaluateJavaScriptWithNoCompletion(const char* jsString) override {}
    void callAsyncJavascript(const char* messageId, const char* jsString, uint32_t webviewId, uint32_t hostWebviewId, void* completionHandler) override {}
    void addPreloadScriptToWebView(const char* jsString) override {}
    void updateCustomPreloadScript(const char* jsString) override {}

    void resize(const GdkRectangle& frame, const char* masksJson) override {
        if (xDisplay && xWindow) {
            XMoveResizeWindow(
                xDisplay,
                xWindow,
                frame.x,
                frame.y,
                std::max(1, frame.width),
                std::max(1, frame.height)
            );
            if (inputXWindow) {
                XMoveResizeWindow(
                    xDisplay,
                    inputXWindow,
                    frame.x,
                    frame.y,
                    std::max(1, frame.width),
                    std::max(1, frame.height)
                );
                XRaiseWindow(xDisplay, inputXWindow);
            } else {
                XRaiseWindow(xDisplay, xWindow);
            }
            XFlush(xDisplay);
            visualBounds = frame;
        } else if (viewWidget) {
            if (fullSize) {
                gtk_widget_set_size_request(viewWidget, -1, -1);
            } else {
                gtk_widget_set_size_request(viewWidget, frame.width, frame.height);
            }

            GtkWidget* wrapper = (GtkWidget*)g_object_get_data(G_OBJECT(viewWidget), "wrapper");
            if (wrapper) {
                int clampedX = MAX(0, frame.x);
                int clampedY = MAX(0, frame.y);
                int offsetX = frame.x - clampedX;
                int offsetY = frame.y - clampedY;

                gtk_widget_set_size_request(wrapper, frame.width, frame.height);
                gtk_widget_set_margin_start(wrapper, clampedX);
                gtk_widget_set_margin_top(wrapper, clampedY);
                gtk_fixed_move(GTK_FIXED(wrapper), viewWidget, offsetX / 2, offsetY / 2);
            } else {
                gtk_widget_set_margin_start(viewWidget, MAX(0, frame.x));
                gtk_widget_set_margin_top(viewWidget, MAX(0, frame.y));
            }

            visualBounds = frame;
        }
        maskJSON = masksJson ? masksJson : "";

        if (masksJson && strlen(masksJson) > 0 && strcmp(masksJson, "[]") != 0) {
            applyVisualMask();
        } else {
            removeMasks();
        }
    }

    void applyVisualMask() override {
        if (maskJSON.empty()) {
            return;
        }

        Display* display = nullptr;
        Window window = 0;
        if (!resolveX11DrawingSurface(display, window)) {
            return;
        }

        std::vector<MaskRect> masks = parseMaskJson(maskJSON);
        if (masks.empty()) {
            applyCurrentInputShape();
            return;
        }

        const LinuxXRectangleFields baseFields =
            linuxPhysicalRectToXRectangleFields({
                0,
                0,
                std::max(1, visualBounds.width),
                std::max(1, visualBounds.height),
            });
        XRectangle baseRect = {
            baseFields.x,
            baseFields.y,
            baseFields.width,
            baseFields.height,
        };
        XShapeCombineRectangles(display, window, ShapeBounding, 0, 0, &baseRect, 1, ShapeSet, YXBanded);

        for (const auto& mask : masks) {
            const LinuxXRectangleFields fields =
                linuxPhysicalRectToXRectangleFields(
                    logicalToLinuxPhysicalRect(
                        mask.x, mask.y, mask.width, mask.height, 1.0));
            if (fields.width == 0 || fields.height == 0) continue;
            XRectangle rect = {
                fields.x,
                fields.y,
                fields.width,
                fields.height,
            };
            XShapeCombineRectangles(display, window, ShapeBounding, 0, 0, &rect, 1, ShapeSubtract, YXBanded);
        }
        applyCurrentInputShape();
        XFlush(display);
    }

    void removeMasks() override {
        Display* display = nullptr;
        Window window = 0;
        if (!resolveX11DrawingSurface(display, window)) {
            return;
        }

        XShapeCombineMask(display, window, ShapeBounding, 0, 0, None, ShapeSet);
        maskJSON.clear();
        applyCurrentInputShape();
        XFlush(display);
    }

    void toggleMirrorMode(bool enable) override {
        if (mirrorModeEnabled == enable) return;
        mirrorModeEnabled = enable;
        if (xDisplay && xWindow) {
            setPassthrough(enable);
        } else if (viewWidget) {
            gtk_widget_set_sensitive(viewWidget, enable ? FALSE : TRUE);
        }
    }

    void setHidden(bool hidden) override {
        if (xDisplay && xWindow) {
            if (hidden) {
                if (inputXWindow) {
                    XUnmapWindow(xDisplay, inputXWindow);
                }
                XUnmapWindow(xDisplay, xWindow);
            } else {
                XMapRaised(xDisplay, xWindow);
                if (inputXWindow) {
                    XMapRaised(xDisplay, inputXWindow);
                }
            }
            XFlush(xDisplay);
        } else if (viewWidget) {
            hidden ? gtk_widget_hide(viewWidget) : gtk_widget_show(viewWidget);
        }
    }

    void setTransparent(bool transparent) override {
        if (xDisplay && xWindow) {
            (void)transparent;
        } else if (viewWidget) {
            gtk_widget_set_opacity(viewWidget, transparent ? 0.0 : 1.0);
        }
    }

    void setPassthrough(bool enable) override {
        AbstractView::setPassthrough(enable);
        if (xDisplay && xWindow) {
            applyCurrentInputShape();
        } else if (viewWidget) {
            gtk_widget_set_sensitive(viewWidget, enable ? FALSE : TRUE);
        }
    }

    void findInPage(const char* searchText, bool forward, bool matchCase) override {}
    void stopFindInPage() override {}
    void openDevTools() override {}
    void closeDevTools() override {}
    void toggleDevTools() override {}

    void remove() override {
        if (xDisplay && xWindow) {
            stopWgpuTestForWindow(xWindow);
            if (inputXWindow) {
                x11DestroyWindowSafe(xDisplay, inputXWindow);
                inputXWindow = 0;
            }
            x11DestroyWindowSafe(xDisplay, xWindow);
            if (inputCursor) {
                XFreeCursor(xDisplay, inputCursor);
                inputCursor = 0;
            }
            xWindow = 0;
            xDisplay = nullptr;
            parentXWindow = 0;
            widget = nullptr;
        } else if (viewWidget) {
            GtkWidget* wrapper = (GtkWidget*)g_object_get_data(G_OBJECT(viewWidget), "wrapper");
            if (wrapper) {
                gtk_widget_destroy(wrapper);
            } else {
                gtk_widget_destroy(viewWidget);
            }
            viewWidget = nullptr;
            widget = nullptr;
        }
        isRemoved = true;
    }

    bool canGoBack() override { return false; }
    bool canGoForward() override { return false; }
};

// Initialize static debounce timestamp for ctrl+click handling
double WebKitWebViewImpl::lastCtrlClickTime = 0;

