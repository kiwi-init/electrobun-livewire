// Container for managing multiple webviews
class ContainerView {
public:
    GtkWidget* window;
    GtkWidget* overlay;
    std::vector<std::shared_ptr<AbstractView>> abstractViews;
    AbstractView* activeWebView = nullptr;
    uint32_t windowId;
    WindowCloseCallback closeCallback;
    WindowShouldCloseHandler shouldCloseCallback;
    WindowMoveCallback moveCallback;
    WindowResizeCallback resizeCallback;
    WindowFocusCallback focusCallback;
    WindowBlurCallback blurCallback;
    WindowKeyHandler keyCallback;
  
    ContainerView(GtkWidget* window) : window(window), windowId(0), closeCallback(nullptr), shouldCloseCallback(nullptr), moveCallback(nullptr), resizeCallback(nullptr), focusCallback(nullptr), blurCallback(nullptr), keyCallback(nullptr) {


        // Create an overlay container as the main container
        overlay = gtk_overlay_new();
        gtk_widget_set_hexpand(overlay, TRUE);
        gtk_widget_set_vexpand(overlay, TRUE);
        gtk_container_add(GTK_CONTAINER(window), overlay);
        
        gtk_widget_show(overlay);
    }
    
    ContainerView(GtkWidget* window, uint32_t windowId, WindowCloseCallback closeCallback, WindowShouldCloseHandler shouldCloseCallback, WindowMoveCallback moveCallback, WindowResizeCallback resizeCallback, WindowFocusCallback focusCallback, WindowBlurCallback blurCallback, WindowKeyHandler keyCallback)
        : window(window), windowId(windowId), closeCallback(closeCallback), shouldCloseCallback(shouldCloseCallback), moveCallback(moveCallback), resizeCallback(resizeCallback), focusCallback(focusCallback), blurCallback(blurCallback), keyCallback(keyCallback) {
        // Create an overlay container as the main container
        overlay = gtk_overlay_new();
        gtk_widget_set_hexpand(overlay, TRUE);
        gtk_widget_set_vexpand(overlay, TRUE);
        gtk_container_add(GTK_CONTAINER(window), overlay);
        
        gtk_widget_show(overlay);
    }
    
    
    void addWebview(std::shared_ptr<AbstractView> view, double x = 0, double y = 0) {
        abstractViews.insert(abstractViews.begin(), view);
        if (view->widget) {
            // Prevent webview from affecting window size
            g_object_set(view->widget,
                        "expand", FALSE,
                        "hexpand", FALSE,
                        "vexpand", FALSE,
                        NULL);
            
            // Add webview to overlay container
            if (abstractViews.size() == 1) {
                // First webview becomes the base layer (determines overlay size)
                g_object_set(view->widget,
                            "expand", TRUE,
                            "hexpand", TRUE,
                            "vexpand", TRUE,
                            NULL);

                gtk_container_add(GTK_CONTAINER(overlay), view->widget);
                
                // Now that widget is anchored, realize it for rendering
                gtk_widget_realize(view->widget);
                
                // Apply pending transparency/passthrough flags now that widget is realized
                if (view->pendingStartTransparent) {
                    view->setTransparent(true);
                    view->pendingStartTransparent = false;
                }
                if (view->pendingStartPassthrough) {
                    view->setPassthrough(true);
                    view->pendingStartPassthrough = false;
                }
            } else if (view->fullSize) {
                // Full-size subsequent webview: add directly as overlay (same as first webview)
                // Set it to expand and fill the overlay
                g_object_set(view->widget,
                            "expand", TRUE,
                            "hexpand", TRUE,
                            "vexpand", TRUE,
                            NULL);

                gtk_overlay_add_overlay(GTK_OVERLAY(overlay), view->widget);

                // Now that widget is anchored, realize it for rendering
                gtk_widget_realize(view->widget);

                // Apply pending transparency/passthrough flags now that widget is realized
                if (view->pendingStartTransparent) {
                    view->setTransparent(true);
                    view->pendingStartTransparent = false;
                }
                if (view->pendingStartPassthrough) {
                    view->setPassthrough(true);
                    view->pendingStartPassthrough = false;
                }
            } else {
                // For OOPIFs, wrap in a fixed container to enforce size constraints
                GtkWidget* wrapper = gtk_fixed_new();
                gtk_widget_set_size_request(wrapper, 1, 1); // Don't affect overlay size
                gtk_widget_set_halign(wrapper, GTK_ALIGN_START);
                gtk_widget_set_valign(wrapper, GTK_ALIGN_START);

                // Make wrapper receive no events (pass through to widgets below)
                gtk_widget_set_events(wrapper, 0);
                gtk_widget_set_can_focus(wrapper, FALSE);

                // Add webview to wrapper at 0,0
                gtk_fixed_put(GTK_FIXED(wrapper), view->widget, 0, 0);
                g_object_set_data(G_OBJECT(view->widget), "wrapper", wrapper);

                // Add wrapper as overlay layer
                gtk_overlay_add_overlay(GTK_OVERLAY(overlay), wrapper);

                // The wrapper is exactly the positioned webview's allocation.
                // It must remain an input target; the WGPU layer's XShape hole
                // handles pass-through outside this rectangle.
                gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), wrapper, FALSE);

                // Position wrapper using margins (will be updated in resize)
                gtk_widget_set_margin_start(wrapper, (int)x);
                gtk_widget_set_margin_top(wrapper, (int)y);

                gtk_widget_show(wrapper);

                // Realize only after the wrapper is attached to the toplevel.
                // Realizing while the GtkFixed is still unanchored is rejected
                // by GTK and can leave positioned WebKit views without a surface.
                gtk_widget_realize(view->widget);

                // Apply pending transparency/passthrough flags now that widget is realized
                if (view->pendingStartTransparent) {
                    view->setTransparent(true);
                    view->pendingStartTransparent = false;
                }
                if (view->pendingStartPassthrough) {
                    view->setPassthrough(true);
                    view->pendingStartPassthrough = false;
                }
            }
            
