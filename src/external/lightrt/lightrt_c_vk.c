/*
 * lightrt_c_vk.c — Vulkan GPU interop implementation.
 *
 * Engine (instance/device/compute-queue/command-pool, host-visible storage
 * buffers, compute pipelines), Path A (CPU build -> GPU trace) and Path B
 * (GPU build front end -> CPU trace). Uses lightrt_vkew (runtime dlopen of
 * libvulkan; no Vulkan SDK headers, no link-time -lvulkan).
 *
 * v1 keeps it simple and correct: all buffers are HOST_VISIBLE | HOST_COHERENT
 * and uploaded/downloaded by map+memcpy (no staging/DEVICE_LOCAL copies yet).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lightrt_c_vk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lightrt_vkew.h"

/* Embedded SPIR-V (checked in; regenerate with scripts/compile_shaders.sh). */
#include "vk/shaders/trace_bvh.spv.h"        /* trace_bvh_spv[]       */
#include "vk/shaders/build_morton.spv.h"     /* build_morton_spv[]    */
#include "vk/shaders/trace_ray_query.spv.h"  /* trace_ray_query_spv[] */
#include "vk/shaders/trace_ray_query_wide.spv.h" /* trace_ray_query_wide_spv[] */
#include "vk/shaders/shade_analytic.spv.h"   /* shade_analytic_spv[]  */
#include "vk/shaders/trace_materialx_path.spv.h"

/* Host-read sync flags not declared in lightrt_vkew.h. */
#ifndef VK_PIPELINE_STAGE_HOST_BIT
#define VK_PIPELINE_STAGE_HOST_BIT 0x00004000
#endif
#ifndef VK_ACCESS_HOST_READ_BIT
#define VK_ACCESS_HOST_READ_BIT 0x00002000
#endif
#ifndef VK_PIPELINE_STAGE_TRANSFER_BIT
#define VK_PIPELINE_STAGE_TRANSFER_BIT 0x00001000
#endif
#ifndef VK_ACCESS_TRANSFER_READ_BIT
#define VK_ACCESS_TRANSFER_READ_BIT 0x00000800
#endif
#ifndef VK_ACCESS_TRANSFER_WRITE_BIT
#define VK_ACCESS_TRANSFER_WRITE_BIT 0x00001000
#endif

/* GPU-assisted build hook implemented in lightrt_c_tri.c (not a public ABI). */
extern lrt_tri_scene *lrt_tri_scene_build_lbvh_morton(const float *vertices,
                                                      size_t ntris,
                                                      const uint32_t *morton,
                                                      lrt_tri_layout layout,
                                                      unsigned max_leaf_size,
                                                      lrt_result *err);

/* ------------------------------------------------------------------------- */
/* LRTS serialization header (must match lightrt_c_tri.c).                   */
/* ------------------------------------------------------------------------- */
typedef struct vk_lrts_header {
    char magic[4];
    uint32_t version;
    uint32_t endian;
    uint32_t flags;
    uint32_t layout;
    uint32_t prim_kind;
    uint32_t node_count;
    uint32_t block_count;
    uint32_t root;
    uint32_t node_stride;
    uint32_t block_stride;
    uint32_t reserved0;
    float root_lo[3];
    float root_hi[3];
    uint64_t node_offset;
    uint64_t block_offset;
    uint64_t file_size;
} vk_lrts_header;

/* ------------------------------------------------------------------------- */
/* Engine.                                                                   */
/* ------------------------------------------------------------------------- */
#define VK_TRACE_PIPE_CACHE 8

typedef struct vk_pipeline {
    int valid;
    uint32_t key_w;
    uint32_t key_stack;
    VkShaderModule module;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
} vk_pipeline;

struct lrt_vk_engine {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    VkCommandPool cmd_pool;
    VkPhysicalDeviceMemoryProperties mem_props;
    uint32_t caps;
    char device_name[256];
    char err[512];
    /* Set once any submit returns VK_ERROR_DEVICE_LOST (e.g. a build ran the GPU
     * out of memory). On a lost device the NVIDIA driver crashes inside
     * vkDestroy*, so all teardown skips driver calls and just frees host state. */
    int device_lost;

    vk_pipeline trace_pipes[VK_TRACE_PIPE_CACHE];
    vk_pipeline build_pipe; /* build_morton: no spec constants */
    vk_pipeline rtx_pipe;   /* trace_ray_query: AS + 2 SSBO bindings */
    vk_pipeline rtx_pipe_wide; /* trace_ray_query_wide: 5-word hits (inst,prim) */
    vk_pipeline shade_pipe; /* shade_analytic: 3 SSBO bindings + push */
    vk_pipeline material_path_pipe; /* TLAS + packed MaterialX/path buffers */
};

static void vk_set_err(lrt_vk_engine *e, const char *msg) {
    if (!e) return;
    snprintf(e->err, sizeof(e->err), "%s", msg);
}

static void vk_set_errf(lrt_vk_engine *e, const char *what, VkResult r) {
    if (!e) return;
    snprintf(e->err, sizeof(e->err), "%s: %s", what, vkewResultToString(r));
}

static uint32_t vk_find_memory_type(const lrt_vk_engine *e, uint32_t type_bits,
                                    VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < e->mem_props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (e->mem_props.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* ------------------------------------------------------------------------- */
/* Host-visible storage buffers.                                             */
/* ------------------------------------------------------------------------- */
typedef struct vk_buffer {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
} vk_buffer;

/* Full buffer creation: arbitrary usage, host-visible (mappable) or device-local
 * memory, and optionally device-address-capable (chains VkMemoryAllocateFlagsInfo
 * + adds SHADER_DEVICE_ADDRESS usage) for AS-input / scratch buffers. */
static int vk_buffer_create_ex(lrt_vk_engine *e, VkDeviceSize size,
                               VkBufferUsageFlags usage, int host_visible,
                               int device_addr, vk_buffer *out) {
    memset(out, 0, sizeof(*out));
    if (size == 0) size = 16;
    out->size = size;
    /* BLAS/TLAS addresses are queried after building (including compacted and
     * pooled structures). Their backing storage must support device addresses,
     * not only the build inputs and scratch buffers. VUID-pInfo-09542. */
    if (usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR)
        device_addr = 1;
    if (device_addr) usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(e->device, &bi, NULL, &out->buf);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkCreateBuffer", r);
        return 0;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(e->device, out->buf, &req);
    VkMemoryPropertyFlags want =
        host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                     : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t mt = vk_find_memory_type(e, req.memoryTypeBits, want);
    if (mt == UINT32_MAX) {
        vk_set_err(e, "no suitable memory type");
        vkDestroyBuffer(e->device, out->buf, NULL);
        out->buf = VK_NULL_HANDLE;
        return 0;
    }
    VkMemoryAllocateFlagsInfo fi;
    memset(&fi, 0, sizeof(fi));
    fi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext = device_addr ? &fi : NULL;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mt;
    r = vkAllocateMemory(e->device, &ai, NULL, &out->mem);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkAllocateMemory", r);
        vkDestroyBuffer(e->device, out->buf, NULL);
        out->buf = VK_NULL_HANDLE;
        return 0;
    }
    vkBindBufferMemory(e->device, out->buf, out->mem, 0);
    return 1;
}

/* Host-visible coherent storage buffer (Path A/B working set). */
static int vk_buffer_create(lrt_vk_engine *e, VkDeviceSize size, vk_buffer *out) {
    return vk_buffer_create_ex(e, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, 0,
                               out);
}

/* TRANSFER_DST readback buffer preferring HOST_CACHED memory (cached CPU reads
 * are far faster than reads from write-combined HOST_COHERENT memory) while
 * keeping HOST_COHERENT too, so no manual vkInvalidateMappedMemoryRanges is
 * needed (vkew does not expose it). Falls back to plain coherent. */
static int vk_buffer_create_staging(lrt_vk_engine *e, VkDeviceSize size,
                                    vk_buffer *out) {
    memset(out, 0, sizeof(*out));
    if (size == 0) size = 16;
    out->size = size;
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(e->device, &bi, NULL, &out->buf) != VK_SUCCESS) {
        vk_set_err(e, "vkCreateBuffer(staging)");
        out->buf = VK_NULL_HANDLE;
        return 0;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(e->device, out->buf, &req);
    uint32_t mt = vk_find_memory_type(
        e, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mt == UINT32_MAX)
        mt = vk_find_memory_type(e, req.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) {
        vk_set_err(e, "no host-visible memory type for staging");
        vkDestroyBuffer(e->device, out->buf, NULL);
        out->buf = VK_NULL_HANDLE;
        return 0;
    }
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mt;
    if (vkAllocateMemory(e->device, &ai, NULL, &out->mem) != VK_SUCCESS) {
        vk_set_err(e, "vkAllocateMemory(staging)");
        vkDestroyBuffer(e->device, out->buf, NULL);
        out->buf = VK_NULL_HANDLE;
        return 0;
    }
    vkBindBufferMemory(e->device, out->buf, out->mem, 0);
    return 1;
}

static void vk_buffer_destroy(lrt_vk_engine *e, vk_buffer *b) {
    if (!e->device_lost) {  /* driver calls crash on a lost device; just free host */
        if (b->buf) vkDestroyBuffer(e->device, b->buf, NULL);
        if (b->mem) vkFreeMemory(e->device, b->mem, NULL);
    }
    memset(b, 0, sizeof(*b));
}

static int vk_buffer_write(lrt_vk_engine *e, vk_buffer *b, const void *src,
                           size_t bytes) {
    void *p = NULL;
    VkResult r = vkMapMemory(e->device, b->mem, 0, VK_WHOLE_SIZE, 0, &p);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkMapMemory(write)", r);
        return 0;
    }
    if (bytes) memcpy(p, src, bytes);
    vkUnmapMemory(e->device, b->mem);
    return 1;
}

static int vk_buffer_read(lrt_vk_engine *e, vk_buffer *b, void *dst,
                          size_t bytes) {
    void *p = NULL;
    VkResult r = vkMapMemory(e->device, b->mem, 0, VK_WHOLE_SIZE, 0, &p);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkMapMemory(read)", r);
        return 0;
    }
    if (bytes) memcpy(dst, p, bytes);
    vkUnmapMemory(e->device, b->mem);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Compute pipeline construction.                                            */
/* ------------------------------------------------------------------------- */
static int vk_pipeline_build(lrt_vk_engine *e, const uint32_t *spv,
                             size_t spv_bytes, uint32_t num_bindings,
                             const VkDescriptorType *types, uint32_t push_size,
                             const VkSpecializationInfo *spec, vk_pipeline *out) {
    VkResult r;
    VkShaderModuleCreateInfo smi;
    memset(&smi, 0, sizeof(smi));
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = spv_bytes;
    smi.pCode = spv;
    r = vkCreateShaderModule(e->device, &smi, NULL, &out->module);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkCreateShaderModule", r);
        return 0;
    }

    VkDescriptorSetLayoutBinding binds[16];
    memset(binds, 0, sizeof(binds));
    for (uint32_t i = 0; i < num_bindings; i++) {
        binds[i].binding = i;
        binds[i].descriptorType =
            types ? types[i] : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci;
    memset(&dlci, 0, sizeof(dlci));
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = num_bindings;
    dlci.pBindings = binds;
    r = vkCreateDescriptorSetLayout(e->device, &dlci, NULL, &out->dsl);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkCreateDescriptorSetLayout", r);
        vkDestroyShaderModule(e->device, out->module, NULL);
        return 0;
    }

    VkPushConstantRange pcr;
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = push_size;
    VkPipelineLayoutCreateInfo plci;
    memset(&plci, 0, sizeof(plci));
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &out->dsl;
    plci.pushConstantRangeCount = push_size ? 1 : 0;
    plci.pPushConstantRanges = push_size ? &pcr : NULL;
    r = vkCreatePipelineLayout(e->device, &plci, NULL, &out->layout);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkCreatePipelineLayout", r);
        vkDestroyDescriptorSetLayout(e->device, out->dsl, NULL);
        vkDestroyShaderModule(e->device, out->module, NULL);
        return 0;
    }

    VkComputePipelineCreateInfo cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = out->module;
    cpci.stage.pName = "main";
    cpci.stage.pSpecializationInfo = spec;
    cpci.layout = out->layout;
    cpci.basePipelineIndex = -1;
    r = vkCreateComputePipelines(e->device, VK_NULL_HANDLE, 1, &cpci, NULL,
                                 &out->pipe);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkCreateComputePipelines", r);
        vkDestroyPipelineLayout(e->device, out->layout, NULL);
        vkDestroyDescriptorSetLayout(e->device, out->dsl, NULL);
        vkDestroyShaderModule(e->device, out->module, NULL);
        return 0;
    }
    out->valid = 1;
    return 1;
}

static void vk_pipeline_destroy(lrt_vk_engine *e, vk_pipeline *p) {
    if (!p->valid) return;
    vkDestroyPipeline(e->device, p->pipe, NULL);
    vkDestroyPipelineLayout(e->device, p->layout, NULL);
    vkDestroyDescriptorSetLayout(e->device, p->dsl, NULL);
    vkDestroyShaderModule(e->device, p->module, NULL);
    memset(p, 0, sizeof(*p));
}

/* Allocate a one-shot descriptor pool + set bound to `n` storage buffers. */
static int vk_descriptors_bind(lrt_vk_engine *e, VkDescriptorSetLayout dsl,
                               const vk_buffer *buffers, uint32_t n,
                               VkDescriptorPool *out_pool,
                               VkDescriptorSet *out_set) {
    VkDescriptorPoolSize ps;
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = n;
    VkDescriptorPoolCreateInfo dpci;
    memset(&dpci, 0, sizeof(dpci));
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    VkResult r = vkCreateDescriptorPool(e->device, &dpci, NULL, out_pool);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkCreateDescriptorPool", r);
        return 0;
    }
    VkDescriptorSetAllocateInfo dsai;
    memset(&dsai, 0, sizeof(dsai));
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = *out_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    r = vkAllocateDescriptorSets(e->device, &dsai, out_set);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkAllocateDescriptorSets", r);
        vkDestroyDescriptorPool(e->device, *out_pool, NULL);
        return 0;
    }
    VkDescriptorBufferInfo bi[16];
    VkWriteDescriptorSet w[16];
    memset(w, 0, sizeof(w));
    for (uint32_t i = 0; i < n; i++) {
        bi[i].buffer = buffers[i].buf;
        bi[i].offset = 0;
        bi[i].range = VK_WHOLE_SIZE;
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = *out_set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(e->device, n, w, 0, NULL);
    return 1;
}

/* Record and submit a single dispatch, blocking on a fence until it completes.
 * A COMPUTE_SHADER -> HOST barrier makes shader writes visible to a later map. */
static int vk_dispatch(lrt_vk_engine *e, const vk_pipeline *p,
                       VkDescriptorSet set, const void *push, uint32_t push_size,
                       uint32_t groups_x) {
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = e->cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VkResult r = vkAllocateCommandBuffers(e->device, &cbai, &cb);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkAllocateCommandBuffers", r);
        return 0;
    }
    VkCommandBufferBeginInfo bbi;
    memset(&bbi, 0, sizeof(bbi));
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bbi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1,
                            &set, 0, NULL);
    if (push_size)
        vkCmdPushConstants(cb, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           push_size, push);
    vkCmdDispatch(cb, groups_x, 1, 1);
    VkMemoryBarrier mb;
    memset(&mb, 0, sizeof(mb));
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);

    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    r = vkCreateFence(e->device, &fci, NULL, &fence);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkCreateFence", r);
        vkFreeCommandBuffers(e->device, e->cmd_pool, 1, &cb);
        return 0;
    }
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    r = vkQueueSubmit(e->queue, 1, &si, fence);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkQueueSubmit", r);
        vkDestroyFence(e->device, fence, NULL);
        vkFreeCommandBuffers(e->device, e->cmd_pool, 1, &cb);
        return 0;
    }
    r = vkWaitForFences(e->device, 1, &fence, VK_TRUE, ~0ULL);
    vkDestroyFence(e->device, fence, NULL);
    vkFreeCommandBuffers(e->device, e->cmd_pool, 1, &cb);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkWaitForFences", r);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Engine create / destroy.                                                  */
/* ------------------------------------------------------------------------- */
static int vk_has_ext(const VkExtensionProperties *props, uint32_t n,
                      const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(props[i].extensionName, name) == 0) return 1;
    return 0;
}

