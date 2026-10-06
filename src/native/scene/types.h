#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <remix/remix_c.h>
#include <unordered_map>
#include <vector>

namespace remix {
  namespace scene {
    using Matrix = std::array<float, 16>; // Column vectors, column-major storage.
    Matrix identity();
    Matrix inverse(const Matrix &matrix);
    uint64_t hashBytes(const void *bytes, size_t size, uint64_t seed = 1469598103934665603ull);
    // Bulk content hash (8 bytes per step) for change detection within a run.
    uint64_t fastHash(const void *bytes, size_t size, uint64_t seed = 0);
    Matrix multiply(const Matrix &a, const Matrix &b);
    Matrix transform(const remixapi_Transform &value);
    const void *extension(const void *next, remixapi_StructType type);

    // Block-compressed texture kept compressed on the GPU ("Keep BC textures
    // compressed"). Pixels::mips/hdrMips stay empty; CPU consumers read only the
    // small derived data below, never a full decoded copy.
    struct CompressedPixels {
      uint32_t format = 0;   // remixapi_Format as submitted (a BC format)
      uint32_t block = 0;    // 1 BC1, 3 BC3, 4 BC4, 5 BC5, 6 BC6H UF16, 16 BC6H SF16, 7 BC7
      bool bc1Alpha = false; // BC1 RGBA semantics; BC1 RGB samples alpha as one
      uint32_t levels = 0;   // supplied mip levels
      uint64_t hash = 0;     // content hash of every supplied block
      // Supplied blocks, kept only until the one-time GPU upload releases them.
      std::vector<uint8_t> upload;
      // One bit per mip-0 texel (row-major) whose decoded alpha is not 255.
      // Empty when every texel is opaque. Opacity partitions read only this.
      std::vector<uint64_t> transparent;
      bool transparentTexel(uint32_t x, uint32_t y, uint32_t width) const {
        if (transparent.empty())
          return false;
        const size_t i = size_t(y) * width + x;
        return (transparent[i >> 6] >> (i & 63)) & 1;
      }
      // Small decoded level (at most 64x64 texels) for CPU emission statistics:
      // RGBA8 like Pixels::mips, plus linear RGBA32F for BC6H.
      uint32_t previewWidth = 0, previewHeight = 0;
      std::vector<uint8_t> preview;
      std::vector<float> hdrPreview;
    };
    struct Pixels {
      uint32_t width = 0, height = 0;
      bool srgb = true;
      std::vector<std::vector<uint8_t>> mips;
      std::vector<std::vector<float>> hdrMips; // Linear RGBA32F, retained through upload.
      bool twoChannel = false; // BC5 stores normal X/Y; reconstruct Z when used as a normal map.
      std::shared_ptr<CompressedPixels> compressed; // Set only for GPU-compressed textures.
      uint32_t levels() const { return compressed ? compressed->levels : uint32_t(mips.size()); }
    };
    // In-place region updates of a texture (UI textures:
    // remixapi_UpdateUITextureRegionEXT). `revision` counts them; `dirty`
    // (x0, y0, x1, y1) is the union of regions not yet on the GPU. The lock
    // guards both and the texels while they are written or uploaded.
    struct TextureUpdates {
      std::mutex lock;
      uint64_t revision = 0;
      uint32_t dirty[4] = {};
    };
    struct Texture {
      Pixels pixels;
      std::shared_ptr<TextureUpdates> updates; // Set for updatable textures only.
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
      uint64_t sourceHash = 0;
      std::shared_ptr<Texture> texture;
      std::shared_ptr<Texture> emissionTexture;
      std::shared_ptr<Texture> roughnessTexture, metallicTexture, normalTexture;
      float roughness = 1, metallic = 0;
      bool pbr = false;
      uint32_t normalEncoding = 0;
      float emission[3] = {};
      bool suppressEmissionLight = false;
      mutable std::unordered_map<uint64_t, std::array<float, 3>> emissionCache;
      float tint[4] = {1, 1, 1, 1};
      uint32_t compare = 7;
      float reference = 0;
      bool drawAlpha = false, translucent = false, transmission = false;
      uint8_t filter = 0, wrapU = 1, wrapV = 1;
      uint8_t rows = 1, columns = 1, fps = 0;
      // remixapi_MaterialInfoOpaqueSpecGlossEXT (dxvk-remix fork semantics).
      // roughnessEncoding: REMIXAPI_TEXTURE_ENCODING_SMOOTHNESS_IN_RED/GREEN or
      // NONE; with a smoothness map, roughness = max(1 - smoothness, roughness).
      // specularMetal: the fork's packed metalness parameters (0 = off): bits
      // 0-3 spec low, 4-7 spec high, 8-11 albedo luminance max, 12-15
      // strength (each /15), 16-31 specular tint R5 G6 B5.
      uint32_t roughnessEncoding = 0;
      uint32_t specularMetal = 0;
      // remixapi_MaterialInfoOpaqueAlbedoTintEXT: gamma multiplier on the
      // albedo texture's RGB, clamped to [0, 16] and rounded to half precision
      // like the fork's. Set only for opaque materials with an albedo texture.
      bool hasAlbedoTint = false;
      float albedoTint[3] = {1, 1, 1};
      // Set once the handle stops resolving to this object (replaced or
      // destroyed), so retained instances know to resolve the handle again.
      bool retired = false;
    };
    // REMIXAPI_TEXTURE_ENCODING_SMOOTHNESS_IN_RED / _GREEN (remix_fo4_ext.h
    // defines them for Windows only).
    constexpr uint32_t smoothnessInRedEncoding = 2, smoothnessInGreenEncoding = 3;
    // IEEE half-precision bits of a finite v (round to nearest even).
    uint16_t halfBits(float v);
    float halfFloat(uint16_t bits);
    // What a backend's shader decodes for a material's fork extensions.
    struct MaterialDecode {
      static constexpr uint32_t roughnessFromSmoothness = 1, smoothnessInGreen = 2, metallicFromSpecular = 4,
                                albedoTint = 8, albedoSrgb = 16;
      uint32_t flags = 0;
      uint32_t specularMetal = 0;
      uint16_t tint[3] = {0x3c00, 0x3c00, 0x3c00}; // half bits
    };
    MaterialDecode materialDecode(const Material &material);
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
    // Counts destroyed Mesh objects: a consumer holding weak references to
    // meshes (the tracer's views) need not look for expired ones while it has
    // not changed.
    inline std::atomic<uint64_t> meshReleases{0};
    struct Mesh {
      std::shared_ptr<const MeshUpdateStream> updateStream;
      bool motionCorrespondence = false;
      std::vector<std::shared_ptr<Surface>> surfaces;
      bool retired = false; // See Material::retired.
      struct Release {
        // Release: the mesh expired first, for a reader on another thread.
        ~Release() { meshReleases.fetch_add(1, std::memory_order_release); }
      } release;
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
      float direction[3] = {0, 0, -1}, coneCos = -1, softness = 0, focus = 0;
      bool operator==(const SphereLight &) const = default;
    };
    struct DomeLight {
      bool enabled = false;
      std::shared_ptr<Texture> texture;
      Matrix worldToLocal = identity();
      std::array<float, 3> radiance{};
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
      bool decal = false;
      bool alphaOverride = false;
      uint32_t compare = 7;
      float reference = 0;
      uint32_t canvasWidth = 0, canvasHeight = 0;
      uint32_t firstIndex = 0, indexCount = 0;
      int32_t vertexOffset = 0;
    };
    // remixapi_InstanceInfoHaloEXT, validated and clamped. Instances whose
    // parameters compare equal share one group (one silhouette mask).
    struct Halo {
      float color[3] = {1, 1, 1}; // display-referred, 0..1
      float blurX = 2, blurY = 2; // pixels at 1080p output height, (0, maxBlur]
      uint32_t passes = 1;        // 1..maxPasses
      bool additive = true, ignoreZ = false;
      static constexpr float maxBlur = 64;
      static constexpr uint32_t maxPasses = 8;
      bool operator==(const Halo &) const = default;
    };
    // Halo sources of one frame. Their draws are in no other list: only the
    // rasterizer's halo passes read them (never traced, lit or drawn).
    struct HaloGroup {
      Halo halo;
      std::vector<Draw> draws;
    };
  } // namespace scene
} // namespace remix
