#pragma once

// xdg-desktop-portal over GDBus: file chooser opt-in, notifications, global
// shortcuts and the accent color setting. The portal is used when a backend provides the interface;
// callers keep their previous path as the fallback.

#include <gio/gio.h>
#include <glib.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <utility>

#include "../shared/accelerator_parser.h"

namespace electrobun::xdg_portal {

inline constexpr const char* kBusName = "org.freedesktop.portal.Desktop";
inline constexpr const char* kObjectPath = "/org/freedesktop/portal/desktop";
inline constexpr const char* kRequestInterface = "org.freedesktop.portal.Request";
inline constexpr const char* kFileChooserInterface = "org.freedesktop.portal.FileChooser";
inline constexpr const char* kNotificationInterface = "org.freedesktop.portal.Notification";
inline constexpr const char* kGlobalShortcutsInterface = "org.freedesktop.portal.GlobalShortcuts";
inline constexpr const char* kSettingsInterface = "org.freedesktop.portal.Settings";
inline constexpr const char* kAppearanceNamespace = "org.freedesktop.appearance";
inline constexpr const char* kAccentColorKey = "accent-color";

// Looking up the session bus without one makes GLib try to autolaunch it.
inline bool hasSessionBus() {
    if (g_getenv("DBUS_SESSION_BUS_ADDRESS")) return true;
    const char* runtimeDir = g_getenv("XDG_RUNTIME_DIR");
    if (!runtimeDir) return false;
    return access((std::string(runtimeDir) + "/bus").c_str(), F_OK) == 0;
}

// GLib's shared session connection, or null. Kept for the process lifetime.
inline GDBusConnection* sessionBus() {
    static GDBusConnection* bus = []() -> GDBusConnection* {
        if (!hasSessionBus()) return nullptr;
        GError* error = nullptr;
        GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        g_clear_error(&error);
        return connection;
    }();
    return bus;
}

// The portal's version of `interfaceName`, or 0 when no backend provides it
// (the frontend only exports interfaces some backend implements).
inline uint32_t interfaceVersion(const char* interfaceName, int timeoutMs = 1000) {
    GDBusConnection* bus = sessionBus();
    if (!bus) return 0;
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        bus, kBusName, kObjectPath, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", interfaceName, "version"), G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE, timeoutMs, nullptr, &error);
    if (!reply) {
        g_clear_error(&error);
        return 0;
    }
    GVariant* value = nullptr;
    g_variant_get(reply, "(v)", &value);
    uint32_t version = g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(value) : 0;
    g_variant_unref(value);
    g_variant_unref(reply);
    return version;
}

inline bool sandboxed() {
    return access("/.flatpak-info", F_OK) == 0 || g_getenv("SNAP") != nullptr;
}

// GTK 3's GtkFileChooserNative only uses the portal with GTK_USE_PORTAL=1 (or
// in a sandbox), and falls back to its own dialog when the portal call fails.
// GTK reads the variable once, so this runs before gtk_init. A value the user
// set is left alone.
inline bool preferPortalFileChooser() {
    if (g_getenv("GTK_USE_PORTAL")) return false;
    if (interfaceVersion(kFileChooserInterface) == 0) return false;
    g_setenv("GTK_USE_PORTAL", "1", TRUE);
    return true;
}

// The trigger the GlobalShortcuts portal expects ("CTRL+SHIFT+space"), from an
// accelerator's modifiers and its key's xkb keysym name. Empty without a name.
inline std::string shortcutTrigger(const AcceleratorParts& parts, const char* keysymName) {
    if (!keysymName || !*keysymName) return "";
    std::string trigger;
    if (parts.commandOrControl || parts.command || parts.control) trigger += "CTRL+";
    if (parts.alt) trigger += "ALT+";
    if (parts.shift) trigger += "SHIFT+";
    if (parts.super) trigger += "LOGO+";
    return trigger + keysymName;
}

inline std::string newToken() {
    static std::atomic<unsigned> counter{0};
    return "electrobun" + std::to_string(getpid()) + "_" + std::to_string(++counter);
}

// Where the portal creates the Request object for `token`, so its Response
// can be subscribed to before the call is made.
inline std::string requestPath(GDBusConnection* bus, const std::string& token) {
    const char* uniqueName = g_dbus_connection_get_unique_name(bus);
    if (!uniqueName || uniqueName[0] != ':') return "";
    std::string sender(uniqueName + 1);
    std::replace(sender.begin(), sender.end(), '.', '_');
    return std::string(kObjectPath) + "/request/" + sender + "/" + token;
}

// response: 0 success, 1 cancelled by the user, 2 anything else.
// `results` is null when the call itself failed.
using ResponseHandler = std::function<void(uint32_t response, GVariant* results)>;

namespace detail {
struct PendingRequest {
    GDBusConnection* bus = nullptr;
    guint subscription = 0;
    bool done = false;
    int refs = 2;  // the method call and the Response subscription
    ResponseHandler handler;
};

inline void release(gpointer data) {
    auto* request = static_cast<PendingRequest*>(data);
    if (--request->refs == 0) delete request;
}

inline void finish(PendingRequest* request, uint32_t response, GVariant* results) {
    if (request->done) return;
    request->done = true;
    ResponseHandler handler = std::move(request->handler);
    guint subscription = std::exchange(request->subscription, 0);
    // May drop the last reference: `request` isn't touched after this.
    if (subscription) g_dbus_connection_signal_unsubscribe(request->bus, subscription);
    handler(response, results);
}
}  // namespace detail

// Calls a portal method that answers through a Request object. `parameters`
// must carry `token` as the handle_token option. Runs on the GLib main
// context; `handler` runs there too.
inline void callRequest(const char* interfaceName, const char* method, const std::string& token,
                        GVariant* parameters, ResponseHandler handler) {
    GDBusConnection* bus = sessionBus();
    std::string path = bus ? requestPath(bus, token) : "";
    if (path.empty()) {
        g_variant_unref(g_variant_ref_sink(parameters));
        handler(2, nullptr);
        return;
    }
    auto* request = new detail::PendingRequest();
    request->bus = bus;
    request->handler = std::move(handler);
    request->subscription = g_dbus_connection_signal_subscribe(
        bus, kBusName, kRequestInterface, "Response", path.c_str(), nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* signalParameters,
           gpointer data) {
            uint32_t response = 2;
            GVariant* results = nullptr;
            g_variant_get(signalParameters, "(u@a{sv})", &response, &results);
            detail::finish(static_cast<detail::PendingRequest*>(data), response, results);
            g_variant_unref(results);
        },
        request, detail::release);
    g_dbus_connection_call(
        bus, kBusName, kObjectPath, interfaceName, method, parameters, G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            auto* request = static_cast<detail::PendingRequest*>(data);
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
            if (reply) {
                g_variant_unref(reply);
            } else {
                fprintf(stderr, "xdg-desktop-portal: %s\n", error ? error->message : "call failed");
                g_clear_error(&error);
                detail::finish(request, 2, nullptr);
            }
            detail::release(request);
        },
        request);
}

