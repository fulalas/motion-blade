#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "memc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define MAXLEV 6
#define BS 8

static const uint32_t spv_down[] =
#include "shaders/down.h"
    ;
static const uint32_t spv_norm[] =
#include "shaders/norm.h"
    ;
static const uint32_t spv_full[] =
#include "shaders/me_full.h"
    ;
static const uint32_t spv_iter[] =
#include "shaders/me_iter.h"
    ;
static const uint32_t spv_refine[] =
#include "shaders/me_refine.h"
    ;
static const uint32_t spv_proj[] =
#include "shaders/tproj.h"
    ;
static const uint32_t spv_sel[] =
#include "shaders/tsel.h"
    ;
static const uint32_t spv_conf[] =
#include "shaders/conf.h"
    ;
static const uint32_t spv_illum[] =
#include "shaders/illum.h"
    ;
static const uint32_t spv_ilmed[] =
#include "shaders/ilmed.h"
    ;
static const uint32_t spv_interp8[] =
#include "shaders/interp8.h"
    ;
static const uint32_t spv_interp16[] =
#include "shaders/interp16.h"
    ;
static const uint32_t spv_interp8nv[] =
#include "shaders/interp8nv.h"
    ;
static const uint32_t spv_interp16nv[] =
#include "shaders/interp16nv.h"
    ;

typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    int w, h;
} vimg;

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    VkDeviceSize size;
} vbuf;

typedef struct {
    VkPipeline p;
    VkPipelineLayout pl;
    VkDescriptorSetLayout dsl;
    char types[32];
    const char *name;
} vpipe;

typedef struct {
    const vimg *im;
    const vbuf *b;
} vres;

typedef struct {
    int32_t i[16];
    float f[16];
} pcs;

typedef struct {
    float pen_parent, pen_temporal, pen_update, pen_zero, pen_cross, lam_full, dc_lo, dc_hi;
    int coarse_r, iters, iters0, refine, sel_passes;
    float pen_inc, sigma, pen_occ, pen_bad, wprior, cthr, pen_fb, fb_spread;
    float fg_sel, fg_pix, fb_ilo, fb_ihi;
    double scut, scut_rel, zcut_rel, zcut_min;
} params;

typedef struct {
    vimg y, c1, c2;
    vimg lev[MAXLEV];
} gframe;

#define NSLOT (MEMC_MAX_PENDING + 1)

typedef struct {
    VkCommandBuffer cb;
    VkFence fence;
    VkQueryPool qp;
    vbuf up, down, statcopy;
    vimg oy, oc1, oc2;
    int busy, has_out, first_of_pair, nq;
    double cut_thr, zcut_thr;
    const char *qname[64];
    double gpu_ms;
} slot;

struct memc_ctx {
    memc_format fmt;
    int bps, cw, ch, nplanes, nlev;
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue q;
    uint32_t qf;
    VkPhysicalDeviceMemoryProperties mp;
    VkCommandPool cp;
    VkSampler smp;
    slot sl[NSLOT];
    int cur, next_slot;
    int fifo[NSLOT], fifo_n;
    float ts_period;
    PFN_vkCmdPushDescriptorSetKHR pushdesc;
    int prof;
    double prof_ms[16];
    const char *prof_name[16];
    int prof_n;
    vpipe p_down, p_norm, p_full, p_iter, p_refine, p_proj, p_sel, p_conf, p_illum, p_ilmed, p_interp;
    gframe fr[2];
    vbuf fa[MAXLEV], fb[MAXLEV], pfa[MAXLEV], pfb[MAXLEV], tmp[MAXLEV];
    int lw[MAXLEV], lh[MAXLEV], bw[MAXLEV], bh[MAXLEV];
    vbuf cnt, slots, tf0, tf1, stat, dummy, fbw, ill0, ill1;
    size_t off[3], rowb[3], rows[3];
    params p;
    memc_stats st;
    int nfr, ib, me_valid, have_prev, lnorm, recording;
    vbuf *last_tf;
    double err_ema, z_ema, pair_thr, pair_zthr;
    int ok;
};

#define S(c) (&(c)->sl[(c)->cur])


#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "memc: %s failed (%d)\n", #x, r_); return -1; } } while (0)

