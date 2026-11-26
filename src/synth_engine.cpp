#include "synth_engine.h"

#define TSF_IMPLEMENTATION
#include "tsf.h"

#include <algorithm>
#include <cstring>

namespace SuperSimpleSF2 {

namespace {
constexpr int kMaxSynthVoices = 256;
constexpr int kNumMidiChannels = 16;
constexpr float kMinReleaseFloorSec = 0.040f; // 40 ms anti-click release floor (~8 ms for -20 dB in SF2 100 dB curve)
} // namespace

SynthEngine::SynthEngine() {
    m_scratchBuffer.resize(4096 * 2, 0.0f);
    m_channelGlide.resize(kNumMidiChannels);
    for (auto& ch : m_channelGlide) {
        ch.heldKeys.reserve(32);
    }
    m_loaderThread = std::thread(&SynthEngine::loaderThreadMain, this);
}

SynthEngine::~SynthEngine() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stopLoader = true;
    }
    m_queueCv.notify_all();
    if (m_loaderThread.joinable()) {
        m_loaderThread.join();
    }

    std::lock_guard<std::mutex> lock(m_synthMutex);
    if (m_synth) {
        tsf_close(m_synth);
        m_synth = nullptr;
    }
}

void SynthEngine::setStatusCallback(StatusCallback cb) {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_statusCallback = std::move(cb);
}

void SynthEngine::setSampleRate(int sampleRate, int maxBlockSize) {
    if (sampleRate < 8000) sampleRate = 44100;
    if (maxBlockSize < 64) maxBlockSize = 4096;

    std::lock_guard<std::mutex> lock(m_synthMutex);
    m_sampleRate = sampleRate;
    if (static_cast<int>(m_scratchBuffer.size()) < maxBlockSize * 2) {
        m_scratchBuffer.resize(static_cast<size_t>(maxBlockSize) * 2, 0.0f);
    }
    if (m_synth) {
        tsf_set_output(m_synth, TSF_STEREO_UNWEAVED, m_sampleRate, 0.0f);
    }
}

void SynthEngine::setVolumeGain(float targetGain) {
    targetGain = std::clamp(targetGain, 0.0f, 2.0f);
    m_targetGain.store(targetGain, std::memory_order_relaxed);
}

void SynthEngine::setAdsrSettings(const AdsrSettings& adsr) {
    std::unique_lock<std::mutex> lock(m_synthMutex, std::try_to_lock);
    m_adsr = adsr;
    if (!lock.owns_lock() || !m_synth) {
        return;
    }
    applyEnvelopesToSynth(m_synth, m_originalEnvelopes, m_adsr);
}

void SynthEngine::setPortamentoSettings(const PortamentoSettings& porta) {
    std::unique_lock<std::mutex> lock(m_synthMutex, std::try_to_lock);
    m_portamento = porta;
}

void SynthEngine::setSelectedPreset(int presetIndex) {
    Sf2Status updatedStatus;
    StatusCallback cb;

    {
        std::lock_guard<std::mutex> lock(m_synthMutex);
        m_selectedPresetIndex = std::max(0, presetIndex);
        if (m_synth && m_synth->presetNum > 0) {
            const int clamped = std::clamp(m_selectedPresetIndex, 0, m_synth->presetNum - 1);
            m_selectedPresetIndex = clamped;
            for (int ch = 0; ch < kNumMidiChannels; ++ch) {
                tsf_channel_set_presetindex(m_synth, ch, clamped);
            }
        }
    }

    {
        std::lock_guard<std::mutex> statusLock(m_statusMutex);
        if (m_status.loaded && m_status.presetCount > 0) {
            m_status.selectedPresetIndex = std::clamp(presetIndex, 0, m_status.presetCount - 1);
            if (m_status.selectedPresetIndex < static_cast<int>(m_status.presets.size())) {
                m_status.firstPresetName = m_status.presets[static_cast<size_t>(m_status.selectedPresetIndex)].name;
            }
        }
        updatedStatus = m_status;
        cb = m_statusCallback;
    }

    if (cb && updatedStatus.loaded) {
        cb(updatedStatus);
    }
}

