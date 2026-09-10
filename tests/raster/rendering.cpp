#include "raster/renderer.h"
#include "scene/fixtures.h"
#include <remix/remix_native_ext.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

using namespace remix::test;
namespace {
  using remix::RasterRenderer;
  void vk(VkResult result) { require(result == VK_SUCCESS, "Vulkan call"); }

  std::atomic<unsigned> errors = 0;
  VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT,
                                         const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++errors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }

  struct Target {
    RasterRenderer raster;
    VkInstance instanceHandle = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkImage color;
    VkImageView view;
    VkDeviceMemory imageMemory, readMemory;
    VkBuffer readBuffer;
    VkRenderPass pass;
    VkFramebuffer framebuffer;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
      VkPhysicalDeviceMemoryProperties memory;
      vkGetPhysicalDeviceMemoryProperties(physical, &memory);
      for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
          return i;
        }
      }
      require(false, "Memory type");
      return 0;
    }
    Target() {
      const bool validation = std::getenv("REMIX_VULKAN_VALIDATION");
      const char *layer = "VK_LAYER_KHRONOS_validation";
      const char *extensions[] = {
          VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#if defined(__APPLE__)
          VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
#endif
      };
      const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                     .pApplicationName = "Remix raster readback",
                                     .apiVersion = VK_API_VERSION_1_1};
      const VkInstanceCreateInfo create = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
#if defined(__APPLE__)
                                           .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
#endif
                                           .pApplicationInfo = &app,
                                           .enabledLayerCount = validation ? 1u : 0u,
                                           .ppEnabledLayerNames = &layer,
                                           .enabledExtensionCount = uint32_t(std::size(extensions)),
                                           .ppEnabledExtensionNames = extensions};
      vk(vkCreateInstance(&create, nullptr, &instanceHandle));
      const VkDebugUtilsMessengerCreateInfoEXT debug = {
          .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
          .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
          .messageType =
              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
          .pfnUserCallback = message};
      vk(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(
          instanceHandle, "vkCreateDebugUtilsMessengerEXT"))(instanceHandle, &debug, nullptr, &messenger));
      uint32_t count = 0;
      vk(vkEnumeratePhysicalDevices(instanceHandle, &count, nullptr));
      require(count > 0, "Vulkan device");
      std::vector<VkPhysicalDevice> devices(count);
      vk(vkEnumeratePhysicalDevices(instanceHandle, &count, devices.data()));
      physical = devices[0];
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
      std::vector<VkQueueFamilyProperties> families(count);
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
      uint32_t family = 0;
      while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        ++family;
      }
      require(family < count, "Graphics family");
      const float priority = 1;
      const VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                         .queueFamilyIndex = family,
                                         .queueCount = 1,
                                         .pQueuePriorities = &priority};
#if defined(__APPLE__)
      const char *portability = "VK_KHR_portability_subset";
