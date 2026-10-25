#include "sni_tray.h"

#include <unistd.h>

#include <atomic>
#include <map>
#include <utility>

namespace sni {
namespace {

constexpr const char* kWatcherName = "org.kde.StatusNotifierWatcher";
constexpr const char* kWatcherPath = "/StatusNotifierWatcher";
constexpr const char* kWatcherInterface = "org.kde.StatusNotifierWatcher";
constexpr const char* kItemInterface = "org.kde.StatusNotifierItem";
constexpr const char* kMenuInterface = "com.canonical.dbusmenu";

constexpr const char* kItemXml = R"XML(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="i" access="read"/>
    <property name="IconThemePath" type="s" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionMovieName" type="s" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <method name="ContextMenu"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Activate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Scroll"><arg name="delta" type="i" direction="in"/><arg name="orientation" type="s" direction="in"/></method>
    <signal name="NewTitle"/>
    <signal name="NewIcon"/>
    <signal name="NewAttentionIcon"/>
    <signal name="NewOverlayIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus"><arg name="status" type="s"/></signal>
  </interface>
</node>
)XML";

constexpr const char* kMenuXml = R"XML(
<node>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <method name="GetLayout">
      <arg name="parentId" type="i" direction="in"/>
      <arg name="recursionDepth" type="i" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="revision" type="u" direction="out"/>
      <arg name="layout" type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="properties" type="a(ia{sv})" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg name="id" type="i" direction="in"/>
      <arg name="name" type="s" direction="in"/>
      <arg name="value" type="v" direction="out"/>
    </method>
    <method name="Event">
      <arg name="id" type="i" direction="in"/>
      <arg name="eventId" type="s" direction="in"/>
      <arg name="data" type="v" direction="in"/>
      <arg name="timestamp" type="u" direction="in"/>
    </method>
    <method name="EventGroup">
      <arg name="events" type="a(isvu)" direction="in"/>
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <method name="AboutToShow">
      <arg name="id" type="i" direction="in"/>
      <arg name="needUpdate" type="b" direction="out"/>
    </method>
    <method name="AboutToShowGroup">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="updatesNeeded" type="ai" direction="out"/>
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <signal name="ItemsPropertiesUpdated">
      <arg name="updatedProps" type="a(ia{sv})"/>
      <arg name="removedProps" type="a(ias)"/>
    </signal>
    <signal name="LayoutUpdated">
      <arg name="revision" type="u"/>
      <arg name="parent" type="i"/>
    </signal>
    <signal name="ItemActivationRequested">
      <arg name="id" type="i"/>
      <arg name="timestamp" type="u"/>
    </signal>
  </interface>
</node>
)XML";

GDBusNodeInfo* nodeInfo(const char* xml) {
    GError* error = nullptr;
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(xml, &error);
    if (!info) {
        g_warning("sni: bad introspection XML: %s", error->message);
        g_error_free(error);
    }
    return info;
}

GDBusNodeInfo* itemNodeInfo() {
    static GDBusNodeInfo* info = nodeInfo(kItemXml);
    return info;
}

GDBusNodeInfo* menuNodeInfo() {
    static GDBusNodeInfo* info = nodeInfo(kMenuXml);
    return info;
}

// The menu flattened by id. Id 0 is the root; the others are numbered in
// document order, so ids stay small and a new menu replaces them all.
struct MenuNode {
    MenuItem item;
    std::vector<int32_t> children;
};

bool wanted(GVariant* propertyNames, const char* name) {
    if (!propertyNames || g_variant_n_children(propertyNames) == 0) {
        return true;
    }
    GVariantIter iter;
    const char* candidate = nullptr;
    g_variant_iter_init(&iter, propertyNames);
    while (g_variant_iter_next(&iter, "&s", &candidate)) {
        if (g_strcmp0(candidate, name) == 0) {
            return true;
        }
    }
    return false;
}

