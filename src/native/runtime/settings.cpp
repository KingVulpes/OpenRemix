#include "runtime/settings.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <type_traits>

namespace remix::config {
  namespace {
    struct Entry {
      std::string key;
      std::function<std::string(const Settings &)> read;
      std::function<bool(Settings &, std::string_view)> write;
    };
    template <typename T> std::string number(T value) {
      char buffer[64];
      auto result = [&] {
        if constexpr (std::is_floating_point_v<T>)
          return std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general,
                               std::numeric_limits<T>::max_digits10);
        else
          return std::to_chars(buffer, buffer + sizeof(buffer), unsigned(value));
      }();
      return {buffer, result.ptr};
    }
    template <typename T> bool readNumber(std::string_view text, T &value) {
      if constexpr (std::is_unsigned_v<T>) {
        // Retain the configuration API's historical integer/boolean aliases.
        if (text == "true") {
          value = 1;
          return true;
        }
        if (text == "false") {
          value = 0;
          return true;
        }
      }
      auto result = std::from_chars(text.data(), text.data() + text.size(), value);
      return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    }
    template <typename Access>
    Entry scalar(std::string key, Access access, double low, double high, double gap = 0,
                 bool sentinel = false) {
      return {std::move(key), [=](const Settings &s) { return number(access(s)); },
              [=](Settings &s, std::string_view text) {
                using T = std::remove_cvref_t<decltype(access(s))>;
                using Parsed = std::conditional_t<std::is_same_v<T, bool>, unsigned, T>;
                Parsed value{};
                if (!readNumber(text, value) || !std::isfinite(double(value)) ||
                    (sentinel && double(value) == -1
                         ? false
                         : value < Parsed(low) || value > Parsed(high) || (value > 0 && value < Parsed(gap))))
                  return false;
                access(s) = T(value);
                return true;
              }};
    }
    template <typename Access>
    Entry choices(std::string key, Access access, std::vector<std::string> values) {
      return {std::move(key),
              [=](const Settings &s) {
                size_t index = size_t(access(s));
                return index < values.size() ? values[index] : "invalid";
              },
              [=](Settings &s, std::string_view text) {
                auto found = std::find(values.begin(), values.end(), text);
                if (found == values.end())
                  return false;
                access(s) = std::remove_cvref_t<decltype(access(s))>(found - values.begin());
                return true;
              }};
    }
    const std::vector<Entry> &registry() {
      static const std::vector<Entry> entries = [] {
        std::vector<Entry> result;
#define FIELD(member) [](auto &s) -> auto & { return s.member; }
#define TRACE(member, low, high)                                                                             \
  result.push_back(scalar("rtx.native.pathtrace." #member, FIELD(trace.member), low, high))
        result.push_back(choices("rtx.native.backend", FIELD(backend), {"raster", "pathtrace"}));
        result.push_back({"rtx.native.vsync", [](const Settings &s) { return number(s.vsync); },
                          [](Settings &s, std::string_view value) {
                            std::string lower(value);
                            for (auto &c : lower)
                              if (c >= 'A' && c <= 'Z')
                                c += 'a' - 'A';
                            if (lower != "0" && lower != "1" && lower != "true" && lower != "false")
                              return false;
                            s.vsync = lower == "1" || lower == "true";
                            return true;
                          }});
        result.push_back(choices("rtx.native.pathtrace.upscaler", FIELD(trace.upscaler),
                                 {"off", "auto", "fsr31", "dlss"}));
        result.push_back(choices("rtx.native.pathtrace.dlssRayReconstruction",
                                 FIELD(trace.dlssRayReconstruction), {"0", "1"}));
        result.push_back(
            choices("rtx.native.pathtrace.primaryVisibility", FIELD(trace.rasterPrimary), {"ray", "raster"}));
        result.push_back(
            choices("rtx.native.pathtrace.denoiser", FIELD(trace.denoiser), {"off", "reblur-sh"}));
        result.push_back(choices("rtx.native.pathtrace.fsrQuality", FIELD(trace.fsrQuality),
                                 {"native-aa", "quality", "balanced", "performance", "ultra-performance"}));
        result.push_back(choices("rtx.native.pathtrace.dlssQuality", FIELD(trace.dlssQuality),
                                 {"native-aa", "quality", "balanced", "performance", "ultra-performance"}));
        TRACE(samples, 1, 16);
        TRACE(bounces, 0, 4);
        TRACE(russianRoulette, 0, 1);
        TRACE(rouletteMinBounces, 1, 4);
        TRACE(accumulate, 0, 1);
        TRACE(sigma, 0, 1);
        TRACE(blueNoiseShadows, 0, 1);
        TRACE(blueNoiseLocalLights, 0, 1);
        TRACE(autoExposure, 0, 1);
        TRACE(exposure, .05, 8);
        TRACE(autoExposureTarget, .02, .6);
        TRACE(autoExposureSpeed, .1, 16);
        TRACE(autoExposureCentre, 0, 1);
        TRACE(autoExposureMinEv, -24, 0);
        TRACE(autoExposureMaxEv, 0, 24);
        TRACE(bloom, 0, 1);
        TRACE(bloomIntensity, 0, 1);
        TRACE(bloomRadius, 0, 1);
        TRACE(prism, 0, 1);
        TRACE(gamma22, 0, 1);
        TRACE(localLights, 0, 1);
        TRACE(torchLights, 0, 1);
        TRACE(emissiveLights, 0, 1);
        TRACE(adaptiveLocalSampling, 0, 1);
        TRACE(stabilizeEmissiveLighting, 0, 1);
        TRACE(emissiveSolidAngleSampling, 0, 1);
        TRACE(freezeEmissiveRadiance, 0, 1);
        TRACE(emissiveCandidates, 1, 8);
        TRACE(sphereCandidates, 1, 8);
        TRACE(separateLocalLighting, 0, 3);
        TRACE(localFireflyFilter, 0, 1);
        TRACE(localDenoiser, 0, 1);
        TRACE(localGuiding, 0, 1);
        TRACE(localGuideCellSize, .05, 100);
        TRACE(localAntilagSigma, .1, 16);
        TRACE(localAntilagSensitivity, .1, 16);
        TRACE(localHistoryFrames, 2, 64);
        TRACE(localHistoryConfidence, 0, 1);
        TRACE(sceneScale, .0001, 1000);
        TRACE(lightVisibilityCulling, 0, 1);
        TRACE(restir, 0, 1);
        TRACE(restirCandidates, 1, 32);
        TRACE(restirSpatialSamples, 0, 8);
        TRACE(restirSpatialRadius, 1, 100);
        TRACE(restirTemporalHistory, 0, 64);
        TRACE(restirInitialVisibility, 0, 1);
        TRACE(restirPermutation, 0, 1);
        TRACE(localSamples, 1, 16);
        TRACE(shadowDebug, 0, 2);
        TRACE(debugView, 0, 11);
        TRACE(taa, 0, 1);
        TRACE(waterIndirectReuse, 0, 1);
        TRACE(translucencyDenoiser, 0, 1);
        TRACE(translucencyReuse, 0, 1);
        TRACE(pbrLocalReuse, 0, 1);
        TRACE(validation, 0, 1);
        TRACE(sharc, 0, 1);
        TRACE(sharcDebug, 0, 1);
        TRACE(sharcCapacityLog2, 16, 22);
        TRACE(sharcDownscale, 1, 16);
        TRACE(sharcSceneScale, 1, 200);
        TRACE(sharcAccumulationFrames, 1, 1024);
        TRACE(sharcStaleFrames, 8, 1024);
        TRACE(sky, 0, 1);
        TRACE(forceRebuild, 0, 1);
        TRACE(fsrSharpness, 0, 1);
        TRACE(localDirectClamp, 0, 1000000);
        TRACE(indirectClamp, 0, 1000000);
        TRACE(localIntensity, 0, 100);
        result.push_back(scalar("rtx.native.pathtrace.localRange", FIELD(trace.localRange), 0, 1024, .01));
        result.push_back(scalar("rtx.native.pathtrace.sunAngularDiameter", FIELD(trace.sunAngularDiameter), 0,
                                10, 0, true));
        TRACE(overrideLight, 0, 1);
        TRACE(lightIntensity, 0, 20);
        TRACE(hardwareFaceCulling, 0, 1);
        TRACE(combinedTlas, 0, 1);
        TRACE(shadowEarlyOut, 0, 1);
        for (size_t i = 0; i < 3; ++i) {
          result.push_back(scalar(
              "rtx.native.pathtrace.lightDirection." + std::string(1, "xyz"[i]),
              [i](auto &s) -> auto & { return s.trace.lightDirection[i]; }, -1, 1));
          result.push_back(scalar(
              "rtx.native.pathtrace.lightColor." + std::string(1, "rgb"[i]),
              [i](auto &s) -> auto & { return s.trace.lightColor[i]; }, 0, 1));
        }
        for (size_t i = 0; i < 4; ++i)
          result.push_back(scalar(
              "rtx.native.clearColor." + std::string(1, "rgba"[i]),
              [i](auto &s) -> auto & { return s.clearColor[i]; }, 0, 1));
        result.push_back(scalar("rtx.native.textures.keepCompressed", FIELD(compressedTextures), 0, 1));
        result.push_back(scalar("rtx.native.vramStatistics", FIELD(vramStatistics), 0, 1));
        result.push_back(scalar("rtx.native.meshes.refreshInPlace", FIELD(refreshGeometry), 0, 1));
        result.push_back(scalar("rtx.native.memoryPressure", FIELD(memoryPressure), 0, 1));
        result.push_back(scalar("rtx.native.scene.persistentStatic", FIELD(persistentStatic), 0, 1));
        result.push_back(
            scalar("rtx.native.scene.persistentStaticValidation", FIELD(persistentStaticValidation), 0, 1));
        result.push_back(scalar("rtx.native.asyncPresent", FIELD(asyncPresent), 0, 1));
        result.push_back(scalar("rtx.native.gpuPassTimers", FIELD(gpuPassTimers), 0, 1));
#undef TRACE
#undef FIELD
        return result;
      }();
      return entries;
    }
    std::string_view trim(std::string_view text) {
      const auto start = text.find_first_not_of(" \t\r");
      if (start == text.npos)
        return {};
      return text.substr(start, text.find_last_not_of(" \t\r") - start + 1);
    }
  } // namespace

  SetResult set(Settings &settings, std::string_view key, std::string_view value) {
    for (const auto &entry : registry())
      if (entry.key == key)
        return entry.write(settings, value) ? SetResult::Success : SetResult::Invalid;
    return SetResult::Unknown;
  }

  bool validate(const Settings &settings, std::string &error) {
    Settings checked;
    for (const auto &entry : registry()) {
      if (!entry.write(checked, entry.read(settings))) {
        error = "Invalid value for " + entry.key;
        return false;
      }
    }
    if (settings.trace.sharcReset) {
      error = "Cache reset is an action, not a profile setting";
      return false;
    }
    return true;
  }

  std::string serialize(const Settings &settings, const UnknownSettings &unknown) {
    std::string text = "# OpenRemix configuration profile\nprofile.version = 1\n";
    for (const auto &entry : registry())
      text += entry.key + " = " + entry.read(settings) + "\n";
    if (!unknown.empty())
      text += "\n# Unrecognized settings retained for compatibility\n";
    for (const auto &[key, value] : unknown)
      text += key + " = " + value + "\n";
    return text;
  }

  bool parse(std::string_view text, Settings &settings, UnknownSettings &unknown,
             std::vector<std::string> &warnings, std::string &error) {
    if (text.starts_with("\xef\xbb\xbf"))
      text.remove_prefix(3);
    Settings next;
    UnknownSettings extra;
    std::vector<std::string> notices;
    std::set<std::string> seen;
    bool version = false;
    size_t line = 0;
    while (!text.empty()) {
      ++line;
      auto end = text.find('\n');
      auto item = trim(text.substr(0, end));
      text = end == text.npos ? std::string_view{} : text.substr(end + 1);
      if (item.empty() || item.front() == '#' || item.front() == ';')
        continue;
      const auto equal = item.find('=');
      const auto key = trim(item.substr(0, equal));
      if (equal == item.npos || key.empty() || !seen.emplace(key).second) {
        error = "Malformed or duplicate key at line " + std::to_string(line);
        return false;
      }
      const auto value = trim(item.substr(equal + 1));
      if (key == "profile.version") {
        if (value != "1") {
          error = "Unsupported profile version: " + std::string(value);
          return false;
        }
        version = true;
        continue;
      }
      const auto result = set(next, key, value);
      if (result == SetResult::Invalid) {
        error = "Invalid value for " + std::string(key);
        return false;
      }
      if (result == SetResult::Unknown) {
        // Never replay one-shot actions from a file.
        if (key == "rtx.native.pathtrace.sharcReset") {
          notices.push_back("Ignored one-shot action: " + std::string(key));
        } else {
          extra.emplace(key, value);
          notices.push_back("Unknown key retained: " + std::string(key));
        }
      }
    }
    if (!version) {
      error = "Missing profile.version";
      return false;
    }
    if (!validate(next, error))
      return false;
    settings = next;
    unknown = std::move(extra);
    warnings = std::move(notices);
    return true;
  }
} // namespace remix::config
