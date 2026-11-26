#include "view_x11.h"
#include "controller.h"

#include "pluginterfaces/base/keycodes.h"

#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "1.0.0"
#endif

namespace SuperSimpleSF2 {

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

constexpr int kPopupMaxVisibleRows = 9;
constexpr double kPopupRowHeight = 22.0;
constexpr double kSearchHeaderHeight = 24.0;
constexpr double kScrollbarWidth = 14.0;

void drawRoundedRect(cairo_t* cr, double x, double y, double w, double h, double r) {
    const double pi = 3.14159265358979323846;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -pi / 2.0, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, pi / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, pi / 2.0, pi);
    cairo_arc(cr, x + r, y + r, r, pi, 3.0 * pi / 2.0);
    cairo_close_path(cr);
}

void drawText(cairo_t* cr, const char* text, double x, double y, double maxW,
              const char* fontDescStr, double r, double g, double b, double a,
              PangoAlignment align = PANGO_ALIGN_LEFT,
              PangoEllipsizeMode ellipsize = PANGO_ELLIPSIZE_NONE) {
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* desc = pango_font_description_from_string(fontDescStr);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_text(layout, text ? text : "", -1);
    if (maxW > 0.0) {
        pango_layout_set_width(layout, static_cast<int>(maxW * PANGO_SCALE));
        pango_layout_set_ellipsize(layout, ellipsize);
    }
    pango_layout_set_alignment(layout, align);

    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    cairo_new_path(cr);
    cairo_restore(cr);

    g_object_unref(layout);
}

std::string basenameOf(const std::string& path) {
    if (path.empty()) return "";
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

std::string dirnameWithTrailingSlash(const std::string& path) {
    if (path.empty()) return "";
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return "";
    return path.substr(0, slash + 1);
}

bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    return it != haystack.end();
}

std::vector<int> collectBanks(const Sf2Status& status) {
    std::vector<int> banks;
    for (const auto& p : status.presets) {
        if (std::find(banks.begin(), banks.end(), p.bank) == banks.end()) {
            banks.push_back(p.bank);
        }
    }
    std::sort(banks.begin(), banks.end());
    return banks;
}

std::vector<const PresetEntry*> collectPatchesInBank(const Sf2Status& status, int bank) {
    std::vector<const PresetEntry*> patches;
    for (const auto& p : status.presets) {
        if (p.bank == bank) {
            patches.push_back(&p);
        }
    }
    std::sort(patches.begin(), patches.end(), [](const PresetEntry* a, const PresetEntry* b) {
        if (a->program != b->program) return a->program < b->program;
        return a->index < b->index;
    });
    return patches;
}

int currentSelectedBank(const Sf2Status& status) {
    if (status.presets.empty()) return 0;
    const int idx = std::clamp(status.selectedPresetIndex, 0, static_cast<int>(status.presets.size()) - 1);
    return status.presets[static_cast<size_t>(idx)].bank;
}

std::string formatPatchEntry(const PresetEntry& p) {
    char buf[300] = {};
    std::snprintf(buf, sizeof(buf), "%03d  %s", p.program, p.name.c_str());
    return buf;
}

std::string formatBankLabel(int bank) {
    char buf[64] = {};
    if (bank == 128) {
        std::snprintf(buf, sizeof(buf), "128 (Drums)");
    } else {
        std::snprintf(buf, sizeof(buf), "Bank %03d", bank);
    }
    return buf;
}

void formatSliderValue(ParamID id, double norm, char* buf, size_t size) {
    switch (id) {
        case kParamVolume:
        case kParamModWheel:
        case kParamSustainLevel:
            std::snprintf(buf, size, "%.0f%%", norm * 100.0);
            break;
        case kParamCoarseTune: {
            const int st = coarseNormToSemitones(norm);
            std::snprintf(buf, size, "%+d st", st);
            break;
        }
        case kParamFineTune: {
            const int ct = static_cast<int>(std::lround(fineNormToCents(norm)));
            std::snprintf(buf, size, "%+d ct", ct);
            break;
        }
        case kParamAttack: {
            const float sec = attackNormToSec(norm);
            if (sec < 1.0f) std::snprintf(buf, size, "%.0f ms", sec * 1000.0f);
            else std::snprintf(buf, size, "%.2f s", sec);
            break;
        }
        case kParamDecay: {
            const float sec = decayNormToSec(norm);
            if (sec < 1.0f) std::snprintf(buf, size, "%.0f ms", sec * 1000.0f);
            else std::snprintf(buf, size, "%.2f s", sec);
            break;
        }
        case kParamRelease: {
            const float sec = releaseNormToSec(norm);
            if (sec < 1.0f) std::snprintf(buf, size, "%.0f ms", sec * 1000.0f);
            else std::snprintf(buf, size, "%.2f s", sec);
            break;
        }
        case kParamGlideTime: {
            const float sec = glideNormToSec(norm);
            if (sec < 1.0f) std::snprintf(buf, size, "%.0f ms", sec * 1000.0f);
            else std::snprintf(buf, size, "%.2f s", sec);
            break;
        }
        default:
            std::snprintf(buf, size, "%.2f", norm);
            break;
    }
}

std::string runDialogCommand(const std::vector<const char*>& argv) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return "";

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return "";
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    close(pipefd[1]);
    std::string output;
    char buf[512];
    ssize_t n = 0;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        output.append(buf, static_cast<size_t>(n));
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return "";
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

std::string openSf2FileDialogBlocking(const std::string& initialDir) {
    std::vector<const char*> zenityArgs = {
        "zenity",
        "--file-selection",
        "--title=Select SoundFont (.sf2)",
        "--file-filter=SoundFont2 (*.sf2 *.SF2) | *.sf2 *.SF2",
        "--file-filter=All files | *"
    };
    std::string filenameArg;
    if (!initialDir.empty()) {
        filenameArg = "--filename=" + initialDir;
        zenityArgs.push_back(filenameArg.c_str());
    }
    zenityArgs.push_back(nullptr);

    std::string chosen = runDialogCommand(zenityArgs);
    if (!chosen.empty()) return chosen;

    if (access("/usr/bin/zenity", X_OK) != 0) {
        const char* startDir = initialDir.empty() ? "." : initialDir.c_str();
        std::vector<const char*> kdialogArgs = {
            "kdialog",
            "--getopenfilename",
            startDir,
            "*.sf2 *.SF2 |SoundFont2 (*.sf2)",
            nullptr
        };
        chosen = runDialogCommand(kdialogArgs);
    }
    return chosen;
}

} // namespace

X11View::X11View(Controller* controller)
    : m_controller(controller), m_pickerState(std::make_shared<PickerState>()) {
    FUNKNOWN_CTOR
}

X11View::~X11View() {
    FUNKNOWN_DTOR
    removed();
    if (m_controller) {
        m_controller->detachView(this);
        m_controller = nullptr;
    }
    if (m_plugFrame) {
        m_plugFrame->release();
        m_plugFrame = nullptr;
    }
}

IMPLEMENT_REFCOUNT(X11View)

tresult PLUGIN_API X11View::queryInterface(const TUID _iid, void** obj) {
    QUERY_INTERFACE(_iid, obj, FUnknown::iid, IPlugView)
    QUERY_INTERFACE(_iid, obj, IPlugView::iid, IPlugView)
    QUERY_INTERFACE(_iid, obj, Linux::IEventHandler::iid, Linux::IEventHandler)
    QUERY_INTERFACE(_iid, obj, Linux::ITimerHandler::iid, Linux::ITimerHandler)
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API X11View::isPlatformTypeSupported(FIDString type) {
    if (type && std::strcmp(type, kPlatformTypeX11EmbedWindowID) == 0) {
        return kResultTrue;
    }
    return kResultFalse;
}

tresult PLUGIN_API X11View::attached(void* parent, FIDString type) {
    if (!parent || !type || std::strcmp(type, kPlatformTypeX11EmbedWindowID) != 0) {
        return kResultFalse;
    }
    if (m_attached) {
        removed();
    }

    m_display = XOpenDisplay(nullptr);
    if (!m_display) {
        return kResultFalse;
    }

    const Window parentWin = static_cast<Window>(reinterpret_cast<uintptr_t>(parent));
    const int screen = DefaultScreen(m_display);

    XSetWindowAttributes attrs;
    std::memset(&attrs, 0, sizeof(attrs));
    attrs.background_pixel = 0x181B22;
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                       PointerMotionMask | LeaveWindowMask | StructureNotifyMask | KeyPressMask;

    m_window = XCreateWindow(
        m_display, parentWin, 0, 0, kWidth, kHeight, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWBackPixel | CWEventMask, &attrs);

    if (!m_window) {
        XCloseDisplay(m_display);
        m_display = nullptr;
        return kResultFalse;
    }

    XMapWindow(m_display, m_window);
    XFlush(m_display);

    m_surface = cairo_xlib_surface_create(
        m_display, m_window, DefaultVisual(m_display, screen), kWidth, kHeight);
    m_cr = cairo_create(m_surface);
    m_backSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kWidth, kHeight);
    m_backCr = cairo_create(m_backSurface);

    if (m_plugFrame) {
        m_plugFrame->queryInterface(Linux::IRunLoop::iid, reinterpret_cast<void**>(&m_runLoop));
        if (m_runLoop) {
            m_runLoop->registerEventHandler(this, ConnectionNumber(m_display));
            m_runLoop->registerTimer(this, 50);
        }
    }

    m_attached = true;
    draw();
    return kResultOk;
}

tresult PLUGIN_API X11View::removed() {
    if (!m_attached) {
        return kResultOk;
    }
    m_attached = false;

    if (m_runLoop) {
        m_runLoop->unregisterTimer(this);
        m_runLoop->unregisterEventHandler(this);
        m_runLoop->release();
        m_runLoop = nullptr;
    }

    if (m_backCr) {
        cairo_destroy(m_backCr);
        m_backCr = nullptr;
    }
    if (m_backSurface) {
        cairo_surface_destroy(m_backSurface);
        m_backSurface = nullptr;
    }
    if (m_cr) {
        cairo_destroy(m_cr);
        m_cr = nullptr;
    }
    if (m_surface) {
        cairo_surface_destroy(m_surface);
        m_surface = nullptr;
    }
    if (m_display) {
        if (m_window) {
            XDestroyWindow(m_display, m_window);
            m_window = 0;
        }
        XFlush(m_display);
        XCloseDisplay(m_display);
        m_display = nullptr;
    }

    return kResultOk;
}

std::vector<const PresetEntry*> X11View::filterPresetsByQuery(const Sf2Status& status) const {
    std::vector<const PresetEntry*> matches;
    matches.reserve(status.presets.size());
    for (const auto& p : status.presets) {
        if (m_searchQuery.empty() || containsCaseInsensitive(p.name, m_searchQuery)) {
            matches.push_back(&p);
        } else {
            char bp[32] = {};
            std::snprintf(bp, sizeof(bp), "%03d:%03d", p.bank, p.program);
            if (containsCaseInsensitive(bp, m_searchQuery)) {
                matches.push_back(&p);
            }
        }
    }
    return matches;
}

