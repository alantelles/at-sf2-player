#include "processor.h"

#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace SuperSimpleSF2 {

using namespace Steinberg;
using namespace Steinberg::Vst;

Processor::Processor() {
    FUNKNOWN_CTOR
    m_synth.setVolumeGain(volumeNormToGain(kDefaultVolumeNorm));
    m_synth.setStatusCallback([this](const Sf2Status& status) {
        if (status.loaded) {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_selectedPresetIndex = status.selectedPresetIndex;
        }
        sendStatusToController(status);
    });
}

Processor::~Processor() {
    FUNKNOWN_DTOR
    m_synth.setStatusCallback(nullptr);
    {
        std::lock_guard<std::mutex> lock(m_peerMutex);
        if (m_peer) {
            m_peer->release();
            m_peer = nullptr;
        }
    }
    if (m_hostApp) {
        m_hostApp->release();
        m_hostApp = nullptr;
    }
}

IMPLEMENT_REFCOUNT(Processor)

tresult PLUGIN_API Processor::queryInterface(const TUID _iid, void** obj) {
    QUERY_INTERFACE(_iid, obj, FUnknown::iid, IComponent)
    QUERY_INTERFACE(_iid, obj, IPluginBase::iid, IComponent)
    QUERY_INTERFACE(_iid, obj, IComponent::iid, IComponent)
    QUERY_INTERFACE(_iid, obj, IAudioProcessor::iid, IAudioProcessor)
    QUERY_INTERFACE(_iid, obj, IConnectionPoint::iid, IConnectionPoint)
    QUERY_INTERFACE(_iid, obj, IProcessContextRequirements::iid, IProcessContextRequirements)
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API Processor::initialize(FUnknown* context) {
    if (m_hostApp) {
        m_hostApp->release();
        m_hostApp = nullptr;
    }
    if (context) {
        context->queryInterface(IHostApplication::iid, reinterpret_cast<void**>(&m_hostApp));
    }
    return kResultOk;
}

tresult PLUGIN_API Processor::terminate() {
    m_synth.setStatusCallback(nullptr);
    {
        std::lock_guard<std::mutex> lock(m_peerMutex);
        if (m_peer) {
            m_peer->release();
            m_peer = nullptr;
        }
    }
    if (m_hostApp) {
        m_hostApp->release();
        m_hostApp = nullptr;
    }
    return kResultOk;
}

tresult PLUGIN_API Processor::getControllerClassId(TUID classId) {
    kControllerUID.toTUID(classId);
    return kResultOk;
}

tresult PLUGIN_API Processor::setIoMode(IoMode /*mode*/) {
    return kResultOk;
}

int32 PLUGIN_API Processor::getBusCount(MediaType type, BusDirection dir) {
    if (type == kAudio && dir == kOutput) return 1;
    if (type == kEvent && dir == kInput) return 1;
    return 0;
}

tresult PLUGIN_API Processor::getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) {
    if (index != 0) return kInvalidArgument;

    if (type == kAudio && dir == kOutput) {
        bus.mediaType = kAudio;
        bus.direction = kOutput;
        bus.channelCount = 2;
        UString128("Main Out").copyTo(bus.name, 128);
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    if (type == kEvent && dir == kInput) {
        bus.mediaType = kEvent;
        bus.direction = kInput;
        bus.channelCount = 16;
        UString128("MIDI In").copyTo(bus.name, 128);
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    return kInvalidArgument;
}

tresult PLUGIN_API Processor::getRoutingInfo(RoutingInfo& /*inInfo*/, RoutingInfo& /*outInfo*/) {
    return kNotImplemented;
}

tresult PLUGIN_API Processor::activateBus(MediaType type, BusDirection dir, int32 index, TBool state) {
    if (index != 0) return kInvalidArgument;
    if (type == kAudio && dir == kOutput) {
        m_audioOutActive = (state != 0);
        return kResultOk;
    }
    if (type == kEvent && dir == kInput) {
        m_eventInActive = (state != 0);
        return kResultOk;
    }
    return kInvalidArgument;
}

tresult PLUGIN_API Processor::setActive(TBool state) {
    if (!state) {
        m_synth.allNotesOff();
    }
    return kResultOk;
}

tresult PLUGIN_API Processor::setState(IBStream* state) {
    PluginStateData data;
    if (!data.readFromStream(state)) return kResultFalse;

    m_volumeNorm.store(data.volumeNorm, std::memory_order_relaxed);
    m_modWheelNorm.store(data.modWheelNorm, std::memory_order_relaxed);
    m_adsrEnabled.store(data.adsr.enabled, std::memory_order_relaxed);
    m_attackNorm.store(data.adsr.attackNorm, std::memory_order_relaxed);
    m_decayNorm.store(data.adsr.decayNorm, std::memory_order_relaxed);
    m_sustainNorm.store(data.adsr.sustainNorm, std::memory_order_relaxed);
    m_releaseNorm.store(data.adsr.releaseNorm, std::memory_order_relaxed);
    m_coarseTuneNorm.store(data.coarseTuneNorm, std::memory_order_relaxed);
    m_fineTuneNorm.store(data.fineTuneNorm, std::memory_order_relaxed);
    m_portaEnabled.store(data.portamento.enabled, std::memory_order_relaxed);
    m_portaLegato.store(data.portamento.legato, std::memory_order_relaxed);
    m_glideTimeNorm.store(data.portamento.glideNorm, std::memory_order_relaxed);

    m_synth.setVolumeGain(volumeNormToGain(data.volumeNorm));
    m_synth.setModWheel(-1, static_cast<float>(data.modWheelNorm));
    m_synth.setAdsrSettings(data.adsr);
    m_synth.setTuning(-1, tuningSemitonesTotal(data.coarseTuneNorm, data.fineTuneNorm));
    m_synth.setPortamentoSettings(data.portamento);

    std::string previousPath;
    int32_t previousPreset = 0;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        previousPath = m_sf2Path;
        previousPreset = m_selectedPresetIndex;
        m_sf2Path = data.sf2Path;
        m_selectedPresetIndex = data.selectedPresetIndex;
    }

    if (data.sf2Path != previousPath || (!data.sf2Path.empty() && !m_synth.getStatus().loaded)) {
        m_synth.requestLoadAsync(data.sf2Path, data.selectedPresetIndex);
    } else if (data.selectedPresetIndex != previousPreset) {
        m_synth.setSelectedPreset(data.selectedPresetIndex);
    }
    return kResultOk;
}

tresult PLUGIN_API Processor::getState(IBStream* state) {
    PluginStateData data;
    data.volumeNorm = m_volumeNorm.load(std::memory_order_relaxed);
    data.modWheelNorm = m_modWheelNorm.load(std::memory_order_relaxed);
    data.adsr.enabled = m_adsrEnabled.load(std::memory_order_relaxed);
    data.adsr.attackNorm = m_attackNorm.load(std::memory_order_relaxed);
    data.adsr.decayNorm = m_decayNorm.load(std::memory_order_relaxed);
    data.adsr.sustainNorm = m_sustainNorm.load(std::memory_order_relaxed);
    data.adsr.releaseNorm = m_releaseNorm.load(std::memory_order_relaxed);
    data.coarseTuneNorm = m_coarseTuneNorm.load(std::memory_order_relaxed);
    data.fineTuneNorm = m_fineTuneNorm.load(std::memory_order_relaxed);
    data.portamento.enabled = m_portaEnabled.load(std::memory_order_relaxed);
    data.portamento.legato = m_portaLegato.load(std::memory_order_relaxed);
    data.portamento.glideNorm = m_glideTimeNorm.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        data.sf2Path = m_sf2Path;
        data.selectedPresetIndex = m_selectedPresetIndex;
    }
    return data.writeToStream(state) ? kResultOk : kResultFalse;
}

