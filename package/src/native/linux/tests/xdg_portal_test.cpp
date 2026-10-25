// xdg_portal.h against a mock portal on a private session bus. Run under
// dbus-run-session (scripts/test-linux-xdg-portal.sh). The mock owns
// org.freedesktop.portal.Desktop on a second connection, implements
// GlobalShortcuts (CreateSession, BindShortcuts) and the version properties,
// answers through Request objects and emits Activated.

#include "../xdg_portal.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace portal = electrobun::xdg_portal;

static int failures = 0;
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static const char* kIntrospection = R"XML(
<node>
  <interface name="org.freedesktop.portal.GlobalShortcuts">
    <method name="CreateSession">
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="o" name="handle" direction="out"/>
    </method>
    <method name="BindShortcuts">
      <arg type="o" name="session_handle" direction="in"/>
      <arg type="a(sa{sv})" name="shortcuts" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="o" name="handle" direction="out"/>
    </method>
    <signal name="Activated">
      <arg type="o" name="session_handle"/>
      <arg type="s" name="shortcut_id"/>
      <arg type="t" name="timestamp"/>
      <arg type="a{sv}" name="options"/>
    </signal>
    <property name="version" type="u" access="read"/>
  </interface>
  <interface name="org.freedesktop.portal.FileChooser">
    <property name="version" type="u" access="read"/>
  </interface>
</node>
)XML";

struct Mock {
    GDBusConnection* bus = nullptr;
    std::string sessionHandle;
    std::vector<std::pair<std::string, std::string>> bound;  // id, preferred_trigger
    int binds = 0;
};

static std::string senderPath(const char* sender) {
    std::string name(sender + 1);
    for (char& c : name) {
        if (c == '.') c = '_';
    }
    return name;
}

// Replies with the Request path and emits its Response once the reply is out.
static void respond(Mock* mock, GDBusMethodInvocation* invocation, const char* sender, GVariant* options,
                    GVariant* results) {
    const char* token = "missing";
    g_variant_lookup(options, "handle_token", "&s", &token);
    std::string path = std::string("/org/freedesktop/portal/desktop/request/") + senderPath(sender) + "/" + token;
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", path.c_str()));
    g_dbus_connection_emit_signal(mock->bus, sender, path.c_str(), "org.freedesktop.portal.Request", "Response",
                                  g_variant_new("(u@a{sv})", 0u, results), nullptr);
}

static void onMethod(GDBusConnection*, const char* sender, const char*, const char*, const char* method,
                     GVariant* parameters, GDBusMethodInvocation* invocation, gpointer data) {
    auto* mock = static_cast<Mock*>(data);
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
    if (g_strcmp0(method, "CreateSession") == 0) {
        GVariant* options = nullptr;
        g_variant_get(parameters, "(@a{sv})", &options);
        const char* sessionToken = "s";
        g_variant_lookup(options, "session_handle_token", "&s", &sessionToken);
        mock->sessionHandle =
            std::string("/org/freedesktop/portal/desktop/session/") + senderPath(sender) + "/" + sessionToken;
        g_variant_builder_add(&results, "{sv}", "session_handle", g_variant_new_string(mock->sessionHandle.c_str()));
        respond(mock, invocation, sender, options, g_variant_builder_end(&results));
        g_variant_unref(options);
    } else if (g_strcmp0(method, "BindShortcuts") == 0) {
        const char* session = nullptr;
        GVariantIter* shortcuts = nullptr;
        const char* parent = nullptr;
        GVariant* options = nullptr;
        g_variant_get(parameters, "(&oa(sa{sv})&s@a{sv})", &session, &shortcuts, &parent, &options);
        CHECK(mock->sessionHandle == session);
        mock->bound.clear();
        const char* id = nullptr;
        GVariant* properties = nullptr;
        while (g_variant_iter_next(shortcuts, "(&s@a{sv})", &id, &properties)) {
            const char* trigger = "";
            g_variant_lookup(properties, "preferred_trigger", "&s", &trigger);
            mock->bound.emplace_back(id, trigger);
            g_variant_unref(properties);
        }
        g_variant_iter_free(shortcuts);
        mock->binds++;
        respond(mock, invocation, sender, options, g_variant_builder_end(&results));
        g_variant_unref(options);
    } else {
        g_variant_builder_clear(&results);
        g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod", method);
    }
}

static GVariant* onGetProperty(GDBusConnection*, const char*, const char*, const char*, const char* property,
                               GError**, gpointer) {
    return g_strcmp0(property, "version") == 0 ? g_variant_new_uint32(1) : nullptr;
}

// Spins the default main context until `done` or the timeout.
template <typename Done>
static bool runUntil(Done done, int timeoutMs = 3000) {
    gint64 deadline = g_get_monotonic_time() + timeoutMs * 1000;
    while (!done() && g_get_monotonic_time() < deadline) g_main_context_iteration(nullptr, FALSE), g_usleep(1000);
    return done();
}

