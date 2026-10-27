#pragma once

#include <webkit2/webkit2.h>

namespace electrobun {

inline constexpr const char* kNear60Feature = "PreferPageRenderingUpdatesNear60FPSEnabled";

// WebKit paces page rendering (requestAnimationFrame, style, layout, paint)
// near 60 fps on displays faster than 60 Hz: "Prefer Page Rendering Updates
// near 60fps", on by default in every port. WebKitGTK 2.42+ lists it among its
// features; turn it off so pages keep up with the monitor. Older WebKitGTK has
// no feature API and keeps its default.
inline bool renderAtDisplayRate(WebKitSettings* settings)
{
#if WEBKIT_CHECK_VERSION(2, 42, 0)
    if (!settings)
        return false;
    WebKitFeatureList* features = webkit_settings_get_all_features();
    bool found = false;
    for (gsize i = 0; i < webkit_feature_list_get_length(features); i++) {
        WebKitFeature* feature = webkit_feature_list_get(features, i);
        if (g_strcmp0(webkit_feature_get_identifier(feature), kNear60Feature) == 0) {
            webkit_settings_set_feature_enabled(settings, feature, FALSE);
            found = true;
            break;
        }
    }
    webkit_feature_list_unref(features);
    return found;
#else
    (void)settings;
    return false;
#endif
}

inline bool rendersNear60(WebKitSettings* settings)
{
#if WEBKIT_CHECK_VERSION(2, 42, 0)
    if (!settings)
        return true;
    WebKitFeatureList* features = webkit_settings_get_all_features();
    bool near60 = true;
    for (gsize i = 0; i < webkit_feature_list_get_length(features); i++) {
        WebKitFeature* feature = webkit_feature_list_get(features, i);
        if (g_strcmp0(webkit_feature_get_identifier(feature), kNear60Feature) == 0) {
            near60 = webkit_settings_get_feature_enabled(settings, feature);
            break;
        }
    }
    webkit_feature_list_unref(features);
    return near60;
#else
    (void)settings;
    return true;
#endif
}

} // namespace electrobun