tresult PLUGIN_API X11View::onWheel(float distance) {
    if (!m_controller) return kResultFalse;

    if (m_dropdownMode != DropdownMode::Closed) {
        const Sf2Status status = m_controller->getUiStatus();
        int total = 0;
        if (m_dropdownMode == DropdownMode::Bank) {
            total = static_cast<int>(collectBanks(status).size());
        } else if (m_dropdownMode == DropdownMode::Patch) {
            total = static_cast<int>(collectPatchesInBank(status, currentSelectedBank(status)).size());
        } else if (m_dropdownMode == DropdownMode::Search) {
            total = static_cast<int>(filterPresetsByQuery(status).size());
        }
        const int maxScroll = std::max(0, total - kPopupMaxVisibleRows);
        m_menuScrollOffset = std::clamp(m_menuScrollOffset - static_cast<int>(std::round(distance)), 0, maxScroll);
        requestRedraw();
        return kResultTrue;
    }

    int targetIdx = m_hoverSliderIdx;
    if (targetIdx < 0) {
        if (bankBoxRect().contains(m_lastMouseX, m_lastMouseY)) {
            stepBank(distance > 0 ? -1 : 1);
            return kResultTrue;
        }
        if (patchBoxRect().contains(m_lastMouseX, m_lastMouseY)) {
            stepPatch(distance > 0 ? -1 : 1);
            return kResultTrue;
        }
        targetIdx = 0;
    }

    const auto& spec = sliders()[static_cast<size_t>(targetIdx)];
    double cur = m_controller->getParamNormalized(spec.paramId);
    const double step = (spec.paramId == kParamCoarseTune) ? (1.0 / 48.0) : 0.04;
    cur = std::clamp(cur + static_cast<double>(distance) * step, 0.0, 1.0);
    ensureAdsrEnabledIfNeeded(spec);
    m_controller->beginParamEdit(spec.paramId);
    m_controller->setParamFromView(spec.paramId, cur);
    m_controller->endParamEdit(spec.paramId);
    requestRedraw();
    return kResultTrue;
}

bool X11View::handleSearchKey(char asciiChar, int virtualKeyCode) {
    if (!m_searchFocused || !m_controller) return false;

    const auto now = std::chrono::steady_clock::now();
    const int keyId = (virtualKeyCode != 0) ? (0x10000 + virtualKeyCode) : static_cast<unsigned char>(asciiChar);
    if (keyId == m_lastKeyCodeOrChar &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastKeyTime).count() < 15) {
        return true; // Deduplicate host + X11 double delivery
    }
    m_lastKeyTime = now;
    m_lastKeyCodeOrChar = keyId;

    const Sf2Status status = m_controller->getUiStatus();
    const auto matches = filterPresetsByQuery(status);
    const int total = static_cast<int>(matches.size());

    if (virtualKeyCode == KEY_ESCAPE) {
        m_dropdownMode = DropdownMode::Closed;
        m_searchFocused = false;
        m_needsRedraw.store(true);
        return true;
    }
    if (virtualKeyCode == KEY_RETURN || virtualKeyCode == KEY_ENTER) {
        if (total > 0) {
            const int row = std::clamp(m_selectedSearchRow, 0, total - 1);
            m_controller->requestSelectPreset(matches[static_cast<size_t>(row)]->index);
            m_dropdownMode = DropdownMode::Closed;
            m_searchFocused = false;
            m_needsRedraw.store(true);
        }
        return true;
    }
    if (virtualKeyCode == KEY_UP) {
        if (total > 0) {
            m_selectedSearchRow = std::max(0, m_selectedSearchRow - 1);
            if (m_selectedSearchRow < m_menuScrollOffset) {
                m_menuScrollOffset = m_selectedSearchRow;
            }
            m_dropdownMode = DropdownMode::Search;
            m_needsRedraw.store(true);
        }
        return true;
    }
    if (virtualKeyCode == KEY_DOWN) {
        if (total > 0) {
            m_selectedSearchRow = std::min(total - 1, m_selectedSearchRow + 1);
            if (m_selectedSearchRow >= m_menuScrollOffset + kPopupMaxVisibleRows) {
                m_menuScrollOffset = m_selectedSearchRow - kPopupMaxVisibleRows + 1;
            }
            m_dropdownMode = DropdownMode::Search;
            m_needsRedraw.store(true);
        }
        return true;
    }
    if (virtualKeyCode == KEY_BACK) {
        if (!m_searchQuery.empty()) {
            m_searchQuery.pop_back();
            m_menuScrollOffset = 0;
            m_selectedSearchRow = 0;
            m_dropdownMode = DropdownMode::Search;
            m_needsRedraw.store(true);
        }
        return true;
    }

    if (asciiChar >= 0x20 && asciiChar <= 0x7E) {
        if (m_searchQuery.size() < 48) {
            m_searchQuery.push_back(asciiChar);
            m_menuScrollOffset = 0;
            m_selectedSearchRow = 0;
            m_dropdownMode = DropdownMode::Search;
            m_needsRedraw.store(true);
        }
        return true;
    }
    return false;
}

tresult PLUGIN_API X11View::onKeyDown(char16 key, int16 keyCode, int16 /*modifiers*/) {
    if (!m_searchFocused) return kResultFalse;
    const char ascii = (key >= 0x20 && key <= 0x7E) ? static_cast<char>(key) : 0;
    return handleSearchKey(ascii, keyCode) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API X11View::onKeyUp(char16 /*key*/, int16 /*keyCode*/, int16 /*modifiers*/) {
    return m_searchFocused ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API X11View::getSize(ViewRect* size) {
    if (!size) return kInvalidArgument;
    *size = ViewRect(0, 0, kWidth, kHeight);
    return kResultOk;
}

tresult PLUGIN_API X11View::onSize(ViewRect* newSize) {
    if (!newSize) return kInvalidArgument;
    if (m_attached && m_display && m_window) {
        XResizeWindow(m_display, m_window, kWidth, kHeight);
        if (m_surface) {
            cairo_xlib_surface_set_size(m_surface, kWidth, kHeight);
        }
        draw();
    }
    return kResultOk;
}

tresult PLUGIN_API X11View::onFocus(TBool state) {
    if (!state && m_searchFocused) {
        m_searchFocused = false;
        if (m_dropdownMode == DropdownMode::Search) {
            m_dropdownMode = DropdownMode::Closed;
        }
        m_needsRedraw.store(true);
    }
    return kResultOk;
}

tresult PLUGIN_API X11View::setFrame(IPlugFrame* frame) {
    if (m_plugFrame) {
        m_plugFrame->release();
        m_plugFrame = nullptr;
    }
    m_plugFrame = frame;
    if (m_plugFrame) {
        m_plugFrame->addRef();
    }
    return kResultOk;
}

tresult PLUGIN_API X11View::canResize() {
    return kResultFalse;
}

tresult PLUGIN_API X11View::checkSizeConstraint(ViewRect* rect) {
    if (!rect) return kInvalidArgument;
    rect->right = rect->left + kWidth;
    rect->bottom = rect->top + kHeight;
    return kResultOk;
}

void PLUGIN_API X11View::onFDIsSet(Linux::FileDescriptor /*fd*/) {
    if (!m_attached || !m_display) return;
    processXEvents();
    if (m_needsRedraw.exchange(false)) {
        draw();
    }
}

void PLUGIN_API X11View::onTimer() {
    if (!m_attached || !m_display) return;

    if (m_pickerState->hasResult.exchange(false)) {
        std::string chosen;
        {
            std::lock_guard<std::mutex> lock(m_pickerState->mutex);
            chosen = std::move(m_pickerState->selectedPath);
        }
        if (!chosen.empty() && m_controller) {
            m_dropdownMode = DropdownMode::Closed;
            m_searchFocused = false;
            m_searchQuery.clear();
            m_controller->requestLoadSf2(chosen);
        }
        m_needsRedraw.store(true);
    }

    processXEvents();
    if (m_needsRedraw.exchange(false)) {
        draw();
    }
}

void X11View::requestRedraw() {
    m_needsRedraw.store(true);
}

void X11View::detachController() {
    m_controller = nullptr;
}

void X11View::processXEvents() {
    while (XPending(m_display) > 0) {
        XEvent ev;
        XNextEvent(m_display, &ev);
        switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) {
                    m_needsRedraw.store(true);
                }
                break;
            case KeyPress:
                if (m_searchFocused) {
                    char buf[16] = {};
                    KeySym ks = NoSymbol;
                    const int len = XLookupString(&ev.xkey, buf, sizeof(buf), &ks, nullptr);
                    int vkey = 0;
                    if (ks == XK_BackSpace) vkey = KEY_BACK;
                    else if (ks == XK_Return || ks == XK_KP_Enter) vkey = KEY_RETURN;
                    else if (ks == XK_Escape) vkey = KEY_ESCAPE;
                    else if (ks == XK_Up) vkey = KEY_UP;
                    else if (ks == XK_Down) vkey = KEY_DOWN;
                    const char ch = (len > 0 && static_cast<unsigned char>(buf[0]) >= 0x20 &&
                                     static_cast<unsigned char>(buf[0]) <= 0x7E)
                                        ? buf[0]
                                        : 0;
                    handleSearchKey(ch, vkey);
                }
                break;
            case ButtonPress:
                m_lastMouseX = ev.xbutton.x;
                m_lastMouseY = ev.xbutton.y;
                handleButtonPress(ev.xbutton.x, ev.xbutton.y, ev.xbutton.button, ev.xbutton.time);
                break;
            case ButtonRelease:
                m_lastMouseX = ev.xbutton.x;
                m_lastMouseY = ev.xbutton.y;
                handleButtonRelease(ev.xbutton.x, ev.xbutton.y, ev.xbutton.button);
                break;
            case MotionNotify:
                m_lastMouseX = ev.xmotion.x;
                m_lastMouseY = ev.xmotion.y;
                handleMotion(ev.xmotion.x, ev.xmotion.y);
                break;
            case LeaveNotify:
                m_hoverButton = false;
                m_hoverPrevBank = false;
                m_hoverNextBank = false;
                m_hoverBankBox = false;
                m_hoverPrevPatch = false;
                m_hoverNextPatch = false;
                m_hoverPatchBox = false;
                m_hoverSearchBox = false;
                m_hoverSearchClear = false;
                m_hoverResetTune = false;
                m_hoverPortaToggle = false;
                m_hoverPortaMode = false;
                m_hoverResetAdsr = false;
                m_hoverAdsrToggle = false;
                m_hoverResetVel = false;
                m_hoverVelPreset = -1;
                m_hoverVelPoint = -1;
                m_hoverSliderIdx = -1;
                m_hoverPopupRow = -1;
                m_hoverScrollbar = false;
                m_needsRedraw.store(true);
                break;
            default:
                break;
        }
    }
}

X11View::Rect X11View::loadButtonRect()  { return {354.0, 59.0, 112.0, 36.0}; }

X11View::Rect X11View::prevBankRect()    { return {66.0, 120.0, 22.0, 26.0}; }
X11View::Rect X11View::bankBoxRect()     { return {90.0, 120.0, 64.0, 26.0}; }
X11View::Rect X11View::nextBankRect()    { return {156.0, 120.0, 22.0, 26.0}; }

X11View::Rect X11View::prevPatchRect()   { return {228.0, 120.0, 22.0, 26.0}; }
X11View::Rect X11View::patchBoxRect()    { return {252.0, 120.0, 194.0, 26.0}; }
X11View::Rect X11View::nextPatchRect()   { return {448.0, 120.0, 22.0, 26.0}; }

