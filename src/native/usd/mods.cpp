#include "usd/mods.h"
#include "usd/submission.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <optional>
#include <utility>
#include <unordered_set>

namespace remix::usd {
  namespace {
    using Clock = std::chrono::steady_clock;
    struct Set {
      std::vector<Snapshot> snapshots;
      std::map<uint64_t, std::pair<size_t, Json>> meshes, lights, materials;
      std::set<std::string> dependencies;
      // Parse failures travel in-band: an exception_ptr crossing threads is
      // destroyed early under mixed MSVC runtimes (Proton's builtin vcruntime).
      std::optional<std::string> error;
      bool loadError = false;
    };
    using Stamps = std::map<std::string, std::pair<std::filesystem::file_time_type, uintmax_t>>;
    struct Scan {
      Stamps stamps;
      std::optional<std::string> error; // in-band, like Set::error
      uint64_t epoch = 0;
    };
    // Replacement materials decoded off the Present thread once first drawn.
    struct MaterialBatch {
      uint64_t generation = 0;
      std::vector<uint64_t> hashes;
      std::vector<std::string> paths;
      std::vector<std::shared_ptr<scene::Texture>> textures;
    };
    struct State {
      bool enabled = false, watch = true;
      Options options;
      std::string directory, diagnostic;
      std::future<Set> pending;
      Set active;
      std::set<std::string> failedDependencies;
      std::set<remixapi_LightHandle> overriddenLights;
      uint64_t generation = 0;
      std::vector<std::unique_ptr<Submission>> submissions;
      std::future<MaterialBatch> materialBatch;
      std::vector<uint64_t> materialQueue;
      Stamps stamps;
      // Watch scans run off the Present thread: walking a large mod tree takes
      // far longer than a frame (notably under Wine). A scan started before
      // the baseline was last reset is discarded.
      std::future<Scan> scan;
      uint64_t scanEpoch = 0;
      Clock::time_point next{}, changed{};
    } state;
    std::vector<std::string> paths(const std::string &directory) {
      std::vector<std::string> result;
      std::error_code error;
      if (!std::filesystem::exists(directory, error))
        return result;
      for (const auto &entry : std::filesystem::recursive_directory_iterator(
               directory, std::filesystem::directory_options::skip_permission_denied)) {
        auto name = entry.path().filename().string();
        if (entry.is_regular_file() && (name == "mod.usda" || name == "mod.usdc" || name == "mod.usd"))
          result.push_back(entry.path().string());
      }
      std::sort(result.begin(), result.end());
      return result;
    }
    Set parse(std::string directory, Options options) {
      Set result;
      options.preloadTextures = true;
      for (const auto &path : paths(directory)) {
        options.path = path;
        Snapshot snapshot;
        try {
          snapshot = load(options);
        } catch (const LoadError &error) {
          result.dependencies.insert(error.dependencies.begin(), error.dependencies.end());
          throw LoadError(error.what(), std::move(result.dependencies));
        }
        const auto index = result.snapshots.size();
        for (const char *group : {"meshes", "lights", "materials"})
          for (const auto &entry : snapshot.scene["replacements"][group]) {
            auto &table = std::string(group) == "meshes"   ? result.meshes
                          : std::string(group) == "lights" ? result.lights
                                                           : result.materials;
            table.try_emplace(entry["hash"].get<uint64_t>(), index, entry);
          }
        result.dependencies.insert(snapshot.dependencies.begin(), snapshot.dependencies.end());
        result.snapshots.push_back(std::move(snapshot));
      }
      return result;
    }
    std::set<std::string> watchedDependencies() {
      auto deps = state.active.dependencies;
      deps.insert(state.failedDependencies.begin(), state.failedDependencies.end());
      return deps;
    }
    Stamps scanStamps(const std::string &directory, std::set<std::string> deps) {
      Stamps result;
      for (const auto &path : paths(directory))
        deps.insert(path);
      for (const auto &path : deps) {
        std::error_code ec;
        auto time = std::filesystem::last_write_time(path, ec);
        auto size = std::filesystem::file_size(path, ec);
        result[path] = {time, ec ? 0 : size};
      }
      return result;
    }
    Stamps stamps() {
      ++state.scanEpoch;
      return scanStamps(state.directory, watchedDependencies());
    }
    void refreshDependencies() {
      auto current = stamps();
      // Do not swallow edits made while a parser job was in flight. New
      // dependencies get one follow-up parse, then the watch baseline settles.
      if (current != state.stamps)
        state.changed = Clock::now();
      state.stamps = std::move(current);
    }
    void launch() {
      state.pending = std::async(std::launch::async, [directory = state.directory, options = state.options] {
        try {
          return parse(directory, options);
        } catch (const LoadError &e) {
          Set failed;
          failed.error = e.what();
          failed.loadError = true;
          failed.dependencies = e.dependencies;
          return failed;
        } catch (const std::exception &e) {
          Set failed;
          failed.error = e.what();
          return failed;
        }
      });
    }
    // Requested replacement materials: their textures decode in a background
    // batch; the materials are created (from the cached textures) when it
    // lands. The original materials draw meanwhile.
    void serviceMaterials(NativeResources &resources) {
      for (auto hash : resources.takeModMaterialRequests())
        state.materialQueue.push_back(hash);
      if (state.materialBatch.valid()) {
        if (state.materialBatch.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
          return;
        auto batch = state.materialBatch.get();
        if (batch.generation == state.generation) {
          for (size_t i = 0; i < batch.paths.size(); ++i)
            if (batch.textures[i])
              resources.cacheTexturePath(batch.paths[i], batch.textures[i]);
          for (auto hash : batch.hashes) {
            auto it = state.active.materials.find(hash);
            if (it == state.active.materials.end())
              continue;
            const auto &[index, entry] = it->second;
            try {
              resources.addModMaterial(hash, state.submissions[index]->materialHandle(entry["path"]));
            } catch (const std::exception &e) {
              state.diagnostic = e.what();
            } catch (...) {
              state.diagnostic = "Could not create mod material " + entry["path"].get<std::string>();
            }
          }
        }
      }
      if (state.materialQueue.empty())
        return;
      MaterialBatch batch;
      batch.generation = state.generation;
      batch.hashes = std::exchange(state.materialQueue, {});
      std::set<std::string> seen;
      for (auto hash : batch.hashes)
        if (auto it = state.active.materials.find(hash); it != state.active.materials.end())
          for (auto &path : state.submissions[it->second.first]->materialTextures(it->second.second["path"]))
            if (seen.insert(path).second)
              batch.paths.push_back(path);
      state.materialBatch = std::async(std::launch::async, [batch = std::move(batch)]() mutable {
        batch.textures = decodeTextures(batch.paths);
        return batch;
      });
    }
  } // namespace
  void startMods(bool disabled) {
    state.enabled = !disabled;
    if (disabled)
      return;
    const char *directory = std::getenv("OPENREMIX_MODS_DIRECTORY");
    state.directory = directory ? directory : (std::filesystem::current_path() / "rtx-remix/mods").string();
    // Host units must be supplied for mods. Defaults match capture conventions.
    state.options.meters = .01;
    state.options.axis = 1;
    state.stamps = stamps();
    launch();
  }
  void stopMods(NativeResources &resources) {
    resources.clearModMaterials();
    for (auto light : state.overriddenLights)
      resources.destroyLight(light);
    state.submissions.clear();
    state = State{};
  }
  remixapi_ErrorCode configureMods(const char *directory, double meters, uint32_t axis, bool watch) {
    if (!directory || !std::isfinite(meters) || meters <= 0 || axis < 1 || axis > 2)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    if (state.pending.valid())
      state.pending.wait();
    state.pending = {};
    state.directory = directory;
    state.options.meters = meters;
    state.options.axis = axis;
    state.watch = watch;
    state.enabled = true;
    try {
      state.stamps = stamps();
      launch();
      return REMIXAPI_ERROR_CODE_SUCCESS;
    } catch (...) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
  }
  void modBoundary(NativeResources &resources) {
    if (!state.enabled)
      return;
    for (auto &submission : state.submissions)
      submission->retireAttachments();
    if (state.pending.valid() &&
        state.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      try {
        auto next = state.pending.get();
        if (next.error && next.loadError)
          throw LoadError(*next.error, std::move(next.dependencies));
        if (next.error)
          throw std::runtime_error(*next.error);
        std::vector<std::unique_ptr<Submission>> submissions;
        state.failedDependencies = next.dependencies;
        resources.invalidateTexturePaths();
        const auto generation = state.generation + 1;
        for (size_t i = 0; i < next.snapshots.size(); ++i)
          submissions.push_back(std::make_unique<Submission>(
              resources, next.snapshots[i], 0x4d4f440000000000ull + (generation << 20) + i, false));
        // Material replacements are created as draws first use them.
        std::unordered_set<uint64_t> candidates;
        for (const auto &[hash, entry] : next.materials)
          candidates.insert(hash);
        resources.setModMaterialCandidates(std::move(candidates));
        state.materialQueue.clear();
        for (auto light : state.overriddenLights)
          resources.destroyLight(light);
        state.overriddenLights.clear();
        state.submissions = std::move(submissions);
        state.active = std::move(next);
        state.generation = generation;
        state.diagnostic.clear();
        state.failedDependencies.clear();
        refreshDependencies();
      } catch (const LoadError &e) {
        state.diagnostic = e.what();
        state.failedDependencies = e.dependencies;
        refreshDependencies();
      } catch (const std::exception &e) {
        state.diagnostic = e.what();
      } catch (...) {
        state.diagnostic = "Mod resources could not be created; retaining previous replacements";
      }
    }
    serviceMaterials(resources);
    auto now = Clock::now();
    if (!state.watch || state.pending.valid() || now < state.next)
      return;
    if (!state.scan.valid()) {
      state.scan = std::async(std::launch::async, [directory = state.directory, deps = watchedDependencies(),
                                                   epoch = state.scanEpoch] {
        Scan scan{.epoch = epoch};
        try {
          scan.stamps = scanStamps(directory, std::move(deps));
        } catch (const std::exception &e) {
          scan.error = e.what();
        }
        return scan;
      });
      return;
    }
    if (state.scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return;
    auto scan = state.scan.get();
    state.next = now + std::chrono::milliseconds(100);
    if (scan.error) {
      state.diagnostic = *scan.error;
      return;
    }
    if (scan.epoch == state.scanEpoch && scan.stamps != state.stamps) {
      state.stamps = std::move(scan.stamps);
      state.changed = now;
    }
    if (state.changed != Clock::time_point{} && now - state.changed >= std::chrono::milliseconds(250)) {
      state.changed = {};
      launch();
    }
  }
  bool modInstance(NativeResources &resources, const remixapi_InstanceInfo &info, uint64_t originalHash) {
    auto it = state.active.meshes.find(originalHash ? originalHash
                                                    : uint64_t(reinterpret_cast<uintptr_t>(info.mesh)));
    if (it == state.active.meshes.end())
      return false;
    auto &[index, entry] = it->second;
    state.submissions[index]->draw(false, 1, &info, entry["path"]);
    if (entry.value("preserve", false) && entry.contains("material")) {
      resources.drawInstance(info, state.submissions[index]->materialHandle(entry["material"]));
      return true;
    }
    return !entry.value("preserve", false);
  }
  bool modLight(NativeResources &resources, remixapi_LightHandle handle, uint64_t originalHash) {
    auto original = uint64_t(reinterpret_cast<uintptr_t>(handle));
    auto it = state.active.lights.find(originalHash ? originalHash : original);
    if (it == state.active.lights.end())
      return false;
    auto &[index, entry] = it->second;
    if (entry.value("hidden", false))
      return true;
    const auto &o = entry["overrides"];
    const std::map<std::string, remixapi_StructType> types{
        {"SphereLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT},
        {"RectLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT},
        {"DiskLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT},
        {"CylinderLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT},
        {"DistantLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT},
        {"DomeLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT}};
    remixapi_LightInfoUSDEXT usd{};
    if (auto type = types.find(entry.value("type", std::string())); type != types.end())
      usd.lightType = type->second;
    std::map<std::string, float> values;
    auto number = [&](const char *key, bool degrees = false) -> const float * {
      if (!o.contains(key))
        return nullptr;
      auto &value = values[key];
      value = o[key].get<float>() * (degrees ? 3.14159265359f / 180 : 1);
      return &value;
    };
    usd.pIntensity = number("intensity");
    usd.pExposure = number("exposure");
    usd.pRadius = number("radius");
    usd.pWidth = number("width");
    usd.pHeight = number("height");
    usd.pLength = number("length");
    usd.pAngleRadians = number("angle", true);
    usd.pConeAngleRadians = number("shaping:cone:angle", true);
    usd.pConeSoftness = number("shaping:cone:softness");
    usd.pFocus = number("shaping:focus");
    usd.pColorTemp = number("colorTemperature");
    remixapi_Float3D color{};
    if (o.contains("color")) {
      color = {o["color"][0], o["color"][1], o["color"][2]};
      usd.pColor = &color;
    }
    remixapi_Bool temperature = false;
    if (o.contains("enableColorTemperature")) {
      temperature = o["enableColorTemperature"].get<bool>();
      usd.pEnableColorTemp = &temperature;
    }
    if (o.contains("transform"))
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
          usd.transform.matrix[r][c] = o["transform"][c * 4 + r];
    auto texture = std::filesystem::path(o.value("texture", std::string()));
    const uint64_t id = original ^ 0x4d4f444c49474854ull;
    auto replacement = resources.overrideLight(handle, id ? id : 1, usd, o.contains("transform"),
                                               o.contains("texture") ? texture.c_str() : nullptr);
    state.overriddenLights.insert(replacement);
    resources.drawLight(replacement);
    return true;
  }
  const char *modDiagnostic() { return state.diagnostic.c_str(); }
} // namespace remix::usd
