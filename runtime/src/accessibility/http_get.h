#ifndef MKW_ACCESSIBILITY_HTTP_GET_H
#define MKW_ACCESSIBILITY_HTTP_GET_H

#include <string>

namespace a11y::net {

// One blocking HTTPS GET, bounded by short timeouts. Never throws; false means "no answer", with
// `body` left empty. Only ever called from a background thread.
bool HttpGet(const std::string& url, std::string& body);

}  // namespace a11y::net

#endif  // MKW_ACCESSIBILITY_HTTP_GET_H
