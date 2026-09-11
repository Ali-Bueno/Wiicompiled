#include "accessibility/update_check.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "accessibility/a11y_log.h"
#include "accessibility/http_get.h"
#include "accessibility/localization.h"
#include "accessibility/screen_reader.h"
#include "accessibility/update_version.h"
#include "aurora_events.h"
#include "runtime_config.h"
#include "runtime_product.h"

namespace a11y::update {
namespace {

// Room between the last spoken word and the process ending: PRISM's in-process backends stop with
// the process, so the sentence needs time to be heard. Long enough for the closing phrase, short
// enough not to read as a hang.
constexpr auto kSpeechGraceBeforeClose = std::chrono::seconds(5);

// What the installer is told to do. An installer that predates these flags ignores them and simply
// opens on the root it was given (installer/src/main.rs).
constexpr const wchar_t* kArgRoot = L" --root ";
constexpr const wchar_t* kArgLaunch = L" --launch ";
constexpr const wchar_t* kArgUpdateMod = L" --update-mod";
constexpr const wchar_t* kArgUpdateRetro = L" --update-retro";
constexpr const wchar_t* kArgUpdateAll = L" --update-all";

// Copied on the guest thread: the worker must never touch the config or any other runtime state
// while the game runs.
struct Settings {
    std::string installedVersion;
    std::string latestReleaseUrl;
    std::string retroVersionUrl;
    std::string retroVersionFile;
};

struct Result {
    std::atomic<bool> done{false};
    std::atomic<bool> modOutdated{false};
    std::atomic<bool> retroOutdated{false};
};

enum class Phase { Off, Waiting, AskMod, AskRetro, Closing, Finished };

constexpr int kAnswerNone = 0;
constexpr int kAnswerAccept = 1;
constexpr int kAnswerDecline = 2;

// Shared with the detached worker, which keeps its own state alive whatever the game does.
std::shared_ptr<Result> g_result;
Phase g_phase = Phase::Off;
bool g_modOutdated = false;
bool g_retroOutdated = false;
bool g_acceptedMod = false;
bool g_acceptedRetro = false;
bool g_canOffer = false;
std::chrono::steady_clock::time_point g_closeAt;
std::atomic<bool> g_questionPending{false};
std::atomic<int> g_answer{kAnswerNone};

std::string ReadTextFile(const std::string& path) {
    std::ifstream file(RuntimeConfigFile::PathFromUtf8(path), std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool RemoteIsNewer(const std::string& remoteText, const std::string& localText) {
    const auto remote = ParseVersion(remoteText);
    const auto local = ParseVersion(localText);
    return remote && local && IsNewer(*remote, *local);
}

void Run(Settings settings, std::shared_ptr<Result> result) {
    std::string body;
    if (net::HttpGet(settings.latestReleaseUrl, body)) {
        if (const auto tag = FindJsonString(body, "tag_name")) {
            result->modOutdated.store(RemoteIsNewer(*tag, settings.installedVersion),
                                      std::memory_order_relaxed);
        }
    }
    if (!settings.retroVersionUrl.empty() && !settings.retroVersionFile.empty()) {
        const std::string installed = ReadTextFile(settings.retroVersionFile);
        body.clear();
        if (!installed.empty() && net::HttpGet(settings.retroVersionUrl, body)) {
            if (const auto newest = LastLineFirstToken(body)) {
                result->retroOutdated.store(RemoteIsNewer(*newest, installed),
                                            std::memory_order_relaxed);
            }
        }
    }
    result->done.store(true, std::memory_order_release);
}

std::wstring Quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

std::wstring ExePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return buffer;
}

bool LaunchInstaller() {
    const std::filesystem::path installer =
        RuntimeConfigFile::PathFromUtf8(RuntimeConfigFile::UpdateInstallerPath());
    std::wstring command = Quote(installer.wstring());
    if (const auto& root = RuntimeConfigFile::PortableRootDirectory()) {
        command += kArgRoot + Quote(root->wstring());
    }
    command += g_acceptedMod && g_acceptedRetro ? kArgUpdateAll
               : g_acceptedMod                  ? kArgUpdateMod
                                                : kArgUpdateRetro;
    if (const std::wstring exe = ExePath(); !exe.empty()) {
        command += kArgLaunch + Quote(exe);
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    const std::wstring directory = installer.parent_path().wstring();
    if (!CreateProcessW(installer.c_str(), command.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS, nullptr,
                        directory.empty() ? nullptr : directory.c_str(), &startup, &process)) {
        RT_LOGF(RT_TAG_A11Y, "update: the installer did not start (error %lu)\n", GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void Say(const char* key) {
    // Never interrupting: this lands while the game is talking over its own opening screens.
    ScreenReader::Instance().Speak(loc::Get(key), /*interrupt=*/false);
}

void Ask(const char* key) {
    Say(key);
    g_answer.store(kAnswerNone, std::memory_order_relaxed);
    g_questionPending.store(true, std::memory_order_release);
}

void Decide() {
    if (!g_acceptedMod && !g_acceptedRetro) {
        Say("update_declined");
        g_phase = Phase::Finished;
        return;
    }
    if (!LaunchInstaller()) {
        Say("update_failed");
        g_phase = Phase::Finished;
        return;
    }
    Say("update_starting");
    g_closeAt = std::chrono::steady_clock::now() + kSpeechGraceBeforeClose;
    g_phase = Phase::Closing;
}

void AfterModAnswer() {
    if (g_retroOutdated) {
        g_phase = Phase::AskRetro;
        Ask("update_retro_rewind");
        return;
    }
    Decide();
}

bool TakeAnswer(bool& accepted) {
    const int answer = g_answer.load(std::memory_order_acquire);
    if (answer == kAnswerNone) {
        return false;
    }
    g_questionPending.store(false, std::memory_order_release);
    accepted = answer == kAnswerAccept;
    return true;
}

void OnCheckFinished() {
    g_modOutdated = g_result->modOutdated.load(std::memory_order_relaxed);
    g_retroOutdated = g_result->retroOutdated.load(std::memory_order_relaxed);
    g_result.reset();
    RT_LOGF(RT_TAG_A11Y, "update: check done (mod %s, retro rewind %s)\n",
            g_modOutdated ? "outdated" : "current", g_retroOutdated ? "outdated" : "current");
    if (!g_modOutdated && !g_retroOutdated) {
        g_phase = Phase::Finished;
        return;
    }
    if (!g_canOffer) {
        // No installer recorded: say what is available and leave the player to run it.
        if (g_modOutdated) {
            Say("update_mod_manual");
        }
        if (g_retroOutdated) {
            Say("update_retro_rewind_manual");
        }
        g_phase = Phase::Finished;
        return;
    }
    if (g_modOutdated) {
        g_phase = Phase::AskMod;
        Ask("update_mod");
        return;
    }
    g_phase = Phase::AskRetro;
    Ask("update_retro_rewind");
}

}  // namespace

void Start() {
    if (!RuntimeConfigFile::AccessibilityCheckUpdates()) {
        return;
    }
    Settings settings;
    settings.installedVersion = RuntimeConfigFile::UpdateInstalledVersion();
    settings.latestReleaseUrl = RuntimeConfigFile::UpdateLatestReleaseUrl();
    if (settings.installedVersion.empty() || settings.latestReleaseUrl.empty()) {
        return;  // the installer wrote nothing to compare against
    }
    // The Retro Rewind pack exists only in that product, and only its own keys describe it.
    if (RuntimeProduct::IsRetroRewind()) {
        settings.retroVersionUrl = RuntimeConfigFile::UpdateRetroRewindVersionUrl();
        settings.retroVersionFile = RuntimeConfigFile::UpdateRetroRewindVersionFile();
    }
    g_canOffer = !RuntimeConfigFile::UpdateInstallerPath().empty();
    g_result = std::make_shared<Result>();
    g_phase = Phase::Waiting;
    std::thread(Run, std::move(settings), g_result).detach();
}

void Tick() {
    switch (g_phase) {
        case Phase::Off:
        case Phase::Finished:
            return;
        case Phase::Waiting:
            if (g_result->done.load(std::memory_order_acquire)) {
                OnCheckFinished();
            }
            return;
        case Phase::AskMod:
            if (TakeAnswer(g_acceptedMod)) {
                AfterModAnswer();
            }
            return;
        case Phase::AskRetro:
            if (TakeAnswer(g_acceptedRetro)) {
                Decide();
            }
            return;
        case Phase::Closing:
            if (std::chrono::steady_clock::now() >= g_closeAt) {
                // The runtime's own quit path, written to be safe from a guest fiber.
                ExitForAuroraWindowClose();
            }
            return;
    }
}

bool OnAnswer(bool accept) {
    if (!g_questionPending.load(std::memory_order_acquire)) {
        return false;
    }
    int expected = kAnswerNone;
    return g_answer.compare_exchange_strong(expected, accept ? kAnswerAccept : kAnswerDecline,
                                            std::memory_order_release,
                                            std::memory_order_relaxed);
}

}  // namespace a11y::update
