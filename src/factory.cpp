#include "controller.h"
#include "plugin_ids.h"
#include "processor.h"

#include "pluginterfaces/base/fstrdefs.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <cstring>

namespace SuperSimpleSF2 {

using namespace Steinberg;
using namespace Steinberg::Vst;

class PluginFactory;
static PluginFactory* gFactory = nullptr;

class PluginFactory : public IPluginFactory3 {
public:
    PluginFactory() {
        FUNKNOWN_CTOR
    }
    virtual ~PluginFactory();

    DECLARE_FUNKNOWN_METHODS

    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) SMTG_OVERRIDE {
        if (!info) return kInvalidArgument;
        *info = PFactoryInfo(
            "Alan Telles",
            "https://github.com/alantelles/at-sf2-player",
            "mailto:support@example.com",
            PFactoryInfo::kUnicode);
        return kResultOk;
    }

    int32 PLUGIN_API countClasses() SMTG_OVERRIDE {
        return 2;
    }

    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) SMTG_OVERRIDE {
        if (!info) return kInvalidArgument;
        if (index == 0) {
            TUID uid;
            kProcessorUID.toTUID(uid);
            *info = PClassInfo(uid, PClassInfo::kManyInstances, kVstAudioEffectClass, "AT SF2 Player");
            return kResultOk;
        }
        if (index == 1) {
            TUID uid;
            kControllerUID.toTUID(uid);
            *info = PClassInfo(uid, PClassInfo::kManyInstances, kVstComponentControllerClass, "AT SF2 Player Controller");
            return kResultOk;
        }
        return kInvalidArgument;
    }

    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) SMTG_OVERRIDE {
        if (!info) return kInvalidArgument;
        if (index == 0) {
            TUID uid;
            kProcessorUID.toTUID(uid);
            *info = PClassInfo2(
                uid, PClassInfo::kManyInstances, kVstAudioEffectClass,
                "AT SF2 Player", 0, PlugType::kInstrumentSampler,
                "Alan Telles", "1.0.0.0", "VST 3.7");
            return kResultOk;
        }
        if (index == 1) {
            TUID uid;
            kControllerUID.toTUID(uid);
            *info = PClassInfo2(
                uid, PClassInfo::kManyInstances, kVstComponentControllerClass,
                "AT SF2 Player Controller", 0, "",
                "Alan Telles", "1.0.0.0", "VST 3.7");
            return kResultOk;
        }
        return kInvalidArgument;
    }

    tresult PLUGIN_API getClassInfoUnicode(int32 index, PClassInfoW* info) SMTG_OVERRIDE {
        if (!info) return kInvalidArgument;
        PClassInfo2 ci2;
        if (getClassInfo2(index, &ci2) != kResultOk) return kInvalidArgument;
        info->fromAscii(ci2);
        return kResultOk;
    }

    tresult PLUGIN_API setHostContext(FUnknown* /*context*/) SMTG_OVERRIDE {
        return kResultOk;
    }

    tresult PLUGIN_API createInstance(FIDString cid, FIDString _iid, void** obj) SMTG_OVERRIDE {
        if (!cid || !_iid || !obj) return kInvalidArgument;
        *obj = nullptr;

        TUID procUid;
        TUID ctrlUid;
        kProcessorUID.toTUID(procUid);
        kControllerUID.toTUID(ctrlUid);

        FUnknown* instance = nullptr;
        if (FUnknownPrivate::iidEqual(cid, procUid)) {
            instance = Processor::createInstance(nullptr);
        } else if (FUnknownPrivate::iidEqual(cid, ctrlUid)) {
            instance = Controller::createInstance(nullptr);
        } else {
            return kNoInterface;
        }

        if (!instance) return kOutOfMemory;
        const tresult res = instance->queryInterface(_iid, obj);
        instance->release();
        return res;
    }
};

IMPLEMENT_REFCOUNT(PluginFactory)

tresult PLUGIN_API PluginFactory::queryInterface(const TUID _iid, void** obj) {
    QUERY_INTERFACE(_iid, obj, FUnknown::iid, IPluginFactory)
    QUERY_INTERFACE(_iid, obj, IPluginFactory::iid, IPluginFactory)
    QUERY_INTERFACE(_iid, obj, IPluginFactory2::iid, IPluginFactory2)
    QUERY_INTERFACE(_iid, obj, IPluginFactory3::iid, IPluginFactory3)
    *obj = nullptr;
    return kNoInterface;
}

PluginFactory::~PluginFactory() {
    FUNKNOWN_DTOR
    if (gFactory == this) {
        gFactory = nullptr;
    }
}

} // namespace SuperSimpleSF2

extern "C" {

SMTG_EXPORT_SYMBOL bool PLUGIN_API ModuleEntry(void* /*sharedLibraryHandle*/) {
    return true;
}

SMTG_EXPORT_SYMBOL bool PLUGIN_API ModuleExit() {
    if (SuperSimpleSF2::gFactory) {
        SuperSimpleSF2::gFactory->release();
        SuperSimpleSF2::gFactory = nullptr;
    }
    return true;
}

SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* PLUGIN_API GetPluginFactory() {
    if (!SuperSimpleSF2::gFactory) {
        SuperSimpleSF2::gFactory = new SuperSimpleSF2::PluginFactory();
    } else {
        SuperSimpleSF2::gFactory->addRef();
    }
    return SuperSimpleSF2::gFactory;
}

} // extern "C"
