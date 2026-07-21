#pragma once

#include "device_state.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

enum class ExpressionId : uint8_t {
    kNeutral,
    kListening,
    kSpeaking,
    kHappy,
    kSurprised,
    kSad,
    kAngry,
    kSleepy,
    kDancing,
};

enum class ExpressionSource : uint8_t {
    kDeviceState,
    kLocalHint,
    kLlmSemantic,
    kExplicitAction,
    kAlert,
};

// Owns expression arbitration. All mutating methods are called by Application's
// main task; only CurrentGeneration() is read from protocol callback tasks.
class ExpressionPlaybackCoordinator {
public:
    using Renderer = std::function<void(const char*)>;

    void SetRenderer(Renderer renderer);

    uint32_t BeginSession(const char* reason);
    uint32_t CancelSession(const char* reason);
    uint32_t CurrentGeneration() const {
        return generation_.load(std::memory_order_acquire);
    }

    void OnDeviceState(DeviceState state);
    void OnTtsStarted(uint32_t generation);
    void OnTtsStopped(uint32_t generation);
    void OnLlmEmotion(const std::string& raw_emotion, uint32_t generation);
    void OnExplicitAction(ExpressionId expression, const char* reason,
                          uint32_t generation, int64_t duration_ms = 0);
    void OnTemporaryHint(ExpressionId expression, const char* reason,
                         uint32_t generation, int64_t duration_ms = 8000);
    void ShowAlert(const std::string& raw_emotion);
    void DismissAlert();
    void Tick();

    static const char* Name(ExpressionId expression);
    static const char* SourceName(ExpressionSource source);

private:
    struct Candidate {
        ExpressionId expression = ExpressionId::kNeutral;
        ExpressionSource source = ExpressionSource::kDeviceState;
        uint32_t generation = 0;
        int64_t expires_at_ms = 0;
        std::string raw;
    };

    Renderer renderer_;
    std::atomic<uint32_t> generation_{1};
    DeviceState state_ = kDeviceStateStarting;
    bool tts_active_ = false;
    std::optional<Candidate> semantic_;
    std::optional<Candidate> action_;
    std::optional<Candidate> hint_;
    std::optional<Candidate> alert_;
    std::optional<Candidate> rendered_;
    std::string last_unknown_emotion_;

    static std::optional<ExpressionId> Parse(const std::string& raw);
    static ExpressionId DefaultForState(DeviceState state);
    static int64_t NowMs();
    bool IsCurrent(const Candidate& candidate) const;
    void ClearSessionCandidates();
    void ExpireCandidates();
    void Resolve(const char* reason);
};