tresult PLUGIN_API Processor::setBusArrangements(SpeakerArrangement* /*inputs*/, int32 numIns,
                                                 SpeakerArrangement* outputs, int32 numOuts) {
    if (numIns == 0 && numOuts == 1 && outputs && outputs[0] == SpeakerArr::kStereo) {
        return kResultTrue;
    }
    return kResultFalse;
}

tresult PLUGIN_API Processor::getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) {
    if (dir == kOutput && index == 0) {
        arr = SpeakerArr::kStereo;
        return kResultOk;
    }
    return kInvalidArgument;
}

tresult PLUGIN_API Processor::canProcessSampleSize(int32 symbolicSampleSize) {
    return (symbolicSampleSize == kSample32) ? kResultTrue : kResultFalse;
}

uint32 PLUGIN_API Processor::getLatencySamples() {
    return 0;
}

tresult PLUGIN_API Processor::setupProcessing(ProcessSetup& setup) {
    m_sampleRate = static_cast<int>(std::lround(setup.sampleRate));
    if (m_sampleRate < 8000) m_sampleRate = 44100;
    m_maxBlockSize = setup.maxSamplesPerBlock > 0 ? setup.maxSamplesPerBlock : 4096;
    m_synth.setSampleRate(m_sampleRate, m_maxBlockSize);
    return kResultOk;
}

tresult PLUGIN_API Processor::setProcessing(TBool state) {
    if (!state) {
        m_synth.allNotesOff();
    }
    return kResultOk;
}