GVariant* pixmapsVariant(const Pixmap& pixmap) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
    if (pixmap.width > 0 && pixmap.height > 0 &&
        pixmap.argb.size() == static_cast<size_t>(pixmap.width) * pixmap.height) {
        std::vector<uint32_t> network(pixmap.argb.size());
        for (size_t i = 0; i < pixmap.argb.size(); ++i) {
            network[i] = GUINT32_TO_BE(pixmap.argb[i]);
        }
        GVariant* bytes = g_variant_new_fixed_array(
            G_VARIANT_TYPE_BYTE, network.data(), network.size() * sizeof(uint32_t), 1);
        g_variant_builder_add(&builder, "(ii@ay)", pixmap.width, pixmap.height, bytes);
    }
    return g_variant_builder_end(&builder);
}

}  // namespace

struct StatusNotifierItem::State {
    GDBusConnection* connection = nullptr;
    std::string id;
    std::string busName;
    ActivateHandler onActivate;
    MenuHandler onMenu;

    std::string title;
    std::string iconName;
    Pixmap pixmap;
    bool visible = true;

    std::vector<MenuNode> menu = std::vector<MenuNode>(1);
    uint32_t revision = 1;

    guint itemRegistration = 0;
    guint menuRegistration = 0;
    guint nameOwner = 0;
    guint watcherWatch = 0;

    const char* status() const { return visible ? "Active" : "Passive"; }

    void emitItem(const char* signal, GVariant* args = nullptr) {
        g_dbus_connection_emit_signal(connection, nullptr, kItemPath, kItemInterface, signal, args, nullptr);
    }

    void registerWithWatcher() {
        // Fire and forget: the watcher answers asynchronously and this state
        // may be gone by then.
        g_dbus_connection_call(connection, kWatcherName, kWatcherPath, kWatcherInterface,
                               "RegisterStatusNotifierItem", g_variant_new("(s)", busName.c_str()),
                               nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr, nullptr, nullptr);
    }

    void addMenuItems(int32_t parent, const std::vector<MenuItem>& items) {
        for (const auto& item : items) {
            int32_t id = static_cast<int32_t>(menu.size());
            menu.push_back(MenuNode{item, {}});
            menu[id].item.children.clear();
            menu[parent].children.push_back(id);
            addMenuItems(id, item.children);
        }
    }

    GVariant* menuProperties(int32_t id, GVariant* names) const {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        if (id == 0) {
            if (wanted(names, "children-display")) {
                g_variant_builder_add(&builder, "{sv}", "children-display", g_variant_new_string("submenu"));
            }
            return g_variant_builder_end(&builder);
        }
        const MenuNode& node = menu[id];
        const MenuItem& item = node.item;
        if (item.type == "separator") {
            if (wanted(names, "type")) {
                g_variant_builder_add(&builder, "{sv}", "type", g_variant_new_string("separator"));
            }
        } else if (wanted(names, "label")) {
            g_variant_builder_add(&builder, "{sv}", "label", g_variant_new_string(item.label.c_str()));
        }
        if (!item.enabled && wanted(names, "enabled")) {
            g_variant_builder_add(&builder, "{sv}", "enabled", g_variant_new_boolean(FALSE));
        }
        if (!item.visible && wanted(names, "visible")) {
            g_variant_builder_add(&builder, "{sv}", "visible", g_variant_new_boolean(FALSE));
        }
        if (item.checked) {
            if (wanted(names, "toggle-type")) {
                g_variant_builder_add(&builder, "{sv}", "toggle-type", g_variant_new_string("checkmark"));
            }
            if (wanted(names, "toggle-state")) {
                g_variant_builder_add(&builder, "{sv}", "toggle-state", g_variant_new_int32(1));
            }
        }
        if (!node.children.empty() && wanted(names, "children-display")) {
            g_variant_builder_add(&builder, "{sv}", "children-display", g_variant_new_string("submenu"));
        }
        return g_variant_builder_end(&builder);
    }