static int memtype(memc_ctx *c, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < c->mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = c->mp.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & avoid))
            return (int)i;
    }
    for (uint32_t i = 0; i < c->mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (c->mp.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static int buf_create(memc_ctx *c, vbuf *b, VkDeviceSize size, int host)
{
    memset(b, 0, sizeof(*b));
    b->size = size;
    VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = size;
    bc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK(vkCreateBuffer(c->dev, &bc, NULL, &b->buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(c->dev, b->buf, &mr);
    int mt = host ? memtype(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0)
                  : memtype(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (host && mt < 0)
        mt = memtype(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    if (mt < 0)
        return -1;
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)mt};
    VK(vkAllocateMemory(c->dev, &ma, NULL, &b->mem));
    VK(vkBindBufferMemory(c->dev, b->buf, b->mem, 0));
    if (host)
        VK(vkMapMemory(c->dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map));
    return 0;
}

static void buf_free(memc_ctx *c, vbuf *b)
{
    if (b->buf)
        vkDestroyBuffer(c->dev, b->buf, NULL);
    if (b->mem)
        vkFreeMemory(c->dev, b->mem, NULL);
    memset(b, 0, sizeof(*b));
}

static int img_create(memc_ctx *c, vimg *im, int w, int h, VkFormat fmt)
{
    memset(im, 0, sizeof(*im));
    im->w = w;
    im->h = h;
    VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = fmt;
    ic.extent = (VkExtent3D){(uint32_t)w, (uint32_t)h, 1};
    ic.mipLevels = ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VK(vkCreateImage(c->dev, &ic, NULL, &im->img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(c->dev, im->img, &mr);
    int mt = memtype(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (mt < 0)
        return -1;
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)mt};
    VK(vkAllocateMemory(c->dev, &ma, NULL, &im->mem));
    VK(vkBindImageMemory(c->dev, im->img, im->mem, 0));
    VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = im->img;
    vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vc.format = fmt;
    vc.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK(vkCreateImageView(c->dev, &vc, NULL, &im->view));
    return 0;
}

static void img_free(memc_ctx *c, vimg *im)
{
    if (im->view)
        vkDestroyImageView(c->dev, im->view, NULL);
    if (im->img)
        vkDestroyImage(c->dev, im->img, NULL);
    if (im->mem)
        vkFreeMemory(c->dev, im->mem, NULL);
    memset(im, 0, sizeof(*im));
}

static int pipe_create(memc_ctx *c, vpipe *p, const uint32_t *code, size_t size, const char *types, const char *name)
{
    memset(p, 0, sizeof(*p));
    if (strlen(types) >= sizeof(p->types))
        return -1;
    strncpy(p->types, types, sizeof(p->types) - 1);
    p->name = name;
    int n = (int)strlen(types);
    VkDescriptorSetLayoutBinding b[32];
    for (int i = 0; i < n; i++) {
        b[i] = (VkDescriptorSetLayoutBinding){(uint32_t)i, 0, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        b[i].descriptorType = types[i] == 'S' ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                              : types[i] == 'I' ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = (uint32_t)n;
    dl.pBindings = b;
    VK(vkCreateDescriptorSetLayout(c->dev, &dl, NULL, &p->dsl));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcs)};
    VkPipelineLayoutCreateInfo plc = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plc.setLayoutCount = 1;
    plc.pSetLayouts = &p->dsl;
    plc.pushConstantRangeCount = 1;
    plc.pPushConstantRanges = &pcr;
    VK(vkCreatePipelineLayout(c->dev, &plc, NULL, &p->pl));
    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule sm;
    VK(vkCreateShaderModule(c->dev, &smi, NULL, &sm));
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                 VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL};
    ci.layout = p->pl;
    VkResult r = vkCreateComputePipelines(c->dev, VK_NULL_HANDLE, 1, &ci, NULL, &p->p);
    vkDestroyShaderModule(c->dev, sm, NULL);
    return r == VK_SUCCESS ? 0 : -1;
}

static void pipe_free(memc_ctx *c, vpipe *p)
{
    if (p->p)
        vkDestroyPipeline(c->dev, p->p, NULL);
    if (p->pl)
        vkDestroyPipelineLayout(c->dev, p->pl, NULL);
    if (p->dsl)
        vkDestroyDescriptorSetLayout(c->dev, p->dsl, NULL);
}

static void barrier(memc_ctx *c)
{
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
                          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                              VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(S(c)->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0,
                         NULL);
}

static void dispatch(memc_ctx *c, const vpipe *p, const vres *res, const pcs *pc, uint32_t gx, uint32_t gy)
{
    int n = (int)strlen(p->types);
    VkWriteDescriptorSet w[32];
    VkDescriptorImageInfo ii[32];
    VkDescriptorBufferInfo bi[32];
    for (int i = 0; i < n; i++) {
        w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        if (p->types[i] == 'B') {
            bi[i] = (VkDescriptorBufferInfo){res[i].b->buf, 0, VK_WHOLE_SIZE};
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &bi[i];
        } else {
            ii[i] = (VkDescriptorImageInfo){c->smp, res[i].im->view, VK_IMAGE_LAYOUT_GENERAL};
            w[i].descriptorType =
                p->types[i] == 'S' ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[i].pImageInfo = &ii[i];
        }
    }
    vkCmdBindPipeline(S(c)->cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->p);
    c->pushdesc(S(c)->cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pl, 0, (uint32_t)n, w);
    vkCmdPushConstants(S(c)->cb, p->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcs), pc);
    vkCmdDispatch(S(c)->cb, gx, gy, 1);
    barrier(c);
    if (c->prof && S(c)->nq < 63) {
        S(c)->qname[S(c)->nq] = p->name;
        vkCmdWriteTimestamp(S(c)->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, S(c)->qp, (uint32_t)(2 + S(c)->nq++));
    }
}

static void slot_wait(memc_ctx *c, int i)
{
    slot *sl = &c->sl[i];
    if (!sl->busy)
        return;
    vkWaitForFences(c->dev, 1, &sl->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(c->dev, 1, &sl->fence);
    sl->busy = 0;
    sl->gpu_ms = 0;
    uint64_t ts[66];
    if (vkGetQueryPoolResults(c->dev, sl->qp, 0, (uint32_t)(2 + sl->nq), sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT) !=
        VK_SUCCESS)
        return;
    uint64_t prev = ts[0];
    for (int k = 0; k < sl->nq; k++) {
        double ms = (double)(ts[2 + k] - prev) * c->ts_period / 1e6;
        prev = ts[2 + k];
        int j = 0;
        while (j < c->prof_n && strcmp(c->prof_name[j], sl->qname[k]))
            j++;
        if (j == c->prof_n && j < 16)
            c->prof_name[c->prof_n++] = sl->qname[k];
        if (j < 16)
            c->prof_ms[j] += ms;
    }
    sl->gpu_ms = (double)(ts[1] - ts[0]) * c->ts_period / 1e6;
}

static int cmd_begin(memc_ctx *c)
{
    if (c->recording)
        return 0;
    int i = c->next_slot;
    for (int k = 0; k < c->fifo_n; k++)
        if (c->fifo[k] == i) {
            slot_wait(c, i);
            memmove(&c->fifo[k], &c->fifo[k + 1], (size_t)(c->fifo_n - k - 1) * sizeof(int));
            c->fifo_n--;
            break;
        }
    slot_wait(c, i);
    c->cur = i;
    c->next_slot = (i + 1) % NSLOT;
    c->recording = 1;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VK(vkResetCommandBuffer(S(c)->cb, 0));
    VK(vkBeginCommandBuffer(S(c)->cb, &bi));
    vkCmdResetQueryPool(S(c)->cb, S(c)->qp, 0, 66);
    vkCmdWriteTimestamp(S(c)->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, S(c)->qp, 0);
    S(c)->nq = 0;
    S(c)->has_out = 0;
    S(c)->first_of_pair = 0;
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(S(c)->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         NULL, 0, NULL);
    return 0;
}

static int cmd_end(memc_ctx *c, int has_out)
{
    if (!c->recording)
        return 0;
    c->recording = 0;
    slot *sl = S(c);
    vkCmdWriteTimestamp(sl->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sl->qp, 1);
    if (vkEndCommandBuffer(sl->cb) != VK_SUCCESS)
        return -1;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &sl->cb;
    if (vkQueueSubmit(c->q, 1, &si, sl->fence) != VK_SUCCESS)
        return -1;
    sl->busy = 1;
    sl->has_out = has_out;
    if (has_out)
        c->fifo[c->fifo_n++] = c->cur;
    return 0;
}

static double cmd_submit(memc_ctx *c)
{
    int i = c->cur;
    if (cmd_end(c, 0) < 0)
        return -1;
    slot_wait(c, i);
    return c->sl[i].gpu_ms;
}

static void to_general(memc_ctx *c, vimg *im)
{
    VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = 0;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = im->img;
    ib.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(S(c)->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &ib);
}

static void copy_b2i(memc_ctx *c, const vbuf *b, size_t off, const vimg *im)
{
    VkBufferImageCopy r = {off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)im->w, (uint32_t)im->h, 1}};
    vkCmdCopyBufferToImage(S(c)->cb, b->buf, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}

static void copy_i2b(memc_ctx *c, const vimg *im, const vbuf *b, size_t off)
{
    VkBufferImageCopy r = {off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)im->w, (uint32_t)im->h, 1}};
    vkCmdCopyImageToBuffer(S(c)->cb, im->img, VK_IMAGE_LAYOUT_GENERAL, b->buf, 1, &r);
}

static int vk_init(memc_ctx *c)
{
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "memc", 1, "memc", 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai};
    VK(vkCreateInstance(&ii, NULL, &c->inst));
    uint32_t np = 16;
    VkPhysicalDevice pds[16];
    VK(vkEnumeratePhysicalDevices(c->inst, &np, pds));
    int best = -1, score = -1;
    for (uint32_t i = 0; i < np; i++) {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(pds[i], &pp);
        int s = pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 0
                                                                          : 1;
        if (s > score) {
            score = s;
            best = (int)i;
        }
    }
    if (best < 0)
        return -1;
    c->pd = pds[best];
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(c->pd, &pp);
    c->ts_period = pp.limits.timestampPeriod;
    if (getenv("MEMC_VERBOSE"))
        fprintf(stderr, "memc: using %s\n", pp.deviceName);
    vkGetPhysicalDeviceMemoryProperties(c->pd, &c->mp);
    uint32_t nq = 16;
    VkQueueFamilyProperties qfp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(c->pd, &nq, qfp);
    c->qf = UINT32_MAX;
    for (int pass = 0; pass < 2 && c->qf == UINT32_MAX; pass++)
        for (uint32_t i = 0; i < nq; i++) {
            VkQueueFlags f = qfp[i].queueFlags;
            if ((f & VK_QUEUE_COMPUTE_BIT) && qfp[i].timestampValidBits && (pass || !(f & VK_QUEUE_GRAPHICS_BIT))) {
                c->qf = i;
                break;
            }
        }
    if (c->qf == UINT32_MAX)
        return -1;
    uint32_t next = 0;
    vkEnumerateDeviceExtensionProperties(c->pd, NULL, &next, NULL);
    VkExtensionProperties *exts = calloc(next ? next : 1, sizeof(*exts));
    vkEnumerateDeviceExtensionProperties(c->pd, NULL, &next, exts);
    int has_prio = 0;
    for (uint32_t i = 0; i < next; i++)
        if (!strcmp(exts[i].extensionName, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME))
            has_prio = 1;
    free(exts);
    float pri = 1;
    VkDeviceQueueGlobalPriorityCreateInfoKHR gp = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR, NULL,
                                                   VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR};
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, c->qf, 1, &pri};
    const char *ext[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME};
    VkPhysicalDeviceFeatures feat = {0};
    feat.shaderStorageImageExtendedFormats = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.ppEnabledExtensionNames = ext;
    dci.pEnabledFeatures = &feat;
    VkResult res = VK_ERROR_INITIALIZATION_FAILED;
    if (has_prio) {
        qci.pNext = &gp;
        dci.enabledExtensionCount = 2;
        res = vkCreateDevice(c->pd, &dci, NULL, &c->dev);
    }
    if (res != VK_SUCCESS) {
        qci.pNext = NULL;
        dci.enabledExtensionCount = 1;
        VK(vkCreateDevice(c->pd, &dci, NULL, &c->dev));
    }
    if (getenv("MEMC_VERBOSE"))
        fprintf(stderr, "memc: queue family %u%s\n", c->qf, res == VK_SUCCESS ? ", low priority" : "");
    vkGetDeviceQueue(c->dev, c->qf, 0, &c->q);
    c->pushdesc = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->dev, "vkCmdPushDescriptorSetKHR");
    if (!c->pushdesc)
        return -1;
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, c->qf};
    VK(vkCreateCommandPool(c->dev, &cpi, NULL, &c->cp));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, c->cp,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 66, 0};
    for (int i = 0; i < NSLOT; i++) {
        VK(vkAllocateCommandBuffers(c->dev, &cai, &c->sl[i].cb));
        VK(vkCreateFence(c->dev, &fci, NULL, &c->sl[i].fence));
        VK(vkCreateQueryPool(c->dev, &qpi, NULL, &c->sl[i].qp));
    }
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK(vkCreateSampler(c->dev, &sci, NULL, &c->smp));
    c->prof = getenv("MEMC_PROFILE") != NULL;

