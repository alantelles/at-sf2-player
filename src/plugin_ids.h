#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "pluginterfaces/base/ibstream.h"
#include "velocity_curve.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace SuperSimpleSF2 {

// Unique 128-bit class IDs for Processor and Controller
static const Steinberg::FUID kProcessorUID(0x8F4A2C10, 0x9E3D4B7A, 0xA1B2C3D4, 0xE5F60101);
static const Steinberg::FUID kControllerUID(0x8F4A2C10, 0x9E3D4B7A, 0xA1B2C3D4, 0xE5F60102);

// Parameter IDs
enum ParamIDs : Steinberg::Vst::ParamID {
    kParamVolume = 0,
    kParamPitchBend = 1,
    kParamSustain = 2,
    kParamModWheel = 3,
    kParamAdsrEnabled = 4,
    kParamAttack = 5,
    kParamDecay = 6,
    kParamSustainLevel = 7,
    kParamRelease = 8,
    kParamCoarseTune = 9,
    kParamFineTune = 10,
    kParamPortamentoEnabled = 11,
    kParamPortamentoMode = 12, // 0 = Retrigger, 1 = Legato
    kParamGlideTime = 13,
    kParamVelP1X = 14,
    kParamVelP1Y = 15,
    kParamVelP2X = 16,
    kParamVelP2Y = 17,
};

// Default normalized values
constexpr double kDefaultVolumeNorm = 0.8;
constexpr double kDefaultPitchBendNorm = 0.5;
constexpr double kDefaultSustainNorm = 0.0;
constexpr double kDefaultModWheelNorm = 0.0;
constexpr double kDefaultAdsrEnabledNorm = 1.0; // ADSR enabled by default
constexpr double kDefaultAttackNorm = 0.0;
constexpr double kDefaultDecayNorm = 0.35;
constexpr double kDefaultSustainLevelNorm = 0.8;
constexpr double kDefaultReleaseNorm = 0.22;
constexpr double kDefaultCoarseTuneNorm = 0.5; // 0 semitones (-24..+24)
constexpr double kDefaultFineTuneNorm = 0.5;   // 0 cents (-100..+100)
constexpr double kDefaultPortamentoEnabledNorm = 0.0;
constexpr double kDefaultPortamentoModeNorm = 0.0; // Retrigger
constexpr double kDefaultGlideTimeNorm = 0.35;     // ~90 ms
constexpr double kDefaultVelP1X = 0.33;
constexpr double kDefaultVelP1Y = 0.33;
constexpr double kDefaultVelP2X = 0.67;
constexpr double kDefaultVelP2Y = 0.67;

// Inter-component message IDs and attribute keys
constexpr const char* kMsgLoadSf2 = "LoadSF2";
constexpr const char* kMsgSelectPreset = "SelectPreset";
constexpr const char* kMsgSf2Status = "SF2Status";
constexpr const char* kAttrPath = "path";
constexpr const char* kAttrLoaded = "loaded";
constexpr const char* kAttrPresetCount = "presetCount";
constexpr const char* kAttrPresetIndex = "presetIndex";
constexpr const char* kAttrFirstPreset = "firstPreset";
constexpr const char* kAttrPresetBlob = "presetBlob";
constexpr const char* kAttrError = "error";

// Versioned binary state header
constexpr uint32_t kStateMagic = 0x53534632; // "SSF2"
constexpr uint32_t kStateVersion1 = 1;
constexpr uint32_t kStateVersion2 = 2;
constexpr uint32_t kStateVersion3 = 3;
constexpr uint32_t kStateVersion4 = 4;
constexpr uint32_t kStateVersion = 5;

struct PresetEntry {
    int32_t index = 0;
    int32_t bank = 0;
    int32_t program = 0;
    std::string name;
};

struct AdsrSettings {
    bool enabled = true;
    double attackNorm = kDefaultAttackNorm;
    double decayNorm = kDefaultDecayNorm;
    double sustainNorm = kDefaultSustainLevelNorm;
    double releaseNorm = kDefaultReleaseNorm;
};

struct PortamentoSettings {
    bool enabled = false;
    bool legato = false;
    double glideNorm = kDefaultGlideTimeNorm;
};

