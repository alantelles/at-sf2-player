#pragma once

#include "plugin_ids.h"
#include "synth_engine.h"

#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include <mutex>
#include <string>

namespace SuperSimpleSF2 {

class X11View;

class Controller : public Steinberg::Vst::IEditController,
                   public Steinberg::Vst::IMidiMapping,
                   public Steinberg::Vst::IConnectionPoint {
public:
    Controller();
    virtual ~Controller();

    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IEditController*>(new Controller());
    }

    // FUnknown
    DECLARE_FUNKNOWN_METHODS

    // IPluginBase
    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API terminate() SMTG_OVERRIDE;

    // IEditController
    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::int32 PLUGIN_API getParameterCount() SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getParameterInfo(Steinberg::int32 paramIndex,
                                                   Steinberg::Vst::ParameterInfo& info) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getParamStringByValue(Steinberg::Vst::ParamID id,
                                                        Steinberg::Vst::ParamValue valueNormalized,
                                                        Steinberg::Vst::String128 string) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getParamValueByString(Steinberg::Vst::ParamID id,
                                                        Steinberg::Vst::TChar* string,
                                                        Steinberg::Vst::ParamValue& valueNormalized) SMTG_OVERRIDE;
    Steinberg::Vst::ParamValue PLUGIN_API normalizedParamToPlain(Steinberg::Vst::ParamID id,
                                                                 Steinberg::Vst::ParamValue valueNormalized) SMTG_OVERRIDE;
    Steinberg::Vst::ParamValue PLUGIN_API plainParamToNormalized(Steinberg::Vst::ParamID id,
                                                                 Steinberg::Vst::ParamValue plainValue) SMTG_OVERRIDE;
    Steinberg::Vst::ParamValue PLUGIN_API getParamNormalized(Steinberg::Vst::ParamID id) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setParamNormalized(Steinberg::Vst::ParamID id,
                                                     Steinberg::Vst::ParamValue value) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setComponentHandler(Steinberg::Vst::IComponentHandler* handler) SMTG_OVERRIDE;
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) SMTG_OVERRIDE;

    // IMidiMapping
    Steinberg::tresult PLUGIN_API getMidiControllerAssignment(Steinberg::int32 busIndex,
                                                              Steinberg::int16 channel,
                                                              Steinberg::Vst::CtrlNumber midiControllerNumber,
                                                              Steinberg::Vst::ParamID& id) SMTG_OVERRIDE;

    // IConnectionPoint
    Steinberg::tresult PLUGIN_API connect(Steinberg::Vst::IConnectionPoint* other) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API disconnect(Steinberg::Vst::IConnectionPoint* other) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) SMTG_OVERRIDE;

    // Helpers called by X11View
    void beginParamEdit(Steinberg::Vst::ParamID id);
    void setParamFromView(Steinberg::Vst::ParamID id, double valueNormalized);
    void endParamEdit(Steinberg::Vst::ParamID id);
    void requestLoadSf2(const std::string& path);
    void requestSelectPreset(int presetIndex);

    Sf2Status getUiStatus() const;
    void detachView(X11View* view);

private:
    void notifyView();
    void markHostDirty();

    Steinberg::Vst::IHostApplication* m_hostApp = nullptr;
    Steinberg::Vst::IComponentHandler* m_componentHandler = nullptr;
    Steinberg::Vst::IConnectionPoint* m_peer = nullptr;

    mutable std::mutex m_mutex;
    double m_volumeNorm = kDefaultVolumeNorm;
    double m_pitchBendNorm = kDefaultPitchBendNorm;
    double m_sustainNorm = kDefaultSustainNorm;
    double m_modWheelNorm = kDefaultModWheelNorm;
    double m_adsrEnabledNorm = kDefaultAdsrEnabledNorm;
    double m_attackNorm = kDefaultAttackNorm;
    double m_decayNorm = kDefaultDecayNorm;
    double m_sustainLevelNorm = kDefaultSustainLevelNorm;
    double m_releaseNorm = kDefaultReleaseNorm;
    double m_coarseTuneNorm = kDefaultCoarseTuneNorm;
    double m_fineTuneNorm = kDefaultFineTuneNorm;
    double m_portaEnabledNorm = kDefaultPortamentoEnabledNorm;
    double m_portaModeNorm = kDefaultPortamentoModeNorm;
    double m_glideTimeNorm = kDefaultGlideTimeNorm;
    double m_velP1X = kDefaultVelP1X;
    double m_velP1Y = kDefaultVelP1Y;
    double m_velP2X = kDefaultVelP2X;
    double m_velP2Y = kDefaultVelP2Y;

    Sf2Status m_status;
    X11View* m_activeView = nullptr;
};

} // namespace SuperSimpleSF2
