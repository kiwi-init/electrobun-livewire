#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <cstdio>

#include "rendering_rate.h"

static int failures = 0;

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "FAIL: %s\n", message); \
            ++failures; \
        } \
    } while (false)

static void testGuards()
{
    CHECK(!electrobun::renderAtDisplayRate(nil), "nil preferences must be refused");
    CHECK(electrobun::rendersNear60(nil), "nil preferences must read as the WebKit default");
}

static void testInstalledWebKit()
{
    CHECK(electrobun::near60Feature() != nil, "installed WebKit must list PreferPageRenderingUpdatesNear60FPSEnabled in +[WKPreferences _features]");

    WKPreferences *untouched = [[WKPreferences alloc] init];
    if (!electrobun::rendersNear60(untouched))
        std::printf("NOTE: this WebKit already renders at the display rate by default\n");

    WKWebViewConfiguration *configuration = [[WKWebViewConfiguration alloc] init];
    configuration.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
    CHECK(electrobun::renderAtDisplayRate(configuration.preferences), "the feature SPI must accept the change");
    CHECK(!electrobun::rendersNear60(configuration.preferences), "the preferences must read back as rendering at the display rate");

    [NSApplication sharedApplication];
    WKWebView *webView = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 320, 240) configuration:configuration];
    CHECK(!electrobun::rendersNear60(webView.configuration.preferences), "a WKWebView made from the configuration must keep rendering at the display rate");
}

int main()
{
    @autoreleasepool {
        testGuards();
        testInstalledWebKit();
    }

    if (failures == 0) {
        std::printf("PASS: macOS rendering rate native tests\n");
        return 0;
    }
    return 1;
}