#define PIPE(P, S, T) do { if (pipe_create(c, &c->P, S, sizeof(S), T, #P) < 0) return -1; } while (0)
    PIPE(p_down, spv_down, "SI");
    PIPE(p_norm, spv_norm, "SI");
    PIPE(p_full, spv_full, "SSB");
    PIPE(p_iter, spv_iter, "SSBBBBB");
    PIPE(p_refine, spv_refine, "SSBB");
    PIPE(p_proj, spv_proj, "BBB");
    PIPE(p_sel, spv_sel, "SSBBBBBB");
    PIPE(p_conf, spv_conf, "BBBB");
    PIPE(p_illum, spv_illum, "SSSSBB");
    PIPE(p_ilmed, spv_ilmed, "BB");
    const char *ti = "SSSSSSBBBIIISSBBB";
    if (c->fmt.nv) {
        if (c->bps == 1)
            PIPE(p_interp, spv_interp8nv, ti);
        else
            PIPE(p_interp, spv_interp16nv, ti);
    } else {
        if (c->bps == 1)
            PIPE(p_interp, spv_interp8, ti);
        else
            PIPE(p_interp, spv_interp16, ti);
    }
#undef PIPE
    return 0;
}

static int res_init(memc_ctx *c)
{
    VkFormat fy = c->bps == 1 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R16_UNORM;
    VkFormat fc = c->fmt.nv ? (c->bps == 1 ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R16G16_UNORM) : fy;
    int W = c->fmt.w, H = c->fmt.h;
    for (int k = 0; k < 2; k++) {
        gframe *f = &c->fr[k];
        if (img_create(c, &f->y, W, H, fy) < 0 || img_create(c, &f->c1, c->cw, c->ch, fc) < 0)
            return -1;
        if (!c->fmt.nv && img_create(c, &f->c2, c->cw, c->ch, fc) < 0)
            return -1;
        for (int L = c->lnorm ? 0 : 1; L < c->nlev; L++)
            if (img_create(c, &f->lev[L], c->lw[L], c->lh[L], VK_FORMAT_R16_SFLOAT) < 0)
                return -1;
    }
    for (int i = 0; i < NSLOT; i++) {
        slot *sl = &c->sl[i];
        if (img_create(c, &sl->oy, W, H, fy) < 0 || img_create(c, &sl->oc1, c->cw, c->ch, fc) < 0)
            return -1;
        if (!c->fmt.nv && img_create(c, &sl->oc2, c->cw, c->ch, fc) < 0)
            return -1;
    }
    for (int L = 0; L < c->nlev; L++) {
        VkDeviceSize sz = (VkDeviceSize)c->bw[L] * c->bh[L] * 16;
        if (buf_create(c, &c->fa[L], sz, 0) || buf_create(c, &c->fb[L], sz, 0) || buf_create(c, &c->pfa[L], sz, 0) ||
            buf_create(c, &c->pfb[L], sz, 0) || buf_create(c, &c->tmp[L], sz, 0))
            return -1;
    }
    int nb = c->bw[0] * c->bh[0];
    if (buf_create(c, &c->cnt, (VkDeviceSize)nb * 4, 0) || buf_create(c, &c->slots, (VkDeviceSize)nb * 4 * 8, 0) ||
        buf_create(c, &c->tf0, (VkDeviceSize)nb * 16, 0) || buf_create(c, &c->tf1, (VkDeviceSize)nb * 16, 0) ||
        buf_create(c, &c->stat, 64, 1) || buf_create(c, &c->dummy, 64, 0) ||
        buf_create(c, &c->fbw, (VkDeviceSize)nb * 4, 0) || buf_create(c, &c->ill0, (VkDeviceSize)nb * 16, 0) ||
        buf_create(c, &c->ill1, (VkDeviceSize)nb * 16, 0))
        return -1;
    int ncomp = c->fmt.nv ? 2 : 1;
    c->rowb[0] = (size_t)W * c->bps;
    c->rows[0] = H;
    c->rowb[1] = c->rowb[2] = (size_t)c->cw * ncomp * c->bps;
    c->rows[1] = c->rows[2] = c->ch;
    size_t off = 0;
    for (int i = 0; i < c->nplanes; i++) {
        c->off[i] = off;
        off += (c->rowb[i] * c->rows[i] + 255) & ~(size_t)255;
    }
    for (int i = 0; i < NSLOT; i++)
        if (buf_create(c, &c->sl[i].up, off, 1) || buf_create(c, &c->sl[i].down, off, 1) ||
            buf_create(c, &c->sl[i].statcopy, 64, 1))
            return -1;
    if (cmd_begin(c) < 0)
        return -1;
    for (int k = 0; k < 2; k++) {
        to_general(c, &c->fr[k].y);
        to_general(c, &c->fr[k].c1);
        if (!c->fmt.nv)
            to_general(c, &c->fr[k].c2);
        for (int L = c->lnorm ? 0 : 1; L < c->nlev; L++)
            to_general(c, &c->fr[k].lev[L]);
    }
    for (int i = 0; i < NSLOT; i++) {
        to_general(c, &c->sl[i].oy);
        to_general(c, &c->sl[i].oc1);
        if (!c->fmt.nv)
            to_general(c, &c->sl[i].oc2);
    }
    return cmd_submit(c) < 0 ? -1 : 0;
}

