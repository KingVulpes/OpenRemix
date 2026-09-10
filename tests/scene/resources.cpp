#include "scene/fixtures.h"
#include "scene/images.h"
#include <fstream>
#include <limits>

using namespace remix::test;
namespace {
  void cpuTests(const std::filesystem::path &directory) {
    remix::NativeResources r;
    auto red = material(r, 1, 1, 0, 0);
    auto mesh = quad(r, 2, red);
    auto draw = instance(mesh);
    r.drawInstance(draw);
    r.destroyMesh(mesh);
    r.destroyMaterial(red);
    require(r.pendingDrawCount() == 1, "Destroy discarded queued geometry");
    rejected([&] { r.drawInstance(draw); });
    r.finishFrame();
    require(!r.pendingDrawCount(), "Frame not consumed");
    red = material(r, 1, 1, 0, 0);
    mesh = quad(r, 2, red);
    require(reinterpret_cast<uintptr_t>(mesh) == 2, "Hash handle identity");
    remixapi_MeshInfo bad = {};
    bad.hash = 2;
    rejected([&] { r.createMesh(bad, mesh); });
    draw = instance(mesh);
    r.drawInstance(draw); // Failed replacement kept the original.
    draw.transform.matrix[0][0] = std::numeric_limits<float>::quiet_NaN();
    rejected([&] { r.drawInstance(draw); });
    remixapi_CameraInfoParameterizedEXT params = {};
    params.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    params.aspect = 1;
    params.nearPlane = .1f;
    params.farPlane = 10;
    params.fovYInDegrees = 60;
    params.right = {1, 0, 0};
    params.up = {0, 1, 0};
    params.forward = {0, 0, 1};
    remixapi_CameraInfo cam = {};
    cam.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    cam.pNext = &params;
    cam.type = REMIXAPI_CAMERA_TYPE_VIEW_MODEL;
    r.setupCamera(cam);
    params.up = {1, 0, 0};
    rejected([&] { r.setupCamera(cam); });
    params.up = {0, 1, 0};
    params.fovYInDegrees = 180;
    rejected([&] { r.setupCamera(cam); });
    remixapi_TextureInfo texture = {};
    texture.width = texture.height = 1;
    texture.mipLevels = 1;
    const uint8_t bgra[] = {10, 20, 30, 255};
    texture.data = bgra;
    texture.dataSize = 4;
    texture.format = REMIXAPI_FORMAT_B8G8R8A8_UNORM;
    auto decoded = remix::scene::decodeTexture(texture);
    require(decoded.mips[0] == std::vector<uint8_t>({30, 20, 10, 255}), "BGRA decoding");
    texture.mipLevels = 2;
    rejected([&] { remix::scene::decodeTexture(texture); });
    texture.mipLevels = 1;
    texture.dataSize = 3;
    rejected([&] { remix::scene::decodeTexture(texture); });
    const uint8_t bc1[] = {0, 0, 0, 0, 255, 255, 255, 255};
    texture.data = bc1;
    texture.dataSize = 8;
    texture.format = REMIXAPI_FORMAT_BC1_RGB_UNORM;
    require(remix::scene::decodeTexture(texture).mips[0][3] == 255, "Raw BC1 RGB is opaque");
    // Tiny valid DDS fixtures cover channel masks, BC1/2/3, odd dimensions and truncation.
    auto save = [&](const char *name, const std::vector<uint8_t> &data) {
      auto path = directory / name;
      std::ofstream out(path, std::ios::binary);
      out.write(reinterpret_cast<const char *>(data.data()), data.size());
      return path;
    };
    for (unsigned block = 1; block <= 3; ++block) {
      std::vector<uint8_t> dds(128 + (block == 1 ? 8 : 16), 0);
      auto word = [&](size_t offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) {
          dds[offset + i] = uint8_t(value >> (i * 8));
        }
      };
      word(0, 0x20534444);
      word(4, 124);
      word(12, 3);
      word(16, 3);
      word(28, 1);
      word(76, 32);
      word(80, 4);
      word(84, block == 1 ? 0x31545844 : block == 2 ? 0x33545844 : 0x35545844);
      const unsigned color = block == 1 ? 128 : 136;
      dds[color] = 0;
      dds[color + 1] = 0xf8;
      if (block == 2) {
        std::fill(dds.begin() + 128, dds.begin() + 136, 255);
      }
      if (block == 3) {
        dds[128] = 255;
        dds[129] = 255;
      }
      auto path = save("raster-texture.dds", dds);
      auto image = remix::scene::loadTexture(path);
      require(image.width == 3 && image.height == 3 && image.mips[0][0] == 255 && image.mips[0][3] == 255,
              "DDS block decode");
      dds.pop_back();
      save("raster-texture.dds", dds);
      rejected([&] { remix::scene::loadTexture(path); });
    }
    const auto png = remix::scene::loadTexture(directory.parent_path() / "raster-colors.png");
    require(png.width == 2 && png.height == 1 &&
                png.mips[0] == std::vector<uint8_t>({255, 0, 0, 255, 0, 255, 0, 128}),
            "PNG decoding");
    uint32_t pixel = 0xffffffff;
    r.registerUiTexture(77, 1, 1, REMIXAPI_FORMAT_R8G8B8A8_UNORM, &pixel, 4);
    ui(r, 77, 0xffffffff);
    r.freeUiTexture(77);
    rejected([&] { ui(r, 77, 0xffffffff); });
    remixapi_UIVertex uiVertices[3] = {};
    uint32_t uiIndices[] = {0, 1, 3};
    remixapi_UIDrawCommand uiCommand = {0, 3, 0, 0, 0};
    remixapi_UIDrawList list = {};
    list.sType = REMIXAPI_STRUCT_TYPE_UI_DRAW_LIST;
    list.displayWidth = list.displayHeight = 64;
    list.pVertices = uiVertices;
    list.vertexCount = 3;
    list.pIndices = uiIndices;
    list.indexCount = 3;
    list.pCommands = &uiCommand;
    list.commandCount = 1;
    rejected([&] { r.submitUi(&list); });
    uiIndices[2] = 2;
    uiCommand.vertexOffset = -1;
    rejected([&] { r.submitUi(&list); });
    uiCommand.vertexOffset = 0;
    uiCommand.flags = 0x20;
    rejected([&] { r.submitUi(&list); });
    r.reset();
    require(!r.pendingDrawCount(), "Reset retained frame");
    std::puts("Raster resource validation, lifetime, cameras and DDS decoding passed");
  }

}
int main(int argc, char **argv) {
  require(argc == 2, "Supply scratch directory");
  const std::filesystem::path directory(argv[1]);
  std::filesystem::create_directories(directory);
  cpuTests(directory);
}