lrt_vk_engine *lrt_vk_engine_create(const lrt_vk_engine_options *opts,
                                    lrt_result *err) {
    lrt_vk_engine_options o;
    if (opts) {
        o = *opts;
    } else {
        memset(&o, 0, sizeof(o));
        o.device_index = -1;
    }

    if (!vkewInit()) {
        if (err) *err = LRT_RESULT_NOT_BUILT;
        return NULL;
    }

    lrt_vk_engine *e = (lrt_vk_engine *)calloc(1, sizeof(*e));
    if (!e) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return NULL;
    }

    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "lightrt";
    app.pEngineName = "lightrt";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkResult r = vkCreateInstance(&ici, NULL, &e->instance);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkCreateInstance", r);
        if (err) *err = LRT_RESULT_NOT_BUILT;
        free(e);
        return NULL;
    }
    if (!vkewLoadInstance(e->instance)) {
        vk_set_err(e, vkewGetError());
        vkDestroyInstance(e->instance, NULL);
        if (err) *err = LRT_RESULT_NOT_BUILT;
        free(e);
        return NULL;
    }

    /* Pick a physical device with a compute queue. */
    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(e->instance, &ndev, NULL);
    if (ndev == 0) {
        vk_set_err(e, "no Vulkan physical devices");
        vkDestroyInstance(e->instance, NULL);
        if (err) *err = LRT_RESULT_NOT_BUILT;
        free(e);
        return NULL;
    }
    if (ndev > 16) ndev = 16;
    VkPhysicalDevice devs[16];
    vkEnumeratePhysicalDevices(e->instance, &ndev, devs);

    int chosen = -1;
    uint32_t chosen_qf = 0;
    int chosen_is_discrete = 0;
    for (uint32_t d = 0; d < ndev; d++) {
        if (o.device_index >= 0 && (uint32_t)o.device_index != d) continue;
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, NULL);
        if (nq == 0) continue;
        if (nq > 32) nq = 32;
        VkQueueFamilyProperties qf[32];
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, qf);
        int qfi = -1;
        for (uint32_t q = 0; q < nq; q++) {
            if (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                qfi = (int)q;
                break;
            }
        }
        if (qfi < 0) continue;
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(devs[d], &pp);
        int is_discrete = (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
        if (chosen < 0 || (o.prefer_discrete && is_discrete && !chosen_is_discrete)) {
            chosen = (int)d;
            chosen_qf = (uint32_t)qfi;
            chosen_is_discrete = is_discrete;
            snprintf(e->device_name, sizeof(e->device_name), "%s", pp.deviceName);
            if (!o.prefer_discrete) break;
        }
    }
    if (chosen < 0) {
        vk_set_err(e, "no compute-capable Vulkan device");
        vkDestroyInstance(e->instance, NULL);
        if (err) *err = LRT_RESULT_NOT_BUILT;
        free(e);
        return NULL;
    }
    e->phys = devs[chosen];
    e->queue_family = chosen_qf;
    vkGetPhysicalDeviceMemoryProperties(e->phys, &e->mem_props);

    /* Detect ray-tracing extensions (loader hooks only in v1). */
    int want_rt = o.want_ray_tracing;
    int have_as = 0, have_rq = 0, have_bda = 0;
    if (want_rt) {
        uint32_t nx = 0;
        vkEnumerateDeviceExtensionProperties(e->phys, NULL, &nx, NULL);
        if (nx > 0) {
            VkExtensionProperties *xs =
                (VkExtensionProperties *)calloc(nx, sizeof(*xs));
            if (xs) {
                vkEnumerateDeviceExtensionProperties(e->phys, NULL, &nx, xs);
                have_as = vk_has_ext(xs, nx, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
                have_rq = vk_has_ext(xs, nx, VK_KHR_RAY_QUERY_EXTENSION_NAME);
                have_bda = vk_has_ext(xs, nx, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
                free(xs);
            }
        }
    }
    int enable_rt = want_rt && have_as && have_rq;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = e->queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    /* Optional RT feature chain. */
    VkPhysicalDeviceRayQueryFeaturesKHR rqf;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf;
    VkPhysicalDeviceVulkan12Features v12;
    VkPhysicalDeviceFeatures2 f2;
    memset(&rqf, 0, sizeof(rqf));
    memset(&asf, 0, sizeof(asf));
    memset(&v12, 0, sizeof(v12));
    memset(&f2, 0, sizeof(f2));
    const char *rt_exts[6];
    uint32_t n_rt_exts = 0;
    const void *device_pnext = NULL;
    if (enable_rt) {
        rqf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        rqf.rayQuery = VK_TRUE;
        asf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        asf.accelerationStructure = VK_TRUE;
        asf.pNext = &rqf;
        v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        v12.bufferDeviceAddress = VK_TRUE;
        v12.descriptorIndexing = VK_TRUE;
        v12.scalarBlockLayout = VK_TRUE;
        v12.pNext = &asf;
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &v12;
        device_pnext = &f2;
        rt_exts[n_rt_exts++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
        rt_exts[n_rt_exts++] = VK_KHR_RAY_QUERY_EXTENSION_NAME;
        rt_exts[n_rt_exts++] = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
        rt_exts[n_rt_exts++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
        rt_exts[n_rt_exts++] = VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME;
        rt_exts[n_rt_exts++] = VK_KHR_SPIRV_1_4_EXTENSION_NAME;
        (void)have_bda;
    }

    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = device_pnext;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = n_rt_exts;
    dci.ppEnabledExtensionNames = n_rt_exts ? rt_exts : NULL;
    r = vkCreateDevice(e->phys, &dci, NULL, &e->device);
    if (r != VK_SUCCESS && enable_rt) {
        /* Retry compute-only. */
        memset(&dci, 0, sizeof(dci));
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        enable_rt = 0;
        r = vkCreateDevice(e->phys, &dci, NULL, &e->device);
    }
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkCreateDevice", r);
        vkDestroyInstance(e->instance, NULL);
        if (err) *err = LRT_RESULT_NOT_BUILT;
        free(e);
        return NULL;
    }
    if (!vkewLoadDevice(e->device)) {
        vk_set_err(e, vkewGetError());
        vkDestroyDevice(e->device, NULL);
        vkDestroyInstance(e->instance, NULL);
        if (err) *err = LRT_RESULT_NOT_BUILT;
        free(e);
        return NULL;
    }

    vkGetDeviceQueue(e->device, e->queue_family, 0, &e->queue);

    VkCommandPoolCreateInfo cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = e->queue_family;
    r = vkCreateCommandPool(e->device, &cpci, NULL, &e->cmd_pool);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkCreateCommandPool", r);
        vkDestroyDevice(e->device, NULL);
        vkDestroyInstance(e->instance, NULL);
        if (err) *err = LRT_RESULT_NOT_BUILT;
        free(e);
        return NULL;
    }

    e->caps = LRT_VK_CAP_COMPUTE;
    /* Reflect what actually loaded, not just what was enumerated. */
    if (vkGetBufferDeviceAddressKHR) e->caps |= LRT_VK_CAP_BUFFER_ADDRESS;
    if (vkCreateAccelerationStructureKHR) e->caps |= LRT_VK_CAP_ACCEL_STRUCT;
    if (enable_rt) e->caps |= LRT_VK_CAP_RAY_QUERY;

    vk_set_err(e, "");
    if (err) *err = LRT_RESULT_OK;
    return e;
}

void lrt_vk_engine_destroy(lrt_vk_engine *e) {
    if (!e) return;
    /* On a lost/corrupted device (e.g. a build ran the GPU out of memory) the
     * NVIDIA driver segfaults inside vkDestroy / vkDeviceWaitIdle, so skip ALL
     * driver teardown and just free the host handle -- the OS reclaims the GPU
     * objects when the process exits. */
    if (e->device && !e->device_lost) {
        vkDeviceWaitIdle(e->device);
        for (int i = 0; i < VK_TRACE_PIPE_CACHE; i++)
            vk_pipeline_destroy(e, &e->trace_pipes[i]);
        vk_pipeline_destroy(e, &e->build_pipe);
        vk_pipeline_destroy(e, &e->rtx_pipe);
        vk_pipeline_destroy(e, &e->rtx_pipe_wide);
        vk_pipeline_destroy(e, &e->shade_pipe);
        vk_pipeline_destroy(e, &e->material_path_pipe);
        if (e->cmd_pool) vkDestroyCommandPool(e->device, e->cmd_pool, NULL);
        vkDestroyDevice(e->device, NULL);
    }
    if (e->instance && !e->device_lost) vkDestroyInstance(e->instance, NULL);
    free(e);
}

uint32_t lrt_vk_engine_caps(const lrt_vk_engine *e) {
    return e ? e->caps : 0u;
}

const char *lrt_vk_engine_device_name(const lrt_vk_engine *e) {
    return e ? e->device_name : "";
}

const char *lrt_vk_engine_last_error(const lrt_vk_engine *e) {
    return e ? e->err : "";
}

uint64_t lrt_vk_device_local_bytes(int prefer_discrete) {
    if (!vkewInit()) return 0;
    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "lightrt";
    app.pEngineName = "lightrt";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) return 0;
    if (!vkewLoadInstance(inst)) {
        vkDestroyInstance(inst, NULL);
        return 0;
    }
    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(inst, &ndev, NULL);
    if (ndev == 0) {
        vkDestroyInstance(inst, NULL);
        return 0;
    }
    if (ndev > 16) ndev = 16;
    VkPhysicalDevice devs[16];
    vkEnumeratePhysicalDevices(inst, &ndev, devs);
    uint64_t best = 0;
    int best_discrete = 0;
    for (uint32_t d = 0; d < ndev; d++) {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(devs[d], &pp);
        int is_discrete = (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(devs[d], &mp);
        uint64_t local = 0;
        for (uint32_t h = 0; h < mp.memoryHeapCount; h++) {
            if ((mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
                mp.memoryHeaps[h].size > local) {
                local = mp.memoryHeaps[h].size;
            }
        }
        /* Prefer a discrete GPU's VRAM; otherwise take the largest local heap. */
        if ((prefer_discrete && is_discrete && !best_discrete) ||
            (is_discrete == best_discrete && local > best)) {
            best = local;
            best_discrete = is_discrete;
        }
    }
    vkDestroyInstance(inst, NULL);
    return best;
}

/* ------------------------------------------------------------------------- */
/* Path A: CPU build -> GPU trace.                                           */
/* ------------------------------------------------------------------------- */
typedef struct trace_push {
    uint32_t root;
    uint32_t node_count;
    uint32_t block_count;
    uint32_t ray_count;
    uint32_t prim_kind;
    uint32_t point_type;
} trace_push;

/* Bucket the traversal stack depth so pipelines are reused across scenes. */
static uint32_t trace_stack_for(uint32_t max_depth, uint32_t w) {
    uint32_t need = max_depth * (w - 1u) + w + 1u;
    static const uint32_t buckets[] = {32u, 64u, 128u, 256u};
    for (int i = 0; i < 4; i++)
        if (need <= buckets[i]) return buckets[i];
    return 0; /* too deep for the compute path */
}

static vk_pipeline *trace_get_pipeline(lrt_vk_engine *e, uint32_t w,
                                       uint32_t stack) {
    for (int i = 0; i < VK_TRACE_PIPE_CACHE; i++) {
        vk_pipeline *p = &e->trace_pipes[i];
        if (p->valid && p->key_w == w && p->key_stack == stack) return p;
    }
    vk_pipeline *slot = NULL;
    for (int i = 0; i < VK_TRACE_PIPE_CACHE; i++) {
        if (!e->trace_pipes[i].valid) {
            slot = &e->trace_pipes[i];
            break;
        }
    }
    if (!slot) { /* cache full: recycle slot 0 */
        vk_pipeline_destroy(e, &e->trace_pipes[0]);
        slot = &e->trace_pipes[0];
    }
    uint32_t spec_data[2] = {w, stack};
    VkSpecializationMapEntry ents[2];
    ents[0].constantID = 0;
    ents[0].offset = 0;
    ents[0].size = sizeof(uint32_t);
    ents[1].constantID = 1;
    ents[1].offset = sizeof(uint32_t);
    ents[1].size = sizeof(uint32_t);
    VkSpecializationInfo spec;
    spec.mapEntryCount = 2;
    spec.pMapEntries = ents;
    spec.dataSize = sizeof(spec_data);
    spec.pData = spec_data;
    if (!vk_pipeline_build(e, trace_bvh_spv, sizeof(trace_bvh_spv), 4, NULL,
                           (uint32_t)sizeof(trace_push), &spec, slot))
        return NULL;
    slot->key_w = w;
    slot->key_stack = stack;
    return slot;
}

static VkCommandBuffer vk_cmd_begin(lrt_vk_engine *e);
static int vk_cmd_end_submit(lrt_vk_engine *e, VkCommandBuffer cb);

/* DEVICE-LOCAL storage buffer filled through a staging copy. The BVH nodes and
 * blocks are re-read at every traversal step, in an access pattern that is by
 * nature scattered; keeping them in host-visible memory means every one of those
 * reads crosses PCIe. They are written once and read billions of times, which is
 * exactly the case device-local memory exists for.
 *
 * Returns 0 without touching *out on failure, so the caller can fall back to a
 * host-visible buffer (an integrated GPU may have no device-local-only heap, and
 * a discrete one can simply be out of VRAM). */
static int vk_buffer_create_device_filled(lrt_vk_engine *e, VkDeviceSize size,
                                          const void *src, size_t bytes,
                                          vk_buffer *out) {
    vk_buffer dev, stage;
    memset(&stage, 0, sizeof(stage));
    if (!vk_buffer_create_ex(e, size,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             0, 0, &dev))
        return 0;
    if (!vk_buffer_create_ex(e, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1, 0,
                             &stage)) {
        vk_buffer_destroy(e, &dev);
        return 0;
    }
    if (!vk_buffer_write(e, &stage, src, bytes)) goto fail;

    VkCommandBuffer cb = vk_cmd_begin(e);
    if (cb == VK_NULL_HANDLE) goto fail;
    VkBufferCopy region;
    memset(&region, 0, sizeof(region));
    region.size = size;
    vkCmdCopyBuffer(cb, stage.buf, dev.buf, 1, &region);
    /* The copy runs in its own submit; without this the trace dispatch may read
     * the buffer before the transfer writes are available to shader reads.
     * (vkew exposes VkMemoryBarrier but not VkBufferMemoryBarrier; a global
     * barrier is stricter than needed and equally correct here.) */
    VkMemoryBarrier mb;
    memset(&mb, 0, sizeof(mb));
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         NULL, 0, NULL);
    if (!vk_cmd_end_submit(e, cb)) goto fail;

    vk_buffer_destroy(e, &stage);
    *out = dev;
    return 1;
fail:
    vk_buffer_destroy(e, &stage);
    vk_buffer_destroy(e, &dev);
    return 0;
}

/* Nodes/blocks buffer. OPT-IN device-local (LRT_VK_DEVICE_LOCAL=1).
 *
 * Device-local was the obvious optimization and it does not pay off, so it is
 * not the default. Measured on ALab (29.2 M tris; 297 MiB of nodes + 1.6 GiB of
 * blocks, both confirmed device-local), RTX 5060 Ti:
 *
 *   per-ray trace cost   0.144 us host-visible   0.144 us device-local
 *   trace, 16.7 M rays   4.65 s                  4.92 s  (the staging copy)
 *
 * i.e. traversal is not bound by where the BVH lives -- and moving it into VRAM
 * costs ~1.9 GiB of the very budget this work exists to reclaim. Kept behind the
 * flag because the reasoning still holds for a GPU whose traversal IS bandwidth
 * bound; measure before turning it on. */
static int vk_bvh_buffer(lrt_vk_engine *e, VkDeviceSize size, const void *src,
                         size_t bytes, vk_buffer *out) {
    const char *ev = getenv("LRT_VK_DEVICE_LOCAL");
    if (ev && ev[0] == '1' &&
        vk_buffer_create_device_filled(e, size, src, bytes, out)) {
        if (getenv("LRT_VK_VERBOSE"))
            fprintf(stderr, "[lrt_vk] BVH buffer %.1f MiB device-local\n",
                    (double)size / (1024.0 * 1024.0));
        return 1;
    }
    if (getenv("LRT_VK_VERBOSE"))
        fprintf(stderr, "[lrt_vk] BVH buffer %.1f MiB host-visible\n",
                (double)size / (1024.0 * 1024.0));
    return vk_buffer_create(e, size, out) && vk_buffer_write(e, out, src, bytes);
}

/* Rays traced per dispatch. The whole frame used to be one dispatch, so the ray
 * + hit buffers alone were 48 B * width * height * spp -- 6.3 GB of VRAM for a
 * 1920x1080x64spp frame, on top of the BVH. Tracing in tiles bounds that to a
 * fixed working set (default 4 M rays = 128 MiB rays + 64 MiB hits) at no cost:
 * the rays in a tile are independent, so the results are identical.
 * LRT_VK_RAY_TILE overrides (0 = no tiling, the old whole-frame dispatch). */
static uint32_t vk_ray_tile_size(void) {
    const char *ev = getenv("LRT_VK_RAY_TILE");
    if (ev) {
        char *end = NULL;
        unsigned long v = strtoul(ev, &end, 10);
        if (end != ev) return v > 0xffffffffUL ? 0xffffffffu : (uint32_t)v;
    }
    return 4u << 20;
}

int lrt_vk_trace_scene(lrt_vk_engine *e, const lrt_tri_scene *s,
                       const lrt_ray *rays, uint32_t n, lrt_hit *out,
                       lrt_result *err) {
    if (!e || !s || (n && (!rays || !out))) {
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return -1;
    }
    if (n == 0) {
        if (err) *err = LRT_RESULT_OK;
        return 0;
    }

    void *blob = NULL;
    size_t blob_n = 0;
    lrt_result sr = lrt_tri_scene_save_to_memory(s, &blob, &blob_n);
    if (sr != LRT_RESULT_OK) {
        vk_set_err(e, "scene not GPU-traceable (quantized/curve/user?)");
        if (err) *err = sr;
        return -1;
    }
    const vk_lrts_header *h = (const vk_lrts_header *)blob;
    /* The trace shader supports triangles and serialized point leaves. */
    if (h->prim_kind != 0u /* TRI_PRIM_TRI */ &&
        h->prim_kind != 6u /* TRI_PRIM_POINT */) {
        free(blob);
        vk_set_err(e, "only triangle scenes are GPU-traceable");
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return -1;
    }
    uint32_t w = h->layout; /* 4 or 8 */

    lrt_tri_stats st;
    lrt_tri_scene_stats(s, &st);
    uint32_t stack = trace_stack_for(st.max_depth, w);
    if (stack == 0) {
        vk_set_err(e, "BVH too deep for the GPU compute stack");
        free(blob);
        if (err) *err = LRT_RESULT_TRAVERSAL_OVERFLOW;
        return -1;
    }

    vk_pipeline *pipe = trace_get_pipeline(e, w, stack);
    if (!pipe) {
        free(blob);
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return -1;
    }

    size_t nodes_bytes = (size_t)h->node_count * h->node_stride;
    size_t blocks_bytes = (size_t)h->block_count * h->block_stride;
    uint32_t tile = vk_ray_tile_size();
    if (tile == 0 || tile > n) tile = n;
    vk_buffer b_nodes, b_blocks, b_rays, b_hits;
    memset(&b_nodes, 0, sizeof(b_nodes));
    memset(&b_blocks, 0, sizeof(b_blocks));
    memset(&b_rays, 0, sizeof(b_rays));
    memset(&b_hits, 0, sizeof(b_hits));
    int ok = vk_bvh_buffer(e, nodes_bytes,
                           (const char *)blob + h->node_offset, nodes_bytes,
                           &b_nodes) &&
             vk_bvh_buffer(e, blocks_bytes,
                           (const char *)blob + h->block_offset, blocks_bytes,
                           &b_blocks) &&
             /* Rays and hits stay host-visible: each is touched exactly once by
              * the shader, so a staging round-trip would only add a copy. Only
              * the tile is resident, not the frame. */
             vk_buffer_create(e, (VkDeviceSize)tile * sizeof(lrt_ray), &b_rays) &&
             vk_buffer_create(e, (VkDeviceSize)tile * sizeof(lrt_hit), &b_hits);
    if (!ok) goto fail_buffers;

    {
        vk_buffer set_bufs[4] = {b_nodes, b_blocks, b_rays, b_hits};
        VkDescriptorPool pool;
        VkDescriptorSet set;
        if (!vk_descriptors_bind(e, pipe->dsl, set_bufs, 4, &pool, &set))
            goto fail_buffers;
        int run_ok = 1;
        for (uint32_t off = 0; off < n && run_ok; off += tile) {
            uint32_t cnt = n - off < tile ? n - off : tile;
            if (!vk_buffer_write(e, &b_rays, rays + off,
                                 (size_t)cnt * sizeof(lrt_ray))) {
                run_ok = 0;
                break;
            }
            trace_push pc;
            pc.root = h->root;
            pc.node_count = h->node_count;
            pc.block_count = h->block_count;
            pc.ray_count = cnt;
            pc.prim_kind = h->prim_kind;
            pc.point_type = h->reserved0;
            uint32_t groups = (cnt + 63u) / 64u;
            run_ok = vk_dispatch(e, pipe, set, &pc, (uint32_t)sizeof(pc), groups) &&
                     vk_buffer_read(e, &b_hits, out + off,
                                    (size_t)cnt * sizeof(lrt_hit));
        }
        vkDestroyDescriptorPool(e->device, pool, NULL);
        if (!run_ok) goto fail_buffers;
    }

    vk_buffer_destroy(e, &b_nodes);
    vk_buffer_destroy(e, &b_blocks);
    vk_buffer_destroy(e, &b_rays);
    vk_buffer_destroy(e, &b_hits);
    free(blob);

    int hits = 0;
    for (uint32_t i = 0; i < n; i++)
        if (out[i].prim_id != LRT_TRI_NO_HIT) hits++;
    if (err) *err = LRT_RESULT_OK;
    return hits;

fail_buffers:
    vk_buffer_destroy(e, &b_nodes);
    vk_buffer_destroy(e, &b_blocks);
    vk_buffer_destroy(e, &b_rays);
    vk_buffer_destroy(e, &b_hits);
    free(blob);
    if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Path B: GPU build front end -> CPU trace.                                 */
/* ------------------------------------------------------------------------- */
typedef struct morton_push {
    uint32_t pass;
    uint32_t ntris;
    float base0, base1, base2;
    float scale0, scale1, scale2;
} morton_push;

static vk_pipeline *build_get_pipeline(lrt_vk_engine *e) {
    if (e->build_pipe.valid) return &e->build_pipe;
    if (!vk_pipeline_build(e, build_morton_spv, sizeof(build_morton_spv), 3, NULL,
                           (uint32_t)sizeof(morton_push), NULL, &e->build_pipe))
        return NULL;
    return &e->build_pipe;
}

int lrt_vk_build_scene(lrt_vk_engine *e, const float *vertices, uint32_t ntris,
                       lrt_tri_layout layout, lrt_tri_scene **out,
                       lrt_result *err) {
    if (!e || !vertices || ntris == 0 || !out) {
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return -1;
    }
    if (layout != LRT_TRI_LAYOUT_BVH4 && layout != LRT_TRI_LAYOUT_BVH8) {
        vk_set_err(e, "Path B supports BVH4/BVH8 only");
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return -1;
    }
    *out = NULL;

    vk_pipeline *pipe = build_get_pipeline(e);
    if (!pipe) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return -1;
    }

    vk_buffer b_verts, b_cen, b_morton;
    int ok = vk_buffer_create(e, (VkDeviceSize)ntris * 9u * sizeof(float),
                              &b_verts) &&
             vk_buffer_create(e, (VkDeviceSize)ntris * 3u * sizeof(float),
                              &b_cen) &&
             vk_buffer_create(e, (VkDeviceSize)ntris * sizeof(uint32_t),
                              &b_morton);
    uint32_t *morton = NULL;
    float *cen = NULL;
    if (!ok) goto fail;
    if (!vk_buffer_write(e, &b_verts, vertices, (size_t)ntris * 9u * sizeof(float)))
        goto fail;

    {
        vk_buffer set_bufs[3] = {b_verts, b_cen, b_morton};
        VkDescriptorPool pool;
        VkDescriptorSet set;
        if (!vk_descriptors_bind(e, pipe->dsl, set_bufs, 3, &pool, &set))
            goto fail;
        uint32_t groups = (ntris + 63u) / 64u;

        /* Pass 0: centroids. */
        morton_push pc;
        memset(&pc, 0, sizeof(pc));
        pc.pass = 0;
        pc.ntris = ntris;
        int run_ok = vk_dispatch(e, pipe, set, &pc, (uint32_t)sizeof(pc), groups);
        if (!run_ok) {
            vkDestroyDescriptorPool(e->device, pool, NULL);
            goto fail;
        }

        /* Read centroids; CPU reduces the centroid AABB (matches
         * tri_morton_encode) and derives base/scale. */
        cen = (float *)malloc((size_t)ntris * 3u * sizeof(float));
        if (!cen) {
            vkDestroyDescriptorPool(e->device, pool, NULL);
            goto fail;
        }
        if (!vk_buffer_read(e, &b_cen, cen, (size_t)ntris * 3u * sizeof(float))) {
            vkDestroyDescriptorPool(e->device, pool, NULL);
            goto fail;
        }
        float clo[3] = {3.402823466e+38f, 3.402823466e+38f, 3.402823466e+38f};
        float chi[3] = {-3.402823466e+38f, -3.402823466e+38f, -3.402823466e+38f};
        for (uint32_t i = 0; i < ntris; i++) {
            for (int a = 0; a < 3; a++) {
                float v = cen[(size_t)i * 3 + a];
                if (v < clo[a]) clo[a] = v;
                if (v > chi[a]) chi[a] = v;
            }
        }
        pc.pass = 1;
        for (int a = 0; a < 3; a++) {
            float ext = chi[a] - clo[a];
            float base = clo[a];
            float scale = ext > 0.0f ? 1024.0f / ext : 0.0f;
            if (a == 0) { pc.base0 = base; pc.scale0 = scale; }
            else if (a == 1) { pc.base1 = base; pc.scale1 = scale; }
            else { pc.base2 = base; pc.scale2 = scale; }
        }

        /* Pass 1: Morton codes. */
        run_ok = vk_dispatch(e, pipe, set, &pc, (uint32_t)sizeof(pc), groups);
        vkDestroyDescriptorPool(e->device, pool, NULL);
        if (!run_ok) goto fail;

        morton = (uint32_t *)malloc((size_t)ntris * sizeof(uint32_t));
        if (!morton) goto fail;
        if (!vk_buffer_read(e, &b_morton, morton,
                            (size_t)ntris * sizeof(uint32_t)))
            goto fail;
    }

    vk_buffer_destroy(e, &b_verts);
    vk_buffer_destroy(e, &b_cen);
    vk_buffer_destroy(e, &b_morton);
    free(cen);

    /* Finish the LBVH on the CPU using the GPU-computed Morton codes. */
    lrt_result br = LRT_RESULT_OK;
    lrt_tri_scene *scene =
        lrt_tri_scene_build_lbvh_morton(vertices, ntris, morton, layout, 0, &br);
    free(morton);
    if (!scene) {
        vk_set_err(e, "CPU LBVH finish failed");
        if (err) *err = br;
        return -1;
    }
    *out = scene;
    if (err) *err = LRT_RESULT_OK;
    return 0;

fail:
    vk_buffer_destroy(e, &b_verts);
    vk_buffer_destroy(e, &b_cen);
    vk_buffer_destroy(e, &b_morton);
    free(cen);
    free(morton);
    if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Analytic-primitive GPU shading (spheres + boxes).                          */
/* ------------------------------------------------------------------------- */
typedef struct shade_push {
    uint32_t width;
    uint32_t height;
    uint32_t nprims;
    uint32_t spp;
} shade_push;

static vk_pipeline *shade_get_pipeline(lrt_vk_engine *e) {
    if (e->shade_pipe.valid) return &e->shade_pipe;
    if (!vk_pipeline_build(e, shade_analytic_spv, sizeof(shade_analytic_spv), 3,
                           NULL, (uint32_t)sizeof(shade_push), NULL,
                           &e->shade_pipe))
        return NULL;
    return &e->shade_pipe;
}

/* Pack the public desc into the 32-float "globals" buffer the shader reads. */
static void shade_pack_globals(const lrt_vk_shade_desc *d, float *g) {
    float aspect = d->aspect;
    if (aspect == 0.0f && d->height) aspect = (float)d->width / (float)d->height;
    memset(g, 0, 32 * sizeof(float));
    g[0] = d->cam_origin[0];  g[1] = d->cam_origin[1];  g[2] = d->cam_origin[2];
    g[3] = d->tan_half_fov;
    g[4] = d->cam_forward[0]; g[5] = d->cam_forward[1]; g[6] = d->cam_forward[2];
    g[7] = aspect;
    g[8] = d->cam_right[0];   g[9] = d->cam_right[1];   g[10] = d->cam_right[2];
    g[12] = d->cam_up[0];     g[13] = d->cam_up[1];     g[14] = d->cam_up[2];
    g[16] = d->sun_dir[0];    g[17] = d->sun_dir[1];    g[18] = d->sun_dir[2];
    g[20] = d->sun_radiance[0]; g[21] = d->sun_radiance[1]; g[22] = d->sun_radiance[2];
    g[24] = d->env_top[0];    g[25] = d->env_top[1];    g[26] = d->env_top[2];
    g[28] = d->env_bottom[0]; g[29] = d->env_bottom[1]; g[30] = d->env_bottom[2];
}

int lrt_vk_shade_analytic(lrt_vk_engine *e, const lrt_vk_shade_prim *prims,
                          uint32_t nprims, const lrt_vk_shade_desc *desc,
                          float *out_rgba, lrt_result *err) {
    if (!e || !desc || !out_rgba || (nprims && !prims) ||
        desc->width == 0 || desc->height == 0) {
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return -1;
    }

    vk_pipeline *pipe = shade_get_pipeline(e);
    if (!pipe) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return -1;
    }

    uint32_t npix = desc->width * desc->height;
    size_t prim_bytes = (size_t)nprims * sizeof(lrt_vk_shade_prim);
    size_t out_bytes = (size_t)npix * 4u * sizeof(float);

    /* lrt_vk_shade_prim is 16 tightly-packed floats == the shader's stride. */
    vk_buffer b_prims, b_out, b_glob;
    int ok = vk_buffer_create(e, prim_bytes, &b_prims) &&
             vk_buffer_create(e, out_bytes, &b_out) &&
             vk_buffer_create(e, 32u * sizeof(float), &b_glob);
    if (!ok) goto fail;

    {
        float glob[32];
        shade_pack_globals(desc, glob);
        if ((prim_bytes && !vk_buffer_write(e, &b_prims, prims, prim_bytes)) ||
            !vk_buffer_write(e, &b_glob, glob, sizeof(glob)))
            goto fail;

        vk_buffer set_bufs[3] = {b_prims, b_out, b_glob};
        VkDescriptorPool pool;
        VkDescriptorSet set;
        if (!vk_descriptors_bind(e, pipe->dsl, set_bufs, 3, &pool, &set))
            goto fail;

        shade_push pc;
        pc.width = desc->width;
        pc.height = desc->height;
        pc.nprims = nprims;
        pc.spp = desc->spp ? desc->spp : 1u;
        uint32_t groups = (npix + 63u) / 64u;
        int run_ok = vk_dispatch(e, pipe, set, &pc, (uint32_t)sizeof(pc), groups);
        vkDestroyDescriptorPool(e->device, pool, NULL);
        if (!run_ok) goto fail;
        if (!vk_buffer_read(e, &b_out, out_rgba, out_bytes)) goto fail;
    }

    vk_buffer_destroy(e, &b_prims);
    vk_buffer_destroy(e, &b_out);
    vk_buffer_destroy(e, &b_glob);
    if (err) *err = LRT_RESULT_OK;
    return 0;

fail:
    vk_buffer_destroy(e, &b_prims);
    vk_buffer_destroy(e, &b_out);
    vk_buffer_destroy(e, &b_glob);
    if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
    return -1;
}

/* Resident scene: prims uploaded once, buffers + descriptor set reused. The
 * output is DEVICE_LOCAL (the shader writes at full bandwidth, not over PCIe to
 * host-coherent memory) and copied to a host-visible staging buffer for
 * readback in the same submission. */
struct lrt_vk_shade_scene {
    vk_buffer prims;
    vk_buffer glob;
    vk_buffer out;           /* DEVICE_LOCAL: STORAGE | TRANSFER_SRC      */
    vk_buffer staging;       /* HOST_CACHED:  TRANSFER_DST (readback)     */
    void     *staging_ptr;   /* persistent map of staging (no per-frame map) */
    uint32_t nprims;
    uint32_t out_pixels;     /* current output-buffer capacity, in pixels */
    VkDescriptorPool pool;
    VkDescriptorSet set;
    int bound;
    VkCommandBuffer cmd;     /* reused across frames (implicit reset on begin) */
    VkFence fence;           /* reused across frames (reset each submit)       */
    int have_cmd;
};

lrt_vk_shade_scene *lrt_vk_shade_scene_build(lrt_vk_engine *e,
                                             const lrt_vk_shade_prim *prims,
                                             uint32_t nprims, lrt_result *err) {
    if (!e || (nprims && !prims)) {
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return NULL;
    }
    if (!shade_get_pipeline(e)) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return NULL;
    }
    lrt_vk_shade_scene *s =
        (lrt_vk_shade_scene *)calloc(1, sizeof(*s));
    if (!s) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return NULL;
    }
    s->nprims = nprims;
    size_t prim_bytes = (size_t)nprims * sizeof(lrt_vk_shade_prim);
    if (!vk_buffer_create(e, prim_bytes, &s->prims) ||
        !vk_buffer_create(e, 32u * sizeof(float), &s->glob)) {
        lrt_vk_shade_scene_free(e, s);
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return NULL;
    }
    if (prim_bytes && !vk_buffer_write(e, &s->prims, prims, prim_bytes)) {
        lrt_vk_shade_scene_free(e, s);
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return NULL;
    }

    /* Persistent command buffer + fence, reused every frame. */
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = e->cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkAllocateCommandBuffers(e->device, &cbai, &s->cmd) != VK_SUCCESS ||
        vkCreateFence(e->device, &fci, NULL, &s->fence) != VK_SUCCESS) {
        vk_set_err(e, "command buffer / fence allocation failed");
        lrt_vk_shade_scene_free(e, s);
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return NULL;
    }
    s->have_cmd = 1;

    if (err) *err = LRT_RESULT_OK;
    return s;
}