void SynthEngine::requestLoadAsync(const std::string& path, int initialPresetIndex) {
    {
        std::lock_guard<std::mutex> statusLock(m_statusMutex);
        m_status.loading = true;
        m_status.path = path;
        m_status.selectedPresetIndex = std::max(0, initialPresetIndex);
        m_status.error.clear();
    }
    {
        std::lock_guard<std::mutex> qLock(m_queueMutex);
        m_pendingPath = path;
        m_pendingPresetIndex = std::max(0, initialPresetIndex);
        m_hasPendingLoad = true;
    }
    m_queueCv.notify_one();
}

bool SynthEngine::loadSync(const std::string& path, int initialPresetIndex) {
    Sf2Status nextStatus;
    const bool ok = loadInternal(path, initialPresetIndex, nextStatus);
    StatusCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_status = nextStatus;
        cb = m_statusCallback;
    }
    if (cb) {
        cb(nextStatus);
    }
    return ok;
}

void SynthEngine::loaderThreadMain() {
    while (true) {
        std::string pathToLoad;
        int presetToSelect = 0;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this]() { return m_hasPendingLoad || m_stopLoader; });
            if (m_stopLoader) break;
            pathToLoad = std::move(m_pendingPath);
            presetToSelect = m_pendingPresetIndex;
            m_hasPendingLoad = false;
        }

        Sf2Status nextStatus;
        loadInternal(pathToLoad, presetToSelect, nextStatus);

        bool hasNewer = false;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            hasNewer = m_hasPendingLoad;
        }
        if (hasNewer) {
            continue;
        }

        StatusCallback cb;
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_status = nextStatus;
            cb = m_statusCallback;
        }
        if (cb) {
            cb(nextStatus);
        }
    }
}

void SynthEngine::captureOriginalEnvelopes(tsf* synth, std::vector<std::vector<tsf_envelope>>& outEnvs) {
    outEnvs.clear();
    if (!synth) return;
    outEnvs.resize(static_cast<size_t>(synth->presetNum));
    for (int p = 0; p < synth->presetNum; ++p) {
        const struct tsf_preset& preset = synth->presets[p];
        auto& regionEnvs = outEnvs[static_cast<size_t>(p)];
        regionEnvs.resize(static_cast<size_t>(preset.regionNum));
        for (int r = 0; r < preset.regionNum; ++r) {
            regionEnvs[static_cast<size_t>(r)] = preset.regions[r].ampenv;
        }
    }
}

void SynthEngine::applyEnvelopesToSynth(tsf* synth,
                                        const std::vector<std::vector<tsf_envelope>>& origEnvs,
                                        const AdsrSettings& adsr) {
    if (!synth) return;
    const float customA = attackNormToSec(adsr.attackNorm);
    const float customD = decayNormToSec(adsr.decayNorm);
    const float customS = sustainNormToLevel(adsr.sustainNorm);
    const float customR = releaseNormToSec(adsr.releaseNorm);

    const int presetCount = std::min<int>(synth->presetNum, static_cast<int>(origEnvs.size()));
    for (int p = 0; p < presetCount; ++p) {
        struct tsf_preset& preset = synth->presets[p];
        const auto& origRegions = origEnvs[static_cast<size_t>(p)];
        const int regionCount = std::min<int>(preset.regionNum, static_cast<int>(origRegions.size()));
        for (int r = 0; r < regionCount; ++r) {
            struct tsf_envelope& env = preset.regions[r].ampenv;
            if (adsr.enabled) {
                env.delay = 0.0f;
                env.attack = customA;
                env.hold = 0.0f;
                env.decay = customD;
                env.sustain = customS;
                env.release = customR;
                env.keynumToHold = 0.0f;
                env.keynumToDecay = 0.0f;
            } else {
                env = origRegions[static_cast<size_t>(r)];
            }
            if (env.release < kMinReleaseFloorSec) {
                env.release = kMinReleaseFloorSec;
            }
        }
    }

    // Also update currently active voices so tweaking release/sustain responds immediately
    if (synth->voices && synth->voiceNum > 0) {
        for (int i = 0; i < synth->voiceNum; ++i) {
            struct tsf_voice& v = synth->voices[i];
            if (v.playingPreset != -1 && v.region) {
                v.ampenv.parameters = v.region->ampenv;
            }
        }
    }
}