X11View::Rect X11View::searchBoxRect()   { return {30.0, 152.0, 440.0, 26.0}; }
X11View::Rect X11View::searchClearRect() { return {444.0, 154.0, 24.0, 22.0}; }

X11View::Rect X11View::activePopupRect(DropdownMode mode) {
    const double h = kPopupMaxVisibleRows * kPopupRowHeight + 4.0;
    if (mode == DropdownMode::Bank) {
        return {90.0, 148.0, 140.0, h};
    }
    if (mode == DropdownMode::Patch) {
        return {228.0, 148.0, 242.0, h};
    }
    // Search results window (with column header)
    return {30.0, 180.0, 440.0, kSearchHeaderHeight + h};
}

X11View::Rect X11View::resetTuneRect()   { return {420.0, 254.0, 48.0, 22.0}; }
X11View::Rect X11View::portaToggleRect() { return {30.0, 304.0, 80.0, 22.0}; }
X11View::Rect X11View::portaModeRect()   { return {116.0, 304.0, 64.0, 22.0}; }
X11View::Rect X11View::resetAdsrRect()   { return {296.0, 350.0, 68.0, 22.0}; }
X11View::Rect X11View::adsrToggleRect()  { return {372.0, 350.0, 94.0, 22.0}; }
X11View::Rect X11View::velGraphRect()    { return {506.0, 90.0, 160.0, 160.0}; }
X11View::Rect X11View::resetVelRect()    { return {624.0, 56.0, 46.0, 20.0}; }
X11View::Rect X11View::velPresetRect(int idx) {
    const int col = idx % 2;
    const int row = idx / 2;
    return {506.0 + col * 83.0, 354.0 + row * 28.0, 77.0, 22.0};
}

const std::array<X11View::SliderSpec, 9>& X11View::sliders() {
    static const std::array<SliderSpec, 9> kSliders = {{
        {
            kParamVolume, "Vol", kDefaultVolumeNorm, false, false,
            {32.0, 205.0, 30.0, 20.0},
            {64.0, 212.0, 142.0, 6.0},
            {56.0, 198.0, 158.0, 32.0},
            {210.0, 205.0, 42.0, 20.0}
        },
        {
            kParamModWheel, "Mod", kDefaultModWheelNorm, false, false,
            {266.0, 205.0, 34.0, 20.0},
            {304.0, 212.0, 116.0, 6.0},
            {296.0, 198.0, 132.0, 32.0},
            {424.0, 205.0, 42.0, 20.0}
        },
        {
            kParamCoarseTune, "Coarse", kDefaultCoarseTuneNorm, false, true,
            {32.0, 255.0, 42.0, 20.0},
            {74.0, 262.0, 100.0, 6.0},
            {68.0, 248.0, 112.0, 32.0},
            {176.0, 255.0, 42.0, 20.0}
        },
        {
            kParamFineTune, "Fine", kDefaultFineTuneNorm, false, true,
            {226.0, 255.0, 30.0, 20.0},
            {256.0, 262.0, 110.0, 6.0},
            {250.0, 248.0, 122.0, 32.0},
            {370.0, 255.0, 44.0, 20.0}
        },
        {
            kParamGlideTime, "Glide", kDefaultGlideTimeNorm, false, false,
            {190.0, 305.0, 34.0, 20.0},
            {226.0, 312.0, 186.0, 6.0},
            {218.0, 298.0, 202.0, 32.0},
            {416.0, 305.0, 52.0, 20.0}
        },
        {
            kParamAttack, "A", kDefaultAttackNorm, true, false,
            {34.0, 384.0, 18.0, 20.0},
            {54.0, 391.0, 134.0, 6.0},
            {46.0, 378.0, 150.0, 28.0},
            {192.0, 384.0, 52.0, 20.0}
        },
        {
            kParamDecay, "D", kDefaultDecayNorm, true, false,
            {262.0, 384.0, 18.0, 20.0},
            {282.0, 391.0, 134.0, 6.0},
            {274.0, 378.0, 150.0, 28.0},
            {420.0, 384.0, 48.0, 20.0}
        },
        {
            kParamSustainLevel, "S", kDefaultSustainLevelNorm, true, false,
            {34.0, 420.0, 18.0, 20.0},
            {54.0, 427.0, 134.0, 6.0},
            {46.0, 414.0, 150.0, 28.0},
            {192.0, 420.0, 52.0, 20.0}
        },
        {
            kParamRelease, "R", kDefaultReleaseNorm, true, false,
            {262.0, 420.0, 18.0, 20.0},
            {282.0, 427.0, 134.0, 6.0},
            {274.0, 414.0, 150.0, 28.0},
            {420.0, 420.0, 48.0, 20.0}
        },
    }};
    return kSliders;
}

double X11View::sliderValueFromX(const SliderSpec& spec, int x) {
    const double t = (static_cast<double>(x) - spec.trackRect.x) / spec.trackRect.w;
    return std::clamp(t, 0.0, 1.0);
}

void X11View::ensureAdsrEnabledIfNeeded(const SliderSpec& spec) {
    if (!spec.isAdsr || !m_controller) return;
    if (m_controller->getParamNormalized(kParamAdsrEnabled) < 0.5) {
        m_controller->beginParamEdit(kParamAdsrEnabled);
        m_controller->setParamFromView(kParamAdsrEnabled, 1.0);
        m_controller->endParamEdit(kParamAdsrEnabled);
    }
}

void X11View::resetTuning() {
    if (!m_controller) return;
    auto resetOne = [this](ParamID id, double defVal) {
        m_controller->beginParamEdit(id);
        m_controller->setParamFromView(id, defVal);
        m_controller->endParamEdit(id);
    };
    resetOne(kParamCoarseTune, kDefaultCoarseTuneNorm);
    resetOne(kParamFineTune, kDefaultFineTuneNorm);
    m_needsRedraw.store(true);
}

void X11View::resetEnvelope() {
    if (!m_controller) return;
    auto resetOne = [this](ParamID id, double defVal) {
        m_controller->beginParamEdit(id);
        m_controller->setParamFromView(id, defVal);
        m_controller->endParamEdit(id);
    };
    resetOne(kParamAttack, kDefaultAttackNorm);
    resetOne(kParamDecay, kDefaultDecayNorm);
    resetOne(kParamSustainLevel, kDefaultSustainLevelNorm);
    resetOne(kParamRelease, kDefaultReleaseNorm);
    m_needsRedraw.store(true);
}

void X11View::setVelocityPoints(double x1, double y1, double x2, double y2) {
    if (!m_controller) return;
    auto setOne = [this](ParamID id, double v) {
        m_controller->beginParamEdit(id);
        m_controller->setParamFromView(id, std::clamp(v, 0.0, 1.0));
        m_controller->endParamEdit(id);
    };
    setOne(kParamVelP1X, x1);
    setOne(kParamVelP1Y, y1);
    setOne(kParamVelP2X, x2);
    setOne(kParamVelP2Y, y2);
    m_needsRedraw.store(true);
}

void X11View::stepBank(int delta) {
    if (!m_controller) return;
    const Sf2Status status = m_controller->getUiStatus();
    if (!status.loaded || status.presets.empty()) return;

    const std::vector<int> banks = collectBanks(status);
    if (banks.empty()) return;

    const int curBank = currentSelectedBank(status);
    auto it = std::find(banks.begin(), banks.end(), curBank);
    int pos = (it != banks.end()) ? static_cast<int>(it - banks.begin()) : 0;
    pos = (pos + delta) % static_cast<int>(banks.size());
    if (pos < 0) pos += static_cast<int>(banks.size());

    const auto patches = collectPatchesInBank(status, banks[static_cast<size_t>(pos)]);
    if (!patches.empty()) {
        m_controller->requestSelectPreset(patches[0]->index);
    }
}

void X11View::stepPatch(int delta) {
    if (!m_controller) return;
    const Sf2Status status = m_controller->getUiStatus();
    if (!status.loaded || status.presets.empty()) return;

    const int curBank = currentSelectedBank(status);
    const auto patches = collectPatchesInBank(status, curBank);
    if (patches.empty()) return;

    int pos = 0;
    for (int i = 0; i < static_cast<int>(patches.size()); ++i) {
        if (patches[static_cast<size_t>(i)]->index == status.selectedPresetIndex) {
            pos = i;
            break;
        }
    }
    pos = (pos + delta) % static_cast<int>(patches.size());
    if (pos < 0) pos += static_cast<int>(patches.size());
    m_controller->requestSelectPreset(patches[static_cast<size_t>(pos)]->index);
}

void X11View::updateScrollFromMouseY(int y, int totalItems) {
    if (totalItems <= kPopupMaxVisibleRows) {
        m_menuScrollOffset = 0;
        return;
    }
    const Rect popup = activePopupRect(m_dropdownMode);
    const double headerH = (m_dropdownMode == DropdownMode::Search) ? kSearchHeaderHeight : 0.0;
    const double barY = popup.y + headerH + 4.0;
    const double barH = kPopupMaxVisibleRows * kPopupRowHeight - 4.0;
    const double t = std::clamp((static_cast<double>(y) - barY) / barH, 0.0, 1.0);
    const int maxScroll = totalItems - kPopupMaxVisibleRows;
    m_menuScrollOffset = static_cast<int>(std::lround(t * maxScroll));
}