    GVariant* layout(int32_t id, int32_t depth, GVariant* names) const {
        GVariantBuilder children;
        g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
        if (depth != 0) {
            for (int32_t child : menu[id].children) {
                g_variant_builder_add(&children, "v", layout(child, depth < 0 ? -1 : depth - 1, names));
            }
        }
        return g_variant_new("(i@a{sv}av)", id, menuProperties(id, names), &children);
    }

    bool validMenuId(int32_t id) const { return id >= 0 && static_cast<size_t>(id) < menu.size(); }

    void menuEvent(int32_t id, const char* eventId) {
        if (id <= 0 || !validMenuId(id) || g_strcmp0(eventId, "clicked") != 0) {
            return;
        }
        const MenuItem& item = menu[id].item;
        if (item.type == "separator" || !item.enabled || !menu[id].children.empty()) {
            return;
        }
        if (onMenu) {
            onMenu(item.action);
        }
    }
};

namespace {

using State = StatusNotifierItem::State;

void itemMethod(GDBusConnection*, const char*, const char*, const char*, const char* method,
                GVariant*, GDBusMethodInvocation* invocation, gpointer data) {
    auto* state = static_cast<State*>(data);
    if (g_strcmp0(method, "Activate") == 0 || g_strcmp0(method, "SecondaryActivate") == 0) {
        g_dbus_method_invocation_return_value(invocation, nullptr);
        if (state->onActivate) {
            state->onActivate();
        }
        return;
    }
    if (g_strcmp0(method, "ContextMenu") == 0 || g_strcmp0(method, "Scroll") == 0) {
        // Hosts show the Menu object themselves; nothing to do here.
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                          "No method %s", method);
}

GVariant* itemProperty(GDBusConnection*, const char*, const char*, const char*, const char* property,
                       GError** error, gpointer data) {
    auto* state = static_cast<State*>(data);
    if (g_strcmp0(property, "Category") == 0) return g_variant_new_string("ApplicationStatus");
    if (g_strcmp0(property, "Id") == 0) return g_variant_new_string(state->id.c_str());
    if (g_strcmp0(property, "Title") == 0) return g_variant_new_string(state->title.c_str());
    if (g_strcmp0(property, "Status") == 0) return g_variant_new_string(state->status());
    if (g_strcmp0(property, "WindowId") == 0) return g_variant_new_int32(0);
    if (g_strcmp0(property, "IconThemePath") == 0) return g_variant_new_string("");
    if (g_strcmp0(property, "IconName") == 0) return g_variant_new_string(state->iconName.c_str());
    if (g_strcmp0(property, "IconPixmap") == 0) return pixmapsVariant(state->pixmap);
    if (g_strcmp0(property, "OverlayIconName") == 0 || g_strcmp0(property, "AttentionIconName") == 0 ||
        g_strcmp0(property, "AttentionMovieName") == 0) {
        return g_variant_new_string("");
    }
    if (g_strcmp0(property, "OverlayIconPixmap") == 0 || g_strcmp0(property, "AttentionIconPixmap") == 0) {
        return pixmapsVariant(Pixmap{});
    }
    if (g_strcmp0(property, "ToolTip") == 0) {
        return g_variant_new("(s@a(iiay)ss)", state->iconName.c_str(), pixmapsVariant(Pixmap{}),
                             state->title.c_str(), "");
    }
    // Activate on left click, the Menu object on right click.
    if (g_strcmp0(property, "ItemIsMenu") == 0) return g_variant_new_boolean(FALSE);
    if (g_strcmp0(property, "Menu") == 0) return g_variant_new_object_path(StatusNotifierItem::kMenuPath);
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "No property %s", property);
    return nullptr;
}

