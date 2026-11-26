#include "controller.h"
#include "view_x11.h"

#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace SuperSimpleSF2 {

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

void formatTimeSec(float sec, char* buf, size_t bufSize) {
    if (sec < 1.0f) {
        std::snprintf(buf, bufSize, "%.0f ms", sec * 1000.0f);
    } else {
        std::snprintf(buf, bufSize, "%.2f s", sec);
    }
}

} // namespace

Controller::Controller() {
    FUNKNOWN_CTOR
}

Controller::~Controller() {
    FUNKNOWN_DTOR
    if (m_peer) {
        m_peer->release();
        m_peer = nullptr;
    }
    if (m_componentHandler) {
        m_componentHandler->release();
        m_componentHandler = nullptr;
    }
    if (m_hostApp) {
        m_hostApp->release();
        m_hostApp = nullptr;
    }
}

IMPLEMENT_REFCOUNT(Controller)

tresult PLUGIN_API Controller::queryInterface(const TUID _iid, void** obj) {
    QUERY_INTERFACE(_iid, obj, FUnknown::iid, IEditController)
    QUERY_INTERFACE(_iid, obj, IPluginBase::iid, IEditController)
    QUERY_INTERFACE(_iid, obj, IEditController::iid, IEditController)
    QUERY_INTERFACE(_iid, obj, IMidiMapping::iid, IMidiMapping)
    QUERY_INTERFACE(_iid, obj, IConnectionPoint::iid, IConnectionPoint)
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API Controller::initialize(FUnknown* context) {
    if (m_hostApp) {
        m_hostApp->release();
        m_hostApp = nullptr;
    }
    if (context) {
        context->queryInterface(IHostApplication::iid, reinterpret_cast<void**>(&m_hostApp));
    }
    return kResultOk;
}

tresult PLUGIN_API Controller::terminate() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_activeView = nullptr;
    }
    if (m_peer) {
        m_peer->release();
        m_peer = nullptr;
    }
    if (m_componentHandler) {
        m_componentHandler->release();
        m_componentHandler = nullptr;
    }
    if (m_hostApp) {
        m_hostApp->release();
        m_hostApp = nullptr;
    }
    return kResultOk;
}

tresult PLUGIN_API Controller::setComponentState(IBStream* state) {
    PluginStateData data;
    if (!data.readFromStream(state)) return kResultFalse;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_volumeNorm = data.volumeNorm;
        m_modWheelNorm = data.modWheelNorm;
        m_adsrEnabledNorm = data.adsr.enabled ? 1.0 : 0.0;
        m_attackNorm = data.adsr.attackNorm;
        m_decayNorm = data.adsr.decayNorm;
        m_sustainLevelNorm = data.adsr.sustainNorm;
        m_releaseNorm = data.adsr.releaseNorm;
        m_coarseTuneNorm = data.coarseTuneNorm;
        m_fineTuneNorm = data.fineTuneNorm;
        m_portaEnabledNorm = data.portamento.enabled ? 1.0 : 0.0;
        m_portaModeNorm = data.portamento.legato ? 1.0 : 0.0;
        m_glideTimeNorm = data.portamento.glideNorm;
        m_velP1X = data.velocityCurve.p1x;
        m_velP1Y = data.velocityCurve.p1y;
        m_velP2X = data.velocityCurve.p2x;
        m_velP2Y = data.velocityCurve.p2y;
        m_status.selectedPresetIndex = data.selectedPresetIndex;
        if (m_status.path != data.sf2Path) {
            m_status.path = data.sf2Path;
            m_status.loaded = false;
            m_status.loading = !data.sf2Path.empty();
            m_status.presets.clear();
            m_status.error.clear();
        }
    }
    notifyView();
    return kResultOk;
}

tresult PLUGIN_API Controller::setState(IBStream* /*state*/) {
    return kResultOk;
}

tresult PLUGIN_API Controller::getState(IBStream* /*state*/) {
    return kResultOk;
}

int32 PLUGIN_API Controller::getParameterCount() {
    return 18;
}