// Parameter curve helpers
inline float volumeNormToGain(double norm) {
    norm = std::clamp(norm, 0.0, 1.0);
    const float f = static_cast<float>(norm);
    return f * f;
}

inline float attackNormToSec(double norm) {
    const double t = std::clamp(norm, 0.0, 1.0);
    return static_cast<float>(0.002 + 2.998 * (t * t * t)); // 2 ms .. 3000 ms
}

inline float decayNormToSec(double norm) {
    const double t = std::clamp(norm, 0.0, 1.0);
    return static_cast<float>(0.010 + 4.990 * (t * t * t)); // 10 ms .. 5000 ms
}

inline float releaseNormToSec(double norm) {
    const double t = std::clamp(norm, 0.0, 1.0);
    return static_cast<float>(0.012 + 4.988 * (t * t * t)); // 12 ms .. 5000 ms (anti-click floor)
}

inline float sustainNormToLevel(double norm) {
    return static_cast<float>(std::clamp(norm, 0.0, 1.0));
}

inline int coarseNormToSemitones(double norm) {
    const double clamped = std::clamp(norm, 0.0, 1.0);
    return static_cast<int>(std::lround(clamped * 48.0)) - 24;
}

inline float fineNormToCents(double norm) {
    const double clamped = std::clamp(norm, 0.0, 1.0);
    return static_cast<float>(clamped * 200.0 - 100.0);
}

inline float tuningSemitonesTotal(double coarseNorm, double fineNorm) {
    return static_cast<float>(coarseNormToSemitones(coarseNorm)) + (fineNormToCents(fineNorm) / 100.0f);
}

inline float glideNormToSec(double norm) {
    const double t = std::clamp(norm, 0.0, 1.0);
    return static_cast<float>(0.005 + 1.995 * (t * t * t)); // 5 ms .. 2000 ms
}

// Serialize/deserialize preset list for IMessage transfer
inline std::vector<uint8_t> serializePresetList(const std::vector<PresetEntry>& presets) {
    std::vector<uint8_t> buf;
    auto appendBytes = [&buf](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    };
    const uint32_t count = static_cast<uint32_t>(presets.size());
    appendBytes(&count, sizeof(count));
    for (const auto& p : presets) {
        appendBytes(&p.index, sizeof(p.index));
        appendBytes(&p.bank, sizeof(p.bank));
        appendBytes(&p.program, sizeof(p.program));
        const uint16_t len = static_cast<uint16_t>(std::min<size_t>(p.name.size(), 255));
        appendBytes(&len, sizeof(len));
        if (len > 0) appendBytes(p.name.data(), len);
    }
    return buf;
}

inline std::vector<PresetEntry> deserializePresetList(const void* data, uint32_t sizeBytes) {
    std::vector<PresetEntry> out;
    if (!data || sizeBytes < sizeof(uint32_t)) return out;
    const auto* p = static_cast<const uint8_t*>(data);
    size_t pos = 0;
    auto readBytes = [&](void* dst, size_t n) -> bool {
        if (pos + n > sizeBytes) return false;
        std::memcpy(dst, p + pos, n);
        pos += n;
        return true;
    };
    uint32_t count = 0;
    if (!readBytes(&count, sizeof(count)) || count > 65536) return out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        PresetEntry entry;
        uint16_t len = 0;
        if (!readBytes(&entry.index, sizeof(entry.index))) break;
        if (!readBytes(&entry.bank, sizeof(entry.bank))) break;
        if (!readBytes(&entry.program, sizeof(entry.program))) break;
        if (!readBytes(&len, sizeof(len))) break;
        if (pos + len > sizeBytes) break;
        entry.name.assign(reinterpret_cast<const char*>(p + pos), len);
        pos += len;
        out.push_back(std::move(entry));
    }
    return out;
}

// Helper: read/write exact bytes from IBStream
inline bool writeStreamBytes(Steinberg::IBStream* s, const void* data, int32_t size) {
    if (!s || size < 0) return false;
    int32_t written = 0;
    const auto res = s->write(const_cast<void*>(data), size, &written);
    return res == Steinberg::kResultOk && written == size;
}

