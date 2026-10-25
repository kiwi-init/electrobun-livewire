// Fallback launchers (xdg-open, gio open, notify-send), run with an argv and
// no shell, so URLs, paths and text reach them as single arguments.
#pragma once

#include <glib.h>

#include <cstdio>
#include <string>
#include <vector>

namespace electrobun::spawn_fallbacks {

// Runs args[0] from PATH with the rest as its arguments and waits for it.
// True when it exits 0.
inline bool run(std::vector<std::string> args) {
    std::vector<char*> argv;
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    gint status = 0;
    GError* error = nullptr;
    if (!g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, nullptr,
                      &status, &error)) {
        fprintf(stderr, "Warning: can't run %s: %s\n", args[0].c_str(), error->message);
        g_error_free(error);
        return false;
    }
    if (!g_spawn_check_wait_status(status, &error)) {
        fprintf(stderr, "Warning: %s failed: %s\n", args[0].c_str(), error->message);
        g_error_free(error);
        return false;
    }
    return true;
}

// "--" ends notify-send's options, so a title or body starting with "-" is
// shown as text.
inline std::vector<std::string> notifySendArgv(const std::string& title, const std::string& body, bool silent) {
    std::vector<std::string> args{"notify-send"};
    if (silent) {
        args.push_back("--urgency=low");
    }
    args.push_back("--");
    args.push_back(title);
    if (!body.empty()) {
        args.push_back(body);
    }
    return args;
}

inline std::vector<std::string> gioOpenArgv(const std::string& target) {
    return {"gio", "open", "--", target};
}

// xdg-open takes no "--" and reads anything starting with "-" as an option,
// so such a target is refused rather than passed.
inline bool xdgOpen(const std::string& target) {
    if (target.empty() || target[0] == '-') {
        fprintf(stderr, "Warning: xdg-open can't open a target starting with '-'\n");
        return false;
    }
    return run({"xdg-open", target});
}

inline bool gioOpen(const std::string& target) {
    return run(gioOpenArgv(target));
}

inline bool notifySend(const std::string& title, const std::string& body, bool silent) {
    return run(notifySendArgv(title, body, silent));
}

}  // namespace electrobun::spawn_fallbacks