tresult PLUGIN_API Controller::getParameterInfo(int32 paramIndex, ParameterInfo& info) {
    std::memset(&info, 0, sizeof(info));
    switch (paramIndex) {
        case 0:
            info.id = kParamVolume;
            UString128("Volume").copyTo(info.title, 128);
            UString128("Vol").copyTo(info.shortTitle, 128);
            UString128("%").copyTo(info.units, 128);
            info.stepCount = 0;
            info.defaultNormalizedValue = kDefaultVolumeNorm;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        case 1:
            info.id = kParamPitchBend;
            UString128("Pitch Bend").copyTo(info.title, 128);
            UString128("Pitch").copyTo(info.shortTitle, 128);
            info.stepCount = 0;
            info.defaultNormalizedValue = kDefaultPitchBendNorm;
            info.flags = ParameterInfo::kNoFlags;
            return kResultOk;
        case 2:
            info.id = kParamSustain;
            UString128("Sustain Pedal").copyTo(info.title, 128);
            UString128("Pedal").copyTo(info.shortTitle, 128);
            info.stepCount = 1;
            info.defaultNormalizedValue = kDefaultSustainNorm;
            info.flags = ParameterInfo::kNoFlags;
            return kResultOk;
        case 3:
            info.id = kParamModWheel;
            UString128("Modulation").copyTo(info.title, 128);
            UString128("Mod").copyTo(info.shortTitle, 128);
            UString128("%").copyTo(info.units, 128);
            info.stepCount = 0;
            info.defaultNormalizedValue = kDefaultModWheelNorm;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        case 4:
            info.id = kParamAdsrEnabled;
            UString128("ADSR Override").copyTo(info.title, 128);
            UString128("ADSR").copyTo(info.shortTitle, 128);
            info.stepCount = 1;
            info.defaultNormalizedValue = kDefaultAdsrEnabledNorm;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        case 5:
            info.id = kParamAttack;
            UString128("Attack").copyTo(info.title, 128);
            UString128("Atk").copyTo(info.shortTitle, 128);
            UString128("s").copyTo(info.units, 128);
            info.stepCount = 0;
            info.defaultNormalizedValue = kDefaultAttackNorm;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        case 6:
            info.id = kParamDecay;
            UString128("Decay").copyTo(info.title, 128);
            UString128("Dec").copyTo(info.shortTitle, 128);
            UString128("s").copyTo(info.units, 128);
            info.stepCount = 0;
            info.defaultNormalizedValue = kDefaultDecayNorm;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        case 7:
            info.id = kParamSustainLevel;
            UString128("Sustain").copyTo(info.title, 128);
            UString128("Sus").copyTo(info.shortTitle, 128);
            UString128("%").copyTo(info.units, 128);
            info.stepCount = 0;
            info.defaultNormalizedValue = kDefaultSustainLevelNorm;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        case 8:
            info.id = kParamRelease;
            UString128("Release").copyTo(info.title, 128);
            UString128("Rel").copyTo(info.shortTitle, 128);
            UString128("s").copyTo(info.units, 128);
            info.stepCount = 0;
            info.defaultNormalizedValue = kDefaultReleaseNorm;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        case 9:
            info.id = kParamCoarseTune;
            UString128("Coarse Tune").copyTo(info.title, 128);
            UString128("Coarse").copyTo(info.shortTitle, 128);
            UString128("st").copyTo(info.units, 128);
            info.stepCount = 48;
            info.defaultNormalizedValue = kDefaultCoarseTuneNorm;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        case 10:
            info.id = kParamFineTune;
            UString128("Fine Tune").copyTo(info.title, 128);
            UString128("Fine").copyTo(info.shortTitle, 128);
            UString128("ct").copyTo(info.units, 128);
            info.stepCount = 0;
            info.defaultNormalizedValue = kDefaultFineTuneNorm;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        case 11:
            info.id = kParamPortamentoEnabled;
            UString128("Portamento").copyTo(info.title, 128);
            UString128("Porta").copyTo(info.shortTitle, 128);
            info.stepCount = 1;
            info.defaultNormalizedValue = kDefaultPortamentoEnabledNorm;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        case 12:
            info.id = kParamPortamentoMode;
            UString128("Glide Mode").copyTo(info.title, 128);
            UString128("Mode").copyTo(info.shortTitle, 128);
            info.stepCount = 1;
            info.defaultNormalizedValue = kDefaultPortamentoModeNorm;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        case 13:
            info.id = kParamGlideTime;
            UString128("Glide Time").copyTo(info.title, 128);
            UString128("Glide").copyTo(info.shortTitle, 128);
            UString128("s").copyTo(info.units, 128);
            info.stepCount = 0;
            info.defaultNormalizedValue = kDefaultGlideTimeNorm;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        case 14:
            info.id = kParamVelP1X;
            UString128("Vel P1 In").copyTo(info.title, 128);
            UString128("VP1X").copyTo(info.shortTitle, 128);
            UString128("%").copyTo(info.units, 128);
            info.stepCount = 0;
            info.defaultNormalizedValue = kDefaultVelP1X;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        case 15:
            info.id = kParamVelP1Y;
            UString128("Vel P1 Out").copyTo(info.title, 128);
            UString128("VP1Y").copyTo(info.shortTitle, 128);
            UString128("%").copyTo(info.units, 128);
            info.stepCount = 0;
            info.defaultNormalizedValue = kDefaultVelP1Y;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        case 16:
            info.id = kParamVelP2X;
            UString128("Vel P2 In").copyTo(info.title, 128);
            UString128("VP2X").copyTo(info.shortTitle, 128);
            UString128("%").copyTo(info.units, 128);
            info.stepCount = 0;
            info.defaultNormalizedValue = kDefaultVelP2X;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        case 17:
            info.id = kParamVelP2Y;
            UString128("Vel P2 Out").copyTo(info.title, 128);
            UString128("VP2Y").copyTo(info.shortTitle, 128);
            UString128("%").copyTo(info.units, 128);
            info.stepCount = 0;
            info.defaultNormalizedValue = kDefaultVelP2Y;
            info.flags = ParameterInfo::kCanAutomate;
            return kResultOk;
        default:
            return kInvalidArgument;
    }
}

