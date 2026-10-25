#include <gtk/gtk.h>
#include <signal.h>
#include <fcntl.h>
#include <webkit2/webkit2.h>
#include <libsoup/soup.h>
#include <jsc/jsc.h>
#ifndef NO_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>
#endif
#include <gdk/gdkx.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/keysymdef.h>
#include <X11/XF86keysym.h>
#include <string>
#include <vector>
#include <memory>
#include <pthread.h>
#include <map>
#include <iostream>
#include <cstring>
#include <dlfcn.h>
#include <algorithm>
#include <limits>
#include <sstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <unistd.h>
#include <functional>
#include <execinfo.h>
#include <cmath>
#include <atomic>
#include "../shared/pending_resize_queue.h"
#include <gio/gio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <filesystem>
#include <set>
#include <cstdarg>
#include <sys/socket.h>
#include <netinet/in.h>
#include "dawn/webgpu.h"

// Shared cross-platform utilities
#include "../shared/glob_match.h"
#include "../shared/callbacks.h"
#include "../shared/permissions.h"
#include "../shared/mime_types.h"
#include "../shared/asar.h"
#include "../shared/config.h"
#include "../shared/preload_script.h"
#include "../shared/webview_storage.h"
#include "../shared/navigation_rules.h"
#include "../shared/thread_safe_map.h"
#include "../shared/view_content_security_policy.h"
#include "../shared/shutdown_guard.h"
#include "../shared/ffi_helpers.h"
#include "../shared/json_menu_parser.h"
#include "../shared/download_event.h"
#include "../shared/app_paths.h"
#include "../shared/views_url.h"
#include "../shared/accelerator_parser.h"
#include "../shared/chromium_flags.h"
#include "../shared/cache_migration.h"
#include "../shared/console_forwarding.h"
#include "native_file_dialog.h"
#include "xdg_portal.h"
#include "spawn_fallbacks.h"
#include "../shared/dialog_paths.h"
#include "../shared/cef_find_session.h"
#include "../shared/linux_dpi.h"
#include "../shared/linux_x11_geometry.h"
#include "wayland_screen_capture.h"
#include "sni_tray.h"
#include "window_resize_handles.h"

using namespace electrobun;

// Global ASAR archive handle (lazy-loaded) with thread-safe initialization
// ASAR C FFI declarations are in shared/asar.h
static AsarArchive* g_asarArchive = nullptr;
static std::once_flag g_asarArchiveInitFlag;
static std::mutex g_asarReadMutex; // Mutex to protect ASAR read operations

// Global shutdown flag to prevent race conditions during cleanup
// Note: shared/shutdown_guard.h provides ShutdownManager singleton for new code
// This local atomic is kept for direct access patterns used throughout this file
static std::atomic<bool> g_shuttingDown{false};

// Quit/shutdown coordination
static QuitRequestedHandler g_quitRequestedHandler = nullptr;
static std::atomic<bool> g_shutdownComplete{false};
static std::atomic<bool> g_eventLoopStopping{false};

// Thread that runs the UI event loop. GLib context ownership alone cannot
// identify it: CefRunMessageLoop runs Chromium tasks (process messages,
// browser callbacks) outside GLib dispatch, where the default context is not
// owned. Re-entrant SDK calls from those callbacks must run inline instead of
// queueing onto the loop they are blocking.
static std::atomic<std::thread::id> g_eventLoopThreadId{std::thread::id()};

static bool isEventLoopThread() {
    return g_main_context_is_owner(g_main_context_default()) ||
           g_eventLoopThreadId.load(std::memory_order_acquire) == std::this_thread::get_id();
}

// Self-pipe for async-signal-safe signal handling.
// Signal handler writes to pipe, GLib IO watch reads and dispatches.
static int g_signal_pipe[2] = {-1, -1};
static int g_sigint_count = 0;

// Additional race condition protection
static std::atomic<int> g_activeOperations{0};
static std::mutex g_cefBrowserMutex;

// Map from CEF browser identifier to electrobun webviewId, used by scheme handlers
static std::map<int, uint32_t> g_browserIdToWebviewId;
static std::mutex g_browserIdToWebviewIdMutex;

// Use OperationGuard from shared/shutdown_guard.h
using electrobun::OperationGuard;

// CEF includes - always include them even if it marginally increases binary size
// we want a few binaries that will work whenever an electrobun developer
// adds CEF into their bundles
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/views/cef_display.h"
#include "include/cef_load_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_response_filter.h"
#include "include/cef_permission_handler.h"
#include "include/cef_dialog_handler.h"
#include "../shared/permissions_cef.h"
#include "../shared/partition_context.h"
#include "include/cef_download_handler.h"
#include "include/wrapper/cef_helpers.h"

// CEF dynamic loader for weak linking
#include "cef_loader.h"


// The rest of the Linux wrapper lives in wrapper/*.inc, in this order. They
// aren't separate translation units: this file includes them as one, exactly
// as when they were a single file, so a part can use anything declared in an
// earlier one. From event_loop.inc on, everything is inside the
// `extern "C" {` that event_loop.inc opens and sessions_and_misc.inc closes.
#include "wrapper/globals.inc"
#include "wrapper/cef_app.inc"
#include "wrapper/abstract_view.inc"
#include "wrapper/webkit_view.inc"
#include "wrapper/wgpu_view.inc"
#include "wrapper/cef_view.inc"
#include "wrapper/container_view.inc"
#include "wrapper/tray.inc"
#include "wrapper/windows_and_menus.inc"
#include "wrapper/url_schemes.inc"
#include "wrapper/gtk_runtime.inc"
#include "wrapper/event_loop.inc"
#include "wrapper/window_exports.inc"
#include "wrapper/view_exports.inc"
#include "wrapper/wgpu_shims.inc"
#include "wrapper/webview_exports.inc"
#include "wrapper/system_exports.inc"
#include "wrapper/clipboard_tray_menus.inc"
#include "wrapper/lifecycle_and_window_state.inc"
#include "wrapper/shortcuts_and_displays.inc"
#include "wrapper/sessions_and_misc.inc"
