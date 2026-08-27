#ifndef MKW_ACCESSIBILITY_ACCESSIBILITY_H
#define MKW_ACCESSIBILITY_ACCESSIBILITY_H

// Entry points the runtime calls. Everything else in the mod hangs off these, so the
// upstream call sites stay one-liners and survive rebases.

struct AuroraEvent;

namespace a11y {

// From RuntimeMain, after the settings/audio/video stack is up and before the game starts.
void Init();

// Once per presented frame, from the GXCopyDisp hook. Runs on the guest thread with a live
// CpuContext, so guest memory is readable here. Must stay cheap: dispatch only.
void Tick();

// From ProcessAuroraEvents, on the host event thread. Only queues menu actions; nothing here
// touches guest memory or speaks.
void HandleEvents(const AuroraEvent* events) noexcept;

void Shutdown();

}  // namespace a11y

#endif  // MKW_ACCESSIBILITY_ACCESSIBILITY_H
