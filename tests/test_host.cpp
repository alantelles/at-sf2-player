#include "plugin_ids.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <dlfcn.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace SuperSimpleSF2;

namespace {

#define MOCK_FUNKNOWN(ClassName, InterfaceName, ClassIID) \
public: \
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) SMTG_OVERRIDE { \
        QUERY_INTERFACE(_iid, obj, FUnknown::iid, InterfaceName) \
        QUERY_INTERFACE(_iid, obj, ClassIID, InterfaceName) \
        *obj = nullptr; \
        return kNoInterface; \
    } \
    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { \
        return FUnknownPrivate::atomicAdd(__funknownRefCount, 1); \
    } \
    uint32 PLUGIN_API release() SMTG_OVERRIDE { \
        if (FUnknownPrivate::atomicAdd(__funknownRefCount, -1) == 0) { \
            delete this; \
            return 0; \
        } \
        return __funknownRefCount; \
    } \
private: \
    int32 __funknownRefCount = 1; \
public:

class MemoryStream : public IBStream {
public:
    MemoryStream() = default;
    virtual ~MemoryStream() = default;
    MOCK_FUNKNOWN(MemoryStream, IBStream, IBStream::iid)

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) SMTG_OVERRIDE {
        if (!buffer || numBytes < 0) return kInvalidArgument;
        const size_t avail = (m_pos < m_data.size()) ? (m_data.size() - m_pos) : 0;
        const size_t toRead = std::min(static_cast<size_t>(numBytes), avail);
        if (toRead > 0) {
            std::memcpy(buffer, m_data.data() + m_pos, toRead);
            m_pos += toRead;
        }
        if (numBytesRead) *numBytesRead = static_cast<int32>(toRead);
        return kResultOk;
    }

    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) SMTG_OVERRIDE {
        if (!buffer || numBytes < 0) return kInvalidArgument;
        const size_t endPos = m_pos + static_cast<size_t>(numBytes);
        if (endPos > m_data.size()) m_data.resize(endPos);
        if (numBytes > 0) {
            std::memcpy(m_data.data() + m_pos, buffer, static_cast<size_t>(numBytes));
            m_pos = endPos;
        }
        if (numBytesWritten) *numBytesWritten = numBytes;
        return kResultOk;
    }

    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) SMTG_OVERRIDE {
        int64 newPos = 0;
        if (mode == kIBSeekSet) newPos = pos;
        else if (mode == kIBSeekCur) newPos = static_cast<int64>(m_pos) + pos;
        else if (mode == kIBSeekEnd) newPos = static_cast<int64>(m_data.size()) + pos;
        else return kInvalidArgument;
        if (newPos < 0) return kInvalidArgument;
        m_pos = static_cast<size_t>(newPos);
        if (result) *result = newPos;
        return kResultOk;
    }

    tresult PLUGIN_API tell(int64* pos) SMTG_OVERRIDE {
        if (!pos) return kInvalidArgument;
        *pos = static_cast<int64>(m_pos);
        return kResultOk;
    }

    void rewind() { m_pos = 0; }

private:
    std::vector<uint8_t> m_data;
    size_t m_pos = 0;
};

class MockAttributeList : public IAttributeList {
public:
    MockAttributeList() = default;
    virtual ~MockAttributeList() = default;
    MOCK_FUNKNOWN(MockAttributeList, IAttributeList, IAttributeList::iid)