static float luma_scale(const memc_ctx *c)
{
    return c->lnorm ? (float)(65535.0 / ((1 << c->fmt.depth) - 1)) : 1.0f;
}

static void set_dc(const memc_ctx *c, pcs *pc)
{
    pc->f[14] = c->p.dc_lo;
    pc->f[15] = c->p.dc_hi;
}

static const vimg *lev_img(memc_ctx *c, int k, int L)
{
    return L == 0 && !c->lnorm ? &c->fr[k].y : &c->fr[k].lev[L];
}

void memc_push(memc_ctx *c, const memc_image *src)
{
    if (!c->ok)
        return;
    if (c->nfr >= 2 && c->me_valid) {
        for (int L = 0; L < c->nlev; L++) {
            vbuf t = c->fa[L];
            c->fa[L] = c->pfa[L];
            c->pfa[L] = t;
            t = c->fb[L];
            c->fb[L] = c->pfb[L];
            c->pfb[L] = t;
        }
        c->have_prev = 1;
    } else {
        c->have_prev = 0;
    }
    c->me_valid = 0;
    cmd_end(c, 0);
    cmd_begin(c);
    c->ib ^= 1;
    gframe *f = &c->fr[c->ib];
    for (int i = 0; i < c->nplanes; i++) {
        uint8_t *d = (uint8_t *)S(c)->up.map + c->off[i];
        for (size_t y = 0; y < c->rows[i]; y++)
            memcpy(d + y * c->rowb[i], src->data[i] + (ptrdiff_t)y * src->stride[i], c->rowb[i]);
    }
    cmd_begin(c);
    copy_b2i(c, &S(c)->up, c->off[0], &f->y);
    copy_b2i(c, &S(c)->up, c->off[1], &f->c1);
    if (!c->fmt.nv)
        copy_b2i(c, &S(c)->up, c->off[2], &f->c2);
    barrier(c);
    if (c->lnorm) {
        pcs pc = {{c->lw[0], c->lh[0]}, {luma_scale(c)}};
        vres r[2] = {{&f->y, NULL}, {&f->lev[0], NULL}};
        dispatch(c, &c->p_norm, r, &pc, (c->lw[0] + 15) / 16, (c->lh[0] + 15) / 16);
    }
    for (int L = 1; L < c->nlev; L++) {
        pcs pc = {{c->lw[L], c->lh[L], c->lw[L - 1], c->lh[L - 1]}, {0}};
        vres r[2] = {{lev_img(c, c->ib, L - 1), NULL}, {&f->lev[L], NULL}};
        dispatch(c, &c->p_down, r, &pc, (c->lw[L] + 15) / 16, (c->lh[L] + 15) / 16);
    }
    if (c->nfr < 2)
        c->nfr++;
}