void X11View::handleButtonPress(int x, int y, unsigned int button, unsigned long timeMs) {
    if (button == Button4) {
        onWheel(1.0f);
        return;
    }
    if (button == Button5) {
        onWheel(-1.0f);
        return;
    }
    if (button != Button1) return;

    // Handle Search Clear button first
    if (!m_searchQuery.empty() && searchClearRect().contains(x, y)) {
        m_searchQuery.clear();
        m_menuScrollOffset = 0;
        m_selectedSearchRow = 0;
        m_needsRedraw.store(true);
        return;
    }

    // Handle clicking inside Search Box
    if (searchBoxRect().contains(x, y) && m_controller) {
        const Sf2Status status = m_controller->getUiStatus();
        if (status.loaded && !status.presets.empty()) {
            m_searchFocused = true;
            m_dropdownMode = DropdownMode::Search;
            if (m_display && m_window) {
                XSetInputFocus(m_display, m_window, RevertToParent, CurrentTime);
            }
            m_needsRedraw.store(true);
        }
        return;
    }

    if (m_dropdownMode != DropdownMode::Closed && m_controller) {
        const Sf2Status status = m_controller->getUiStatus();
        const std::vector<int> banks = collectBanks(status);
        const int curBank = currentSelectedBank(status);
        const auto patches = collectPatchesInBank(status, curBank);
        const auto searchMatches = (m_dropdownMode == DropdownMode::Search)
                                       ? filterPresetsByQuery(status)
                                       : std::vector<const PresetEntry*>{};

        int total = 0;
        if (m_dropdownMode == DropdownMode::Bank) total = static_cast<int>(banks.size());
        else if (m_dropdownMode == DropdownMode::Patch) total = static_cast<int>(patches.size());
        else total = static_cast<int>(searchMatches.size());

        const double headerH = (m_dropdownMode == DropdownMode::Search) ? kSearchHeaderHeight : 0.0;
        const int visible = std::min(kPopupMaxVisibleRows, total);
        const Rect popup = activePopupRect(m_dropdownMode);
        const double actualH = headerH + std::max(1, visible) * kPopupRowHeight + 4.0;
        const Rect actualPopup{popup.x, popup.y, popup.w, actualH};

        if (actualPopup.contains(x, y)) {
            const bool hasScrollbar = total > kPopupMaxVisibleRows;
            if (hasScrollbar && x >= (popup.x + popup.w - kScrollbarWidth - 4.0) &&
                y >= (popup.y + headerH)) {
                m_draggingScrollbar = true;
                updateScrollFromMouseY(y, total);
                m_needsRedraw.store(true);
                return;
            }

            if (y >= (popup.y + headerH + 2.0) && total > 0) {
                const int row = static_cast<int>((static_cast<double>(y) - (popup.y + headerH + 2.0)) / kPopupRowHeight);
                if (row >= 0 && row < visible) {
                    const int itemIdx = m_menuScrollOffset + row;
                    if (m_dropdownMode == DropdownMode::Search) {
                        const bool isDoubleClick =
                            (itemIdx == m_lastSearchClickRow) && ((timeMs - m_lastSearchClickTime) < 400);
                        m_selectedSearchRow = itemIdx;
                        m_lastSearchClickRow = itemIdx;
                        m_lastSearchClickTime = timeMs;
                        if (isDoubleClick && itemIdx < static_cast<int>(searchMatches.size())) {
                            m_controller->requestSelectPreset(searchMatches[static_cast<size_t>(itemIdx)]->index);
                            m_dropdownMode = DropdownMode::Closed;
                            m_searchFocused = false;
                            m_hoverPopupRow = -1;
                        }
                        m_needsRedraw.store(true);
                        return;
                    }

                    if (m_dropdownMode == DropdownMode::Bank && itemIdx < static_cast<int>(banks.size())) {
                        const auto bankPatches = collectPatchesInBank(status, banks[static_cast<size_t>(itemIdx)]);
                        if (!bankPatches.empty()) {
                            m_controller->requestSelectPreset(bankPatches[0]->index);
                        }
                    } else if (m_dropdownMode == DropdownMode::Patch && itemIdx < static_cast<int>(patches.size())) {
                        m_controller->requestSelectPreset(patches[static_cast<size_t>(itemIdx)]->index);
                    }
                    m_dropdownMode = DropdownMode::Closed;
                    m_hoverPopupRow = -1;
                    m_needsRedraw.store(true);
                }
            }
            return;
        }

        const DropdownMode prevMode = m_dropdownMode;
        m_dropdownMode = DropdownMode::Closed;
        m_searchFocused = false;
        m_hoverPopupRow = -1;
        m_needsRedraw.store(true);
        if ((prevMode == DropdownMode::Bank && bankBoxRect().contains(x, y)) ||
            (prevMode == DropdownMode::Patch && patchBoxRect().contains(x, y))) {
            return;
        }
    } else {
        m_searchFocused = false;
    }

    if (loadButtonRect().contains(x, y)) {
        m_pressedButton = true;
        m_needsRedraw.store(true);
        return;
    }

    if (prevBankRect().contains(x, y)) { stepBank(-1); return; }
    if (nextBankRect().contains(x, y)) { stepBank(1); return; }
    if (prevPatchRect().contains(x, y)) { stepPatch(-1); return; }
    if (nextPatchRect().contains(x, y)) { stepPatch(1); return; }

    if (bankBoxRect().contains(x, y) && m_controller) {
        const Sf2Status status = m_controller->getUiStatus();
        if (status.loaded && !status.presets.empty()) {
            m_dropdownMode = DropdownMode::Bank;
            const auto banks = collectBanks(status);
            const int curBank = currentSelectedBank(status);
            auto it = std::find(banks.begin(), banks.end(), curBank);
            const int pos = (it != banks.end()) ? static_cast<int>(it - banks.begin()) : 0;
            const int maxScroll = std::max(0, static_cast<int>(banks.size()) - kPopupMaxVisibleRows);
            m_menuScrollOffset = std::clamp(pos - 3, 0, maxScroll);
            m_needsRedraw.store(true);
        }
        return;
    }

    if (patchBoxRect().contains(x, y) && m_controller) {
        const Sf2Status status = m_controller->getUiStatus();
        if (status.loaded && !status.presets.empty()) {
            m_dropdownMode = DropdownMode::Patch;
            const auto patches = collectPatchesInBank(status, currentSelectedBank(status));
            int pos = 0;
            for (int i = 0; i < static_cast<int>(patches.size()); ++i) {
                if (patches[static_cast<size_t>(i)]->index == status.selectedPresetIndex) {
                    pos = i;
                    break;
                }
            }
            const int maxScroll = std::max(0, static_cast<int>(patches.size()) - kPopupMaxVisibleRows);
            m_menuScrollOffset = std::clamp(pos - 3, 0, maxScroll);
            m_needsRedraw.store(true);
        }
        return;
    }

    if (resetTuneRect().contains(x, y)) {
        resetTuning();
        return;
    }

    if (portaToggleRect().contains(x, y) && m_controller) {
        const double cur = m_controller->getParamNormalized(kParamPortamentoEnabled);
        const double next = (cur >= 0.5) ? 0.0 : 1.0;
        m_controller->beginParamEdit(kParamPortamentoEnabled);
        m_controller->setParamFromView(kParamPortamentoEnabled, next);
        m_controller->endParamEdit(kParamPortamentoEnabled);
        m_needsRedraw.store(true);
        return;
    }

    if (portaModeRect().contains(x, y) && m_controller) {
        const double cur = m_controller->getParamNormalized(kParamPortamentoMode);
        const double next = (cur >= 0.5) ? 0.0 : 1.0;
        m_controller->beginParamEdit(kParamPortamentoMode);
        m_controller->setParamFromView(kParamPortamentoMode, next);
        m_controller->endParamEdit(kParamPortamentoMode);
        m_needsRedraw.store(true);
        return;
    }

    if (resetAdsrRect().contains(x, y)) {
        resetEnvelope();
        return;
    }

    if (adsrToggleRect().contains(x, y) && m_controller) {
        const double cur = m_controller->getParamNormalized(kParamAdsrEnabled);
        const double next = (cur >= 0.5) ? 0.0 : 1.0;
        m_controller->beginParamEdit(kParamAdsrEnabled);
        m_controller->setParamFromView(kParamAdsrEnabled, next);
        m_controller->endParamEdit(kParamAdsrEnabled);
        m_needsRedraw.store(true);
        return;
    }

    if (resetVelRect().contains(x, y)) {
        setVelocityPoints(kDefaultVelP1X, kDefaultVelP1Y, kDefaultVelP2X, kDefaultVelP2Y);
        return;
    }

    for (int p = 0; p < 4; ++p) {
        if (velPresetRect(p).contains(x, y)) {
            if (p == 0) setVelocityPoints(0.33, 0.33, 0.67, 0.67);      // Linear
            else if (p == 1) setVelocityPoints(0.42, 0.14, 0.78, 0.48); // Soft
            else if (p == 2) setVelocityPoints(0.22, 0.52, 0.58, 0.86); // Hard
            else setVelocityPoints(0.0, 1.0, 1.0, 1.0);                 // Fixed 127
            return;
        }
    }

    const Rect vg = velGraphRect();
    const Rect vgHit{vg.x - 10.0, vg.y - 10.0, vg.w + 20.0, vg.h + 20.0};
    if (vgHit.contains(x, y) && m_controller) {
        const double p1x = m_controller->getParamNormalized(kParamVelP1X);
        const double p1y = m_controller->getParamNormalized(kParamVelP1Y);
        const double p2x = m_controller->getParamNormalized(kParamVelP2X);
        const double p2y = m_controller->getParamNormalized(kParamVelP2Y);

        const double sx1 = vg.x + p1x * vg.w;
        const double sy1 = vg.y + (1.0 - p1y) * vg.h;
        const double sx2 = vg.x + p2x * vg.w;
        const double sy2 = vg.y + (1.0 - p2y) * vg.h;

        const double d1 = std::hypot(x - sx1, y - sy1);
        const double d2 = std::hypot(x - sx2, y - sy2);
        int chosenPt = (d1 <= d2) ? 0 : 1;
        if (std::min(d1, d2) > 16.0) {
            // Clicked elsewhere on graph: pick closest horizontally
            const double gx = std::clamp((x - vg.x) / vg.w, 0.0, 1.0);
            chosenPt = (std::abs(gx - p1x) <= std::abs(gx - p2x)) ? 0 : 1;
        }

        const bool isDoubleClick = (timeMs - m_lastClickTime) < 300;
        m_lastClickTime = timeMs;
        if (isDoubleClick) {
            setVelocityPoints(kDefaultVelP1X, kDefaultVelP1Y, kDefaultVelP2X, kDefaultVelP2Y);
            return;
        }

        m_draggingVelPoint = chosenPt;
        if (chosenPt == 0) {
            m_controller->beginParamEdit(kParamVelP1X);
            m_controller->beginParamEdit(kParamVelP1Y);
        } else {
            m_controller->beginParamEdit(kParamVelP2X);
            m_controller->beginParamEdit(kParamVelP2Y);
        }
        m_needsRedraw.store(true);
        return;
    }

    const auto& allSliders = sliders();
    for (int i = 0; i < static_cast<int>(allSliders.size()); ++i) {
        const auto& spec = allSliders[static_cast<size_t>(i)];
        if (spec.hitRect.contains(x, y) && m_controller) {
            const bool isDoubleClick = (timeMs - m_lastClickTime) < 300;
            m_lastClickTime = timeMs;
            ensureAdsrEnabledIfNeeded(spec);
            m_draggingSliderIdx = i;
            m_controller->beginParamEdit(spec.paramId);
            const double val = isDoubleClick ? spec.defaultValue : sliderValueFromX(spec, x);
            m_controller->setParamFromView(spec.paramId, val);
            if (isDoubleClick) {
                m_draggingSliderIdx = -1;
                m_controller->endParamEdit(spec.paramId);
            }
            m_needsRedraw.store(true);
            return;
        }
    }
}

void X11View::handleButtonRelease(int x, int y, unsigned int button) {
    if (button != Button1) return;

    if (m_draggingScrollbar) {
        m_draggingScrollbar = false;
        m_needsRedraw.store(true);
    }

    if (m_pressedButton) {
        m_pressedButton = false;
        if (loadButtonRect().contains(x, y)) {
            launchFilePicker();
        }
        m_needsRedraw.store(true);
    }

    if (m_draggingSliderIdx >= 0) {
        const auto& spec = sliders()[static_cast<size_t>(m_draggingSliderIdx)];
        m_draggingSliderIdx = -1;
        if (m_controller) {
            m_controller->endParamEdit(spec.paramId);
        }
        m_needsRedraw.store(true);
    }

    if (m_draggingVelPoint >= 0) {
        const int pt = m_draggingVelPoint;
        m_draggingVelPoint = -1;
        if (m_controller) {
            if (pt == 0) {
                m_controller->endParamEdit(kParamVelP1X);
                m_controller->endParamEdit(kParamVelP1Y);
            } else {
                m_controller->endParamEdit(kParamVelP2X);
                m_controller->endParamEdit(kParamVelP2Y);
            }
        }
        m_needsRedraw.store(true);
    }
}