void SynthEngine::configureChannels(tsf* synth, int sampleRate, int presetIndex, float modWheel, float tuningSemitones) {
    tsf_set_output(synth, TSF_STEREO_UNWEAVED, sampleRate, 0.0f);
    tsf_set_max_voices(synth, kMaxSynthVoices);

    const int presetCount = tsf_get_presetcount(synth);
    const int clampedPreset = (presetCount > 0) ? std::clamp(presetIndex, 0, presetCount - 1) : 0;

    for (int ch = 0; ch < kNumMidiChannels; ++ch) {
        if (presetCount > 0) {
            tsf_channel_set_presetindex(synth, ch, clampedPreset);
        }
        tsf_channel_set_volume(synth, ch, 1.0f);
        tsf_channel_set_pan(synth, ch, 0.5f);
        tsf_channel_set_pitchrange(synth, ch, 2.0f);
        tsf_channel_set_modwheel(synth, ch, modWheel);
        tsf_channel_set_tuning(synth, ch, tuningSemitones);
    }
}

bool SynthEngine::loadInternal(const std::string& path, int initialPresetIndex, Sf2Status& outStatus) {
    outStatus.path = path;
    outStatus.loading = false;

    if (path.empty()) {
        tsf* oldSynth = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_synthMutex);
            oldSynth = m_synth;
            m_synth = nullptr;
            m_originalEnvelopes.clear();
            m_selectedPresetIndex = 0;
        }
        if (oldSynth) tsf_close(oldSynth);
        m_activeVoices.store(0, std::memory_order_relaxed);
        outStatus.loaded = false;
        return false;
    }

    tsf* newSynth = tsf_load_filename(path.c_str());
    if (!newSynth) {
        outStatus.loaded = false;
        outStatus.error = "Failed to load SoundFont (.sf2)";
        return false;
    }

    std::vector<std::vector<tsf_envelope>> origEnvs;
    captureOriginalEnvelopes(newSynth, origEnvs);

    const int presetCount = tsf_get_presetcount(newSynth);
    const int selectedIdx = (presetCount > 0) ? std::clamp(initialPresetIndex, 0, presetCount - 1) : 0;

    std::vector<PresetEntry> presets;
    presets.reserve(static_cast<size_t>(presetCount));
    for (int i = 0; i < presetCount; ++i) {
        PresetEntry entry;
        entry.index = i;
        entry.bank = newSynth->presets[i].bank;
        entry.program = newSynth->presets[i].preset;
        const char* pname = tsf_get_presetname(newSynth, i);
        entry.name = pname ? pname : ("Preset " + std::to_string(i));
        presets.push_back(std::move(entry));
    }

    tsf* oldSynth = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_synthMutex);
        configureChannels(newSynth, m_sampleRate, selectedIdx, m_modWheel, m_tuningSemitones);
        applyEnvelopesToSynth(newSynth, origEnvs, m_adsr);
        oldSynth = m_synth;
        m_synth = newSynth;
        m_originalEnvelopes = std::move(origEnvs);
        m_selectedPresetIndex = selectedIdx;
    }
    if (oldSynth) {
        tsf_close(oldSynth);
    }

    outStatus.loaded = true;
    outStatus.presetCount = presetCount;
    outStatus.selectedPresetIndex = selectedIdx;
    outStatus.presets = std::move(presets);
    if (!outStatus.presets.empty()) {
        outStatus.firstPresetName = outStatus.presets[static_cast<size_t>(selectedIdx)].name;
    }
    outStatus.error.clear();
    return true;
}

bool SynthEngine::glideActiveVoicesToKey(tsf* synth, int channel, int newKey, float glideDecayPerSample) {
    if (!synth || !synth->voices || synth->voiceNum <= 0) return false;
    float pitchShift = 0.0f;
    if (synth->channels && channel >= 0 && channel < synth->channels->channelNum) {
        const struct tsf_channel& c = synth->channels->channels[channel];
        pitchShift = (c.pitchWheel == 8192)
                         ? c.tuning
                         : ((c.pitchWheel / 16383.0f * c.pitchRange * 2.0f) - c.pitchRange + c.tuning);
    }
    bool found = false;
    for (int i = 0; i < synth->voiceNum; ++i) {
        struct tsf_voice& v = synth->voices[i];
        if (v.playingPreset != -1 && v.playingChannel == channel &&
            v.ampenv.segment < TSF_SEGMENT_RELEASE && v.region) {
            const double currentPitchCents = v.pitchInputTimecents + static_cast<double>(v.glideCents);
            v.playingKey = newKey;
            tsf_voice_calcpitchratio(&v, pitchShift, synth->outSampleRate);
            v.glideCents = static_cast<float>(currentPitchCents - v.pitchInputTimecents);
            v.glideDecay = glideDecayPerSample;
            found = true;
        }
    }
    return found;
}

