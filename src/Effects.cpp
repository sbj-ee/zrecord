#include "Effects.h"

#include <algorithm>
#include <cmath>

namespace zrecord {

namespace {

// The ranges and defaults are the old live filter panel's, so a stack set up
// like the panel sounds like it did.
const EffectInfo kEffects[kEffectTypeCount] = {
    {EffectType::Gain, "gain", "Gain", 1, {{"gainDb", "Gain", "dB", -24.0, 24.0, 0.0, 1}}},
    {EffectType::HighPass, "highPass", "High-pass", 1, {{"cutoffHz", "Cutoff", "Hz", 20.0, 2000.0, 100.0, 0}}},
    {EffectType::LowPass, "lowPass", "Low-pass", 1, {{"cutoffHz", "Cutoff", "Hz", 200.0, 20000.0, 8000.0, 0}}},
    {EffectType::NoiseGate,
     "noiseGate",
     "Noise gate",
     3,
     {{"thresholdDb", "Threshold", "dB", -80.0, 0.0, -40.0, 0},
      {"attackMs", "Attack", "ms", 1.0, 200.0, 5.0, 0},
      {"releaseMs", "Release", "ms", 10.0, 1000.0, 80.0, 0}}},
    {EffectType::Compressor,
     "compressor",
     "Compressor",
     2,
     {{"thresholdDb", "Threshold", "dB", -60.0, 0.0, -20.0, 0}, {"ratio", "Ratio", ":1", 1.0, 10.0, 3.0, 1}}},
    {EffectType::Robot, "robot", "Robot Voice", 1, {{"carrierHz", "Carrier", "Hz", 10.0, 200.0, 30.0, 0}}},
    {EffectType::Echo,
     "echo",
     "Echo",
     3,
     {{"delayMs", "Delay", "ms", 20.0, EffectProcessor::kMaxEchoDelayMs, 280.0, 0},
      {"feedback", "Feedback", "", 0.0, 0.9, 0.35, 2},
      {"mix", "Mix", "", 0.0, 1.0, 0.5, 2}}},
    {EffectType::DeepVoice, "deepVoice", "Deep Voice", 0, {}},
    {EffectType::Chipmunk, "chipmunk", "Chipmunk", 0, {}},
    {EffectType::Distortion, "distortion", "Distortion", 1, {{"drive", "Drive", "", 1.0, 20.0, 6.0, 1}}},
    {EffectType::Limiter, "limiter", "Limiter", 1, {{"ceilingDb", "Ceiling", "dB", -12.0, 0.0, -1.0, 1}}},
};

} // namespace

const EffectInfo& effectInfo(EffectType type) {
    return kEffects[std::clamp(static_cast<int>(type), 0, kEffectTypeCount - 1)];
}

bool effectTypeFromKey(const std::string& key, EffectType& type) {
    for (const EffectInfo& info : kEffects) {
        if (key == info.key) {
            type = info.type;
            return true;
        }
    }
    return false;
}

Effect Effect::make(EffectType type) {
    Effect effect;
    effect.type = type;
    const EffectInfo& info = effectInfo(type);
    for (int i = 0; i < info.paramCount; ++i) {
        effect.params[static_cast<size_t>(i)] = info.params[i].defaultValue;
    }
    return effect;
}

void Effect::clampParams() {
    const EffectInfo& info = effectInfo(type);
    for (int i = 0; i < kMaxEffectParams; ++i) {
        double& p = params[static_cast<size_t>(i)];
        if (i >= info.paramCount) {
            p = 0.0;
        } else if (!std::isfinite(p)) {
            p = info.params[i].defaultValue;
        } else {
            p = std::clamp(p, info.params[i].minValue, info.params[i].maxValue);
        }
    }
}

bool anyEffectActive(const std::vector<Effect>& effects) {
    return std::any_of(effects.begin(), effects.end(), [](const Effect& e) { return !e.bypassed; });
}

std::string describeEffects(const std::vector<Effect>& effects) {
    std::string text;
    for (const Effect& e : effects) {
        if (!text.empty()) text += ", ";
        const std::string name = effectInfo(e.type).name;
        text += e.bypassed ? "(" + name + ")" : name;
    }
    return text;
}

void EffectProcessor::prepare(const Effect& effect, double sampleRate, int channels) {
    effect_ = effect;
    effect_.clampParams();
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    channels_ = std::max(1, channels);
    const auto n = static_cast<size_t>(channels_);
    biquads_.clear();
    gates_.clear();
    compressors_.clear();
    limiters_.clear();
    ringMods_.clear();
    echoes_.clear();
    pitchShifters_.clear();
    // Only the chosen effect's per-channel state is allocated.
    switch (effect_.type) {
        case EffectType::HighPass:
        case EffectType::LowPass: biquads_.assign(n, Biquad{}); break;
        case EffectType::NoiseGate: gates_.assign(n, NoiseGate{}); break;
        case EffectType::Compressor: compressors_.assign(n, Compressor{}); break;
        case EffectType::Limiter: limiters_.assign(n, Limiter{}); break;
        case EffectType::Robot: ringMods_.assign(n, RingModulator{}); break;
        case EffectType::Echo:
            echoes_.assign(n, EchoEffect{});
            for (EchoEffect& echo : echoes_) echo.reserve(sampleRate_, kMaxEchoDelayMs);
            break;
        case EffectType::DeepVoice:
        case EffectType::Chipmunk: pitchShifters_.assign(n, PitchShifter{}); break;
        case EffectType::Gain:
        case EffectType::Distortion: break;
    }
    configure();
    reset();
}

void EffectProcessor::apply(const Effect& effect) {
    if (effect.type != effect_.type) {
        return; // a different effect needs prepare(); never allocate here
    }
    const bool wasBypassed = effect_.bypassed;
    effect_ = effect;
    effect_.clampParams();
    configure();
    if (wasBypassed && !effect_.bypassed) {
        reset(); // its state is from whenever it last ran
    }
}

void EffectProcessor::configure() {
    const double p0 = effect_.params[0], p1 = effect_.params[1], p2 = effect_.params[2];
    switch (effect_.type) {
        case EffectType::Gain: gain_ = static_cast<float>(std::pow(10.0, p0 / 20.0)); break;
        case EffectType::HighPass:
            for (Biquad& b : biquads_) b.configure(Biquad::Type::HighPass, sampleRate_, p0);
            break;
        case EffectType::LowPass:
            for (Biquad& b : biquads_) b.configure(Biquad::Type::LowPass, sampleRate_, p0);
            break;
        case EffectType::NoiseGate:
            for (NoiseGate& g : gates_) g.configure(sampleRate_, p0, p1, p2);
            break;
        case EffectType::Compressor:
            for (Compressor& c : compressors_) c.configure(sampleRate_, p0, p1, 10.0, 150.0);
            break;
        case EffectType::Limiter:
            for (Limiter& l : limiters_) l.configure(sampleRate_, p0, 50.0);
            break;
        case EffectType::Robot:
            for (RingModulator& r : ringMods_) r.configure(sampleRate_, p0);
            break;
        case EffectType::Echo:
            for (EchoEffect& e : echoes_) e.configure(sampleRate_, p0, p1, p2);
            break;
        case EffectType::DeepVoice:
            for (PitchShifter& p : pitchShifters_) p.configure(0.75);
            break;
        case EffectType::Chipmunk:
            for (PitchShifter& p : pitchShifters_) p.configure(1.5);
            break;
        case EffectType::Distortion: distortion_.configure(p0); break;
    }
}

void EffectProcessor::reset() {
    for (Biquad& b : biquads_) b.reset();
    for (NoiseGate& g : gates_) g.reset();
    for (Compressor& c : compressors_) c.reset();
    for (Limiter& l : limiters_) l.reset();
    for (RingModulator& r : ringMods_) r.reset();
    for (EchoEffect& e : echoes_) e.reset();
    for (PitchShifter& p : pitchShifters_) p.reset();
}

namespace {
template <typename Stage>
void perChannel(std::vector<Stage>& stages, float* x, size_t frames, int channels) {
    for (size_t f = 0; f < frames; ++f) {
        float* frame = x + f * static_cast<size_t>(channels);
        for (int c = 0; c < channels; ++c) {
            frame[c] = stages[static_cast<size_t>(c)].process(frame[c]);
        }
    }
}
} // namespace

void EffectProcessor::process(float* x, size_t frames) {
    if (effect_.bypassed) {
        return;
    }
    const size_t count = frames * static_cast<size_t>(channels_);
    switch (effect_.type) {
        case EffectType::Gain:
            for (size_t i = 0; i < count; ++i) x[i] *= gain_;
            break;
        case EffectType::HighPass:
        case EffectType::LowPass: perChannel(biquads_, x, frames, channels_); break;
        case EffectType::NoiseGate: perChannel(gates_, x, frames, channels_); break;
        case EffectType::Compressor: perChannel(compressors_, x, frames, channels_); break;
        case EffectType::Limiter: perChannel(limiters_, x, frames, channels_); break;
        case EffectType::Robot: perChannel(ringMods_, x, frames, channels_); break;
        case EffectType::Echo: perChannel(echoes_, x, frames, channels_); break;
        case EffectType::DeepVoice:
        case EffectType::Chipmunk: perChannel(pitchShifters_, x, frames, channels_); break;
        case EffectType::Distortion:
            for (size_t i = 0; i < count; ++i) x[i] = distortion_.process(x[i]);
            break;
    }
}

void EffectStack::prepare(const std::vector<Effect>& effects, double sampleRate, int channels) {
    processors_.assign(effects.size(), EffectProcessor{});
    shared_ = std::make_unique<SharedParams[]>(effects.size());
    types_.clear();
    for (size_t i = 0; i < effects.size(); ++i) {
        processors_[i].prepare(effects[i], sampleRate, channels);
        types_.push_back(effects[i].type);
    }
    publish(effects);
    applied_ = version_.load(std::memory_order_relaxed); // already applied by prepare()
    active_ = anyEffectActive(effects);
}

bool EffectStack::sameStructure(const std::vector<Effect>& effects) const {
    if (effects.size() != types_.size()) {
        return false;
    }
    for (size_t i = 0; i < effects.size(); ++i) {
        if (effects[i].type != types_[i]) return false;
    }
    return true;
}

void EffectStack::publish(const std::vector<Effect>& effects) {
    const size_t n = std::min(effects.size(), types_.size());
    for (size_t i = 0; i < n; ++i) {
        Effect effect = effects[i];
        effect.clampParams();
        shared_[i].bypassed.store(effect.bypassed, std::memory_order_relaxed);
        for (int p = 0; p < kMaxEffectParams; ++p) {
            shared_[i].params[p].store(effect.params[static_cast<size_t>(p)], std::memory_order_relaxed);
        }
    }
    // Release: the values above are visible to whoever sees the new version.
    // A reader that catches a half-written update sees the version move again
    // and re-reads at its next block.
    version_.fetch_add(1, std::memory_order_release);
}

bool EffectStack::update() {
    const uint64_t version = version_.load(std::memory_order_acquire);
    if (version != applied_) {
        bool active = false;
        for (size_t i = 0; i < processors_.size(); ++i) {
            Effect effect = processors_[i].effect(); // plain data: no allocation
            effect.bypassed = shared_[i].bypassed.load(std::memory_order_relaxed);
            for (int p = 0; p < kMaxEffectParams; ++p) {
                effect.params[static_cast<size_t>(p)] = shared_[i].params[p].load(std::memory_order_relaxed);
            }
            if (effect != processors_[i].effect()) {
                processors_[i].apply(effect);
            }
            active = active || !effect.bypassed;
        }
        active_ = active;
        applied_ = version;
    }
    return active_;
}

void EffectStack::process(float* interleaved, size_t frames) {
    for (EffectProcessor& p : processors_) {
        p.process(interleaved, frames);
    }
}

void EffectStack::reset() {
    for (EffectProcessor& p : processors_) {
        p.reset();
    }
}

} // namespace zrecord
