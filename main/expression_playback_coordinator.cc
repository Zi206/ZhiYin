#include "expression_playback_coordinator.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include <esp_log.h>
#include <esp_timer.h>

namespace {

constexpr char kTag[] = "ExpressionCoordinator";

std::string Normalize(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

}  // namespace

void ExpressionPlaybackCoordinator::SetRenderer(Renderer renderer) {
    renderer_ = std::move(renderer);
    Resolve("renderer_ready");
}

uint32_t ExpressionPlaybackCoordinator::BeginSession(const char* reason) {
    const uint32_t generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    tts_active_ = false;
    ClearSessionCandidates();
    ESP_LOGI(kTag, "New session generation=%u reason=%s",
             static_cast<unsigned>(generation), reason ? reason : "unknown");
    Resolve("new_session");
    return generation;
}

uint32_t ExpressionPlaybackCoordinator::CancelSession(const char* reason) {
    const uint32_t generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    tts_active_ = false;
    ClearSessionCandidates();
    ESP_LOGI(kTag, "Canceled session; generation=%u reason=%s",
             static_cast<unsigned>(generation), reason ? reason : "unknown");
    Resolve("cancel_session");
    return generation;
}

void ExpressionPlaybackCoordinator::OnDeviceState(DeviceState state) {
    state_ = state;
    if (state_ == kDeviceStateIdle && !tts_active_) {
        semantic_.reset();
        hint_.reset();
    }
    Resolve("device_state");
}

void ExpressionPlaybackCoordinator::OnTtsStarted(uint32_t generation) {
    if (generation != CurrentGeneration()) {
        ESP_LOGW(kTag, "Drop stale TTS start generation=%u current=%u",
                 static_cast<unsigned>(generation), static_cast<unsigned>(CurrentGeneration()));
        return;
    }
    tts_active_ = true;
    if (semantic_ && IsCurrent(*semantic_) && semantic_->expression == ExpressionId::kSleepy) {
        action_ = Candidate{ExpressionId::kSleepy, ExpressionSource::kLlmSemantic,
                            generation, 0, semantic_->raw};
    }
    Resolve("tts_start");
}

void ExpressionPlaybackCoordinator::OnTtsStopped(uint32_t generation) {
    if (generation != CurrentGeneration()) {
        ESP_LOGW(kTag, "Drop stale TTS stop generation=%u current=%u",
                 static_cast<unsigned>(generation), static_cast<unsigned>(CurrentGeneration()));
        return;
    }
    tts_active_ = false;
    semantic_.reset();
    hint_.reset();
    Resolve("tts_stop");
}

void ExpressionPlaybackCoordinator::OnLlmEmotion(const std::string& raw_emotion,
                                                   uint32_t generation) {
    if (generation != CurrentGeneration()) {
        ESP_LOGW(kTag, "Drop stale LLM emotion raw=%s generation=%u current=%u",
                 raw_emotion.c_str(), static_cast<unsigned>(generation),
                 static_cast<unsigned>(CurrentGeneration()));
        return;
    }

    const auto parsed = Parse(raw_emotion);
    if (!parsed.has_value()) {
        if (last_unknown_emotion_ != raw_emotion) {
            ESP_LOGW(kTag, "Unknown emotion raw=%s; using state fallback", raw_emotion.c_str());
            last_unknown_emotion_ = raw_emotion;
        }
        semantic_.reset();
        Resolve("unknown_llm_emotion");
        return;
    }

    last_unknown_emotion_.clear();
    semantic_ = Candidate{*parsed, ExpressionSource::kLlmSemantic, generation, 0, raw_emotion};
    hint_.reset();

    // Sleep is intentionally latched until the next active conversation. The
    // MJPEG player renders one loop and keeps its final frame.
    if (*parsed == ExpressionId::kSleepy &&
        (tts_active_ || state_ == kDeviceStateSpeaking)) {
        action_ = Candidate{*parsed, ExpressionSource::kLlmSemantic, generation, 0, raw_emotion};
    }

    ESP_LOGI(kTag, "LLM emotion raw=%s parsed=%s generation=%u tts=%d",
             raw_emotion.c_str(), Name(*parsed), static_cast<unsigned>(generation), tts_active_);
    Resolve("llm_emotion");
}

void ExpressionPlaybackCoordinator::OnExplicitAction(ExpressionId expression, const char* reason,
                                                       uint32_t generation, int64_t duration_ms) {
    if (generation != CurrentGeneration()) {
        ESP_LOGW(kTag, "Drop stale action=%s generation=%u current=%u",
                 Name(expression), static_cast<unsigned>(generation),
                 static_cast<unsigned>(CurrentGeneration()));
        return;
    }
    const int64_t expires_at = duration_ms > 0 ? NowMs() + duration_ms : 0;
    action_ = Candidate{expression, ExpressionSource::kExplicitAction, generation,
                        expires_at, reason ? reason : "explicit"};
    ESP_LOGI(kTag, "Action expression=%s generation=%u duration_ms=%lld reason=%s",
             Name(expression), static_cast<unsigned>(generation),
             static_cast<long long>(duration_ms), reason ? reason : "explicit");
    Resolve("explicit_action");
}

void ExpressionPlaybackCoordinator::OnTemporaryHint(ExpressionId expression, const char* reason,
                                                      uint32_t generation, int64_t duration_ms) {
    if (generation != CurrentGeneration()) {
        ESP_LOGW(kTag, "Drop stale local hint=%s generation=%u current=%u",
                 Name(expression), static_cast<unsigned>(generation),
                 static_cast<unsigned>(CurrentGeneration()));
        return;
    }
    const int64_t expires_at = duration_ms > 0 ? NowMs() + duration_ms : 0;
    hint_ = Candidate{expression, ExpressionSource::kLocalHint, generation,
                      expires_at, reason ? reason : "local_hint"};
    Resolve("local_hint");
}

void ExpressionPlaybackCoordinator::ShowAlert(const std::string& raw_emotion) {
    auto parsed = Parse(raw_emotion);
    if (!parsed.has_value()) {
        parsed = ExpressionId::kSurprised;
        ESP_LOGW(kTag, "Alert icon/emotion raw=%s mapped to surprised", raw_emotion.c_str());
    }
    alert_ = Candidate{*parsed, ExpressionSource::kAlert, CurrentGeneration(), 0, raw_emotion};
    Resolve("alert_open");
}

void ExpressionPlaybackCoordinator::DismissAlert() {
    alert_.reset();
    Resolve("alert_close");
}

void ExpressionPlaybackCoordinator::Tick() {
    ExpireCandidates();
    Resolve("tick");
}

const char* ExpressionPlaybackCoordinator::Name(ExpressionId expression) {
    switch (expression) {
        case ExpressionId::kNeutral: return "neutral";
        case ExpressionId::kListening: return "listening";
        case ExpressionId::kSpeaking: return "speaking";
        case ExpressionId::kHappy: return "happy";
        case ExpressionId::kSurprised: return "surprised";
        case ExpressionId::kSad: return "sad";
        case ExpressionId::kAngry: return "angry";
        case ExpressionId::kSleepy: return "sleepy";
        case ExpressionId::kDancing: return "dancing";
    }
    return "neutral";
}

const char* ExpressionPlaybackCoordinator::SourceName(ExpressionSource source) {
    switch (source) {
        case ExpressionSource::kDeviceState: return "device_state";
        case ExpressionSource::kLocalHint: return "local_hint";
        case ExpressionSource::kLlmSemantic: return "llm_semantic";
        case ExpressionSource::kExplicitAction: return "explicit_action";
        case ExpressionSource::kAlert: return "alert";
    }
    return "unknown";
}

std::optional<ExpressionId> ExpressionPlaybackCoordinator::Parse(const std::string& raw) {
    const std::string value = Normalize(raw);
    if (value == "neutral" || value == "relaxed" || value == "confident") {
        return ExpressionId::kNeutral;
    }
    if (value == "listening") return ExpressionId::kListening;
    if (value == "speaking" || value == "thinking") return ExpressionId::kSpeaking;
    if (value == "happy" || value == "funny" || value == "laughing" ||
        value == "delicious" || value == "winking" || value == "silly" ||
        value == "cool" || value == "kissy" || value == "loving" || value == "excited") {
        return ExpressionId::kHappy;
    }
    if (value == "surprised" || value == "shocked" || value == "confused") {
        return ExpressionId::kSurprised;
    }
    if (value == "sad" || value == "crying" || value == "embarrassed") {
        return ExpressionId::kSad;
    }
    if (value == "angry") return ExpressionId::kAngry;
    if (value == "sleep" || value == "sleepy") return ExpressionId::kSleepy;
    if (value == "dancing") return ExpressionId::kDancing;
    return std::nullopt;
}

ExpressionId ExpressionPlaybackCoordinator::DefaultForState(DeviceState state) {
    switch (state) {
        case kDeviceStateListening: return ExpressionId::kListening;
        case kDeviceStateSpeaking: return ExpressionId::kSpeaking;
        case kDeviceStateMusicPlaying: return ExpressionId::kDancing;
        default: return ExpressionId::kNeutral;
    }
}

int64_t ExpressionPlaybackCoordinator::NowMs() {
    return esp_timer_get_time() / 1000;
}

bool ExpressionPlaybackCoordinator::IsCurrent(const Candidate& candidate) const {
    return candidate.generation == CurrentGeneration();
}

void ExpressionPlaybackCoordinator::ClearSessionCandidates() {
    semantic_.reset();
    action_.reset();
    hint_.reset();
}

void ExpressionPlaybackCoordinator::ExpireCandidates() {
    const int64_t now = NowMs();
    auto expire = [now](std::optional<Candidate>& candidate, const char* kind) {
        if (candidate && candidate->expires_at_ms > 0 && now >= candidate->expires_at_ms) {
            ESP_LOGI(kTag, "Expired %s expression=%s generation=%u", kind,
                     ExpressionPlaybackCoordinator::Name(candidate->expression),
                     static_cast<unsigned>(candidate->generation));
            candidate.reset();
        }
    };
    expire(action_, "action");
    expire(hint_, "hint");
}

void ExpressionPlaybackCoordinator::Resolve(const char* reason) {
    ExpireCandidates();
    const ExpressionId state_default = tts_active_ ? ExpressionId::kSpeaking : DefaultForState(state_);
    Candidate desired{state_default, ExpressionSource::kDeviceState,
                      CurrentGeneration(), 0, "state"};

    if (alert_) {
        desired = *alert_;
    } else if (state_ == kDeviceStateMusicPlaying) {
        desired = Candidate{ExpressionId::kDancing, ExpressionSource::kDeviceState,
                            CurrentGeneration(), 0, "music"};
    } else if (action_ && IsCurrent(*action_)) {
        desired = *action_;
    } else if ((tts_active_ || state_ == kDeviceStateSpeaking) &&
               semantic_ && IsCurrent(*semantic_)) {
        desired = *semantic_;
    } else if (hint_ && IsCurrent(*hint_)) {
        desired = *hint_;
    }

    const bool changed = !rendered_ || rendered_->expression != desired.expression;
    const bool metadata_changed = !rendered_ || rendered_->source != desired.source ||
                                  rendered_->generation != desired.generation;
    if (changed || metadata_changed) {
        ESP_LOGI(kTag,
                 "Resolve reason=%s source=%s raw=%s result=%s generation=%u state=%d tts=%d",
                 reason ? reason : "unknown", SourceName(desired.source), desired.raw.c_str(),
                 Name(desired.expression), static_cast<unsigned>(desired.generation),
                 static_cast<int>(state_), tts_active_);
    }
    if (changed && renderer_) {
        renderer_(Name(desired.expression));
    }
    rendered_ = desired;
}
