#pragma once
#include <functional>
#include <map>
#include <vector>
#include <vulkan/vulkan.h>
namespace remix::upscale {
// Context creation records no GPU work. If the SDK returns halfway through an
// allocation sequence, reclaim every live object before falling back to native.
class ContextAllocations {
public:
  ContextAllocations();
  ~ContextAllocations();
  void commit();
  using Key = std::pair<uint64_t, uintptr_t>;
  void add(Key handle, std::function<void()> destroy);
  void remove(Key handle);

private:
  std::map<Key, std::function<void()>> live;
  std::vector<Key> order;
};
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL sdkDeviceProc(VkDevice device,
                                                       const char *name);
} // namespace remix::upscale