int lrt_vk_shade_scene_render(lrt_vk_engine *e, lrt_vk_shade_scene *s,
                              const lrt_vk_shade_desc *desc, float *out_rgba,
                              lrt_result *err) {
    if (!e || !s || !desc || !out_rgba || desc->width == 0 ||
        desc->height == 0) {
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return -1;
    }
    vk_pipeline *pipe = shade_get_pipeline(e);
    if (!pipe) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return -1;
    }

    uint32_t npix = desc->width * desc->height;
    size_t out_bytes = (size_t)npix * 4u * sizeof(float);

    /* Grow (and rebind) the output buffers only when a larger frame appears.
     * The host-visible staging buffer is mapped once and kept mapped. */
    if (npix > s->out_pixels) {
        if (s->bound) {
            vkDestroyDescriptorPool(e->device, s->pool, NULL);
            s->bound = 0;
        }
        if (s->staging_ptr) {
            vkUnmapMemory(e->device, s->staging.mem);
            s->staging_ptr = NULL;
        }
        vk_buffer_destroy(e, &s->out);
        vk_buffer_destroy(e, &s->staging);
        int ok = vk_buffer_create_ex(
                     e, out_bytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     0, 0, &s->out) &&
                 vk_buffer_create_staging(e, out_bytes, &s->staging);
        if (!ok || vkMapMemory(e->device, s->staging.mem, 0, VK_WHOLE_SIZE, 0,
                               &s->staging_ptr) != VK_SUCCESS) {
            vk_buffer_destroy(e, &s->out);
            vk_buffer_destroy(e, &s->staging);
            s->staging_ptr = NULL;
            s->out_pixels = 0;
            if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
            return -1;
        }
        s->out_pixels = npix;
    }
    if (!s->bound) {
        vk_buffer set_bufs[3] = {s->prims, s->out, s->glob};
        if (!vk_descriptors_bind(e, pipe->dsl, set_bufs, 3, &s->pool, &s->set)) {
            if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
            return -1;
        }
        s->bound = 1;
    }

    float glob[32];
    shade_pack_globals(desc, glob);
    if (!vk_buffer_write(e, &s->glob, glob, sizeof(glob))) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return -1;
    }

    shade_push pc;
    pc.width = desc->width;
    pc.height = desc->height;
    pc.nprims = s->nprims;
    pc.spp = desc->spp ? desc->spp : 1u;
    uint32_t groups = (npix + 63u) / 64u;

    /* Record into the reused command buffer (vkBeginCommandBuffer implicitly
     * resets it; the pool has RESET_COMMAND_BUFFER_BIT): dispatch into the
     * device-local output, then copy it to the host-cached staging buffer — one
     * submission, with the two barriers the copy and the host read require. */
    VkCommandBuffer cb = s->cmd;
    VkCommandBufferBeginInfo bbi;
    memset(&bbi, 0, sizeof(bbi));
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bbi) != VK_SUCCESS) {
        vk_set_err(e, "vkBeginCommandBuffer(shade)");
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return -1;
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->layout, 0,
                            1, &s->set, 0, NULL);
    vkCmdPushConstants(cb, pipe->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       (uint32_t)sizeof(pc), &pc);
    vkCmdDispatch(cb, groups, 1, 1);
    VkMemoryBarrier mb;
    memset(&mb, 0, sizeof(mb));
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0,
                         NULL);
    VkBufferCopy region;
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size = out_bytes;
    vkCmdCopyBuffer(cb, s->out.buf, s->staging.buf, 1, &region);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0,
                         NULL);
    vkEndCommandBuffer(cb);

    vkResetFences(e->device, 1, &s->fence);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (vkQueueSubmit(e->queue, 1, &si, s->fence) != VK_SUCCESS ||
        vkWaitForFences(e->device, 1, &s->fence, VK_TRUE, ~0ULL) != VK_SUCCESS) {
        vk_set_err(e, "shade dispatch/copy submission failed");
        if (err) *err = LRT_RESULT_TRAVERSAL_OVERFLOW;
        return -1;
    }
    /* Staging is HOST_COHERENT, so the host read needs no invalidate. */
    memcpy(out_rgba, s->staging_ptr, out_bytes);
    if (err) *err = LRT_RESULT_OK;
    return 0;
}