inline void addString(GVariantBuilder* builder, const char* key, const std::string& value) {
    g_variant_builder_add(builder, "{sv}", key, g_variant_new_string(value.c_str()));
}

// Notification portal (sandboxed apps; the portal can't name a host app).
// Fire and forget; returns false when the call couldn't be made.
inline bool addNotification(const std::string& id, const std::string& title, const std::string& body,
                            bool silent) {
    GDBusConnection* bus = sessionBus();
    if (!bus) return false;
    GVariantBuilder notification;
    g_variant_builder_init(&notification, G_VARIANT_TYPE("a{sv}"));
    addString(&notification, "title", title);
    if (!body.empty()) addString(&notification, "body", body);
    addString(&notification, "priority", silent ? "low" : "normal");
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        bus, kBusName, kObjectPath, kNotificationInterface, "AddNotification",
        g_variant_new("(sa{sv})", id.c_str(), &notification), nullptr, G_DBUS_CALL_FLAGS_NONE, 2000,
        nullptr, &error);
    if (!reply) {
        fprintf(stderr, "Notification portal: %s\n", error ? error->message : "call failed");
        g_clear_error(&error);
        return false;
    }
    g_variant_unref(reply);
    return true;
}

// org.freedesktop.Notifications, the notification server the portal itself
// forwards to. Returns false when there's no server.
inline bool notifyFreedesktop(const std::string& appName, const std::string& title, const std::string& body,
                              bool silent) {
    GDBusConnection* bus = sessionBus();
    if (!bus) return false;
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    if (silent) {
        g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(0));
        g_variant_builder_add(&hints, "{sv}", "suppress-sound", g_variant_new_boolean(TRUE));
    }
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify",
        g_variant_new("(susss@asa{sv}i)", appName.c_str(), 0u, "", title.c_str(), body.c_str(),
                      g_variant_new_strv(nullptr, 0), &hints, -1),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
    if (!reply) {
        fprintf(stderr, "org.freedesktop.Notifications: %s\n", error ? error->message : "call failed");
        g_clear_error(&error);
        return false;
    }
    g_variant_unref(reply);
    return true;
}