static void estimate(memc_ctx *c, int dir)
{
    int kx = dir ? c->ib : c->ib ^ 1, ky = dir ? c->ib ^ 1 : c->ib;
    vbuf *F = dir ? c->fb : c->fa, *T = dir ? c->pfb : c->pfa;
    int top = c->nlev - 1;
    for (int L = top; L >= 0; L--) {
        const vimg *X = lev_img(c, kx, L), *Y = lev_img(c, ky, L);
        int nb = c->bw[L] * c->bh[L];
        int have = 0;
        if (L == top) {
            pcs pc = {{c->bw[L], c->bh[L], c->lw[L], c->lh[L], c->p.coarse_r}, {c->p.lam_full}};
            vres r[3] = {{X, NULL}, {Y, NULL}, {NULL, &F[L]}};
            dispatch(c, &c->p_full, r, &pc, (uint32_t)nb, 1);
            have = 1;
        }
        int niter = L == 0 ? c->p.iters0 : c->p.iters;
        for (int it = 0; it < niter; it++) {
            int flags = (have ? 1 : 0) | (L < top ? 2 : 0) | (c->have_prev ? 4 : 0) | (dir ? 8 : 0);
            pcs pc = {{c->bw[L], c->bh[L], c->lw[L], c->lh[L], L < top ? c->bw[L + 1] : 1, L < top ? c->bh[L + 1] : 1,
                       flags, it + L * 16 + dir * 256},
                      {c->p.pen_parent, c->p.pen_temporal, c->p.pen_update, c->p.pen_zero, c->p.pen_cross}};
            set_dc(c, &pc);
            vres r[7] = {{X, NULL},
                         {Y, NULL},
                         {NULL, &F[L]},
                         {NULL, &c->tmp[L]},
                         {NULL, L < top ? &F[L + 1] : &c->dummy},
                         {NULL, c->have_prev ? &T[L] : &c->dummy},
                         {NULL, dir ? &c->fa[L] : &c->dummy}};
            dispatch(c, &c->p_iter, r, &pc, (uint32_t)(nb + 3) / 4, 1);
            static const char *const iter_names[MAXLEV] = {"iter_L0", "iter_L1", "iter_L2", "iter_L3", "iter_L4", "iter_L5"};
            if (c->prof && S(c)->nq > 0)
                S(c)->qname[S(c)->nq - 1] = iter_names[L];
            vbuf t = F[L];
            F[L] = c->tmp[L];
            c->tmp[L] = t;
            have = 1;
        }
        if (L <= 1 && (c->p.refine || L == 0)) {
            pcs pc = {{c->bw[L], c->bh[L], c->lw[L], c->lh[L], L == 0 && dir == 0, c->p.refine}, {0}};
            set_dc(c, &pc);
            vres r[4] = {{X, NULL}, {Y, NULL}, {NULL, &F[L]}, {NULL, &c->stat}};
            dispatch(c, &c->p_refine, r, &pc, (uint32_t)(nb + 3) / 4, 1);
        }
    }
}

