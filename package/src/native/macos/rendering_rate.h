#pragma once

#import <WebKit/WebKit.h>
#import <objc/message.h>

namespace electrobun {

inline NSString *near60FeatureKey()
{
    return @"PreferPageRenderingUpdatesNear60FPSEnabled";
}

inline id near60Feature()
{
    static id found = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        SEL features = NSSelectorFromString(@"_features");
        if (![WKPreferences respondsToSelector:features])
            return;
        NSArray *all = reinterpret_cast<NSArray *(*)(id, SEL)>(objc_msgSend)([WKPreferences class], features);
        for (id feature in all) {
            if ([feature respondsToSelector:NSSelectorFromString(@"key")] && [[feature valueForKey:@"key"] isEqual:near60FeatureKey()]) {
                found = feature;
                return;
            }
        }
    });
    return found;
}

// WebKit paces page rendering (requestAnimationFrame, style, layout, paint)
// near 60 fps even on 120 Hz displays: Safari's "Prefer Page Rendering Updates
// near 60fps" feature flag, on by default. An app should keep up with its
// display, so turn it off. There's no public API: this is WKPreferences'
// feature SPI, guarded so a WebKit without it is detectable and harmless.
inline bool renderAtDisplayRate(WKPreferences *preferences)
{
    SEL setEnabled = NSSelectorFromString(@"_setEnabled:forFeature:");
    id feature = near60Feature();
    if (!preferences || !feature || ![preferences respondsToSelector:setEnabled])
        return false;
    reinterpret_cast<void (*)(id, SEL, BOOL, id)>(objc_msgSend)(preferences, setEnabled, NO, feature);
    return true;
}

inline bool rendersNear60(WKPreferences *preferences)
{
    SEL isEnabled = NSSelectorFromString(@"_isEnabledForFeature:");
    id feature = near60Feature();
    if (!preferences || !feature || ![preferences respondsToSelector:isEnabled])
        return true;
    return reinterpret_cast<BOOL (*)(id, SEL, id)>(objc_msgSend)(preferences, isEnabled, feature);
}

} // namespace electrobun
