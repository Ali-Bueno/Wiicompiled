#ifndef MKW_ACCESSIBILITY_PRISM_RUNTIME_H
#define MKW_ACCESSIBILITY_PRISM_RUNTIME_H

// prism.dll is bound at runtime rather than linked: the shipped import library is MSVC-only and
// this runtime is built with LLVM-MinGW. PRISM's surface is a C API, so GetProcAddress is
// ABI-clean across the two. Speech is optional - a missing DLL must not stop the game.

// We resolve every symbol ourselves, so suppress the dllimport decoration on the declarations.
#define PRISM_STATIC
// The workspace root is already on the include path (PublicProducts.cmake), so no CMake change
// is needed to reach the vendored header.
#include "prism/include/prism.h"

#include <cstddef>
#include <cstdint>

namespace a11y {

// Only the entry points the mod actually uses.
struct PrismApi {
    PrismConfig    (PRISM_CALL* config_init)(void);
    PrismContext*  (PRISM_CALL* init)(PrismConfig*);
    void           (PRISM_CALL* shutdown)(PrismContext*);
    std::size_t    (PRISM_CALL* registry_count)(PrismContext*);
    PrismBackendId (PRISM_CALL* registry_id_at)(PrismContext*, std::size_t);
    int            (PRISM_CALL* registry_priority)(PrismContext*, PrismBackendId);
    const char*    (PRISM_CALL* registry_name)(PrismContext*, PrismBackendId);
    // create() hands out a throwaway instance we must free; acquire() hands out one the
    // context owns and frees on shutdown.
    PrismBackend*  (PRISM_CALL* registry_create)(PrismContext*, PrismBackendId);
    PrismBackend*  (PRISM_CALL* registry_acquire)(PrismContext*, PrismBackendId);
    void           (PRISM_CALL* backend_free)(PrismBackend*);
    const char*    (PRISM_CALL* backend_name)(PrismBackend*);
    std::uint64_t  (PRISM_CALL* backend_get_features)(PrismBackend*);
    PrismError     (PRISM_CALL* backend_initialize)(PrismBackend*);
    PrismError     (PRISM_CALL* backend_output)(PrismBackend*, const char*, bool);
    PrismError     (PRISM_CALL* backend_stop)(PrismBackend*);
    const char*    (PRISM_CALL* error_string)(PrismError);
};

// Loads prism.dll from the executable's directory. Returns nullptr if it is missing or if any
// required export is absent. Idempotent.
const PrismApi* LoadPrism();

void UnloadPrism();

}  // namespace a11y

#endif  // MKW_ACCESSIBILITY_PRISM_RUNTIME_H
