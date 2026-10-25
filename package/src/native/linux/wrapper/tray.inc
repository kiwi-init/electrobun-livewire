// Tray: a StatusNotifierItem on the session bus (sni_tray.cpp), which Waybar,
// KDE, the GNOME AppIndicator extension and other SNI hosts show, with left
// click as tray-clicked and right click as the menu. AppIndicator is the
// fallback when no StatusNotifierWatcher is on the bus (GNOME without the
// extension, X11 panels with only an XEmbed tray).
#ifndef NO_APPINDICATOR
// AppIndicator is loaded when the first tray is made, not linked: many
// distributions (Fedora, Arch) don't install it by default, and a missing
// tray library shouldn't keep the whole app from starting.
struct AppIndicatorApi {
    decltype(&::app_indicator_new) indicatorNew;
    decltype(&::app_indicator_set_status) setStatus;
    decltype(&::app_indicator_set_title) setTitle;
    decltype(&::app_indicator_set_icon_full) setIconFull;
    decltype(&::app_indicator_set_menu) setMenu;
};

static const AppIndicatorApi* appIndicatorApi() {
    static const AppIndicatorApi* api = []() -> const AppIndicatorApi* {
        void* lib = dlopen("libayatana-appindicator3.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            fprintf(stderr, "Tray: no tray icon, libayatana-appindicator3.so.1 couldn't be loaded: %s\n", dlerror());
            return nullptr;
        }
        static AppIndicatorApi loaded = {
            reinterpret_cast<decltype(&::app_indicator_new)>(dlsym(lib, "app_indicator_new")),
            reinterpret_cast<decltype(&::app_indicator_set_status)>(dlsym(lib, "app_indicator_set_status")),
            reinterpret_cast<decltype(&::app_indicator_set_title)>(dlsym(lib, "app_indicator_set_title")),
            reinterpret_cast<decltype(&::app_indicator_set_icon_full)>(dlsym(lib, "app_indicator_set_icon_full")),
            reinterpret_cast<decltype(&::app_indicator_set_menu)>(dlsym(lib, "app_indicator_set_menu")),
        };
        if (!loaded.indicatorNew || !loaded.setStatus || !loaded.setTitle || !loaded.setIconFull || !loaded.setMenu) {
            fprintf(stderr, "Tray: no tray icon, libayatana-appindicator3.so.1 lacks the functions it needs\n");
            return nullptr;
        }
        return &loaded;
    }();
    return api;
}

#define app_indicator_new appIndicatorApi()->indicatorNew
#define app_indicator_set_status appIndicatorApi()->setStatus
#define app_indicator_set_title appIndicatorApi()->setTitle
#define app_indicator_set_icon_full appIndicatorApi()->setIconFull
#define app_indicator_set_menu appIndicatorApi()->setMenu
#endif // NO_APPINDICATOR

static GDBusConnection* traySessionBus() {
    static GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    return bus;
}

// SNI hosts draw IconPixmap at panel size; 64 px keeps it sharp on HiDPI.
static bool trayPixmapFromFile(const std::string& path, sni::Pixmap& out) {
    GError* error = nullptr;
    GdkPixbuf* loaded = gdk_pixbuf_new_from_file_at_size(path.c_str(), 64, 64, &error);
    if (!loaded) {
        fprintf(stderr, "Tray: can't load icon %s: %s\n", path.c_str(), error ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }
    GdkPixbuf* pixbuf = gdk_pixbuf_add_alpha(loaded, FALSE, 0, 0, 0);
    g_object_unref(loaded);
    const int width = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);
    const int stride = gdk_pixbuf_get_rowstride(pixbuf);
    const guchar* pixels = gdk_pixbuf_read_pixels(pixbuf);
    out.width = width;
    out.height = height;
    out.argb.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        const guchar* row = pixels + static_cast<size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            const guchar* px = row + x * 4;
            out.argb[static_cast<size_t>(y) * width + x] =
                (uint32_t(px[3]) << 24) | (uint32_t(px[0]) << 16) | (uint32_t(px[1]) << 8) | uint32_t(px[2]);
        }
    }
    g_object_unref(pixbuf);
    return true;
}

// The same items the GTK menu shows: hidden items are left out, labels fall
// back to the role, and a click sends the action (or the role).
static std::vector<sni::MenuItem> trayMenuItems(const std::vector<MenuJsonValue>& items) {
    std::vector<sni::MenuItem> result;
    for (const auto& item : items) {
        sni::MenuItem entry;
        if (item.type == "divider" || item.type == "separator") {
            entry.type = "separator";
            result.push_back(std::move(entry));
            continue;
        }
        if (item.hidden) {
            continue;
        }
        entry.label = !item.label.empty() ? item.label : (!item.role.empty() ? item.role : "Menu Item");
        entry.action = !item.action.empty() ? item.action : item.role;
        entry.enabled = item.enabled;
        entry.checked = item.checked;
        entry.children = trayMenuItems(item.submenu);
        result.push_back(std::move(entry));
    }
    return result;
}

class TrayItem {
public:
    uint32_t trayId;
    ZigStatusItemHandler clickHandler;
    std::string title;
    std::string imagePath;
    std::unique_ptr<sni::StatusNotifierItem> sniItem;
#ifndef NO_APPINDICATOR
    AppIndicator* indicator = nullptr;
    GtkWidget* menu = nullptr;
#endif

