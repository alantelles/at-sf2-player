#pragma once

#include "plugin_ids.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct tsf;
struct tsf_envelope;

namespace SuperSimpleSF2 {

struct Sf2Status {
    bool loaded = false;
    bool loading = false;
    int presetCount = 0;
    int selectedPresetIndex = 0;
    std::string path;
    std::string firstPresetName;
    std::string error;
    std::vector<PresetEntry> presets;
};

/**
 * Thread-safe SoundFont2 synthesizer engine wrapping TinySoundFont.
 *
 * Audio thread calls render() and MIDI methods using try_to_lock so it never
 * stalls on disk I/O or heap allocation while a new .sf2 is loaded in the background.
 */
class SynthEngine {
public:
    using StatusCallback = std::function<void(const Sf2Status&)>;

    SynthEngine();
    ~SynthEngine();

    SynthEngine(const SynthEngine&) = delete;
    SynthEngine& operator=(const SynthEngine&) = delete;

    void setStatusCallback(StatusCallback cb);

    void setSampleRate(int sampleRate, int maxBlockSize);
    void setVolumeGain(float targetGain);
    void setAdsrSettings(const AdsrSettings& adsr);
    void setPortamentoSettings(const PortamentoSettings& porta);
    void setSelectedPreset(int presetIndex);

    void requestLoadAsync(const std::string& path, int initialPresetIndex = 0);
    bool loadSync(const std::string& path, int initialPresetIndex = 0);

    void noteOn(int channel, int key, float velocity);
    void noteOff(int channel, int key);
    void allNotesOff();
    void setPitchWheel(int channel, int wheel14Bit);
    void setSustain(int channel, bool on);
    void setModWheel(int channel, float value);
    void setTuning(int channel, float semitones);

    // Renders stereo float audio into outL/outR (size numSamples).
    // Safe for real-time audio thread.
    void render(float* outL, float* outR, int numSamples);

    Sf2Status getStatus() const;
    int activeVoiceCount() const;

private:
    void loaderThreadMain();
    bool loadInternal(const std::string& path, int initialPresetIndex, Sf2Status& outStatus);
    static void configureChannels(tsf* synth, int sampleRate, int presetIndex, float modWheel, float tuningSemitones);
    static void applyEnvelopesToSynth(tsf* synth,
                                      const std::vector<std::vector<tsf_envelope>>& origEnvs,
                                      const AdsrSettings& adsr);
    static void captureOriginalEnvelopes(tsf* synth, std::vector<std::vector<tsf_envelope>>& outEnvs);
    static bool glideActiveVoicesToKey(tsf* synth, int channel, int newKey, float glideDecayPerSample);
    static void applyInitialGlideToKey(tsf* synth, int channel, int newKey, float startGlideCents, float glideDecayPerSample);

    struct ChannelGlideState {
        int lastKey = -1;
        std::vector<int> heldKeys;
    };

    mutable std::mutex m_synthMutex;
    tsf* m_synth = nullptr;
    std::vector<std::vector<tsf_envelope>> m_originalEnvelopes;
    std::vector<ChannelGlideState> m_channelGlide;

    int m_sampleRate = 44100;
    std::vector<float> m_scratchBuffer;

    std::atomic<float> m_targetGain{0.64f};
    float m_smoothedGain = 0.64f;
    std::atomic<int> m_activeVoices{0};

    AdsrSettings m_adsr;
    PortamentoSettings m_portamento;
    int m_selectedPresetIndex = 0;
    float m_modWheel = 0.0f;
    float m_tuningSemitones = 0.0f;

    mutable std::mutex m_statusMutex;
    Sf2Status m_status;
    StatusCallback m_statusCallback;

    // Background loader thread state
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::string m_pendingPath;
    int m_pendingPresetIndex = 0;
    bool m_hasPendingLoad = false;
    bool m_stopLoader = false;
    std::thread m_loaderThread;
};

} // namespace SuperSimpleSF2