// "#rrggbb" from an accent-color value: sRGB as three doubles in 0..1, maybe
// wrapped in variants. Empty when it isn't one; out-of-range means unset.
inline std::string accentColorHex(GVariant* value) {
    if (!value) return "";
    GVariant* color = g_variant_ref(value);
    while (g_variant_is_of_type(color, G_VARIANT_TYPE_VARIANT)) {
        GVariant* inner = g_variant_get_variant(color);
        g_variant_unref(color);
        color = inner;
    }
    std::string hex;
    if (g_variant_is_of_type(color, G_VARIANT_TYPE("(ddd)"))) {
        double rgb[3] = {0, 0, 0};
        g_variant_get(color, "(ddd)", &rgb[0], &rgb[1], &rgb[2]);
        if (std::all_of(rgb, rgb + 3, [](double c) { return c >= 0 && c <= 1; })) {
            char buffer[8];
            snprintf(buffer, sizeof buffer, "#%02x%02x%02x", static_cast<unsigned>(std::lround(rgb[0] * 255)),
                     static_cast<unsigned>(std::lround(rgb[1] * 255)),
                     static_cast<unsigned>(std::lround(rgb[2] * 255)));
            hex = buffer;
        }
    }
    g_variant_unref(color);
    return hex;
}

// The desktop's accent color from the Settings portal, or empty when there's
// no portal or the desktop doesn't set one. Blocking; callable from any thread.
inline std::string readAccentColor(int timeoutMs = 1000) {
    GDBusConnection* bus = sessionBus();
    if (!bus) return "";
    // ReadOne is Settings version 2; older portals only have Read, whose
    // value comes wrapped in one more variant.
    for (const char* method : {"ReadOne", "Read"}) {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(
            bus, kBusName, kObjectPath, kSettingsInterface, method,
            g_variant_new("(ss)", kAppearanceNamespace, kAccentColorKey), G_VARIANT_TYPE("(v)"),
            G_DBUS_CALL_FLAGS_NONE, timeoutMs, nullptr, &error);
        if (reply) {
            GVariant* value = nullptr;
            g_variant_get(reply, "(v)", &value);
            std::string hex = accentColorHex(value);
            g_variant_unref(value);
            g_variant_unref(reply);
            return hex;
        }
        bool unknownMethod = g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD);
        g_clear_error(&error);
        if (!unknownMethod) return "";
    }
    return "";
}

// Calls `onChanged` with the new accent color ("" when it's unset) whenever
// the Settings portal reports one. Runs on the main context of the thread
// that calls it. Returns the subscription id, 0 without a session bus.
inline guint watchAccentColor(std::function<void(const std::string& hex)> onChanged) {
    GDBusConnection* bus = sessionBus();
    if (!bus) return 0;
    using Handler = std::function<void(const std::string&)>;
    return g_dbus_connection_signal_subscribe(
        bus, kBusName, kSettingsInterface, "SettingChanged", kObjectPath, kAppearanceNamespace,
        G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* parameters,
           gpointer data) {
            if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ssv)"))) return;
            const char* ns = nullptr;
            const char* key = nullptr;
            GVariant* value = nullptr;
            g_variant_get(parameters, "(&s&sv)", &ns, &key, &value);
            if (g_strcmp0(ns, kAppearanceNamespace) == 0 && g_strcmp0(key, kAccentColorKey) == 0) {
                (*static_cast<Handler*>(data))(accentColorHex(value));
            }
            g_variant_unref(value);
        },
        new Handler(std::move(onChanged)), [](gpointer data) { delete static_cast<Handler*>(data); });
}

// GlobalShortcuts portal: one session, rebound with the full set whenever it
// changes. The only way to get global shortcuts on Wayland. Every method runs
// on the GLib main context.
class GlobalShortcuts {
  public:
    using Activated = std::function<void(const std::string& id)>;