void lrt_vk_shade_scene_free(lrt_vk_engine *e, lrt_vk_shade_scene *s) {
    if (!s) return;
    if (e->device && !e->device_lost) vkDeviceWaitIdle(e->device);
    if (s->have_cmd) {
        vkFreeCommandBuffers(e->device, e->cmd_pool, 1, &s->cmd);
        vkDestroyFence(e->device, s->fence, NULL);
    }
    if (s->staging_ptr) vkUnmapMemory(e->device, s->staging.mem);
    if (s->bound) vkDestroyDescriptorPool(e->device, s->pool, NULL);
    vk_buffer_destroy(e, &s->prims);
    vk_buffer_destroy(e, &s->glob);
    vk_buffer_destroy(e, &s->out);
    vk_buffer_destroy(e, &s->staging);
    free(s);
}

/* ------------------------------------------------------------------------- */
/* Hardware ray tracing (VK_KHR_ray_query): build a real acceleration         */
/* structure on the GPU and trace it. Separate from Path A/B (the AS is        */
/* vendor-opaque, so it can only serve the trace direction).                  */
/* ------------------------------------------------------------------------- */

static uint64_t vk_device_address(lrt_vk_engine *e, const vk_buffer *b) {
    VkBufferDeviceAddressInfo i;
    memset(&i, 0, sizeof(i));
    i.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    i.buffer = b->buf;
    return (uint64_t)vkGetBufferDeviceAddressKHR(e->device, &i);
}

/* One-shot command buffer helpers (for AS builds; the trace reuses vk_dispatch). */
static VkCommandBuffer vk_cmd_begin(lrt_vk_engine *e) {
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = e->cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb;
    if (vkAllocateCommandBuffers(e->device, &cbai, &cb) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bbi;
    memset(&bbi, 0, sizeof(bbi));
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bbi);
    return cb;
}

static int vk_cmd_end_submit(lrt_vk_engine *e, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    if (vkCreateFence(e->device, &fci, NULL, &fence) != VK_SUCCESS) {
        vkFreeCommandBuffers(e->device, e->cmd_pool, 1, &cb);
        return 0;
    }
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkResult r = vkQueueSubmit(e->queue, 1, &si, fence);
    if (r == VK_SUCCESS) {
        /* 5 second timeout — ~0ULL (infinite) would hang the process on a
         * device-lost or misconfigured AS build. */
        r = vkWaitForFences(e->device, 1, &fence, VK_TRUE, 5000000000ULL);
        if (r == VK_TIMEOUT) {
            vk_set_err(e, "GPU command timed out after 5 s (device lost or "
                          "misconfigured acceleration-structure build)");
            e->device_lost = 1;
            vkDestroyFence(e->device, fence, NULL);
            vkFreeCommandBuffers(e->device, e->cmd_pool, 1, &cb);
            return 0;
        }
    }
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkQueueSubmit/vkWaitForFences", r);
        if (r == VK_ERROR_DEVICE_LOST || r == VK_TIMEOUT) e->device_lost = 1;
        vkDestroyFence(e->device, fence, NULL);
        vkFreeCommandBuffers(e->device, e->cmd_pool, 1, &cb);
        return 0;
    }
    vkDestroyFence(e->device, fence, NULL);
    vkFreeCommandBuffers(e->device, e->cmd_pool, 1, &cb);
    return 1;
}

typedef struct vk_accel {
    VkAccelerationStructureKHR as;
    vk_buffer storage;
    uint64_t address;
} vk_accel;

/* BLAS compaction (default ON, LRT_VK_BLAS_COMPACT=0 opts out): build with
 * ALLOW_COMPACTION into transient full-size storage, read the compacted size
 * from a query, then vkCmdCopyAccelerationStructureKHR (COMPACT) into
 * right-sized storage. Triangle BLAS typically compact to ~50-60%, which is the
 * dominant resident VRAM of large ray-query scenes. Requires the query/copy
 * entry points (device-loaded with VK_KHR_acceleration_structure). */
static int rtx_compact_enabled(void) {
    const char *env = getenv("LRT_VK_BLAS_COMPACT");
    if (env && env[0] == '0') return 0;
    return vkCmdWriteAccelerationStructuresPropertiesKHR != NULL &&
           vkCmdCopyAccelerationStructureKHR != NULL && vkCreateQueryPool != NULL &&
           vkDestroyQueryPool != NULL && vkGetQueryPoolResults != NULL &&
           vkCmdResetQueryPool != NULL;
}

/* Build one acceleration structure (BLAS or TLAS) on the device from a single
 * geometry. Allocates the AS backing store + a transient scratch buffer, records
 * the build, and resolves the AS device address. BLAS builds are compacted (see
 * rtx_compact_enabled): the full-size storage is transient and only the
 * compacted copy stays resident. */
static int vk_build_accel(lrt_vk_engine *e, VkAccelerationStructureTypeKHR type,
                          const VkAccelerationStructureGeometryKHR *geom,
                          uint32_t prim_count, uint32_t prim_offset,
                          uint32_t first_vertex, vk_accel *out) {
    memset(out, 0, sizeof(*out));
    const int compact = (type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR) &&
                        rtx_compact_enabled();
    VkAccelerationStructureBuildGeometryInfoKHR bgi;
    memset(&bgi, 0, sizeof(bgi));
    bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    bgi.type = type;
    bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (compact) bgi.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bgi.geometryCount = 1;
    bgi.pGeometries = geom;

    VkAccelerationStructureBuildSizesInfoKHR sizes;
    memset(&sizes, 0, sizeof(sizes));
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(
        e->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi,
        &prim_count, &sizes);

    /* Full-size storage: resident (non-compact) or transient (compact). */
    vk_buffer full_store;
    memset(&full_store, 0, sizeof(full_store));
    VkAccelerationStructureKHR full_as = 0;
    if (!vk_buffer_create_ex(e, sizes.accelerationStructureSize,
                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                             0, 0, &full_store))
        return 0;

    VkAccelerationStructureCreateInfoKHR ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    ci.buffer = full_store.buf;
    ci.offset = 0;
    ci.size = sizes.accelerationStructureSize;
    ci.type = type;
    VkResult r = vkCreateAccelerationStructureKHR(e->device, &ci, NULL, &full_as);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkCreateAccelerationStructureKHR", r);
        vk_buffer_destroy(e, &full_store);
        return 0;
    }

    /* Scratch: over-allocate 256 bytes so we can 256-align the device address
     * (a safe upper bound for minAccelerationStructureScratchOffsetAlignment). */
    vk_buffer scratch;
    if (!vk_buffer_create_ex(e, sizes.buildScratchSize + 256u,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, 1, &scratch)) {
        vkDestroyAccelerationStructureKHR(e->device, full_as, NULL);
        vk_buffer_destroy(e, &full_store);
        return 0;
    }
    uint64_t scratch_addr = vk_device_address(e, &scratch);
    scratch_addr = (scratch_addr + 255u) & ~(uint64_t)255u;

    VkQueryPool qp = 0;
    if (compact) {
        VkQueryPoolCreateInfo qci;
        memset(&qci, 0, sizeof(qci));
        qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qci.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        qci.queryCount = 1;
        if (vkCreateQueryPool(e->device, &qci, NULL, &qp) != VK_SUCCESS) qp = 0;
    }

    bgi.dstAccelerationStructure = full_as;
    bgi.scratchData.deviceAddress = scratch_addr;
    VkAccelerationStructureBuildRangeInfoKHR range;
    range.primitiveCount = prim_count;
    range.primitiveOffset = prim_offset;
    range.firstVertex = first_vertex;
    range.transformOffset = 0;
    const VkAccelerationStructureBuildRangeInfoKHR *pranges = &range;

    VkCommandBuffer cb = vk_cmd_begin(e);
    int ok = cb != VK_NULL_HANDLE;
    if (ok) {
        vkCmdBuildAccelerationStructuresKHR(cb, 1, &bgi, &pranges);
        VkMemoryBarrier mb;
        memset(&mb, 0, sizeof(mb));
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
        if (compact && qp) {
            vkCmdResetQueryPool(cb, qp, 0, 1);
            vkCmdWriteAccelerationStructuresPropertiesKHR(
                cb, 1, &full_as,
                VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, qp, 0);
        }
        ok = vk_cmd_end_submit(e, cb);
    }
    vk_buffer_destroy(e, &scratch);

    /* Compact: read the compacted size and copy into right-sized storage; the
     * full-size AS + storage are destroyed either way. Any compact-path failure
     * falls back to keeping the (valid) full-size build. */
    if (ok && compact && qp) {
        uint64_t csize = 0;
        VkResult qr = vkGetQueryPoolResults(
            e->device, qp, 0, 1, sizeof(csize), &csize, sizeof(csize),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (qr == VK_SUCCESS && csize > 0 &&
            csize < sizes.accelerationStructureSize) {
            vk_buffer cstore;
            memset(&cstore, 0, sizeof(cstore));
            VkAccelerationStructureKHR cas = 0;
            if (vk_buffer_create_ex(
                    e, csize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                    0, 0, &cstore)) {
                ci.buffer = cstore.buf;
                ci.size = csize;
                if (vkCreateAccelerationStructureKHR(e->device, &ci, NULL, &cas) ==
                    VK_SUCCESS) {
                    VkCommandBuffer ccb = vk_cmd_begin(e);
                    int cok = ccb != VK_NULL_HANDLE;
                    if (cok) {
                        VkCopyAccelerationStructureInfoKHR cpi;
                        memset(&cpi, 0, sizeof(cpi));
                        cpi.sType =
                            VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
                        cpi.src = full_as;
                        cpi.dst = cas;
                        cpi.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
                        vkCmdCopyAccelerationStructureKHR(ccb, &cpi);
                        VkMemoryBarrier mb2;
                        memset(&mb2, 0, sizeof(mb2));
                        mb2.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                        mb2.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                        mb2.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                        vkCmdPipelineBarrier(
                            ccb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            0, 1, &mb2, 0, NULL, 0, NULL);
                        cok = vk_cmd_end_submit(e, ccb);
                    }
                    if (cok) {
                        vkDestroyAccelerationStructureKHR(e->device, full_as, NULL);
                        vk_buffer_destroy(e, &full_store);
                        full_as = cas;
                        full_store = cstore;
                        memset(&cstore, 0, sizeof(cstore));
                        cas = 0;
                    }
                }
                if (cas) vkDestroyAccelerationStructureKHR(e->device, cas, NULL);
                if (cstore.buf) vk_buffer_destroy(e, &cstore);
            }
        }
    }
    if (qp) vkDestroyQueryPool(e->device, qp, NULL);

    if (!ok) {
        vk_set_err(e, "acceleration-structure build submit failed");
        vkDestroyAccelerationStructureKHR(e->device, full_as, NULL);
        vk_buffer_destroy(e, &full_store);
        return 0;
    }
    out->as = full_as;
    out->storage = full_store;

    VkAccelerationStructureDeviceAddressInfoKHR ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    ai.accelerationStructure = out->as;
    out->address =
        (uint64_t)vkGetAccelerationStructureDeviceAddressKHR(e->device, &ai);
    return 1;
}