int main() {
    // Pure helpers.
    auto parts = electrobun::parseAccelerator("CommandOrControl+Shift+Space");
    CHECK(portal::shortcutTrigger(parts, "space") == "CTRL+SHIFT+space");
    CHECK(portal::shortcutTrigger(electrobun::parseAccelerator("Alt+Super+F5"), "F5") == "ALT+LOGO+F5");
    CHECK(portal::shortcutTrigger(electrobun::parseAccelerator("a"), "a") == "a");
    CHECK(portal::shortcutTrigger(parts, nullptr).empty());
    CHECK(portal::newToken() != portal::newToken());

    GDBusConnection* client = portal::sessionBus();
    CHECK(client != nullptr);
    if (!client) return 1;
    CHECK(portal::requestPath(client, "tok").find("/org/freedesktop/portal/desktop/request/") == 0);

    // The mock portal, on its own connection. It must own the portal name
    // before anything calls it, or D-Bus would start a real portal installed
    // on the machine.
    Mock mock;
    GError* error = nullptr;
    gchar* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    mock.bus = g_dbus_connection_new_for_address_sync(
        address, static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &error);
    g_free(address);
    CHECK(mock.bus != nullptr);
    if (!mock.bus) return 1;
    GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(kIntrospection, &error);
    static const GDBusInterfaceVTable vtable = {onMethod, onGetProperty, nullptr, {nullptr}};
    for (int i = 0; node->interfaces[i]; i++) {
        g_dbus_connection_register_object(mock.bus, portal::kObjectPath, node->interfaces[i], &vtable, &mock,
                                          nullptr, &error);
    }
    GVariant* owned = g_dbus_connection_call_sync(
        mock.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", portal::kBusName, 4u), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
        &error);
    guint32 ownership = 0;
    if (owned) {
        g_variant_get(owned, "(u)", &ownership);
        g_variant_unref(owned);
    }
    if (ownership != 1) {  // DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER
        fprintf(stderr, "mock portal didn't get %s (reply %u)\n", portal::kBusName, ownership);
        return 1;
    }

    // interfaceVersion is a blocking call the mock answers from this thread's
    // main context, so run it on another thread.
    auto versionOf = [](const char* iface) {
        struct Job { const char* iface; uint32_t version = 0; bool done = false; } job{iface};
        GThread* thread = g_thread_new("version", [](gpointer data) -> gpointer {
            auto* j = static_cast<Job*>(data);
            j->version = portal::interfaceVersion(j->iface);
            g_atomic_int_set(reinterpret_cast<gint*>(&j->done), 1);
            return nullptr;
        }, &job);
        runUntil([&] { return g_atomic_int_get(reinterpret_cast<gint*>(&job.done)) != 0; });
        g_thread_join(thread);
        return job.version;
    };
    CHECK(versionOf(portal::kGlobalShortcutsInterface) == 1);
    CHECK(versionOf(portal::kFileChooserInterface) == 1);
    CHECK(versionOf(portal::kNotificationInterface) == 0);  // not implemented by the mock

    // GlobalShortcuts: shortcuts set before the session is ready are bound once
    // it is, changes rebind the full set, and Activated reaches the callback.
    portal::GlobalShortcuts shortcuts;
    std::vector<std::string> activated;
    shortcuts.start([&](const std::string& id) { activated.push_back(id); });
    shortcuts.set("CommandOrControl+Shift+Space", "CommandOrControl+Shift+Space", "CTRL+SHIFT+space");
    shortcuts.set("Alt+F5", "Alt+F5", "ALT+F5");
    CHECK(runUntil([&] { return mock.binds >= 1 && mock.bound.size() == 2; }));
    CHECK(shortcuts.ready());
    CHECK((mock.bound == std::vector<std::pair<std::string, std::string>>{
                             {"Alt+F5", "ALT+F5"}, {"CommandOrControl+Shift+Space", "CTRL+SHIFT+space"}}));

    shortcuts.remove("Alt+F5");
    CHECK(runUntil([&] { return mock.bound.size() == 1; }));
    CHECK(!mock.bound.empty() && mock.bound.front().first == "CommandOrControl+Shift+Space");

    const char* clientName = g_dbus_connection_get_unique_name(client);
    auto activate = [&](const std::string& session, const char* id) {
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        g_dbus_connection_emit_signal(mock.bus, clientName, portal::kObjectPath, portal::kGlobalShortcutsInterface,
                                      "Activated",
                                      g_variant_new("(osta{sv})", session.c_str(), id, static_cast<guint64>(1),
                                                    &options),
                                      nullptr);
    };
    activate("/org/freedesktop/portal/desktop/session/other/x", "CommandOrControl+Shift+Space");
    activate(mock.sessionHandle, "CommandOrControl+Shift+Space");
    CHECK(runUntil([&] { return !activated.empty(); }));
    runUntil([] { return false; }, 100);
    CHECK((activated == std::vector<std::string>{"CommandOrControl+Shift+Space"}));

    // FileChooser opt-in, run on another thread for the same reason as above.
    g_unsetenv("GTK_USE_PORTAL");
    struct Opt { bool result = false; gint done = 0; } opt;
    GThread* optThread = g_thread_new("opt-in", [](gpointer data) -> gpointer {
        auto* o = static_cast<Opt*>(data);
        o->result = portal::preferPortalFileChooser();
        g_atomic_int_set(&o->done, 1);
        return nullptr;
    }, &opt);
    runUntil([&] { return g_atomic_int_get(&opt.done) != 0; });
    g_thread_join(optThread);
    CHECK(opt.result);
    CHECK(g_strcmp0(g_getenv("GTK_USE_PORTAL"), "1") == 0);
    g_setenv("GTK_USE_PORTAL", "0", TRUE);
    CHECK(!portal::preferPortalFileChooser());  // a value the user set is kept
    CHECK(g_strcmp0(g_getenv("GTK_USE_PORTAL"), "0") == 0);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("xdg-desktop-portal tests passed\n");
    return 0;
}