tresult PLUGIN_API Controller::getParamStringByValue(ParamID id, ParamValue valueNormalized, String128 string) {
    char buf[64] = {};
    switch (id) {
        case kParamVolume:
        case kParamModWheel:
        case kParamSustainLevel:
        case kParamVelP1X:
        case kParamVelP1Y:
        case kParamVelP2X:
        case kParamVelP2Y:
            std::snprintf(buf, sizeof(buf), "%.0f%%", valueNormalized * 100.0);
            UString128(buf).copyTo(string, 128);
            return kResultOk;
        case kParamPitchBend:
            std::snprintf(buf, sizeof(buf), "%+.2f", (valueNormalized - 0.5) * 2.0);
            UString128(buf).copyTo(string, 128);
            return kResultOk;
        case kParamSustain:
        case kParamAdsrEnabled:
            UString128(valueNormalized >= 0.5 ? "On" : "Off").copyTo(string, 128);
            return kResultOk;
        case kParamAttack:
            formatTimeSec(attackNormToSec(valueNormalized), buf, sizeof(buf));
            UString128(buf).copyTo(string, 128);
            return kResultOk;
        case kParamDecay:
            formatTimeSec(decayNormToSec(valueNormalized), buf, sizeof(buf));
            UString128(buf).copyTo(string, 128);
            return kResultOk;
        case kParamRelease:
            formatTimeSec(releaseNormToSec(valueNormalized), buf, sizeof(buf));
            UString128(buf).copyTo(string, 128);
            return kResultOk;
        case kParamCoarseTune: {
            const int st = coarseNormToSemitones(valueNormalized);
            std::snprintf(buf, sizeof(buf), "%+d st", st);
            UString128(buf).copyTo(string, 128);
            return kResultOk;
        }
        case kParamFineTune: {
            const float ct = fineNormToCents(valueNormalized);
            std::snprintf(buf, sizeof(buf), "%+.0f ct", ct);
            UString128(buf).copyTo(string, 128);
            return kResultOk;
        }
        case kParamPortamentoEnabled:
            UString128(valueNormalized >= 0.5 ? "On" : "Off").copyTo(string, 128);
            return kResultOk;
        case kParamPortamentoMode:
            UString128(valueNormalized >= 0.5 ? "Legato" : "Retrigger").copyTo(string, 128);
            return kResultOk;
        case kParamGlideTime:
            formatTimeSec(glideNormToSec(valueNormalized), buf, sizeof(buf));
            UString128(buf).copyTo(string, 128);
            return kResultOk;
        default:
            return kInvalidArgument;
    }
}

