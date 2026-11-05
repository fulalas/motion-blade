#include "vk_pipelines.h"

#include <string.h>

#include "vk_jobs.h"
#include "vk_pipeline_cache.h"

static const uint32_t SPV_NORMALIZE[] =
#include "normalize.spv.h"
    ;
static const uint32_t SPV_DOWNSAMPLE[] =
#include "downsample.spv.h"
    ;
static const uint32_t SPV_FULL_SEARCH[] =
#include "full_search.spv.h"
    ;
static const uint32_t SPV_ITERATE[] =
#include "iterate_search.spv.h"
    ;
static const uint32_t SPV_MEASURE[] =
#include "measure.spv.h"
    ;
static const uint32_t SPV_PROJECT[] =
#include "project.spv.h"
    ;
static const uint32_t SPV_CROSS_CHECK[] =
#include "cross_check.spv.h"
    ;
static const uint32_t SPV_LOCAL_SEARCH[] =
#include "local_search.spv.h"
    ;
static const uint32_t SPV_DECIDE_CUT[] =
#include "decide_cut.spv.h"
    ;
static const uint32_t SPV_SELECT[] =
#include "select.spv.h"
    ;
static const uint32_t SPV_MEDIAN[] =
#include "median.spv.h"
    ;
static const uint32_t SPV_FALLBACK[] =
#include "fallback.spv.h"
    ;
static const uint32_t SPV_CHROMA_OFFSETS[] =
#include "chroma_offsets.spv.h"
    ;
static const uint32_t SPV_OFFSET_MEDIAN[] =
#include "offset_median.spv.h"
    ;
static const uint32_t SPV_GLOBAL_MOTION[] =
#include "global_motion.spv.h"
    ;
static const uint32_t SPV_PANELS[] =
#include "panels.spv.h"
    ;
static const uint32_t SPV_PANEL_SPREAD[] =
#include "panel_spread.spv.h"
    ;
static const uint32_t SPV_SPLIT[] =
#include "split.spv.h"
    ;
static const uint32_t SPV_BAR_FLAGS[] =
#include "bar_flags.spv.h"
    ;
static const uint32_t SPV_INTERPOLATE_8[] =
#include "interpolate8.spv.h"
    ;
static const uint32_t SPV_INTERPOLATE_16[] =
#include "interpolate16.spv.h"
    ;
static const uint32_t SPV_INTERPOLATE_8_NV[] =
#include "interpolate8nv.spv.h"
    ;
static const uint32_t SPV_INTERPOLATE_16_NV[] =
#include "interpolate16nv.spv.h"
    ;
static const uint32_t SPV_EXTRAPOLATE_8[] =
#include "extrapolate8.spv.h"
    ;
static const uint32_t SPV_EXTRAPOLATE_16[] =
#include "extrapolate16.spv.h"
    ;
static const uint32_t SPV_EXTRAPOLATE_8_NV[] =
#include "extrapolate8nv.spv.h"
    ;
static const uint32_t SPV_EXTRAPOLATE_16_NV[] =
#include "extrapolate16nv.spv.h"
    ;

enum { MAX_BINDINGS = 32 };

typedef struct pass_info {
    const uint32_t *code;
    size_t size;
    const char *bindings;
    bool subgroup;
    int items_per_group;
    const char *name;
} pass_info;

#define SPIRV(code) code, sizeof(code)
#define INTERPOLATE_BINDINGS "SSSSSSBBBIIISSBBBBB"
#define EXTRAPOLATE_BINDINGS "SSSSSSBBBIIISSBBSS"

