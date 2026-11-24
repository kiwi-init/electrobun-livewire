// webview2_guards.h - The rules behind the guards an app can turn on for its
// Windows WebView2 views (nativeWrapper.cpp's webview2* exports).
//
// This is deliberately independent of WebView2.h so the rules can be tested on
// every build host.

#ifndef ELECTROBUN_WEBVIEW2_GUARDS_H
#define ELECTROBUN_WEBVIEW2_GUARDS_H

#include <cstddef>
#include <string>

namespace electrobun {

namespace webview2_guard_detail {

inline bool hasSchemeIgnoringCase(const std::string& url, const char* scheme) {
    size_t index = 0;
    for (; scheme[index] != '\0'; ++index) {
        if (index >= url.size()) return false;
        char character = url[index];
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
        if (character != scheme[index]) return false;
    }
    return true;
}

} // namespace webview2_guard_detail

// A download the page made itself: a blob: or data: URL. Every other download
// comes from the network.
inline bool isOwnDownloadUrl(const std::string& url) {
    return webview2_guard_detail::hasSchemeIgnoringCase(url, "blob:") ||
           webview2_guard_detail::hasSchemeIgnoringCase(url, "data:");
}

// Whether a view locked to `page` may navigate to `url`: only to the page
// itself, with or without a fragment. A query, a longer path or another
// spelling of the same address is a different page.
inline bool isAllowedPageUrl(const std::string& page, const std::string& url) {
    if (page.empty() || url.compare(0, page.size(), page) != 0) return false;
    return url.size() == page.size() || url[page.size()] == '#';
}

// The browser arguments for every WebView2 environment. An app that keeps web
// security gets none of the switches that turn the same-origin policy, the
// localhost certificate check or SmartScreen off.
inline const wchar_t* webView2BrowserArguments(bool keepWebSecurity) {
    return keepWebSecurity
        ? L"--disable-features=msWebOOUI,msPdfOOUI"
        : L"--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection --allow-insecure-localhost --disable-web-security";
}

} // namespace electrobun

#endif // ELECTROBUN_WEBVIEW2_GUARDS_H