void menuMethod(GDBusConnection*, const char*, const char*, const char*, const char* method,
                GVariant* params, GDBusMethodInvocation* invocation, gpointer data) {
    auto* state = static_cast<State*>(data);
    if (g_strcmp0(method, "GetLayout") == 0) {
        gint32 parent = 0;
        gint32 depth = -1;
        GVariant* names = nullptr;
        g_variant_get(params, "(ii@as)", &parent, &depth, &names);
        if (!state->validMenuId(parent)) {
            g_variant_unref(names);
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                  "No menu item %d", parent);
            return;
        }
        GVariant* layout = state->layout(parent, depth, names);
        g_variant_unref(names);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@(ia{sv}av))", state->revision, layout));
        return;
    }
    if (g_strcmp0(method, "GetGroupProperties") == 0) {
        GVariant* ids = nullptr;
        GVariant* names = nullptr;
        g_variant_get(params, "(@ai@as)", &ids, &names);
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ia{sv})"));
        GVariantIter iter;
        gint32 id = 0;
        g_variant_iter_init(&iter, ids);
        while (g_variant_iter_next(&iter, "i", &id)) {
            if (state->validMenuId(id)) {
                g_variant_builder_add(&builder, "(i@a{sv})", id, state->menuProperties(id, names));
            }
        }
        g_variant_unref(ids);
        g_variant_unref(names);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&builder)));
        return;
    }
    if (g_strcmp0(method, "GetProperty") == 0) {
        gint32 id = 0;
        const char* name = nullptr;
        g_variant_get(params, "(i&s)", &id, &name);
        GVariant* value = nullptr;
        if (state->validMenuId(id)) {
            GVariant* names = g_variant_ref_sink(g_variant_new_strv(&name, 1));
            GVariant* properties = g_variant_ref_sink(state->menuProperties(id, names));
            value = g_variant_lookup_value(properties, name, nullptr);
            g_variant_unref(properties);
            g_variant_unref(names);
        }
        if (!value) {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                  "No property %s on item %d", name, id);
            return;
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
        g_variant_unref(value);
        return;
    }
    if (g_strcmp0(method, "Event") == 0) {
        gint32 id = 0;
        const char* eventId = nullptr;
        g_variant_get(params, "(i&svu)", &id, &eventId, nullptr, nullptr);
        std::string event = eventId;
        g_dbus_method_invocation_return_value(invocation, nullptr);
        state->menuEvent(id, event.c_str());
        return;
    }
    if (g_strcmp0(method, "EventGroup") == 0) {
        GVariantIter* events = nullptr;
        g_variant_get(params, "(a(isvu))", &events);
        GVariantBuilder errors;
        g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
        std::vector<std::pair<int32_t, std::string>> valid;
        gint32 id = 0;
        const char* eventId = nullptr;
        GVariant* eventData = nullptr;
        guint32 timestamp = 0;
        while (g_variant_iter_next(events, "(i&svu)", &id, &eventId, &eventData, &timestamp)) {
            if (state->validMenuId(id)) {
                valid.emplace_back(id, eventId);
            } else {
                g_variant_builder_add(&errors, "i", id);
            }
            g_variant_unref(eventData);
        }
        g_variant_iter_free(events);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(ai)", &errors));
        for (const auto& [eventItem, name] : valid) {
            state->menuEvent(eventItem, name.c_str());
        }
        return;
    }
    if (g_strcmp0(method, "AboutToShow") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
        return;
    }
    if (g_strcmp0(method, "AboutToShowGroup") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ai@ai)",
            g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0),
            g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
        return;
    }
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                          "No method %s", method);
}

GVariant* menuProperty(GDBusConnection*, const char*, const char*, const char*, const char* property,
                       GError** error, gpointer) {
    if (g_strcmp0(property, "Version") == 0) return g_variant_new_uint32(3);
    if (g_strcmp0(property, "TextDirection") == 0) return g_variant_new_string("ltr");
    if (g_strcmp0(property, "Status") == 0) return g_variant_new_string("normal");
    if (g_strcmp0(property, "IconThemePath") == 0) return g_variant_new_strv(nullptr, 0);
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "No property %s", property);
    return nullptr;
}

