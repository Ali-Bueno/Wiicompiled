#ifndef MKW_ACCESSIBILITY_A11Y_LOG_H
#define MKW_ACCESSIBILITY_A11Y_LOG_H

#include "runtime_log.h"

// RT_LOGF pastes the tag straight into a string literal ("[" tag "] "), so this has to stay a macro
// expanding to a literal - a constexpr string would not concatenate and fails with a syntax error at
// every call site.
#define RT_TAG_A11Y "accessibility"

#endif  // MKW_ACCESSIBILITY_A11Y_LOG_H
