// sni_tray against a mock org.kde.StatusNotifierWatcher on a private session
// bus. Run under dbus-run-session (scripts/test-linux-sni-tray.sh).
//
// The item, the mock watcher and the host watch live on the main loop; the
// checks run on a worker thread with their own connection, the way a bar
// would see the item.
#include "../sni_tray.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                             \
        }                                                                           \
    } while (0)

GDBusConnection* connect() {
    GError* error = nullptr;
    const char* address = g_getenv("DBUS_SESSION_BUS_ADDRESS");
    if (!address) {
        std::fprintf(stderr, "DBUS_SESSION_BUS_ADDRESS is not set; run under dbus-run-session\n");
        std::exit(2);
    }
    GDBusConnection* connection = g_dbus_connection_new_for_address_sync(
        address,
        static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &error);
    if (!connection) {
        std::fprintf(stderr, "can't connect: %s\n", error->message);
        std::exit(2);
    }
    return connection;
}

bool waitFor(const std::function<bool()>& condition, int ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

// Runs work on the loop that owns context (nullptr: the main loop) and waits.
void onContext(GMainContext* context, std::function<void()> work) {
    using Job = std::pair<std::function<void()>, std::promise<void>>;
    auto* job = new Job(std::move(work), std::promise<void>());
    auto future = job->second.get_future();
    g_main_context_invoke(context, [](gpointer data) -> gboolean {
        auto* job = static_cast<Job*>(data);
        job->first();
        job->second.set_value();
        delete job;
        return G_SOURCE_REMOVE;
    }, job);
    future.wait();
}

void onMain(std::function<void()> work) {
    onContext(nullptr, std::move(work));
}

// --- mock watcher ----------------------------------------------------------

constexpr const char* kWatcherXml = R"XML(
<node>
  <interface name="org.kde.StatusNotifierWatcher">
    <method name="RegisterStatusNotifierItem"><arg name="service" type="s" direction="in"/></method>
    <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
    <signal name="StatusNotifierHostRegistered"/>
  </interface>
</node>
)XML";

// The watcher is another process in real life, and the item calls it
// synchronously from the main loop, so it gets its own thread and context.
struct Watcher {
    GMainContext* context = nullptr;
    GDBusConnection* connection = nullptr;
    guint registration = 0;
    guint owner = 0;
    std::atomic<bool> owned{false};
    std::atomic<bool> hostRegistered{false};
    std::mutex mutex;
    std::vector<std::string> registered;

    size_t registrations() {
        std::lock_guard<std::mutex> lock(mutex);
        return registered.size();
    }
    std::string lastRegistered() {
        std::lock_guard<std::mutex> lock(mutex);
        return registered.empty() ? "" : registered.back();
    }
};

Watcher watcher;