    tresult PLUGIN_API setInt(AttrID id, int64 value) SMTG_OVERRIDE {
        m_ints[id] = value;
        return kResultOk;
    }
    tresult PLUGIN_API getInt(AttrID id, int64& value) SMTG_OVERRIDE {
        auto it = m_ints.find(id);
        if (it == m_ints.end()) return kResultFalse;
        value = it->second;
        return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID id, double value) SMTG_OVERRIDE {
        m_floats[id] = value;
        return kResultOk;
    }
    tresult PLUGIN_API getFloat(AttrID id, double& value) SMTG_OVERRIDE {
        auto it = m_floats.find(id);
        if (it == m_floats.end()) return kResultFalse;
        value = it->second;
        return kResultOk;
    }
    tresult PLUGIN_API setString(AttrID id, const TChar* string) SMTG_OVERRIDE {
        if (!string) return kInvalidArgument;
        std::u16string s(reinterpret_cast<const char16_t*>(string));
        m_strings[id] = std::move(s);
        return kResultOk;
    }
    tresult PLUGIN_API getString(AttrID id, TChar* string, uint32 sizeInBytes) SMTG_OVERRIDE {
        auto it = m_strings.find(id);
        if (it == m_strings.end() || !string || sizeInBytes < sizeof(TChar)) return kResultFalse;
        const size_t maxChars = (sizeInBytes / sizeof(TChar)) - 1;
        const size_t copyChars = std::min(maxChars, it->second.size());
        std::memcpy(string, it->second.data(), copyChars * sizeof(TChar));
        string[copyChars] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32 sizeInBytes) SMTG_OVERRIDE {
        const auto* bytes = static_cast<const uint8_t*>(data);
        m_binaries[id].assign(bytes, bytes + sizeInBytes);
        return kResultOk;
    }
    tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32& sizeInBytes) SMTG_OVERRIDE {
        auto it = m_binaries.find(id);
        if (it == m_binaries.end()) return kResultFalse;
        data = it->second.data();
        sizeInBytes = static_cast<uint32>(it->second.size());
        return kResultOk;
    }

private:
    std::map<std::string, int64> m_ints;
    std::map<std::string, double> m_floats;
    std::map<std::string, std::u16string> m_strings;
    std::map<std::string, std::vector<uint8_t>> m_binaries;
};

class MockMessage : public IMessage {
public:
    MockMessage() : m_attrs(new MockAttributeList()) {}
    virtual ~MockMessage() {
        if (m_attrs) m_attrs->release();
    }
    MOCK_FUNKNOWN(MockMessage, IMessage, IMessage::iid)

    FIDString PLUGIN_API getMessageID() SMTG_OVERRIDE { return m_id.c_str(); }
    void PLUGIN_API setMessageID(FIDString id) SMTG_OVERRIDE { m_id = id ? id : ""; }
    IAttributeList* PLUGIN_API getAttributes() SMTG_OVERRIDE { return m_attrs; }

private:
    std::string m_id;
    MockAttributeList* m_attrs;
};

class MockHostApp : public IHostApplication {
public:
    MockHostApp() = default;
    virtual ~MockHostApp() = default;
    MOCK_FUNKNOWN(MockHostApp, IHostApplication, IHostApplication::iid)

    tresult PLUGIN_API getName(String128 name) SMTG_OVERRIDE {
        const char* s = "TestHost";
        for (int i = 0; s[i]; ++i) name[i] = static_cast<TChar>(s[i]);
        name[8] = 0;
        return kResultOk;
    }

    tresult PLUGIN_API createInstance(TUID cid, TUID _iid, void** obj) SMTG_OVERRIDE {
        if (!obj) return kInvalidArgument;
        *obj = nullptr;
        TUID msgIid;
        IMessage::iid.toTUID(msgIid);
        if (FUnknownPrivate::iidEqual(cid, msgIid) && FUnknownPrivate::iidEqual(_iid, msgIid)) {
            *obj = static_cast<IMessage*>(new MockMessage());
            return kResultOk;
        }
        return kNoInterface;
    }
};

class MockEventList : public IEventList {
public:
    MockEventList() = default;
    virtual ~MockEventList() = default;
    MOCK_FUNKNOWN(MockEventList, IEventList, IEventList::iid)

    int32 PLUGIN_API getEventCount() SMTG_OVERRIDE { return static_cast<int32>(m_events.size()); }
    tresult PLUGIN_API getEvent(int32 index, Event& e) SMTG_OVERRIDE {
        if (index < 0 || index >= static_cast<int32>(m_events.size())) return kInvalidArgument;
        e = m_events[static_cast<size_t>(index)];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Event& e) SMTG_OVERRIDE {
        m_events.push_back(e);
        return kResultOk;
    }
    void clear() { m_events.clear(); }

private:
    std::vector<Event> m_events;
};