void SynthEngine::applyInitialGlideToKey(tsf* synth, int channel, int newKey, float startGlideCents, float glideDecayPerSample) {
    if (!synth || !synth->voices || synth->voiceNum <= 0) return;
    for (int i = 0; i < synth->voiceNum; ++i) {
        struct tsf_voice& v = synth->voices[i];
        if (v.playingPreset != -1 && v.playingChannel == channel && v.playingKey == newKey &&
            v.ampenv.segment < TSF_SEGMENT_RELEASE) {
            v.glideCents = startGlideCents;
            v.glideDecay = glideDecayPerSample;
        }
    }
}

void SynthEngine::noteOn(int channel, int key, float velocity) {
    if (channel < 0 || channel >= kNumMidiChannels) channel = 0;
    if (key < 0 || key > 127) return;
    if (velocity <= 0.0f) {
        noteOff(channel, key);
        return;
    }
    if (velocity > 1.0f) velocity = 1.0f;

    std::unique_lock<std::mutex> lock(m_synthMutex, std::try_to_lock);
    if (!lock.owns_lock() || !m_synth) return;

    ChannelGlideState& gs = m_channelGlide[static_cast<size_t>(channel)];
    auto addHeldKey = [&gs, key]() {
        if (std::find(gs.heldKeys.begin(), gs.heldKeys.end(), key) == gs.heldKeys.end()) {
            if (gs.heldKeys.size() < 32) {
                gs.heldKeys.push_back(key);
            }
        }
    };

    if (!m_portamento.enabled) {
        addHeldKey();
        gs.lastKey = key;
        tsf_channel_note_on(m_synth, channel, key, velocity);
        return;
    }

    const float glideSec = std::max(0.005f, glideNormToSec(m_portamento.glideNorm));
    const float decayPerSample = std::exp(-4.6f / (glideSec * static_cast<float>(m_sampleRate)));

    if (m_portamento.legato) {
        if (!gs.heldKeys.empty()) {
            addHeldKey();
            gs.lastKey = key;
            if (glideActiveVoicesToKey(m_synth, channel, key, decayPerSample)) {
                return;
            }
        } else {
            addHeldKey();
            gs.lastKey = key;
        }
        tsf_channel_note_on(m_synth, channel, key, velocity);
        return;
    }

    // Retrigger mode: release previous note on channel, trigger new note and glide from lastKey
    const int prevKey = gs.lastKey;
    tsf_channel_note_off_all(m_synth, channel);
    gs.heldKeys.clear();
    addHeldKey();
    gs.lastKey = key;
    tsf_channel_note_on(m_synth, channel, key, velocity);
    if (prevKey >= 0 && prevKey != key) {
        applyInitialGlideToKey(m_synth, channel, key, static_cast<float>((prevKey - key) * 100), decayPerSample);
    }
}

void SynthEngine::noteOff(int channel, int key) {
    if (channel < 0 || channel >= kNumMidiChannels) channel = 0;
    if (key < 0 || key > 127) return;

    std::unique_lock<std::mutex> lock(m_synthMutex, std::try_to_lock);
    if (!lock.owns_lock() || !m_synth) return;

    ChannelGlideState& gs = m_channelGlide[static_cast<size_t>(channel)];
    auto it = std::find(gs.heldKeys.begin(), gs.heldKeys.end(), key);
    if (it != gs.heldKeys.end()) {
        gs.heldKeys.erase(it);
    }

    if (m_portamento.enabled && m_portamento.legato && !gs.heldKeys.empty()) {
        const int targetKey = gs.heldKeys.back();
        const float glideSec = std::max(0.005f, glideNormToSec(m_portamento.glideNorm));
        const float decayPerSample = std::exp(-4.6f / (glideSec * static_cast<float>(m_sampleRate)));
        gs.lastKey = targetKey;
        if (glideActiveVoicesToKey(m_synth, channel, targetKey, decayPerSample)) {
            return;
        }
    }

    tsf_channel_note_off(m_synth, channel, key);
}