void Processor::handleParameterChanges(IParameterChanges* paramChanges) {
    if (!paramChanges) return;
    bool adsrChanged = false;
    bool tuningChanged = false;
    bool portaChanged = false;
    const int32 count = paramChanges->getParameterCount();
    for (int32 i = 0; i < count; ++i) {
        IParamValueQueue* queue = paramChanges->getParameterData(i);
        if (!queue) continue;
        const int32 numPoints = queue->getPointCount();
        if (numPoints <= 0) continue;

        int32 offset = 0;
        ParamValue val = 0.0;
        if (queue->getPoint(numPoints - 1, offset, val) != kResultOk) continue;
        val = std::clamp(val, 0.0, 1.0);

        switch (queue->getParameterId()) {
            case kParamVolume:
                m_volumeNorm.store(val, std::memory_order_relaxed);
                m_synth.setVolumeGain(volumeNormToGain(val));
                break;
            case kParamPitchBend:
                m_synth.setPitchWheel(-1, static_cast<int>(std::lround(val * 16383.0)));
                break;
            case kParamSustain:
                m_synth.setSustain(-1, val >= 0.5);
                break;
            case kParamModWheel:
                m_modWheelNorm.store(val, std::memory_order_relaxed);
                m_synth.setModWheel(-1, static_cast<float>(val));
                break;
            case kParamAdsrEnabled:
                m_adsrEnabled.store(val >= 0.5, std::memory_order_relaxed);
                adsrChanged = true;
                break;
            case kParamAttack:
                m_attackNorm.store(val, std::memory_order_relaxed);
                adsrChanged = true;
                break;
            case kParamDecay:
                m_decayNorm.store(val, std::memory_order_relaxed);
                adsrChanged = true;
                break;
            case kParamSustainLevel:
                m_sustainNorm.store(val, std::memory_order_relaxed);
                adsrChanged = true;
                break;
            case kParamRelease:
                m_releaseNorm.store(val, std::memory_order_relaxed);
                adsrChanged = true;
                break;
            case kParamCoarseTune:
                m_coarseTuneNorm.store(val, std::memory_order_relaxed);
                tuningChanged = true;
                break;
            case kParamFineTune:
                m_fineTuneNorm.store(val, std::memory_order_relaxed);
                tuningChanged = true;
                break;
            case kParamPortamentoEnabled:
                m_portaEnabled.store(val >= 0.5, std::memory_order_relaxed);
                portaChanged = true;
                break;
            case kParamPortamentoMode:
                m_portaLegato.store(val >= 0.5, std::memory_order_relaxed);
                portaChanged = true;
                break;
            case kParamGlideTime:
                m_glideTimeNorm.store(val, std::memory_order_relaxed);
                portaChanged = true;
                break;
            default:
                break;
        }
    }

    if (adsrChanged) {
        AdsrSettings adsr;
        adsr.enabled = m_adsrEnabled.load(std::memory_order_relaxed);
        adsr.attackNorm = m_attackNorm.load(std::memory_order_relaxed);
        adsr.decayNorm = m_decayNorm.load(std::memory_order_relaxed);
        adsr.sustainNorm = m_sustainNorm.load(std::memory_order_relaxed);
        adsr.releaseNorm = m_releaseNorm.load(std::memory_order_relaxed);
        m_synth.setAdsrSettings(adsr);
    }

    if (tuningChanged) {
        m_synth.setTuning(-1, tuningSemitonesTotal(
            m_coarseTuneNorm.load(std::memory_order_relaxed),
            m_fineTuneNorm.load(std::memory_order_relaxed)));
    }

    if (portaChanged) {
        PortamentoSettings porta;
        porta.enabled = m_portaEnabled.load(std::memory_order_relaxed);
        porta.legato = m_portaLegato.load(std::memory_order_relaxed);
        porta.glideNorm = m_glideTimeNorm.load(std::memory_order_relaxed);
        m_synth.setPortamentoSettings(porta);
    }
}

