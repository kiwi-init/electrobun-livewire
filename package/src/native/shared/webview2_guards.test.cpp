#include "webview2_guards.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
    using electrobun::isAllowedPageUrl;
    using electrobun::isOwnDownloadUrl;
    using electrobun::webView2BrowserArguments;

    // Only what the page made itself is its own download.
    assert(isOwnDownloadUrl("blob:views://internal/5c1c7a0e"));
    assert(isOwnDownloadUrl("data:text/plain;base64,aGk="));
    assert(isOwnDownloadUrl("BLOB:views://internal/5c1c7a0e"));
    assert(isOwnDownloadUrl("Data:,hi"));
    assert(!isOwnDownloadUrl("https://example.com/setup.exe"));
    assert(!isOwnDownloadUrl("http://localhost/blob:"));
    assert(!isOwnDownloadUrl("views://internal/file.zip"));
    assert(!isOwnDownloadUrl("file:///C:/Windows/notepad.exe"));
    assert(!isOwnDownloadUrl("blob"));
    assert(!isOwnDownloadUrl(" data:,hi"));
    // A URL that couldn't be read fails closed.
    assert(!isOwnDownloadUrl(""));

    // A locked view shows its page, with any fragment, and nothing else.
    const std::string page = "views://internal/index.html";
    assert(isAllowedPageUrl(page, "views://internal/index.html"));
    assert(isAllowedPageUrl(page, "views://internal/index.html#"));
    assert(isAllowedPageUrl(page, "views://internal/index.html#/channel/general"));
    assert(!isAllowedPageUrl(page, "views://internal/index.html?next=https://example.com"));
    assert(!isAllowedPageUrl(page, "views://internal/index.htmlx"));
    assert(!isAllowedPageUrl(page, "views://internal/index.html/extra"));
    assert(!isAllowedPageUrl(page, "views://internal/"));
    assert(!isAllowedPageUrl(page, "VIEWS://internal/index.html"));
    assert(!isAllowedPageUrl(page, "https://example.com/#views://internal/index.html"));
    assert(!isAllowedPageUrl(page, "about:blank"));
    assert(!isAllowedPageUrl(page, ""));
    // No page named is no page allowed, never every page.
    assert(!isAllowedPageUrl("", ""));
    assert(!isAllowedPageUrl("", "views://internal/index.html"));

    // Keeping web security drops every switch that weakens the page's sandbox.
    const std::wstring kept = webView2BrowserArguments(true);
    assert(kept.find(L"disable-web-security") == std::wstring::npos);
    assert(kept.find(L"allow-insecure-localhost") == std::wstring::npos);
    assert(kept.find(L"msSmartScreenProtection") == std::wstring::npos);
    assert(kept.find(L"msWebOOUI") != std::wstring::npos);
    assert(kept.find(L"msPdfOOUI") != std::wstring::npos);

    // An app that asked for nothing starts as it always did.
    const std::wstring open = webView2BrowserArguments(false);
    assert(open == L"--disable-features=msWebOOUI,msPdfOOUI,msSmartScreenProtection --allow-insecure-localhost --disable-web-security");

    std::cout << "WebView2 guard rules passed" << std::endl;
    return 0;
}