static const pass_info PASSES[PASS_COUNT] = {
    [PASS_NORMALIZE] = {SPIRV(SPV_NORMALIZE), "SI", false, 0, "normalize"},
    [PASS_DOWNSAMPLE] = {SPIRV(SPV_DOWNSAMPLE), "SI", false, 0, "downsample"},
    [PASS_FULL_SEARCH] = {SPIRV(SPV_FULL_SEARCH), "SSW", false, 1, "full_search"},
    [PASS_ITERATE] = {SPIRV(SPV_ITERATE), "SSBWBBBB", true, 4, "iterate"},
    [PASS_MEASURE] = {SPIRV(SPV_MEASURE), "SSWW", true, 4, "measure"},
    [PASS_PROJECT] = {SPIRV(SPV_PROJECT), "BW", false, 64, "project"},
    [PASS_CROSS_CHECK] = {SPIRV(SPV_CROSS_CHECK), "SSBWBB", true, 64, "cross_check"},
    [PASS_LOCAL_SEARCH] = {SPIRV(SPV_LOCAL_SEARCH), "SSBWBB", true, 64, "local_search"},
    [PASS_DECIDE_CUT] = {SPIRV(SPV_DECIDE_CUT), "WW", false, 1, "decide_cut"},
    [PASS_SELECT] = {SPIRV(SPV_SELECT), "SSSSBBBWBBBBB", true, 4, "select"},
    [PASS_MEDIAN] = {SPIRV(SPV_MEDIAN), "BW", false, 64, "median"},
    [PASS_FALLBACK] = {SPIRV(SPV_FALLBACK), "BWBB", false, 64, "fallback"},
    [PASS_CHROMA_OFFSETS] = {SPIRV(SPV_CHROMA_OFFSETS), "SSSSBW", false, 64, "chroma_offsets"},
    [PASS_OFFSET_MEDIAN] = {SPIRV(SPV_OFFSET_MEDIAN), "BW", false, 64, "offset_median"},
    [PASS_GLOBAL_MOTION] = {SPIRV(SPV_GLOBAL_MOTION), "BW", false, 1, "global_motion"},
    [PASS_PANELS] = {SPIRV(SPV_PANELS), "SSSSSSWB", false, 64, "panels"},
    [PASS_PANEL_SPREAD] = {SPIRV(SPV_PANEL_SPREAD), "BWBB", false, 64, "panel_spread"},
    [PASS_SPLIT] = {SPIRV(SPV_SPLIT), "SSBWB", true, 64, "split"},
    [PASS_BAR_FLAGS] = {SPIRV(SPV_BAR_FLAGS), "SSW", false, 0, "bar_flags"},
    [PASS_INTERPOLATE_8] = {SPIRV(SPV_INTERPOLATE_8), INTERPOLATE_BINDINGS, true, 0, "interpolate"},
    [PASS_INTERPOLATE_16] = {SPIRV(SPV_INTERPOLATE_16), INTERPOLATE_BINDINGS, true, 0, "interpolate"},
    [PASS_INTERPOLATE_8_NV] = {SPIRV(SPV_INTERPOLATE_8_NV), INTERPOLATE_BINDINGS, true, 0, "interpolate"},
    [PASS_INTERPOLATE_16_NV] = {SPIRV(SPV_INTERPOLATE_16_NV), INTERPOLATE_BINDINGS, true, 0, "interpolate"},
    [PASS_EXTRAPOLATE_8] = {SPIRV(SPV_EXTRAPOLATE_8), EXTRAPOLATE_BINDINGS, true, 0, "extrapolate"},
    [PASS_EXTRAPOLATE_16] = {SPIRV(SPV_EXTRAPOLATE_16), EXTRAPOLATE_BINDINGS, true, 0, "extrapolate"},
    [PASS_EXTRAPOLATE_8_NV] = {SPIRV(SPV_EXTRAPOLATE_8_NV), EXTRAPOLATE_BINDINGS, true, 0, "extrapolate"},
    [PASS_EXTRAPOLATE_16_NV] = {SPIRV(SPV_EXTRAPOLATE_16_NV), EXTRAPOLATE_BINDINGS, true, 0, "extrapolate"},
};