void X11View::handleMotion(int x, int y) {
    if (m_draggingScrollbar && m_dropdownMode != DropdownMode::Closed && m_controller) {
        const Sf2Status status = m_controller->getUiStatus();
        int total = 0;
        if (m_dropdownMode == DropdownMode::Bank) total = static_cast<int>(collectBanks(status).size());
        else if (m_dropdownMode == DropdownMode::Patch) total = static_cast<int>(collectPatchesInBank(status, currentSelectedBank(status)).size());
        else total = static_cast<int>(filterPresetsByQuery(status).size());
        updateScrollFromMouseY(y, total);
        m_needsRedraw.store(true);
        return;
    }

    if (m_draggingSliderIdx >= 0 && m_controller) {
        const auto& spec = sliders()[static_cast<size_t>(m_draggingSliderIdx)];
        m_controller->setParamFromView(spec.paramId, sliderValueFromX(spec, x));
        m_needsRedraw.store(true);
        return;
    }

    if (m_draggingVelPoint >= 0 && m_controller) {
        const Rect vg = velGraphRect();
        double nx = std::clamp((static_cast<double>(x) - vg.x) / vg.w, 0.0, 1.0);
        double ny = std::clamp(1.0 - (static_cast<double>(y) - vg.y) / vg.h, 0.0, 1.0);

        if (m_draggingVelPoint == 0) {
            const double p2x = m_controller->getParamNormalized(kParamVelP2X);
            if (nx < 0.015) nx = 0.0;
            nx = std::min(nx, std::max(0.0, p2x - 0.02));
            m_controller->setParamFromView(kParamVelP1X, nx);
            m_controller->setParamFromView(kParamVelP1Y, ny);
        } else {
            const double p1x = m_controller->getParamNormalized(kParamVelP1X);
            if (nx > 0.985) nx = 1.0;
            nx = std::max(nx, std::min(1.0, p1x + 0.02));
            m_controller->setParamFromView(kParamVelP2X, nx);
            m_controller->setParamFromView(kParamVelP2Y, ny);
        }
        m_needsRedraw.store(true);
        return;
    }

    int nextPopupRow = -1;
    bool nextHoverScroll = false;
    if (m_dropdownMode != DropdownMode::Closed) {
        const Rect popup = activePopupRect(m_dropdownMode);
        const double headerH = (m_dropdownMode == DropdownMode::Search) ? kSearchHeaderHeight : 0.0;
        if (popup.contains(x, y) && y >= (popup.y + headerH)) {
            if (x >= (popup.x + popup.w - kScrollbarWidth - 4.0)) {
                nextHoverScroll = true;
            } else {
                nextPopupRow = static_cast<int>((static_cast<double>(y) - (popup.y + headerH + 2.0)) / kPopupRowHeight);
            }
        }
    }

    const bool menuOpen = (m_dropdownMode != DropdownMode::Closed);
    const bool hoverBtn = !menuOpen && loadButtonRect().contains(x, y);
    const bool hoverPB = !menuOpen && prevBankRect().contains(x, y);
    const bool hoverNB = !menuOpen && nextBankRect().contains(x, y);
    const bool hoverBB = bankBoxRect().contains(x, y);
    const bool hoverPP = !menuOpen && prevPatchRect().contains(x, y);
    const bool hoverNP = !menuOpen && nextPatchRect().contains(x, y);
    const bool hoverPaB = patchBoxRect().contains(x, y);
    const bool hoverSrch = searchBoxRect().contains(x, y);
    const bool hoverClr = !m_searchQuery.empty() && searchClearRect().contains(x, y);
    const bool hoverRstTune = !menuOpen && resetTuneRect().contains(x, y);
    const bool hoverPortaTog = !menuOpen && portaToggleRect().contains(x, y);
    const bool hoverPortaMode = !menuOpen && portaModeRect().contains(x, y);
    const bool hoverRstAdsr = !menuOpen && resetAdsrRect().contains(x, y);
    const bool hoverAdsr = !menuOpen && adsrToggleRect().contains(x, y);
    const bool hoverRstVel = !menuOpen && resetVelRect().contains(x, y);

    int nextVelPresetHover = -1;
    int nextVelPtHover = -1;
    if (!menuOpen) {
        for (int p = 0; p < 4; ++p) {
            if (velPresetRect(p).contains(x, y)) {
                nextVelPresetHover = p;
                break;
            }
        }
        const Rect vg = velGraphRect();
        const Rect vgHit{vg.x - 10.0, vg.y - 10.0, vg.w + 20.0, vg.h + 20.0};
        if (vgHit.contains(x, y) && m_controller) {
            const double sx1 = vg.x + m_controller->getParamNormalized(kParamVelP1X) * vg.w;
            const double sy1 = vg.y + (1.0 - m_controller->getParamNormalized(kParamVelP1Y)) * vg.h;
            const double sx2 = vg.x + m_controller->getParamNormalized(kParamVelP2X) * vg.w;
            const double sy2 = vg.y + (1.0 - m_controller->getParamNormalized(kParamVelP2Y)) * vg.h;
            if (std::hypot(x - sx1, y - sy1) <= 13.0) nextVelPtHover = 0;
            else if (std::hypot(x - sx2, y - sy2) <= 13.0) nextVelPtHover = 1;
        }
    }

    int nextSliderHover = -1;
    if (!menuOpen) {
        const auto& allSliders = sliders();
        for (int i = 0; i < static_cast<int>(allSliders.size()); ++i) {
            if (allSliders[static_cast<size_t>(i)].hitRect.contains(x, y)) {
                nextSliderHover = i;
                break;
            }
        }
    }

    if (hoverBtn != m_hoverButton || hoverPB != m_hoverPrevBank || hoverNB != m_hoverNextBank ||
        hoverBB != m_hoverBankBox || hoverPP != m_hoverPrevPatch || hoverNP != m_hoverNextPatch ||
        hoverPaB != m_hoverPatchBox || hoverSrch != m_hoverSearchBox || hoverClr != m_hoverSearchClear ||
        hoverRstTune != m_hoverResetTune || hoverPortaTog != m_hoverPortaToggle ||
        hoverPortaMode != m_hoverPortaMode || hoverRstAdsr != m_hoverResetAdsr ||
        hoverAdsr != m_hoverAdsrToggle || hoverRstVel != m_hoverResetVel ||
        nextVelPresetHover != m_hoverVelPreset || nextVelPtHover != m_hoverVelPoint ||
        nextSliderHover != m_hoverSliderIdx || nextPopupRow != m_hoverPopupRow ||
        nextHoverScroll != m_hoverScrollbar) {
        m_hoverButton = hoverBtn;
        m_hoverPrevBank = hoverPB;
        m_hoverNextBank = hoverNB;
        m_hoverBankBox = hoverBB;
        m_hoverPrevPatch = hoverPP;
        m_hoverNextPatch = hoverNP;
        m_hoverPatchBox = hoverPaB;
        m_hoverSearchBox = hoverSrch;
        m_hoverSearchClear = hoverClr;
        m_hoverResetTune = hoverRstTune;
        m_hoverPortaToggle = hoverPortaTog;
        m_hoverPortaMode = hoverPortaMode;
        m_hoverResetAdsr = hoverRstAdsr;
        m_hoverAdsrToggle = hoverAdsr;
        m_hoverResetVel = hoverRstVel;
        m_hoverVelPreset = nextVelPresetHover;
        m_hoverVelPoint = nextVelPtHover;
        m_hoverSliderIdx = nextSliderHover;
        m_hoverPopupRow = nextPopupRow;
        m_hoverScrollbar = nextHoverScroll;
        m_needsRedraw.store(true);
    }
}

void X11View::launchFilePicker() {
    if (m_pickerState->open.exchange(true)) {
        return;
    }

    std::string initialDir;
    if (m_controller) {
        initialDir = dirnameWithTrailingSlash(m_controller->getUiStatus().path);
    }

    std::shared_ptr<PickerState> state = m_pickerState;
    std::thread([state, initialDir]() {
        std::string selected = openSf2FileDialogBlocking(initialDir);
        if (!selected.empty()) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->selectedPath = std::move(selected);
            state->hasResult.store(true);
        }
        state->open.store(false);
    }).detach();
}

