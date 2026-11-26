#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

namespace Steinberg {

DEF_CLASS_IID(IPlugView)
DEF_CLASS_IID(IPlugFrame)

namespace Linux {
DEF_CLASS_IID(IEventHandler)
DEF_CLASS_IID(ITimerHandler)
DEF_CLASS_IID(IRunLoop)
} // namespace Linux

namespace Vst {
DEF_CLASS_IID(IComponent)
DEF_CLASS_IID(IAudioProcessor)
DEF_CLASS_IID(IProcessContextRequirements)
DEF_CLASS_IID(IEditController)
DEF_CLASS_IID(IEditController2)
DEF_CLASS_IID(IMidiMapping)
DEF_CLASS_IID(IComponentHandler)
DEF_CLASS_IID(IComponentHandler2)
DEF_CLASS_IID(IEventList)
DEF_CLASS_IID(IParameterChanges)
DEF_CLASS_IID(IParamValueQueue)
DEF_CLASS_IID(IMessage)
DEF_CLASS_IID(IConnectionPoint)
DEF_CLASS_IID(IAttributeList)
DEF_CLASS_IID(IStreamAttributes)
DEF_CLASS_IID(IHostApplication)
} // namespace Vst

} // namespace Steinberg