const GDBusInterfaceVTable kItemVTable = {itemMethod, itemProperty, nullptr, {nullptr}};
const GDBusInterfaceVTable kMenuVTable = {menuMethod, menuProperty, nullptr, {nullptr}};

void onWatcherAppeared(GDBusConnection*, const char*, const char*, gpointer data) {
    static_cast<State*>(data)->registerWithWatcher();
}

}  // namespace

StatusNotifierItem::StatusNotifierItem(GDBusConnection* connection, std::string id, ActivateHandler onActivate,
                                       MenuHandler onMenu)
    : state_(std::make_unique<State>()) {
    static std::atomic<uint32_t> nextInstance{1};
    state_->connection = G_DBUS_CONNECTION(g_object_ref(connection));
    state_->id = std::move(id);
    state_->onActivate = std::move(onActivate);
    state_->onMenu = std::move(onMenu);
    // One name per item: hide/show and several trays never collide.
    busName_ = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-" +
               std::to_string(nextInstance.fetch_add(1));
    state_->busName = busName_;
}

StatusNotifierItem::~StatusNotifierItem() {
    State* state = state_.get();
    if (state->watcherWatch) g_bus_unwatch_name(state->watcherWatch);
    if (state->nameOwner) g_bus_unown_name(state->nameOwner);
    if (state->itemRegistration) g_dbus_connection_unregister_object(state->connection, state->itemRegistration);
    if (state->menuRegistration) g_dbus_connection_unregister_object(state->connection, state->menuRegistration);
    g_object_unref(state->connection);
}

bool StatusNotifierItem::start() {
    State* state = state_.get();
    if (!itemNodeInfo() || !menuNodeInfo()) {
        return false;
    }
    GError* error = nullptr;
    state->itemRegistration = g_dbus_connection_register_object(
        state->connection, kItemPath, itemNodeInfo()->interfaces[0], &kItemVTable, state, nullptr, &error);
    if (!state->itemRegistration) {
        g_warning("sni: can't export %s: %s", kItemPath, error->message);
        g_clear_error(&error);
        return false;
    }
    state->menuRegistration = g_dbus_connection_register_object(
        state->connection, kMenuPath, menuNodeInfo()->interfaces[0], &kMenuVTable, state, nullptr, &error);
    if (!state->menuRegistration) {
        g_warning("sni: can't export %s: %s", kMenuPath, error->message);
        g_clear_error(&error);
        return false;
    }
    state->nameOwner = g_bus_own_name_on_connection(state->connection, busName_.c_str(),
                                                    G_BUS_NAME_OWNER_FLAGS_NONE, nullptr, nullptr, nullptr, nullptr);
    // Registers now if the watcher is up, and again each time it restarts
    // (a bar reload drops every item).
    state->watcherWatch = g_bus_watch_name_on_connection(state->connection, kWatcherName,
                                                         G_BUS_NAME_WATCHER_FLAGS_NONE, onWatcherAppeared,
                                                         nullptr, state, nullptr);
    return true;
}

void StatusNotifierItem::setTitle(const std::string& title) {
    if (state_->title == title) return;
    state_->title = title;
    state_->emitItem("NewTitle");
    state_->emitItem("NewToolTip");
}

void StatusNotifierItem::setIconName(const std::string& iconName) {
    state_->iconName = iconName;
    state_->pixmap = Pixmap{};
    state_->emitItem("NewIcon");
}

void StatusNotifierItem::setIconPixmap(Pixmap pixmap) {
    state_->iconName.clear();
    state_->pixmap = std::move(pixmap);
    state_->emitItem("NewIcon");
}

void StatusNotifierItem::setVisible(bool visible) {
    if (state_->visible == visible) return;
    state_->visible = visible;
    state_->emitItem("NewStatus", g_variant_new("(s)", state_->status()));
}