static void run_me(memc_ctx *c)
{
    cmd_begin(c);
    vkCmdFillBuffer(S(c)->cb, c->stat.buf, 0, VK_WHOLE_SIZE, 0);
    barrier(c);
    estimate(c, 0);
    estimate(c, 1);
    c->me_valid = 1;
}

static double cut_threshold(memc_ctx *c)
{
    double thr = c->p.scut;
    if (c->err_ema > 0) {
        double rel = c->p.scut_rel * c->err_ema + 4;
        thr = fmin(thr, fmax(10, rel));
    }
    return thr;
}

static double zcut_threshold(memc_ctx *c)
{
    return c->z_ema > 0 ? fmax(c->p.zcut_rel * c->z_ema, c->p.zcut_min) : 0;
}

static void read_out(memc_ctx *c, const slot *sl, memc_image *dst)
{
    for (int i = 0; i < c->nplanes; i++) {
        const uint8_t *s = (const uint8_t *)sl->down.map + c->off[i];
        for (size_t y = 0; y < c->rows[i]; y++)
            memcpy(dst->data[i] + (ptrdiff_t)y * dst->stride[i], s + y * c->rowb[i], c->rowb[i]);
    }
}

int memc_submit(memc_ctx *c, double t)
{
    if (!c->ok || c->nfr < 2)
        return -1;
    cmd_begin(c);
    int first = 0;
    if (!c->me_valid) {
        run_me(c);
        c->pair_thr = cut_threshold(c);
        c->pair_zthr = zcut_threshold(c);
        first = 1;
    }
    double cut_sum = c->pair_thr * c->bw[0] * c->bh[0] * 256, zcut_sum = c->pair_zthr * c->bw[0] * c->bh[0] * 256;
    gframe *A = &c->fr[c->ib ^ 1], *B = &c->fr[c->ib];
    int bw = c->bw[0], bh = c->bh[0], nb = bw * bh;
    vkCmdFillBuffer(S(c)->cb, c->cnt.buf, 0, VK_WHOLE_SIZE, 0);
    barrier(c);
    for (int dir = 0; dir < 2; dir++) {
        pcs pc = {{bw, bh, 0, 0, dir}, {(float)(dir ? 1 - t : t)}};
        vres r[3] = {{NULL, dir ? &c->fb[0] : &c->fa[0]}, {NULL, &c->cnt}, {NULL, &c->slots}};
        dispatch(c, &c->p_proj, r, &pc, (uint32_t)(nb + 63) / 64, 1);
    }
    vbuf *tfin = &c->dummy, *tfout = &c->tf0;
    for (int pass = 0; pass < c->p.sel_passes; pass++) {
        pcs pc = {{bw, bh, c->fmt.w, c->fmt.h, pass}, {(float)t, c->p.pen_inc, c->p.cthr, c->p.fg_sel}};
        set_dc(c, &pc);
        vres r[8] = {{lev_img(c, c->ib ^ 1, 0), NULL}, {lev_img(c, c->ib, 0), NULL},
                     {NULL, &c->fa[0]},      {NULL, &c->fb[0]},
                     {NULL, &c->cnt},        {NULL, &c->slots},
                     {NULL, tfin}, {NULL, tfout}};
        dispatch(c, &c->p_sel, r, &pc, (uint32_t)(nb + 3) / 4, 1);
        tfin = tfout;
        tfout = tfout == &c->tf0 ? &c->tf1 : &c->tf0;
    }
    c->last_tf = tfin;
    {
        pcs pc = {{bw, bh}, {0, 0, c->p.fb_spread, 0, (float)t, c->p.fb_ilo, c->p.fb_ihi}};
        vres r[4] = {{NULL, tfin}, {NULL, &c->fbw}, {NULL, &c->fa[0]}, {NULL, &c->fb[0]}};
        dispatch(c, &c->p_conf, r, &pc, (uint32_t)(nb + 63) / 64, 1);
    }
    const vimg *ac2 = c->fmt.nv ? &A->c1 : &A->c2, *bc2 = c->fmt.nv ? &B->c1 : &B->c2;
    {
        pcs pc = {{c->fmt.w, c->fmt.h, bw, bh, c->cw, c->ch, c->fmt.cshift_x, c->fmt.cshift_y, c->fmt.nv}, {(float)t, luma_scale(c)}};
        vres r[6] = {{&A->c1, NULL}, {&B->c1, NULL}, {ac2, NULL}, {bc2, NULL}, {NULL, tfin}, {NULL, &c->ill0}};
        dispatch(c, &c->p_illum, r, &pc, (uint32_t)(nb + 63) / 64, 1);
        pcs pm = {{bw, bh}};
        vres rm[2] = {{NULL, &c->ill0}, {NULL, &c->ill1}};
        dispatch(c, &c->p_ilmed, rm, &pm, (uint32_t)(nb + 63) / 64, 1);
    }
    {
        pcs pc = {{c->fmt.w, c->fmt.h, bw, bh, c->cw, c->ch, c->fmt.cshift_x, c->fmt.cshift_y},
                  {(float)t, c->p.sigma, c->p.pen_occ, c->p.pen_bad, c->p.pen_fb, c->p.wprior, c->p.cthr, (float)cut_sum,
                   luma_scale(c), (float)zcut_sum, 0, 0, c->p.fg_pix, 0, 0}};
        vres r[17] = {{&A->y, NULL},  {&B->y, NULL},  {&A->c1, NULL},   {&B->c1, NULL},
                      {ac2, NULL},    {bc2, NULL},    {NULL, tfin},  {NULL, &c->fa[0]},
                      {NULL, &c->fb[0]}, {&S(c)->oy, NULL}, {&S(c)->oc1, NULL}, {c->fmt.nv ? &S(c)->oc1 : &S(c)->oc2, NULL},
                      {lev_img(c, c->ib ^ 1, 0), NULL}, {lev_img(c, c->ib, 0), NULL}, {NULL, &c->stat}, {NULL, &c->fbw}, {NULL, &c->ill1}};
        dispatch(c, &c->p_interp, r, &pc, (uint32_t)((c->fmt.w + 1) / 2 + 7) / 8, (uint32_t)((c->fmt.h + 1) / 2 + 7) / 8);
    }
    copy_i2b(c, &S(c)->oy, &S(c)->down, c->off[0]);
    copy_i2b(c, &S(c)->oc1, &S(c)->down, c->off[1]);
    if (!c->fmt.nv)
        copy_i2b(c, &S(c)->oc2, &S(c)->down, c->off[2]);
    VkBufferCopy bc = {0, 0, 64};
    vkCmdCopyBuffer(S(c)->cb, c->stat.buf, S(c)->statcopy.buf, 1, &bc);
    S(c)->first_of_pair = first;
    S(c)->cut_thr = c->pair_thr;
    S(c)->zcut_thr = c->pair_zthr;
    return cmd_end(c, 1);
}