    static bool available() { return interfaceVersion(kGlobalShortcutsInterface) > 0; }

    void start(Activated onActivated) {
        onActivated_ = std::move(onActivated);
        GDBusConnection* bus = sessionBus();
        if (!bus || state_ != State::idle) return;
        activatedSubscription_ = g_dbus_connection_signal_subscribe(
            bus, kBusName, kGlobalShortcutsInterface, "Activated", kObjectPath, nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE, &GlobalShortcuts::onActivatedSignal, this, nullptr);
        state_ = State::creating;
        std::string token = newToken();
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        addString(&options, "handle_token", token);
        addString(&options, "session_handle_token", newToken());
        callRequest(kGlobalShortcutsInterface, "CreateSession", token, g_variant_new("(a{sv})", &options),
                    [this](uint32_t response, GVariant* results) {
                        const char* session = nullptr;
                        if (response == 0 && results &&
                            g_variant_lookup(results, "session_handle", "&s", &session) && session) {
                            session_ = session;
                            state_ = State::ready;
                            if (dirty_) bind();
                        } else {
                            state_ = State::failed;
                            fprintf(stderr, "GlobalShortcuts portal: CreateSession failed (%u)\n", response);
                        }
                    });
    }

    // `trigger` is the preferred one; the portal (or the user) may pick another.
    void set(const std::string& id, const std::string& description, const std::string& trigger) {
        shortcuts_[id] = {description, trigger};
        changed();
    }

    void remove(const std::string& id) {
        if (shortcuts_.erase(id)) changed();
    }

    void clear() {
        if (shortcuts_.empty()) return;
        shortcuts_.clear();
        changed();
    }

    bool ready() const { return state_ == State::ready; }

  private:
    enum class State { idle, creating, ready, failed };

    // Many shortcuts are usually registered at once: bind them together.
    void changed() {
        dirty_ = true;
        if (state_ != State::ready || binding_ || bindTimer_) return;
        bindTimer_ = g_timeout_add(50, [](gpointer data) -> gboolean {
            auto* self = static_cast<GlobalShortcuts*>(data);
            self->bindTimer_ = 0;
            self->bind();
            return G_SOURCE_REMOVE;
        }, this);
    }

    void bind() {
        if (binding_ || state_ != State::ready) return;
        binding_ = true;
        dirty_ = false;
        GVariantBuilder shortcuts;
        g_variant_builder_init(&shortcuts, G_VARIANT_TYPE("a(sa{sv})"));
        for (const auto& [id, shortcut] : shortcuts_) {
            GVariantBuilder properties;
            g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
            addString(&properties, "description", shortcut.first);
            if (!shortcut.second.empty()) addString(&properties, "preferred_trigger", shortcut.second);
            g_variant_builder_add(&shortcuts, "(sa{sv})", id.c_str(), &properties);
        }
        std::string token = newToken();
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        addString(&options, "handle_token", token);
        callRequest(kGlobalShortcutsInterface, "BindShortcuts", token,
                    g_variant_new("(oa(sa{sv})sa{sv})", session_.c_str(), &shortcuts, "", &options),
                    [this](uint32_t response, GVariant*) {
                        binding_ = false;
                        if (response != 0) {
                            fprintf(stderr, "GlobalShortcuts portal: BindShortcuts returned %u\n", response);
                        }
                        if (dirty_) changed();
                    });
    }

    static void onActivatedSignal(GDBusConnection*, const char*, const char*, const char*, const char*,
                                  GVariant* parameters, gpointer data) {
        auto* self = static_cast<GlobalShortcuts*>(data);
        const char* session = nullptr;
        const char* id = nullptr;
        guint64 timestamp = 0;
        GVariant* options = nullptr;
        g_variant_get(parameters, "(&o&st@a{sv})", &session, &id, &timestamp, &options);
        g_variant_unref(options);
        if (session && id && self->session_ == session && self->onActivated_) self->onActivated_(id);
    }

    Activated onActivated_;
    std::map<std::string, std::pair<std::string, std::string>> shortcuts_;
    std::string session_;
    State state_ = State::idle;
    bool dirty_ = false;
    bool binding_ = false;
    guint bindTimer_ = 0;
    guint activatedSubscription_ = 0;
};

}  // namespace electrobun::xdg_portal