tresult PLUGIN_API Controller::getParamValueByString(ParamID id, TChar* string, ParamValue& valueNormalized) {
    if (!string) return kInvalidArgument;
    UString wrapper(string, 128);
    double val = 0.0;
    if ((id == kParamVolume || id == kParamModWheel || id == kParamSustainLevel) && wrapper.scanFloat(val)) {
        valueNormalized = std::clamp(val / 100.0, 0.0, 1.0);
        return kResultOk;
    }
    return kResultFalse;
}

ParamValue PLUGIN_API Controller::normalizedParamToPlain(ParamID id, ParamValue valueNormalized) {
    switch (id) {
        case kParamVolume:
        case kParamModWheel:
        case kParamSustainLevel:
            return valueNormalized * 100.0;
        case kParamPitchBend:
            return (valueNormalized - 0.5) * 2.0;
        case kParamAttack:
            return attackNormToSec(valueNormalized);
        case kParamDecay:
            return decayNormToSec(valueNormalized);
        case kParamRelease:
            return releaseNormToSec(valueNormalized);
        case kParamCoarseTune:
            return static_cast<ParamValue>(coarseNormToSemitones(valueNormalized));
        case kParamFineTune:
            return static_cast<ParamValue>(fineNormToCents(valueNormalized));
        case kParamGlideTime:
            return glideNormToSec(valueNormalized);
        default:
            return valueNormalized;
    }
}

ParamValue PLUGIN_API Controller::plainParamToNormalized(ParamID id, ParamValue plainValue) {
    switch (id) {
        case kParamVolume:
        case kParamModWheel:
        case kParamSustainLevel:
            return std::clamp(plainValue / 100.0, 0.0, 1.0);
        case kParamPitchBend:
            return std::clamp((plainValue * 0.5) + 0.5, 0.0, 1.0);
        case kParamAttack:
            return std::cbrt(std::clamp((plainValue - 0.002) / 2.998, 0.0, 1.0));
        case kParamDecay:
            return std::cbrt(std::clamp((plainValue - 0.010) / 4.990, 0.0, 1.0));
        case kParamRelease:
            return std::cbrt(std::clamp((plainValue - 0.012) / 4.988, 0.0, 1.0));
        case kParamCoarseTune:
            return std::clamp((plainValue + 24.0) / 48.0, 0.0, 1.0);
        case kParamFineTune:
            return std::clamp((plainValue + 100.0) / 200.0, 0.0, 1.0);
        case kParamGlideTime:
            return std::cbrt(std::clamp((plainValue - 0.005) / 1.995, 0.0, 1.0));
        default:
            return std::clamp(plainValue, 0.0, 1.0);
    }
}

ParamValue PLUGIN_API Controller::getParamNormalized(ParamID id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    switch (id) {
        case kParamVolume: return m_volumeNorm;
        case kParamPitchBend: return m_pitchBendNorm;
        case kParamSustain: return m_sustainNorm;
        case kParamModWheel: return m_modWheelNorm;
        case kParamAdsrEnabled: return m_adsrEnabledNorm;
        case kParamAttack: return m_attackNorm;
        case kParamDecay: return m_decayNorm;
        case kParamSustainLevel: return m_sustainLevelNorm;
        case kParamRelease: return m_releaseNorm;
        case kParamCoarseTune: return m_coarseTuneNorm;
        case kParamFineTune: return m_fineTuneNorm;
        case kParamPortamentoEnabled: return m_portaEnabledNorm;
        case kParamPortamentoMode: return m_portaModeNorm;
        case kParamGlideTime: return m_glideTimeNorm;
        case kParamVelP1X: return m_velP1X;
        case kParamVelP1Y: return m_velP1Y;
        case kParamVelP2X: return m_velP2X;
        case kParamVelP2Y: return m_velP2Y;
        default: return 0.0;
    }
}

