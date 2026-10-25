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

