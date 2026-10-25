// spawn_fallbacks.h against fake xdg-open, gio and notify-send on PATH that
// record their argv.
#include "../spawn_fallbacks.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                           \
        }                                                                         \
    } while (0)

std::string dir;

// Each fake writes its arguments, each followed by a NUL, to <dir>/<name>.argv
// and exits with the status in <dir>/<name>.status (0 when missing).
void writeFake(const std::string& name) {
    const std::string path = dir + "/" + name;
    std::ofstream script(path);
    script << "#!/bin/sh\n"
           << "printf '%s\\0' \"$@\" > '" << path << ".argv'\n"
           << "[ -f '" << path << ".status' ] && read status < '" << path << ".status' && exit \"$status\"\n"
           << "exit 0\n";
    script.close();
    chmod(path.c_str(), 0755);
}

void setStatus(const std::string& name, int status) {
    std::ofstream(dir + "/" + name + ".status") << status << "\n";
}

std::vector<std::string> recorded(const std::string& name) {
    std::ifstream in(dir + "/" + name + ".argv", std::ios::binary);
    if (!in) return {"<not run>"};
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string data = buffer.str();
    std::vector<std::string> args;
    size_t start = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '\0') {
            args.push_back(data.substr(start, i - start));
            start = i + 1;
        }
    }
    return args;
}

void forget(const std::string& name) {
    std::remove((dir + "/" + name + ".argv").c_str());
    std::remove((dir + "/" + name + ".status").c_str());
}

bool exists(const std::string& path) {
    return g_file_test(path.c_str(), G_FILE_TEST_EXISTS);
}

}  // namespace

int main() {
    namespace sf = electrobun::spawn_fallbacks;

    char dirTemplate[] = "/tmp/electrobun-spawn-fallbacks.XXXXXX";
    if (!mkdtemp(dirTemplate)) {
        std::perror("mkdtemp");
        return 2;
    }
    dir = dirTemplate;
    for (const char* name : {"xdg-open", "gio", "notify-send"}) {
        writeFake(name);
    }
    g_setenv("PATH", dir.c_str(), TRUE);
    // Commands in the strings below would create this file if a shell ran them.
    const std::string pwned = dir + "/pwned";
    if (chdir(dir.c_str()) != 0) {
        std::perror("chdir");
        return 2;
    }

    // A URL with a command substitution, a backtick and quotes reaches
    // xdg-open as one argument and runs nothing.
    const std::string url = "https://example.com/?q=\"$(touch pwned)\"`touch pwned`'x'";
    CHECK(sf::xdgOpen(url));
    CHECK((recorded("xdg-open") == std::vector<std::string>{url}));
    CHECK(!exists(pwned));

    // xdg-open has no "--", so a target starting with "-" isn't passed.
    forget("xdg-open");
    CHECK(!sf::xdgOpen("--help"));
    CHECK((recorded("xdg-open") == std::vector<std::string>{"<not run>"}));

    // gio open gets "--" before the target.
    const std::string fileUri = "file:///tmp/a \"b\" $(touch pwned)";
    CHECK(sf::gioOpen(fileUri));
    CHECK((recorded("gio") == std::vector<std::string>{"open", "--", fileUri}));
    CHECK(!exists(pwned));

    // notify-send: options end before the title, whatever it looks like.
    const std::string title = "-u critical $(touch pwned) `id` \"q\" 'q'";
    const std::string body = "--help\nsecond line";
    CHECK(sf::notifySend(title, body, true));
    CHECK((recorded("notify-send") == std::vector<std::string>{"--urgency=low", "--", title, body}));
    CHECK(sf::notifySend("Hi", "", false));
    CHECK((recorded("notify-send") == std::vector<std::string>{"--", "Hi"}));
    CHECK(!exists(pwned));

    // A failing launcher is reported, so the caller can fall back.
    setStatus("xdg-open", 3);
    CHECK(!sf::xdgOpen(url));
    forget("xdg-open");

    // A missing launcher is reported.
    g_setenv("PATH", "/nonexistent", TRUE);
    CHECK(!sf::xdgOpen(url));

    for (const char* name : {"xdg-open", "gio", "notify-send"}) {
        forget(name);
        std::remove((dir + "/" + name).c_str());
    }
    std::remove(pwned.c_str());
    rmdir(dir.c_str());

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("spawn_fallbacks tests passed\n");
    return 0;
}