void X11View::drawDropdown(const Sf2Status& status) {
    const std::vector<int> banks = collectBanks(status);
    const int curBank = currentSelectedBank(status);
    const auto patches = collectPatchesInBank(status, curBank);
    const auto searchMatches = (m_dropdownMode == DropdownMode::Search)
                                   ? filterPresetsByQuery(status)
                                   : std::vector<const PresetEntry*>{};

    int total = 0;
    if (m_dropdownMode == DropdownMode::Bank) total = static_cast<int>(banks.size());
    else if (m_dropdownMode == DropdownMode::Patch) total = static_cast<int>(patches.size());
    else total = static_cast<int>(searchMatches.size());

    const Rect popup = activePopupRect(m_dropdownMode);
    const double headerH = (m_dropdownMode == DropdownMode::Search) ? kSearchHeaderHeight : 0.0;
    const int visible = std::min(kPopupMaxVisibleRows, total);
    const double actualH = headerH + std::max(1, visible) * kPopupRowHeight + 4.0;

    // Shadow
    drawRoundedRect(m_cr, popup.x + 2.0, popup.y + 3.0, popup.w, actualH, 6.0);
    cairo_set_source_rgba(m_cr, 0.0, 0.0, 0.0, 0.55);
    cairo_fill(m_cr);

    // Background
    drawRoundedRect(m_cr, popup.x, popup.y, popup.w, actualH, 6.0);
    cairo_set_source_rgb(m_cr, 0.11, 0.13, 0.17);
    cairo_fill_preserve(m_cr);
    cairo_set_source_rgb(m_cr, 0.28, 0.34, 0.44);
    cairo_set_line_width(m_cr, 1.0);
    cairo_stroke(m_cr);

    if (m_dropdownMode == DropdownMode::Search) {
        // Column header bar
        cairo_set_source_rgb(m_cr, 0.15, 0.18, 0.24);
        cairo_rectangle(m_cr, popup.x + 1.0, popup.y + 1.0, popup.w - 2.0, headerH - 1.0);
        cairo_fill(m_cr);

        drawText(m_cr, "Bank", popup.x + 10.0, popup.y + 4.0, 44.0, "Sans Bold 8.5", 0.62, 0.70, 0.82, 1.0);
        drawText(m_cr, "Patch", popup.x + 56.0, popup.y + 4.0, 46.0, "Sans Bold 8.5", 0.62, 0.70, 0.82, 1.0);
        drawText(m_cr, "Preset", popup.x + 106.0, popup.y + 4.0, 200.0, "Sans Bold 8.5", 0.62, 0.70, 0.82, 1.0);

        char countBuf[48] = {};
        std::snprintf(countBuf, sizeof(countBuf), "%d found (2x click)", total);
        drawText(m_cr, countBuf, popup.x + 290.0, popup.y + 4.0, 140.0, "Sans 8",
                 0.55, 0.63, 0.74, 1.0, PANGO_ALIGN_RIGHT);

        if (total == 0) {
            drawText(m_cr, "No matching presets", popup.x + 12.0, popup.y + headerH + 5.0, popup.w - 24.0,
                     "Sans 9.5", 0.55, 0.60, 0.68, 1.0);
            return;
        }
    }

    if (total <= 0) return;

    const bool hasScrollbar = total > kPopupMaxVisibleRows;
    const double rowW = hasScrollbar ? (popup.w - kScrollbarWidth - 8.0) : (popup.w - 6.0);

    for (int r = 0; r < visible; ++r) {
        const int itemIdx = m_menuScrollOffset + r;
        if (itemIdx < 0 || itemIdx >= total) break;
        const double ry = popup.y + headerH + 2.0 + r * kPopupRowHeight;

        const bool isHovered = (r == m_hoverPopupRow);
        bool isSelected = false;

        if (m_dropdownMode == DropdownMode::Search) {
            const PresetEntry* p = searchMatches[static_cast<size_t>(itemIdx)];
            isSelected = (itemIdx == m_selectedSearchRow);
            const bool isActivePreset = (p->index == status.selectedPresetIndex);

            if (isSelected || isHovered || isActivePreset) {
                drawRoundedRect(m_cr, popup.x + 3.0, ry + 1.0, rowW, kPopupRowHeight - 2.0, 4.0);
                if (isSelected) cairo_set_source_rgb(m_cr, 0.231, 0.510, 0.965);
                else if (isActivePreset) cairo_set_source_rgb(m_cr, 0.16, 0.25, 0.39);
                else cairo_set_source_rgb(m_cr, 0.19, 0.23, 0.30);
                cairo_fill(m_cr);
            }

            char bBuf[16] = {}, pBuf[16] = {};
            std::snprintf(bBuf, sizeof(bBuf), "%03d", p->bank);
            std::snprintf(pBuf, sizeof(pBuf), "%03d", p->program);
            drawText(m_cr, bBuf, popup.x + 10.0, ry + 3.0, 40.0, "Sans 9", 0.78, 0.86, 0.96, 1.0);
            drawText(m_cr, pBuf, popup.x + 56.0, ry + 3.0, 42.0, "Sans 9", 0.78, 0.86, 0.96, 1.0);
            drawText(m_cr, p->name.c_str(), popup.x + 106.0, ry + 3.0, rowW - 114.0, "Sans 9.5",
                     0.95, 0.97, 1.0, 1.0, PANGO_ALIGN_LEFT, PANGO_ELLIPSIZE_END);
        } else {
            std::string line;
            if (m_dropdownMode == DropdownMode::Bank) {
                const int b = banks[static_cast<size_t>(itemIdx)];
                isSelected = (b == curBank);
                line = formatBankLabel(b);
            } else {
                const PresetEntry* p = patches[static_cast<size_t>(itemIdx)];
                isSelected = (p->index == status.selectedPresetIndex);
                line = formatPatchEntry(*p);
            }

            if (isSelected || isHovered) {
                drawRoundedRect(m_cr, popup.x + 3.0, ry + 1.0, rowW, kPopupRowHeight - 2.0, 4.0);
                if (isSelected) cairo_set_source_rgb(m_cr, 0.231, 0.510, 0.965);
                else cairo_set_source_rgb(m_cr, 0.19, 0.23, 0.30);
                cairo_fill(m_cr);
            }

            drawText(m_cr, line.c_str(), popup.x + 10.0, ry + 3.0, rowW - 14.0, "Sans 9.5",
                     0.94, 0.96, 0.99, 1.0, PANGO_ALIGN_LEFT, PANGO_ELLIPSIZE_END);
        }
    }

    if (hasScrollbar) {
        const double barX = popup.x + popup.w - kScrollbarWidth - 3.0;
        const double barY = popup.y + headerH + 4.0;
        const double barH = (actualH - headerH) - 8.0;
        const double thumbH = std::max(24.0, barH * (static_cast<double>(kPopupMaxVisibleRows) / total));
        const double maxScroll = static_cast<double>(total - kPopupMaxVisibleRows);
        const double thumbY = barY + (barH - thumbH) * (m_menuScrollOffset / maxScroll);

        drawRoundedRect(m_cr, barX, barY, kScrollbarWidth, barH, 6.0);
        cairo_set_source_rgb(m_cr, 0.08, 0.09, 0.12);
        cairo_fill(m_cr);

        drawRoundedRect(m_cr, barX + 2.0, thumbY, kScrollbarWidth - 4.0, thumbH, 5.0);
        if (m_draggingScrollbar || m_hoverScrollbar) {
            cairo_set_source_rgb(m_cr, 0.45, 0.58, 0.78);
        } else {
            cairo_set_source_rgb(m_cr, 0.35, 0.43, 0.56);
        }
        cairo_fill(m_cr);
    }
}