int memc_debug_field(memc_ctx *c, int which, float *out, int *w, int *h)
{
    vbuf *f = which == 0 ? &c->fa[0] : which == 1 ? &c->fb[0] : c->last_tf;
    if (!f || !f->buf)
        return -1;
    int n = c->bw[0] * c->bh[0];
    vbuf tmp;
    if (buf_create(c, &tmp, (VkDeviceSize)n * 16, 1) < 0)
        return -1;
    cmd_begin(c);
    VkBufferCopy bc = {0, 0, (VkDeviceSize)n * 16};
    vkCmdCopyBuffer(S(c)->cb, f->buf, tmp.buf, 1, &bc);
    cmd_submit(c);
    memcpy(out, tmp.map, (size_t)n * 16);
    *w = c->bw[0];
    *h = c->bh[0];
    buf_free(c, &tmp);
    return 0;
}

int memc_pending(memc_ctx *c)
{
    return c->fifo_n;
}

int memc_ready(memc_ctx *c)
{
    return c->fifo_n && vkGetFenceStatus(c->dev, c->sl[c->fifo[0]].fence) == VK_SUCCESS;
}

int memc_retrieve(memc_ctx *c, memc_image *dst)
{
    if (!c->fifo_n)
        return -1;
    int i = c->fifo[0];
    memmove(&c->fifo[0], &c->fifo[1], (size_t)(c->fifo_n - 1) * sizeof(int));
    c->fifo_n--;
    slot *sl = &c->sl[i];
    slot_wait(c, i);
    double e = ((uint32_t *)sl->statcopy.map)[0] / ((double)c->bw[0] * c->bh[0] * 256);
    double z = ((uint32_t *)sl->statcopy.map)[1] / ((double)c->bw[0] * c->bh[0] * 256);
    int ecut = e > sl->cut_thr, cut = ecut && z > sl->zcut_thr;
    if (sl->first_of_pair && !ecut)
        c->err_ema = c->err_ema > 0 ? c->err_ema * 0.8 + e * 0.2 : e;
    if (sl->first_of_pair && !cut)
        c->z_ema = c->z_ema > 0 ? c->z_ema * 0.8 + z * 0.2 : z;
    c->st.mean_err = e;
    c->st.scene_cut = cut;
    c->st.me_ms = 0;
    c->st.interp_ms = sl->gpu_ms;
    read_out(c, sl, dst);
    return cut;
}

int memc_interpolate(memc_ctx *c, double t, memc_image *dst)
{
    if (memc_submit(c, t) < 0)
        return -1;
    return memc_retrieve(c, dst);
}

void memc_reset(memc_ctx *c)
{
    cmd_end(c, 0);
    for (int i = 0; i < NSLOT; i++)
        slot_wait(c, i);
    c->fifo_n = 0;
    c->nfr = 0;
    c->me_valid = 0;
    c->have_prev = 0;
    c->err_ema = 0;
    c->z_ema = 0;
    c->st.scene_cut = 0;
}

static void default_params(params *p)
{
    p->pen_parent = 4;
    p->pen_temporal = 8;
    p->pen_update = 16;
    p->pen_zero = 8;
    p->pen_cross = 4;
    p->lam_full = 2;
    p->dc_lo = 0.5f;
    p->dc_hi = 1;
    p->coarse_r = 16;
    p->iters = 3;
    p->iters0 = 1;
    p->refine = 0;
    p->sel_passes = 1;
    p->pen_inc = 256;
    p->sigma = 24;
    p->pen_occ = 96;
    p->pen_bad = 256;
    p->wprior = 0.25f;
    p->cthr = 8;
    p->pen_fb = 1e6f;
    p->fb_spread = 0.7f;
    p->fb_ilo = 2;
    p->fb_ihi = 6;
    p->fg_sel = 1024;
    p->fg_pix = 60;
    p->scut = 40;
    p->scut_rel = 4;
    p->zcut_rel = 2;
    p->zcut_min = 1;
}

