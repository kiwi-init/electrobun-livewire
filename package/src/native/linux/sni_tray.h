// StatusNotifierItem tray over GDBus: the org.kde.StatusNotifierItem and
// com.canonical.dbusmenu interfaces that Waybar, KDE, the GNOME AppIndicator
// extension and other SNI hosts read. Needs only gio, so it builds and tests
// without GTK. Everything here runs on the thread that owns the default
// GMainContext (the GTK main thread in Electrobun).
#pragma once

#include <gio/gio.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sni {

struct MenuItem {
    // "standard" or "separator".
    std::string type = "standard";
    std::string label;
    // Sent to the menu handler when the item is clicked.
    std::string action;
    bool enabled = true;
    bool visible = true;
    // Shown as a checkmark when true.
    bool checked = false;
    std::vector<MenuItem> children;
};

// ARGB32 pixels, one uint32 per pixel in host order (0xAARRGGBB). Converted to
// network byte order on the wire, as the spec asks.
struct Pixmap {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> argb;
};

// The org.kde.StatusNotifierWatcher session-bus name: present when an SNI
// host (a panel or bar) can show tray items.
bool watcherAvailable(GDBusConnection* connection);

// The watcher is present and reports IsStatusNotifierHostRegistered.
bool hostAvailable(GDBusConnection* connection);

// Calls onChange(available) whenever hostAvailable() changes: the watcher
// appears or leaves the bus, or a host registers or unregisters. Returns an id
// for unwatchHost.
guint watchHost(GDBusConnection* connection, std::function<void(bool)> onChange);
void unwatchHost(guint id);

class StatusNotifierItem {
public:
    using ActivateHandler = std::function<void()>;
    using MenuHandler = std::function<void(const std::string& action)>;

    // id is the SNI Id property (stable per app). Nothing is on the bus until
    // start().
    StatusNotifierItem(GDBusConnection* connection, std::string id, ActivateHandler onActivate, MenuHandler onMenu);
    ~StatusNotifierItem();

    StatusNotifierItem(const StatusNotifierItem&) = delete;
    StatusNotifierItem& operator=(const StatusNotifierItem&) = delete;

    // Exports the item and menu, owns org.kde.StatusNotifierItem-<pid>-<n>
    // and registers with the watcher, again whenever the watcher restarts.
    // False when the objects can't be exported.
    bool start();

    void setTitle(const std::string& title);
    void setIconName(const std::string& iconName);
    void setIconPixmap(Pixmap pixmap);
    // Active when visible, Passive when hidden.
    void setVisible(bool visible);
    void setMenu(std::vector<MenuItem> items);

    const std::string& busName() const { return busName_; }
    static constexpr const char* kItemPath = "/StatusNotifierItem";
    static constexpr const char* kMenuPath = "/MenuBar";

    struct State;

private:
    std::unique_ptr<State> state_;
    std::string busName_;
};

}  // namespace sni
