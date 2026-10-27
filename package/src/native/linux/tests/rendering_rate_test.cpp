#include "../rendering_rate.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond, msg)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            std::fprintf(stderr, "FAIL: %s\n", msg);       \
            std::exit(1);                                  \
        }                                                  \
    } while (0)

int main()
{
    std::printf("WebKitGTK %u.%u.%u\n", webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
    CHECK(!electrobun::renderAtDisplayRate(nullptr), "no settings must change nothing");
    WebKitSettings* settings = webkit_settings_new();
    CHECK(electrobun::rendersNear60(settings), "WebKitGTK must list the near-60fps feature, on by default");
    CHECK(electrobun::renderAtDisplayRate(settings), "turning the near-60fps feature off must find it");
    CHECK(!electrobun::rendersNear60(settings), "the settings must keep the near-60fps feature off");
    WebKitSettings* fresh = webkit_settings_new();
    CHECK(electrobun::rendersNear60(fresh), "other settings must keep WebKit's default");
    g_object_unref(fresh);
    g_object_unref(settings);
    std::printf("rendering rate: ok\n");
    return 0;
}