class MockParamQueue : public IParamValueQueue {
public:
    explicit MockParamQueue(ParamID id) : m_id(id) {}
    virtual ~MockParamQueue() = default;
    MOCK_FUNKNOWN(MockParamQueue, IParamValueQueue, IParamValueQueue::iid)

    ParamID PLUGIN_API getParameterId() SMTG_OVERRIDE { return m_id; }
    int32 PLUGIN_API getPointCount() SMTG_OVERRIDE { return static_cast<int32>(m_points.size()); }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& value) SMTG_OVERRIDE {
        if (index < 0 || index >= static_cast<int32>(m_points.size())) return kInvalidArgument;
        sampleOffset = m_points[static_cast<size_t>(index)].first;
        value = m_points[static_cast<size_t>(index)].second;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 sampleOffset, ParamValue value, int32& index) SMTG_OVERRIDE {
        index = static_cast<int32>(m_points.size());
        m_points.emplace_back(sampleOffset, value);
        return kResultOk;
    }

private:
    ParamID m_id;
    std::vector<std::pair<int32, ParamValue>> m_points;
};

class MockParamChanges : public IParameterChanges {
public:
    MockParamChanges() = default;
    virtual ~MockParamChanges() { clear(); }
    MOCK_FUNKNOWN(MockParamChanges, IParameterChanges, IParameterChanges::iid)

    int32 PLUGIN_API getParameterCount() SMTG_OVERRIDE { return static_cast<int32>(m_queues.size()); }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) SMTG_OVERRIDE {
        if (index < 0 || index >= static_cast<int32>(m_queues.size())) return nullptr;
        return m_queues[static_cast<size_t>(index)];
    }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) SMTG_OVERRIDE {
        auto* q = new MockParamQueue(id);
        index = static_cast<int32>(m_queues.size());
        m_queues.push_back(q);
        return q;
    }
    void addSinglePoint(ParamID id, ParamValue val) {
        int32 idx = 0;
        int32 ptIdx = 0;
        addParameterData(id, idx)->addPoint(0, val, ptIdx);
    }
    void clear() {
        for (auto* q : m_queues) q->release();
        m_queues.clear();
    }

private:
    std::vector<MockParamQueue*> m_queues;
};

class ConnectionRelay : public IConnectionPoint {
public:
    explicit ConnectionRelay(IConnectionPoint* target) : m_target(target) {
        if (m_target) m_target->addRef();
    }
    virtual ~ConnectionRelay() {
        if (m_target) m_target->release();
    }
    MOCK_FUNKNOWN(ConnectionRelay, IConnectionPoint, IConnectionPoint::iid)

    tresult PLUGIN_API connect(IConnectionPoint*) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API disconnect(IConnectionPoint*) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API notify(IMessage* message) SMTG_OVERRIDE {
        if (message && message->getMessageID() && std::strcmp(message->getMessageID(), kMsgSf2Status) == 0) {
            int64 loaded = 0;
            int64 presets = 0;
            int64 presetIdx = 0;
            if (auto* attrs = message->getAttributes()) {
                attrs->getInt(kAttrLoaded, loaded);
                attrs->getInt(kAttrPresetCount, presets);
                attrs->getInt(kAttrPresetIndex, presetIdx);
                const void* raw = nullptr;
                uint32 size = 0;
                if (attrs->getBinary(kAttrPresetBlob, raw, size) == kResultOk && raw && size > 0) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_presets = deserializePresetList(raw, size);
                }
            }
            if (loaded != 0) {
                m_presetCount.store(static_cast<int>(presets));
                m_selectedPreset.store(static_cast<int>(presetIdx));
                m_loaded.store(true);
            }
        }
        return m_target ? m_target->notify(message) : kResultOk;
    }

    std::vector<PresetEntry> presets() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_presets;
    }

    std::atomic<bool> m_loaded{false};
    std::atomic<int> m_presetCount{0};
    std::atomic<int> m_selectedPreset{0};

private:
    IConnectionPoint* m_target = nullptr;
    mutable std::mutex m_mutex;
    std::vector<PresetEntry> m_presets;
};

