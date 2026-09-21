#include "pathtrace/gpu.h"

namespace remix {
using namespace trace::detail;
void PathTracer::Gpu::beginRayProfile(VkCommandBuffer command, Frame &frame) {
  if (!rayProfile.path)
    return;
  if (!rayProfile.counters)
    rayProfile.counters = buffer(384,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 false);
  if (!rayProfile.readback)
    rayProfile.readback = buffer(384, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  frame.reuseConfig[2] = uint32_t(rayProfile.counters->address);
  frame.reuseConfig[3] = uint32_t(rayProfile.counters->address >> 32);
  barrier(command,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  vkCmdFillBuffer(command, rayProfile.counters->handle, 0, 384, 0);
  barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}
void PathTracer::Gpu::finishRayProfile(VkCommandBuffer command) {
  if (!rayProfile.path)
    return;
  barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferCopy copy{.size = 384};
  vkCmdCopyBuffer(command, rayProfile.counters->handle,
                  rayProfile.readback->handle, 1, &copy);
  barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
  rayProfile.pending = true;
}
void PathTracer::Gpu::completeRayProfile() {
  if (!rayProfile.pending)
    return;
  void *mapped{};
  check(vkMapMemory(device, rayProfile.readback->memory, 0, VK_WHOLE_SIZE, 0,
                    &mapped));
  VkMappedMemoryRange range{.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                            .memory = rayProfile.readback->memory,
                            .size = VK_WHOLE_SIZE};
  const auto result = vkInvalidateMappedMemoryRanges(device, 1, &range);
  uint32_t counts[96] = {};
  if (result == VK_SUCCESS)
    std::memcpy(counts, mapped, sizeof(counts));
  vkUnmapMemory(device, rayProfile.readback->memory);
  check(result);
  if (auto *file = std::fopen(rayProfile.path, "a")) {
    if (std::ftell(file) == 0) {
      std::fprintf(file, "time_ms");
      for (const char *pass : {"opaque", "direct", "transparency"})
        for (const char *ray : {"scene", "sun", "local"})
          for (const char *event :
               {"queries", "candidates", "alpha", "alpha_reject", "back_reject",
                "committed", "alpha_texture", "reserved"})
            std::fprintf(file, ",%s_%s_%s", pass, ray, event);
      std::fprintf(file, "\n");
    }
    const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    std::fprintf(file, "%lld", static_cast<long long>(time));
    for (unsigned pass = 0; pass < 3; ++pass)
      for (unsigned i = 0; i < 24; ++i)
        std::fprintf(file, ",%u", counts[pass * 32 + i]);
    std::fprintf(file, "\n");
    std::fclose(file);
  }
  rayProfile.pending = false;
}
} // namespace remix