static void vk_accel_destroy(lrt_vk_engine *e, vk_accel *a) {
    if (a->as && !e->device_lost)
        vkDestroyAccelerationStructureKHR(e->device, a->as, NULL);
    vk_buffer_destroy(e, &a->storage);
    memset(a, 0, sizeof(*a));
}

typedef struct rtx_push {
    uint32_t ray_count;
    uint32_t tri_chunk; /* prim_id = instanceId*tri_chunk + primitiveIndex */
} rtx_push;

/* Triangles per BLAS chunk. Kept well below the count at which RADV/gfx1201's
 * vkGetAccelerationStructureBuildSizesKHR returns a corrupted (under-sized)
 * scratch size and the build GPU-faults (empirically OK at 16M, broken by 24M).
 * Must be a power of two? No — the trace shader just multiplies by it. */
#define LRT_VK_BLAS_CHUNK (8u * 1024u * 1024u)

static vk_pipeline *rtx_get_pipeline(lrt_vk_engine *e) {
    if (e->rtx_pipe.valid) return &e->rtx_pipe;
    VkDescriptorType types[3] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    if (!vk_pipeline_build(e, trace_ray_query_spv, sizeof(trace_ray_query_spv), 3,
                           types, (uint32_t)sizeof(rtx_push), NULL, &e->rtx_pipe))
        return NULL;
    return &e->rtx_pipe;
}

/* Wide-id trace pipeline: identical bindings/push to rtx_pipe, but the shader
 * writes 5 words/hit ({t,u,v,instanceId,prim}) so instanced scenes past the
 * 32-bit prim_id product still trace. Same descriptor layout, so scenes can bind
 * against either pipeline. */
static vk_pipeline *rtx_get_pipeline_wide(lrt_vk_engine *e) {
    if (e->rtx_pipe_wide.valid) return &e->rtx_pipe_wide;
    VkDescriptorType types[3] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    if (!vk_pipeline_build(e, trace_ray_query_wide_spv,
                           sizeof(trace_ray_query_wide_spv), 3, types,
                           (uint32_t)sizeof(rtx_push), NULL, &e->rtx_pipe_wide))
        return NULL;
    return &e->rtx_pipe_wide;
}

/* TLAS + 2 storage buffers (rays, hits). The AS write is chained via pNext. */
static int vk_descriptors_bind_rtx(lrt_vk_engine *e, VkDescriptorSetLayout dsl,
                                   VkAccelerationStructureKHR tlas,
                                   const vk_buffer *rays, const vk_buffer *hits,
                                   VkDescriptorPool *out_pool,
                                   VkDescriptorSet *out_set) {
    VkDescriptorPoolSize ps[2];
    ps[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    ps[0].descriptorCount = 1;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps[1].descriptorCount = 2;
    VkDescriptorPoolCreateInfo dpci;
    memset(&dpci, 0, sizeof(dpci));
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = ps;
    VkResult r = vkCreateDescriptorPool(e->device, &dpci, NULL, out_pool);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkCreateDescriptorPool(rtx)", r);
        return 0;
    }
    VkDescriptorSetAllocateInfo dsai;
    memset(&dsai, 0, sizeof(dsai));
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = *out_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    r = vkAllocateDescriptorSets(e->device, &dsai, out_set);
    if (r != VK_SUCCESS) {
        vk_set_errf(e, "vkAllocateDescriptorSets(rtx)", r);
        vkDestroyDescriptorPool(e->device, *out_pool, NULL);
        return 0;
    }
    VkWriteDescriptorSetAccelerationStructureKHR asw;
    memset(&asw, 0, sizeof(asw));
    asw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asw.accelerationStructureCount = 1;
    asw.pAccelerationStructures = &tlas;
    VkDescriptorBufferInfo bi[2];
    bi[0].buffer = rays->buf;
    bi[0].offset = 0;
    bi[0].range = VK_WHOLE_SIZE;
    bi[1].buffer = hits->buf;
    bi[1].offset = 0;
    bi[1].range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet w[3];
    memset(w, 0, sizeof(w));
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].pNext = &asw;
    w[0].dstSet = *out_set;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = *out_set;
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &bi[0];
    w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[2].dstSet = *out_set;
    w[2].dstBinding = 2;
    w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[2].pBufferInfo = &bi[1];
    vkUpdateDescriptorSets(e->device, 3, w, 0, NULL);
    return 1;
}

/* Resident ray-tracing scene: the BLAS+TLAS are built once and kept on the
 * device; the trace working set (device-local ray/hit buffers + host-visible
 * staging) is allocated lazily and reused/grown across traces. This is what
 * makes lrt_vk_rtx_scene_trace measure traversal, not per-call AS build. */
struct lrt_vk_rtx_scene {
    /* The geometry is split into chunks of <= LRT_VK_BLAS_CHUNK triangles, one
     * BLAS per chunk, to work around drivers (e.g. RADV/gfx1201) whose AS
     * build-size query overflows for very large single-BLAS builds and then
     * GPU-faults. A single TLAS holds one identity instance per chunk; the trace
     * shader recovers the global triangle id as instanceId*tri_chunk + primIndex. */
    vk_accel *blas;      /* nblas chunk BLASes */
    uint32_t nblas;
    uint32_t tri_chunk;  /* triangles per chunk (the prim_id stride) */
    vk_accel tlas;
    vk_buffer rays_dev;   /* device-local STORAGE | TRANSFER_DST */
    vk_buffer hits_dev;   /* device-local STORAGE | TRANSFER_SRC */
    vk_buffer rays_stage; /* host-visible TRANSFER_SRC           */
    vk_buffer hits_stage; /* host-visible TRANSFER_DST           */
    uint32_t cap;         /* trace-buffer capacity in rays       */
    uint32_t hit_words;   /* words written per hit: 4 (narrow) or 5 (wide id) */
    /* Multi-TLAS: instanced scenes past the device TLAS maxInstanceCount (2^24)
     * are split into `ntlas` slices of <= LRT_VK_TLAS_SLICE instances, each its
     * own TLAS over the SHARED `blas` set. `tlas` is slice 0; `extra_tlas` holds
     * slices 1..ntlas-1. `tlas_base[k]` is the global instance index where slice k
     * starts (added back at trace so the reported instanceId is global). ntlas==1
     * is the ordinary single-TLAS scene (extra_tlas NULL). Wide id only. */
    vk_accel *extra_tlas; /* ntlas-1 additional TLAS slices, or NULL */
    uint32_t *tlas_base;  /* ntlas global base instance indices */
    uint32_t ntlas;       /* number of TLAS slices (>=1) */
    /* BLAS suballocation pools (see vk_subpool). A scene with ~100k prototypes
     * would otherwise do ~6 vkAllocateMemory per BLAS -> hundreds of thousands of
     * allocations (NVIDIA's allocator is ~O(live allocs); also a per-device count
     * limit), exhausting/faulting the driver. These pool the per-BLAS vertex/index
     * input and AS storage into a few large blocks each. Live for the scene. */
    struct vk_subpool *blas_input_pool;  /* host-visible verts+indices, dev address */
    struct vk_subpool *blas_store_pool;  /* device-local AS storage */
};

/* Bump suballocator: many small "buffers" share a few large VkBuffer blocks (one
 * VkDeviceMemory each), handed out as (block buffer, offset, device address). No
 * per-item free -- the whole pool is released at scene teardown. Collapses the
 * per-BLAS allocation storm into O(total_bytes / block_size) allocations. */
typedef struct vk_pool_block {
    vk_buffer buf;       /* the block's VkBuffer + VkDeviceMemory (via vk_buffer) */
    uint64_t base_addr;  /* device address of offset 0 (0 if not addressable)     */
    void *mapped;        /* persistent host pointer (host-visible pools), else NULL */
    VkDeviceSize used;   /* bump offset                                            */
} vk_pool_block;

typedef struct vk_subpool {
    VkBufferUsageFlags usage;
    int host_visible;
    int device_addr;
    VkDeviceSize block_size;
    vk_pool_block *blocks;
    uint32_t nblocks, cap;
} vk_subpool;

/* Instances per TLAS slice for the multi-TLAS builder. Kept safely below the
 * Vulkan-guaranteed TLAS maxInstanceCount floor (2^24 = 16,777,216). */
#define LRT_VK_TLAS_SLICE (16u * 1000u * 1000u)

/* Slice k's TLAS (slice 0 is the inline `tlas`, the rest live in extra_tlas). */
static vk_accel *rtx_scene_tlas(lrt_vk_rtx_scene *s, uint32_t k) {
    return k == 0u ? &s->tlas : &s->extra_tlas[k - 1u];
}

/* Bytes the trace shader writes per hit for scene s (narrow lrt_hit = 4 words). */
static size_t rtx_hit_bytes(const lrt_vk_rtx_scene *s) {
    return (size_t)(s->hit_words ? s->hit_words : 4u) * sizeof(uint32_t);
}

/* Shared BLAS+TLAS build behind the soup and indexed entry points. When
 * `indices` is NULL the BLAS is built from a de-indexed triangle soup
 * (`vertices` = 9*ntris floats, nverts == 3*ntris, no index buffer). When
 * `indices` is non-NULL the BLAS uses a real VK_INDEX_TYPE_UINT32 index buffer
 * (`vertices` = 3*nverts floats of unique positions, `indices` = 3*ntris vertex
 * ids), so callers with shared vertices need not de-index. Either way the
 * primitiveIndex is the triangle build order, so lrt_hit.prim_id stays
 * 0..ntris-1. */
static lrt_vk_rtx_scene *rtx_scene_build_core(lrt_vk_engine *e,
                                              const float *vertices,
                                              uint32_t nverts,
                                              const uint32_t *indices,
                                              uint32_t ntris, lrt_result *err) {
    if (!e || !vertices || ntris == 0 || nverts == 0) {
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return NULL;
    }
    if (!(e->caps & LRT_VK_CAP_RAY_QUERY) || !vkCreateAccelerationStructureKHR) {
        vk_set_err(e, "ray_query unavailable (create the engine with "
                      "want_ray_tracing=1 on an RT-capable device)");
        if (err) *err = LRT_RESULT_NOT_BUILT;
        return NULL;
    }
    if (!rtx_get_pipeline(e)) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return NULL;
    }

    lrt_vk_rtx_scene *s = (lrt_vk_rtx_scene *)calloc(1, sizeof(*s));
    if (!s) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return NULL;
    }

    /* The vertex/index/instance build inputs are only needed during the build —
     * once vkCmdBuildAccelerationStructures completes, the AS is self-contained,
     * so they are freed here and the resident scene holds only the BLAS + TLAS.
     * The vertex/index build inputs are staged into device-local memory: the AS
     * build input must be device-local on many implementations (NVIDIA requires
     * it). */
    vk_buffer vbuf = {0}, idxbuf = {0}, instbuf = {0};
    vk_buffer vstage = {0}, istage = {0};
    int ok = 0;
    do {
        VkDeviceSize vbytes = (VkDeviceSize)nverts * 3u * sizeof(float);
        VkDeviceSize ibytes = (VkDeviceSize)ntris * 3u * sizeof(uint32_t);
        /* Vertex staging (host-visible) + device-local AS build input. */
        if (!vk_buffer_create_ex(e, vbytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1, 0,
                                 &vstage))
            break;
        if (!vk_buffer_write(e, &vstage, vertices, (size_t)vbytes))
            break;
        if (!vk_buffer_create_ex(
                e, vbytes,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                0, 1, &vbuf))
            break;
        /* Optional index staging + device-local AS build input. */
        if (indices) {
            if (!vk_buffer_create_ex(e, ibytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1,
                                     0, &istage))
                break;
            if (!vk_buffer_write(e, &istage, indices, (size_t)ibytes))
                break;
            if (!vk_buffer_create_ex(
                    e, ibytes,
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    0, 1, &idxbuf))
                break;
        }
        /* One command buffer stages both vertex and (optional) index data. */
        VkCommandBuffer cb = vk_cmd_begin(e);
        if (!cb) break;
        VkBufferCopy vcopy = {0, 0, vbytes};
        vkCmdCopyBuffer(cb, vstage.buf, vbuf.buf, 1, &vcopy);
        if (indices) {
            VkBufferCopy icopy = {0, 0, ibytes};
            vkCmdCopyBuffer(cb, istage.buf, idxbuf.buf, 1, &icopy);
        }
        if (!vk_cmd_end_submit(e, cb)) break;
        uint64_t vaddr = vk_device_address(e, &vbuf);
        uint64_t idxaddr = indices ? vk_device_address(e, &idxbuf) : 0;

        VkAccelerationStructureGeometryKHR tgeom;
        memset(&tgeom, 0, sizeof(tgeom));
        tgeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        tgeom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        tgeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        tgeom.geometry.triangles.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tgeom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tgeom.geometry.triangles.vertexData.deviceAddress = vaddr;
        tgeom.geometry.triangles.vertexStride = 3u * sizeof(float);
        tgeom.geometry.triangles.maxVertex = nverts - 1u;
        if (indices) {
            tgeom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
            tgeom.geometry.triangles.indexData.deviceAddress = idxaddr;
        } else {
            tgeom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        }
        /* Split into chunk BLASes (one TLAS instance each). The build range's
         * primitiveOffset is a BYTE offset into the index buffer (indexed) or the
         * vertex buffer (soup); the shared tgeom keeps the full buffer addresses
         * and maxVertex. */
        const uint32_t chunk = LRT_VK_BLAS_CHUNK;
        const uint32_t nblas = (ntris + chunk - 1u) / chunk;
        s->blas = (vk_accel *)calloc(nblas, sizeof(vk_accel));
        if (!s->blas) break;
        s->nblas = nblas;
        s->tri_chunk = chunk;
        int blas_ok = 1;
        for (uint32_t c = 0; c < nblas; c++) {
            uint32_t base = c * chunk;
            uint32_t ct = (ntris - base < chunk) ? (ntris - base) : chunk;
            uint32_t prim_off =
                indices ? base * 3u * (uint32_t)sizeof(uint32_t)
                        : base * 3u * 3u * (uint32_t)sizeof(float);
            if (!vk_build_accel(e, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                &tgeom, ct, prim_off, 0u, &s->blas[c])) {
                blas_ok = 0;
                break;
            }
        }
        if (!blas_ok) break;

        /* One identity instance per chunk BLAS, in chunk order, so the hit's
         * instanceId (build order) recovers the global triangle id in the trace
         * shader as instanceId*tri_chunk + primitiveIndex. */
        VkAccelerationStructureInstanceKHR *insts =
            (VkAccelerationStructureInstanceKHR *)calloc(
                nblas, sizeof(VkAccelerationStructureInstanceKHR));
        if (!insts) break;
        for (uint32_t c = 0; c < nblas; c++) {
            insts[c].transform.matrix[0][0] = 1.0f;
            insts[c].transform.matrix[1][1] = 1.0f;
            insts[c].transform.matrix[2][2] = 1.0f;
            insts[c].mask = 0xFFu;
            insts[c].instanceCustomIndex = c;
            insts[c].accelerationStructureReference = s->blas[c].address;
        }
        VkDeviceSize inst_bytes =
            (VkDeviceSize)nblas * sizeof(VkAccelerationStructureInstanceKHR);
        int ibuilt =
            vk_buffer_create_ex(
                e, inst_bytes,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                1, 1, &instbuf) &&
            vk_buffer_write(e, &instbuf, insts, (size_t)inst_bytes);
        free(insts);
        if (!ibuilt) break;
        uint64_t iaddr = vk_device_address(e, &instbuf);

        VkAccelerationStructureGeometryKHR igeom;
        memset(&igeom, 0, sizeof(igeom));
        igeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        igeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        igeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        igeom.geometry.instances.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        igeom.geometry.instances.arrayOfPointers = VK_FALSE;
        igeom.geometry.instances.data.deviceAddress = iaddr;
        if (!vk_build_accel(e, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &igeom,
                            nblas, 0u, 0u, &s->tlas))
            break;
        ok = 1;
    } while (0);

    vk_buffer_destroy(e, &vstage);
    vk_buffer_destroy(e, &istage);
    vk_buffer_destroy(e, &vbuf);
    vk_buffer_destroy(e, &idxbuf);
    vk_buffer_destroy(e, &instbuf);
    if (!ok) {
        vk_accel_destroy(e, &s->tlas);
        if (s->blas) {
            for (uint32_t c = 0; c < s->nblas; c++) vk_accel_destroy(e, &s->blas[c]);
            free(s->blas);
        }
        free(s);
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return NULL;
    }
    if (err) *err = LRT_RESULT_OK;
    return s;
}