void StatusNotifierItem::setMenu(std::vector<MenuItem> items) {
    State* state = state_.get();
    state->menu.assign(1, MenuNode{});
    state->addMenuItems(0, items);
    state->revision++;
    g_dbus_connection_emit_signal(state->connection, nullptr, kMenuPath, kMenuInterface, "LayoutUpdated",
                                  g_variant_new("(ui)", state->revision, 0), nullptr);
}

bool watcherAvailable(GDBusConnection* connection) {
    GVariant* reply = g_dbus_connection_call_sync(
        connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new("(s)", kWatcherName), G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr);
    if (!reply) return false;
    gboolean hasOwner = FALSE;
    g_variant_get(reply, "(b)", &hasOwner);
    g_variant_unref(reply);
    return hasOwner;
}

bool hostAvailable(GDBusConnection* connection) {
    if (!watcherAvailable(connection)) return false;
    GVariant* reply = g_dbus_connection_call_sync(
        connection, kWatcherName, kWatcherPath, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", kWatcherInterface, "IsStatusNotifierHostRegistered"), G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, nullptr, nullptr);
    if (!reply) return false;
    GVariant* value = nullptr;
    g_variant_get(reply, "(v)", &value);
    bool registered = g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(value);
    g_variant_unref(value);
    g_variant_unref(reply);
    return registered;
}

namespace {

struct HostWatch {
    GDBusConnection* connection = nullptr;
    std::function<void(bool)> onChange;
    bool available = false;
    guint nameWatch = 0;
    guint registered = 0;
    guint unregistered = 0;
};

std::map<guint, std::unique_ptr<HostWatch>>& hostWatches() {
    static std::map<guint, std::unique_ptr<HostWatch>> watches;
    return watches;
}

void recheckHost(HostWatch* watch) {
    bool available = hostAvailable(watch->connection);
    if (available != watch->available) {
        watch->available = available;
        watch->onChange(available);
    }
}

void onHostNameChanged(GDBusConnection*, const char*, const char*, gpointer data) {
    recheckHost(static_cast<HostWatch*>(data));
}

void onHostNameVanished(GDBusConnection*, const char*, gpointer data) {
    recheckHost(static_cast<HostWatch*>(data));
}

void onHostSignal(GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*, gpointer data) {
    recheckHost(static_cast<HostWatch*>(data));
}

}  // namespace

guint watchHost(GDBusConnection* connection, std::function<void(bool)> onChange) {
    static guint nextId = 1;
    auto watch = std::make_unique<HostWatch>();
    HostWatch* raw = watch.get();
    raw->connection = G_DBUS_CONNECTION(g_object_ref(connection));
    raw->onChange = std::move(onChange);
    raw->available = hostAvailable(connection);
    raw->nameWatch = g_bus_watch_name_on_connection(connection, kWatcherName, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                    onHostNameChanged, onHostNameVanished, raw, nullptr);
    raw->registered = g_dbus_connection_signal_subscribe(connection, kWatcherName, kWatcherInterface,
                                                         "StatusNotifierHostRegistered", kWatcherPath, nullptr,
                                                         G_DBUS_SIGNAL_FLAGS_NONE, onHostSignal, raw, nullptr);
    raw->unregistered = g_dbus_connection_signal_subscribe(connection, kWatcherName, kWatcherInterface,
                                                           "StatusNotifierHostUnregistered", kWatcherPath, nullptr,
                                                           G_DBUS_SIGNAL_FLAGS_NONE, onHostSignal, raw, nullptr);
    guint id = nextId++;
    hostWatches()[id] = std::move(watch);
    return id;
}

void unwatchHost(guint id) {
    auto it = hostWatches().find(id);
    if (it == hostWatches().end()) return;
    HostWatch* watch = it->second.get();
    g_bus_unwatch_name(watch->nameWatch);
    g_dbus_connection_signal_unsubscribe(watch->connection, watch->registered);
    g_dbus_connection_signal_unsubscribe(watch->connection, watch->unregistered);
    g_object_unref(watch->connection);
    hostWatches().erase(it);
}

}  // namespace sni
