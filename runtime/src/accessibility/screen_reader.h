#ifndef MKW_ACCESSIBILITY_SCREEN_READER_H
#define MKW_ACCESSIBILITY_SCREEN_READER_H

#include <string>

struct PrismContext;
struct PrismBackend;

namespace a11y {

struct PrismApi;

// The single sink for every spoken string in the mod. Nothing else may call PRISM directly, so
// the backend can be swapped in one place.
class ScreenReader {
public:
    static ScreenReader& Instance();

    // Safe when prism.dll is absent or no reader is running: Available() stays false and speech
    // becomes a no-op. Audio cues must never depend on this.
    void Initialise();
    void Shutdown();

    bool Available() const { return mBackend != nullptr; }
    const char* BackendName() const { return mBackendName.c_str(); }

    // `interrupt` cuts whatever the reader is currently saying.
    void Speak(const std::string& text, bool interrupt = true);
    void Silence();

private:
    ScreenReader() = default;

    const PrismApi* mApi = nullptr;
    PrismContext* mContext = nullptr;
    PrismBackend* mBackend = nullptr;
    std::string mBackendName = "none";
    bool mInitialised = false;
};

}  // namespace a11y

#endif  // MKW_ACCESSIBILITY_SCREEN_READER_H
