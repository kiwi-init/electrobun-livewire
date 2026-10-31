// inspector_setting.h - Reads runtime.inspector from build.json.
// Apps set it to false to ship without the web inspector. Any other value, or
// a build.json without it, keeps the inspector on.

#ifndef ELECTROBUN_INSPECTOR_SETTING_H
#define ELECTROBUN_INSPECTOR_SETTING_H

#include <cctype>
#include <string>

namespace electrobun {

inline size_t skipJsonWhitespace(const std::string& json, size_t at) {
    while (at < json.size() && std::isspace(static_cast<unsigned char>(json[at]))) at++;
    return at;
}

// Returns the index just past the closing quote of the string opening at `at`.
inline size_t skipJsonString(const std::string& json, size_t at) {
    at++;
    while (at < json.size() && json[at] != '"') at += json[at] == '\\' ? 2 : 1;
    return at + 1;
}

// Walks one object's direct keys and returns where `key`'s value starts, or
// npos. `at` is the index of the object's opening brace.
inline size_t findJsonValue(const std::string& json, size_t at, const std::string& key) {
    int depth = 0;
    bool expectKey = false;
    while (at < json.size()) {
        const char c = json[at];
        if (c == '"') {
            const size_t end = skipJsonString(json, at);
            if (depth == 1 && expectKey) {
                const size_t colon = skipJsonWhitespace(json, end);
                if (json.compare(at + 1, end - at - 2, key) == 0 && colon < json.size() && json[colon] == ':') {
                    return skipJsonWhitespace(json, colon + 1);
                }
                expectKey = false;
            }
            at = end;
            continue;
        }
        if (c == '{' || c == '[') {
            depth++;
            if (depth == 1) expectKey = true;
        } else if (c == '}' || c == ']') {
            depth--;
            if (depth == 0) return std::string::npos;
        } else if (c == ',' && depth == 1) {
            expectKey = true;
        }
        at++;
    }
    return std::string::npos;
}

inline bool inspectorEnabled(const std::string& buildJson) {
    const size_t root = skipJsonWhitespace(buildJson, 0);
    if (root >= buildJson.size() || buildJson[root] != '{') return true;
    const size_t runtime = findJsonValue(buildJson, root, "runtime");
    if (runtime == std::string::npos || runtime >= buildJson.size() || buildJson[runtime] != '{') return true;
    const size_t value = findJsonValue(buildJson, runtime, "inspector");
    if (value == std::string::npos) return true;
    return buildJson.compare(value, 5, "false") != 0;
}

}  // namespace electrobun

#endif