            if (auto* webKitView = dynamic_cast<WebKitWebViewImpl*>(view.get())) {
                webKitView->applyVisibilityState();
            } else {
                gtk_widget_show(view->widget);
            }
        }
    }
    
    void removeView(uint32_t webviewId) {
        auto it = std::find_if(abstractViews.begin(), abstractViews.end(),
            [webviewId](const std::shared_ptr<AbstractView>& view) {
                return view->webviewId == webviewId;
            });
        
        if (it != abstractViews.end()) {
            if ((*it)->widget) {
                gtk_widget_destroy((*it)->widget);
            }
            abstractViews.erase(it);
        }
    }
    
    void resizeAutoSizingViews(int width, int height) {
        // Skip if no webviews have been added yet (timing issue during window creation)
        if (abstractViews.empty()) {
            return;
        }
        
        GdkRectangle frame = { 0, 0, width, height };
        
        for (auto& view : abstractViews) {
            // Skip removed webviews
            if (view->isRemoved) {
                continue;
            }
            
            if (view->fullSize) {
                // GTK emits configure-event for window moves as well as resizes.
                // A pure move must not reconfigure a full-size WGPU child: doing
                // so with an empty mask removes native-layer holes even though
                // their view-local geometry has not changed.
                const GdkRectangle currentBounds = view->visualBounds;
                if (currentBounds.x == 0 &&
                    currentBounds.y == 0 &&
                    currentBounds.width == width &&
                    currentBounds.height == height) {
                    continue;
                }

                // Auto-resize webviews should fill the entire window
                // Preserve any native-layer masks while the SDK computes and
                // applies updated anchor geometry for the new client size.
                const std::string masks = view->maskJSON;
                view->resize(frame, masks.c_str());
            }
            // OOPIFs (fullSize=false) keep their positioning and don't auto-resize
            // The JavaScript ResizeObserver will handle repositioning them
        }
    }
};

// Window configure callback for move and resize events
static gboolean onWindowConfigure(GtkWidget* widget, GdkEventConfigure* event, gpointer user_data) {
    ContainerView* container = static_cast<ContainerView*>(user_data);
    if (container) {
        // Handle resize events
        container->resizeAutoSizingViews(event->width, event->height);
        
        // Handle move events - call the move callback with position
        if (container->moveCallback) {
            container->moveCallback(container->windowId, event->x, event->y);
        }
        
        // Handle resize events - call the resize callback with position and size
        if (container->resizeCallback) {
            container->resizeCallback(container->windowId, event->x, event->y, event->width, event->height);
        }
    }
    return FALSE; // Let other handlers process this event too
}

// Mouse move callback for debugging
static gboolean onMouseMove(GtkWidget* widget, GdkEventMotion* event, gpointer user_data) {

    return FALSE; // Let other handlers process this event too
}

// Window delete event callback - handles X button clicks
static gboolean onWindowDeleteEvent(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
    ContainerView* container = static_cast<ContainerView*>(user_data);
    if (container) {
        if (container->shouldCloseCallback) {
            container->shouldCloseCallback(container->windowId);
            return TRUE;
        }
        if (container->closeCallback) {
            container->closeCallback(container->windowId);
        }
    }
    
    // Hide the window immediately to give user feedback
    gtk_widget_hide(widget);
    
    // Schedule the window destruction on the next iteration of the main loop
    // This allows the callback to complete before destroying the window
    g_idle_add_full(G_PRIORITY_HIGH, [](gpointer data) -> gboolean {
        GtkWidget* window = GTK_WIDGET(data);
        gtk_widget_destroy(window);
        return G_SOURCE_REMOVE;
    }, widget, nullptr);
    
    // Return TRUE to prevent the default handler from running
    // We're handling the destruction ourselves
    return TRUE;
}

