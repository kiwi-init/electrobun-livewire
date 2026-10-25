// Global state
static std::map<uint32_t, std::shared_ptr<ContainerView>> g_containers;
static std::mutex g_containersMutex;
static std::map<uint32_t, std::shared_ptr<TrayItem>> g_trays;
static std::mutex g_traysMutex;
static bool g_gtkInitialized = false;
static std::mutex g_gtkInitMutex;
static std::condition_variable g_gtkInitCondition;

// Window dragging state
static GtkWidget* g_draggedWindow = nullptr;
static gint g_dragStartX = 0;
static gint g_dragStartY = 0;
static guint g_motionHandlerId = 0;
static guint g_buttonReleaseHandlerId = 0;

// X11 window management
static std::map<uint32_t, std::shared_ptr<X11Window>> g_x11_windows;
static std::map<Window, uint32_t> g_x11_window_to_id;
static std::map<Window, uint32_t> g_x11_child_window_to_parent_id;
static std::mutex g_x11WindowsMutex;

static void removeWGPUViewsForParentWindow(Window parent_window) {
    std::vector<std::shared_ptr<WGPUViewImpl>> views_to_remove;
    {
        std::lock_guard<std::mutex> lock(g_wgpuViewMapMutex);
        for (auto it = g_wgpuViewMap.begin(); it != g_wgpuViewMap.end();) {
            auto wgpu_view = std::dynamic_pointer_cast<WGPUViewImpl>(it->second);
            if (wgpu_view && wgpu_view->parentXWindow == parent_window &&
                !wgpu_view->isRemoved) {
                views_to_remove.push_back(wgpu_view);
                it = g_wgpuViewMap.erase(it);
            } else {
                ++it;
            }
        }
    }

    // The resize queue and X11 child routing table both store non-owning view
    // state, so detach them before releasing the registry's shared_ptr.
    for (const auto& view : views_to_remove) {
        g_pendingResizeQueue.remove(view.get());

        if (view->xWindow) {
            std::lock_guard<std::mutex> lock(g_x11WindowsMutex);
            g_x11_child_window_to_parent_id.erase(view->xWindow);
            if (view->inputXWindow) {
                g_x11_child_window_to_parent_id.erase(view->inputXWindow);
            }
        }

        view->remove();
    }
}

static uint32_t modifiersFromX11State(unsigned int state) {
    uint32_t modifiers = 0;
    if (state & ShiftMask) modifiers |= 1u << 0;
    if (state & ControlMask) modifiers |= 1u << 1;
    if (state & Mod1Mask) modifiers |= 1u << 2;
    if (state & Mod4Mask) modifiers |= 1u << 3;
    return modifiers;
}

static uint64_t mouseButtonsFromGdkModifiers(GdkModifierType modifiers) {
    uint64_t buttons = 0;
    if (modifiers & GDK_BUTTON1_MASK) buttons |= 1ull << 0;
    if (modifiers & GDK_BUTTON3_MASK) buttons |= 1ull << 1;
    if (modifiers & GDK_BUTTON2_MASK) buttons |= 1ull << 2;
    return buttons;
}

static void focusX11Window(Display* display, Window window) {
    if (!display || !window) return;
    XRaiseWindow(display, window);
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    XFlush(display);
}

// Helper function to get ContainerView overlay for a window
GtkWidget* getContainerViewOverlay(GtkWidget* window) {
    std::lock_guard<std::mutex> lock(g_containersMutex);
    for (auto& [id, container] : g_containers) {
        if (container->window == window) {
            return container->overlay;
        }
    }
    return nullptr;
}

static std::shared_ptr<ContainerView> getContainerViewForWindow(GtkWidget* window) {
    std::lock_guard<std::mutex> lock(g_containersMutex);
    for (auto& [id, container] : g_containers) {
        if (container->window == window) {
            return container;
        }
    }
    return nullptr;
}

