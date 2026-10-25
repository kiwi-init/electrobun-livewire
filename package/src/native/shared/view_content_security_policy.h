// view_content_security_policy.h - Per-webview Content-Security-Policy that the
// views:// and appdata:// scheme handlers send as a response header. The core
// sets it before the view loads anything; an empty policy removes it.

#ifndef ELECTROBUN_VIEW_CONTENT_SECURITY_POLICY_H
#define ELECTROBUN_VIEW_CONTENT_SECURITY_POLICY_H

#include <cstdint>
#include <string>

#include "thread_safe_map.h"

namespace electrobun {

inline ThreadSafeMap<uint32_t, std::string>& viewContentSecurityPolicies() {
    static ThreadSafeMap<uint32_t, std::string> policies;
    return policies;
}

inline void setViewContentSecurityPolicy(uint32_t webviewId, const char* policy) {
    if (policy && *policy) {
        viewContentSecurityPolicies().set(webviewId, std::string(policy));
    } else {
        viewContentSecurityPolicies().remove(webviewId);
    }
}

inline std::string viewContentSecurityPolicy(uint32_t webviewId) {
    return viewContentSecurityPolicies().getOrDefault(webviewId, std::string());
}

}  // namespace electrobun

#endif  // ELECTROBUN_VIEW_CONTENT_SECURITY_POLICY_H