static VkDescriptorType descriptor_type(char kind)
{
    switch (kind) {
    case 'S':
        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case 'I':
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    default:
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
}

static mb_status create_layouts(mbvk_backend *vk, pipeline *p, const pass_info *info)
{
    int count = (int)strlen(info->bindings);
    VkDescriptorSetLayoutBinding bindings[MAX_BINDINGS];
    for (int i = 0; i < count; i++) {
        bindings[i] = (VkDescriptorSetLayoutBinding){(uint32_t)i, descriptor_type(info->bindings[i]), 1,
                                                     VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    }
    VkDescriptorSetLayoutCreateInfo set = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    set.bindingCount = (uint32_t)count;
    set.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(vk->device, &set, NULL, &p->set_layout) != VK_SUCCESS) {
        return MB_ERROR_DEVICE;
    }
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, PUSH_CONSTANT_BYTES};
    VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &p->set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &range;
    return vkCreatePipelineLayout(vk->device, &layout, NULL, &p->layout) == VK_SUCCESS ? MB_SUCCESS : MB_ERROR_DEVICE;
}

static mb_status create_pipeline(mbvk_backend *vk, pipeline *p, const pass_info *info)
{
    mb_status status = create_layouts(vk, p, info);
    VkShaderModule module;
    VkShaderModuleCreateInfo code = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, info->size, info->code};
    if (status != MB_SUCCESS || vkCreateShaderModule(vk->device, &code, NULL, &module) != VK_SUCCESS) {
        return MB_ERROR_DEVICE;
    }
    VkComputePipelineCreateInfo create = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    create.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                     VK_SHADER_STAGE_COMPUTE_BIT, module, "main", NULL};
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT size = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT,
        NULL,
        vk->features.subgroup_size,
    };
    if (info->subgroup && vk->features.subgroup_size) {
        create.stage.pNext = &size;
        if (vk->features.full_subgroups) {
            create.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT;
        }
    }
    create.layout = p->layout;
    VkResult r = vkCreateComputePipelines(vk->device, vk->pipeline_cache, 1, &create, NULL, &p->pipeline);
    vkDestroyShaderModule(vk->device, module, NULL);
    return r == VK_SUCCESS ? MB_SUCCESS : MB_ERROR_DEVICE;
}

static bool depends_on_format(pass_id pass)
{
    return pass >= PASS_INTERPOLATE_8;
}

mb_status pipelines_create(mbvk_backend *vk)
{
    pipeline_cache_open(vk);
    for (int i = 0; i < PASS_COUNT; i++) {
        if (depends_on_format((pass_id)i)) {
            continue;
        }
        mb_status status = create_pipeline(vk, &vk->pipelines[i], &PASSES[i]);
        if (status != MB_SUCCESS) {
            mb_log(&vk->logger, MB_LOG_ERROR, "could not create the %s pipeline", PASSES[i].name);
            return status;
        }
    }
    return MB_SUCCESS;
}

static void destroy_pipeline(mbvk_backend *vk, pipeline *p)
{
    if (p->pipeline) {
        vkDestroyPipeline(vk->device, p->pipeline, NULL);
    }
    if (p->layout) {
        vkDestroyPipelineLayout(vk->device, p->layout, NULL);
    }
    if (p->set_layout) {
        vkDestroyDescriptorSetLayout(vk->device, p->set_layout, NULL);
    }
    memset(p, 0, sizeof(*p));
}

void pipelines_destroy(mbvk_backend *vk)
{
    for (int i = 0; i < PASS_COUNT; i++) {
        destroy_pipeline(vk, &vk->pipelines[i]);
    }
    pipeline_cache_close(vk);
}

static const pipeline *ready_pipeline(mbvk_backend *vk, pass_id pass)
{
    pipeline *p = &vk->pipelines[pass];
    if (!p->pipeline && create_pipeline(vk, p, &PASSES[pass]) != MB_SUCCESS) {
        mb_log(&vk->logger, MB_LOG_ERROR, "could not create the %s pipeline", PASSES[pass].name);
        destroy_pipeline(vk, p);
        vk->healthy = false;
        return NULL;
    }
    return p;
}

typedef struct descriptor_writes {
    VkWriteDescriptorSet writes[MAX_BINDINGS];
    VkDescriptorImageInfo images[MAX_BINDINGS];
    VkDescriptorBufferInfo buffers[MAX_BINDINGS];
} descriptor_writes;

