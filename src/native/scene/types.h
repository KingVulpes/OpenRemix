#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <unordered_map>
#include <remix/remix_c.h>

namespace remix {
  namespace scene {
    using Matrix = std::array<float, 16>; // Column vectors, column-major storage.
    Matrix identity();
    Matrix inverse(const Matrix &matrix);
    uint64_t hashBytes(const void *bytes, size_t size, uint64_t seed = 1469598103934665603ull);
    Matrix multiply(const Matrix &a, const Matrix &b);
    Matrix transform(const remixapi_Transform &value);
    const void *extension(const void *next, remixapi_StructType type);

    struct Pixels {
      uint32_t width = 0, height = 0;
      bool srgb = true;
      std::vector<std::vector<uint8_t>> mips;
      bool twoChannel = false; // BC5 stores normal X/Y; reconstruct Z when used as a normal map.
    };
    struct Texture {
      Pixels pixels;
      mutable std::weak_ptr<Texture> linear;
      std::shared_ptr<Texture> encodedSource; // Keep a file-backed data view's weak path cache alive.
      mutable uint64_t contentHash = 0;
      uint64_t fingerprint() const;
    };
    struct Vertex {
      float position[3];
      float uv[2];
      float color[4];
      float normal[3] = {}; // Zero selects the geometric-normal fallback.
    };
    struct Material {
      std::shared_ptr<Texture> texture;
      std::shared_ptr<Texture> emissionTexture;
      std::shared_ptr<Texture> roughnessTexture, metallicTexture, normalTexture;
      float roughness = 1, metallic = 0;
      bool pbr = false;
      float emission[3] = {};
      bool suppressEmissionLight = false;
      mutable std::unordered_map<uint64_t, std::array<float, 3>> emissionCache;
      float tint[4] = {1, 1, 1, 1};
      uint32_t compare = 7;
      float reference = 0;
      bool drawAlpha = false, translucent = false, transmission = false;
      uint8_t filter = 0, wrapU = 1, wrapV = 1;
      uint8_t rows = 1, columns = 1, fps = 0;
    };
    struct Surface {
      std::vector<Vertex> vertices;
      std::vector<uint32_t> indices;
      std::vector<float> weights;
      std::vector<uint32_t> bones;
      uint32_t bonesPerVertex = 0;
      remixapi_MaterialHandle material = nullptr;
      std::array<float, 3> center = {};
    };
    struct MeshUpdateStream {};
    struct Mesh {
      std::shared_ptr<const MeshUpdateStream> updateStream;
      bool motionCorrespondence = false;
      std::vector<std::shared_ptr<Surface>> surfaces;
    };
    struct Blend {
      bool enabled = false;
      uint32_t src = 6, dst = 7, op = 0, srcAlpha = 1, dstAlpha = 7, alphaOp = 0;
      uint32_t mask = 15;
      // Supported legacy texture operations: select arg1/arg2, modulate.
      uint32_t colorOp = 3, color1 = 1, color2 = 2;
      uint32_t alphaOpTexture = 1, alpha1 = 1, alpha2 = 0;
      uint32_t factor = 0xffffffff;
    };
    struct SphereLight {
      uint64_t id = 0;
      float position[3] = {}, radius = 0;
      float radiance[3] = {};
      // Triangle area emitters share the light hierarchy and stable identity path.
      uint32_t triangle = 0; // 0 sphere, 1 one-sided, 2 two-sided
      float corners[3][3] = {};
      bool operator==(const SphereLight &) const = default;
    };
    struct DistantLight {
      float direction[3] = {0, -1, 0}; // Direction in which light travels.
      float radiance[3] = {};
      float angularDiameterDegrees = 0;
      bool isDistant = false;
      bool operator==(const DistantLight &) const = default;
    };
    struct Draw {
      std::shared_ptr<Mesh> mesh;
      uint64_t instanceId = 0;
      uint32_t instanceIndex = 0, surfaceIndex = 0;
      uint32_t classification = 1; // dynamic for callers without the extension
      bool rasterVisible = true;
      bool traced() const {
        return !ui && camera == REMIXAPI_CAMERA_TYPE_WORLD && classification != 2 && blend.mask != 0;
      }
      std::shared_ptr<Surface> surface;
      std::shared_ptr<Material> material;
      std::shared_ptr<std::vector<Matrix>> bones;
      Matrix model = identity();
      Blend blend;
      uint32_t camera = REMIXAPI_CAMERA_TYPE_WORLD;
      bool doubleSided = false, ui = false, depth = true, ignoreAlpha = false;
      bool depthWriteOverride = false, depthWrite = true, depthBias = false;
      uint32_t depthCompare = 3;
      bool alphaOverride = false;
      uint32_t compare = 7;
      float reference = 0;
      uint32_t canvasWidth = 0, canvasHeight = 0;
      uint32_t firstIndex = 0, indexCount = 0;
      int32_t vertexOffset = 0;
    };
  } // namespace scene
} // namespace remix
