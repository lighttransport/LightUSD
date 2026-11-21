// SPDX-License-Identifier: Apache-2.0
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "scene-preview-compute-spv.hh"
#include "scene-preview-compute.hh"
namespace lightusd {
namespace tydra {
namespace next {
namespace {
class VulkanCompute final : public PreviewCompute {
  // Private loader dispatch avoids interposing the viewer's Volk globals.
  struct Api {
    std::shared_ptr<HostLibrary> library;
#define PREVIEW_VK_FUNCTIONS(X)               \
  X(vkAllocateCommandBuffers)                 \
  X(vkAllocateDescriptorSets)                 \
  X(vkAllocateMemory)                         \
  X(vkBeginCommandBuffer)                     \
  X(vkBindBufferMemory)                       \
  X(vkCmdBindDescriptorSets)                  \
  X(vkCmdBindPipeline)                        \
  X(vkCmdCopyBuffer)                          \
  X(vkCmdDispatch)                            \
  X(vkCmdPushConstants)                       \
  X(vkCmdPipelineBarrier)                     \
  X(vkCreateBuffer)                           \
  X(vkCreateCommandPool)                      \
  X(vkCreateComputePipelines)                 \
  X(vkCreateDescriptorPool)                   \
  X(vkCreateDescriptorSetLayout)              \
  X(vkCreateDevice)                           \
  X(vkCreateInstance)                         \
  X(vkCreatePipelineLayout)                   \
  X(vkCreateShaderModule)                     \
  X(vkDestroyBuffer)                          \
  X(vkDestroyCommandPool)                     \
  X(vkDestroyDescriptorPool)                  \
  X(vkDestroyDescriptorSetLayout)             \
  X(vkDestroyDevice)                          \
  X(vkDestroyInstance)                        \
  X(vkDestroyPipeline)                        \
  X(vkDestroyPipelineLayout)                  \
  X(vkDestroyShaderModule)                    \
  X(vkDeviceWaitIdle)                         \
  X(vkEndCommandBuffer)                       \
  X(vkEnumeratePhysicalDevices)               \
  X(vkFreeCommandBuffers)                     \
  X(vkFreeMemory)                             \
  X(vkGetBufferMemoryRequirements)            \
  X(vkGetDeviceQueue)                         \
  X(vkGetPhysicalDeviceMemoryProperties)      \
  X(vkGetPhysicalDeviceQueueFamilyProperties) \
  X(vkGetPhysicalDeviceProperties)            \
  X(vkMapMemory)                              \
  X(vkQueueSubmit)                            \
  X(vkQueueWaitIdle)                          \
  X(vkResetCommandPool)                       \
  X(vkResetDescriptorPool)                    \
  X(vkUnmapMemory)                            \
  X(vkUpdateDescriptorSets)
#define DECLARE(name) PFN_##name name = nullptr;
    PREVIEW_VK_FUNCTIONS(DECLARE)
#undef DECLARE
    void* symbol(const char* name) {
      return library ? library->Symbol(name) : nullptr;
    }
    bool load(const PreviewComputeOptions& options, std::string* error) {
      if (!options.host_services || !options.host_services->open_library ||
          options.vulkan_library.empty()) {
        *error = "Vulkan preview requires explicit host services and a library filename";
        return false;
      }
      library = options.host_services->open_library(options.vulkan_library, error);
      if (!library) return false;
#define LOAD(name)                                    \
  name = reinterpret_cast<PFN_##name>(symbol(#name)); \
  if (!name) return false;
      PREVIEW_VK_FUNCTIONS(LOAD)
#undef LOAD
#undef PREVIEW_VK_FUNCTIONS
      return true;
    }
  } api_;
  VkInstance instance_{};
  VkPhysicalDevice physical_{};
  VkDevice device_{};
  VkQueue queue_{};
  uint32_t family_ = 0;
  VkDescriptorSetLayout layout_{};
  VkPipelineLayout pipelineLayout_{};
  VkPipeline pipeline_{};
  std::array<VkPipeline, 5> pipelines_{};
  VkShaderModule module_{};
  VkCommandPool commands_{};
  VkDescriptorPool descriptors_{};
  uint64_t budget_;
  PreviewComputeOptions options_;
  struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkDeviceSize size = 0;
  };
  Buffer input_;
  void free(Buffer& b) {
    if (b.buffer) api_.vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory) api_.vkFreeMemory(device_, b.memory, nullptr);
    b = {};
  }
  bool buffer(VkDeviceSize size, VkBufferUsageFlags usage,
              VkMemoryPropertyFlags properties, Buffer& b) {
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (api_.vkCreateBuffer(device_, &ci, nullptr, &b.buffer) != VK_SUCCESS)
      return false;
    VkMemoryRequirements req{};
    api_.vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    api_.vkGetPhysicalDeviceMemoryProperties(physical_, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((req.memoryTypeBits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & properties) == properties) {
        type = i;
        break;
      }
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (api_.vkAllocateMemory(device_, &ai, nullptr, &b.memory) != VK_SUCCESS)
      return false;
    b.size = size;
    return api_.vkBindBufferMemory(device_, b.buffer, b.memory, 0) ==
           VK_SUCCESS;
  }

 public:
  VulkanCompute(uint64_t budget, const PreviewComputeOptions& options)
      : budget_(budget), options_(options) {}
  ~VulkanCompute() override {
    if (device_) {
      api_.vkDeviceWaitIdle(device_);
      free(input_);
      if (descriptors_)
        api_.vkDestroyDescriptorPool(device_, descriptors_, nullptr);
      if (commands_) api_.vkDestroyCommandPool(device_, commands_, nullptr);
      for (auto pipeline : pipelines_)
        if (pipeline) api_.vkDestroyPipeline(device_, pipeline, nullptr);
      if (module_) api_.vkDestroyShaderModule(device_, module_, nullptr);
      if (pipelineLayout_)
        api_.vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
      if (layout_) api_.vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
      api_.vkDestroyDevice(device_, nullptr);
    }
    if (instance_) api_.vkDestroyInstance(instance_, nullptr);
  }
  const char* name() const override { return "vulkan"; }
  bool init(std::string* error) {
    if (!api_.load(options_, error)) {
      return false;
    }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "LightUSD preview compute";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    if (api_.vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) {
      *error = "Vulkan instance unavailable";
      return false;
    }
    uint32_t count = 0;
    api_.vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    api_.vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    const bool software_only = options_.software_only;
    for (auto pd : devices) {
      VkPhysicalDeviceProperties props{};
      api_.vkGetPhysicalDeviceProperties(pd, &props);
      if (software_only
              ? props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU
              : (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
                 props.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU))
        continue;
      uint32_t n = 0;
      api_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
      std::vector<VkQueueFamilyProperties> q(n);
      api_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, q.data());
      for (uint32_t i = 0; i < n; ++i)
        if (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
          physical_ = pd;
          family_ = i;
          break;
        }
      if (physical_) break;
    }
    if (!physical_) {
      *error = software_only ? "no software Vulkan compute device"
                             : "no hardware Vulkan compute device";
      return false;
    }
    float priority = 1;
    VkDeviceQueueCreateInfo qc{};
    qc.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qc.queueFamilyIndex = family_;
    qc.queueCount = 1;
    qc.pQueuePriorities = &priority;
    VkDeviceCreateInfo dc{};
    dc.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &qc;
    if (api_.vkCreateDevice(physical_, &dc, nullptr, &device_) != VK_SUCCESS)
      return false;
    api_.vkGetDeviceQueue(device_, family_, 0, &queue_);
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lc{};
    lc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lc.bindingCount = 2;
    lc.pBindings = bindings;
    if (api_.vkCreateDescriptorSetLayout(device_, &lc, nullptr, &layout_) !=
        VK_SUCCESS)
      return false;
    VkPipelineLayoutCreateInfo pc{};
    pc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pc.setLayoutCount = 1;
    pc.pSetLayouts = &layout_;
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = sizeof(uint32_t);
    pc.pushConstantRangeCount = 1;
    pc.pPushConstantRanges = &range;
    if (api_.vkCreatePipelineLayout(device_, &pc, nullptr, &pipelineLayout_) !=
        VK_SUCCESS)
      return false;
    VkShaderModuleCreateInfo sc{};
    sc.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sc.codeSize = sizeof(kPreviewComputeSpv);
    sc.pCode = kPreviewComputeSpv;
    if (api_.vkCreateShaderModule(device_, &sc, nullptr, &module_) !=
        VK_SUCCESS)
      return false;
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = family_;
    if (api_.vkCreateCommandPool(device_, &pool, nullptr, &commands_) !=
        VK_SUCCESS)
      return false;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dp{};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &size;
    return api_.vkCreateDescriptorPool(device_, &dp, nullptr, &descriptors_) ==
           VK_SUCCESS;
  }
  bool selectPipeline(const std::vector<uint32_t>& input, std::string* error) {
    const uint32_t key = input[0] == 0    ? 0
                         : input[0] == 2  ? 1
                         : input[27] == 0 ? 2
                         : input[35] <= 1 ? 3
                                          : 4;
    if (!pipelines_[key]) {
      const bool trace = options_.trace_pipeline;
      const auto started = std::chrono::steady_clock::now();
      if (trace)
        std::fprintf(stderr,
                     "[scene-preview] software Vulkan compiling pipeline %u\n",
                     key);
      const uint32_t values[] = {input[0], key >= 3 ? 1u : 0u,
                                 key == 4 ? 1u : 0u};
      VkSpecializationMapEntry entries[3]{};
      for (uint32_t i = 0; i < 3; ++i) {
        entries[i].constantID = i;
        entries[i].offset = i * sizeof(uint32_t);
        entries[i].size = sizeof(uint32_t);
      }
      VkSpecializationInfo specialization{};
      specialization.mapEntryCount = 3;
      specialization.pMapEntries = entries;
      specialization.dataSize = sizeof(values);
      specialization.pData = values;
      VkComputePipelineCreateInfo compute{};
      compute.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      compute.layout = pipelineLayout_;
      compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      compute.stage.module = module_;
      compute.stage.pName = "main";
      compute.stage.pSpecializationInfo = &specialization;
      if (api_.vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &compute,
                                        nullptr,
                                        &pipelines_[key]) != VK_SUCCESS) {
        *error = "Vulkan preview pipeline creation failed";
        return false;
      }
      if (trace)
        std::fprintf(
            stderr,
            "[scene-preview] software Vulkan pipeline %u ready in %.3fs\n", key,
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          started)
                .count());
    }
    pipeline_ = pipelines_[key];
    return true;
  }
  bool dispatch(const std::vector<uint32_t>& input, size_t count,
                std::vector<float>* result, std::string* error,
                bool camera_only) override {
    if (!ValidatePreviewDispatch(input, count, error, options_.debug_validate)) return false;
    if (!selectPipeline(input, error)) return false;
    const uint64_t bytes = input.size() * 4, outputBytes = count * 4;
    const uint64_t reserve = std::min(budget_ / 2, uint64_t(512) << 20);
    if (bytes > (budget_ - reserve) / 2 ||
        outputBytes > (budget_ - reserve) / 2 - bytes) {
      *error = "Vulkan compute exceeds preview budget";
      return false;
    }
    VkPhysicalDeviceProperties props{};
    api_.vkGetPhysicalDeviceProperties(physical_, &props);
    if (bytes > props.limits.maxStorageBufferRange ||
        outputBytes > props.limits.maxStorageBufferRange) {
      *error = "preview buffer exceeds device storage-buffer range";
      return false;
    }
    bool reuse = camera_only && input_.buffer && input_.size == bytes;
    if (!reuse) {
      free(input_);
      if (!buffer(bytes,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, input_)) {
        *error = "Vulkan input allocation failed";
        return false;
      }
    }
    Buffer stage, out, readback;
    struct Cleanup {
      VulkanCompute* owner;
      Buffer *a, *b, *c;
      ~Cleanup() {
        owner->free(*a);
        owner->free(*b);
        owner->free(*c);
      }
    } cleanup{this, &stage, &out, &readback};
    const VkDeviceSize uploadBytes = reuse ? 64 * 4 : bytes;
    auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!buffer(uploadBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host, stage) ||
        !buffer(outputBytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out) ||
        !buffer(outputBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host,
                readback)) {
      *error = "Vulkan compute staging allocation failed";
      return false;
    }
    void* mapped = nullptr;
    if (api_.vkMapMemory(device_, stage.memory, 0, uploadBytes, 0, &mapped) !=
        VK_SUCCESS)
      return false;
    std::memcpy(mapped, input.data(), uploadBytes);
    api_.vkUnmapMemory(device_, stage.memory);
    api_.vkResetDescriptorPool(device_, descriptors_, 0);
    VkDescriptorSet set{};
    VkDescriptorSetAllocateInfo sa{};
    sa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    sa.descriptorPool = descriptors_;
    sa.descriptorSetCount = 1;
    sa.pSetLayouts = &layout_;
    if (api_.vkAllocateDescriptorSets(device_, &sa, &set) != VK_SUCCESS)
      return false;
    VkDescriptorBufferInfo info[2] = {{input_.buffer, 0, bytes},
                                      {out.buffer, 0, outputBytes}};
    VkWriteDescriptorSet write[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
      write[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write[i].dstSet = set;
      write[i].dstBinding = i;
      write[i].descriptorCount = 1;
      write[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write[i].pBufferInfo = &info[i];
    }
    api_.vkUpdateDescriptorSets(device_, 2, write, 0, nullptr);
    const uint64_t items = count / (input[0] == 2 ? 16 : 4);
    const uint64_t batch =
        input[0] == 1 ? (input[27] ? (input[35] > 1 ? 64 : 512) : 4096) : items;
    for (uint64_t base = 0; base < items; base += batch) {
      api_.vkResetCommandPool(device_, commands_, 0);
      VkCommandBuffer command{};
      VkCommandBufferAllocateInfo ca{};
      ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      ca.commandPool = commands_;
      ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ca.commandBufferCount = 1;
      if (api_.vkAllocateCommandBuffers(device_, &ca, &command) != VK_SUCCESS)
        return false;
      VkCommandBufferBeginInfo begin{};
      begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      api_.vkBeginCommandBuffer(command, &begin);
      VkBufferCopy copy{0, 0, uploadBytes};
      if (base == 0)
        api_.vkCmdCopyBuffer(command, stage.buffer, input_.buffer, 1, &copy);
      VkMemoryBarrier barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      api_.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                &barrier, 0, nullptr, 0, nullptr);
      api_.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                             pipeline_);
      api_.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                   pipelineLayout_, 0, 1, &set, 0, nullptr);
      const uint32_t dispatchBase = static_cast<uint32_t>(base);
      api_.vkCmdPushConstants(command, pipelineLayout_,
                              VK_SHADER_STAGE_COMPUTE_BIT, 0,
                              sizeof(dispatchBase), &dispatchBase);
      const uint64_t groups = (std::min(batch, items - base) + 63) / 64;
      if (groups > props.limits.maxComputeWorkGroupCount[0]) {
        *error = "preview dispatch exceeds device workgroup count";
        api_.vkEndCommandBuffer(command);
        api_.vkFreeCommandBuffers(device_, commands_, 1, &command);
        return false;
      }
      api_.vkCmdDispatch(command, static_cast<uint32_t>(groups), 1, 1);
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      api_.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier,
                                0, nullptr, 0, nullptr);
      copy.size = outputBytes;
      if (base + batch >= items)
        api_.vkCmdCopyBuffer(command, out.buffer, readback.buffer, 1, &copy);
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      api_.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                                nullptr, 0, nullptr);
      api_.vkEndCommandBuffer(command);
      VkSubmitInfo submit{};
      submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &command;
      auto status = api_.vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
      if (status == VK_SUCCESS) status = api_.vkQueueWaitIdle(queue_);
      api_.vkFreeCommandBuffers(device_, commands_, 1, &command);
      if (status != VK_SUCCESS) {
        *error = "Vulkan preview dispatch failed (status " +
                 std::to_string(status) + ")";
        return false;
      }
    }
    if (api_.vkMapMemory(device_, readback.memory, 0, outputBytes, 0,
                         &mapped) != VK_SUCCESS)
      return false;
    result->resize(count);
    std::memcpy(result->data(), mapped, outputBytes);
    api_.vkUnmapMemory(device_, readback.memory);
    return true;
  }
};
}  // namespace
std::unique_ptr<PreviewCompute> CreateVulkanPreviewCompute(
    uint64_t budget, std::string* error, const PreviewComputeOptions& options) {
  auto engine = std::make_unique<VulkanCompute>(budget, options);
  if (!engine->init(error)) return {};
  return engine;
}
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