float computeRms(const std::vector<float>& buf) {
    double sum = 0.0;
    for (float s : buf) sum += static_cast<double>(s) * s;
    return static_cast<float>(std::sqrt(sum / std::max<size_t>(1, buf.size())));
}

} // namespace

int main() {
    const char* soPath = "build/ATSF2Player.vst3/Contents/x86_64-linux/ATSF2Player.so";
    void* handle = dlopen(soPath, RTLD_NOW | RTLD_LOCAL);
    assert(handle && "Failed to dlopen ATSF2Player.so");

    using ModuleEntryFn = bool(PLUGIN_API*)(void*);
    using ModuleExitFn = bool(PLUGIN_API*)();
    using GetFactoryFn = IPluginFactory*(PLUGIN_API*)();

    auto moduleEntry = reinterpret_cast<ModuleEntryFn>(dlsym(handle, "ModuleEntry"));
    auto moduleExit = reinterpret_cast<ModuleExitFn>(dlsym(handle, "ModuleExit"));
    auto getFactory = reinterpret_cast<GetFactoryFn>(dlsym(handle, "GetPluginFactory"));
    assert(moduleEntry && moduleExit && getFactory);
    assert(moduleEntry(handle));

    IPluginFactory* factory = getFactory();
    assert(factory);
    assert(factory->countClasses() == 2);

    auto* hostApp = new MockHostApp();

    TUID procUid, ctrlUid;
    kProcessorUID.toTUID(procUid);
    kControllerUID.toTUID(ctrlUid);

    IComponent* comp = nullptr;
    assert(factory->createInstance(procUid, IComponent::iid, reinterpret_cast<void**>(&comp)) == kResultOk);
    assert(comp->initialize(hostApp) == kResultOk);

    IEditController* ctrl = nullptr;
    assert(factory->createInstance(ctrlUid, IEditController::iid, reinterpret_cast<void**>(&ctrl)) == kResultOk);
    assert(ctrl->initialize(hostApp) == kResultOk);

    // Verify all 18 parameters & MIDI mapping (Volume CC7, ModWheel CC1, Porta CC65/CC5, PitchBend, Sustain)
    assert(ctrl->getParameterCount() == 18);
    assert(std::abs(ctrl->getParamNormalized(kParamAdsrEnabled) - 1.0) < 1e-6);
    IMidiMapping* midiMap = nullptr;
    assert(ctrl->queryInterface(IMidiMapping::iid, reinterpret_cast<void**>(&midiMap)) == kResultOk);
    ParamID mappedId = 999;
    assert(midiMap->getMidiControllerAssignment(0, 0, kCtrlVolume, mappedId) == kResultTrue);
    assert(mappedId == kParamVolume);
    assert(midiMap->getMidiControllerAssignment(0, 0, kCtrlModWheel, mappedId) == kResultTrue);
    assert(mappedId == kParamModWheel);
    assert(midiMap->getMidiControllerAssignment(0, 0, kPitchBend, mappedId) == kResultTrue);
    assert(mappedId == kParamPitchBend);
    assert(midiMap->getMidiControllerAssignment(0, 0, kCtrlSustainOnOff, mappedId) == kResultTrue);
    assert(mappedId == kParamSustain);
    assert(midiMap->getMidiControllerAssignment(0, 0, kCtrlPortaOnOff, mappedId) == kResultTrue);
    assert(mappedId == kParamPortamentoEnabled);
    assert(midiMap->getMidiControllerAssignment(0, 0, kCtrlPortaTime, mappedId) == kResultTrue);
    assert(mappedId == kParamGlideTime);
    midiMap->release();
    std::cout << "[PASS] Parameters (14), ADSR default ON, and MIDI mappings verified\n";

    // Check X11 view metadata
    IPlugView* view = ctrl->createView(ViewType::kEditor);
    assert(view);
    assert(view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) == kResultTrue);
    ViewRect rect{};
    assert(view->getSize(&rect) == kResultOk);
    assert(rect.getWidth() == 700 && rect.getHeight() == 478);
    assert(view->canResize() == kResultFalse);
    view->release();

    // Connect Processor <-> Controller
    IConnectionPoint* procConn = nullptr;
    IConnectionPoint* ctrlConn = nullptr;
    assert(comp->queryInterface(IConnectionPoint::iid, reinterpret_cast<void**>(&procConn)) == kResultOk);
    assert(ctrl->queryInterface(IConnectionPoint::iid, reinterpret_cast<void**>(&ctrlConn)) == kResultOk);
    auto* relay = new ConnectionRelay(ctrlConn);
    assert(procConn->connect(relay) == kResultOk);
    assert(ctrlConn->connect(procConn) == kResultOk);

    // Configure audio processing
    IAudioProcessor* audio = nullptr;
    assert(comp->queryInterface(IAudioProcessor::iid, reinterpret_cast<void**>(&audio)) == kResultOk);
    SpeakerArrangement stereo = SpeakerArr::kStereo;
    assert(audio->setBusArrangements(nullptr, 0, &stereo, 1) == kResultTrue);
    assert(audio->canProcessSampleSize(kSample32) == kResultTrue);
    assert(comp->activateBus(kAudio, kOutput, 0, 1) == kResultOk);
    assert(comp->activateBus(kEvent, kInput, 0, 1) == kResultOk);

    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = 512;
    setup.sampleRate = 44100.0;
    assert(audio->setupProcessing(setup) == kResultOk);
    assert(comp->setActive(1) == kResultOk);
    assert(audio->setProcessing(1) == kResultOk);

    // Load TimGM6mb.sf2
    const std::string sf2Path = "/usr/share/sounds/sf2/TimGM6mb.sf2";
    auto* loadMsg = allocateMessage(hostApp);
    assert(loadMsg);
    loadMsg->setMessageID(kMsgLoadSf2);
    loadMsg->getAttributes()->setBinary(kAttrPath, sf2Path.data(), static_cast<uint32>(sf2Path.size()));
    assert(procConn->notify(loadMsg) == kResultOk);
    loadMsg->release();

    for (int i = 0; i < 200 && !relay->m_loaded.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(relay->m_loaded.load());
    assert(relay->m_presetCount.load() == 136);
    assert(relay->presets().size() == 136);
    std::cout << "[PASS] Loaded " << sf2Path << " with " << relay->presets().size() << " presets\n";

    // Select Preset #19 (Church Organ) via IMessage
    auto* presetMsg = allocateMessage(hostApp);
    assert(presetMsg);
    presetMsg->setMessageID(kMsgSelectPreset);
    presetMsg->getAttributes()->setInt(kAttrPresetIndex, 19);
    assert(procConn->notify(presetMsg) == kResultOk);
    presetMsg->release();
    assert(relay->m_selectedPreset.load() == 19);
    std::cout << "[PASS] Preset switched to #19 (" << relay->presets()[19].name << ")\n";

    // Render with Modulation Wheel = 1.0 and ADSR enabled with long Release
    std::vector<float> outL(512, 0.0f), outR(512, 0.0f);
    float* outPtrs[2] = {outL.data(), outR.data()};
    AudioBusBuffers outBus{};
    outBus.numChannels = 2;
    outBus.channelBuffers32 = outPtrs;

    auto* events = new MockEventList();
    Event noteOn{};
    noteOn.busIndex = 0;
    noteOn.sampleOffset = 0;
    noteOn.type = Event::kNoteOnEvent;
    noteOn.noteOn.channel = 0;
    noteOn.noteOn.pitch = 60;
    noteOn.noteOn.velocity = 0.9f;
    noteOn.noteOn.noteId = 1;
    events->addEvent(noteOn);

    auto* paramChanges = new MockParamChanges();
    paramChanges->addSinglePoint(kParamModWheel, 1.0);
    paramChanges->addSinglePoint(kParamAdsrEnabled, 1.0);
    paramChanges->addSinglePoint(kParamAttack, 0.0);
    paramChanges->addSinglePoint(kParamDecay, 0.4);
    paramChanges->addSinglePoint(kParamSustainLevel, 0.85);
    paramChanges->addSinglePoint(kParamRelease, 0.5); // ~634 ms release
    paramChanges->addSinglePoint(kParamCoarseTune, 0.75); // +12 st
    paramChanges->addSinglePoint(kParamFineTune, 0.65);   // +30 ct
    paramChanges->addSinglePoint(kParamPortamentoEnabled, 1.0);
    paramChanges->addSinglePoint(kParamPortamentoMode, 1.0); // Legato
    paramChanges->addSinglePoint(kParamGlideTime, 0.45);
    paramChanges->addSinglePoint(kParamVelP1X, 0.0);
    paramChanges->addSinglePoint(kParamVelP1Y, 1.0);
    paramChanges->addSinglePoint(kParamVelP2X, 1.0);
    paramChanges->addSinglePoint(kParamVelP2Y, 1.0); // Fixed 127 velocity curve

    ProcessData pdata{};
    pdata.processMode = kRealtime;
    pdata.symbolicSampleSize = kSample32;
    pdata.numSamples = 512;
    pdata.numOutputs = 1;
    pdata.outputs = &outBus;
    pdata.inputEvents = events;
    pdata.inputParameterChanges = paramChanges;

    assert(audio->process(pdata) == kResultOk);
    events->clear();
    paramChanges->clear();

    for (int block = 0; block < 8; ++block) {
        assert(audio->process(pdata) == kResultOk);
    }
    const float rmsModOn = computeRms(outL);
    assert(rmsModOn > 0.005f && std::isfinite(rmsModOn));
    std::cout << "[PASS] Organ + ModWheel + ADSR rendered, RMS=" << rmsModOn << "\n";

    // Test Legato glide to E4 (64) while C4 (60) is still held
    Event legatoOn = noteOn;
    legatoOn.noteOn.pitch = 64;
    legatoOn.noteOn.noteId = 2;
    events->addEvent(legatoOn);
    assert(audio->process(pdata) == kResultOk);
    events->clear();
    for (int block = 0; block < 4; ++block) {
        assert(audio->process(pdata) == kResultOk);
    }
    const float rmsLegato = computeRms(outL);
    assert(rmsLegato > 0.005f && std::isfinite(rmsLegato));
    std::cout << "[PASS] Legato portamento glide rendered, RMS=" << rmsLegato << "\n";

    // Release E4 while C4 still held (should glide back and stay audible), then release C4
    Event noteOff64{};
    noteOff64.busIndex = 0;
    noteOff64.sampleOffset = 0;
    noteOff64.type = Event::kNoteOffEvent;
    noteOff64.noteOff.channel = 0;
    noteOff64.noteOff.pitch = 64;
    noteOff64.noteOff.velocity = 0.0f;
    noteOff64.noteOff.noteId = 2;
    events->addEvent(noteOff64);
    assert(audio->process(pdata) == kResultOk);
    events->clear();

    // Release note and verify ADSR Release tail keeps sounding smoothly
    Event noteOff{};
    noteOff.busIndex = 0;
    noteOff.sampleOffset = 0;
    noteOff.type = Event::kNoteOffEvent;
    noteOff.noteOff.channel = 0;
    noteOff.noteOff.pitch = 60;
    noteOff.noteOff.velocity = 0.0f;
    noteOff.noteOff.noteId = 1;
    events->addEvent(noteOff);
    assert(audio->process(pdata) == kResultOk);
    events->clear();
    for (int block = 0; block < 4; ++block) {
        assert(audio->process(pdata) == kResultOk);
    }
    const float rmsTail = computeRms(outL);
    assert(rmsTail > 0.002f && "Expected audible ADSR release tail after NoteOff");
    std::cout << "[PASS] ADSR release tail verified, RMS=" << rmsTail << "\n";

    // Save state and restore into fresh Processor & Controller
    auto* stream = new MemoryStream();
    assert(comp->getState(stream) == kResultOk);
    stream->rewind();

    IComponent* comp2 = nullptr;
    assert(factory->createInstance(procUid, IComponent::iid, reinterpret_cast<void**>(&comp2)) == kResultOk);
    assert(comp2->initialize(hostApp) == kResultOk);
    IConnectionPoint* procConn2 = nullptr;
    assert(comp2->queryInterface(IConnectionPoint::iid, reinterpret_cast<void**>(&procConn2)) == kResultOk);

    IEditController* ctrl2 = nullptr;
    assert(factory->createInstance(ctrlUid, IEditController::iid, reinterpret_cast<void**>(&ctrl2)) == kResultOk);
    assert(ctrl2->initialize(hostApp) == kResultOk);
    IConnectionPoint* ctrlConn2 = nullptr;
    assert(ctrl2->queryInterface(IConnectionPoint::iid, reinterpret_cast<void**>(&ctrlConn2)) == kResultOk);
    auto* relay2 = new ConnectionRelay(ctrlConn2);
    assert(procConn2->connect(relay2) == kResultOk);

    assert(comp2->setState(stream) == kResultOk);
    stream->rewind();
    assert(ctrl2->setComponentState(stream) == kResultOk);

    assert(std::abs(ctrl2->getParamNormalized(kParamModWheel) - 1.0) < 1e-6);
    assert(std::abs(ctrl2->getParamNormalized(kParamAdsrEnabled) - 1.0) < 1e-6);
    assert(std::abs(ctrl2->getParamNormalized(kParamRelease) - 0.5) < 1e-6);
    assert(std::abs(ctrl2->getParamNormalized(kParamCoarseTune) - 0.75) < 1e-6);
    assert(std::abs(ctrl2->getParamNormalized(kParamFineTune) - 0.65) < 1e-6);
    assert(std::abs(ctrl2->getParamNormalized(kParamPortamentoEnabled) - 1.0) < 1e-6);
    assert(std::abs(ctrl2->getParamNormalized(kParamPortamentoMode) - 1.0) < 1e-6);
    assert(std::abs(ctrl2->getParamNormalized(kParamGlideTime) - 0.45) < 1e-6);
    assert(std::abs(ctrl2->getParamNormalized(kParamVelP1X) - 0.0) < 1e-6);
    assert(std::abs(ctrl2->getParamNormalized(kParamVelP1Y) - 1.0) < 1e-6);
    assert(std::abs(ctrl2->getParamNormalized(kParamVelP2X) - 1.0) < 1e-6);
    assert(std::abs(ctrl2->getParamNormalized(kParamVelP2Y) - 1.0) < 1e-6);

    // Also verify VelocityCurve math directly (Linear, Soft, Hard, Fixed)
    VelocityCurve linCurve{0.33, 0.33, 0.67, 0.67};
    assert(std::abs(linCurve.evaluate(0.5f) - 0.5f) < 0.01f);
    VelocityCurve softCurve{0.42, 0.14, 0.78, 0.48};
    assert(softCurve.evaluate(0.5f) < 0.35f);
    VelocityCurve hardCurve{0.22, 0.52, 0.58, 0.86};
    assert(hardCurve.evaluate(0.5f) > 0.70f);
    VelocityCurve fixCurve{0.0, 0.75, 1.0, 0.75};
    assert(std::abs(fixCurve.evaluate(0.1f) - 0.75f) < 1e-4f);
    assert(std::abs(fixCurve.evaluate(0.9f) - 0.75f) < 1e-4f);

    for (int i = 0; i < 200 && !relay2->m_loaded.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(relay2->m_loaded.load());
    assert(relay2->m_selectedPreset.load() == 19);
    std::cout << "[PASS] State save/restore (preset=19, modWheel, tuning, portamento, ADSR) verified\n";

    // Cleanup
    stream->release();
    events->release();
    paramChanges->release();

    procConn2->disconnect(relay2);
    relay2->release();
    procConn2->release();
    ctrlConn2->release();
    ctrl2->terminate();
    ctrl2->release();
    comp2->terminate();
    comp2->release();

    audio->setProcessing(0);
    comp->setActive(0);
    audio->release();
    procConn->disconnect(relay);
    ctrlConn->disconnect(procConn);
    relay->release();
    procConn->release();
    ctrlConn->release();
    ctrl->terminate();
    ctrl->release();
    comp->terminate();
    comp->release();

    hostApp->release();
    factory->release();
    assert(moduleExit());
    dlclose(handle);

    std::cout << "All VST3 tests passed!\n";
    return 0;
}