lrt_vk_rtx_scene *lrt_vk_rtx_scene_build(lrt_vk_engine *e, const float *vertices,
                                         uint32_t ntris, lrt_result *err) {
    return rtx_scene_build_core(e, vertices, ntris * 3u, NULL, ntris, err);
}

lrt_vk_rtx_scene *lrt_vk_rtx_scene_build_indexed(lrt_vk_engine *e,
                                                 const float *vertices,
                                                 uint32_t nverts,
                                                 const uint32_t *indices,
                                                 uint32_t ntris,
                                                 lrt_result *err) {
    if (!indices) {
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return NULL;
    }
    return rtx_scene_build_core(e, vertices, nverts, indices, ntris, err);
}

/* --- BLAS suballocation pool ------------------------------------------------- */

static void vk_subpool_init(vk_subpool *p, VkBufferUsageFlags usage,
                            int host_visible, int device_addr,
                            VkDeviceSize block_size) {
    memset(p, 0, sizeof(*p));
    p->usage = usage;
    p->host_visible = host_visible;
    p->device_addr = device_addr;
    p->block_size = block_size;
}

/* Suballocate `size` bytes (aligned to power-of-two `align`) from the pool. Returns
 * the containing block's VkBuffer, the byte offset within it, and -- when
 * applicable -- the persistent host pointer and device address at that offset. */
static int vk_subpool_alloc(lrt_vk_engine *e, vk_subpool *p, VkDeviceSize size,
                            VkDeviceSize align, VkBuffer *out_buf,
                            VkDeviceSize *out_off, void **out_mapped,
                            uint64_t *out_addr) {
    if (size == 0) size = 16;
    if (align == 0) align = 1;
    VkDeviceSize off = 0;
    int need_new = 1;
    if (p->nblocks) {
        vk_pool_block *last = &p->blocks[p->nblocks - 1];
        off = (last->used + (align - 1)) & ~(align - 1);
        if (off + size <= last->buf.size) need_new = 0;
    }
    if (need_new) {
        VkDeviceSize bs = size > p->block_size ? size : p->block_size;
        if (p->nblocks == p->cap) {
            uint32_t nc = p->cap ? p->cap * 2u : 8u;
            vk_pool_block *nb =
                (vk_pool_block *)realloc(p->blocks, nc * sizeof(vk_pool_block));
            if (!nb) return 0;
            p->blocks = nb;
            p->cap = nc;
        }
        vk_pool_block *blk = &p->blocks[p->nblocks];
        memset(blk, 0, sizeof(*blk));
        if (!vk_buffer_create_ex(e, bs, p->usage, p->host_visible, p->device_addr,
                                 &blk->buf))
            return 0;
        if (p->device_addr) blk->base_addr = vk_device_address(e, &blk->buf);
        if (p->host_visible &&
            vkMapMemory(e->device, blk->buf.mem, 0, VK_WHOLE_SIZE, 0, &blk->mapped) !=
                VK_SUCCESS) {
            vk_buffer_destroy(e, &blk->buf);
            return 0;
        }
        p->nblocks++;
        off = 0;
    }
    vk_pool_block *blk = &p->blocks[p->nblocks - 1];
    blk->used = off + size;
    if (out_buf) *out_buf = blk->buf.buf;
    if (out_off) *out_off = off;
    if (out_mapped) *out_mapped = blk->mapped ? (char *)blk->mapped + off : NULL;
    if (out_addr) *out_addr = blk->base_addr ? blk->base_addr + off : 0u;
    return 1;
}

static void vk_subpool_free(lrt_vk_engine *e, vk_subpool *p) {
    if (!p) return;
    for (uint32_t i = 0; i < p->nblocks; i++) {
        if (p->blocks[i].mapped && !e->device_lost)
            vkUnmapMemory(e->device, p->blocks[i].buf.mem);
        vk_buffer_destroy(e, &p->blocks[i].buf);
    }
    free(p->blocks);
    memset(p, 0, sizeof(*p));
}

/* Compaction variant of build_protos_pooled (used when rtx_compact_enabled()):
 * prototypes are built in waves into one reused transient full-size buffer, the
 * compacted sizes are read back from a query pool, and the BLAS are tight-packed
 * into the resident store pool with vkCmdCopyAccelerationStructureKHR (COMPACT).
 * Two submits per wave instead of one per prototype, and only the compacted BLAS
 * (typically ~50-60% of full size) stay resident; transient VRAM overhead is one
 * wave (~WAVE_BYTES) of full-size storage plus its scratch. Same ownership shape
 * as build_protos_pooled: blas[p].as owns only the handle, storage is pool-owned. */