inline bool readStreamBytes(Steinberg::IBStream* s, void* data, int32_t size) {
    if (!s || size < 0) return false;
    int32_t readBytes = 0;
    const auto res = s->read(data, size, &readBytes);
    return res == Steinberg::kResultOk && readBytes == size;
}

struct PluginStateData {
    double volumeNorm = kDefaultVolumeNorm;
    double modWheelNorm = kDefaultModWheelNorm;
    AdsrSettings adsr;
    int32_t selectedPresetIndex = 0;
    double coarseTuneNorm = kDefaultCoarseTuneNorm;
    double fineTuneNorm = kDefaultFineTuneNorm;
    PortamentoSettings portamento;
    VelocityCurve velocityCurve;
    std::string sf2Path;

    bool writeToStream(Steinberg::IBStream* stream) const {
        if (!stream) return false;
        const uint32_t magic = kStateMagic;
        const uint32_t version = kStateVersion;
        const uint32_t pathLen = static_cast<uint32_t>(sf2Path.size());
        const uint8_t adsrFlag = adsr.enabled ? 1 : 0;
        const uint8_t portaFlag = portamento.enabled ? 1 : 0;
        const uint8_t legatoFlag = portamento.legato ? 1 : 0;

        if (!writeStreamBytes(stream, &magic, sizeof(magic))) return false;
        if (!writeStreamBytes(stream, &version, sizeof(version))) return false;
        if (!writeStreamBytes(stream, &volumeNorm, sizeof(volumeNorm))) return false;
        if (!writeStreamBytes(stream, &pathLen, sizeof(pathLen))) return false;
        if (pathLen > 0) {
            if (!writeStreamBytes(stream, sf2Path.data(), static_cast<int32_t>(pathLen))) return false;
        }
        if (!writeStreamBytes(stream, &selectedPresetIndex, sizeof(selectedPresetIndex))) return false;
        if (!writeStreamBytes(stream, &modWheelNorm, sizeof(modWheelNorm))) return false;
        if (!writeStreamBytes(stream, &adsrFlag, sizeof(adsrFlag))) return false;
        if (!writeStreamBytes(stream, &adsr.attackNorm, sizeof(adsr.attackNorm))) return false;
        if (!writeStreamBytes(stream, &adsr.decayNorm, sizeof(adsr.decayNorm))) return false;
        if (!writeStreamBytes(stream, &adsr.sustainNorm, sizeof(adsr.sustainNorm))) return false;
        if (!writeStreamBytes(stream, &adsr.releaseNorm, sizeof(adsr.releaseNorm))) return false;
        if (!writeStreamBytes(stream, &coarseTuneNorm, sizeof(coarseTuneNorm))) return false;
        if (!writeStreamBytes(stream, &fineTuneNorm, sizeof(fineTuneNorm))) return false;
        if (!writeStreamBytes(stream, &portaFlag, sizeof(portaFlag))) return false;
        if (!writeStreamBytes(stream, &legatoFlag, sizeof(legatoFlag))) return false;
        if (!writeStreamBytes(stream, &portamento.glideNorm, sizeof(portamento.glideNorm))) return false;
        if (!writeStreamBytes(stream, &velocityCurve.p1x, sizeof(velocityCurve.p1x))) return false;
        if (!writeStreamBytes(stream, &velocityCurve.p1y, sizeof(velocityCurve.p1y))) return false;
        if (!writeStreamBytes(stream, &velocityCurve.p2x, sizeof(velocityCurve.p2x))) return false;
        if (!writeStreamBytes(stream, &velocityCurve.p2y, sizeof(velocityCurve.p2y))) return false;
        return true;
    }

