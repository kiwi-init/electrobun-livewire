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

// Menu item click callback
static void onMenuItemActivate(GtkMenuItem* menuItem, gpointer userData) {
    MenuItemData* itemData = static_cast<MenuItemData*>(userData);
    
    if (itemData && itemData->clickHandler) {
        try {
            itemData->clickHandler(itemData->menuId, itemData->action.c_str());
        } catch (...) {
            // Handle exception silently
        }
    }
}

// Create GTK menu from parsed menu items
GtkWidget* createMenuFromParsedItems(const std::vector<MenuJsonValue>& items, ZigStatusItemHandler clickHandler, uint32_t trayId) {
    GtkWidget* menu = gtk_menu_new();
    
    for (const auto& item : items) {
        if (item.type == "divider" || item.type == "separator") {
            // Create separator
            GtkWidget* separator = gtk_separator_menu_item_new();
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), separator);
        } else {
            // Skip hidden items entirely
            if (item.hidden) {
                continue;
            }
            
            // Create normal menu item
            std::string displayLabel = !item.label.empty() ? item.label : 
                                     (!item.role.empty() ? item.role : "Menu Item");
            
            GtkWidget* menuItem;
            if (item.checked) {
                menuItem = gtk_check_menu_item_new_with_label(displayLabel.c_str());
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menuItem), TRUE);
            } else {
                menuItem = gtk_menu_item_new_with_label(displayLabel.c_str());
            }
            
            gtk_widget_set_sensitive(menuItem, item.enabled);
            
            // Create menu item data for callback
            auto itemData = std::make_shared<MenuItemData>();
            itemData->menuId = trayId;
            itemData->action = !item.action.empty() ? item.action : 
                              (!item.role.empty() ? item.role : "");
            itemData->type = item.type;
            itemData->clickHandler = clickHandler;
            
            uint32_t currentMenuId = g_nextMenuId++;
            g_menuItems[currentMenuId] = itemData;
            
            // Connect click handler
            g_signal_connect(menuItem, "activate", G_CALLBACK(onMenuItemActivate), itemData.get());
            
            // Handle submenu
            if (!item.submenu.empty()) {
                GtkWidget* submenu = createMenuFromParsedItems(item.submenu, clickHandler, trayId);
                if (submenu) {
                    gtk_menu_item_set_submenu(GTK_MENU_ITEM(menuItem), submenu);
                }
            }
            
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuItem);
        }
    }
    
    return menu;
}

// Create GTK menu bar for application menus (File, Edit, etc.)
GtkWidget* createApplicationMenuBar(const std::vector<MenuJsonValue>& items, ZigStatusItemHandler clickHandler) {
    GtkWidget* menuBar = gtk_menu_bar_new();
    
    for (const auto& item : items) {
        if (item.type == "divider" || item.type == "separator") {
            // Skip separators at the top level of menu bar (they don't make sense there)
            continue;
        } else {
            // Skip hidden items entirely
            if (item.hidden) {
                continue;
            }
            
            // Create top-level menu item (like "File", "Edit", etc.)
            std::string displayLabel = !item.label.empty() ? item.label : 
                                     (!item.role.empty() ? item.role : "Menu");
            
            GtkWidget* menuItem = gtk_menu_item_new_with_label(displayLabel.c_str());
            
            // Set enabled/disabled state
            gtk_widget_set_sensitive(menuItem, item.enabled);
            
            // If this item has a submenu, create it
            if (!item.submenu.empty()) {
                GtkWidget* submenu = createMenuFromParsedItems(item.submenu, clickHandler, 0);
                gtk_menu_item_set_submenu(GTK_MENU_ITEM(menuItem), submenu);
            } else if (!item.action.empty()) {
                // If no submenu but has an action, create a menu item data and connect signal
                auto itemData = std::make_shared<MenuItemData>();
                itemData->menuId = g_nextMenuId++;
                itemData->action = item.action;
                itemData->type = item.type;
                itemData->clickHandler = clickHandler;
                
                g_menuItems[itemData->menuId] = itemData;
                
                g_signal_connect(menuItem, "activate", G_CALLBACK(onMenuItemActivate), itemData.get());
            }
            
            gtk_menu_shell_append(GTK_MENU_SHELL(menuBar), menuItem);
        }
    }
    
    gtk_widget_show_all(menuBar);
    return menuBar;
}

// Apply the stored application menu to a specific window
// NOTE: Application menus are not supported on Linux due to platform differences.
// On Linux, application menus overlay over content rather than shifting content down
// like on Windows/macOS, which interferes with OOPIF positioning and masking.
// Developers should implement menu UI directly in their HTML instead.
void applyApplicationMenuToWindow(GtkWidget* window) {
    printf("Application menus are not supported on Linux. Implement menu UI in your webview HTML instead.\n");
    fflush(stdout);
}

// Apply the stored application menu to a specific X11 window
// NOTE: Application menus are not supported on Linux due to platform differences.
// X11 application menus would require complex webview positioning adjustments
// that could interfere with OOPIF layering and maskJSON cutout mechanisms.
// Developers should implement menu UI directly in their HTML instead.
void applyApplicationMenuToX11Window(X11Window* x11win) {
    printf("Application menus are not supported on Linux. Implement menu UI in your webview HTML instead.\n");
    fflush(stdout);
}