#endif
      const VkDeviceCreateInfo d = {
          .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
          .queueCreateInfoCount = 1,
          .pQueueCreateInfos = &q,
#if defined(__APPLE__)
          .enabledExtensionCount = 1,
          .ppEnabledExtensionNames = &portability,
#endif
      };
      vk(vkCreateDevice(physical, &d, nullptr, &device));
      vkGetDeviceQueue(device, family, 0, &queue);
      raster.initializeGpu(physical, device);
      const VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                       .imageType = VK_IMAGE_TYPE_2D,
                                       .format = VK_FORMAT_R8G8B8A8_UNORM,
                                       .extent = {64, 64, 1},
                                       .mipLevels = 1,
                                       .arrayLayers = 1,
                                       .samples = VK_SAMPLE_COUNT_1_BIT,
                                       .tiling = VK_IMAGE_TILING_OPTIMAL,
                                       .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
      vk(vkCreateImage(device, &image, nullptr, &color));
      VkMemoryRequirements mem;
      vkGetImageMemoryRequirements(device, color, &mem);
      VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                    .allocationSize = mem.size,
                                    .memoryTypeIndex =
                                        memoryType(mem.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
      vk(vkAllocateMemory(device, &alloc, nullptr, &imageMemory));
      vk(vkBindImageMemory(device, color, imageMemory, 0));
      const VkImageViewCreateInfo v = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                       .image = color,
                                       .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                       .format = VK_FORMAT_R8G8B8A8_UNORM,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      vk(vkCreateImageView(device, &v, nullptr, &view));
      const VkAttachmentDescription attachments[] = {
          {.format = VK_FORMAT_R8G8B8A8_UNORM,
           .samples = VK_SAMPLE_COUNT_1_BIT,
           .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
           .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
           .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
          {.format = raster.depthFormat(),
           .samples = VK_SAMPLE_COUNT_1_BIT,
           .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
           .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
           .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
      const VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                  depthRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
      const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            .colorAttachmentCount = 1,
                                            .pColorAttachments = &colorRef,
                                            .pDepthStencilAttachment = &depthRef};
      const VkSubpassDependency deps[] = {
          {.srcSubpass = VK_SUBPASS_EXTERNAL,
           .dstSubpass = 0,
           .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
           .dstStageMask =
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
           .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
           .dstAccessMask =
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
          {.srcSubpass = 0,
           .dstSubpass = VK_SUBPASS_EXTERNAL,
           .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
           .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
           .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
           .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT}};
      const VkRenderPassCreateInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                         .attachmentCount = 2,
                                         .pAttachments = attachments,
                                         .subpassCount = 1,
                                         .pSubpasses = &subpass,
                                         .dependencyCount = 2,
                                         .pDependencies = deps};
      vk(vkCreateRenderPass(device, &rp, nullptr, &pass));
      raster.configureTarget(pass, {64, 64});
      const VkImageView views[] = {view, raster.depthView()};
      const VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                          .renderPass = pass,
                                          .attachmentCount = 2,
                                          .pAttachments = views,
                                          .width = 64,
                                          .height = 64,
                                          .layers = 1};
      vk(vkCreateFramebuffer(device, &fb, nullptr, &framebuffer));
      const VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                         .size = 64 * 64 * 4,
                                         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
      vk(vkCreateBuffer(device, &buffer, nullptr, &readBuffer));
      vkGetBufferMemoryRequirements(device, readBuffer, &mem);
      alloc.allocationSize = mem.size;
      alloc.memoryTypeIndex = memoryType(mem.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
      vk(vkAllocateMemory(device, &alloc, nullptr, &readMemory));
      vk(vkBindBufferMemory(device, readBuffer, readMemory, 0));
      const VkCommandPoolCreateInfo cp = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                          .queueFamilyIndex = family};
      vk(vkCreateCommandPool(device, &cp, nullptr, &pool));
      const VkCommandBufferAllocateInfo cb = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = 1};
      vk(vkAllocateCommandBuffers(device, &cb, &command));
      const VkFenceCreateInfo f = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      vk(vkCreateFence(device, &f, nullptr, &fence));
    }
    ~Target() {
      vkDeviceWaitIdle(device);
      vkDestroyFramebuffer(device, framebuffer, nullptr);
      raster.shutdownGpu();
      vkDestroyRenderPass(device, pass, nullptr);
      vkDestroyImageView(device, view, nullptr);
      vkDestroyImage(device, color, nullptr);
      vkFreeMemory(device, imageMemory, nullptr);
      vkDestroyBuffer(device, readBuffer, nullptr);
      vkFreeMemory(device, readMemory, nullptr);
      vkDestroyFence(device, fence, nullptr);
      vkDestroyCommandPool(device, pool, nullptr);
      vkDestroyDevice(device, nullptr);
      reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(
          instanceHandle, "vkDestroyDebugUtilsMessengerEXT"))(instanceHandle, messenger, nullptr);
      vkDestroyInstance(instanceHandle, nullptr);
    }
    void outputEncoding(bool enabled) {
      vk(vkDeviceWaitIdle(device));
      vkDestroyFramebuffer(device, framebuffer, nullptr);
      raster.configureTarget(pass, {64, 64}, enabled);
      const VkImageView views[] = {view, raster.depthView()};
      const VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                          .renderPass = pass,
                                          .attachmentCount = 2,
                                          .pAttachments = views,
                                          .width = 64,
                                          .height = 64,
                                          .layers = 1};
      vk(vkCreateFramebuffer(device, &fb, nullptr, &framebuffer));
    }
    std::vector<uint8_t> render() {
      vk(vkResetCommandBuffer(command, 0));
      const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      vk(vkBeginCommandBuffer(command, &begin));
      raster.prepare(command);
      VkClearValue clear[2] = {};
      clear[1].depthStencil.depth = 1;
      const VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                        .renderPass = pass,
                                        .framebuffer = framebuffer,
                                        .renderArea = {{0, 0}, {64, 64}},
                                        .clearValueCount = 2,
                                        .pClearValues = clear};
      vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
      raster.record(command);
      vkCmdEndRenderPass(command);
      const VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .imageExtent = {64, 64, 1}};
      vkCmdCopyImageToBuffer(command, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readBuffer, 1, &copy);
      const VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                       .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                       .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                           &barrier, 0, nullptr, 0, nullptr);
      vk(vkEndCommandBuffer(command));
      vk(vkResetFences(device, 1, &fence));
      const VkSubmitInfo submit = {
          .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
      vk(vkQueueSubmit(queue, 1, &submit, fence));
      vk(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
      void *mapped;
      vk(vkMapMemory(device, readMemory, 0, VK_WHOLE_SIZE, 0, &mapped));
      const VkMappedMemoryRange range = {
          .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = readMemory, .size = VK_WHOLE_SIZE};
      vk(vkInvalidateMappedMemoryRanges(device, 1, &range));
      std::vector<uint8_t> pixels(64 * 64 * 4);
      std::memcpy(pixels.data(), mapped, pixels.size());
      vkUnmapMemory(device, readMemory);
      raster.finishFrame();
      return pixels;
    }
  };
  void pixel(const std::vector<uint8_t> &data, int x, int y, int r, int g, int b, const char *what) {
    auto p = data.data() + (y * 64 + x) * 4;
    if (std::abs(p[0] - r) > 2 || std::abs(p[1] - g) > 2 || std::abs(p[2] - b) > 2) {
      std::fprintf(stderr, "%s: pixel %d,%d = %u,%u,%u; expected %d,%d,%d\n", what, x, y, p[0], p[1], p[2], r,
                   g, b);
      std::exit(1);
    }
  }
  void gpuTests(const std::filesystem::path &directory) {
    Target target;
    auto &r = target.raster;
    camera(r);
    auto red = material(r, 1, 1, 0, 0), green = material(r, 2, 0, 1, 0), white = material(r, 3, 1, 1, 1);
    auto nearMesh = quad(r, 4, red), farMesh = quad(r, 5, green);
    r.drawInstance(instance(nearMesh, -.2f));
    r.drawInstance(instance(farMesh, .2f));
    pixel(target.render(), 32, 32, 255, 0, 0, "Depth occlusion and front face");
    remixapi_InstanceRasterStateEXT state = {};
    state.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_RASTER_STATE_EXT;
    state.depthTest = 1;
    state.depthWrite = 0;
    state.depthCompare = 3;
    auto stateDraw = instance(nearMesh, -.2f);
    stateDraw.pNext = &state;
    r.drawInstance(stateDraw);
    r.drawInstance(instance(farMesh, .2f));
    pixel(target.render(), 32, 32, 0, 255, 0, "Native depth write disabled");
    state.depthTest = 0;
    state.depthWrite = 1;
    stateDraw = instance(farMesh, .2f);
    stateDraw.pNext = &state;
    r.drawInstance(instance(nearMesh, -.2f));
    r.drawInstance(stateDraw);
    pixel(target.render(), 32, 32, 0, 255, 0, "Native depth test disabled");
    state.depthTest = 1;
    state.depthCompare = 2; // equal
    stateDraw = instance(farMesh, -.2f);
    stateDraw.pNext = &state;
    r.drawInstance(instance(nearMesh, -.2f));
    r.drawInstance(stateDraw);
    pixel(target.render(), 32, 32, 0, 255, 0, "Native equal depth overlay");
    state.depthCompare = 3;
    state.disableColorWrites = 1;
    stateDraw = instance(nearMesh, -.2f);
    stateDraw.pNext = &state;
    r.drawInstance(stateDraw);
    r.drawInstance(instance(farMesh, .2f));
    pixel(target.render(), 32, 32, 0, 0, 0, "Native depth-only prepass");
    state.depthCompare = 8;
    rejected([&] { r.drawInstance(stateDraw); });
    auto backwards = instance(nearMesh);
    backwards.transform.matrix[0][0] = -1;
    r.drawInstance(backwards);
    pixel(target.render(), 32, 32, 0, 0, 0, "Backface culling");
    backwards.doubleSided = 1;
    r.drawInstance(backwards);
    pixel(target.render(), 32, 32, 255, 0, 0, "Double-sided geometry");
    pixel(target.render(), 32, 32, 0, 0, 0, "Empty frame cleared old geometry");
    auto colored = quad(r, 6, white, 0xff0000ff);
    r.drawInstance(instance(colored));
    pixel(target.render(), 32, 32, 0, 0, 255, "ARGB world vertex colors");
    remixapi_CameraInfoParameterizedEXT params = {};
    params.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    params.position = {0, 0, -2};
    params.forward = {0, 0, 1};
    params.right = {1, 0, 0};
    params.up = {0, 1, 0};
    params.aspect = 1;
    params.fovYInDegrees = 90;
    params.nearPlane = .1f;
    params.farPlane = 10;
    remixapi_CameraInfo cam = {};
    cam.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    cam.pNext = &params;
    r.setupCamera(cam);
    r.drawInstance(instance(colored));
    auto perspective = target.render();
    pixel(perspective, 32, 32, 0, 0, 255, "Parameterized camera");
    pixel(perspective, 10, 32, 0, 0, 0, "Perspective projection extent");
    camera(r);
    cam.type = REMIXAPI_CAMERA_TYPE_VIEW_MODEL;
    r.setupCamera(cam);
    r.drawInstance(instance(farMesh, -.4f));
    auto viewModel = instance(colored);
    viewModel.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
    r.drawInstance(viewModel);
    pixel(target.render(), 32, 32, 0, 0, 255, "View-model camera and depth reset");
    auto glass = material(r, 7, 0, 1, 0, .5f);
    auto translucent = quad(r, 8, glass);
    r.drawInstance(instance(nearMesh, .2f));
    r.drawInstance(instance(translucent, -.2f));
    pixel(target.render(), 32, 32, 128, 128, 0, "Alpha blending");
    // Resource replacement and destruction after submission must preserve this frame.
    r.drawInstance(instance(nearMesh));
    material(r, 1, 0, 0, 1);
    r.destroyMesh(nearMesh);
    r.destroyMaterial(red);
    pixel(target.render(), 32, 32, 255, 0, 0, "Queued resource versions");
    auto skinned = quad(r, 9, white, 0xffffffff, true);
    auto d = instance(skinned);
    remixapi_Transform bone = {};
    bone.matrix[0][0] = bone.matrix[1][1] = bone.matrix[2][2] = .25f;
    bone.matrix[0][3] = .5f;
    remixapi_InstanceInfoBoneTransformsEXT skin = {};
    skin.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
    skin.boneTransforms_values = &bone;
    skin.boneTransforms_count = 1;
    d.pNext = &skin;
    r.drawInstance(d);
    bone.matrix[0][3] = -100; // Caller memory may change immediately.
    auto pixels = target.render();
    pixel(pixels, 48, 32, 255, 255, 255, "CPU skinning and deep copy");
    pixel(pixels, 24, 32, 0, 0, 0, "Skinned translation");
    bone.matrix[0][3] = -.5f;
    r.drawInstance(d);
    pixels = target.render();
    pixel(pixels, 16, 32, 255, 255, 255, "New skinned vertices uploaded on the next frame");
    pixel(pixels, 48, 32, 0, 0, 0, "Previous skinned upload was retired");
    remixapi_Transform transforms[2] = {};
    for (int i = 0; i < 2; ++i) {
      transforms[i].matrix[0][0] = transforms[i].matrix[1][1] = transforms[i].matrix[2][2] = .25f;
      transforms[i].matrix[0][3] = i ? .5f : -.5f;
    }
    remixapi_InstanceInfoGpuInstancingEXT instancing = {};
    instancing.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT;
    instancing.instanceTransforms_values = transforms;
    instancing.instanceTransforms_count = 2;
    d = instance(colored);
    d.pNext = &instancing;
    r.drawInstance(d);
    transforms[0].matrix[0][3] = 10;
    pixels = target.render();
    pixel(pixels, 16, 32, 0, 0, 255, "Expanded instancing left");
    pixel(pixels, 48, 32, 0, 0, 255, "Expanded instancing right");
    const uint8_t texels[] = {255, 0, 0, 255, 0, 255, 0, 0};
    remixapi_TextureInfo tex = {};
    tex.hash = 10;
    tex.width = 2;
    tex.height = 1;
    tex.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
    tex.data = texels;
    tex.dataSize = 8;
    remixapi_TextureHandle th;
    r.createTexture(tex, th);
#if defined(_WIN32)
    auto cutout = material(r, 11, 1, 1, 1, 1, L"0xa", 4);
#else
    auto cutout = material(r, 11, 1, 1, 1, 1, "0xa", 4);
#endif
    auto cutMesh = quad(r, 12, cutout);
    r.drawInstance(instance(cutMesh));
    pixels = target.render();
    pixel(pixels, 16, 32, 255, 0, 0, "Texture orientation");
    pixel(pixels, 48, 32, 0, 0, 0, "Alpha cutout");
    ui(r, 0, 0xff0000ff);
    pixel(target.render(), 32, 32, 255, 0, 0, "RGBA UI vertex colors");
    remixapi_UIVertex inventory[8] = {};
    for (int i = 0; i < 8; ++i) {
      inventory[i].x = (i % 4 == 0 || i % 4 == 3) ? 16.f : 48.f;
      inventory[i].y = i % 4 < 2 ? 16.f : 48.f;
      inventory[i].z = i < 4 ? .2f : .8f;
      inventory[i].color = i < 4 ? 0xff0000ff : 0xff00ff00;
    }
    const uint32_t inventoryIndices[] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    remixapi_UIDrawCommand inventoryCommands[] = {{0, 6, 0, 0, 1}, {0, 6, 6, 0, 1}};
    remixapi_UIDrawList inventoryUi = {};
    inventoryUi.sType = REMIXAPI_STRUCT_TYPE_UI_DRAW_LIST;
    inventoryUi.displayWidth = inventoryUi.displayHeight = 64;
    inventoryUi.pVertices = inventory;
    inventoryUi.vertexCount = 8;
    inventoryUi.pIndices = inventoryIndices;
    inventoryUi.indexCount = 12;
    inventoryUi.pCommands = inventoryCommands;
    inventoryUi.commandCount = 2;
    r.submitUi(&inventoryUi);
    pixel(target.render(), 32, 32, 255, 0, 0, "Inventory model depth");
    inventoryCommands[1].flags = 0;
    r.submitUi(&inventoryUi);
    pixel(target.render(), 32, 32, 0, 255, 0, "Flat UI painter order");
    const uint8_t repeating[] = {255, 0, 0, 255, 0, 255, 0, 255};
    r.registerUiTexture(98, 2, 1, REMIXAPI_FORMAT_R8G8B8A8_UNORM, repeating, sizeof(repeating));
    for (int i = 0; i < 4; ++i) {
      inventory[i].u = (i == 0 || i == 3) ? 0.f : 2.f;
      inventory[i].color = 0xffffffff;
    }
    inventoryUi.commandCount = 1;
    inventoryCommands[0].textureId = 98;
    inventoryCommands[0].flags = 2;
    r.submitUi(&inventoryUi);
    pixel(target.render(), 36, 32, 255, 0, 0, "Native UI repeats texture coordinates");
    inventoryCommands[0].flags = 0;
    r.submitUi(&inventoryUi);
    pixel(target.render(), 36, 32, 0, 255, 0, "Legacy UI clamps texture coordinates");
    const uint8_t grey[] = {128, 128, 128, 255};
    r.registerUiTexture(99, 1, 1, REMIXAPI_FORMAT_R8G8B8A8_UNORM, grey, 4);
    ui(r, 99, 0xffffffff);
    pixel(target.render(), 32, 32, 55, 55, 55, "UI display-color decoding into linear target");
    const uint8_t blue[] = {0, 0, 255, 255};
    r.registerUiTexture(1, 1, 1, REMIXAPI_FORMAT_R8G8B8A8_UNORM, blue, 4);
    ui(r, 1, 0xffffffff, true, .9f);
    r.freeUiTexture(1);
    r.drawInstance(instance(farMesh, -.4f));
    pixels = target.render();
    pixel(pixels, 32, 32, 0, 0, 255, "UI has separate depth and retains freed texture");
    r.overlay(blue, 1, 1, REMIXAPI_FORMAT_R8G8B8A8_UNORM, 1);
    pixels = target.render();
    pixel(pixels, 0, 0, 0, 0, 255, "Screen overlay");
    r.overlay(nullptr, 0, 0, REMIXAPI_FORMAT_R8G8B8A8_UNORM, 0);
    r.setTint(1, 0, 0, .5f);
    pixel(target.render(), 32, 32, 128, 0, 0, "Screen tint");
    r.setTint(0, 0, 0, 0);
    target.outputEncoding(true);
    const auto greyMaterial = material(r, 100, .21404114f, .21404114f, .21404114f);
    r.drawInstance(instance(quad(r, 101, greyMaterial)));
    pixel(target.render(), 32, 32, 128, 128, 128, "UNORM-only output encoding");
    target.outputEncoding(false);
    // Persistent scene + UI reference artifact.
    r.drawInstance(instance(farMesh));
    ui(r, 0, 0x800000ff);
    pixels = target.render();
    std::ofstream out(directory / "raster-readback.ppm", std::ios::binary);
    out << "P6\n64 64\n255\n";
    for (size_t i = 0; i < pixels.size(); i += 4) {
      out.write(reinterpret_cast<const char *>(pixels.data() + i), 3);
    }
    std::puts(
        "Raster GPU readback: depth, culling, textures, cutouts, blending, skinning, UI and lifetime passed");
  }
} // namespace
int main(int argc, char **argv) {
  require(argc >= 2, "Supply scratch directory");
  const std::filesystem::path directory(argv[1]);
  std::filesystem::create_directories(directory);
  gpuTests(directory);
  require(!errors.load(), "Vulkan validation errors");
}
