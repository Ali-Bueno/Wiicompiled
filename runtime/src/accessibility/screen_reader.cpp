#include "screen_reader.h"

#include "prism_runtime.h"
#include "runtime_log.h"

namespace a11y {

ScreenReader& ScreenReader::Instance() {
    static ScreenReader instance;
    return instance;
}

void ScreenReader::Initialise() {
    if (mInitialised) {
        return;
    }
    mInitialised = true;

    mApi = LoadPrism();
    if (mApi == nullptr) {
        return;  // LoadPrism already logged why.
    }

    PrismConfig cfg = mApi->config_init();
    mContext = mApi->init(&cfg);
    if (mContext == nullptr) {
        RT_LOGF(RT_TAG_A11Y, "PRISM context could not be created; running without speech\n");
        return;
    }

    // Dumping the registry is the diagnostic that matters when a user reports "my reader is
    // running but the game is silent": it shows whether the backend is even present.
    const unsigned long count = static_cast<unsigned long>(mApi->registry_count(mContext));
    RT_LOGF(RT_TAG_A11Y, "PRISM registry has %lu backend(s):\n", count);
    for (unsigned long i = 0; i < count; ++i) {
        const PrismBackendId id = mApi->registry_id_at(mContext, i);
        const char* name = mApi->registry_name(mContext, id);
        RT_LOGF(RT_TAG_A11Y, "  %s (priority %d)\n", name != nullptr ? name : "(unnamed)",
                mApi->registry_priority(mContext, id));
    }

    // Deliberately not prism_registry_acquire_best(): it takes the first backend whose
    // initialize() succeeds and never consults get_features(). PRISM's NVDA backend has the
    // highest priority on Windows and its initialize() wrongly succeeds when NVDA is not
    // running, shadowing a JAWS or SAPI that really is live - the user then gets no speech at
    // all. get_features() does its own liveness probe and is reliable, so take the first
    // backend reporting IS_SUPPORTED_AT_RUNTIME. The registry is already ordered by descending
    // priority, so the first live match is also the best one.
    for (unsigned long i = 0; i < count; ++i) {
        const PrismBackendId id = mApi->registry_id_at(mContext, i);

        PrismBackend* probe = mApi->registry_create(mContext, id);
        if (probe == nullptr) {
            continue;
        }
        const bool liveAtRuntime =
            (mApi->backend_get_features(probe) & PRISM_BACKEND_IS_SUPPORTED_AT_RUNTIME) != 0;
        mApi->backend_free(probe);
        if (!liveAtRuntime) {
            continue;
        }

        PrismBackend* backend = mApi->registry_acquire(mContext, id);
        if (backend == nullptr) {
            continue;
        }
        // acquire() can hand back an already-initialised backend; that is success, not failure.
        const PrismError err = mApi->backend_initialize(backend);
        if (err != PRISM_OK && err != PRISM_ERROR_ALREADY_INITIALIZED) {
            RT_LOGF(RT_TAG_A11Y, "backend '%s' failed to initialise: %s\n",
                    mApi->backend_name(backend), mApi->error_string(err));
            continue;
        }

        mBackend = backend;
        const char* name = mApi->backend_name(backend);
        mBackendName = (name != nullptr) ? name : "unnamed";
        RT_LOGF(RT_TAG_A11Y, "screen reader ready: %s\n", mBackendName.c_str());
        return;
    }

    RT_LOGF(RT_TAG_A11Y, "no screen reader reported itself available; running without speech\n");
}

void ScreenReader::Shutdown() {
    if (!mInitialised) {
        return;
    }
    // Acquired backends belong to the context, so shutting the context down frees them.
    if (mApi != nullptr && mContext != nullptr) {
        mApi->shutdown(mContext);
    }
    mContext = nullptr;
    mBackend = nullptr;
    mApi = nullptr;
    mBackendName = "none";
    mInitialised = false;
}

void ScreenReader::Speak(const std::string& text, bool interrupt) {
    if (mBackend == nullptr || text.empty()) {
        return;
    }
    // output() drives speech and braille displays alike, which is what a screen-reader
    // integration wants.
    (void)mApi->backend_output(mBackend, text.c_str(), interrupt);
}

void ScreenReader::Silence() {
    if (mBackend == nullptr) {
        return;
    }
    (void)mApi->backend_stop(mBackend);
}

}  // namespace a11y
