// xdg_portal.h against a mock portal on a private session bus. Run under
// dbus-run-session (scripts/test-linux-xdg-portal.sh). The mock owns
// org.freedesktop.portal.Desktop on a second connection, implements
// GlobalShortcuts (CreateSession, BindShortcuts), Settings (ReadOne, Read)
// and the version properties, answers through Request objects and emits
// Activated and SettingChanged.

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
  <interface name="org.freedesktop.portal.Settings">
    <method name="ReadOne">
      <arg type="s" name="namespace" direction="in"/>
      <arg type="s" name="key" direction="in"/>
      <arg type="v" name="value" direction="out"/>
    </method>
    <method name="Read">
      <arg type="s" name="namespace" direction="in"/>
      <arg type="s" name="key" direction="in"/>
      <arg type="v" name="value" direction="out"/>
    </method>
    <signal name="SettingChanged">
      <arg type="s" name="namespace"/>
      <arg type="s" name="key"/>
      <arg type="v" name="value"/>
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
    GVariant* accentColor = nullptr;  // (ddd), or null when the desktop sets none
    bool settingsV1 = false;          // only Read, as before Settings version 2
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
    } else if ((g_strcmp0(method, "ReadOne") == 0 && !mock->settingsV1) || g_strcmp0(method, "Read") == 0) {
        g_variant_builder_clear(&results);
        const char* ns = nullptr;
        const char* key = nullptr;
        g_variant_get(parameters, "(&s&s)", &ns, &key);
        if (g_strcmp0(ns, "org.freedesktop.appearance") != 0 || g_strcmp0(key, "accent-color") != 0 ||
            !mock->accentColor) {
            g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.portal.Error.NotFound",
                                                       "Requested setting not found");
            return;
        }
        GVariant* value = g_variant_new_variant(mock->accentColor);
        // Read wraps the value in one more variant.
        if (g_strcmp0(method, "Read") == 0) value = g_variant_new_variant(value);
        g_dbus_method_invocation_return_value(invocation, g_variant_new_tuple(&value, 1));
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

    // Settings: the accent color, read on another thread for the same reason.
    CHECK(portal::accentColorHex(nullptr).empty());
    {
        GVariant* blue = g_variant_ref_sink(g_variant_new("(ddd)", 0.0, 0.478, 1.0));
        GVariant* wrapped = g_variant_ref_sink(g_variant_new_variant(g_variant_new_variant(blue)));
        GVariant* unset = g_variant_ref_sink(g_variant_new("(ddd)", -1.0, -1.0, -1.0));
        GVariant* notColor = g_variant_ref_sink(g_variant_new_string("#ff0000"));
        CHECK(portal::accentColorHex(blue) == "#007aff");
        CHECK(portal::accentColorHex(wrapped) == "#007aff");
        CHECK(portal::accentColorHex(unset).empty());
        CHECK(portal::accentColorHex(notColor).empty());
        g_variant_unref(blue);
        g_variant_unref(wrapped);
        g_variant_unref(unset);
        g_variant_unref(notColor);
    }
    auto readAccent = []() {
        struct Job { std::string hex; gint done = 0; } job;
        GThread* thread = g_thread_new("accent", [](gpointer data) -> gpointer {
            auto* j = static_cast<Job*>(data);
            j->hex = portal::readAccentColor();
            g_atomic_int_set(&j->done, 1);
            return nullptr;
        }, &job);
        runUntil([&] { return g_atomic_int_get(&job.done) != 0; });
        g_thread_join(thread);
        return job.hex;
    };
    CHECK(readAccent().empty());  // NotFound: the desktop sets none
    mock.accentColor = g_variant_ref_sink(g_variant_new("(ddd)", 0.2, 0.6, 0.4));
    CHECK(readAccent() == "#339966");
    mock.settingsV1 = true;
    CHECK(readAccent() == "#339966");  // through Read when ReadOne is unknown
    mock.settingsV1 = false;

    std::vector<std::string> accentChanges;
    guint accentWatch = portal::watchAccentColor([&](const std::string& hex) { accentChanges.push_back(hex); });
    CHECK(accentWatch != 0);
    auto settingChanged = [&](const char* ns, const char* key, GVariant* value) {
        g_dbus_connection_emit_signal(mock.bus, clientName, portal::kObjectPath, portal::kSettingsInterface,
                                      "SettingChanged", g_variant_new("(ssv)", ns, key, value), nullptr);
    };
    settingChanged("org.freedesktop.appearance", "color-scheme", g_variant_new_uint32(1));
    settingChanged("org.gnome.desktop.interface", "accent-color", g_variant_new_string("red"));
    settingChanged("org.freedesktop.appearance", "accent-color", g_variant_new("(ddd)", 1.0, 0.0, 0.0));
    settingChanged("org.freedesktop.appearance", "accent-color", g_variant_new("(ddd)", -1.0, -1.0, -1.0));
    CHECK(runUntil([&] { return accentChanges.size() >= 2; }));
    runUntil([] { return false; }, 100);
    CHECK((accentChanges == std::vector<std::string>{"#ff0000", ""}));
    g_dbus_connection_signal_unsubscribe(client, accentWatch);
    g_variant_unref(mock.accentColor);

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