    bool readFromStream(Steinberg::IBStream* stream) {
        if (!stream) return false;
        uint32_t magic = 0;
        uint32_t version = 0;
        if (!readStreamBytes(stream, &magic, sizeof(magic)) || magic != kStateMagic) return false;
        if (!readStreamBytes(stream, &version, sizeof(version))) return false;
        if (version != kStateVersion1 && version != kStateVersion2 &&
            version != kStateVersion3 && version != kStateVersion4 &&
            version != kStateVersion) return false;

        double vol = kDefaultVolumeNorm;
        if (!readStreamBytes(stream, &vol, sizeof(vol))) return false;
        vol = std::clamp(vol, 0.0, 1.0);

        uint32_t pathLen = 0;
        if (!readStreamBytes(stream, &pathLen, sizeof(pathLen))) return false;
        if (pathLen > 65536) return false;

        std::string path;
        if (pathLen > 0) {
            path.resize(pathLen);
            if (!readStreamBytes(stream, &path[0], static_cast<int32_t>(pathLen))) return false;
        }

        volumeNorm = vol;
        sf2Path = std::move(path);

        if (version >= kStateVersion2) {
            int32_t presetIdx = 0;
            double modWheel = kDefaultModWheelNorm;
            uint8_t adsrFlag = 1;
            double a = kDefaultAttackNorm;
            double d = kDefaultDecayNorm;
            double s = kDefaultSustainLevelNorm;
            double r = kDefaultReleaseNorm;

            if (!readStreamBytes(stream, &presetIdx, sizeof(presetIdx))) return false;
            if (!readStreamBytes(stream, &modWheel, sizeof(modWheel))) return false;
            if (!readStreamBytes(stream, &adsrFlag, sizeof(adsrFlag))) return false;
            if (!readStreamBytes(stream, &a, sizeof(a))) return false;
            if (!readStreamBytes(stream, &d, sizeof(d))) return false;
            if (!readStreamBytes(stream, &s, sizeof(s))) return false;
            if (!readStreamBytes(stream, &r, sizeof(r))) return false;

            selectedPresetIndex = std::max<int32_t>(0, presetIdx);
            modWheelNorm = std::clamp(modWheel, 0.0, 1.0);
            adsr.enabled = (adsrFlag != 0);
            adsr.attackNorm = std::clamp(a, 0.0, 1.0);
            adsr.decayNorm = std::clamp(d, 0.0, 1.0);
            adsr.sustainNorm = std::clamp(s, 0.0, 1.0);
            adsr.releaseNorm = std::clamp(r, 0.0, 1.0);
        }

        if (version >= kStateVersion3) {
            double coarse = kDefaultCoarseTuneNorm;
            double fine = kDefaultFineTuneNorm;
            if (!readStreamBytes(stream, &coarse, sizeof(coarse))) return false;
            if (!readStreamBytes(stream, &fine, sizeof(fine))) return false;
            coarseTuneNorm = std::clamp(coarse, 0.0, 1.0);
            fineTuneNorm = std::clamp(fine, 0.0, 1.0);
        }

        if (version >= kStateVersion4) {
            uint8_t portaFlag = 0;
            uint8_t legatoFlag = 0;
            double glide = kDefaultGlideTimeNorm;
            if (!readStreamBytes(stream, &portaFlag, sizeof(portaFlag))) return false;
            if (!readStreamBytes(stream, &legatoFlag, sizeof(legatoFlag))) return false;
            if (!readStreamBytes(stream, &glide, sizeof(glide))) return false;
            portamento.enabled = (portaFlag != 0);
            portamento.legato = (legatoFlag != 0);
            portamento.glideNorm = std::clamp(glide, 0.0, 1.0);
        }

        if (version >= kStateVersion) {
            double x1 = kDefaultVelP1X, y1 = kDefaultVelP1Y;
            double x2 = kDefaultVelP2X, y2 = kDefaultVelP2Y;
            if (!readStreamBytes(stream, &x1, sizeof(x1))) return false;
            if (!readStreamBytes(stream, &y1, sizeof(y1))) return false;
            if (!readStreamBytes(stream, &x2, sizeof(x2))) return false;
            if (!readStreamBytes(stream, &y2, sizeof(y2))) return false;
            velocityCurve.p1x = std::clamp(x1, 0.0, 1.0);
            velocityCurve.p1y = std::clamp(y1, 0.0, 1.0);
            velocityCurve.p2x = std::clamp(x2, 0.0, 1.0);
            velocityCurve.p2y = std::clamp(y2, 0.0, 1.0);
        }

        return true;
    }
};

} // namespace SuperSimpleSF2