static int build_protos_pooled_compact(lrt_vk_engine *e, const lrt_vk_proto *protos,
                                       uint32_t nprotos, lrt_vk_rtx_scene *s) {
    const VkDeviceSize BLOCK = 128u * 1024u * 1024u;
    const uint32_t WAVE_MAX = 512u;        /* also the query-pool capacity */
    const VkDeviceSize WAVE_BYTES = BLOCK; /* full-size AS / scratch budget per wave */

    s->blas_input_pool = (vk_subpool *)calloc(1, sizeof(vk_subpool));
    s->blas_store_pool = (vk_subpool *)calloc(1, sizeof(vk_subpool));
    if (!s->blas_input_pool || !s->blas_store_pool) return 0;
    vk_subpool_init(
        s->blas_input_pool,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        /*host_visible=*/1, /*device_addr=*/1, BLOCK);
    vk_subpool_init(s->blas_store_pool,
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                    /*host_visible=*/0, /*device_addr=*/0, BLOCK);

    VkAccelerationStructureGeometryKHR *geoms =
        (VkAccelerationStructureGeometryKHR *)calloc(WAVE_MAX, sizeof(*geoms));
    VkAccelerationStructureBuildGeometryInfoKHR *bgis =
        (VkAccelerationStructureBuildGeometryInfoKHR *)calloc(WAVE_MAX, sizeof(*bgis));
    VkAccelerationStructureBuildRangeInfoKHR *ranges =
        (VkAccelerationStructureBuildRangeInfoKHR *)calloc(WAVE_MAX, sizeof(*ranges));
    const VkAccelerationStructureBuildRangeInfoKHR **prange =
        (const VkAccelerationStructureBuildRangeInfoKHR **)calloc(WAVE_MAX,
                                                                  sizeof(*prange));
    VkAccelerationStructureKHR *full_as =
        (VkAccelerationStructureKHR *)calloc(WAVE_MAX, sizeof(*full_as));
    VkDeviceSize *as_off = (VkDeviceSize *)calloc(WAVE_MAX, sizeof(*as_off));
    VkDeviceSize *as_size = (VkDeviceSize *)calloc(WAVE_MAX, sizeof(*as_size));
    VkDeviceSize *sc_off = (VkDeviceSize *)calloc(WAVE_MAX, sizeof(*sc_off));
    uint64_t *csize = (uint64_t *)calloc(WAVE_MAX, sizeof(*csize));
    vk_buffer full_store = {0};  // transient full-size storage, reused across waves
    vk_buffer scratch = {0};     // reused across waves, grown on demand
    VkQueryPool qp = 0;
    int ok = geoms && bgis && ranges && prange && full_as && as_off && as_size &&
             sc_off && csize;

    if (ok) {
        VkQueryPoolCreateInfo qci;
        memset(&qci, 0, sizeof(qci));
        qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qci.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        qci.queryCount = WAVE_MAX;
        if (vkCreateQueryPool(e->device, &qci, NULL, &qp) != VK_SUCCESS) {
            qp = 0;
            ok = 0;
        }
    }

    uint32_t p = 0;
    while (ok && p < nprotos) {
        /* Plan the wave from size queries only (build-sizes ignores the data
         * addresses), stopping at the full-size/scratch budget. The first
         * prototype is always taken, so one BLAS bigger than WAVE_BYTES still
         * works -- the transient buffers just grow for that wave. */
        uint32_t count = 0;
        VkDeviceSize full_top = 0, sc_top = 0;
        while (p + count < nprotos && count < WAVE_MAX) {
            const lrt_vk_proto *pr = &protos[p + count];
            VkAccelerationStructureGeometryKHR *g = &geoms[count];
            memset(g, 0, sizeof(*g));
            g->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            g->geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            g->flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            g->geometry.triangles.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            g->geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            g->geometry.triangles.vertexStride = 3u * sizeof(float);
            g->geometry.triangles.maxVertex = pr->nverts - 1u;
            g->geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;

            VkAccelerationStructureBuildGeometryInfoKHR *bg = &bgis[count];
            memset(bg, 0, sizeof(*bg));
            bg->sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            bg->type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            bg->flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                        VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
            bg->mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            bg->geometryCount = 1;
            bg->pGeometries = g;

            VkAccelerationStructureBuildSizesInfoKHR sizes;
            memset(&sizes, 0, sizeof(sizes));
            sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
            vkGetAccelerationStructureBuildSizesKHR(
                e->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, bg,
                &pr->ntris, &sizes);

            VkDeviceSize aoff = (full_top + 255u) & ~(VkDeviceSize)255u;
            VkDeviceSize soff = (sc_top + 255u) & ~(VkDeviceSize)255u;
            if (count && (aoff + sizes.accelerationStructureSize > WAVE_BYTES ||
                          soff + sizes.buildScratchSize > WAVE_BYTES))
                break;
            as_off[count] = aoff;
            as_size[count] = sizes.accelerationStructureSize;
            sc_off[count] = soff;
            full_top = aoff + sizes.accelerationStructureSize;
            sc_top = soff + sizes.buildScratchSize;
            ranges[count].primitiveCount = pr->ntris;
            ranges[count].primitiveOffset = 0;
            ranges[count].firstVertex = 0;
            ranges[count].transformOffset = 0;
            prange[count] = &ranges[count];
            count++;
        }

        /* Upload this wave's inputs and patch the geometry addresses. */
        for (uint32_t i = 0; i < count && ok; i++) {
            const lrt_vk_proto *pr = &protos[p + i];
            VkDeviceSize vbytes = (VkDeviceSize)pr->nverts * 3u * sizeof(float);
            VkDeviceSize ibytes = (VkDeviceSize)pr->ntris * 3u * sizeof(uint32_t);
            VkBuffer vbuf, ibuf;
            VkDeviceSize voff, ioff;
            void *vmap, *imap;
            uint64_t vaddr, iaddr;
            if (!vk_subpool_alloc(e, s->blas_input_pool, vbytes, 16, &vbuf, &voff,
                                  &vmap, &vaddr) ||
                !vk_subpool_alloc(e, s->blas_input_pool, ibytes, 16, &ibuf, &ioff,
                                  &imap, &iaddr)) {
                ok = 0;
                break;
            }
            memcpy(vmap, pr->vertices, (size_t)vbytes);
            memcpy(imap, pr->indices, (size_t)ibytes);
            geoms[i].geometry.triangles.vertexData.deviceAddress = vaddr;
            geoms[i].geometry.triangles.indexData.deviceAddress = iaddr;
        }
        if (!ok) break;

        /* Transient full-size + scratch buffers, grown on demand. */
        if (full_store.size < full_top) {
            vk_buffer_destroy(e, &full_store);
            if (!vk_buffer_create_ex(
                    e, full_top,
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0, 0,
                    &full_store)) {
                ok = 0;
                break;
            }
        }
        if (scratch.size < sc_top + 256u) {
            vk_buffer_destroy(e, &scratch);
            if (!vk_buffer_create_ex(e, sc_top + 256u,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, 1,
                                     &scratch)) {
                ok = 0;
                break;
            }
        }
        uint64_t sbase = (vk_device_address(e, &scratch) + 255u) & ~(uint64_t)255u;

        for (uint32_t i = 0; i < count && ok; i++) {
            VkAccelerationStructureCreateInfoKHR ci;
            memset(&ci, 0, sizeof(ci));
            ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
            ci.buffer = full_store.buf;
            ci.offset = as_off[i];
            ci.size = as_size[i];
            ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            if (vkCreateAccelerationStructureKHR(e->device, &ci, NULL,
                                                 &full_as[i]) != VK_SUCCESS) {
                ok = 0;
                break;
            }
            bgis[i].dstAccelerationStructure = full_as[i];
            bgis[i].scratchData.deviceAddress = sbase + sc_off[i];
        }

        /* Submit 1: batched build of the wave + compacted-size query writes. */
        if (ok) {
            VkCommandBuffer cb = vk_cmd_begin(e);
            ok = cb != VK_NULL_HANDLE;
            if (ok) {
                vkCmdBuildAccelerationStructuresKHR(cb, count, bgis, prange);
                VkMemoryBarrier mb;
                memset(&mb, 0, sizeof(mb));
                mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                vkCmdPipelineBarrier(
                    cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &mb,
                    0, NULL, 0, NULL);
                vkCmdResetQueryPool(cb, qp, 0, count);
                vkCmdWriteAccelerationStructuresPropertiesKHR(
                    cb, count, full_as,
                    VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, qp, 0);
                ok = vk_cmd_end_submit(e, cb);
            }
        }
        if (ok && vkGetQueryPoolResults(
                      e->device, qp, 0, count, (size_t)count * sizeof(uint64_t),
                      csize, sizeof(uint64_t),
                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
            ok = 0;

        /* Submit 2: tight-pack the compacted BLAS into the resident store pool. */
        VkCommandBuffer ccb = VK_NULL_HANDLE;
        if (ok) {
            ccb = vk_cmd_begin(e);
            ok = ccb != VK_NULL_HANDLE;
        }
        for (uint32_t i = 0; i < count && ok; i++) {
            if (csize[i] == 0 || csize[i] > as_size[i]) {
                ok = 0;
                break;
            }
            VkBuffer sbuf;
            VkDeviceSize soff;
            if (!vk_subpool_alloc(e, s->blas_store_pool, csize[i], 256, &sbuf, &soff,
                                  NULL, NULL)) {
                ok = 0;
                break;
            }
            VkAccelerationStructureCreateInfoKHR ci;
            memset(&ci, 0, sizeof(ci));
            ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
            ci.buffer = sbuf;
            ci.offset = soff;
            ci.size = csize[i];
            ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            if (vkCreateAccelerationStructureKHR(e->device, &ci, NULL,
                                                 &s->blas[p + i].as) != VK_SUCCESS) {
                ok = 0;
                break;
            }
            VkCopyAccelerationStructureInfoKHR cpi;
            memset(&cpi, 0, sizeof(cpi));
            cpi.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
            cpi.src = full_as[i];
            cpi.dst = s->blas[p + i].as;
            cpi.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
            vkCmdCopyAccelerationStructureKHR(ccb, &cpi);
        }
        if (ok) {
            VkMemoryBarrier mb;
            memset(&mb, 0, sizeof(mb));
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            vkCmdPipelineBarrier(
                ccb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 1, &mb, 0, NULL, 0, NULL);
            ok = vk_cmd_end_submit(e, ccb);
        } else if (ccb && !e->device_lost) {
            vkEndCommandBuffer(ccb);
            vkFreeCommandBuffers(e->device, e->cmd_pool, 1, &ccb);
        }

        /* The full-size builds are transient -- drop the wave's handles. */
        for (uint32_t i = 0; i < count; i++) {
            if (full_as[i] && !e->device_lost)
                vkDestroyAccelerationStructureKHR(e->device, full_as[i], NULL);
            full_as[i] = 0;
        }
        for (uint32_t i = 0; i < count && ok; i++) {
            VkAccelerationStructureDeviceAddressInfoKHR ai;
            memset(&ai, 0, sizeof(ai));
            ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
            ai.accelerationStructure = s->blas[p + i].as;
            s->blas[p + i].address =
                vkGetAccelerationStructureDeviceAddressKHR(e->device, &ai);
        }
        p += count;
    }

    if (qp && !e->device_lost) vkDestroyQueryPool(e->device, qp, NULL);
    vk_buffer_destroy(e, &full_store);
    vk_buffer_destroy(e, &scratch);
    free(geoms);
    free(bgis);
    free(ranges);
    free(prange);
    free(full_as);
    free(as_off);
    free(as_size);
    free(sc_off);
    free(csize);
    return ok;
}

/* Build all prototype BLAS through two suballocation pools (vertex/index INPUT and
 * AS STORAGE) plus one reused scratch buffer, instead of ~6 vkAllocateMemory + 2
 * submits per prototype. On a ~100k-prototype scene (full Moana island) the naive
 * path exhausts NVIDIA's allocator and the driver faults; pooling keeps it to a few
 * dozen block allocations. Input verts/indices go in a host-visible, device-address
 * pool (memcpy'd directly -- no staging, no copy submit); each BLAS is built into a
 * pooled device-local storage slice. blas[p].as owns only the AS handle (storage is
 * pool-owned; blas[p].storage stays empty). Returns 1 on success. */
static int build_protos_pooled(lrt_vk_engine *e, const lrt_vk_proto *protos,
                               uint32_t nprotos, lrt_vk_rtx_scene *s) {
    if (rtx_compact_enabled())
        return build_protos_pooled_compact(e, protos, nprotos, s);
    const VkDeviceSize BLOCK = 128u * 1024u * 1024u;  // 128 MiB blocks
    s->blas_input_pool = (vk_subpool *)calloc(1, sizeof(vk_subpool));
    s->blas_store_pool = (vk_subpool *)calloc(1, sizeof(vk_subpool));
    if (!s->blas_input_pool || !s->blas_store_pool) return 0;
    vk_subpool_init(
        s->blas_input_pool,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        /*host_visible=*/1, /*device_addr=*/1, BLOCK);
    vk_subpool_init(s->blas_store_pool,
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                    /*host_visible=*/0, /*device_addr=*/0, BLOCK);

    vk_buffer scratch = {0};  // reused across builds, grown on demand
    int ok = 1;
    for (uint32_t p = 0; p < nprotos && ok; p++) {
        const lrt_vk_proto *pr = &protos[p];
        VkDeviceSize vbytes = (VkDeviceSize)pr->nverts * 3u * sizeof(float);
        VkDeviceSize ibytes = (VkDeviceSize)pr->ntris * 3u * sizeof(uint32_t);
        VkBuffer vbuf, ibuf;
        VkDeviceSize voff, ioff;
        void *vmap, *imap;
        uint64_t vaddr, iaddr;
        if (!vk_subpool_alloc(e, s->blas_input_pool, vbytes, 16, &vbuf, &voff, &vmap,
                              &vaddr) ||
            !vk_subpool_alloc(e, s->blas_input_pool, ibytes, 16, &ibuf, &ioff, &imap,
                              &iaddr)) {
            ok = 0;
            break;
        }
        memcpy(vmap, pr->vertices, (size_t)vbytes);
        memcpy(imap, pr->indices, (size_t)ibytes);

        VkAccelerationStructureGeometryKHR g;
        memset(&g, 0, sizeof(g));
        g.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        g.geometry.triangles.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        g.geometry.triangles.vertexData.deviceAddress = vaddr;
        g.geometry.triangles.vertexStride = 3u * sizeof(float);
        g.geometry.triangles.maxVertex = pr->nverts - 1u;
        g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        g.geometry.triangles.indexData.deviceAddress = iaddr;

        VkAccelerationStructureBuildGeometryInfoKHR bgi;
        memset(&bgi, 0, sizeof(bgi));
        bgi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bgi.geometryCount = 1;
        bgi.pGeometries = &g;

        VkAccelerationStructureBuildSizesInfoKHR sizes;
        memset(&sizes, 0, sizeof(sizes));
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(
            e->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bgi,
            &pr->ntris, &sizes);

        VkBuffer sbuf;
        VkDeviceSize soff;
        if (!vk_subpool_alloc(e, s->blas_store_pool, sizes.accelerationStructureSize,
                              256, &sbuf, &soff, NULL, NULL)) {
            ok = 0;
            break;
        }
        VkAccelerationStructureCreateInfoKHR ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = sbuf;
        ci.offset = soff;
        ci.size = sizes.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (vkCreateAccelerationStructureKHR(e->device, &ci, NULL, &s->blas[p].as) !=
            VK_SUCCESS) {
            ok = 0;
            break;
        }

        VkDeviceSize need = sizes.buildScratchSize + 256u;
        if (scratch.size < need) {
            vk_buffer_destroy(e, &scratch);
            if (!vk_buffer_create_ex(e, need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, 1,
                                     &scratch)) {
                ok = 0;
                break;
            }
        }
        uint64_t saddr = (vk_device_address(e, &scratch) + 255u) & ~(uint64_t)255u;

        bgi.dstAccelerationStructure = s->blas[p].as;
        bgi.scratchData.deviceAddress = saddr;
        VkAccelerationStructureBuildRangeInfoKHR range = {pr->ntris, 0u, 0u, 0u};
        const VkAccelerationStructureBuildRangeInfoKHR *pranges = &range;

        VkCommandBuffer cb = vk_cmd_begin(e);
        if (cb == VK_NULL_HANDLE) {
            ok = 0;
            break;
        }
        vkCmdBuildAccelerationStructuresKHR(cb, 1, &bgi, &pranges);
        VkMemoryBarrier mb;
        memset(&mb, 0, sizeof(mb));
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0,
                             1, &mb, 0, NULL, 0, NULL);
        if (!vk_cmd_end_submit(e, cb)) {
            ok = 0;
            break;
        }
        VkAccelerationStructureDeviceAddressInfoKHR ai;
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        ai.accelerationStructure = s->blas[p].as;
        s->blas[p].address =
            vkGetAccelerationStructureDeviceAddressKHR(e->device, &ai);
    }
    vk_buffer_destroy(e, &scratch);
    return ok;
}

/* Build one TLAS over the instance slice insts[off .. off+count) referencing the
 * scene's shared BLAS. instanceCustomIndex / build order is the slice-LOCAL index
 * (0..count-1); the caller adds tlas_base[k]=off at trace to recover the global
 * instance id. Returns 1 on success (out_tlas filled), 0 on failure. */
static int build_tlas_slice(lrt_vk_engine *e, lrt_vk_rtx_scene *s, uint32_t nprotos,
                            const lrt_vk_instance *insts, uint32_t off,
                            uint32_t count, vk_accel *out_tlas) {
    VkAccelerationStructureInstanceKHR *vi =
        (VkAccelerationStructureInstanceKHR *)calloc(
            count, sizeof(VkAccelerationStructureInstanceKHR));
    if (!vi) return 0;
    vk_buffer instbuf = {0};
    int ok = 0;
    do {
        int inst_ok = 1;
        for (uint32_t j = 0; j < count; j++) {
            const lrt_vk_instance *in = &insts[off + j];
            if (in->proto >= nprotos) {
                inst_ok = 0;
                break;
            }
            /* transform[12] is row-major 3x4 (world = M*[p;1]), matching
             * VkTransformMatrixKHR.matrix[3][4] exactly. */
            memcpy(vi[j].transform.matrix, in->transform, 12u * sizeof(float));
            vi[j].mask = 0xFFu;
            vi[j].instanceCustomIndex = j;
            vi[j].accelerationStructureReference = s->blas[in->proto].address;
        }
        if (!inst_ok) break;
        VkDeviceSize inst_bytes =
            (VkDeviceSize)count * sizeof(VkAccelerationStructureInstanceKHR);
        if (!(vk_buffer_create_ex(
                  e, inst_bytes,
                  VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                  1, 1, &instbuf) &&
              vk_buffer_write(e, &instbuf, vi, (size_t)inst_bytes)))
            break;
        VkAccelerationStructureGeometryKHR igeom;
        memset(&igeom, 0, sizeof(igeom));
        igeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        igeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        igeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        igeom.geometry.instances.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        igeom.geometry.instances.arrayOfPointers = VK_FALSE;
        igeom.geometry.instances.data.deviceAddress = vk_device_address(e, &instbuf);
        if (!vk_build_accel(e, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &igeom,
                            count, 0u, 0u, out_tlas))
            break;
        ok = 1;
    } while (0);
    vk_buffer_destroy(e, &instbuf);
    free(vi);
    return ok;
}

/* Core of all instanced builders. `wide`=0 uses the narrow 4-word trace and
 * enforces the 32-bit prim_id product (ninsts*maxPrototypeTris < 2^32); `wide`=1
 * uses the 5-word trace that stores instanceId + prim separately, so there is no
 * product to overflow. `max_per_tlas`=0 builds a single TLAS; otherwise the
 * instances are split into ceil(ninsts/max_per_tlas) TLAS slices sharing one BLAS
 * set (multi-TLAS -- renders scenes past the device TLAS maxInstanceCount). Wide
 * is required for multi-TLAS (the CPU merge needs the separate instanceId). */
static lrt_vk_rtx_scene *rtx_build_instanced_core(
    lrt_vk_engine *e, const lrt_vk_proto *protos, uint32_t nprotos,
    const lrt_vk_instance *insts, uint32_t ninsts, int wide,
    uint32_t max_per_tlas, uint32_t *out_tri_stride, lrt_result *err) {
    if (!e || !protos || nprotos == 0 || !insts || ninsts == 0) {
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return NULL;
    }
    if (!(e->caps & LRT_VK_CAP_RAY_QUERY) || !vkCreateAccelerationStructureKHR) {
        vk_set_err(e, "ray_query unavailable (create the engine with "
                      "want_ray_tracing=1 on an RT-capable device)");
        if (err) *err = LRT_RESULT_NOT_BUILT;
        return NULL;
    }
    if (!(wide ? rtx_get_pipeline_wide(e) : rtx_get_pipeline(e))) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return NULL;
    }
    /* Narrow: prim_id = instanceId*stride + prim must fit 32 bits and never
     * collide with LRT_TRI_NO_HIT (0xFFFFFFFF). stride = max prototype tri count.
     * Wide: instanceId and prim are stored separately, so stride is unused for
     * encoding (kept only to report *out_tri_stride). */
    uint32_t stride = 1u;
    for (uint32_t p = 0; p < nprotos; p++) {
        if (!protos[p].vertices || !protos[p].indices || protos[p].ntris == 0 ||
            protos[p].nverts == 0) {
            if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
            return NULL;
        }
        if (protos[p].ntris > stride) stride = protos[p].ntris;
    }
    if (!wide && (uint64_t)ninsts * (uint64_t)stride >= (uint64_t)LRT_TRI_NO_HIT) {
        vk_set_err(e, "instanced scene exceeds 32-bit prim_id encoding "
                      "(ninsts*maxPrototypeTris too large; use the wide builder)");
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return NULL;
    }

    lrt_vk_rtx_scene *s = (lrt_vk_rtx_scene *)calloc(1, sizeof(*s));
    if (!s) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return NULL;
    }
    s->blas = (vk_accel *)calloc(nprotos, sizeof(vk_accel));
    if (!s->blas) {
        free(s);
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return NULL;
    }
    s->nblas = nprotos;
    s->tri_chunk = stride;
    s->hit_words = wide ? 5u : 4u;

    /* Slice the instances across one or more TLASes (all sharing the BLAS set).
     * LRT_VK_TLAS_SLICE (env) shrinks the slice for testing the merge on small
     * scenes; it only applies to the multi-TLAS builder (max_per_tlas>0). */
    uint32_t cap = max_per_tlas;
    if (cap) {
        const char *ev = getenv("LRT_VK_TLAS_SLICE");
        if (ev) {
            long v = atol(ev);
            if (v > 0) cap = (uint32_t)v;
        }
    }
    const uint32_t slice = (cap && cap < ninsts) ? cap : ninsts;
    const uint32_t ntlas = (ninsts + slice - 1u) / slice;

    int ok = 0;
    int gpu_fail = 0;  /* a GPU build (not a host alloc) failed -> driver may be faulted */
    do {
        /* Build every prototype BLAS through the suballocation pools (a few large
         * blocks) instead of ~6 vkAllocateMemory + 2 submits per prototype, which
         * on ~100k-prototype scenes (full Moana island) exhausts NVIDIA's allocator
         * and faults the driver. */
        if (!build_protos_pooled(e, protos, nprotos, s)) { gpu_fail = 1; break; }

        s->tlas_base = (uint32_t *)calloc(ntlas, sizeof(uint32_t));
        if (!s->tlas_base) break;
        if (ntlas > 1u) {
            s->extra_tlas = (vk_accel *)calloc(ntlas - 1u, sizeof(vk_accel));
            if (!s->extra_tlas) break;
        }
        int slices_ok = 1;
        for (uint32_t k = 0; k < ntlas; k++) {
            const uint32_t off = k * slice;
            const uint32_t count = (off + slice <= ninsts) ? slice : (ninsts - off);
            s->tlas_base[k] = off;
            if (!build_tlas_slice(e, s, nprotos, insts, off, count,
                                  rtx_scene_tlas(s, k))) {
                slices_ok = 0;
                break;
            }
        }
        if (!slices_ok) { gpu_fail = 1; break; }
        s->ntlas = ntlas;
        ok = 1;
    } while (0);

    if (!ok) {
        /* A GPU build in the (possibly ~100k-BLAS) storm failed -- typically the GPU
         * ran out of memory. NVIDIA's driver is then left in a state where destroying
         * the already-built accels segfaults INSIDE the driver, so mark the device
         * unusable: vk_accel_destroy / vk_subpool_free / engine teardown then skip all
         * driver calls and only free host memory (the OS reclaims the GPU on exit).
         * A pure host-allocation failure leaves the device healthy, so tear the
         * partial scene down normally and keep the engine usable for a retry. */
        if (gpu_fail) e->device_lost = 1;
        vk_accel_destroy(e, &s->tlas);
        if (s->extra_tlas)
            for (uint32_t k = 1; k < ntlas; k++)
                vk_accel_destroy(e, &s->extra_tlas[k - 1u]);
        for (uint32_t p = 0; p < s->nblas; p++) vk_accel_destroy(e, &s->blas[p]);
        if (s->blas_input_pool) {
            vk_subpool_free(e, s->blas_input_pool);
            free(s->blas_input_pool);
        }
        if (s->blas_store_pool) {
            vk_subpool_free(e, s->blas_store_pool);
            free(s->blas_store_pool);
        }
        free(s->extra_tlas);
        free(s->tlas_base);
        free(s->blas);
        free(s);
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return NULL;
    }
    if (out_tri_stride) *out_tri_stride = stride;
    if (err) *err = LRT_RESULT_OK;
    return s;
}

lrt_vk_rtx_scene *lrt_vk_rtx_scene_build_instanced(
    lrt_vk_engine *e, const lrt_vk_proto *protos, uint32_t nprotos,
    const lrt_vk_instance *insts, uint32_t ninsts, uint32_t *out_tri_stride,
    lrt_result *err) {
    return rtx_build_instanced_core(e, protos, nprotos, insts, ninsts,
                                    /*wide=*/0, /*max_per_tlas=*/0, out_tri_stride,
                                    err);
}

lrt_vk_rtx_scene *lrt_vk_rtx_scene_build_instanced_wide(
    lrt_vk_engine *e, const lrt_vk_proto *protos, uint32_t nprotos,
    const lrt_vk_instance *insts, uint32_t ninsts, lrt_result *err) {
    return rtx_build_instanced_core(e, protos, nprotos, insts, ninsts,
                                    /*wide=*/1, /*max_per_tlas=*/0, NULL, err);
}

lrt_vk_rtx_scene *lrt_vk_rtx_scene_build_instanced_multi(
    lrt_vk_engine *e, const lrt_vk_proto *protos, uint32_t nprotos,
    const lrt_vk_instance *insts, uint32_t ninsts, lrt_result *err) {
    return rtx_build_instanced_core(e, protos, nprotos, insts, ninsts,
                                    /*wide=*/1, /*max_per_tlas=*/LRT_VK_TLAS_SLICE,
                                    NULL, err);
}

/* Grow the device-local + staging trace buffers to hold at least n rays. */
static int rtx_scene_ensure(lrt_vk_engine *e, lrt_vk_rtx_scene *s, uint32_t n) {
    if (s->rays_dev.buf && n <= s->cap) return 1;
    vk_buffer_destroy(e, &s->rays_dev);
    vk_buffer_destroy(e, &s->hits_dev);
    vk_buffer_destroy(e, &s->rays_stage);
    vk_buffer_destroy(e, &s->hits_stage);
    s->cap = 0;
    VkDeviceSize rb = (VkDeviceSize)n * sizeof(lrt_ray);
    VkDeviceSize hb = (VkDeviceSize)n * rtx_hit_bytes(s);
    if (!vk_buffer_create_ex(e, rb,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             0, 0, &s->rays_dev) ||
        !vk_buffer_create_ex(e, hb,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             0, 0, &s->hits_dev) ||
        !vk_buffer_create_ex(e, rb, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1, 0,
                             &s->rays_stage) ||
        !vk_buffer_create_ex(e, hb, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, 0,
                             &s->hits_stage))
        return 0;
    s->cap = n;
    return 1;
}

