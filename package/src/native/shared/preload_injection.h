// preload_injection.h - where CEF's response filters put the preload <script>
// tags in an HTML document. Used by the macOS and Windows filters.
#ifndef ELECTROBUN_PRELOAD_INJECTION_H
#define ELECTROBUN_PRELOAD_INJECTION_H

#include <algorithm>
#include <cctype>
#include <string>

namespace electrobun {

inline size_t findCaseInsensitive(const std::string& haystack, const std::string& needle) {
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it == haystack.end() ? std::string::npos : static_cast<size_t>(it - haystack.begin());
}

// Inserts |scriptTags| so they run before any page script: right after the
// opening <head> tag; else in a new <head> right after <html>; else after the
// doctype (a script there is legal and keeps standards mode); else at the start.
inline void injectPreloadScripts(std::string& html, const std::string& scriptTags) {
    size_t head = findCaseInsensitive(html, "<head>");
    if (head == std::string::npos) head = findCaseInsensitive(html, "<head ");
    if (head != std::string::npos) {
        size_t end = html.find('>', head);
        if (end != std::string::npos) {
            html.insert(end + 1, scriptTags);
            return;
        }
    }
    size_t htmlTag = findCaseInsensitive(html, "<html");
    if (htmlTag != std::string::npos) {
        size_t end = html.find('>', htmlTag);
        if (end != std::string::npos) {
            html.insert(end + 1, "<head>" + scriptTags + "</head>");
            return;
        }
    }
    size_t doctype = findCaseInsensitive(html, "<!doctype");
    if (doctype != std::string::npos) {
        size_t end = html.find('>', doctype);
        if (end != std::string::npos) {
            html.insert(end + 1, scriptTags);
            return;
        }
    }
    html.insert(0, scriptTags);
}

} // namespace electrobun

#endif // ELECTROBUN_PRELOAD_INJECTION_H