void SynthEngine::allNotesOff() {
    std::unique_lock<std::mutex> lock(m_synthMutex, std::try_to_lock);
    if (!lock.owns_lock() || !m_synth) return;
    for (auto& gs : m_channelGlide) {
        gs.heldKeys.clear();
    }
    tsf_note_off_all(m_synth);
}

void SynthEngine::setPitchWheel(int channel, int wheel14Bit) {
    wheel14Bit = std::clamp(wheel14Bit, 0, 16383);

    std::unique_lock<std::mutex> lock(m_synthMutex, std::try_to_lock);
    if (!lock.owns_lock() || !m_synth) return;
    if (channel < 0) {
        for (int ch = 0; ch < kNumMidiChannels; ++ch) {
            tsf_channel_set_pitchwheel(m_synth, ch, wheel14Bit);
        }
    } else if (channel < kNumMidiChannels) {
        tsf_channel_set_pitchwheel(m_synth, channel, wheel14Bit);
    }
}

void SynthEngine::setSustain(int channel, bool on) {
    std::unique_lock<std::mutex> lock(m_synthMutex, std::try_to_lock);
    if (!lock.owns_lock() || !m_synth) return;
    const int flag = on ? 1 : 0;
    if (channel < 0) {
        for (int ch = 0; ch < kNumMidiChannels; ++ch) {
            tsf_channel_set_sustain(m_synth, ch, flag);
        }
    } else if (channel < kNumMidiChannels) {
        tsf_channel_set_sustain(m_synth, channel, flag);
    }
}

void SynthEngine::setModWheel(int channel, float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    std::unique_lock<std::mutex> lock(m_synthMutex, std::try_to_lock);
    m_modWheel = value;
    if (!lock.owns_lock() || !m_synth) return;
    if (channel < 0) {
        for (int ch = 0; ch < kNumMidiChannels; ++ch) {
            tsf_channel_set_modwheel(m_synth, ch, value);
        }
    } else if (channel < kNumMidiChannels) {
        tsf_channel_set_modwheel(m_synth, channel, value);
    }
}

void SynthEngine::setTuning(int channel, float semitones) {
    semitones = std::clamp(semitones, -25.0f, 25.0f);
    std::unique_lock<std::mutex> lock(m_synthMutex, std::try_to_lock);
    m_tuningSemitones = semitones;
    if (!lock.owns_lock() || !m_synth) return;
    if (channel < 0) {
        for (int ch = 0; ch < kNumMidiChannels; ++ch) {
            tsf_channel_set_tuning(m_synth, ch, semitones);
        }
    } else if (channel < kNumMidiChannels) {
        tsf_channel_set_tuning(m_synth, channel, semitones);
    }
}

void SynthEngine::render(float* outL, float* outR, int numSamples) {
    if (numSamples <= 0 || !outL || !outR) return;

    const float target = m_targetGain.load(std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(m_synthMutex, std::try_to_lock);
    if (!lock.owns_lock() || !m_synth) {
        std::memset(outL, 0, static_cast<size_t>(numSamples) * sizeof(float));
        std::memset(outR, 0, static_cast<size_t>(numSamples) * sizeof(float));
        m_smoothedGain = target;
        m_activeVoices.store(0, std::memory_order_relaxed);
        return;
    }

    int offset = 0;
    const int scratchCapacity = static_cast<int>(m_scratchBuffer.size() / 2);
    const float smoothCoeff = 0.004f;

    while (offset < numSamples) {
        const int chunk = std::min(numSamples - offset, scratchCapacity);
        float* bufL = m_scratchBuffer.data();
        float* bufR = bufL + chunk;

        tsf_render_float(m_synth, m_scratchBuffer.data(), chunk, 0);

        for (int i = 0; i < chunk; ++i) {
            m_smoothedGain += smoothCoeff * (target - m_smoothedGain);
            outL[offset + i] = bufL[i] * m_smoothedGain;
            outR[offset + i] = bufR[i] * m_smoothedGain;
        }
        offset += chunk;
    }

    m_activeVoices.store(tsf_active_voice_count(m_synth), std::memory_order_relaxed);
}

Sf2Status SynthEngine::getStatus() const {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_status;
}

int SynthEngine::activeVoiceCount() const {
    return m_activeVoices.load(std::memory_order_relaxed);
}

} // namespace SuperSimpleSF2