static void vk_buf_barrier(VkCommandBuffer cb, VkFlags src_stage, VkFlags dst_stage,
                           VkFlags src_access, VkFlags dst_access) {
    VkMemoryBarrier mb;
    memset(&mb, 0, sizeof(mb));
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = src_access;
    mb.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* Shared trace body for both the narrow (lrt_hit, 4-word) and wide (lrt_hit_wide,
 * 5-word) hit layouts. `pipe` selects the shader variant; `out` must point at n
 * hits of rtx_hit_bytes(s). Both layouts store the miss sentinel LRT_TRI_NO_HIT in
 * word 3 (narrow prim_id / wide instanceId), so the hit count is one check. */
static int rtx_trace_impl(lrt_vk_engine *e, lrt_vk_rtx_scene *s, vk_pipeline *pipe,
                          VkAccelerationStructureKHR tlas, const lrt_ray *rays,
                          uint32_t n, void *out, lrt_result *err) {
    if (!e || !s || (n && (!rays || !out))) {
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return -1;
    }
    if (n == 0) {
        if (err) *err = LRT_RESULT_OK;
        return 0;
    }
    if (!pipe || !rtx_scene_ensure(e, s, n)) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return -1;
    }
    const size_t hit_bytes = rtx_hit_bytes(s);

    /* Host -> staging (bulk memcpy), then one command buffer does staging ->
     * device-local rays, dispatch (reads/writes VRAM), device-local hits ->
     * staging — all with barriers, a single submit. */
    if (!vk_buffer_write(e, &s->rays_stage, rays, (size_t)n * sizeof(lrt_ray))) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return -1;
    }

    VkDescriptorPool pool;
    VkDescriptorSet set;
    if (!vk_descriptors_bind_rtx(e, pipe->dsl, tlas, &s->rays_dev,
                                 &s->hits_dev, &pool, &set)) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return -1;
    }

    int run_ok = 0;
    VkCommandBuffer cb = vk_cmd_begin(e);
    if (cb != VK_NULL_HANDLE) {
        VkBufferCopy up = {0, 0, (VkDeviceSize)n * sizeof(lrt_ray)};
        vkCmdCopyBuffer(cb, s->rays_stage.buf, s->rays_dev.buf, 1, &up);
        vk_buf_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        rtx_push pc;
        pc.ray_count = n;
        pc.tri_chunk = s->tri_chunk;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->layout, 0,
                                1, &set, 0, NULL);
        vkCmdPushConstants(cb, pipe->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           (uint32_t)sizeof(pc), &pc);
        vkCmdDispatch(cb, (n + 63u) / 64u, 1, 1);
        vk_buf_barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferCopy down = {0, 0, (VkDeviceSize)n * hit_bytes};
        vkCmdCopyBuffer(cb, s->hits_dev.buf, s->hits_stage.buf, 1, &down);
        vk_buf_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_HOST_READ_BIT);
        run_ok = vk_cmd_end_submit(e, cb);
    }
    /* vk_cmd_end_submit may have flagged the device lost (trace timeout / loss); on a
     * lost device every other site in this file skips driver calls, so match that and
     * only leak the pool handle (the OS reclaims the GPU on exit). */
    if (!e->device_lost) vkDestroyDescriptorPool(e->device, pool, NULL);
    if (!run_ok) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return -1;
    }
    if (!vk_buffer_read(e, &s->hits_stage, out, (size_t)n * hit_bytes)) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return -1;
    }

    const uint32_t hw = s->hit_words ? s->hit_words : 4u;
    const uint32_t *w = (const uint32_t *)out;
    int hits = 0;
    for (uint32_t i = 0; i < n; i++)
        if (w[(size_t)i * hw + 3u] != LRT_TRI_NO_HIT) hits++;
    if (err) *err = LRT_RESULT_OK;
    return hits;
}

int lrt_vk_rtx_scene_trace(lrt_vk_engine *e, lrt_vk_rtx_scene *s,
                           const lrt_ray *rays, uint32_t n, lrt_hit *out,
                           lrt_result *err) {
    if (s && s->hit_words == 5u) {  /* built wide: caller must use the wide trace */
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return -1;
    }
    return rtx_trace_impl(e, s, rtx_get_pipeline(e), s ? s->tlas.as : VK_NULL_HANDLE,
                          rays, n, out, err);
}

int lrt_vk_rtx_scene_trace_wide(lrt_vk_engine *e, lrt_vk_rtx_scene *s,
                                const lrt_ray *rays, uint32_t n, lrt_hit_wide *out,
                                lrt_result *err) {
    if (!s || s->hit_words != 5u) {  /* not built wide */
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT;
        return -1;
    }
    vk_pipeline *pipe = rtx_get_pipeline_wide(e);
    /* Single TLAS: trace it directly into `out`. */
    if (s->ntlas <= 1u)
        return rtx_trace_impl(e, s, pipe, s->tlas.as, rays, n, out, err);

    /* Multi-TLAS: trace slice 0 into `out` (its base is 0, so its instanceIds are
     * already global), then trace each remaining slice into a scratch buffer and
     * keep the nearer hit, offsetting the slice-local instanceId by tlas_base[k].
     * K sequential dispatches + a CPU merge -- offline throughput, no shader/
     * descriptor changes vs the single-TLAS wide path. */
    int r = rtx_trace_impl(e, s, pipe, s->tlas.as, rays, n, out, err);
    if (r < 0) return -1;
    lrt_hit_wide *tmp = (lrt_hit_wide *)malloc((size_t)n * sizeof(lrt_hit_wide));
    if (!tmp) {
        if (err) *err = LRT_RESULT_OUT_OF_MEMORY;
        return -1;
    }
    for (uint32_t k = 1; k < s->ntlas; k++) {
        if (rtx_trace_impl(e, s, pipe, s->extra_tlas[k - 1u].as, rays, n, tmp, err) <
            0) {
            free(tmp);
            return -1;
        }
        const uint32_t base = s->tlas_base[k];
        for (uint32_t i = 0; i < n; i++) {
            if (tmp[i].inst == LRT_TRI_NO_HIT) continue;
            if (out[i].inst == LRT_TRI_NO_HIT || tmp[i].t < out[i].t) {
                out[i] = tmp[i];
                out[i].inst += base;  /* slice-local -> global instance id */
            }
        }
    }
    free(tmp);
    int hits = 0;
    for (uint32_t i = 0; i < n; i++)
        if (out[i].inst != LRT_TRI_NO_HIT) hits++;
    if (err) *err = LRT_RESULT_OK;
    return hits;
}

typedef struct material_path_push {
    float inv_view_proj[16], camera[4], clear_color[4];
    float context[4];
    uint32_t frame[4], path[4];
} material_path_push;

static vk_pipeline *material_path_get_pipeline(lrt_vk_engine *e) {
    VkDescriptorType types[10];
    uint32_t i;
    if (e->material_path_pipe.valid) return &e->material_path_pipe;
    types[0] = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    for (i = 1; i < 10; ++i) types[i] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (!vk_pipeline_build(e, trace_materialx_path_spv,
                           sizeof(trace_materialx_path_spv), 10, types,
                           (uint32_t)sizeof(material_path_push), NULL,
                           &e->material_path_pipe)) return NULL;
    return &e->material_path_pipe;
}

static int material_path_bind(lrt_vk_engine *e, VkDescriptorSetLayout dsl,
                              VkAccelerationStructureKHR tlas,
                              const vk_buffer *buffers,
                              VkDescriptorPool *out_pool,
                              VkDescriptorSet *out_set) {
    VkDescriptorPoolSize ps[2];
    VkDescriptorPoolCreateInfo pci;
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorBufferInfo bi[9];
    VkWriteDescriptorSet w[10];
    VkWriteDescriptorSetAccelerationStructureKHR asw;
    uint32_t i;
    memset(ps, 0, sizeof(ps));
    ps[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    ps[0].descriptorCount = 1;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps[1].descriptorCount = 9;
    memset(&pci, 0, sizeof(pci));
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 1; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(e->device, &pci, NULL, out_pool) != VK_SUCCESS)
        return 0;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = *out_pool; ai.descriptorSetCount = 1;
    ai.pSetLayouts = &dsl;
    if (vkAllocateDescriptorSets(e->device, &ai, out_set) != VK_SUCCESS) {
        vkDestroyDescriptorPool(e->device, *out_pool, NULL); return 0;
    }
    memset(w, 0, sizeof(w)); memset(&asw, 0, sizeof(asw));
    asw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asw.accelerationStructureCount = 1; asw.pAccelerationStructures = &tlas;
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].pNext = &asw;
    w[0].dstSet = *out_set; w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    for (i = 0; i < 9; ++i) {
        bi[i].buffer = buffers[i].buf; bi[i].offset = 0; bi[i].range = VK_WHOLE_SIZE;
        w[i+1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i+1].dstSet = *out_set; w[i+1].dstBinding = i+1;
        w[i+1].descriptorCount = 1; w[i+1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i+1].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(e->device, 10, w, 0, NULL);
    return 1;
}

int lrt_vk_rtx_scene_render_materialx_path(
    lrt_vk_engine *e, lrt_vk_rtx_scene *s,
    const lrt_vk_material_path_desc *d, float *out_rgba, lrt_result *err) {
    vk_buffer b[9]; size_t sizes[9]; const void *data[9];
    VkDescriptorPool pool = VK_NULL_HANDLE; VkDescriptorSet set;
    material_path_push pc; vk_pipeline *pipe; uint32_t i, pixels; int ok = 0;
    const char *stage = "validate";
    if (!e || !s || !d || !out_rgba || !d->vertices || !d->indices ||
        !d->triangle_materials || !d->materials || !d->graphs || !d->width ||
        !d->height || !d->ntris || !d->nmaterials ||
        d->material_stride_floats < 80u || s->ntlas > 1u) {
        if (err) *err = LRT_RESULT_INVALID_ARGUMENT; return -1;
    }
    stage = "pipeline"; pipe = material_path_get_pipeline(e);
    if (!pipe) {
        if (e->err[0] == '\0') vk_set_err(e, "material path pipeline");
        if (err) *err = LRT_RESULT_NOT_BUILT; return -1;
    }
    pixels = d->width * d->height; memset(b, 0, sizeof(b));
    sizes[0]=(size_t)d->nverts*8u*sizeof(float); data[0]=d->vertices;
    sizes[1]=(size_t)d->ntris*3u*sizeof(uint32_t); data[1]=d->indices;
    sizes[2]=(size_t)d->ntris*sizeof(uint32_t); data[2]=d->triangle_materials;
    sizes[3]=(size_t)d->nmaterials*d->material_stride_floats*sizeof(float); data[3]=d->materials;
    sizes[4]=(size_t)d->nmaterials*d->graph_stride_floats*sizeof(float); data[4]=d->graphs;
    sizes[5]=(size_t)d->ntexels*sizeof(uint32_t); data[5]=d->texels;
    sizes[6]=(size_t)d->ntextures*8u*sizeof(int32_t); data[6]=d->texture_descs;
    /* Keep this in lockstep with lrt_vk_material_path_desc and
     * trace_materialx_path.comp: every analytic/environment light occupies
     * position/type, direction/radius, radiance/shape, and auxiliary fields
     * (16 floats). The old 8-float upload truncated radiance to zero and made
     * descriptor-backed Vulkan materials render black. */
    sizes[7]=(size_t)d->nlights*LRT_VK_MATERIAL_PATH_LIGHT_FLOATS*sizeof(float);
    data[7]=d->lights;
    sizes[8]=(size_t)pixels*4u*sizeof(float); data[8]=NULL;
    stage = "buffers"; for (i=0;i<9;++i) {
        size_t n=sizes[i]?sizes[i]:4u;
        if(!vk_buffer_create(e,n,&b[i])) goto done;
        if(sizes[i]&&data[i]&&!vk_buffer_write(e,&b[i],data[i],sizes[i])) goto done;
    }
    stage = "descriptors";
    if(!material_path_bind(e,pipe->dsl,s->tlas.as,b,&pool,&set)) goto done;
    memset(&pc,0,sizeof(pc)); memcpy(pc.inv_view_proj,d->inv_view_proj,sizeof(pc.inv_view_proj));
    memcpy(pc.camera,d->camera,3u*sizeof(float)); memcpy(pc.clear_color,d->clear_color,3u*sizeof(float));
    pc.clear_color[3]=d->exposure; pc.frame[0]=d->width; pc.frame[1]=d->height;
    pc.context[0]=d->scene_time; pc.context[1]=d->scene_frame;
    pc.frame[2]=d->samples?d->samples:1u; pc.frame[3]=d->max_depth?d->max_depth:1u;
    pc.path[0]=d->rr_depth; pc.path[1]=d->seed; pc.path[2]=d->nmaterials; pc.path[3]=d->nlights;
    stage = "dispatch";
    if(!vk_dispatch(e,pipe,set,&pc,(uint32_t)sizeof(pc),(pixels+63u)/64u)) goto done;
    stage = "readback";
    if(!vk_buffer_read(e,&b[8],out_rgba,sizes[8])) goto done; ok=1;
done:
    if (!ok && e->err[0] == '\0') vk_set_err(e, stage);
    if(pool!=VK_NULL_HANDLE&&!e->device_lost)vkDestroyDescriptorPool(e->device,pool,NULL);
    for(i=0;i<9;++i)vk_buffer_destroy(e,&b[i]);
    if(err)*err=ok?LRT_RESULT_OK:LRT_RESULT_OUT_OF_MEMORY;
    return ok?0:-1;
}

uint32_t lrt_vk_rtx_scene_ntlas(const lrt_vk_rtx_scene *s) {
    return s ? s->ntlas : 0u;
}

void lrt_vk_rtx_scene_free(lrt_vk_engine *e, lrt_vk_rtx_scene *s) {
    if (!e || !s) return;
    if (!e->device_lost) vkDeviceWaitIdle(e->device);
    vk_buffer_destroy(e, &s->rays_dev);
    vk_buffer_destroy(e, &s->hits_dev);
    vk_buffer_destroy(e, &s->rays_stage);
    vk_buffer_destroy(e, &s->hits_stage);
    vk_accel_destroy(e, &s->tlas);
    if (s->extra_tlas) {
        for (uint32_t k = 1; k < s->ntlas; k++)
            vk_accel_destroy(e, &s->extra_tlas[k - 1u]);
        free(s->extra_tlas);
    }
    free(s->tlas_base);
    if (s->blas) {
        /* Destroys only the AS handles; the underlying storage is pool-owned. */
        for (uint32_t c = 0; c < s->nblas; c++) vk_accel_destroy(e, &s->blas[c]);
        free(s->blas);
    }
    if (s->blas_input_pool) {
        vk_subpool_free(e, s->blas_input_pool);
        free(s->blas_input_pool);
    }
    if (s->blas_store_pool) {
        vk_subpool_free(e, s->blas_store_pool);
        free(s->blas_store_pool);
    }
    free(s);
}

/* One-shot convenience: build a resident scene, trace once, free. NOTE: this
 * rebuilds the whole acceleration structure on every call — fine for a single
 * batch, but do NOT call it once per ray/pixel in a render loop. For many
 * batches against the same geometry, build a resident lrt_vk_rtx_scene once and
 * call lrt_vk_rtx_scene_trace per batch. */
int lrt_vk_trace_scene_rtx(lrt_vk_engine *e, const float *vertices, uint32_t ntris,
                           const lrt_ray *rays, uint32_t n, lrt_hit *out,
                           lrt_result *err) {
    lrt_vk_rtx_scene *s = lrt_vk_rtx_scene_build(e, vertices, ntris, err);
    if (!s) return -1;
    int rc = lrt_vk_rtx_scene_trace(e, s, rays, n, out, err);
    lrt_vk_rtx_scene_free(e, s);
    return rc;
}

/* One-shot convenience for indexed geometry (see lrt_vk_rtx_scene_build_indexed).
 * Same per-call AS rebuild caveat as lrt_vk_trace_scene_rtx above. */
int lrt_vk_trace_scene_rtx_indexed(lrt_vk_engine *e, const float *vertices,
                                   uint32_t nverts, const uint32_t *indices,
                                   uint32_t ntris, const lrt_ray *rays, uint32_t n,
                                   lrt_hit *out, lrt_result *err) {
    lrt_vk_rtx_scene *s =
        lrt_vk_rtx_scene_build_indexed(e, vertices, nverts, indices, ntris, err);
    if (!s) return -1;
    int rc = lrt_vk_rtx_scene_trace(e, s, rays, n, out, err);
    lrt_vk_rtx_scene_free(e, s);
    return rc;
}
