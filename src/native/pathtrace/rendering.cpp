#include "pathtrace/gpu.h"
#include "pathtrace/sharc_policy.h"
#include "scene/emission.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>

namespace remix {
  using namespace scene;
  using namespace trace::detail;

  void PathTracer::Gpu::prepare(VkCommandBuffer command, RasterRenderer &resources,
                                const TraceOptions &requestedOptions, scene::TraceFrame &prepared) {
    const auto options = effectiveOptions(requestedOptions);
    debugView = options.debugView;
    const bool falseColor = debugView != 0 && debugView != 6 && debugView != 7;
    prism = options.prism && !falseColor;
    gamma22 = options.gamma22 && !falseColor;
    if (timestamps) {
      vkCmdResetQueryPool(command, timestamps, 0, stageProfile ? 10 : 4);
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 0);
    }
    complete(command);
    configureProfile(options);
    if (options.localDirectClamp != previousDirectClamp || options.indirectClamp != previousIndirectClamp ||
        options.localFireflyFilter != previousFireflyFilter) {
      history.clear();
      stats.accumulationCount = 0;
      previousDirectClamp = options.localDirectClamp;
      previousIndirectClamp = options.indirectClamp;
      previousFireflyFilter = options.localFireflyFilter;
    }
    rendered = false;
    pendingTime = std::chrono::steady_clock::now();
    rouletteActive = options.russianRoulette && options.rouletteMinBounces < options.bounces && !rouletteFailed;
    const uint64_t integrator =
        options.samples | uint64_t(options.bounces) << 8 | uint64_t(options.accumulate) << 16 |
        uint64_t(bool(denoiser) && postReblur) << 17 | uint64_t(bool(denoiser) && postTaa) << 18 |
        uint64_t(options.rasterPrimary) << 19 | uint64_t(bool(sharc.stats.active)) << 20 |
        uint64_t(options.sharcDebug) << 21 | uint64_t(options.pbrLocalReuse) << 22 |
        uint64_t(options.russianRoulette) << 23 | uint64_t(options.rouletteMinBounces) << 24 |
        uint64_t(rouletteFailed) << 27;
    history.begin(resources.temporalEpoch(), integrator, extent.width, extent.height);
    pendingVertices.clear();
    std::vector<trace::InstanceInput> inputs[2];
    std::vector<TraceSurface> surfaces;
    const auto emissionMode = options.freezeEmissiveRadiance ? 2u : options.stabilizeEmissiveLighting ? 1u : 0u;
    primaryDraws.clear();
    std::vector<VkDescriptorImageInfo> textures;
    std::unordered_map<const Material *, uint32_t> textureIndices;
    std::unordered_map<const Material *, uint32_t> emissionTextures;
    std::map<std::tuple<const scene::Texture *, uint8_t, uint8_t, uint8_t>, uint32_t> pbrTextures;
    std::vector<uint8_t> scene, staticGeometry, dynamicGeometry;
    auto appendStatic = [&](const void *data, size_t size) {
      const auto *bytes = static_cast<const uint8_t *>(data);
      staticGeometry.insert(staticGeometry.end(), bytes, bytes + size);
    };
    auto append = [&](const void *data, size_t size) {
      const auto *bytes = static_cast<const uint8_t *>(data);
      scene.insert(scene.end(), bytes, bytes + size);
    };
    for (const auto &draw : resources.draws())
      if (draw.camera == REMIXAPI_CAMERA_TYPE_SKY) {
        append(draw.model.data(), sizeof(draw.model));
        const auto material = scene::TemporalHistory::materialSignature(draw);
        append(&material, sizeof(material));
        append(draw.surface->vertices.data(), draw.surface->vertices.size() * sizeof(scene::Vertex));
        append(draw.surface->indices.data(), draw.surface->indices.size() * sizeof(uint32_t));
        const auto &camera = resources.camera(REMIXAPI_CAMERA_TYPE_SKY);
        append(camera.data(), sizeof(camera));
        if (draw.material->fps) {
          auto animationFrame = uint64_t(prepared.seconds * draw.material->fps);
          append(&animationFrame, sizeof(animationFrame));
        }
      }
    uint64_t reuseSignature = 1469598103934665603ull;
    bool hasCacheReceivers = false, hasBlendedGeometry = false;
    for (const auto &instance : prepared.instances) {
      const auto &draw = instance[0].draw;
      std::vector<std::shared_ptr<const scene::OpacityPartition>> partitions(draw.mesh->surfaces.size());
      for (const auto &surface : instance) {
        partitions[surface.draw.surfaceIndex] = surface.opacity;
        hasBlendedGeometry |= surface.draw.blend.enabled;
        const auto &material = *surface.draw.material;
        hasCacheReceivers |= surface.draw.classification == 0 && !surface.draw.blend.enabled &&
            (!material.pbr || material.roughnessTexture || material.roughness >= REMIX_SHARC_PBR_MIN_ROUGHNESS);
      }
      auto &view = mesh(command, draw, partitions);
      const auto &geometry = *view.geometry;
      auto model = draw.model;
      if (draw.classification == 1) {
        auto appendDynamic = [&](const void *data, size_t size) {
          const auto *bytes = static_cast<const uint8_t *>(data);
          dynamicGeometry.insert(dynamicGeometry.end(), bytes, bytes + size);
        };
        appendDynamic(&draw.instanceId, sizeof(draw.instanceId));
        appendDynamic(&draw.instanceIndex, sizeof(draw.instanceIndex));
        appendDynamic(model.data(), sizeof(model));
        appendDynamic(&geometry.revision, sizeof(geometry.revision));
        appendDynamic(&geometry.fingerprint, sizeof(geometry.fingerprint));
      }
      if (draw.classification == 0) {
        appendStatic(&draw.instanceId, sizeof(draw.instanceId));
        appendStatic(&draw.instanceIndex, sizeof(draw.instanceIndex));
        appendStatic(model.data(), sizeof(model));
        const auto identity = reinterpret_cast<uintptr_t>(draw.mesh.get());
        appendStatic(&identity, sizeof(identity));
        appendStatic(&geometry.revision, sizeof(geometry.revision));
        appendStatic(&geometry.fingerprint, sizeof(geometry.fingerprint));
      }
      float scale = 0;
      for (int c = 0; c < 3; ++c)
        scale = std::max(scale, std::hypot(model[c * 4], model[c * 4 + 1], model[c * 4 + 2]));
      if (surfaces.size() > 0xffffffu) throw VK_ERROR_TOO_MANY_OBJECTS;
      inputs[draw.classification].push_back({draw.instanceId, geometry.accel->address, draw.instanceIndex,
                                             uint32_t(surfaces.size()), model, geometry.diagonal * scale,
                                             geometry.revision});
      append(model.data(), sizeof(model));
      append(&view.fingerprint, sizeof(view.fingerprint));
      append(&draw.instanceId, sizeof(draw.instanceId));
      append(&draw.instanceIndex, sizeof(draw.instanceIndex));
      append(&draw.classification, sizeof(draw.classification));
      size_t groupIndex = 0;
      for (const auto &preparedSurface : instance) {
        const auto &surface = preparedSurface.draw;
        const auto &material = *surface.material;
        auto [entry, inserted] = textureIndices.emplace(surface.material.get(), uint32_t(textures.size()));
        if (inserted) {
          if (textures.size() >= PathTracer::textureLimit)
            throw VK_ERROR_TOO_MANY_OBJECTS;
          textures.push_back(resources.sampledMaterial(surface.material, command));
          const auto fingerprint = material.texture->fingerprint();
          append(&fingerprint, sizeof(fingerprint));
          const uint8_t sampler[] = {material.filter, material.wrapU, material.wrapV};
          append(sampler, sizeof(sampler));
        }
        TraceSurface value;
        value.pbr[0] = material.roughness;
        value.pbr[1] = material.metallic;
        value.pbr[2] = material.pbr ? 1.f : 0.f;
        value.pbrTextures[3] = material.normalTexture && material.normalTexture->pixels.twoChannel;
        const std::shared_ptr<scene::Texture> maps[] = {
            material.roughnessTexture, material.metallicTexture, material.normalTexture};
        for (unsigned i = 0; i < 3; ++i) {
          if (!maps[i]) continue;
          auto [map, added] = pbrTextures.emplace(
              std::make_tuple(maps[i].get(), material.filter, material.wrapU, material.wrapV),
              uint32_t(textures.size()));
          if (added) {
            if (textures.size() >= PathTracer::textureLimit) throw VK_ERROR_TOO_MANY_OBJECTS;
            textures.push_back(resources.sampledTexture(surface.material, maps[i], command));
            const auto fingerprint = maps[i]->fingerprint();
            append(&fingerprint, sizeof(fingerprint));
          }
          value.pbrTextures[i] = map->second + 1;
        }
        std::copy_n(material.emission, 3, value.emission);
        if (material.emissionTexture && (material.emission[0] > 0 || material.emission[1] > 0 || material.emission[2] > 0)) {
          auto [emitter, newTexture] = emissionTextures.emplace(surface.material.get(), uint32_t(textures.size()));
          if (newTexture) {
            if (textures.size() >= PathTracer::textureLimit) throw VK_ERROR_TOO_MANY_OBJECTS;
            textures.push_back(resources.sampledEmission(surface.material, command));
            auto fingerprint = material.emissionTexture->fingerprint();
            append(&fingerprint, sizeof(fingerprint));
          }
          value.textureState[2] = emitter->second + 1;
        }
        value.vertices = view.vertices.at(surface.surfaceIndex)->address;
        const uint64_t debugKey[] = {surface.instanceId, surface.instanceIndex, surface.surfaceIndex};
        value.debugIdentity = hashBytes(debugKey, sizeof(debugKey));
        value.indices = geometry.input->address + geometry.indexOffsets.at(surface.surfaceIndex);
        auto temporalDraw = surface;
        if (!prepared.persistentIds.count(surface.instanceId))
          temporalDraw.instanceId = 0;
        const scene::TemporalHistory::Key temporalKey{temporalDraw.instanceId, temporalDraw.instanceIndex,
                                                      temporalDraw.surfaceIndex};
        // A texture upload (including an unrelated animated tile in a shared
        // terrain atlas) does not change vertex correspondence. Keep motion and
        // let per-pixel depth/normal rejection, lighting confidence and TAA color
        // clipping respond to the shading that actually changed on screen.
        if (const auto *previous = history.previousGeometry(temporalDraw)) {
          const auto old = previousVertices.find(temporalKey);
          if (old != previousVertices.end()) {
            value.previousVertices = old->second->address;
            value.previousModel = previous->draw.model;
          }
        }
        if (temporalDraw.instanceId)
          pendingVertices[temporalKey] = view.vertices.at(surface.surfaceIndex);
        std::copy_n(material.tint, 4, value.tint);
        history.stage(temporalDraw, preparedSurface.animationFrame);
        if (options.pbrLocalReuse) {
          const uint64_t values[] = {view.fingerprint, value.debugIdentity,
              scene::TemporalHistory::materialSignature(surface), preparedSurface.animationFrame};
          reuseSignature = hashBytes(values, sizeof(values), reuseSignature);
          reuseSignature = hashBytes(model.data(), sizeof(model), reuseSignature);
        }
        std::copy(preparedSurface.uv.begin(), preparedSurface.uv.end(), value.uv);
        const auto &b = surface.blend;
        value.factor[0] = linear(float((b.factor >> 16) & 255) / 255);
        value.factor[1] = linear(float((b.factor >> 8) & 255) / 255);
        value.factor[2] = linear(float(b.factor & 255) / 255);
        value.factor[3] = float(b.factor >> 24) / 255;
        value.state[0] = b.colorOp | b.color1 << 2 | b.color2 << 4 | b.alphaOpTexture << 6 | b.alpha1 << 8 |
                         b.alpha2 << 10;
        value.state[1] = surface.ignoreAlpha ? 7 : surface.alphaOverride ? surface.compare : material.compare;
        const float reference = surface.alphaOverride ? surface.reference : material.reference;
        std::memcpy(&value.state[2], &reference, 4);
        value.state[3] = uint32_t(surface.ignoreAlpha) | uint32_t(b.enabled) << 1 |
                         uint32_t(surface.doubleSided) << 2 | uint32_t(material.transmission) << 3 |
                         uint32_t(draw.classification == 0) << 4;
        if (draw.classification == 0) {
          appendStatic(value.tint, sizeof(value.tint));
          appendStatic(value.factor, sizeof(value.factor));
          appendStatic(value.state, sizeof(value.state));
        }
        value.textureState[0] = entry->second;
        primaryDraws.push_back(
            {model, uint32_t(surfaces.size()), uint32_t(surface.surface->indices.size()), b.enabled});
        do {
          const auto &group = geometry.groups.at(groupIndex++);
          value.triangleMap = group.triangleMap ? geometry.input->address + group.triangleMap : 0;
          value.traceIndices = geometry.input->address + group.indices;
          surfaces.push_back(value);
        } while (groupIndex < geometry.groups.size() && geometry.groups[groupIndex].surface == surface.surfaceIndex);
      }
    }
    // Retire absent deformation instances after their last-use fence. Rigid
    // geometry remains cached for the lifetime of its shared CPU mesh.
    for (auto it = meshes.begin(); it != meshes.end();) {
      if (it->second.owner.expired() || (std::get<1>(it->first) && it->second.lastUse != sequence)) {
        retiredGeometry.push_back(std::move(it->second.geometry));
        for (auto &vertices : it->second.vertices)
          retiredBuffers.push_back(std::move(vertices));
        it = meshes.erase(it);
      } else
        ++it;
    }
    for (auto it = updateViews.begin(); it != updateViews.end();) {
      auto view = meshes.find(it->second);
      if (view == meshes.end() || view->second.updateStream.get() != it->first.first) it = updateViews.erase(it);
      else ++it;
    }
    // Geometry uploads feed both AS construction and BDA reads in the compute shader.
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            buildStage | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            asRead | VK_ACCESS_SHADER_READ_BIT);
    buildBottom(command);
    // Compaction is also an AS write, including on a frame with no new BLAS work.
    barrier(command, buildStage, asWrite, buildStage, asRead);
    buildTop(command, inputs, options.forceRebuild);
    barrier(command, buildStage, asWrite, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, asRead);
    if (timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, timestamps, 1);
    if (surfaces.empty())
      surfaces.emplace_back();
    for (const auto &surface : surfaces)
      append(&surface, offsetof(TraceSurface, previousVertices));
    const auto tableBytes = surfaces.size() * sizeof(surfaces[0]);
    // A diagnostic placement switch replaces only this completed slot's table.
    if (table && table->host == deviceLocalScene) table.reset();
    if (deviceLocalScene) {
      hostBuffer(tableUpload, surfaces.data(), tableBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
      if (!table || table->size < tableBytes)
        table = buffer(tableBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      const VkBufferCopy copy{.size = tableBytes};
      vkCmdCopyBuffer(command, tableUpload->handle, table->handle, 1, &copy);
    } else {
      hostBuffer(table, surfaces.data(), tableBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
    // Emission constants, UVs and texture fingerprints already participate in
    // the scene signature above. Do not duplicate every triangle's reduction.
    auto &lighting = prepared.lighting;
    const bool hasEmissiveLights = lighting.hasEmissive;
    emissionHistory = std::move(lighting.history);
    lightBvh = std::move(lighting.combined);
    localAngularActive = hasEmissiveLights && options.emissiveSolidAngleSampling;
    splitLightingActive = hasEmissiveLights && options.separateLocalLighting != 0 && (!denoiser || postSplit || rrActive());
    auto uploadLights = [&](scene::LightBvh &bvh, uint64_t &uploadedGeneration,
                            std::unique_ptr<Buffer> &nodes, std::unique_ptr<Buffer> &emitters) {
      if ((!nodes || nodes->host == deviceLocalScene || uploadedGeneration != bvh.generation) && !bvh.nodes().empty()) {
        uploadedGeneration = bvh.generation;
        if (deviceLocalScene) {
          nodes = upload(command, bvh.nodes().data(), bvh.nodes().size() * sizeof(scene::LightNode),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
          emitters = upload(command, bvh.emitters().data(), bvh.emitters().size() * sizeof(scene::LightEmitter),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        } else {
          // This slot's fence has completed; replace GPU-only allocations before
          // hostBuffer tries to map them when a diagnostic switches back.
          if (nodes && !nodes->host) nodes.reset();
          if (emitters && !emitters->host) emitters.reset();
          hostBuffer(nodes, bvh.nodes().data(), bvh.nodes().size() * sizeof(scene::LightNode),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
          hostBuffer(emitters, bvh.emitters().data(), bvh.emitters().size() * sizeof(scene::LightEmitter),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        }
      }
    };
    const bool splitFamilies = splitLightingActive && options.separateLocalLighting != 3;
    if (splitFamilies) {
      torchBvh = std::move(lighting.torch);
      emissiveBvh = std::move(lighting.emissive);
      uploadLights(torchBvh, torchBufferGeneration, torchNodes, torchEmitters);
      uploadLights(emissiveBvh, emissiveBufferGeneration, emissiveNodes, emissiveEmitters);
    }
    uploadLights(lightBvh, lightBufferGeneration, lightNodes, lightEmitters);
    if (deviceLocalScene)
      barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    Frame frame = {};
    frame.localConfig[0] = options.localLights && options.localIntensity > 0 ? lightBvh.active : 0;
    localIlluminationActive = frame.localConfig[0] != 0;
    localFirstHitActive = localIlluminationActive && !hasBlendedGeometry;
    frame.splitLocal[0] = splitLightingActive ? options.separateLocalLighting : 0;
    frame.splitLocal[1] = localIlluminationActive && splitFamilies ? torchBvh.active : 0;
    frame.splitLocal[2] = localIlluminationActive && splitFamilies ? emissiveBvh.active : 0;
    frame.splitLocal[3] = debugView;
    frame.localConfig[1] = options.localSamples;
    frame.localConfig[2] = uint32_t(lightBvh.lightingGeneration);
    frame.localConfig[3] = uint32_t(lightBvh.lightingGeneration >> 32) |
                           (options.adaptiveLocalSampling ? 0x80000000u : 0u);
    frame.localScale[0] = options.localIntensity;
    frame.localScale[1] = options.localRange;
    // Private, previously unused shader word: bounded candidate count. Keep
    // the established one-/many-sphere and <=4-emitter estimators unchanged.
    frame.localScale[2] = hasEmissiveLights && lightBvh.emitters().size() > 4
                              ? float(options.emissiveCandidates) : 1.f;
    localResamplingActive = localIlluminationActive && frame.localScale[2] > 1;
    frame.localScale[3] = options.localRange > 0 ? 1.f / (options.localRange * options.localRange) : 0;
    frame.camera = resources.camera();
    primaryReverseDepth = resources.reverseDepth();
    frame.inverseCamera = scene::inverse(frame.camera);
    const auto eye = scene::inverse(resources.view());
    std::copy_n(eye.data() + 12, 3, frame.eye);
    auto light = resources.light();
    if (options.overrideLight) {
      float norm =
          std::hypot(options.lightDirection[0], options.lightDirection[1], options.lightDirection[2]);
      if (norm > 1e-6f)
        for (int i = 0; i < 3; ++i)
          light.direction[i] = options.lightDirection[i] / norm;
      for (int i = 0; i < 3; ++i)
        light.radiance[i] = options.lightColor[i] * options.lightIntensity;
    }
    const float diameter = options.sunAngularDiameter >= 0 ? options.sunAngularDiameter : light.angularDiameterDegrees;
    frame.sun[0] = std::tan(diameter * (3.14159265359f / 360.f));
    frame.sun[1] = denoiser && postSigma;
    configureBlueNoise(command, frame, options);
    frame.sun[2] = frame.sun[1] != 0 ? float(options.shadowDebug) : 0;
    const char *localDebug = std::getenv("REMIX_EMISSIVE_HIT_DISTANCE");
    frame.sun[3] = localDebug ? (std::strcmp(localDebug, "budget") == 0 ? 2.f : 1.f) : 0.f;
    std::copy_n(light.direction, 3, frame.direction);
    std::copy_n(light.radiance, 3, frame.radiance);
    if (options.sky)
      std::copy_n(resources.skyRadiance().data(), 3, frame.skyRadiance);
    frame.config[0] = options.samples;
    frame.config[1] = options.bounces;
    frame.target[0] = extent.width;
    frame.target[1] = extent.height;
    frame.target[2] = uint32_t(!inputs[0].empty()) | uint32_t(!inputs[1].empty()) << 1;
    frame.target[3] = options.rasterPrimary;
    // Reserved private word; included in scene-change detection. Raw/reference
    // and multi-sample rendering retain one estimator at every surface pixel.
    frame.jitter[2] = options.waterIndirectReuse && options.indirectClamp == 0 && denoiser && (postTaa || upscaler) && !options.accumulate &&
                      options.samples == 1 && options.bounces > 0;
    frame.firefly[0] = options.localDirectClamp;
    frame.firefly[1] = options.indirectClamp;
    frame.pathConfig[0] = rouletteActive;
    frame.pathConfig[1] = options.rouletteMinBounces;
    append(&frame, sizeof(frame));
    append(&options.localFireflyFilter, sizeof(options.localFireflyFilter));
    append(&lightBvh.generation, sizeof(lightBvh.generation));
    append(&integrator, sizeof(integrator));
    append(&options.validation, sizeof(options.validation));
    append(&options.localGuiding, sizeof(options.localGuiding));
    append(&options.emissiveSolidAngleSampling, sizeof(options.emissiveSolidAngleSampling));
    append(&emissionMode, sizeof(emissionMode));
    append(&options.fsrSharpness, sizeof(options.fsrSharpness));
    const uint64_t cacheSettings[] = {options.sharcReset, options.sharcCapacityLog2, options.sharcSceneScale,
        options.sharcDownscale, options.sharcAccumulationFrames, options.sharcStaleFrames};
    append(cacheSettings, sizeof(cacheSettings));
    const auto temporalEpoch = resources.temporalEpoch();
    append(&temporalEpoch, sizeof(temporalEpoch));
    configureSharc(frame, options, temporalEpoch, std::move(staticGeometry), std::move(dynamicGeometry), hasCacheReceivers);
    // An unchanged world can reuse its cached output once both histories are warm.
    if (scene == lastScene && !profileEveryFrame && !std::getenv("REMIX_PATH_LINEAR_CAPTURE") && !options.accumulate && !debugView && denoiser && !denoiser->statistics().redrawRequested &&
        !history.reset() && !(upscaler && upscaler->statistics().frames < 128) && !(sharc.stats.active && sharc.hasReceivers && sharc.warmFrames < 2 * options.sharcAccumulationFrames))
      return;
    const bool sceneChanged = scene != lastScene;
    if (sceneChanged) {
      ++stats.sceneGeneration;
      stats.accumulationCount = 0;
      lastScene = std::move(scene);
    }
    if (options.accumulate != bool(stats.accumulationEnabled))
      stats.accumulationCount = 0;
    if (!options.accumulate || stats.accumulationCount >= 1048576)
      stats.accumulationCount = 0;
    const bool localBlueNoise = (uint32_t(frame.jitter[3]) & 2u) != 0;
    const bool localSettingsChanged = frame.localScale[0] != previousLocalIntensity || frame.localScale[1] != previousLocalRange ||
                                      localBlueNoise != previousLocalBlueNoise;
    if (lightBvh.lightingGeneration != previousLocalGeneration || localSettingsChanged ||
        frame.localConfig[0] != previousLocalActive ||
        (frame.localConfig[3] & 0x80000000u) != previousLocalAdaptive)
      localResponsiveFrames = 8;
    else if (localResponsiveFrames)
      --localResponsiveFrames;
    previousLocalGeneration = lightBvh.lightingGeneration;
    previousLocalIntensity = frame.localScale[0];
    previousLocalRange = frame.localScale[1];
    previousLocalBlueNoise = localBlueNoise;
    previousLocalActive = frame.localConfig[0];
    previousLocalAdaptive = frame.localConfig[3] & 0x80000000u;
    temporalFrame = {};
    temporalFrame.shadow[2] = std::any_of(resources.draws().begin(), resources.draws().end(),
        [](const auto &draw) { return draw.traced() && draw.material->pbr; });
    temporalFrame.shadow[3] = std::any_of(resources.draws().begin(), resources.draws().end(),
        [](const auto &draw) { return draw.traced() && !draw.material->pbr; });
    temporalFrame.local[0] = denoiser && postLocalDenoise;
    temporalFrame.local[1] = frame.localConfig[0];
    temporalFrame.local[2] = localResponsiveFrames;
    if (std::getenv("REMIX_EMISSIVE_DIAGNOSTICS"))
      std::fprintf(stderr, "EMISSION mode=%u sources=%zu raw_delta=%.6f filtered_delta=%.6f response=%u generation=%llu visibility=%llu\n",
          emissionMode, emissionHistory.size(), emissionHistory.rawChange, emissionHistory.filteredChange,
          localResponsiveFrames, static_cast<unsigned long long>(lightBvh.lightingGeneration),
          static_cast<unsigned long long>(lightBvh.visibilityGeneration));
    temporalFrame.local[3] = frame.splitLocal[0];
    for (unsigned i = 0; i < 2; ++i) {
      const auto &family = i ? emissiveBvh : torchBvh;
      if (previousFamilyGeneration[i] != family.lightingGeneration || localSettingsChanged || history.reset())
        familyResponsive[i] = 8;
      else if (familyResponsive[i])
        --familyResponsive[i];
      previousFamilyGeneration[i] = family.lightingGeneration;
      temporalFrame.families[i] = frame.splitLocal[i + 1];
      temporalFrame.families[i + 2] = familyResponsive[i];
    }
    if (!splitFamilies) {
      temporalFrame.families[0] = frame.localConfig[0];
      temporalFrame.families[1] = 0;
      temporalFrame.families[2] = localResponsiveFrames;
      temporalFrame.families[3] = 0;
    }
    temporalFrame.view = resources.view();
    temporalFrame.inverseCamera = frame.inverseCamera;
    pendingCamera = frame.camera;
    pendingProjection = scene::multiply(frame.camera, scene::inverse(resources.view()));
    temporalFrame.previousView = history.reset() ? resources.view() : previousView;
    temporalFrame.previousCamera = history.reset() ? frame.camera : previousCamera;
    temporalFrame.previousInverseCamera = scene::inverse(temporalFrame.previousCamera);
    auto halton = [](uint32_t n, uint32_t base) {
      float value = 0, scale = 1.f / base;
      while (n) {
        value += float(n % base) * scale;
        n /= base;
        scale /= base;
      }
      return value - .5f;
    };
    // Camera jitter needs a resolve. NRD filters indirect lighting, not primary
    // coverage; raw and NRD-only rendering must use stable pixel centers.
    if (options.accumulate || (denoiser && postTaa)) {
      temporalFrame.jitter[0] = halton(uint32_t(sequence % 16) + 1, 2);
      temporalFrame.jitter[1] = halton(uint32_t(sequence % 16) + 1, 3);
    }
    if (upscaler) upscale::jitter(temporalFrame.jitter, sequence, extent, displayExtent);
    std::copy_n(temporalFrame.jitter, 2, frame.jitter);
    temporalFrame.jitter[2] = history.reset() ? temporalFrame.jitter[0] : previousJitter[0];
    temporalFrame.jitter[3] = history.reset() ? temporalFrame.jitter[1] : previousJitter[1];
    temporalFrame.hitDistance[3] = rrActive() ? 1.f : 0.f;
    temporalFrame.flags[0] = history.reset() || frame.sun[2] != previousSun[7];
    temporalFrame.flags[1] = postReblur;
    temporalFrame.flags[2] = postTaa;
    temporalFrame.flags[3] = denoiser ? uint32_t(denoiser->statistics().renderedFrames % 2) : 0;
    temporalFrame.target[0] = extent.width;
    temporalFrame.target[1] = extent.height;
    temporalFrame.target[2] = previousSeed;
    temporalFrame.target[3] = options.samples;
    temporalFrame.timing[0] =
        previousTime.time_since_epoch().count()
            ? std::clamp(float(std::chrono::duration<double, std::milli>(pendingTime - previousTime).count()),
                         1.f, 1000.f)
            : 16.6667f;
    temporalFrame.timing[1] = options.validation;
    temporalFrame.timing[2] = sceneChanged;
    temporalFrame.timing[3] = float(debugView);
    for (int i = 0; i < 3; ++i) {
      temporalFrame.sun[i] = -frame.direction[i];
      pendingSun[i] = frame.direction[i];
      pendingSun[i + 3] = frame.radiance[i];
    }
    pendingSun[6] = diameter;
    pendingSun[7] = frame.sun[2];
    pendingSun[8] = (uint32_t(frame.jitter[3]) & 1u) != 0;
    temporalFrame.sun[3] = frame.sun[1];
    // Abrupt sun changes invalidate only SIGMA. Ordinary time/weather changes
    // keep both denoisers' histories and the existing lighting confidence path.
    float directionDelta = 0, radianceDelta = 0, previousRadiance = 0;
    for (int i = 0; i < 3; ++i) {
      directionDelta += (pendingSun[i] - previousSun[i]) * (pendingSun[i] - previousSun[i]);
      radianceDelta += std::abs(pendingSun[i + 3] - previousSun[i + 3]);
      previousRadiance += previousSun[i + 3];
    }
    temporalFrame.shadow[0] = directionDelta > .01f || radianceDelta > std::max(.1f, previousRadiance * .5f) ||
                              diameter != previousSun[6] || pendingSun[8] != previousSun[8];
    temporalFrame.shadow[1] = uint32_t(frame.sun[2]);
    frame.lightNodes = lightNodes ? lightNodes->address : 0;
    frame.lightEmitters = lightEmitters ? lightEmitters->address : 0;
    frame.torchNodes = torchNodes ? torchNodes->address : 0;
    frame.torchEmitters = torchEmitters ? torchEmitters->address : 0;
    frame.emissiveNodes = emissiveNodes ? emissiveNodes->address : 0;
    frame.emissiveEmitters = emissiveEmitters ? emissiveEmitters->address : 0;
    configureLightGuiding(frame, options);
    if (options.pbrLocalReuse) {
      const uint32_t settings[] = {options.localSamples, options.emissiveCandidates,
          uint32_t(options.adaptiveLocalSampling), uint32_t(options.localGuiding),
          options.separateLocalLighting, uint32_t(options.emissiveSolidAngleSampling), uint32_t(localBlueNoise)};
      reuseSignature = hashBytes(settings, sizeof(settings), reuseSignature);
    }
    configurePbrReuse(command, frame, options, reuseSignature,
        inputs[1].empty() && temporalFrame.local[2] == 0 && !history.reset());
    const bool querySharc = sharc.stats.active && (sharc.hasReceivers || options.sharcDebug);
    configureIndirect(frame, options, querySharc);
    beginRayProfile(command, frame);
    frame.config[2] = uint32_t(sequence++);
    // Opt-in diagnostic offset for independently seeded HDR references.
    if (const auto *seed = std::getenv("REMIX_PATH_SAMPLE_SEED"))
      frame.config[2] += uint32_t(std::strtoul(seed, nullptr, 10));
    frame.config[3] = uint32_t(stats.accumulationCount);
    hostBuffer(uniform, &frame, sizeof(frame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    stats.samplesPerPixel = options.samples;
    stats.indirectBounces = options.bounces;
    stats.accumulationEnabled = options.accumulate;
    ++stats.accumulationCount;
    stats.sampleSequence = sequence;
    stats.redrawRequested = options.accumulate || debugView ||
        (sharc.stats.active && sharc.hasReceivers && sharc.warmFrames < 2 * options.sharcAccumulationFrames);
    if (textures.empty())
      textures.push_back(resources.sampledMaterial(resources.defaultMaterial(), command));
    textures.resize(PathTracer::textureLimit, textures.front());
    VkDescriptorBufferInfo buffers[] = {{table->handle, 0, surfaces.size() * sizeof(surfaces[0])},
                                        {uniform->handle, 0, sizeof(frame)}};
    VkDescriptorImageInfo images[] = {{VK_NULL_HANDLE, world->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, transmission->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, sky->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, primary[0]->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, primary[1]->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, primary[2]->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, primary[3]->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, primary[4]->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, primary[5]->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, primary[6]->view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSetAccelerationStructureKHR structures[2] = {};
    VkWriteDescriptorSet writes[15] = {};
    for (unsigned i = 0; i < 15; ++i) {
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = set,
                   .dstBinding = i,
                   .descriptorCount = 1};
      if (i < 2) {
        structures[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                         .accelerationStructureCount = 1,
                         .pAccelerationStructures = &tlas[i].accel->handle};
        writes[i].pNext = &structures[i];
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
      } else if (i < 4) {
        writes[i].descriptorType =
            i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[i].pBufferInfo = &buffers[i - 2];
      } else {
        writes[i].descriptorType =
            i != 6 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = i >= 7 ? &images[i - 5] : i < 6 ? &images[i - 4] : textures.data();
        if (i == 6)
          writes[i].descriptorCount = PathTracer::textureLimit;
      }
    }
    vkUpdateDescriptorSets(device, 15, writes, 0, nullptr);
    if (!imagesInitialized) {
      VkImageMemoryBarrier barriers[2] = {};
      for (unsigned i = 0; i < 2; ++i)
        barriers[i] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                       .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                       .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                       .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                       .image = i ? transmission->handle : world->handle,
                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 2, barriers);
      imagesInitialized = true;
    } else
      barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    VkClearValue clear[2] = {};
    clear[0].color.float32[0] = .025f;
    clear[0].color.float32[1] = .035f;
    clear[0].color.float32[2] = .055f;
    clear[0].color.float32[3] = 1;
    clear[1].depthStencil.depth = 1;
    VkRenderPassBeginInfo skyBegin{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                   .renderPass = skyPass,
                                   .framebuffer = skyFramebuffer,
                                   .renderArea = {{0, 0}, extent},
                                   .clearValueCount = 2,
                                   .pClearValues = clear};
    vkCmdBeginRenderPass(command, &skyBegin, VK_SUBPASS_CONTENTS_INLINE);
    resources.record(command, true, false, temporalFrame.jitter, extent);
    vkCmdEndRenderPass(command);
    if (timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, timestamps, 2);
    beginLightGuiding(command);
    auto stageTime = [&](uint32_t query) {
      if (timestamps && stageProfile)
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, timestamps, query);
    };
    recordPrimary(command, options.rasterPrimary);
    stageTime(4);
    recordSharc(command, options);
    if (denoiser)
      denoiser->begin(command, temporalFrame, pendingProjection,
                      history.reset() ? pendingProjection : previousProjection, world->view, sky->view,
                      options.localFireflyFilter && (postLocalDenoise || options.bounces > 0));
    stageTime(5);
    // Idle caches have no eligible hit in either TLAS. Select the ordinary
    // shader so unused hash-grid code does not reserve tracing registers.
    // Keep the cache debug specialization available even in an empty scene.
    const auto opaqueSource = querySharc ? (denoiser ? sharc.temporal : sharc.query) : (denoiser ? temporalCompute : compute);
    VkPipeline opaquePipeline = VK_NULL_HANDLE;
    if (indirectPass.active) {
      opaquePipeline = selectLightingPipeline(opaqueSource, false, true);
      if (!opaquePipeline) {
        indirectPass.active = false;
        indirectPass.failed = true;
        if (indirectPass.results) retiredBuffers.push_back(std::move(indirectPass.results));
        std::fprintf(stderr, "Separate indirect consumer unavailable; using inline tracing\n");
      }
    }
    if (!opaquePipeline) opaquePipeline = selectLightingPipeline(opaqueSource);
    const auto opaqueKey = selectedLightingKey;
    VkPipeline indirectPipeline = VK_NULL_HANDLE;
    uint32_t indirectKey = UINT32_MAX;
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, denoiser ? temporalLayout : layout, 0, 1,
                            &set, 0, nullptr);
    if (denoiser) {
      auto inputs = denoiser->inputs();
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, temporalLayout, 1, 1, &inputs, 0,
                              nullptr);
    }
    if (indirectPass.active) {
      // Scratch is queue-ordered across frame slots. Publish last frame's reads
      // before overwriting, then publish this producer to the opaque consumer.
      barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
      indirectPipeline = selectLightingPipeline(indirectPass.pipeline[querySharc]);
      indirectKey = selectedLightingKey;
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, indirectPipeline);
      vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
      barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, opaquePipeline);
    vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
    stageTime(6);
    // Publish opaque lighting, RNG and sheet count to transparent composition.
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (splitLightingActive && denoiser && postSplit) {
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                        selectLightingPipeline(splitDirectCompute, options.separateLocalLighting == 3));
      vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
      barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    stageTime(7);
    const auto transparencyPipeline = selectLightingPipeline(
        querySharc ? (denoiser ? sharc.temporalTransparency : sharc.transparency)
                   : (denoiser ? temporalTransparencyCompute : transparencyCompute));
    const auto transparencyKey = selectedLightingKey;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, transparencyPipeline);
    vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
    if (timestamps) {
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timestamps, 3);
      timingPending = true;
    }
    if (denoiser) {
      denoiser->beginConfidence(command);
      if (postReblur) {
        barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, selectLightingPipeline(querySharc ? sharc.confidence : confidenceCompute));
        vkCmdDispatch(command, ((extent.width + 4) / 5 + 7) / 8, ((extent.height + 4) / 5 + 7) / 8, 1);
      }
      stageTime(8);
      denoiser->record(command);
    } else {
      stageTime(8);
    }
    stageTime(9);
    if (stateProfile) {
      const uint64_t values[] = {frame.config[2], frameIndex, uint64_t(querySharc), uint64_t(deviceLocalScene), uint64_t(indirectPass.active),
          uint64_t(opaquePipeline), opaqueKey, uint64_t(indirectPipeline), indirectKey,
          uint64_t(transparencyPipeline), transparencyKey,
          tlas[0].accel ? tlas[0].accel->address : 0, tlas[1].accel ? tlas[1].accel->address : 0,
          table->address, uint64_t(table->memory), table->size, uniform->address, uint64_t(uniform->memory),
          frame.lightNodes, frame.lightEmitters, frame.sharcHash, frame.sharcAccumulation, frame.sharcResolved,
          frame.indirectResults, stats.sceneGeneration, stats.staticTlasBuilds,
          hashBytes(surfaces.data(), surfaces.size() * sizeof(surfaces[0]))};
      traceState.clear();
      for (auto value : values) {
        traceState += std::to_string(value);
        traceState += ',';
      }
      constexpr char hex[] = "0123456789abcdef";
      for (size_t i = 0; i < sizeof(frame); ++i) {
        const auto value = reinterpret_cast<const uint8_t *>(&frame)[i];
        traceState += hex[value >> 4];
        traceState += hex[value & 15];
      }
    }
    if (upscaler) {
      upscaler->record(command, denoiser->image(denoise::Composed), denoiser->image(denoise::Depth),
                       denoiser->image(denoise::Motion), denoiser->image(denoise::Transmission),
                       pendingProjection, temporalFrame.jitter, temporalFrame.timing[0],
                       options.fsrSharpness, temporalFrame.flags[0],
                       {denoiser->image(denoise::RrDiffuse), denoiser->image(denoise::RrSpecular),
                        denoiser->image(denoise::RrNormal), denoiser->image(denoise::RrHitDistance)}, resources.view());
    }
    captureLinear(command);
    finishPbrReuse(command);
    finishRayProfile(command);
    finishLightGuiding(command);
    finishSharc(command);
    rendered = true;
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    stats.accelerationBytes = 0;
    for (const auto &arena : arenas)
      stats.accelerationBytes += arena->buffer->size;
    if (scratch)
      stats.accelerationBytes += scratch->size;
    if (topScratch)
      stats.accelerationBytes += topScratch->size;
    for (const auto &top : tlas)
      if (top.instances)
        stats.accelerationBytes += top.instances->size;
    stats.peakAccelerationBytes = std::max(stats.accelerationBytes, stats.peakAccelerationBytes);
  }

  scene::TraceFrame PathTracer::prepareScene(const RasterRenderer &resources,
                                             const TraceOptions &options) const {
    using Clock = std::chrono::steady_clock;
    const char *profile = std::getenv("REMIX_OPACITY_PROFILE");
    const auto start = profile ? Clock::now() : Clock::time_point{};
    auto frame = scene::prepareTraceFrame(
        resources.draws(),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_gpu->epoch).count(),
        options.emissiveLights && options.localLights &&
            (options.freezeEmissiveRadiance || options.stabilizeEmissiveLighting),
        m_gpu->surfaceEmissionCache);
    const auto prepared = profile ? Clock::now() : Clock::time_point{};
    // Coverage proof is CPU-only and can overlap the previous GPU submission.
    // Keep the result on this immutable frame, alongside its animated UVs.
    for (auto &instance : frame.instances)
      for (auto &surface : instance)
        surface.opacity = m_gpu->opacityCache.partition(surface.draw, surface.uv);
    const auto classified = profile ? Clock::now() : Clock::time_point{};
    m_gpu->opacityCache.prune();
    const auto pruned = profile ? Clock::now() : Clock::time_point{};
    if (std::getenv("REMIX_OPACITY_DIAGNOSTICS")) {
      uint64_t reasons[scene::OpacityPartition::Count] = {}, mixed = 0;
      for (const auto &instance : frame.instances)
        for (const auto &surface : instance) {
          const auto &partition = surface.opacity;
          mixed += partition->key[0] == 2;
          for (unsigned i = 0; i < scene::OpacityPartition::Count; ++i) reasons[i] += partition->reasons[i];
        }
      std::fprintf(stderr, "OPACITY mixed_surfaces=%llu", static_cast<unsigned long long>(mixed));
      const char *names[] = {"solid", "one_sided", "blended", "transmission", "alpha_test", "invalid",
                             "missing_texture", "texture_boundary", "texture_cutout"};
      for (unsigned i = 0; i < scene::OpacityPartition::Count; ++i)
        std::fprintf(stderr, " %s=%llu", names[i], static_cast<unsigned long long>(reasons[i]));
      std::fprintf(stderr, "\n");
    }
    auto &staged = frame.lighting;
    staged.combined = m_gpu->lightBvh;
    staged.history = m_gpu->emissionHistory;
    const auto mode = options.freezeEmissiveRadiance ? 2u : options.stabilizeEmissiveLighting ? 1u : 0u;
    staged.history.begin(resources.temporalEpoch().world, frame.seconds,
                         options.emissiveLights && options.localLights ? mode : 0u);
    auto lights = options.torchLights ? resources.sphereLights() : std::vector<scene::SphereLight>{};
    const auto sphereCount = lights.size();
    for (const auto &source : frame.emitters) {
      auto light = source.light;
      if (source.animated)
        staged.history.apply(light, source.layout);
      if (options.emissiveLights && light.radius > 1e-8f)
        lights.push_back(light);
    }
    staged.history.end();
    staged.hasEmissive = lights.size() > sphereCount;
    if (staged.hasEmissive && options.separateLocalLighting && options.separateLocalLighting != 3) {
      staged.torch = m_gpu->torchBvh;
      staged.emissive = m_gpu->emissiveBvh;
      staged.torch.update({lights.begin(), lights.begin() + sphereCount});
      staged.emissive.update({lights.begin() + sphereCount, lights.end()});
    }
    staged.combined.update(std::move(lights));
    if (profile) {
      const auto end = Clock::now();
      const auto count = m_gpu->opacityCache.takeStatistics();
      if (auto *file = std::fopen(profile, "a")) {
        if (!std::ftell(file)) std::fprintf(file, "prepare_ms,opacity_ms,prune_ms,lights_ms,lookups,hits,proofs,triangles,evictions,owners,textures\n");
        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b-a).count(); };
        std::fprintf(file, "%.6f,%.6f,%.6f,%.6f,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                     ms(start,prepared), ms(prepared,classified), ms(classified,pruned), ms(pruned,end),
                     (unsigned long long)count.lookups, (unsigned long long)count.hits,
                     (unsigned long long)count.proofs, (unsigned long long)count.triangles,
                     (unsigned long long)count.evictions, (unsigned long long)count.owners, (unsigned long long)count.textures);
        std::fclose(file);
      }
    }
    return frame;
  }

  void PathTracer::prepare(VkCommandBuffer command, RasterRenderer &resources,
                           const TraceOptions &options, scene::TraceFrame &scene) {
    m_gpu->prepare(command, resources, options, scene);
  }

  void PathTracer::record(VkCommandBuffer command) {
    auto &g = *m_gpu;
    if (g.upscaler && !g.debugView) { g.upscaler->composite(command, g.encodeOutput, g.prism, g.gamma22, g.hardwareSrgb); return; }
    VkViewport viewport = {0, 0, float(g.displayExtent.width), float(g.displayExtent.height), 0, 1};
    VkRect2D scissor = {{0, 0}, g.displayExtent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &g.set, 0, nullptr);
    for (uint32_t i = 0; i < 2; ++i) {
      const uint32_t push[] = {i, uint32_t(g.encodeOutput), uint32_t(g.prism),
                               uint32_t(g.gamma22), uint32_t(g.hardwareSrgb),
                               g.displayExtent.width, g.displayExtent.height};
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.composite[i]);
      vkCmdPushConstants(command, g.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
      vkCmdDraw(command, 3, 1, 0, 0);
    }
  }

} // namespace remix
