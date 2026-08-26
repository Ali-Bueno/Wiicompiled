#include "prism_runtime.h"

#include <windows.h>

#include <cwchar>
#include <string>

#include "accessibility/a11y_log.h"

namespace a11y {
namespace {

HMODULE g_module = nullptr;
PrismApi g_api{};
bool g_loaded = false;
bool g_attempted = false;

// Resolves one export into `slot`, reporting the first one that is missing.
template <typename Fn>
bool Bind(Fn& slot, const char* name) {
    slot = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(::GetProcAddress(g_module, name)));
    if (slot == nullptr) {
        RT_LOGF(RT_TAG_A11Y, "prism.dll is missing export '%s'; speech disabled\n", name);
        return false;
    }
    return true;
}

}  // namespace

const PrismApi* LoadPrism() {
    if (g_attempted) {
        return g_loaded ? &g_api : nullptr;
    }
    g_attempted = true;

    // Load from the executable's own directory only, so a stray prism.dll elsewhere on the
    // search path can never be picked up instead.
    wchar_t exePath[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        RT_LOGF(RT_TAG_A11Y, "could not resolve the executable path; speech disabled\n");
        return nullptr;
    }
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash == nullptr) {
        RT_LOGF(RT_TAG_A11Y, "unexpected executable path layout; speech disabled\n");
        return nullptr;
    }
    *(lastSlash + 1) = L'\0';

    std::wstring dllPath(exePath);
    dllPath += L"prism.dll";

    g_module = ::LoadLibraryW(dllPath.c_str());
    if (g_module == nullptr) {
        RT_LOGF(RT_TAG_A11Y,
                "prism.dll not found next to the executable (error %lu); running without speech\n",
                ::GetLastError());
        return nullptr;
    }

    const bool ok =
        Bind(g_api.config_init, "prism_config_init") &&
        Bind(g_api.init, "prism_init") &&
        Bind(g_api.shutdown, "prism_shutdown") &&
        Bind(g_api.registry_count, "prism_registry_count") &&
        Bind(g_api.registry_id_at, "prism_registry_id_at") &&
        Bind(g_api.registry_priority, "prism_registry_priority") &&
        Bind(g_api.registry_name, "prism_registry_name") &&
        Bind(g_api.registry_create, "prism_registry_create") &&
        Bind(g_api.registry_acquire, "prism_registry_acquire") &&
        Bind(g_api.backend_free, "prism_backend_free") &&
        Bind(g_api.backend_name, "prism_backend_name") &&
        Bind(g_api.backend_get_features, "prism_backend_get_features") &&
        Bind(g_api.backend_initialize, "prism_backend_initialize") &&
        Bind(g_api.backend_output, "prism_backend_output") &&
        Bind(g_api.backend_stop, "prism_backend_stop") &&
        Bind(g_api.error_string, "prism_error_string");

    if (!ok) {
        ::FreeLibrary(g_module);
        g_module = nullptr;
        g_api = PrismApi{};
        return nullptr;
    }

    g_loaded = true;
    return &g_api;
}

void UnloadPrism() {
    if (g_module != nullptr) {
        ::FreeLibrary(g_module);
        g_module = nullptr;
    }
    g_api = PrismApi{};
    g_loaded = false;
    g_attempted = false;
}

}  // namespace a11y
