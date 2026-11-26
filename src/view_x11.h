#pragma once

#include "plugin_ids.h"
#include "pluginterfaces/gui/iplugview.h"

#include <X11/Xlib.h>
#include <cairo/cairo.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace SuperSimpleSF2 {

class Controller;
struct Sf2Status;

class X11View : public Steinberg::IPlugView,
                public Steinberg::Linux::IEventHandler,
                public Steinberg::Linux::ITimerHandler {
public:
    explicit X11View(Controller* controller);
    virtual ~X11View();

    // FUnknown
    DECLARE_FUNKNOWN_METHODS

    // IPlugView
    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API removed() SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API onWheel(float distance) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16 key, Steinberg::int16 keyCode,
                                            Steinberg::int16 modifiers) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API onKeyUp(Steinberg::char16 key, Steinberg::int16 keyCode,
                                          Steinberg::int16 modifiers) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect* size) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame* frame) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API canResize() SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* rect) SMTG_OVERRIDE;

    // Linux::IEventHandler
    void PLUGIN_API onFDIsSet(Steinberg::Linux::FileDescriptor fd) SMTG_OVERRIDE;

    // Linux::ITimerHandler
    void PLUGIN_API onTimer() SMTG_OVERRIDE;

    void requestRedraw();
    void detachController();

    static constexpr int kWidth = 500;
    static constexpr int kHeight = 478;

private:
    struct Rect {
        double x, y, w, h;
        bool contains(double px, double py) const {
            return px >= x && px <= (x + w) && py >= y && py <= (y + h);
        }
    };

    enum class DropdownMode {
        Closed = 0,
        Bank,
        Patch,
        Search,
    };

    struct SliderSpec {
        Steinberg::Vst::ParamID paramId;
        const char* label;
        double defaultValue;
        bool isAdsr;
        bool isBipolar;
        Rect labelRect;
        Rect trackRect;
        Rect hitRect;
        Rect valueRect;
    };

    struct PickerState {
        std::mutex mutex;
        std::atomic<bool> open{false};
        std::atomic<bool> hasResult{false};
        std::string selectedPath;
    };

    void processXEvents();
    void handleButtonPress(int x, int y, unsigned int button, unsigned long timeMs);
    void handleButtonRelease(int x, int y, unsigned int button);
    void handleMotion(int x, int y);
    bool handleSearchKey(char asciiChar, int virtualKeyCode);
    void draw();
    void drawDropdown(const Sf2Status& status);

    void launchFilePicker();
    void stepBank(int delta);
    void stepPatch(int delta);
    void updateScrollFromMouseY(int y, int totalItems);
    void ensureAdsrEnabledIfNeeded(const SliderSpec& spec);
    void resetTuning();
    void resetEnvelope();
    std::vector<const PresetEntry*> filterPresetsByQuery(const Sf2Status& status) const;
    static double sliderValueFromX(const SliderSpec& spec, int x);
    static const std::array<SliderSpec, 9>& sliders();

    static Rect loadButtonRect();
    static Rect prevBankRect();
    static Rect bankBoxRect();
    static Rect nextBankRect();
    static Rect prevPatchRect();
    static Rect patchBoxRect();
    static Rect nextPatchRect();
    static Rect searchBoxRect();
    static Rect searchClearRect();
    static Rect activePopupRect(DropdownMode mode);
    static Rect resetTuneRect();
    static Rect portaToggleRect();
    static Rect portaModeRect();
    static Rect resetAdsrRect();
    static Rect adsrToggleRect();

    Controller* m_controller = nullptr;
    Steinberg::IPlugFrame* m_plugFrame = nullptr;
    Steinberg::Linux::IRunLoop* m_runLoop = nullptr;

    Display* m_display = nullptr;
    Window m_window = 0;
    cairo_surface_t* m_surface = nullptr;
    cairo_t* m_cr = nullptr;

    bool m_attached = false;
    int m_lastMouseX = 0;
    int m_lastMouseY = 0;

    bool m_hoverButton = false;
    bool m_pressedButton = false;
    bool m_hoverPrevBank = false;
    bool m_hoverNextBank = false;
    bool m_hoverBankBox = false;
    bool m_hoverPrevPatch = false;
    bool m_hoverNextPatch = false;
    bool m_hoverPatchBox = false;
    bool m_hoverSearchBox = false;
    bool m_hoverSearchClear = false;
    bool m_hoverResetTune = false;
    bool m_hoverPortaToggle = false;
    bool m_hoverPortaMode = false;
    bool m_hoverResetAdsr = false;
    bool m_hoverAdsrToggle = false;

    bool m_searchFocused = false;
    std::string m_searchQuery;
    int m_selectedSearchRow = 0;
    int m_lastSearchClickRow = -1;
    unsigned long m_lastSearchClickTime = 0;

    std::chrono::steady_clock::time_point m_lastKeyTime{};
    int m_lastKeyCodeOrChar = -1;

    DropdownMode m_dropdownMode = DropdownMode::Closed;
    int m_menuScrollOffset = 0;
    int m_hoverPopupRow = -1;
    bool m_hoverScrollbar = false;
    bool m_draggingScrollbar = false;

    int m_hoverSliderIdx = -1;
    int m_draggingSliderIdx = -1;
    unsigned long m_lastClickTime = 0;

    std::atomic<bool> m_needsRedraw{false};
    std::shared_ptr<PickerState> m_pickerState;
};

} // namespace SuperSimpleSF2