static bool is_buffer(char kind)
{
    return kind == 'B' || kind == 'W';
}

static resource_use use_of(const mbvk_backend *vk, char kind, const binding *b)
{
    if (is_buffer(kind)) {
        return (resource_use){buffer_handle(b->buffer ? b->buffer : &vk->dummy), kind == 'W'};
    }
    return (resource_use){image_handle(b->image ? b->image : &vk->dummy_image), kind == 'I'};
}

static void wait_for_inputs(mbvk_backend *vk, const char *kinds, const binding *bindings)
{
    resource_use uses[MAX_BINDINGS];
    int count = (int)strlen(kinds);
    for (int i = 0; i < count; i++) {
        uses[i] = use_of(vk, kinds[i], &bindings[i]);
    }
    jobs_prepare(vk, uses, count);
}

static void describe(mbvk_backend *vk, descriptor_writes *d, const char *kinds, const binding *bindings)
{
    int count = (int)strlen(kinds);
    for (int i = 0; i < count; i++) {
        VkWriteDescriptorSet *w = &d->writes[i];
        *w = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w->dstBinding = (uint32_t)i;
        w->descriptorCount = 1;
        w->descriptorType = descriptor_type(kinds[i]);
        if (is_buffer(kinds[i])) {
            const struct mb_buffer *b = bindings[i].buffer ? bindings[i].buffer : &vk->dummy;
            d->buffers[i] = (VkDescriptorBufferInfo){b->buffer, 0, VK_WHOLE_SIZE};
            w->pBufferInfo = &d->buffers[i];
        } else {
            const struct mb_image *image = bindings[i].image ? bindings[i].image : &vk->dummy_image;
            d->images[i] = (VkDescriptorImageInfo){vk->sampler, image->view, VK_IMAGE_LAYOUT_GENERAL};
            w->pImageInfo = &d->images[i];
        }
    }
}

void pipeline_dispatch(mbvk_backend *vk, const dispatch_desc *desc)
{
    const pipeline *p = ready_pipeline(vk, desc->pass);
    if (!p || !vk->recording) {
        return;
    }
    const char *kinds = PASSES[desc->pass].bindings;
    if (desc->binding_count != (int)strlen(kinds)) {
        mb_log(&vk->logger, MB_LOG_ERROR, "%s got %d bindings, needs %d", PASSES[desc->pass].name, desc->binding_count,
               (int)strlen(kinds));
        vk->healthy = false;
        return;
    }
    descriptor_writes d;
    describe(vk, &d, kinds, desc->bindings);
    wait_for_inputs(vk, kinds, desc->bindings);
    VkCommandBuffer commands = jobs_tracked_commands(vk);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
    vk->push_descriptor_set(commands, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, (uint32_t)strlen(kinds), d.writes);
    vkCmdPushConstants(commands, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)desc->push.bytes,
                       desc->push.data);
    vkCmdDispatch(commands, desc->groups_x, desc->groups_y, 1);
    if (vk->prof.enabled) {
        jobs_barrier(vk);
    }
    jobs_timestamp(vk, desc->name ? desc->name : PASSES[desc->pass].name);
}

pass_id warp_variant(pass_id first, bool interleaved, bool sixteen_bit)
{
    return (pass_id)(first + (interleaved ? 2 : 0) + (sixteen_bit ? 1 : 0));
}

void pipelines_prepare(mbvk_backend *vk, const mb_video_format *format)
{
    bool interleaved = format->interleaved_chroma;
    bool sixteen_bit = mb_bytes_per_sample(format) == 2;
    ready_pipeline(vk, warp_variant(PASS_INTERPOLATE_8, interleaved, sixteen_bit));
    ready_pipeline(vk, warp_variant(PASS_EXTRAPOLATE_8, interleaved, sixteen_bit));
}

uint32_t pipeline_groups(pass_id pass, int items)
{
    int per = PASSES[pass].items_per_group;
    return (uint32_t)((items + per - 1) / per);
}
