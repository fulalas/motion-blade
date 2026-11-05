#include "vk_pipelines.h"

#include <string.h>

#include "vk_jobs.h"

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
#define INTERPOLATE_BINDINGS "SSSSSSBBBIIISSBBBBBBBBBSS"

static const pass_info PASSES[PASS_COUNT] = {
    [PASS_NORMALIZE] = {SPIRV(SPV_NORMALIZE), "SI", false, 0, "normalize"},
    [PASS_DOWNSAMPLE] = {SPIRV(SPV_DOWNSAMPLE), "SI", false, 0, "downsample"},
    [PASS_FULL_SEARCH] = {SPIRV(SPV_FULL_SEARCH), "SSB", false, 1, "full_search"},
    [PASS_ITERATE] = {SPIRV(SPV_ITERATE), "SSBBBBBB", true, 4, "iterate"},
    [PASS_MEASURE] = {SPIRV(SPV_MEASURE), "SSBB", true, 4, "measure"},
    [PASS_PROJECT] = {SPIRV(SPV_PROJECT), "BB", false, 64, "project"},
    [PASS_CROSS_CHECK] = {SPIRV(SPV_CROSS_CHECK), "SSBBBB", true, 64, "cross_check"},
    [PASS_LOCAL_SEARCH] = {SPIRV(SPV_LOCAL_SEARCH), "SSBBBB", true, 64, "local_search"},
    [PASS_DECIDE_CUT] = {SPIRV(SPV_DECIDE_CUT), "BB", false, 1, "decide_cut"},
    [PASS_SELECT] = {SPIRV(SPV_SELECT), "SSSSBBBBBBBBB", true, 4, "select"},
    [PASS_MEDIAN] = {SPIRV(SPV_MEDIAN), "BB", false, 64, "median"},
    [PASS_FALLBACK] = {SPIRV(SPV_FALLBACK), "BBBB", false, 64, "fallback"},
    [PASS_CHROMA_OFFSETS] = {SPIRV(SPV_CHROMA_OFFSETS), "SSSSBB", false, 64, "chroma_offsets"},
    [PASS_OFFSET_MEDIAN] = {SPIRV(SPV_OFFSET_MEDIAN), "BB", false, 64, "offset_median"},
    [PASS_GLOBAL_MOTION] = {SPIRV(SPV_GLOBAL_MOTION), "BB", false, 1, "global_motion"},
    [PASS_PANELS] = {SPIRV(SPV_PANELS), "SSSSSSBB", false, 64, "panels"},
    [PASS_PANEL_SPREAD] = {SPIRV(SPV_PANEL_SPREAD), "BBBB", false, 64, "panel_spread"},
    [PASS_SPLIT] = {SPIRV(SPV_SPLIT), "SSBBB", true, 64, "split"},
    [PASS_BAR_FLAGS] = {SPIRV(SPV_BAR_FLAGS), "SSB", false, 0, "bar_flags"},
    [PASS_INTERPOLATE_8] = {SPIRV(SPV_INTERPOLATE_8), INTERPOLATE_BINDINGS, true, 0, "interpolate"},
    [PASS_INTERPOLATE_16] = {SPIRV(SPV_INTERPOLATE_16), INTERPOLATE_BINDINGS, true, 0, "interpolate"},
    [PASS_INTERPOLATE_8_NV] = {SPIRV(SPV_INTERPOLATE_8_NV), INTERPOLATE_BINDINGS, true, 0, "interpolate"},
    [PASS_INTERPOLATE_16_NV] = {SPIRV(SPV_INTERPOLATE_16_NV), INTERPOLATE_BINDINGS, true, 0, "interpolate"},
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
    VkResult r = vkCreateComputePipelines(vk->device, VK_NULL_HANDLE, 1, &create, NULL, &p->pipeline);
    vkDestroyShaderModule(vk->device, module, NULL);
    return r == VK_SUCCESS ? MB_SUCCESS : MB_ERROR_DEVICE;
}

static bool is_interpolate(pass_id pass)
{
    return pass >= PASS_INTERPOLATE_8;
}

mb_status pipelines_create(mbvk_backend *vk)
{
    for (int i = 0; i < PASS_COUNT; i++) {
        if (is_interpolate((pass_id)i)) {
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

static void describe(mbvk_backend *vk, descriptor_writes *d, const char *kinds, const binding *bindings)
{
    int count = (int)strlen(kinds);
    for (int i = 0; i < count; i++) {
        VkWriteDescriptorSet *w = &d->writes[i];
        *w = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w->dstBinding = (uint32_t)i;
        w->descriptorCount = 1;
        w->descriptorType = descriptor_type(kinds[i]);
        if (kinds[i] == 'B') {
            const struct mb_buffer *b = bindings[i].buffer ? bindings[i].buffer : &vk->dummy;
            d->buffers[i] = (VkDescriptorBufferInfo){b->buffer, 0, VK_WHOLE_SIZE};
            w->pBufferInfo = &d->buffers[i];
        } else {
            d->images[i] = (VkDescriptorImageInfo){vk->sampler, bindings[i].image->view, VK_IMAGE_LAYOUT_GENERAL};
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
    descriptor_writes d;
    describe(vk, &d, kinds, desc->bindings);
    VkCommandBuffer commands = jobs_commands(vk);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
    vk->push_descriptor_set(commands, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, (uint32_t)strlen(kinds), d.writes);
    vkCmdPushConstants(commands, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)desc->push.bytes,
                       desc->push.data);
    vkCmdDispatch(commands, desc->groups_x, desc->groups_y, 1);
    jobs_barrier(vk);
    jobs_timestamp(vk, desc->name ? desc->name : PASSES[desc->pass].name);
}

uint32_t pipeline_groups(pass_id pass, int items)
{
    int per = PASSES[pass].items_per_group;
    return (uint32_t)((items + per - 1) / per);
}