tresult PLUGIN_API Controller::setParamNormalized(ParamID id, ParamValue value) {
    value = std::clamp(value, 0.0, 1.0);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        switch (id) {
            case kParamVolume: m_volumeNorm = value; break;
            case kParamPitchBend: m_pitchBendNorm = value; break;
            case kParamSustain: m_sustainNorm = value; break;
            case kParamModWheel: m_modWheelNorm = value; break;
            case kParamAdsrEnabled: m_adsrEnabledNorm = value; break;
            case kParamAttack: m_attackNorm = value; break;
            case kParamDecay: m_decayNorm = value; break;
            case kParamSustainLevel: m_sustainLevelNorm = value; break;
            case kParamRelease: m_releaseNorm = value; break;
            case kParamCoarseTune: m_coarseTuneNorm = value; break;
            case kParamFineTune: m_fineTuneNorm = value; break;
            case kParamPortamentoEnabled: m_portaEnabledNorm = value; break;
            case kParamPortamentoMode: m_portaModeNorm = value; break;
            case kParamGlideTime: m_glideTimeNorm = value; break;
            case kParamVelP1X: m_velP1X = value; break;
            case kParamVelP1Y: m_velP1Y = value; break;
            case kParamVelP2X: m_velP2X = value; break;
            case kParamVelP2Y: m_velP2Y = value; break;
            default: return kInvalidArgument;
        }
    }
    notifyView();
    return kResultOk;
}

tresult PLUGIN_API Controller::setComponentHandler(IComponentHandler* handler) {
    if (m_componentHandler) {
        m_componentHandler->release();
        m_componentHandler = nullptr;
    }
    m_componentHandler = handler;
    if (m_componentHandler) {
        m_componentHandler->addRef();
    }
    return kResultOk;
}

IPlugView* PLUGIN_API Controller::createView(FIDString name) {
    if (name && std::strcmp(name, ViewType::kEditor) == 0) {
        auto* view = new X11View(this);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_activeView = view;
        return view;
    }
    return nullptr;
}

tresult PLUGIN_API Controller::getMidiControllerAssignment(int32 busIndex, int16 /*channel*/,
                                                         CtrlNumber midiControllerNumber,
                                                         ParamID& id) {
    if (busIndex != 0) return kResultFalse;
    if (midiControllerNumber == kCtrlVolume) {
        id = kParamVolume;
        return kResultTrue;
    }
    if (midiControllerNumber == kPitchBend) {
        id = kParamPitchBend;
        return kResultTrue;
    }
    if (midiControllerNumber == kCtrlSustainOnOff) {
        id = kParamSustain;
        return kResultTrue;
    }
    if (midiControllerNumber == kCtrlModWheel) {
        id = kParamModWheel;
        return kResultTrue;
    }
    if (midiControllerNumber == kCtrlPortaOnOff) {
        id = kParamPortamentoEnabled;
        return kResultTrue;
    }
    if (midiControllerNumber == kCtrlPortaTime) {
        id = kParamGlideTime;
        return kResultTrue;
    }
    return kResultFalse;
}

tresult PLUGIN_API Controller::connect(IConnectionPoint* other) {
    if (!other) return kInvalidArgument;
    if (m_peer) m_peer->release();
    m_peer = other;
    m_peer->addRef();
    return kResultOk;
}

tresult PLUGIN_API Controller::disconnect(IConnectionPoint* other) {
    if (m_peer && (!other || m_peer == other)) {
        m_peer->release();
        m_peer = nullptr;
        return kResultOk;
    }
    return kResultFalse;
}