tresult PLUGIN_API Processor::process(ProcessData& data) {
    handleParameterChanges(data.inputParameterChanges);

    if (data.numSamples <= 0 || data.numOutputs < 1 || !data.outputs) {
        return kResultOk;
    }

    AudioBusBuffers& outBus = data.outputs[0];
    if (outBus.numChannels < 2 || !outBus.channelBuffers32 ||
        !outBus.channelBuffers32[0] || !outBus.channelBuffers32[1]) {
        return kResultOk;
    }

    float* outL = outBus.channelBuffers32[0];
    float* outR = outBus.channelBuffers32[1];

    if (!m_audioOutActive) {
        std::memset(outL, 0, static_cast<size_t>(data.numSamples) * sizeof(float));
        std::memset(outR, 0, static_cast<size_t>(data.numSamples) * sizeof(float));
        outBus.silenceFlags = 0x3;
        return kResultOk;
    }

    int32 currentSample = 0;
    IEventList* events = (m_eventInActive ? data.inputEvents : nullptr);
    const int32 eventCount = events ? events->getEventCount() : 0;

    for (int32 i = 0; i < eventCount; ++i) {
        Event e;
        if (events->getEvent(i, e) != kResultOk) continue;

        int32 offset = e.sampleOffset;
        if (offset < currentSample) offset = currentSample;
        if (offset > data.numSamples) offset = data.numSamples;

        if (offset > currentSample) {
            m_synth.render(outL + currentSample, outR + currentSample, offset - currentSample);
            currentSample = offset;
        }

        if (e.type == Event::kNoteOnEvent) {
            m_synth.noteOn(e.noteOn.channel, e.noteOn.pitch, e.noteOn.velocity);
        } else if (e.type == Event::kNoteOffEvent) {
            m_synth.noteOff(e.noteOff.channel, e.noteOff.pitch);
        }
    }

    if (currentSample < data.numSamples) {
        m_synth.render(outL + currentSample, outR + currentSample, data.numSamples - currentSample);
    }

    outBus.silenceFlags = 0;
    return kResultOk;
}

uint32 PLUGIN_API Processor::getTailSamples() {
    return kInfiniteTail;
}

tresult PLUGIN_API Processor::connect(IConnectionPoint* other) {
    if (!other) return kInvalidArgument;
    {
        std::lock_guard<std::mutex> lock(m_peerMutex);
        if (m_peer) m_peer->release();
        m_peer = other;
        m_peer->addRef();
    }
    sendStatusToController(m_synth.getStatus());
    return kResultOk;
}

tresult PLUGIN_API Processor::disconnect(IConnectionPoint* other) {
    std::lock_guard<std::mutex> lock(m_peerMutex);
    if (m_peer && (!other || m_peer == other)) {
        m_peer->release();
        m_peer = nullptr;
        return kResultOk;
    }
    return kResultFalse;
}

tresult PLUGIN_API Processor::notify(IMessage* message) {
    if (!message) return kInvalidArgument;
    const char* msgId = message->getMessageID();
    if (!msgId) return kResultFalse;

    if (std::strcmp(msgId, kMsgLoadSf2) == 0) {
        IAttributeList* attrs = message->getAttributes();
        if (!attrs) return kResultFalse;
        const void* rawData = nullptr;
        uint32 sizeBytes = 0;
        std::string newPath;
        if (attrs->getBinary(kAttrPath, rawData, sizeBytes) == kResultOk && rawData && sizeBytes > 0) {
            newPath.assign(static_cast<const char*>(rawData), sizeBytes);
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_sf2Path = newPath;
            m_selectedPresetIndex = 0;
        }
        m_synth.requestLoadAsync(newPath, 0);
        return kResultOk;
    }

    if (std::strcmp(msgId, kMsgSelectPreset) == 0) {
        IAttributeList* attrs = message->getAttributes();
        if (!attrs) return kResultFalse;
        int64 presetIdx = 0;
        if (attrs->getInt(kAttrPresetIndex, presetIdx) != kResultOk) return kResultFalse;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_selectedPresetIndex = static_cast<int32_t>(std::max<int64>(0, presetIdx));
        }
        m_synth.setSelectedPreset(static_cast<int>(presetIdx));
        return kResultOk;
    }

    return kResultFalse;
}

uint32 PLUGIN_API Processor::getProcessContextRequirements() {
    return 0;
}

void Processor::sendStatusToController(const Sf2Status& status) {
    std::lock_guard<std::mutex> lock(m_peerMutex);
    if (!m_peer || !m_hostApp) return;

    IMessage* msg = allocateMessage(m_hostApp);
    if (!msg) return;

    msg->setMessageID(kMsgSf2Status);
    if (IAttributeList* attrs = msg->getAttributes()) {
        attrs->setInt(kAttrLoaded, status.loaded ? 1 : 0);
        attrs->setInt(kAttrPresetCount, status.presetCount);
        attrs->setInt(kAttrPresetIndex, status.selectedPresetIndex);
        if (!status.path.empty()) {
            attrs->setBinary(kAttrPath, status.path.data(), static_cast<uint32>(status.path.size()));
        }
        if (!status.firstPresetName.empty()) {
            attrs->setBinary(kAttrFirstPreset, status.firstPresetName.data(),
                             static_cast<uint32>(status.firstPresetName.size()));
        }
        if (!status.presets.empty()) {
            const std::vector<uint8_t> blob = serializePresetList(status.presets);
            attrs->setBinary(kAttrPresetBlob, blob.data(), static_cast<uint32>(blob.size()));
        }
        if (!status.error.empty()) {
            attrs->setBinary(kAttrError, status.error.data(), static_cast<uint32>(status.error.size()));
        }
    }

    m_peer->notify(msg);
    msg->release();
}

} // namespace SuperSimpleSF2