int memc_set_param(memc_ctx *c, const char *name, double v)
{
    params *p = &c->p;
#define P_(N) if (!strcmp(name, #N)) { p->N = v; return 0; }
    P_(pen_parent) P_(pen_temporal) P_(pen_update) P_(pen_zero) P_(pen_cross) P_(lam_full) P_(dc_lo) P_(dc_hi)
    P_(coarse_r) P_(iters) P_(iters0) P_(refine) P_(sel_passes) P_(pen_inc) P_(sigma) P_(pen_occ) P_(pen_bad)
    P_(wprior) P_(cthr) P_(pen_fb) P_(fb_spread) P_(fg_sel) P_(fg_pix) P_(fb_ilo) P_(fb_ihi) P_(scut) P_(scut_rel) P_(zcut_rel) P_(zcut_min)
#undef P_
    return -1;
}

void memc_get_stats(memc_ctx *c, memc_stats *st)
{
    *st = c->st;
}

void memc_destroy(memc_ctx *c)
{
    if (!c)
        return;
    if (c->prof)
        for (int k = 0; k < c->prof_n; k++)
            fprintf(stderr, "memc profile: %-10s %8.1f ms total\n", c->prof_name[k], c->prof_ms[k]);
    if (c->dev) {
        vkDeviceWaitIdle(c->dev);
        for (int k = 0; k < 2; k++) {
            img_free(c, &c->fr[k].y);
            img_free(c, &c->fr[k].c1);
            img_free(c, &c->fr[k].c2);
            for (int L = 0; L < MAXLEV; L++)
                img_free(c, &c->fr[k].lev[L]);
        }
        for (int i = 0; i < NSLOT; i++) {
            slot *sl = &c->sl[i];
            img_free(c, &sl->oy);
            img_free(c, &sl->oc1);
            img_free(c, &sl->oc2);
            buf_free(c, &sl->up);
            buf_free(c, &sl->down);
            buf_free(c, &sl->statcopy);
            if (sl->qp)
                vkDestroyQueryPool(c->dev, sl->qp, NULL);
            if (sl->fence)
                vkDestroyFence(c->dev, sl->fence, NULL);
        }
        for (int L = 0; L < MAXLEV; L++) {
            buf_free(c, &c->fa[L]);
            buf_free(c, &c->fb[L]);
            buf_free(c, &c->pfa[L]);
            buf_free(c, &c->pfb[L]);
            buf_free(c, &c->tmp[L]);
        }
        buf_free(c, &c->cnt);
        buf_free(c, &c->slots);
        buf_free(c, &c->tf0);
        buf_free(c, &c->tf1);
        buf_free(c, &c->stat);
        buf_free(c, &c->dummy);
        buf_free(c, &c->fbw);
        buf_free(c, &c->ill0);
        buf_free(c, &c->ill1);
        pipe_free(c, &c->p_down);
        pipe_free(c, &c->p_norm);
        pipe_free(c, &c->p_full);
        pipe_free(c, &c->p_iter);
        pipe_free(c, &c->p_refine);
        pipe_free(c, &c->p_proj);
        pipe_free(c, &c->p_sel);
        pipe_free(c, &c->p_conf);
        pipe_free(c, &c->p_illum);
        pipe_free(c, &c->p_ilmed);
        pipe_free(c, &c->p_interp);
        if (c->smp)
            vkDestroySampler(c->dev, c->smp, NULL);
        if (c->cp)
            vkDestroyCommandPool(c->dev, c->cp, NULL);
        vkDestroyDevice(c->dev, NULL);
    }
    if (c->inst)
        vkDestroyInstance(c->inst, NULL);
    free(c);
}

memc_ctx *memc_create(const memc_format *fmt, int threads)
{
    (void)threads;
    if (fmt->w < 64 || fmt->h < 64 || fmt->depth < 8 || fmt->depth > 16)
        return NULL;
    memc_ctx *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fmt = *fmt;
    c->bps = fmt->depth > 8 ? 2 : 1;
    c->cw = (fmt->w + (1 << fmt->cshift_x) - 1) >> fmt->cshift_x;
    c->ch = (fmt->h + (1 << fmt->cshift_y) - 1) >> fmt->cshift_y;
    c->nplanes = fmt->nv ? 2 : 3;
    c->lnorm = c->bps == 2 && fmt->depth < 16;
    default_params(&c->p);
    int nlev = 1;
    while (nlev < MAXLEV && (fmt->w >> nlev) >= 200 && (fmt->h >> nlev) >= 64)
        nlev++;
    c->nlev = nlev;
    for (int L = 0; L < nlev; L++) {
        c->lw[L] = (fmt->w + (1 << L) - 1) >> L;
        c->lh[L] = (fmt->h + (1 << L) - 1) >> L;
        c->bw[L] = (c->lw[L] + BS - 1) / BS;
        c->bh[L] = (c->lh[L] + BS - 1) / BS;
    }
    if (vk_init(c) < 0 || res_init(c) < 0) {
        fprintf(stderr, "memc: Vulkan init failed\n");
        memc_destroy(c);
        return NULL;
    }
    c->ok = 1;
    return c;
}