tresult PLUGIN_API Controller::notify(IMessage* message) {
    if (!message) return kInvalidArgument;
    const char* msgId = message->getMessageID();
    if (!msgId || std::strcmp(msgId, kMsgSf2Status) != 0) return kResultFalse;

    IAttributeList* attrs = message->getAttributes();
    if (!attrs) return kResultFalse;

    Sf2Status next;
    int64 loaded = 0;
    int64 presetCount = 0;
    int64 presetIndex = 0;
    if (attrs->getInt(kAttrLoaded, loaded) == kResultOk) {
        next.loaded = (loaded != 0);
    }
    if (attrs->getInt(kAttrPresetCount, presetCount) == kResultOk) {
        next.presetCount = static_cast<int>(presetCount);
    }
    if (attrs->getInt(kAttrPresetIndex, presetIndex) == kResultOk) {
        next.selectedPresetIndex = static_cast<int>(presetIndex);
    }

    const void* raw = nullptr;
    uint32 size = 0;
    if (attrs->getBinary(kAttrPath, raw, size) == kResultOk && raw && size > 0) {
        next.path.assign(static_cast<const char*>(raw), size);
    }
    if (attrs->getBinary(kAttrFirstPreset, raw, size) == kResultOk && raw && size > 0) {
        next.firstPresetName.assign(static_cast<const char*>(raw), size);
    }
    if (attrs->getBinary(kAttrPresetBlob, raw, size) == kResultOk && raw && size > 0) {
        next.presets = deserializePresetList(raw, size);
    }
    if (attrs->getBinary(kAttrError, raw, size) == kResultOk && raw && size > 0) {
        next.error.assign(static_cast<const char*>(raw), size);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status = std::move(next);
    }
    notifyView();
    return kResultOk;
}

void Controller::beginParamEdit(ParamID id) {
    if (m_componentHandler) {
        m_componentHandler->beginEdit(id);
    }
}

void Controller::setParamFromView(ParamID id, double valueNormalized) {
    valueNormalized = std::clamp(valueNormalized, 0.0, 1.0);
    setParamNormalized(id, valueNormalized);
    if (m_componentHandler) {
        m_componentHandler->performEdit(id, valueNormalized);
    }
}

void Controller::endParamEdit(ParamID id) {
    if (m_componentHandler) {
        m_componentHandler->endEdit(id);
    }
}

void Controller::requestLoadSf2(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.path = path;
        m_status.loading = true;
        m_status.loaded = false;
        m_status.selectedPresetIndex = 0;
        m_status.presets.clear();
        m_status.error.clear();
    }
    notifyView();

    if (m_peer && m_hostApp) {
        if (IMessage* msg = allocateMessage(m_hostApp)) {
            msg->setMessageID(kMsgLoadSf2);
            if (IAttributeList* attrs = msg->getAttributes()) {
                attrs->setBinary(kAttrPath, path.data(), static_cast<uint32>(path.size()));
            }
            m_peer->notify(msg);
            msg->release();
        }
    }
    markHostDirty();
}

void Controller::requestSelectPreset(int presetIndex) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_status.presetCount > 0) {
            presetIndex = std::clamp(presetIndex, 0, m_status.presetCount - 1);
        } else {
            presetIndex = 0;
        }
        m_status.selectedPresetIndex = presetIndex;
        if (presetIndex < static_cast<int>(m_status.presets.size())) {
            m_status.firstPresetName = m_status.presets[static_cast<size_t>(presetIndex)].name;
        }
    }
    notifyView();

    if (m_peer && m_hostApp) {
        if (IMessage* msg = allocateMessage(m_hostApp)) {
            msg->setMessageID(kMsgSelectPreset);
            if (IAttributeList* attrs = msg->getAttributes()) {
                attrs->setInt(kAttrPresetIndex, presetIndex);
            }
            m_peer->notify(msg);
            msg->release();
        }
    }
    markHostDirty();
}

void Controller::markHostDirty() {
    if (!m_componentHandler) return;
    IComponentHandler2* handler2 = nullptr;
    if (m_componentHandler->queryInterface(IComponentHandler2::iid, reinterpret_cast<void**>(&handler2)) == kResultOk &&
        handler2) {
        handler2->setDirty(1);
        handler2->release();
    }
}

Sf2Status Controller::getUiStatus() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void Controller::detachView(X11View* view) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_activeView == view) {
        m_activeView = nullptr;
    }
}

void Controller::notifyView() {
    X11View* view = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        view = m_activeView;
    }
    if (view) {
        view->requestRedraw();
    }
}

} // namespace SuperSimpleSF2