void watcherMethod(GDBusConnection*, const char*, const char*, const char*, const char* method, GVariant* params,
                   GDBusMethodInvocation* invocation, gpointer) {
    if (g_strcmp0(method, "RegisterStatusNotifierItem") == 0) {
        const char* service = nullptr;
        g_variant_get(params, "(&s)", &service);
        {
            std::lock_guard<std::mutex> lock(watcher.mutex);
            watcher.registered.emplace_back(service);
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "no");
}

GVariant* watcherProperty(GDBusConnection*, const char*, const char*, const char*, const char*, GError**, gpointer) {
    return g_variant_new_boolean(watcher.hostRegistered.load());
}

const GDBusInterfaceVTable kWatcherVTable = {watcherMethod, watcherProperty, nullptr, {nullptr}};

void startWatcherOnItsThread() {
    static GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(kWatcherXml, nullptr);
    watcher.registration = g_dbus_connection_register_object(watcher.connection, "/StatusNotifierWatcher",
                                                             info->interfaces[0], &kWatcherVTable, nullptr,
                                                             nullptr, nullptr);
    watcher.owner = g_bus_own_name_on_connection(
        watcher.connection, "org.kde.StatusNotifierWatcher", G_BUS_NAME_OWNER_FLAGS_NONE,
        [](GDBusConnection*, const char*, gpointer) { watcher.owned = true; }, nullptr, nullptr, nullptr);
}

void stopWatcherOnItsThread() {
    g_bus_unown_name(watcher.owner);
    g_dbus_connection_unregister_object(watcher.connection, watcher.registration);
    watcher.owned = false;
}

void startWatcher() { onContext(watcher.context, startWatcherOnItsThread); }
void stopWatcher() { onContext(watcher.context, stopWatcherOnItsThread); }

// --- the bar's side ----------------------------------------------------------

GVariant* call(GDBusConnection* bar, const std::string& name, const char* path, const char* interface,
               const char* method, GVariant* args, const char* replyType) {
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(bar, name.c_str(), path, interface, method, args,
                                                  replyType ? G_VARIANT_TYPE(replyType) : nullptr,
                                                  G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
    if (!reply) {
        std::fprintf(stderr, "call %s.%s failed: %s\n", interface, method, error->message);
        g_error_free(error);
        failures++;
    }
    return reply;
}

GVariant* itemProperty(GDBusConnection* bar, const std::string& name, const char* property) {
    GVariant* reply = call(bar, name, "/StatusNotifierItem", "org.freedesktop.DBus.Properties", "Get",
                           g_variant_new("(ss)", "org.kde.StatusNotifierItem", property), "(v)");
    if (!reply) return nullptr;
    GVariant* value = nullptr;
    g_variant_get(reply, "(v)", &value);
    g_variant_unref(reply);
    return value;
}

std::string stringProperty(GDBusConnection* bar, const std::string& name, const char* property) {
    GVariant* value = itemProperty(bar, name, property);
    if (!value) return "";
    std::string result = g_variant_get_string(value, nullptr);
    g_variant_unref(value);
    return result;
}

std::string menuLabel(GVariant* node) {
    GVariant* props = g_variant_get_child_value(node, 1);
    const char* label = nullptr;
    std::string result = g_variant_lookup(props, "label", "&s", &label) ? label : "";
    g_variant_unref(props);
    return result;
}

}  // namespace

int main() {
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    GDBusConnection* app = connect();
    watcher.connection = connect();
    watcher.context = g_main_context_new();
    GMainLoop* watcherLoop = g_main_loop_new(watcher.context, FALSE);
    std::thread watcherThread([&] {
        g_main_context_push_thread_default(watcher.context);
        g_main_loop_run(watcherLoop);
        g_main_context_pop_thread_default(watcher.context);
    });

    std::atomic<int> activations{0};
    std::mutex actionsMutex;
    std::vector<std::string> actions;
    std::vector<bool> hostChanges;
    std::mutex hostMutex;

    sni::StatusNotifierItem item(
        app, "kitchen", [&] { activations++; },
        [&](const std::string& action) {
            std::lock_guard<std::mutex> lock(actionsMutex);
            actions.push_back(action);
        });
    item.setTitle("Kitchen Tray");
    item.setIconPixmap(sni::Pixmap{1, 1, {0x11223344u}});
    if (!item.start()) {
        std::fprintf(stderr, "start() failed\n");
        return 1;
    }
    guint hostWatch = sni::watchHost(app, [&](bool available) {
        std::lock_guard<std::mutex> lock(hostMutex);
        hostChanges.push_back(available);
    });

    std::thread checks([&] {
        GDBusConnection* bar = connect();
        const std::string name = item.busName();

        // No watcher yet: no host, nothing registered.
        CHECK(!sni::hostAvailable(bar));
        CHECK(watcher.registrations() == 0);

        // The watcher appears: the item registers with its own bus name.
        startWatcher();
        CHECK(waitFor([] { return watcher.registrations() == 1; }));
        CHECK(watcher.lastRegistered() == name);
        CHECK(name.rfind("org.kde.StatusNotifierItem-", 0) == 0);

        // A watcher with no host isn't somewhere to show a tray.
        CHECK(sni::watcherAvailable(bar));
        CHECK(!sni::hostAvailable(bar));

        // Item properties.
        CHECK(stringProperty(bar, name, "Id") == "kitchen");
        CHECK(stringProperty(bar, name, "Title") == "Kitchen Tray");
        CHECK(stringProperty(bar, name, "Status") == "Active");
        CHECK(stringProperty(bar, name, "Category") == "ApplicationStatus");
        CHECK(stringProperty(bar, name, "Menu") == "/MenuBar");
        if (GVariant* isMenu = itemProperty(bar, name, "ItemIsMenu")) {
            CHECK(!g_variant_get_boolean(isMenu));
            g_variant_unref(isMenu);
        }
        if (GVariant* pixmaps = itemProperty(bar, name, "IconPixmap")) {
            CHECK(g_variant_n_children(pixmaps) == 1);
            if (g_variant_n_children(pixmaps) == 1) {
                gint32 width = 0, height = 0;
                GVariant* bytes = nullptr;
                g_variant_get_child(pixmaps, 0, "(ii@ay)", &width, &height, &bytes);
                gsize length = 0;
                auto* data = static_cast<const guint8*>(g_variant_get_fixed_array(bytes, &length, 1));
                CHECK(width == 1 && height == 1 && length == 4);
                // ARGB32 in network byte order.
                CHECK(length == 4 && data[0] == 0x11 && data[1] == 0x22 && data[2] == 0x33 && data[3] == 0x44);
                g_variant_unref(bytes);
            }
            g_variant_unref(pixmaps);
        }
        if (GVariant* tip = itemProperty(bar, name, "ToolTip")) {
            const char* title = nullptr;
            g_variant_get_child(tip, 2, "&s", &title);
            CHECK(g_strcmp0(title, "Kitchen Tray") == 0);
            g_variant_unref(tip);
        }

        // Left click.
        if (GVariant* r = call(bar, name, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "Activate",
                               g_variant_new("(ii)", 0, 0), nullptr)) {
            g_variant_unref(r);
        }
        CHECK(waitFor([&] { return activations.load() == 1; }));

        // The menu, and LayoutUpdated with a new revision when it changes.
        std::atomic<int> layoutUpdates{0};
        guint layoutSub = g_dbus_connection_signal_subscribe(
            bar, name.c_str(), "com.canonical.dbusmenu", "LayoutUpdated", "/MenuBar", nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*, gpointer data) {
                (*static_cast<std::atomic<int>*>(data))++;
            },
            &layoutUpdates, nullptr);
        // Subscribed with no thread-default context, so the main loop
        // dispatches it.

        sni::MenuItem open;
        open.label = "Open";
        open.action = "open";
        sni::MenuItem separator;
        separator.type = "separator";
        sni::MenuItem checked;
        checked.label = "Pinned";
        checked.action = "pin";
        checked.checked = true;
        sni::MenuItem off;
        off.label = "Off";
        off.action = "off";
        off.enabled = false;
        sni::MenuItem child;
        child.label = "Child";
        child.action = "child";
        sni::MenuItem more;
        more.label = "More";
        more.children = {child};
        onMain([&] { item.setMenu({open, separator, checked, off, more}); });

        GVariant* layoutReply = call(bar, name, "/MenuBar", "com.canonical.dbusmenu", "GetLayout",
                                     g_variant_new("(ii@as)", 0, -1, g_variant_new_strv(nullptr, 0)),
                                     "(u(ia{sv}av))");
        int32_t openId = -1, offId = -1, childId = -1, moreId = -1;
        if (layoutReply) {
            guint32 revision = 0;
            GVariant* root = nullptr;
            g_variant_get(layoutReply, "(u@(ia{sv}av))", &revision, &root);
            CHECK(revision >= 2);
            GVariant* children = g_variant_get_child_value(root, 2);
            CHECK(g_variant_n_children(children) == 5);
            if (g_variant_n_children(children) == 5) {
                auto node = [&](size_t i) {
                    GVariant* boxed = g_variant_get_child_value(children, i);
                    GVariant* inner = g_variant_get_variant(boxed);
                    g_variant_unref(boxed);
                    return inner;
                };
                GVariant* n0 = node(0);
                GVariant* n1 = node(1);
                GVariant* n2 = node(2);
                GVariant* n3 = node(3);
                GVariant* n4 = node(4);
                g_variant_get_child(n0, 0, "i", &openId);
                g_variant_get_child(n3, 0, "i", &offId);
                g_variant_get_child(n4, 0, "i", &moreId);
                CHECK(menuLabel(n0) == "Open");
                GVariant* p1 = g_variant_get_child_value(n1, 1);
                const char* type = nullptr;
                CHECK(g_variant_lookup(p1, "type", "&s", &type) && g_strcmp0(type, "separator") == 0);
                g_variant_unref(p1);
                GVariant* p2 = g_variant_get_child_value(n2, 1);
                gint32 toggleState = 0;
                CHECK(g_variant_lookup(p2, "toggle-state", "i", &toggleState) && toggleState == 1);
                g_variant_unref(p2);
                GVariant* p3 = g_variant_get_child_value(n3, 1);
                gboolean enabled = TRUE;
                CHECK(g_variant_lookup(p3, "enabled", "b", &enabled) && !enabled);
                g_variant_unref(p3);
                CHECK(menuLabel(n4) == "More");
                GVariant* moreChildren = g_variant_get_child_value(n4, 2);
                CHECK(g_variant_n_children(moreChildren) == 1);
                if (g_variant_n_children(moreChildren) == 1) {
                    GVariant* boxed = g_variant_get_child_value(moreChildren, 0);
                    GVariant* c = g_variant_get_variant(boxed);
                    g_variant_get_child(c, 0, "i", &childId);
                    CHECK(menuLabel(c) == "Child");
                    g_variant_unref(c);
                    g_variant_unref(boxed);
                }
                g_variant_unref(moreChildren);
                for (GVariant* n : {n0, n1, n2, n3, n4}) g_variant_unref(n);
            }
            g_variant_unref(children);
            g_variant_unref(root);
            g_variant_unref(layoutReply);
        }
        CHECK(waitFor([&] { return layoutUpdates.load() >= 1; }));
        g_dbus_connection_signal_unsubscribe(bar, layoutSub);

        // Clicks: enabled leaves fire their action; disabled items and
        // submenu parents don't.
        auto click = [&](int32_t id) {
            if (GVariant* r = call(bar, name, "/MenuBar", "com.canonical.dbusmenu", "Event",
                                   g_variant_new("(isvu)", id, "clicked", g_variant_new_int32(0), 0u), nullptr)) {
                g_variant_unref(r);
            }
        };
        click(openId);
        click(offId);
        click(moreId);
        click(childId);
        CHECK(waitFor([&] {
            std::lock_guard<std::mutex> lock(actionsMutex);
            return actions.size() >= 2;
        }));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        {
            std::lock_guard<std::mutex> lock(actionsMutex);
            CHECK(actions.size() == 2);
            CHECK(actions.size() == 2 && actions[0] == "open" && actions[1] == "child");
        }

        // Hidden is Passive.
        onMain([&] { item.setVisible(false); });
        CHECK(stringProperty(bar, name, "Status") == "Passive");
        onMain([&] { item.setVisible(true); });
        CHECK(stringProperty(bar, name, "Status") == "Active");

        // A host registers: hostAvailable and the watch say so.
        watcher.hostRegistered = true;
        onContext(watcher.context, [] {
            g_dbus_connection_emit_signal(watcher.connection, nullptr, "/StatusNotifierWatcher",
                                          "org.kde.StatusNotifierWatcher", "StatusNotifierHostRegistered",
                                          nullptr, nullptr);
        });
        CHECK(sni::hostAvailable(bar));
        CHECK(waitFor([&] {
            std::lock_guard<std::mutex> lock(hostMutex);
            return !hostChanges.empty() && hostChanges.back();
        }));

        // The bar restarts: the watcher leaves (no host) and comes back, and
        // the item registers again.
        stopWatcher();
        CHECK(waitFor([&] {
            std::lock_guard<std::mutex> lock(hostMutex);
            return !hostChanges.empty() && !hostChanges.back();
        }));
        CHECK(!sni::hostAvailable(bar));
        startWatcher();
        CHECK(waitFor([] { return watcher.registrations() == 2; }));
        CHECK(watcher.lastRegistered() == name);

        g_object_unref(bar);
        g_main_loop_quit(loop);
    });

    g_main_loop_run(loop);
    checks.join();
    sni::unwatchHost(hostWatch);
    g_main_loop_quit(watcherLoop);
    watcherThread.join();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("sni_tray tests passed\n");
    return 0;
}