void X11View::draw() {
    if (!m_cr || !m_surface || !m_backCr || !m_backSurface) return;

    cairo_t* const origCr = m_cr;
    m_cr = m_backCr;
    cairo_new_path(m_cr);

    const Sf2Status status = m_controller ? m_controller->getUiStatus() : Sf2Status{};
    const bool adsrEnabled = m_controller ? (m_controller->getParamNormalized(kParamAdsrEnabled) >= 0.5) : false;

    // Background
    cairo_set_source_rgb(m_cr, 0.094, 0.106, 0.133); // #181B22
    cairo_paint(m_cr);

    // Top accent strip
    cairo_set_source_rgb(m_cr, 0.231, 0.510, 0.965); // #3B82F6
    cairo_rectangle(m_cr, 0.0, 0.0, kWidth, 3.0);
    cairo_fill(m_cr);

    // Title & subtitle
    drawText(m_cr, "AT SF2 Player", 20.0, 14.0, 220.0, "Sans Bold 13",
             0.94, 0.96, 0.98, 1.0);
    drawText(m_cr, "v" PLUGIN_VERSION "  •  SoundFont (.sf2) Player", 400.0, 17.0, 280.0, "Sans 9.5",
             0.58, 0.64, 0.72, 1.0, PANGO_ALIGN_RIGHT);

    // SoundFont file card
    drawRoundedRect(m_cr, 20.0, 48.0, 460.0, 58.0, 8.0);
    cairo_set_source_rgb(m_cr, 0.129, 0.149, 0.192);
    cairo_fill_preserve(m_cr);
    cairo_set_source_rgb(m_cr, 0.20, 0.23, 0.29);
    cairo_set_line_width(m_cr, 1.0);
    cairo_stroke(m_cr);

    // Status LED
    double ledR = 0.45, ledG = 0.50, ledB = 0.58;
    if (status.loading) {
        ledR = 0.96; ledG = 0.62; ledB = 0.04;
    } else if (!status.error.empty()) {
        ledR = 0.94; ledG = 0.27; ledB = 0.27;
    } else if (status.loaded) {
        ledR = 0.13; ledG = 0.77; ledB = 0.37;
    }
    cairo_arc(m_cr, 36.0, 70.0, 4.5, 0.0, 2.0 * 3.14159265358979323846);
    cairo_set_source_rgb(m_cr, ledR, ledG, ledB);
    cairo_fill(m_cr);

    std::string mainLine = basenameOf(status.path);
    if (mainLine.empty()) mainLine = "No SoundFont loaded";

    std::string subLine;
    if (status.loading) {
        subLine = "Loading SoundFont...";
    } else if (!status.error.empty()) {
        subLine = status.error;
    } else if (status.loaded) {
        const auto banks = collectBanks(status);
        char buf[128] = {};
        std::snprintf(buf, sizeof(buf), "%d preset%s in %zu bank%s",
                      status.presetCount, status.presetCount == 1 ? "" : "s",
                      banks.size(), banks.size() == 1 ? "" : "s");
        subLine = buf;
    } else {
        subLine = "Click \"Load .sf2...\" to choose a SoundFont";
    }

    drawText(m_cr, mainLine.c_str(), 50.0, 56.0, 290.0, "Sans Bold 10.5",
             0.93, 0.95, 0.98, 1.0, PANGO_ALIGN_LEFT, PANGO_ELLIPSIZE_MIDDLE);
    drawText(m_cr, subLine.c_str(), 50.0, 78.0, 290.0, "Sans 8.5",
             0.62, 0.68, 0.76, 1.0, PANGO_ALIGN_LEFT, PANGO_ELLIPSIZE_END);

    // Load button
    const Rect btn = loadButtonRect();
    drawRoundedRect(m_cr, btn.x, btn.y, btn.w, btn.h, 6.0);
    if (m_pressedButton) cairo_set_source_rgb(m_cr, 0.145, 0.388, 0.922);
    else if (m_hoverButton) cairo_set_source_rgb(m_cr, 0.294, 0.561, 0.980);
    else cairo_set_source_rgb(m_cr, 0.231, 0.510, 0.965);
    cairo_fill(m_cr);
    drawText(m_cr, "Load .sf2...", btn.x, btn.y + 9.0, btn.w, "Sans Bold 10",
             1.0, 1.0, 1.0, 1.0, PANGO_ALIGN_CENTER);

    // Bank, Patch & Search card
    drawRoundedRect(m_cr, 20.0, 114.0, 460.0, 72.0, 8.0);
    cairo_set_source_rgb(m_cr, 0.129, 0.149, 0.192);
    cairo_fill_preserve(m_cr);
    cairo_set_source_rgb(m_cr, 0.20, 0.23, 0.29);
    cairo_set_line_width(m_cr, 1.0);
    cairo_stroke(m_cr);

    auto drawSmallBtn = [&](const Rect& r, const char* label, bool hover, bool enabled) {
        drawRoundedRect(m_cr, r.x, r.y, r.w, r.h, 5.0);
        if (!enabled) cairo_set_source_rgb(m_cr, 0.15, 0.17, 0.22);
        else if (hover) cairo_set_source_rgb(m_cr, 0.24, 0.28, 0.36);
        else cairo_set_source_rgb(m_cr, 0.18, 0.21, 0.27);
        cairo_fill_preserve(m_cr);
        cairo_set_source_rgb(m_cr, 0.27, 0.31, 0.39);
        cairo_set_line_width(m_cr, 1.0);
        cairo_stroke(m_cr);
        drawText(m_cr, label, r.x, r.y + 4.0, r.w, "Sans Bold 9.5",
                 enabled ? 0.92 : 0.42, enabled ? 0.94 : 0.45, enabled ? 0.98 : 0.50, 1.0,
                 PANGO_ALIGN_CENTER);
    };

    const bool hasPresets = status.loaded && !status.presets.empty();
    drawText(m_cr, "Bank", 30.0, 125.0, 34.0, "Sans 9", 0.80, 0.84, 0.90, 1.0);
    drawSmallBtn(prevBankRect(), "<", m_hoverPrevBank, hasPresets);
    drawSmallBtn(nextBankRect(), ">", m_hoverNextBank, hasPresets);

    const Rect bbox = bankBoxRect();
    drawRoundedRect(m_cr, bbox.x, bbox.y, bbox.w, bbox.h, 5.0);
    cairo_set_source_rgb(m_cr, (m_hoverBankBox && hasPresets) ? 0.16 : 0.10,
                         (m_hoverBankBox && hasPresets) ? 0.19 : 0.12,
                         (m_hoverBankBox && hasPresets) ? 0.25 : 0.16);
    cairo_fill_preserve(m_cr);
    cairo_set_source_rgb(m_cr, (m_dropdownMode == DropdownMode::Bank) ? 0.231 : 0.25,
                         (m_dropdownMode == DropdownMode::Bank) ? 0.510 : 0.29,
                         (m_dropdownMode == DropdownMode::Bank) ? 0.965 : 0.37);
    cairo_set_line_width(m_cr, 1.0);
    cairo_stroke(m_cr);

    char bankStr[32] = "---";
    if (hasPresets) {
        std::snprintf(bankStr, sizeof(bankStr), "%03d", currentSelectedBank(status));
    }
    drawText(m_cr, bankStr, bbox.x + 6.0, bbox.y + 5.0, bbox.w - 22.0, "Sans 9.5",
             hasPresets ? 0.94 : 0.48, hasPresets ? 0.96 : 0.52, hasPresets ? 0.99 : 0.58, 1.0);
    drawText(m_cr, (m_dropdownMode == DropdownMode::Bank) ? "▲" : "▼",
             bbox.x + bbox.w - 18.0, bbox.y + 6.0, 14.0, "Sans 7.5",
             hasPresets ? 0.72 : 0.40, hasPresets ? 0.78 : 0.44, hasPresets ? 0.86 : 0.50, 1.0,
             PANGO_ALIGN_CENTER);

    drawText(m_cr, "Patch", 186.0, 125.0, 40.0, "Sans 9", 0.80, 0.84, 0.90, 1.0);
    drawSmallBtn(prevPatchRect(), "<", m_hoverPrevPatch, hasPresets);
    drawSmallBtn(nextPatchRect(), ">", m_hoverNextPatch, hasPresets);

    const Rect pbox = patchBoxRect();
    drawRoundedRect(m_cr, pbox.x, pbox.y, pbox.w, pbox.h, 5.0);
    cairo_set_source_rgb(m_cr, (m_hoverPatchBox && hasPresets) ? 0.16 : 0.10,
                         (m_hoverPatchBox && hasPresets) ? 0.19 : 0.12,
                         (m_hoverPatchBox && hasPresets) ? 0.25 : 0.16);
    cairo_fill_preserve(m_cr);
    cairo_set_source_rgb(m_cr, (m_dropdownMode == DropdownMode::Patch) ? 0.231 : 0.25,
                         (m_dropdownMode == DropdownMode::Patch) ? 0.510 : 0.29,
                         (m_dropdownMode == DropdownMode::Patch) ? 0.965 : 0.37);
    cairo_set_line_width(m_cr, 1.0);
    cairo_stroke(m_cr);

    std::string patchLabel = "No patches";
    if (hasPresets) {
        const int idx = std::clamp(status.selectedPresetIndex, 0, static_cast<int>(status.presets.size()) - 1);
        patchLabel = formatPatchEntry(status.presets[static_cast<size_t>(idx)]);
    }
    drawText(m_cr, patchLabel.c_str(), pbox.x + 8.0, pbox.y + 5.0, pbox.w - 26.0, "Sans 9.5",
             hasPresets ? 0.94 : 0.48, hasPresets ? 0.96 : 0.52, hasPresets ? 0.99 : 0.58, 1.0,
             PANGO_ALIGN_LEFT, PANGO_ELLIPSIZE_END);
    drawText(m_cr, (m_dropdownMode == DropdownMode::Patch) ? "▲" : "▼",
             pbox.x + pbox.w - 20.0, pbox.y + 6.0, 14.0, "Sans 7.5",
             hasPresets ? 0.72 : 0.40, hasPresets ? 0.78 : 0.44, hasPresets ? 0.86 : 0.50, 1.0,
             PANGO_ALIGN_CENTER);

    // Search input box
    const Rect sbox = searchBoxRect();
    drawRoundedRect(m_cr, sbox.x, sbox.y, sbox.w, sbox.h, 5.0);
    cairo_set_source_rgb(m_cr, (m_searchFocused || m_hoverSearchBox) ? 0.12 : 0.09,
                         (m_searchFocused || m_hoverSearchBox) ? 0.14 : 0.11,
                         (m_searchFocused || m_hoverSearchBox) ? 0.19 : 0.15);
    cairo_fill_preserve(m_cr);
    cairo_set_source_rgb(m_cr, m_searchFocused ? 0.231 : 0.24,
                         m_searchFocused ? 0.510 : 0.28,
                         m_searchFocused ? 0.965 : 0.36);
    cairo_set_line_width(m_cr, m_searchFocused ? 1.5 : 1.0);
    cairo_stroke(m_cr);

    if (m_searchQuery.empty()) {
        const char* placeholder = m_searchFocused
                                      ? "Type preset name (e.g. piano, organ, strings)..."
                                      : "Search presets by text...";
        drawText(m_cr, placeholder, sbox.x + 10.0, sbox.y + 5.0, sbox.w - 36.0, "Sans 9",
                 0.48, 0.54, 0.62, 1.0, PANGO_ALIGN_LEFT, PANGO_ELLIPSIZE_END);
    } else {
        std::string displayQuery = m_searchQuery + (m_searchFocused ? "|" : "");
        drawText(m_cr, displayQuery.c_str(), sbox.x + 10.0, sbox.y + 5.0, sbox.w - 38.0, "Sans 9.5",
                 0.94, 0.96, 0.99, 1.0, PANGO_ALIGN_LEFT, PANGO_ELLIPSIZE_START);
        const Rect clr = searchClearRect();
        drawText(m_cr, "×", clr.x, clr.y + 1.0, clr.w, "Sans Bold 11",
                 m_hoverSearchClear ? 0.98 : 0.65, m_hoverSearchClear ? 0.98 : 0.72,
                 m_hoverSearchClear ? 1.0 : 0.80, 1.0, PANGO_ALIGN_CENTER);
    }

    // Volume & Mod card
    drawRoundedRect(m_cr, 20.0, 194.0, 460.0, 42.0, 8.0);
    cairo_set_source_rgb(m_cr, 0.129, 0.149, 0.192);
    cairo_fill_preserve(m_cr);
    cairo_set_source_rgb(m_cr, 0.20, 0.23, 0.29);
    cairo_set_line_width(m_cr, 1.0);
    cairo_stroke(m_cr);

    // Coarse & Fine Tuning card
    drawRoundedRect(m_cr, 20.0, 244.0, 460.0, 42.0, 8.0);
    cairo_set_source_rgb(m_cr, 0.129, 0.149, 0.192);
    cairo_fill_preserve(m_cr);
    cairo_set_source_rgb(m_cr, 0.20, 0.23, 0.29);
    cairo_set_line_width(m_cr, 1.0);
    cairo_stroke(m_cr);

    auto drawPillBtn = [&](const Rect& r, const char* label, bool active, bool hover, bool dimmed = false) {
        drawRoundedRect(m_cr, r.x, r.y, r.w, r.h, 11.0);
        if (active) {
            cairo_set_source_rgb(m_cr, hover ? 0.294 : 0.231, hover ? 0.561 : 0.510, hover ? 0.980 : 0.965);
        } else {
            cairo_set_source_rgb(m_cr, hover ? 0.24 : 0.18, hover ? 0.28 : 0.21, hover ? 0.36 : 0.27);
        }
        cairo_fill(m_cr);
        const double ta = dimmed ? 0.50 : 1.0;
        drawText(m_cr, label, r.x, r.y + 4.0, r.w, "Sans Bold 8.5",
                 active ? 1.0 : 0.80, active ? 1.0 : 0.84, active ? 1.0 : 0.90, ta, PANGO_ALIGN_CENTER);
    };

    drawPillBtn(resetTuneRect(), "Reset", false, m_hoverResetTune);

    // Portamento card
    drawRoundedRect(m_cr, 20.0, 294.0, 460.0, 42.0, 8.0);
    cairo_set_source_rgb(m_cr, 0.129, 0.149, 0.192);
    cairo_fill_preserve(m_cr);
    cairo_set_source_rgb(m_cr, 0.20, 0.23, 0.29);
    cairo_set_line_width(m_cr, 1.0);
    cairo_stroke(m_cr);

    const bool portaEnabled = m_controller ? (m_controller->getParamNormalized(kParamPortamentoEnabled) >= 0.5) : false;
    const bool portaLegato = m_controller ? (m_controller->getParamNormalized(kParamPortamentoMode) >= 0.5) : false;
    drawPillBtn(portaToggleRect(), portaEnabled ? "Porta: ON" : "Porta: OFF", portaEnabled, m_hoverPortaToggle);
    drawPillBtn(portaModeRect(), portaLegato ? "Legato" : "Retrig", portaEnabled, m_hoverPortaMode, !portaEnabled);

    // ADSR card
    drawRoundedRect(m_cr, 20.0, 344.0, 460.0, 116.0, 8.0);
    cairo_set_source_rgb(m_cr, 0.129, 0.149, 0.192);
    cairo_fill_preserve(m_cr);
    cairo_set_source_rgb(m_cr, 0.20, 0.23, 0.29);
    cairo_set_line_width(m_cr, 1.0);
    cairo_stroke(m_cr);

    drawText(m_cr, "Envelope (ADSR)", 34.0, 352.0, 140.0, "Sans Bold 9.5",
             0.88, 0.91, 0.96, 1.0);
    if (!adsrEnabled) {
        drawText(m_cr, "SF2 native (15 ms anti-click)", 158.0, 353.0, 134.0, "Sans 8",
                 0.55, 0.61, 0.69, 1.0);
    }

    drawPillBtn(resetAdsrRect(), "Reset", false, m_hoverResetAdsr);
    drawPillBtn(adsrToggleRect(), adsrEnabled ? "ADSR: ON" : "ADSR: OFF", adsrEnabled, m_hoverAdsrToggle);

    // Draw all 9 sliders uniformly
    const auto& allSliders = sliders();
    for (int i = 0; i < static_cast<int>(allSliders.size()); ++i) {
        const auto& spec = allSliders[static_cast<size_t>(i)];
        const double val = m_controller ? m_controller->getParamNormalized(spec.paramId) : spec.defaultValue;
        const bool dimmed = (spec.isAdsr && !adsrEnabled) || (spec.paramId == kParamGlideTime && !portaEnabled);
        const double alpha = dimmed ? 0.45 : 1.0;

        drawText(m_cr, spec.label, spec.labelRect.x, spec.labelRect.y, spec.labelRect.w, "Sans 9",
                 0.82, 0.86, 0.92, alpha);

        drawRoundedRect(m_cr, spec.trackRect.x, spec.trackRect.y, spec.trackRect.w, spec.trackRect.h, 3.0);
        cairo_set_source_rgba(m_cr, 0.08, 0.09, 0.12, 1.0);
        cairo_fill(m_cr);

        if (spec.isBipolar) {
            const double centerX = spec.trackRect.x + spec.trackRect.w * 0.5;
            const double thumbX = spec.trackRect.x + spec.trackRect.w * val;
            const double fx = std::min(centerX, thumbX);
            const double fw = std::abs(thumbX - centerX);
            if (fw > 1.0) {
                drawRoundedRect(m_cr, fx, spec.trackRect.y, fw, spec.trackRect.h, 3.0);
                cairo_set_source_rgba(m_cr, 0.231, 0.510, 0.965, alpha);
                cairo_fill(m_cr);
            }
            cairo_set_source_rgba(m_cr, 0.45, 0.50, 0.60, 0.6);
            cairo_rectangle(m_cr, centerX - 0.5, spec.trackRect.y - 2.0, 1.0, spec.trackRect.h + 4.0);
            cairo_fill(m_cr);
        } else {
            const double fillW = std::max(0.0, spec.trackRect.w * val);
            if (fillW > 1.0) {
                drawRoundedRect(m_cr, spec.trackRect.x, spec.trackRect.y, fillW, spec.trackRect.h, 3.0);
                cairo_set_source_rgba(m_cr, 0.231, 0.510, 0.965, dimmed ? 0.35 : 1.0);
                cairo_fill(m_cr);
            }
        }

        const double thumbX = spec.trackRect.x + spec.trackRect.w * val;
        const double thumbY = spec.trackRect.y + spec.trackRect.h * 0.5;
        const double thumbR = (m_draggingSliderIdx == i || m_hoverSliderIdx == i) ? 7.5 : 6.5;
        cairo_arc(m_cr, thumbX, thumbY, thumbR, 0.0, 2.0 * 3.14159265358979323846);
        cairo_set_source_rgba(m_cr, 0.95, 0.97, 1.0, dimmed ? 0.55 : 1.0);
        cairo_fill_preserve(m_cr);
        cairo_set_source_rgba(m_cr, 0.231, 0.510, 0.965, dimmed ? 0.45 : 1.0);
        cairo_set_line_width(m_cr, 1.6);
        cairo_stroke(m_cr);

        char valBuf[32] = {};
        formatSliderValue(spec.paramId, val, valBuf, sizeof(valBuf));
        drawText(m_cr, valBuf, spec.valueRect.x, spec.valueRect.y, spec.valueRect.w, "Sans Bold 8.5",
                 0.86, 0.90, 0.95, alpha, PANGO_ALIGN_RIGHT);
    }

    drawVelocityPanel();

    if (m_dropdownMode != DropdownMode::Closed && hasPresets) {
        drawDropdown(status);
    }

    cairo_surface_flush(m_backSurface);
    m_cr = origCr;
    cairo_set_source_surface(m_cr, m_backSurface, 0.0, 0.0);
    cairo_paint(m_cr);

    cairo_surface_flush(m_surface);
    XFlush(m_display);
}