    TrayItem(uint32_t id, const char* title, const char* pathToImage, bool isTemplate, ZigStatusItemHandler handler)
        : trayId(id), clickHandler(handler),
          title(title ? title : ""), imagePath(pathToImage ? pathToImage : "") {
        GDBusConnection* bus = traySessionBus();
#ifndef NO_APPINDICATOR
        // With a watcher on the bus, or no AppIndicator to fall back on, use
        // SNI: it registers whenever a watcher shows up.
        const bool useSni = bus && (sni::watcherAvailable(bus) || !appIndicatorApi());
#else
        const bool useSni = bus != nullptr;
#endif
        if (useSni) {
            createStatusNotifierItem(bus);
        }
#ifndef NO_APPINDICATOR
        else if (appIndicatorApi()) {
            createIndicator();
        }
#endif
    }

    ~TrayItem() {
        sniItem.reset();
#ifndef NO_APPINDICATOR
        if (indicator) {
            app_indicator_set_status(indicator, APP_INDICATOR_STATUS_PASSIVE);
        }
        // The indicator may own the menu's last reference, so destroy the menu
        // before releasing the indicator.
        if (menu) {
            gtk_widget_destroy(menu);
        }
        if (indicator) {
            g_object_unref(indicator);
        }
#endif
    }

    bool valid() const {
#ifndef NO_APPINDICATOR
        if (indicator) return true;
#endif
        return sniItem != nullptr;
    }

    void setTitle(const char* newTitle) {
        title = newTitle ? newTitle : "";
        if (sniItem) {
            sniItem->setTitle(title);
        }
#ifndef NO_APPINDICATOR
        if (indicator) {
            app_indicator_set_title(indicator, title.c_str());
        }
#endif
    }

    void setImage(const char* newImage) {
        imagePath = newImage ? newImage : "";
        if (sniItem) {
            applySniIcon();
        }
#ifndef NO_APPINDICATOR
        if (indicator && !imagePath.empty()) {
            app_indicator_set_icon_full(indicator, imagePath.c_str(), "Electrobun Tray Icon");
        }
#endif
    }

    void setMenu(const char* jsonString) {
        std::vector<MenuJsonValue> menuItems;
        bool parsed = false;
        if (jsonString && strlen(jsonString) > 0) {
            try {
                menuItems = parseMenuJson(std::string(jsonString));
                parsed = true;
            } catch (const std::exception&) {
            }
        }
        if (sniItem) {
            sniItem->setMenu(parsed ? trayMenuItems(menuItems) : std::vector<sni::MenuItem>{});
        }
#ifndef NO_APPINDICATOR
        if (indicator) {
            if (menu) {
                gtk_widget_destroy(menu);
                menu = nullptr;
            }
            if (!parsed) {
                createDefaultMenu();
                return;
            }
            menu = createMenuFromParsedItems(menuItems, this->clickHandler, trayId);
            if (menu) {
                gtk_widget_show_all(menu);
                app_indicator_set_menu(indicator, GTK_MENU(menu));
            }
        }
#endif
    }

private:
    void createStatusNotifierItem(GDBusConnection* bus) {
        const uint32_t id = trayId;
        const ZigStatusItemHandler handler = clickHandler;
        auto item = std::make_unique<sni::StatusNotifierItem>(
            bus, g_electrobunWindowClass,
            [id, handler]() {
                if (handler) handler(id, "");
            },
            [id, handler](const std::string& action) {
                if (handler) handler(id, action.c_str());
            });
        item->setTitle(title);
        sniItem = std::move(item);
        applySniIcon();
        if (!sniItem->start()) {
            sniItem.reset();
        }
    }

    // A file is sent as pixels; anything else is taken as an icon theme name.
    void applySniIcon() {
        sni::Pixmap pixmap;
        if (!imagePath.empty() && g_file_test(imagePath.c_str(), G_FILE_TEST_IS_REGULAR) &&
            trayPixmapFromFile(imagePath, pixmap)) {
            sniItem->setIconPixmap(std::move(pixmap));
        } else {
            sniItem->setIconName(!imagePath.empty() && imagePath.find('/') == std::string::npos
                                     ? imagePath
                                     : "application-x-executable");
        }
    }

#ifndef NO_APPINDICATOR
    void createIndicator() {
        // Hide/show recreates the native tray while the previous indicator
        // may still be registered on D-Bus, so the ID must be unique per
        // instance, not per tray: AppIndicator derives its object path from it.
        static std::atomic<uint64_t> nextIndicatorInstance{0};
        std::string indicatorId = "electrobun-tray-" + std::to_string(trayId) + "-" +
            std::to_string(nextIndicatorInstance.fetch_add(1));

        indicator = app_indicator_new(indicatorId.c_str(),
                                      !imagePath.empty() ? imagePath.c_str() : "application-default-icon",
                                      APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
        if (indicator) {
            app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
            if (!title.empty()) {
                app_indicator_set_title(indicator, title.c_str());
            }
            // Create default menu (required for AppIndicator)
            createDefaultMenu();
        }
    }

    void createDefaultMenu() {
        menu = gtk_menu_new();

        GtkWidget* defaultItem = gtk_menu_item_new_with_label("Electrobun App");
        gtk_widget_set_sensitive(defaultItem, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), defaultItem);

        gtk_widget_show_all(menu);

        if (indicator) {
            app_indicator_set_menu(indicator, GTK_MENU(menu));
        }
    }
#endif
};

