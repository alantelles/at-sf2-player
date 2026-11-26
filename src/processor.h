#pragma once

#include "plugin_ids.h"
#include "synth_engine.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include <atomic>
#include <mutex>
#include <string>

namespace SuperSimpleSF2 {

class Processor : public Steinberg::Vst::IComponent,
                  public Steinberg::Vst::IAudioProcessor,
                  public Steinberg::Vst::IConnectionPoint,
                  public Steinberg::Vst::IProcessContextRequirements {
public:
    Processor();
    virtual ~Processor();

    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IComponent*>(new Processor());
    }

    // FUnknown
    DECLARE_FUNKNOWN_METHODS

    // IPluginBase
    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API terminate() SMTG_OVERRIDE;

    // IComponent
    Steinberg::tresult PLUGIN_API getControllerClassId(Steinberg::TUID classId) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setIoMode(Steinberg::Vst::IoMode mode) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getBusCount(Steinberg::Vst::MediaType type,
                                              Steinberg::Vst::BusDirection dir) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getBusInfo(Steinberg::Vst::MediaType type,
                                             Steinberg::Vst::BusDirection dir,
                                             Steinberg::int32 index,
                                             Steinberg::Vst::BusInfo& bus) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getRoutingInfo(Steinberg::Vst::RoutingInfo& inInfo,
                                                 Steinberg::Vst::RoutingInfo& outInfo) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API activateBus(Steinberg::Vst::MediaType type,
                                              Steinberg::Vst::BusDirection dir,
                                              Steinberg::int32 index,
                                              Steinberg::TBool state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) SMTG_OVERRIDE;

    // IAudioProcessor
    Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs,
                                                     Steinberg::int32 numIns,
                                                     Steinberg::Vst::SpeakerArrangement* outputs,
                                                     Steinberg::int32 numOuts) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getBusArrangement(Steinberg::Vst::BusDirection dir,
                                                    Steinberg::int32 index,
                                                    Steinberg::Vst::SpeakerArrangement& arr) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize) SMTG_OVERRIDE;
    Steinberg::uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setProcessing(Steinberg::TBool state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) SMTG_OVERRIDE;
    Steinberg::uint32 PLUGIN_API getTailSamples() SMTG_OVERRIDE;

    // IConnectionPoint
    Steinberg::tresult PLUGIN_API connect(Steinberg::Vst::IConnectionPoint* other) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API disconnect(Steinberg::Vst::IConnectionPoint* other) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) SMTG_OVERRIDE;

    // IProcessContextRequirements
    Steinberg::uint32 PLUGIN_API getProcessContextRequirements() SMTG_OVERRIDE;

private:
    void handleParameterChanges(Steinberg::Vst::IParameterChanges* paramChanges);
    void sendStatusToController(const Sf2Status& status);

    Steinberg::Vst::IHostApplication* m_hostApp = nullptr;
    Steinberg::Vst::IConnectionPoint* m_peer = nullptr;
    std::mutex m_peerMutex;

    SynthEngine m_synth;

    bool m_audioOutActive = true;
    bool m_eventInActive = true;
    int m_sampleRate = 44100;
    int m_maxBlockSize = 4096;

    std::atomic<double> m_volumeNorm{kDefaultVolumeNorm};
    std::atomic<double> m_modWheelNorm{kDefaultModWheelNorm};
    std::atomic<bool> m_adsrEnabled{true};
    std::atomic<double> m_attackNorm{kDefaultAttackNorm};
    std::atomic<double> m_decayNorm{kDefaultDecayNorm};
    std::atomic<double> m_sustainNorm{kDefaultSustainLevelNorm};
    std::atomic<double> m_releaseNorm{kDefaultReleaseNorm};
    std::atomic<double> m_coarseTuneNorm{kDefaultCoarseTuneNorm};
    std::atomic<double> m_fineTuneNorm{kDefaultFineTuneNorm};
    std::atomic<bool> m_portaEnabled{false};
    std::atomic<bool> m_portaLegato{false};
    std::atomic<double> m_glideTimeNorm{kDefaultGlideTimeNorm};
    std::atomic<double> m_velP1X{kDefaultVelP1X};
    std::atomic<double> m_velP1Y{kDefaultVelP1Y};
    std::atomic<double> m_velP2X{kDefaultVelP2X};
    std::atomic<double> m_velP2Y{kDefaultVelP2Y};

    std::mutex m_stateMutex;
    std::string m_sf2Path;
    int32_t m_selectedPresetIndex = 0;
};

} // namespace SuperSimpleSF2