void X11View::drawVelocityPanel() {
    // Card background
    drawRoundedRect(m_cr, 492.0, 48.0, 188.0, 412.0, 8.0);
    cairo_set_source_rgb(m_cr, 0.129, 0.149, 0.192);
    cairo_fill_preserve(m_cr);
    cairo_set_source_rgb(m_cr, 0.20, 0.23, 0.29);
    cairo_set_line_width(m_cr, 1.0);
    cairo_stroke(m_cr);

    drawText(m_cr, "Velocity Curve", 504.0, 58.0, 114.0, "Sans Bold 9.5", 0.90, 0.93, 0.97, 1.0);

    // Reset button
    const Rect rbtn = resetVelRect();
    drawRoundedRect(m_cr, rbtn.x, rbtn.y, rbtn.w, rbtn.h, 10.0);
    cairo_set_source_rgb(m_cr, m_hoverResetVel ? 0.24 : 0.18,
                         m_hoverResetVel ? 0.28 : 0.21,
                         m_hoverResetVel ? 0.36 : 0.27);
    cairo_fill(m_cr);
    drawText(m_cr, "Reset", rbtn.x, rbtn.y + 3.0, rbtn.w, "Sans Bold 8",
             0.84, 0.88, 0.94, 1.0, PANGO_ALIGN_CENTER);

    // Graph box
    const Rect vg = velGraphRect();
    cairo_set_source_rgb(m_cr, 0.085, 0.098, 0.128);
    cairo_rectangle(m_cr, vg.x, vg.y, vg.w, vg.h);
    cairo_fill_preserve(m_cr);
    cairo_set_source_rgb(m_cr, 0.24, 0.28, 0.36);
    cairo_set_line_width(m_cr, 1.0);
    cairo_stroke(m_cr);

    // 4x4 grid
    cairo_set_source_rgba(m_cr, 0.30, 0.36, 0.46, 0.35);
    cairo_set_line_width(m_cr, 0.8);
    for (int i = 1; i < 4; ++i) {
        const double frac = i * 0.25;
        cairo_move_to(m_cr, vg.x + frac * vg.w, vg.y);
        cairo_line_to(m_cr, vg.x + frac * vg.w, vg.y + vg.h);
        cairo_move_to(m_cr, vg.x, vg.y + frac * vg.h);
        cairo_line_to(m_cr, vg.x + vg.w, vg.y + frac * vg.h);
    }
    cairo_stroke(m_cr);

    // Diagonal linear reference (dashed)
    const double dashes[2] = {4.0, 4.0};
    cairo_set_dash(m_cr, dashes, 2, 0.0);
    cairo_set_source_rgba(m_cr, 0.45, 0.52, 0.64, 0.35);
    cairo_move_to(m_cr, vg.x, vg.y + vg.h);
    cairo_line_to(m_cr, vg.x + vg.w, vg.y);
    cairo_stroke(m_cr);
    cairo_set_dash(m_cr, nullptr, 0, 0.0);

    // OUT / IN axis labels
    drawText(m_cr, "OUT", vg.x + 6.0, vg.y + 4.0, 32.0, "Sans Bold 7.5", 0.55, 0.62, 0.72, 0.85);
    drawText(m_cr, "IN", vg.x + vg.w - 22.0, vg.y + vg.h - 14.0, 18.0, "Sans Bold 7.5",
             0.55, 0.62, 0.72, 0.85, PANGO_ALIGN_RIGHT);

    const VelocityCurve curve{
        m_controller ? m_controller->getParamNormalized(kParamVelP1X) : kDefaultVelP1X,
        m_controller ? m_controller->getParamNormalized(kParamVelP1Y) : kDefaultVelP1Y,
        m_controller ? m_controller->getParamNormalized(kParamVelP2X) : kDefaultVelP2X,
        m_controller ? m_controller->getParamNormalized(kParamVelP2Y) : kDefaultVelP2Y,
    };

    // Shaded area under curve
    cairo_save(m_cr);
    cairo_rectangle(m_cr, vg.x, vg.y, vg.w, vg.h);
    cairo_clip(m_cr);

    constexpr int kSteps = 64;
    cairo_move_to(m_cr, vg.x, vg.y + vg.h);
    for (int i = 0; i <= kSteps; ++i) {
        const float tx = static_cast<float>(i) / kSteps;
        const float ty = curve.evaluate(tx);
        cairo_line_to(m_cr, vg.x + tx * vg.w, vg.y + (1.0 - ty) * vg.h);
    }
    cairo_line_to(m_cr, vg.x + vg.w, vg.y + vg.h);
    cairo_close_path(m_cr);
    cairo_set_source_rgba(m_cr, 0.231, 0.510, 0.965, 0.16);
    cairo_fill(m_cr);

    // Curve stroke
    for (int i = 0; i <= kSteps; ++i) {
        const float tx = static_cast<float>(i) / kSteps;
        const float ty = curve.evaluate(tx);
        const double px = vg.x + tx * vg.w;
        const double py = vg.y + (1.0 - ty) * vg.h;
        if (i == 0) cairo_move_to(m_cr, px, py);
        else cairo_line_to(m_cr, px, py);
    }
    cairo_set_source_rgb(m_cr, 0.30, 0.62, 0.99);
    cairo_set_line_width(m_cr, 2.2);
    cairo_stroke(m_cr);
    cairo_restore(m_cr);

    // Control points P1 & P2
    auto drawHandle = [&](int idx, double pxNorm, double pyNorm, const char* tag) {
        const double hx = vg.x + pxNorm * vg.w;
        const double hy = vg.y + (1.0 - pyNorm) * vg.h;
        const bool active = (m_draggingVelPoint == idx || m_hoverVelPoint == idx);
        const double rad = active ? 7.5 : 6.0;

        cairo_new_path(m_cr);
        cairo_arc(m_cr, hx, hy, rad, 0.0, 2.0 * 3.14159265358979323846);
        cairo_set_source_rgb(m_cr, active ? 1.0 : 0.94, active ? 1.0 : 0.96, 1.0);
        cairo_fill_preserve(m_cr);
        cairo_set_source_rgb(m_cr, 0.231, 0.510, 0.965);
        cairo_set_line_width(m_cr, 2.0);
        cairo_stroke(m_cr);

        const double tx = (hx + 20.0 > vg.x + vg.w) ? (hx - 20.0) : (hx + 8.0);
        const double ty = (hy - 14.0 < vg.y) ? (hy + 6.0) : (hy - 14.0);
        drawText(m_cr, tag, tx, ty, 20.0, "Sans Bold 7.5", 0.85, 0.91, 0.98, 0.9);
    };
    drawHandle(0, curve.p1x, curve.p1y, "P1");
    drawHandle(1, curve.p2x, curve.p2y, "P2");

    // Readout of P1 & P2 in MIDI velocity units (0..127)
    char p1Buf[64] = {}, p2Buf[64] = {};
    std::snprintf(p1Buf, sizeof(p1Buf), "P1: In %d -> Out %d",
                  static_cast<int>(std::lround(curve.p1x * 127.0)),
                  static_cast<int>(std::lround(curve.p1y * 127.0)));
    std::snprintf(p2Buf, sizeof(p2Buf), "P2: In %d -> Out %d",
                  static_cast<int>(std::lround(curve.p2x * 127.0)),
                  static_cast<int>(std::lround(curve.p2y * 127.0)));
    drawText(m_cr, p1Buf, 506.0, 264.0, 160.0, "Sans 8.5", 0.78, 0.84, 0.92, 1.0);
    drawText(m_cr, p2Buf, 506.0, 284.0, 160.0, "Sans 8.5", 0.78, 0.84, 0.92, 1.0);

    // Quick curve preset buttons
    drawText(m_cr, "Quick Presets", 506.0, 330.0, 160.0, "Sans Bold 8.5", 0.65, 0.72, 0.82, 1.0);
    static constexpr const char* kPresetNames[4] = {"Linear", "Soft", "Hard", "Fixed 127"};
    for (int p = 0; p < 4; ++p) {
        const Rect pr = velPresetRect(p);
        const bool hov = (m_hoverVelPreset == p);
        drawRoundedRect(m_cr, pr.x, pr.y, pr.w, pr.h, 5.0);
        cairo_set_source_rgb(m_cr, hov ? 0.24 : 0.18, hov ? 0.28 : 0.21, hov ? 0.36 : 0.27);
        cairo_fill_preserve(m_cr);
        cairo_set_source_rgb(m_cr, 0.27, 0.32, 0.40);
        cairo_set_line_width(m_cr, 1.0);
        cairo_stroke(m_cr);
        drawText(m_cr, kPresetNames[p], pr.x, pr.y + 3.0, pr.w, "Sans Bold 8",
                 0.88, 0.92, 0.98, 1.0, PANGO_ALIGN_CENTER);
    }

    drawText(m_cr, "Drag P1/P2 points on graph.\nSnap to left/right edges for Fixed.",
             506.0, 418.0, 162.0, "Sans 7.5", 0.54, 0.60, 0.68, 1.0);
}

} // namespace SuperSimpleSF2
