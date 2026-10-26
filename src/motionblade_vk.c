#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "motionblade.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#define MAXLEV 6
#define BS 8
#define STAT_SCALE 16.0

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
static const uint32_t spv_cross[] =
#include "shaders/me_cross.h"
    ;
static const uint32_t spv_local[] =
#include "shaders/me_local.h"
    ;
static const uint32_t spv_proj[] =
#include "shaders/tproj.h"
    ;
static const uint32_t spv_sel[] =
#include "shaders/tsel.h"
    ;
static const uint32_t spv_vmed[] =
#include "shaders/vmed.h"
    ;
static const uint32_t spv_sub[] =
#include "shaders/tsub.h"
    ;
static const uint32_t spv_conf[] =
#include "shaders/conf.h"
    ;
static const uint32_t spv_cutdec[] =
#include "shaders/cutdec.h"
    ;
static const uint32_t spv_bars[] =
#include "shaders/bars.h"
    ;
static const uint32_t spv_agree[] =
#include "shaders/agree.h"
    ;
static const uint32_t spv_gmed[] =
#include "shaders/gmed.h"
    ;
static const uint32_t spv_agspread[] =
#include "shaders/agspread.h"
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
    float pen_parent, pen_temporal, pen_update, pen_zero, pen_cross, pen_len, lam_full, dc_lo, dc_hi, skip;
    int coarse_r, iters, iters0, refine, sel_passes;
    float pen_inc, sigma, pen_occ, pen_bad, wprior, cthr, pen_fb, fb_spread, fb_avg;
    float fg_sel, fg_pix, fb_ilo, fb_ihi, pen_one, one_rel, one_min, one_tex, one_sw, pen_fbm, unc_min;
    int uncov, unc_cont, xfix, tsup, xloc, tex, tref, twaive, xchk;
    float tsup_rel, tsup_abs, tsup_pen, tsup_min, xbad, xnoise, xr, xs;
    float pen_xfix, xrel, xabs, xmin, vm_thr, ws_lo, ws_hi, sub_thr, pen_sub, pen_sref;
    int sub_ref;
    int la, bar_wait, bars, tz_pen;
    float ag_lo, ag_hi, ag_flat, ag_sat, ag_sw, ag_src;
    int ag_r;
    double scut, scut_rel, zcut_rel, zcut_min;
    float cut_sd, cut_kmin;
} params;

typedef struct {
    vimg y, c1, c2;
    vimg lev[MAXLEV];
} gframe;

#define NSLOT (MB_MAX_PENDING + 1)
#define MAXX 32

typedef struct {
    vbuf b;
    int fd, ver;
} xbuf;

typedef struct {
    atomic_int refs;
    atomic_int busy[MAXX];
} xpool;

#define MAXIMP 128

typedef struct {
    unsigned long long dev, ino;
    uint32_t off;
    vimg im;
    unsigned used;
} impimg;
#define NFR 4

typedef struct {
    VkCommandBuffer cb;
    VkFence fence;
    VkQueryPool qp;
    vbuf down, statcopy;
    int x0, y0, w, h, xidx;
    void (*rel[4])(void *);
    void *relo[4];
    int nrel;
    int busy, nq;
    unsigned serial;
    const char *qname[64];
    double gpu_ms;
} slot;

struct mb_ctx {
    mb_format fmt, full;
    int bps, cw, ch, nplanes, nlev;
    int x0, y0, nx0, ny0, nw, nh, nstable, seen, scan, npush, det_push;
    uint8_t *tmpl;
    size_t toff[3], trowb[3], trows[3], tsize;
    int has_dmabuf, nx, bars_ver;
    PFN_vkGetMemoryFdKHR getfd;
    xbuf xb[MAXX];
    xpool *xp;
    size_t xoff[3], xpitch[3], xsize;
    int has_mod, has_foreign, gpu_in, nimp, bf_slot, bf_pending, bf_tick, bg_ver;
    unsigned clock, bf_serial;
    PFN_vkGetMemoryFdPropertiesKHR getfdprops;
    impimg imp[MAXIMP];
    vimg bgy, bgc;
    vbuf bflags;
    vpipe p_bars, p_agree, p_gmed, p_agspread;
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
    double prof_ms[32];
    const char *prof_name[32];
    int prof_n;
    vpipe p_down, p_norm, p_full, p_iter, p_refine, p_cross, p_local, p_proj, p_sel, p_vmed, p_sub, p_conf, p_cutdec, p_illum, p_ilmed, p_interp;
    gframe fr[NFR];
    vbuf fa[MAXLEV], fb[MAXLEV], pfa[MAXLEV], pfb[MAXLEV], tmp[MAXLEV], qfb;
    int lw[MAXLEV], lh[MAXLEV], bw[MAXLEV], bh[MAXLEV];
    vbuf tf4, slots, xkeys, eslots, tf0, tf1, stat, pstat, qstat, dummy, fbw, agw, agw2, gmv, ill0, ill1, hist;
    size_t off[3], rowb[3], rows[3];
    params p;
    mb_stats st;
    int nfr, ib, me_valid, have_prev, have_q, eos, lnorm, recording;
    vbuf *last_tf, last_fa, last_fb;
    vimg oy, oc1, oc2;
    vbuf up[2];
    int up_k, up_slot[2];
    unsigned up_serial[2], serial;
    uint32_t max_groups;
    int hist_clear;
    int ok;
    uint32_t sg_size;
};

#define S(c) (&(c)->sl[(c)->cur])


#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "motionblade: %s failed (%d)\n", #x, r_); return -1; } } while (0)

static int memtype(mb_ctx *c, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
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

static int buf_create(mb_ctx *c, vbuf *b, VkDeviceSize size, int host)
{
    memset(b, 0, sizeof(*b));
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

static void buf_free(mb_ctx *c, vbuf *b)
{
    if (b->buf)
        vkDestroyBuffer(c->dev, b->buf, NULL);
    if (b->mem)
        vkFreeMemory(c->dev, b->mem, NULL);
    memset(b, 0, sizeof(*b));
}

static int img_create(mb_ctx *c, vimg *im, int w, int h, VkFormat fmt)
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

static void img_free(mb_ctx *c, vimg *im)
{
    if (im->view)
        vkDestroyImageView(c->dev, im->view, NULL);
    if (im->img)
        vkDestroyImage(c->dev, im->img, NULL);
    if (im->mem)
        vkFreeMemory(c->dev, im->mem, NULL);
    memset(im, 0, sizeof(*im));
}

static int pipe_create(mb_ctx *c, vpipe *p, const uint32_t *code, size_t size, const char *types, const char *name,
                       int sg)
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
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT rs = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT, NULL, c->sg_size};
    if (sg && c->sg_size)
        ci.stage.pNext = &rs;
    ci.layout = p->pl;
    VkResult r = vkCreateComputePipelines(c->dev, VK_NULL_HANDLE, 1, &ci, NULL, &p->p);
    vkDestroyShaderModule(c->dev, sm, NULL);
    return r == VK_SUCCESS ? 0 : -1;
}

static void pipe_free(mb_ctx *c, vpipe *p)
{
    if (p->p)
        vkDestroyPipeline(c->dev, p->p, NULL);
    if (p->pl)
        vkDestroyPipelineLayout(c->dev, p->pl, NULL);
    if (p->dsl)
        vkDestroyDescriptorSetLayout(c->dev, p->dsl, NULL);
}

static void barrier(mb_ctx *c)
{
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
                          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                              VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(S(c)->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0,
                         NULL);
}

static void host_barrier(mb_ctx *c)
{
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(S(c)->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0,
                         NULL);
}

static void dispatch(mb_ctx *c, const vpipe *p, const vres *res, const pcs *pc, uint32_t gx, uint32_t gy)
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

static void slot_release(slot *sl)
{
    for (int k = 0; k < sl->nrel; k++)
        sl->rel[k](sl->relo[k]);
    sl->nrel = 0;
}

static void slot_wait(mb_ctx *c, int i)
{
    slot *sl = &c->sl[i];
    if (!sl->busy)
        return;
    vkWaitForFences(c->dev, 1, &sl->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(c->dev, 1, &sl->fence);
    sl->busy = 0;
    slot_release(sl);
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
        if (j == c->prof_n && j < 32)
            c->prof_name[c->prof_n++] = sl->qname[k];
        if (j < 32)
            c->prof_ms[j] += ms;
    }
    sl->gpu_ms = (double)(ts[1] - ts[0]) * c->ts_period / 1e6;
}

static int in_fifo(const mb_ctx *c, int i)
{
    for (int k = 0; k < c->fifo_n; k++)
        if (c->fifo[k] == i)
            return 1;
    return 0;
}

static int cmd_begin(mb_ctx *c)
{
    if (c->recording)
        return 0;
    int i = -1;
    for (int k = 0; k < NSLOT && i < 0; k++)
        if (!in_fifo(c, (c->next_slot + k) % NSLOT))
            i = (c->next_slot + k) % NSLOT;
    if (i < 0)
        return -1;
    slot_wait(c, i);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    if (vkResetCommandBuffer(c->sl[i].cb, 0) != VK_SUCCESS || vkBeginCommandBuffer(c->sl[i].cb, &bi) != VK_SUCCESS) {
        fprintf(stderr, "motionblade: could not begin command buffer\n");
        c->ok = 0;
        return -1;
    }
    c->cur = i;
    c->next_slot = (i + 1) % NSLOT;
    c->recording = 1;
    c->sl[i].xidx = -1;
    vkCmdResetQueryPool(S(c)->cb, S(c)->qp, 0, 66);
    vkCmdWriteTimestamp(S(c)->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, S(c)->qp, 0);
    S(c)->nq = 0;
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(S(c)->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         NULL, 0, NULL);
    return 0;
}

static int cmd_end(mb_ctx *c, int has_out)
{
    if (!c->recording)
        return 0;
    c->recording = 0;
    slot *sl = S(c);
    vkCmdWriteTimestamp(sl->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sl->qp, 1);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &sl->cb;
    if (vkEndCommandBuffer(sl->cb) != VK_SUCCESS || vkQueueSubmit(c->q, 1, &si, sl->fence) != VK_SUCCESS) {
        fprintf(stderr, "motionblade: could not submit command buffer\n");
        c->ok = 0;
        return -1;
    }
    sl->busy = 1;
    sl->serial = ++c->serial;
    if (has_out)
        c->fifo[c->fifo_n++] = c->cur;
    return 0;
}

static double cmd_submit(mb_ctx *c)
{
    int i = c->cur;
    if (cmd_end(c, 0) < 0)
        return -1;
    slot_wait(c, i);
    return c->sl[i].gpu_ms;
}

static void to_general(mb_ctx *c, vimg *im)
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

static void copy_b2i(mb_ctx *c, const vbuf *b, size_t off, const vimg *im)
{
    VkBufferImageCopy r = {off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)im->w, (uint32_t)im->h, 1}};
    vkCmdCopyBufferToImage(S(c)->cb, b->buf, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}

static void copy_i2b(mb_ctx *c, const vimg *im, const vbuf *b, size_t off)
{
    VkBufferImageCopy r = {off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)im->w, (uint32_t)im->h, 1}};
    vkCmdCopyImageToBuffer(S(c)->cb, im->img, VK_IMAGE_LAYOUT_GENERAL, b->buf, 1, &r);
}

static int vk_init(mb_ctx *c)
{
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "motionblade", 1, "motionblade", 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai};
    VK(vkCreateInstance(&ii, NULL, &c->inst));
    uint32_t np = 16;
    VkPhysicalDevice pds[16];
    VkResult er = vkEnumeratePhysicalDevices(c->inst, &np, pds);
    if (er != VK_SUCCESS && er != VK_INCOMPLETE) {
        fprintf(stderr, "motionblade: vkEnumeratePhysicalDevices failed (%d)\n", er);
        return -1;
    }
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
    c->max_groups = pp.limits.maxComputeWorkGroupCount[0];
    int verbose = getenv("MOTIONBLADE_VERBOSE") != NULL;
    if (verbose)
        fprintf(stderr, "motionblade: using %s\n", pp.deviceName);
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
    if (!exts)
        return -1;
    vkEnumerateDeviceExtensionProperties(c->pd, NULL, &next, exts);
    int has_prio = 0, has_sgctl = 0, has_fd = 0, has_dma = 0, has_mod = 0, has_foreign = 0;
    for (uint32_t i = 0; i < next; i++) {
        if (!strcmp(exts[i].extensionName, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME))
            has_mod = 1;
        if (!strcmp(exts[i].extensionName, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME))
            has_foreign = 1;
        if (!strcmp(exts[i].extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME))
            has_fd = 1;
        if (!strcmp(exts[i].extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME))
            has_dma = 1;
        if (!strcmp(exts[i].extensionName, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME))
            has_prio = 1;
        if (!strcmp(exts[i].extensionName, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME))
            has_sgctl = 1;
    }
    free(exts);

    if (pp.apiVersion < VK_API_VERSION_1_1) {
        fprintf(stderr, "motionblade: %s does not support Vulkan 1.1\n", pp.deviceName);
        return -1;
    }
    VkPhysicalDeviceSubgroupSizeControlPropertiesEXT sgc = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES_EXT};
    VkPhysicalDeviceSubgroupProperties sgp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
                                              has_sgctl ? &sgc : NULL};
    VkPhysicalDeviceProperties2 pp2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sgp};
    vkGetPhysicalDeviceProperties2(c->pd, &pp2);
    VkSubgroupFeatureFlags need = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_CLUSTERED_BIT;
    if ((sgp.supportedOperations & need) != need || !(sgp.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT)) {
        fprintf(stderr, "motionblade: %s lacks clustered subgroup operations\n", pp.deviceName);
        return -1;
    }
    uint32_t sg_min = has_sgctl ? sgc.minSubgroupSize : sgp.subgroupSize;
    int can_req = has_sgctl && (sgc.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT);
    if (sg_min >= 16 && can_req && sgc.minSubgroupSize <= 32 && sgc.maxSubgroupSize >= 32 &&
        sgc.maxComputeWorkgroupSubgroups >= 2) {
        c->sg_size = 32;
    } else if (sg_min < 16) {
        if (!can_req || sgc.maxSubgroupSize < 16 || sgc.maxComputeWorkgroupSubgroups < 4) {
            fprintf(stderr, "motionblade: %s cannot run 16-wide subgroups\n", pp.deviceName);
            return -1;
        }
        c->sg_size = 16;
    }

    float pri = 1;
    VkDeviceQueueGlobalPriorityCreateInfoKHR gp = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR, NULL,
                                                   VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR};
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, c->qf, 1, &pri};
    const char *ext[8] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    uint32_t next_base = 1;
    (void)has_fd, (void)has_dma, (void)has_mod, (void)has_foreign;
#ifndef _WIN32
    if (has_fd && has_dma) {
        ext[next_base++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        ext[next_base++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
        c->has_dmabuf = 1;
        if (has_mod && pp.apiVersion >= VK_API_VERSION_1_2) {
            ext[next_base++] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
            c->has_mod = 1;
            if (has_foreign) {
                ext[next_base++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
                c->has_foreign = 1;
            }
        }
    }
#endif
    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT sgf = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT, NULL, VK_TRUE, VK_FALSE};
    VkPhysicalDeviceFeatures feat = {0};
    feat.shaderStorageImageExtendedFormats = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.ppEnabledExtensionNames = ext;
    dci.pEnabledFeatures = &feat;
    if (c->sg_size) {
        ext[next_base++] = VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME;
        dci.pNext = &sgf;
    }
    VkResult res = VK_ERROR_INITIALIZATION_FAILED;
    if (has_prio) {
        qci.pNext = &gp;
        ext[next_base] = VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME;
        dci.enabledExtensionCount = next_base + 1;
        res = vkCreateDevice(c->pd, &dci, NULL, &c->dev);
    }
    if (res != VK_SUCCESS) {
        qci.pNext = NULL;
        dci.enabledExtensionCount = next_base;
        VK(vkCreateDevice(c->pd, &dci, NULL, &c->dev));
    }
    if (verbose)
        fprintf(stderr, "motionblade: queue family %u%s\n", c->qf, res == VK_SUCCESS ? ", low priority" : "");
    vkGetDeviceQueue(c->dev, c->qf, 0, &c->q);
    c->pushdesc = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->dev, "vkCmdPushDescriptorSetKHR");
    if (c->has_dmabuf) {
        c->getfd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(c->dev, "vkGetMemoryFdKHR");
        c->getfdprops = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(c->dev, "vkGetMemoryFdPropertiesKHR");
        if (!c->getfdprops)
            c->has_mod = 0;
    }
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
    c->prof = getenv("MOTIONBLADE_PROFILE") != NULL;

#define PIPE(P, S, T) do { if (pipe_create(c, &c->P, S, sizeof(S), T, #P, 0) < 0) return -1; } while (0)
#define PIPE_SG(P, S, T) do { if (pipe_create(c, &c->P, S, sizeof(S), T, #P, 1) < 0) return -1; } while (0)
    PIPE(p_down, spv_down, "SI");
    PIPE(p_norm, spv_norm, "SI");
    PIPE(p_full, spv_full, "SSB");
    PIPE_SG(p_iter, spv_iter, "SSBBBBBB");
    PIPE_SG(p_refine, spv_refine, "SSBB");
    PIPE(p_proj, spv_proj, "BB");
    PIPE_SG(p_cross, spv_cross, "SSBBBB");
    PIPE_SG(p_local, spv_local, "SSBBBB");
    PIPE_SG(p_sel, spv_sel, "SSSSBBBBBBBBBB");
    PIPE(p_vmed, spv_vmed, "BB");
    PIPE_SG(p_sub, spv_sub, "SSBBB");
    PIPE(p_conf, spv_conf, "BBBB");
    PIPE(p_cutdec, spv_cutdec, "BB");
    PIPE(p_illum, spv_illum, "SSSSBB");
    PIPE(p_ilmed, spv_ilmed, "BB");
    PIPE(p_bars, spv_bars, "SSB");
    PIPE(p_agree, spv_agree, "SSSSSSBB");
    PIPE(p_gmed, spv_gmed, "BB");
    PIPE(p_agspread, spv_agspread, "BBBB");
    const char *ti = "SSSSSSBBBIIISSBBBBB";
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
#undef PIPE_SG
    return 0;
}

static int cdim(int v, int s)
{
    return (v + (1 << s) - 1) >> s;
}

static VkFormat plane_fmt(const mb_ctx *c, int comps)
{
    if (comps == 2)
        return c->bps == 1 ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R16G16_UNORM;
    return c->bps == 1 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R16_UNORM;
}

static size_t layout(const mb_ctx *c, int w, int h, size_t off[3], size_t rowb[3], size_t rows[3])
{
    int ncomp = c->fmt.nv ? 2 : 1;
    int cw = cdim(w, c->fmt.cshift_x), ch = cdim(h, c->fmt.cshift_y);
    rowb[0] = (size_t)w * c->bps;
    rows[0] = h;
    rowb[1] = rowb[2] = (size_t)cw * ncomp * c->bps;
    rows[1] = rows[2] = ch;
    size_t o = 0;
    for (int i = 0; i < c->nplanes; i++) {
        off[i] = o;
        o += (rowb[i] * rows[i] + 255) & ~(size_t)255;
    }
    return o;
}

static void set_geom(mb_ctx *c)
{
    const mb_format *fmt = &c->fmt;
    c->cw = cdim(fmt->w, fmt->cshift_x);
    c->ch = cdim(fmt->h, fmt->cshift_y);
    int nlev = 1;
    while (nlev < MAXLEV && (fmt->w >> nlev) >= 48 && (fmt->h >> nlev) >= 48)
        nlev++;
    c->nlev = nlev;
    for (int L = 0; L < nlev; L++) {
        c->lw[L] = (fmt->w + (1 << L) - 1) >> L;
        c->lh[L] = (fmt->h + (1 << L) - 1) >> L;
        c->bw[L] = (c->lw[L] + BS - 1) / BS;
        c->bh[L] = (c->lh[L] + BS - 1) / BS;
    }
    layout(c, fmt->w, fmt->h, c->off, c->rowb, c->rows);
}

static int res_fixed_init(mb_ctx *c)
{
    c->tsize = layout(c, c->full.w, c->full.h, c->toff, c->trowb, c->trows);
    for (int i = 0; i < NSLOT; i++)
        if (buf_create(c, &c->sl[i].down, c->tsize, 1) || buf_create(c, &c->sl[i].statcopy, 64, 1))
            return -1;
    for (int k = 0; k < 2; k++) {
        if (buf_create(c, &c->up[k], c->tsize, 1))
            return -1;
        c->up_slot[k] = -1;
    }
    return 0;
}

static void frames_free(mb_ctx *c, gframe *fr)
{
    for (int k = 0; k < NFR; k++) {
        img_free(c, &fr[k].y);
        img_free(c, &fr[k].c1);
        img_free(c, &fr[k].c2);
        for (int L = 0; L < MAXLEV; L++)
            img_free(c, &fr[k].lev[L]);
    }
}

static void res_geom_free(mb_ctx *c)
{
    frames_free(c, c->fr);
    for (int L = 0; L < MAXLEV; L++) {
        buf_free(c, &c->fa[L]);
        buf_free(c, &c->fb[L]);
        buf_free(c, &c->pfa[L]);
        buf_free(c, &c->pfb[L]);
        buf_free(c, &c->tmp[L]);
    }
    img_free(c, &c->oy);
    img_free(c, &c->oc1);
    img_free(c, &c->oc2);
    vbuf *bufs[] = {&c->slots, &c->xkeys, &c->eslots, &c->tf4,  &c->tf0, &c->tf1,  &c->stat, &c->pstat,
                    &c->qstat, &c->qfb,   &c->dummy,  &c->fbw, &c->agw, &c->agw2, &c->gmv, &c->ill0, &c->ill1, &c->hist};
    for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++)
        buf_free(c, bufs[i]);
}

static int res_geom_init(mb_ctx *c)
{
    VkFormat fy = plane_fmt(c, 1), fc = plane_fmt(c, c->fmt.nv ? 2 : 1);
    int W = c->fmt.w, H = c->fmt.h;
    for (int k = 0; k < NFR; k++) {
        gframe *f = &c->fr[k];
        if (img_create(c, &f->y, W, H, fy) < 0 || img_create(c, &f->c1, c->cw, c->ch, fc) < 0)
            return -1;
        if (!c->fmt.nv && img_create(c, &f->c2, c->cw, c->ch, fc) < 0)
            return -1;
        for (int L = c->lnorm ? 0 : 1; L < c->nlev; L++)
            if (img_create(c, &f->lev[L], c->lw[L], c->lh[L], VK_FORMAT_R16_SFLOAT) < 0)
                return -1;
    }
    if (img_create(c, &c->oy, W, H, fy) < 0 || img_create(c, &c->oc1, c->cw, c->ch, fc) < 0)
        return -1;
    if (!c->fmt.nv && img_create(c, &c->oc2, c->cw, c->ch, fc) < 0)
        return -1;
    for (int L = 0; L < c->nlev; L++) {
        VkDeviceSize sz = (VkDeviceSize)c->bw[L] * c->bh[L] * 16;
        if (buf_create(c, &c->fa[L], sz, 0) || buf_create(c, &c->fb[L], sz, 0) || buf_create(c, &c->pfa[L], sz, 0) ||
            buf_create(c, &c->pfb[L], sz, 0) || buf_create(c, &c->tmp[L], sz, 0))
            return -1;
    }
    int nb = c->bw[0] * c->bh[0];
    if (buf_create(c, &c->qfb, (VkDeviceSize)nb * 16, 0) || buf_create(c, &c->pstat, 64, 0) ||
        buf_create(c, &c->qstat, 64, 0))
        return -1;
    if (buf_create(c, &c->slots, (VkDeviceSize)nb * 4 * 4, 0) || buf_create(c, &c->xkeys, (VkDeviceSize)nb * 4 * 4, 0) ||
        buf_create(c, &c->eslots, (VkDeviceSize)nb * 4 * 4, 0) || buf_create(c, &c->tf4, (VkDeviceSize)nb * 64, 0) ||
        buf_create(c, &c->tf0, (VkDeviceSize)nb * 16, 0) || buf_create(c, &c->tf1, (VkDeviceSize)nb * 16, 0) ||
        buf_create(c, &c->stat, 64, 0) || buf_create(c, &c->dummy, 64, 0) || buf_create(c, &c->hist, 128, 0) ||
        buf_create(c, &c->fbw, (VkDeviceSize)nb * 4, 0) || buf_create(c, &c->agw, (VkDeviceSize)nb * 8, 0) ||
        buf_create(c, &c->agw2, (VkDeviceSize)nb * 4, 0) || buf_create(c, &c->gmv, 16, 0) ||
        buf_create(c, &c->ill0, (VkDeviceSize)nb * 16, 0) ||
        buf_create(c, &c->ill1, (VkDeviceSize)nb * 16, 0))
        return -1;
    if (cmd_begin(c) < 0)
        return -1;
    for (int k = 0; k < NFR; k++) {
        to_general(c, &c->fr[k].y);
        to_general(c, &c->fr[k].c1);
        if (!c->fmt.nv)
            to_general(c, &c->fr[k].c2);
        for (int L = c->lnorm ? 0 : 1; L < c->nlev; L++)
            to_general(c, &c->fr[k].lev[L]);
    }
    to_general(c, &c->oy);
    to_general(c, &c->oc1);
    if (!c->fmt.nv)
        to_general(c, &c->oc2);
    return cmd_submit(c) < 0 ? -1 : 0;
}

static float luma_scale(const mb_ctx *c)
{
    return c->lnorm ? (float)(65535.0 / ((1 << c->fmt.depth) - 1)) : 1.0f;
}

static void out_range(const mb_ctx *c, float *maxv, float *step)
{
    int pad = c->fmt.lsb_pad;
    *maxv = c->bps == 1 ? 1.0f
            : c->lnorm  ? (float)(((1 << c->fmt.depth) - 1) / 65535.0)
                        : (float)((65535 - ((1 << pad) - 1)) / 65535.0);
    *step = pad ? (float)((1 << pad) / 65535.0) : 0.0f;
}

static void set_dc(const mb_ctx *c, pcs *pc)
{
    pc->f[14] = c->p.dc_lo;
    pc->f[15] = c->p.dc_hi;
}

static int fidx(const mb_ctx *c, int back)
{
    return (c->ib - back + NFR) % NFR;
}

static const vimg *lev_img(mb_ctx *c, int k, int L)
{
    return L == 0 && !c->lnorm ? &c->fr[k].y : &c->fr[k].lev[L];
}

static void run_me(mb_ctx *c);

static void swapbuf(vbuf *a, vbuf *b)
{
    vbuf t = *a;
    *a = *b;
    *b = t;
}

static void rot3(vbuf *q, vbuf *p, vbuf *n)
{
    vbuf t = *q;
    *q = *p;
    *p = *n;
    *n = t;
}

static unsigned px(const mb_ctx *c, const mb_image *src, int i, int x, int y)
{
    const uint8_t *r = src->data[i] + (ptrdiff_t)y * src->stride[i];
    return c->bps == 1 ? r[x] : ((const uint16_t *)r)[x];
}

static int black_span(const mb_ctx *c, const mb_image *src, int y, int x0, int x1)
{
    unsigned sc = c->bps == 1 ? 1 : c->lnorm ? 1u << (c->full.depth - 8) : 256;
    unsigned ymax = 24 * sc, lo = 118 * sc, hi = 138 * sc;
    int sx = c->full.cshift_x, cy = y >> c->full.cshift_y;
    for (int x = x0; x < x1; x += 2) {
        if (px(c, src, 0, x, y) > ymax)
            return 0;
        int cx = x >> sx;
        for (int k = 0; k < 2; k++) {
            unsigned v = c->full.nv ? px(c, src, 1, cx * 2 + k, cy) : px(c, src, 1 + k, cx, cy);
            if (v < lo || v > hi)
                return 0;
        }
    }
    return 1;
}

static int black_col(const mb_ctx *c, const mb_image *src, int x, int y0, int y1)
{
    for (int y = y0; y < y1; y += 2)
        if (!black_span(c, src, y, x, x + 1))
            return 0;
    return 1;
}

typedef struct {
    const mb_image *src;
    const uint32_t *fl;
} barsrc;

static int brow(const mb_ctx *c, const barsrc *b, int y)
{
    return b->fl ? b->fl[y] != 0 : black_span(c, b->src, y, 0, c->full.w);
}

static int bcol(const mb_ctx *c, const barsrc *b, int x, int y0, int y1)
{
    return b->fl ? b->fl[c->full.h + x] != 0 : black_col(c, b->src, x, y0, y1);
}

static int detect_bars(const mb_ctx *c, const barsrc *b, int *rx, int *ry, int *rw, int *rh)
{
    int W = c->full.w, H = c->full.h;
    int top = 0, bot = H, l = 0, r = W;
    while (top < H / 4 && brow(c, b, top))
        top++;
    while (bot > H - H / 4 && brow(c, b, bot - 1))
        bot--;
    while (l < W / 4 && bcol(c, b, l, top, bot))
        l++;
    while (r > W - W / 4 && bcol(c, b, r - 1, top, bot))
        r--;
    if (top == H / 4 || bot == H - H / 4 || l == W / 4 || r == W - W / 4)
        return -1;
    int x0 = l & ~7, y0 = top & ~7, x1 = (r + 7) & ~7, y1 = (bot + 7) & ~7;
    x1 = x1 > W ? W : x1;
    y1 = y1 > H ? H : y1;
    if ((x1 - x0) & 1 || (y1 - y0) & 1 || (double)(x1 - x0) * (y1 - y0) > 0.96 * W * H)
        x0 = y0 = 0, x1 = W, y1 = H;
    *rx = x0, *ry = y0, *rw = x1 - x0, *rh = y1 - y0;
    return 0;
}

static void plane_org(const mb_ctx *c, int i, int x0, int y0, size_t *xb, size_t *yr)
{
    int ncomp = c->fmt.nv ? 2 : 1;
    *xb = i ? (size_t)(x0 >> c->fmt.cshift_x) * ncomp * c->bps : (size_t)x0 * c->bps;
    *yr = i ? (size_t)(y0 >> c->fmt.cshift_y) : (size_t)y0;
}

static void copy_border(const mb_ctx *c, int i, int x0, int y0, size_t rowb, size_t rows, uint8_t *d, ptrdiff_t ds,
                        const uint8_t *s, ptrdiff_t ss)
{
    size_t xb, yr;
    plane_org(c, i, x0, y0, &xb, &yr);
    for (size_t y = 0; y < c->trows[i]; y++, d += ds, s += ss) {
        if (y < yr || y >= yr + rows) {
            memcpy(d, s, c->trowb[i]);
        } else {
            memcpy(d, s, xb);
            memcpy(d + xb + rowb, s + xb + rowb, c->trowb[i] - xb - rowb);
        }
    }
}

static void build_levels(mb_ctx *c, int k)
{
    gframe *f = &c->fr[k];
    if (c->lnorm) {
        pcs pc = {{c->lw[0], c->lh[0]}, {luma_scale(c)}};
        vres r[2] = {{&f->y, NULL}, {&f->lev[0], NULL}};
        dispatch(c, &c->p_norm, r, &pc, (c->lw[0] + 15) / 16, (c->lh[0] + 15) / 16);
    }
    for (int L = 1; L < c->nlev; L++) {
        pcs pc = {{c->lw[L], c->lh[L], c->lw[L - 1], c->lh[L - 1]}, {0}};
        vres r[2] = {{lev_img(c, k, L - 1), NULL}, {&f->lev[L], NULL}};
        dispatch(c, &c->p_down, r, &pc, (c->lw[L] + 15) / 16, (c->lh[L] + 15) / 16);
    }
}

static void copy_overlap(mb_ctx *c, const vimg *from, int fx, int fy, vimg *to, int tx, int ty)
{
    int x0 = fx > tx ? fx : tx, y0 = fy > ty ? fy : ty;
    int x1 = fx + from->w < tx + to->w ? fx + from->w : tx + to->w;
    int y1 = fy + from->h < ty + to->h ? fy + from->h : ty + to->h;
    if (x1 <= x0 || y1 <= y0)
        return;
    VkImageCopy r = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {x0 - fx, y0 - fy, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     {x0 - tx, y0 - ty, 0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0), 1}};
    vkCmdCopyImage(S(c)->cb, from->img, VK_IMAGE_LAYOUT_GENERAL, to->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}

static int set_crop(mb_ctx *c, int x0, int y0, int w, int h)
{
    cmd_end(c, 0);
    vkQueueWaitIdle(c->q);
    gframe old[NFR];
    memcpy(old, c->fr, sizeof(old));
    memset(c->fr, 0, sizeof(c->fr));
    int ox0 = c->x0, oy0 = c->y0, sx = c->fmt.cshift_x, sy = c->fmt.cshift_y;
    int had_tmpl = c->tmpl != NULL;
    res_geom_free(c);
    c->x0 = x0;
    c->y0 = y0;
    c->fmt.w = w;
    c->fmt.h = h;
    set_geom(c);
    c->me_valid = 0;
    c->have_prev = 0;
    c->have_q = 0;
    c->hist_clear = 1;
    c->bars_ver++;
    int ok = 1;
    if (!c->tmpl) {
        c->tmpl = malloc(c->tsize);
        ok = c->tmpl != NULL;
    }
    ok = ok && res_geom_init(c) == 0 && cmd_begin(c) == 0;
    if (ok && c->nfr) {
        vbuf *up = &c->up[0];
        if (c->gpu_in) {
            for (int k = 0; k < NFR; k++) {
                copy_overlap(c, &c->bgy, 0, 0, &c->fr[k].y, x0, y0);
                copy_overlap(c, &c->bgc, 0, 0, &c->fr[k].c1, x0 >> sx, y0 >> sy);
            }
            had_tmpl = 0;
        } else if (had_tmpl) {
            for (int i = 0; i < c->nplanes; i++) {
                size_t xb, yr;
                plane_org(c, i, x0, y0, &xb, &yr);
                for (size_t y = 0; y < c->rows[i]; y++)
                    memcpy((uint8_t *)up->map + c->off[i] + y * c->rowb[i],
                           c->tmpl + c->toff[i] + (y + yr) * c->trowb[i] + xb, c->rowb[i]);
            }
        }
        for (int k = 0; k < NFR; k++) {
            gframe *f = &c->fr[k];
            if (had_tmpl) {
                copy_b2i(c, up, c->off[0], &f->y);
                copy_b2i(c, up, c->off[1], &f->c1);
                if (!c->fmt.nv)
                    copy_b2i(c, up, c->off[2], &f->c2);
            }
            barrier(c);
            copy_overlap(c, &old[k].y, ox0, oy0, &f->y, x0, y0);
            copy_overlap(c, &old[k].c1, ox0 >> sx, oy0 >> sy, &f->c1, x0 >> sx, y0 >> sy);
            if (!c->fmt.nv)
                copy_overlap(c, &old[k].c2, ox0 >> sx, oy0 >> sy, &f->c2, x0 >> sx, y0 >> sy);
        }
        barrier(c);
        for (int k = 0; k < NFR; k++)
            build_levels(c, k);
        barrier(c);
        if (c->nfr >= 2)
            run_me(c);
    }
    if (ok)
        ok = cmd_submit(c) >= 0;
    frames_free(c, old);
    c->up_slot[0] = -1;
    if (!ok)
        c->ok = 0;
    return ok ? 0 : -1;
}

static int bars_still_black(const mb_ctx *c, const barsrc *b)
{
    int W = c->full.w, H = c->full.h, x1 = c->x0 + c->fmt.w, y1 = c->y0 + c->fmt.h;
    for (int y = 0; y < H; y += 3) {
        int bar = y < c->y0 || y >= y1;
        if (bar && !brow(c, b, y))
            return 0;
        if (!bar && b->src && (!black_span(c, b->src, y, 0, c->x0) || !black_span(c, b->src, y, x1, W)))
            return 0;
    }
    for (int x = 0; b->fl && x < W; x += 3)
        if ((x < c->x0 || x >= x1) && !bcol(c, b, x, c->y0, y1))
            return 0;
    return 1;
}

static void track_bars(mb_ctx *c, const barsrc *b)
{
    int x0, y0, w, h;
    if (!c->p.bars)
        return;
    c->scan = (c->scan + 1) % 12;
    if (!b->fl && c->seen && c->scan && ((c->scan & 3) || bars_still_black(c, b)))
        return;
    int pushes = c->npush - c->det_push;
    c->det_push = c->npush;
    if (detect_bars(c, b, &x0, &y0, &w, &h) < 0)
        return;
    if (!c->seen) {
        c->seen = 1;
        if (x0 || y0 || w != c->fmt.w || h != c->fmt.h)
            set_crop(c, x0, y0, w, h);
        return;
    }
    int cx1 = c->x0 + c->fmt.w, cy1 = c->y0 + c->fmt.h;
    if (x0 < c->x0 || y0 < c->y0 || x0 + w > cx1 || y0 + h > cy1) {
        int ux0 = x0 < c->x0 ? x0 : c->x0, uy0 = y0 < c->y0 ? y0 : c->y0;
        int ux1 = x0 + w > cx1 ? x0 + w : cx1, uy1 = y0 + h > cy1 ? y0 + h : cy1;
        c->nstable = 0;
        set_crop(c, ux0, uy0, ux1 - ux0, uy1 - uy0);
        return;
    }
    if (x0 == c->x0 && y0 == c->y0 && w == c->fmt.w && h == c->fmt.h) {
        c->nstable = 0;
        return;
    }
    if (x0 != c->nx0 || y0 != c->ny0 || w != c->nw || h != c->nh) {
        c->nx0 = x0, c->ny0 = y0, c->nw = w, c->nh = h;
        c->nstable = 0;
    }
    c->nstable += pushes;
    if (c->nstable >= c->p.bar_wait) {
        c->nstable = 0;
        set_crop(c, x0, y0, w, h);
    }
}


static int cropped(const mb_ctx *c)
{
    return c->x0 || c->y0 || c->fmt.w != c->full.w || c->fmt.h != c->full.h;
}

static void save_bars(mb_ctx *c, const mb_image *src)
{
    for (int i = 0; i < c->nplanes; i++)
        copy_border(c, i, c->x0, c->y0, c->rowb[i], c->rows[i], c->tmpl + c->toff[i], (ptrdiff_t)c->trowb[i],
                    src->data[i], src->stride[i]);
}

static int push_begin(mb_ctx *c)
{
    c->eos = 0;
    c->npush++;
    if (c->nfr >= 2 && c->me_valid) {
        rot3(&c->qfb, &c->pfb[0], &c->fb[0]);
        rot3(&c->qstat, &c->pstat, &c->stat);
        for (int L = 0; L < c->nlev; L++) {
            swapbuf(&c->fa[L], &c->pfa[L]);
            if (L)
                swapbuf(&c->fb[L], &c->pfb[L]);
        }
        c->have_q = c->have_prev;
        c->have_prev = 1;
    } else {
        c->have_prev = 0;
        c->have_q = 0;
    }
    c->me_valid = 0;
    cmd_end(c, 0);
    if (cmd_begin(c) < 0)
        return -1;
    c->ib = (c->ib + 1) % NFR;
    return 0;
}

static void push_end(mb_ctx *c)
{
    barrier(c);
    build_levels(c, c->ib);
    if (c->nfr < NFR)
        c->nfr++;
    if (c->p.la && c->nfr >= 2)
        run_me(c);
}

void mb_push(mb_ctx *c, const mb_image *src)
{
    if (!c->ok)
        return;
    barsrc b = {src, NULL};
    c->gpu_in = 0;
    track_bars(c, &b);
    if (!c->ok)
        return;
    if (cropped(c))
        save_bars(c, src);
    if (push_begin(c) < 0)
        return;
    gframe *f = &c->fr[c->ib];
    int k = c->up_k = !c->up_k;
    if (c->up_slot[k] >= 0 && c->sl[c->up_slot[k]].serial == c->up_serial[k])
        slot_wait(c, c->up_slot[k]);
    c->up_slot[k] = c->cur;
    c->up_serial[k] = c->serial + 1;
    vbuf *up = &c->up[k];
    for (int i = 0; i < c->nplanes; i++) {
        size_t xb, yr;
        plane_org(c, i, c->x0, c->y0, &xb, &yr);
        uint8_t *d = (uint8_t *)up->map + c->off[i];
        for (size_t y = 0; y < c->rows[i]; y++)
            memcpy(d + y * c->rowb[i], src->data[i] + (ptrdiff_t)(y + yr) * src->stride[i] + xb, c->rowb[i]);
    }
    copy_b2i(c, up, c->off[0], &f->y);
    copy_b2i(c, up, c->off[1], &f->c1);
    if (!c->fmt.nv)
        copy_b2i(c, up, c->off[2], &f->c2);
    push_end(c);
}

static void xfill_bars_gpu(mb_ctx *c, xbuf *x)
{
    if (x->ver == c->bars_ver)
        return;
    const vimg *im[2] = {&c->bgy, &c->bgc};
    for (int i = 0; i < 2; i++) {
        int sx = i ? c->fmt.cshift_x : 0, sy = i ? c->fmt.cshift_y : 0;
        int W = im[i]->w, H = im[i]->h, ax = c->x0 >> sx, ay = c->y0 >> sy;
        int aw = i ? c->cw : c->fmt.w, ah = i ? c->ch : c->fmt.h;
        uint32_t px = (uint32_t)((i ? 2 : 1) * c->bps);
        int rect[4][4] = {{0, 0, W, ay}, {0, ay + ah, W, H - ay - ah}, {0, ay, ax, ah}, {ax + aw, ay, W - ax - aw, ah}};
        for (int k = 0; k < 4; k++) {
            if (rect[k][2] <= 0 || rect[k][3] <= 0)
                continue;
            VkBufferImageCopy r = {c->xoff[i] + (size_t)rect[k][1] * c->xpitch[i] + (size_t)rect[k][0] * px,
                                   (uint32_t)(c->xpitch[i] / px), 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                   {rect[k][0], rect[k][1], 0}, {(uint32_t)rect[k][2], (uint32_t)rect[k][3], 1}};
            vkCmdCopyImageToBuffer(S(c)->cb, im[i]->img, VK_IMAGE_LAYOUT_GENERAL, x->b.buf, 1, &r);
        }
    }
    x->ver = c->bars_ver;
}

static void poll_slots(mb_ctx *c)
{
    for (int i = 0; i < NSLOT; i++)
        if (c->sl[i].busy && vkGetFenceStatus(c->dev, c->sl[i].fence) == VK_SUCCESS)
            slot_wait(c, i);
}

static vimg *import_plane(mb_ctx *c, const mb_dmabuf_in *in, int p)
{
#ifdef _WIN32
    (void)c;
    (void)in;
    (void)p;
    return NULL;
#else
    int fd = in->fd[in->obj[p]];
    struct stat st;
    if (fstat(fd, &st) < 0)
        return NULL;
    c->clock++;
    if ((c->clock & 63) == 0) {
        int n = 0;
        for (int i = 0; i < c->nimp; i++) {
            if (c->clock - c->imp[i].used > 256)
                img_free(c, &c->imp[i].im);
            else
                c->imp[n++] = c->imp[i];
        }
        c->nimp = n;
    }
    int lru = 0;
    for (int i = 0; i < c->nimp; i++) {
        impimg *m = &c->imp[i];
        if (m->dev == (unsigned long long)st.st_dev && m->ino == (unsigned long long)st.st_ino && m->off == in->offset[p]) {
            m->used = c->clock;
            return &m->im;
        }
        if (m->used < c->imp[lru].used)
            lru = i;
    }
    impimg *m;
    if (c->nimp < MAXIMP) {
        m = &c->imp[c->nimp++];
    } else {
        vkQueueWaitIdle(c->q);
        m = &c->imp[lru];
        img_free(c, &m->im);
    }
    memset(m, 0, sizeof(*m));
    int w = p ? cdim(c->full.w, c->full.cshift_x) : c->full.w;
    int h = p ? cdim(c->full.h, c->full.cshift_y) : c->full.h;
    VkFormat fmt = plane_fmt(c, p ? 2 : 1);
    VkSubresourceLayout pl = {in->offset[p], 0, in->pitch[p], 0, 0};
    VkImageDrmFormatModifierExplicitCreateInfoEXT ex = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
                                                        NULL, in->modifier, 1, &pl};
    VkExternalMemoryImageCreateInfo ei = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &ex,
                                          VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ei};
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = fmt;
    ic.extent = (VkExtent3D){(uint32_t)w, (uint32_t)h, 1};
    ic.mipLevels = ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    m->im.w = w;
    m->im.h = h;
    if (vkCreateImage(c->dev, &ic, NULL, &m->im.img) != VK_SUCCESS)
        goto fail;
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (c->getfdprops(c->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) != VK_SUCCESS)
        goto fail;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(c->dev, m->im.img, &mr);
    uint32_t bits = mr.memoryTypeBits & fp.memoryTypeBits;
    if (!bits)
        goto fail;
    int dfd = dup(fd);
    if (dfd < 0)
        goto fail;
    VkMemoryDedicatedAllocateInfo da = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, m->im.img, VK_NULL_HANDLE};
    VkImportMemoryFdInfoKHR imi = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &da,
                                   VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dfd};
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imi, mr.size, (uint32_t)__builtin_ctz(bits)};
    if (vkAllocateMemory(c->dev, &ma, NULL, &m->im.mem) != VK_SUCCESS) {
        close(dfd);
        goto fail;
    }
    if (vkBindImageMemory(c->dev, m->im.img, m->im.mem, 0) != VK_SUCCESS)
        goto fail;
    VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = m->im.img;
    vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vc.format = fmt;
    vc.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(c->dev, &vc, NULL, &m->im.view) != VK_SUCCESS)
        goto fail;
    m->dev = (unsigned long long)st.st_dev;
    m->ino = (unsigned long long)st.st_ino;
    m->off = in->offset[p];
    m->used = c->clock;
    return &m->im;
fail:
    img_free(c, &m->im);
    memset(m, 0, sizeof(*m));
    return NULL;
#endif
}

static void ext_own(mb_ctx *c, const vimg *a, const vimg *b, int acquire)
{
    uint32_t ext = c->has_foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
    VkImageMemoryBarrier ib[2];
    for (int k = 0; k < 2; k++) {
        ib[k] = (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib[k].srcAccessMask = acquire ? 0 : VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        ib[k].dstAccessMask = acquire ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT : 0;
        ib[k].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[k].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[k].srcQueueFamilyIndex = acquire ? ext : c->qf;
        ib[k].dstQueueFamilyIndex = acquire ? c->qf : ext;
        ib[k].image = (k ? b : a)->img;
        ib[k].subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkPipelineStageFlags work = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    vkCmdPipelineBarrier(S(c)->cb, acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : work,
                         acquire ? work : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 2, ib);
}

static void add_release(mb_ctx *c, const mb_dmabuf_in *in)
{
    slot *sl = S(c);
    if (!in->release)
        return;
    if (sl->nrel == 4) {
        in->release(in->opaque);
        return;
    }
    sl->rel[sl->nrel] = in->release;
    sl->relo[sl->nrel++] = in->opaque;
}

static void reset_flags(mb_ctx *c)
{
    uint32_t *f = c->bflags.map;
    for (int i = 0; i < c->full.w + c->full.h; i++)
        f[i] = 1;
}

static int bg_init(mb_ctx *c)
{
    if (c->bgy.img)
        return 0;
    if (img_create(c, &c->bgy, c->full.w, c->full.h, plane_fmt(c, 1)) < 0 ||
        img_create(c, &c->bgc, cdim(c->full.w, c->full.cshift_x), cdim(c->full.h, c->full.cshift_y), plane_fmt(c, 2)) < 0 ||
        buf_create(c, &c->bflags, (VkDeviceSize)(c->full.w + c->full.h) * 4, 1) < 0)
        return -1;
    reset_flags(c);
    to_general(c, &c->bgy);
    to_general(c, &c->bgc);
    return 0;
}

static void gpu_bars(mb_ctx *c)
{
    if (!c->bf_pending)
        return;
    slot *sl = &c->sl[c->bf_slot];
    if ((c->recording && c->cur == c->bf_slot) || (sl->busy && sl->serial == c->bf_serial))
        return;
    c->bf_pending = 0;
    barsrc b = {NULL, c->bflags.map};
    track_bars(c, &b);
    reset_flags(c);
}

int mb_push_dmabuf(mb_ctx *c, const mb_dmabuf_in *in)
{
    if (!c->ok || !c->has_mod || !c->fmt.nv)
        return -1;
    poll_slots(c);
    vimg *py = import_plane(c, in, 0), *pu = py ? import_plane(c, in, 1) : NULL;
    if (!pu)
        return -1;
    c->gpu_in = 1;
    gpu_bars(c);
    if (!c->ok || push_begin(c) < 0)
        return -1;
    if (bg_init(c) < 0) {
        c->ok = 0;
        return -1;
    }
    gframe *f = &c->fr[c->ib];
    int sx = c->fmt.cshift_x, sy = c->fmt.cshift_y;
    ext_own(c, py, pu, 1);
    copy_overlap(c, py, -c->x0, -c->y0, &f->y, 0, 0);
    copy_overlap(c, pu, -(c->x0 >> sx), -(c->y0 >> sy), &f->c1, 0, 0);
    if (cropped(c) && (c->bg_ver != c->bars_ver || (c->bf_tick & 15) == 0)) {
        copy_overlap(c, py, 0, 0, &c->bgy, 0, 0);
        copy_overlap(c, pu, 0, 0, &c->bgc, 0, 0);
        c->bg_ver = c->bars_ver;
    }
    if (c->p.bars && !c->bf_pending && (c->bf_tick++ & 3) == 0) {
        pcs pc = {{c->full.w, c->full.h, sx, sy}, {24.0f / 255.0f, 10.0f / 255.0f}};
        vres r[3] = {{py, NULL}, {pu, NULL}, {NULL, &c->bflags}};
        dispatch(c, &c->p_bars, r, &pc, (uint32_t)(pu->w + 15) / 16, (uint32_t)(pu->h + 15) / 16);
        c->bf_pending = 1;
        c->bf_slot = c->cur;
        c->bf_serial = c->serial + 1;
    }
    ext_own(c, py, pu, 0);
    host_barrier(c);
    add_release(c, in);
    push_end(c);
    return 0;
}

void mb_flush(mb_ctx *c)
{
    c->eos = 1;
}

static void estimate(mb_ctx *c, int dir)
{
    int kp = fidx(c, 1);
    int kx = dir ? c->ib : kp, ky = dir ? kp : c->ib;
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
                      {c->p.pen_parent, c->p.pen_temporal, c->p.pen_update, c->p.pen_zero, c->p.pen_cross, c->p.pen_len, c->p.skip}};
            set_dc(c, &pc);
            vres r[8] = {{X, NULL},
                         {Y, NULL},
                         {NULL, &F[L]},
                         {NULL, &c->tmp[L]},
                         {NULL, L < top ? &F[L + 1] : &c->dummy},
                         {NULL, c->have_prev ? &T[L] : &c->dummy},
                         {NULL, dir ? &c->fa[L] : &c->dummy},
                         {NULL, &c->hist}};
            dispatch(c, &c->p_iter, r, &pc, (uint32_t)(nb + 3) / 4, 1);
            static const char *const iter_names[MAXLEV] = {"iter_L0", "iter_L1", "iter_L2", "iter_L3", "iter_L4", "iter_L5"};
            if (c->prof && S(c)->nq > 0)
                S(c)->qname[S(c)->nq - 1] = iter_names[L];
            swapbuf(&F[L], &c->tmp[L]);
            have = 1;
        }
        if (L <= 1 && (c->p.refine || L == 0)) {
            pcs pc = {{c->bw[L], c->bh[L], c->lw[L], c->lh[L], L == 0 && dir == 0, c->p.refine, L == 0, dir}, {0}};
            set_dc(c, &pc);
            vres r[4] = {{X, NULL}, {Y, NULL}, {NULL, &F[L]}, {NULL, &c->stat}};
            dispatch(c, &c->p_refine, r, &pc, (uint32_t)(nb + 3) / 4, 1);
        }
    }
}

static void cross_fix(mb_ctx *c, int dir)
{
    int bw = c->bw[0], bh = c->bh[0], nb = bw * bh;
    int kp = fidx(c, 1);
    vkCmdFillBuffer(S(c)->cb, c->xkeys.buf, 0, VK_WHOLE_SIZE, 0xffffffffu);
    barrier(c);
    {
        pcs pc = {{bw, bh, 0, 0, 0}, {1.0f}};
        vres r[2] = {{NULL, dir ? &c->fa[0] : &c->fb[0]}, {NULL, &c->xkeys}};
        dispatch(c, &c->p_proj, r, &pc, (uint32_t)(nb + 63) / 64, 1);
    }
    vbuf *F = dir ? c->fb : c->fa;
    pcs pc = {{bw, bh, c->lw[0], c->lh[0]}, {0, 0, 0, 0, c->p.pen_xfix, c->p.pen_len, 0, 0, c->p.xrel, c->p.xabs, c->p.xmin}};
    vres r[6] = {{lev_img(c, dir ? c->ib : kp, 0), NULL}, {lev_img(c, dir ? kp : c->ib, 0), NULL}, {NULL, &F[0]},
                 {NULL, &c->tmp[0]}, {NULL, dir ? &c->fa[0] : &c->fb[0]}, {NULL, &c->xkeys}};
    dispatch(c, &c->p_cross, r, &pc, (uint32_t)(nb + 63) / 64, 1);
    swapbuf(&F[0], &c->tmp[0]);
}

static void local_fix(mb_ctx *c, int dir)
{
    int bw = c->bw[0], bh = c->bh[0], nb = bw * bh;
    int kp = fidx(c, 1);
    vbuf *F = dir ? c->fb : c->fa;
    int chk = c->p.xchk && (dir || c->have_prev);
    pcs pc = {{bw, bh, c->lw[0], c->lh[0], chk, dir},
              {0, 0, 0, 0, 0, c->p.pen_len, c->p.tsup_rel, c->p.tsup_abs, c->p.xrel, c->p.xabs, c->p.xnoise, c->p.xbad, c->p.xr, c->p.xs}};
    vres r[6] = {{lev_img(c, dir ? c->ib : kp, 0), NULL}, {lev_img(c, dir ? kp : c->ib, 0), NULL}, {NULL, &F[0]},
                 {NULL, &c->tmp[0]}, {NULL, dir ? &c->fa[0] : (c->have_prev ? &c->pfb[0] : &c->dummy)}, {NULL, &c->stat}};
    dispatch(c, &c->p_local, r, &pc, (uint32_t)(nb + 63) / 64, 1);
    swapbuf(&F[0], &c->tmp[0]);
}

static void run_me(mb_ctx *c)
{
    cmd_begin(c);
    vkCmdFillBuffer(S(c)->cb, c->stat.buf, 0, VK_WHOLE_SIZE, 0);
    if (c->hist_clear)
        vkCmdFillBuffer(S(c)->cb, c->hist.buf, 0, VK_WHOLE_SIZE, 0);
    c->hist_clear = 0;
    barrier(c);
    estimate(c, 0);
    estimate(c, 1);
    for (int dir = 0; dir < c->p.xfix; dir++)
        cross_fix(c, dir);
    for (int dir = 0; dir < 2 && c->p.xloc; dir++)
        local_fix(c, dir);
    pcs pc = {{c->bw[0] * c->bh[0]}, {(float)(STAT_SCALE / ((double)c->bw[0] * c->bh[0] * 256)), (float)c->p.scut, (float)c->p.scut_rel,
                    (float)c->p.zcut_rel, (float)c->p.zcut_min, c->p.cut_sd, c->p.cut_kmin}};
    vres r[2] = {{NULL, &c->stat}, {NULL, &c->hist}};
    dispatch(c, &c->p_cutdec, r, &pc, 1, 1);
    c->me_valid = 1;
}

static void read_out(mb_ctx *c, const slot *sl, mb_image *dst)
{
    size_t off[3], rowb[3], rows[3];
    layout(c, sl->w, sl->h, off, rowb, rows);
    int crop = sl->x0 || sl->y0 || sl->w != c->full.w || sl->h != c->full.h;
    for (int i = 0; i < c->nplanes; i++) {
        const uint8_t *s = (const uint8_t *)sl->down.map + off[i];
        size_t xb = 0, yr = 0;
        if (crop) {
            plane_org(c, i, sl->x0, sl->y0, &xb, &yr);
            copy_border(c, i, sl->x0, sl->y0, rowb[i], rows[i], dst->data[i], dst->stride[i], c->tmpl + c->toff[i],
                        (ptrdiff_t)c->trowb[i]);
        }
        for (size_t y = 0; y < rows[i]; y++)
            memcpy(dst->data[i] + (ptrdiff_t)(y + yr) * dst->stride[i] + xb, s + y * rowb[i], rowb[i]);
    }
}

static void copy_image(const mb_ctx *c, const mb_image *dst, const mb_image *src)
{
    for (int k = 0; k < c->nplanes; k++)
        for (size_t y = 0; y < c->trows[k]; y++)
            memcpy(dst->data[k] + (ptrdiff_t)y * dst->stride[k], src->data[k] + (ptrdiff_t)y * src->stride[k], c->trowb[k]);
}

static int xbuf_create(mb_ctx *c, xbuf *x)
{
#ifdef _WIN32
    (void)c;
    (void)x;
    return -1;
#else
    memset(x, 0, sizeof(*x));
    x->fd = -1;
    x->ver = -1;
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL,
                                           VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb};
    bc.size = c->xsize;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK(vkCreateBuffer(c->dev, &bc, NULL, &x->b.buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(c->dev, x->b.buf, &mr);
    VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int mt = memtype(c, mr.memoryTypeBits, hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mt < 0)
        mt = memtype(c, mr.memoryTypeBits, hv, 0);
    if (mt < 0)
        return -1;
    VkMemoryDedicatedAllocateInfo da = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, x->b.buf};
    VkExportMemoryAllocateInfo ea = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &da,
                                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ea, mr.size, (uint32_t)mt};
    VK(vkAllocateMemory(c->dev, &ma, NULL, &x->b.mem));
    VK(vkBindBufferMemory(c->dev, x->b.buf, x->b.mem, 0));
    VK(vkMapMemory(c->dev, x->b.mem, 0, VK_WHOLE_SIZE, 0, &x->b.map));
    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, x->b.mem,
                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VK(c->getfd(c->dev, &gi, &x->fd));
    return 0;
#endif
}

static void xbuf_free(mb_ctx *c, xbuf *x)
{
    buf_free(c, &x->b);
#ifndef _WIN32
    if (x->fd >= 0)
        close(x->fd);
#endif
    x->fd = -1;
}

int mb_export_init(mb_ctx *c)
{
    if (!c->ok || !c->has_dmabuf || !c->getfd || (c->fmt.depth > 8 && c->lnorm))
        return -1;
    if (c->xp)
        return 0;
    size_t o = 0;
    for (int i = 0; i < c->nplanes; i++) {
        c->xpitch[i] = (c->trowb[i] + 255) & ~(size_t)255;
        c->xoff[i] = o;
        o += (c->xpitch[i] * c->trows[i] + 4095) & ~(size_t)4095;
    }
    c->xsize = o;
    c->xp = calloc(1, sizeof(*c->xp));
    if (!c->xp)
        return -1;
    atomic_init(&c->xp->refs, 1);
    for (int i = 0; i < 4; i++)
        if (xbuf_create(c, &c->xb[c->nx++]) < 0) {
            xbuf_free(c, &c->xb[--c->nx]);
            break;
        }
    return c->nx ? 0 : -1;
}

static int xacquire(mb_ctx *c)
{
    for (int i = 0; i < c->nx; i++) {
        int z = 0;
        if (atomic_compare_exchange_strong(&c->xp->busy[i], &z, 1))
            return i;
    }
    if (c->nx >= MAXX)
        return -1;
    if (xbuf_create(c, &c->xb[c->nx]) < 0) {
        xbuf_free(c, &c->xb[c->nx]);
        return -1;
    }
    atomic_store(&c->xp->busy[c->nx], 1);
    return c->nx++;
}

static mb_image xbuf_image(const mb_ctx *c, const xbuf *x)
{
    mb_image im = {{0}};
    for (int k = 0; k < c->nplanes; k++) {
        im.data[k] = (uint8_t *)x->b.map + c->xoff[k];
        im.stride[k] = (ptrdiff_t)c->xpitch[k];
    }
    return im;
}

static void xfill_bars(mb_ctx *c, xbuf *x)
{
    if (x->ver == c->bars_ver || !c->tmpl)
        return;
    mb_image d = xbuf_image(c, x);
    for (int i = 0; i < c->nplanes; i++)
        copy_border(c, i, c->x0, c->y0, c->rowb[i], c->rows[i], d.data[i], d.stride[i], c->tmpl + c->toff[i],
                    (ptrdiff_t)c->trowb[i]);
    x->ver = c->bars_ver;
}

static void xcopy_out(mb_ctx *c, xbuf *x)
{
    const vimg *im[3] = {&c->oy, &c->oc1, &c->oc2};
    int ncomp = c->fmt.nv ? 2 : 1;
    for (int i = 0; i < c->nplanes; i++) {
        size_t xb, yr;
        plane_org(c, i, c->x0, c->y0, &xb, &yr);
        uint32_t px = (uint32_t)((i ? ncomp : 1) * c->bps);
        VkBufferImageCopy r = {c->xoff[i] + yr * c->xpitch[i] + xb, (uint32_t)(c->xpitch[i] / px), 0,
                               {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                               {(uint32_t)im[i]->w, (uint32_t)im[i]->h, 1}};
        vkCmdCopyImageToBuffer(S(c)->cb, im[i]->img, VK_IMAGE_LAYOUT_GENERAL, x->b.buf, 1, &r);
    }
}

static void xfill_desc(mb_ctx *c, int i, mb_dmabuf *out)
{
    memset(out, 0, sizeof(*out));
#ifndef _WIN32
    out->fd = dup(c->xb[i].fd);
#endif
    out->nplanes = c->nplanes;
    for (int k = 0; k < c->nplanes; k++) {
        out->offset[k] = (uint32_t)c->xoff[k];
        out->pitch[k] = (uint32_t)c->xpitch[k];
    }
    out->size = c->xsize;
    out->pool = c->xp;
    out->idx = i;
    atomic_fetch_add(&c->xp->refs, 1);
}

void mb_dmabuf_release(void *pool, int idx)
{
    xpool *xp = pool;
    atomic_store(&xp->busy[idx], 0);
    if (atomic_fetch_sub(&xp->refs, 1) == 1)
        free(xp);
}

int mb_export_image(mb_ctx *c, const mb_image *src, mb_dmabuf *out)
{
    if (!c->xp)
        return -1;
    int i = xacquire(c);
    if (i < 0)
        return -1;
    xbuf *x = &c->xb[i];
    mb_image d = xbuf_image(c, x);
    copy_image(c, &d, src);
    x->ver = c->bars_ver;
    xfill_desc(c, i, out);
    return 0;
}

int mb_submit(mb_ctx *c, double t)
{
    int la = c->p.la && !c->eos && c->have_prev && c->me_valid;
    if (!c->ok || c->nfr < 2 || c->fifo_n >= MB_MAX_PENDING || (c->p.la && !c->eos && !la))
        return -1;
    int xi = c->xp ? xacquire(c) : -1;
    if ((c->xp && xi < 0) || cmd_begin(c) < 0) {
        if (xi >= 0)
            atomic_store(&c->xp->busy[xi], 0);
        return -1;
    }
    if (!c->me_valid)
        run_me(c);
    int ka, kb, kc, kz, past;
    vbuf *FA0, *FB0, *NFA, *PFB, *SC, *SP;
    if (la) {
        ka = fidx(c, 2), kb = fidx(c, 1), kc = fidx(c, 0), kz = fidx(c, 3);
        FA0 = &c->pfa[0], FB0 = &c->pfb[0], NFA = &c->fa[0], PFB = &c->qfb;
        SC = &c->pstat, SP = &c->qstat;
        past = c->have_q;
    } else {
        ka = fidx(c, 1), kb = fidx(c, 0), kc = kb, kz = fidx(c, 2);
        FA0 = &c->fa[0], FB0 = &c->fb[0], NFA = &c->dummy, PFB = &c->pfb[0];
        SC = &c->stat, SP = &c->pstat;
        past = c->have_prev;
    }
    if (!past)
        kz = ka, PFB = &c->dummy, SP = SC;
    c->last_fa = *FA0;
    c->last_fb = *FB0;
    gframe *A = &c->fr[ka], *B = &c->fr[kb];
    int bw = c->bw[0], bh = c->bh[0], nb = bw * bh;
    vkCmdFillBuffer(S(c)->cb, c->slots.buf, 0, VK_WHOLE_SIZE, 0xffffffffu);
    vkCmdFillBuffer(S(c)->cb, c->eslots.buf, 0, VK_WHOLE_SIZE, 0xffffffffu);
    barrier(c);
    for (int dir = 0; dir < 2 && c->p.tex; dir++) {
        if (dir == 0 ? !past : !la)
            continue;
        pcs pc = {{bw, bh, 0, 0, dir}, {(float)(dir ? -(1 - t) : -t)}};
        vres r[2] = {{NULL, dir ? NFA : PFB}, {NULL, &c->eslots}};
        dispatch(c, &c->p_proj, r, &pc, (uint32_t)(nb + 63) / 64, 1);
    }
    for (int dir = 0; dir < 2; dir++) {
        pcs pc = {{bw, bh, 0, 0, dir}, {(float)(dir ? 1 - t : t)}};
        vres r[2] = {{NULL, dir ? FB0 : FA0}, {NULL, &c->slots}};
        dispatch(c, &c->p_proj, r, &pc, (uint32_t)(nb + 63) / 64, 1);
    }
    vbuf *tfin = &c->dummy, *tfout = &c->tf0;
    for (int pass = 0; pass < c->p.sel_passes; pass++) {
        pcs pc = {{bw, bh, c->fmt.w, c->fmt.h, pass, la, past, c->p.tsup, (int)lrintf(c->p.tsup_rel * 100), (int)lrintf(c->p.tsup_abs * 100), (int)c->p.tsup_pen, (int)lrintf(c->p.tsup_min * 100), c->p.tref, c->p.tz_pen, c->p.twaive},
                  {(float)t, c->p.pen_inc, c->p.cthr, c->p.fg_sel, 0, 0, c->p.pen_one, c->p.one_rel, c->p.one_min, c->p.one_tex,
                   c->p.unc_min, (float)c->p.uncov, c->p.one_sw, c->p.unc_cont * 0.01f}};
        set_dc(c, &pc);
        vres r[14] = {{lev_img(c, ka, 0), NULL}, {lev_img(c, kb, 0), NULL}, {lev_img(c, kc, 0), NULL},
                      {lev_img(c, kz, 0), NULL}, {NULL, FA0}, {NULL, FB0}, {NULL, &c->slots}, {NULL, tfin},
                      {NULL, tfout}, {NULL, NFA}, {NULL, PFB}, {NULL, &c->stat}, {NULL, SP}, {NULL, &c->eslots}};
        dispatch(c, &c->p_sel, r, &pc, (uint32_t)(nb + 3) / 4, 1);
        tfin = tfout;
        tfout = tfout == &c->tf0 ? &c->tf1 : &c->tf0;
    }
    if (c->p.vm_thr > 0) {
        pcs pc = {{bw, bh}, {c->p.vm_thr}};
        vres r[2] = {{NULL, tfin}, {NULL, tfout}};
        dispatch(c, &c->p_vmed, r, &pc, (uint32_t)(nb + 63) / 64, 1);
        tfin = tfout;
    }
    c->last_tf = tfin;
    {
        pcs pc = {{bw, bh}, {0, 0, c->p.fb_spread, c->p.fb_avg, (float)t, c->p.fb_ilo, c->p.fb_ihi}};
        vres r[4] = {{NULL, tfin}, {NULL, &c->fbw}, {NULL, FA0}, {NULL, FB0}};
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
        pcs pa = {{bw, bh, c->fmt.w, c->fmt.h, c->cw, c->ch, c->fmt.cshift_x, c->fmt.cshift_y, c->fmt.nv},
                  {(float)t, 255.0f * luma_scale(c), c->p.ag_lo, c->p.ag_hi, c->p.ag_flat, 0,
                   c->bps == 1 ? 128.0f / 255.0f : c->lnorm ? (float)(1 << (c->fmt.depth - 1)) / 65535.0f : 0.5f, c->p.ag_sat}};
        pcs pg = {{bw, bh}};
        vres rg[2] = {{NULL, tfin}, {NULL, &c->gmv}};
        dispatch(c, &c->p_gmed, rg, &pg, 1, 1);
        vres ra[8] = {{&A->y, NULL}, {&B->y, NULL}, {&A->c1, NULL}, {&B->c1, NULL},
                      {ac2, NULL},   {bc2, NULL},   {NULL, &c->agw}, {NULL, &c->gmv}};
        dispatch(c, &c->p_agree, ra, &pa, (uint32_t)(nb + 63) / 64, 1);
        pcs ps = {{bw, bh, c->p.ag_r}, {c->p.ag_src, (float)t}};
        vres rs[4] = {{NULL, &c->agw}, {NULL, &c->agw2}, {NULL, tfin}, {NULL, &c->gmv}};
        dispatch(c, &c->p_agspread, rs, &ps, (uint32_t)(nb + 63) / 64, 1);
    }
    {
        pcs pc = {{bw, bh, c->fmt.w, c->fmt.h, c->p.sub_ref},
                  {(float)t, c->p.sub_thr, c->p.pen_sub, luma_scale(c), c->p.pen_sref}};
        vres r[5] = {{lev_img(c, ka, 0), NULL}, {lev_img(c, kb, 0), NULL}, {NULL, tfin}, {NULL, &c->tf4}, {NULL, &c->ill1}};
        dispatch(c, &c->p_sub, r, &pc, (uint32_t)(nb * 4 + 63) / 64, 1);
    }
    {
        float maxv, step;
        out_range(c, &maxv, &step);
        pcs pc = {{c->fmt.w, c->fmt.h, bw, bh, c->cw, c->ch, c->fmt.cshift_x, c->fmt.cshift_y},
                  {(float)t, c->p.sigma, c->p.pen_occ, c->p.pen_bad, c->p.pen_fb, c->p.wprior, c->p.cthr, c->p.pen_fbm,
                   luma_scale(c), maxv, step, 0, c->p.fg_pix, c->p.ws_lo, c->p.ws_hi, c->p.ag_sw}};
        vres r[19] = {{&A->y, NULL},  {&B->y, NULL},  {&A->c1, NULL},   {&B->c1, NULL},
                      {ac2, NULL},    {bc2, NULL},    {NULL, &c->tf4},  {NULL, FA0},
                      {NULL, FB0}, {&c->oy, NULL}, {&c->oc1, NULL}, {c->fmt.nv ? &c->oc1 : &c->oc2, NULL},
                      {lev_img(c, ka, 0), NULL}, {lev_img(c, kb, 0), NULL}, {NULL, SC}, {NULL, &c->fbw}, {NULL, &c->ill1},
                      {NULL, &c->agw2}, {NULL, &c->gmv}};
        dispatch(c, &c->p_interp, r, &pc, (uint32_t)((c->fmt.w + 1) / 2 + 7) / 8, (uint32_t)((c->fmt.h + 1) / 2 + 7) / 8);
    }
    S(c)->xidx = xi;
    if (xi >= 0) {
        if (cropped(c) && c->gpu_in)
            xfill_bars_gpu(c, &c->xb[S(c)->xidx]);
        else if (cropped(c))
            xfill_bars(c, &c->xb[S(c)->xidx]);
        xcopy_out(c, &c->xb[S(c)->xidx]);
    } else {
        copy_i2b(c, &c->oy, &S(c)->down, c->off[0]);
        copy_i2b(c, &c->oc1, &S(c)->down, c->off[1]);
        if (!c->fmt.nv)
            copy_i2b(c, &c->oc2, &S(c)->down, c->off[2]);
    }
    VkBufferCopy bc = {0, 0, 64};
    vkCmdCopyBuffer(S(c)->cb, SC->buf, S(c)->statcopy.buf, 1, &bc);
    S(c)->x0 = c->x0;
    S(c)->y0 = c->y0;
    S(c)->w = c->fmt.w;
    S(c)->h = c->fmt.h;
    host_barrier(c);
    return cmd_end(c, 1);
}

int mb_debug_field(mb_ctx *c, int which, float *out, int *w, int *h)
{
    if (which < 0 || which > 2)
        return -1;
    vbuf *f = which == 0 ? &c->last_fa : which == 1 ? &c->last_fb : c->last_tf;
    if (!f || !f->buf)
        return -1;
    int n = c->bw[0] * c->bh[0];
    vbuf tmp;
    if (buf_create(c, &tmp, (VkDeviceSize)n * 16, 1) < 0 || cmd_begin(c) < 0) {
        buf_free(c, &tmp);
        return -1;
    }
    VkBufferCopy bc = {0, 0, (VkDeviceSize)n * 16};
    vkCmdCopyBuffer(S(c)->cb, f->buf, tmp.buf, 1, &bc);
    host_barrier(c);
    if (cmd_submit(c) < 0) {
        buf_free(c, &tmp);
        return -1;
    }
    memcpy(out, tmp.map, (size_t)n * 16);
    *w = c->bw[0];
    *h = c->bh[0];
    buf_free(c, &tmp);
    return 0;
}

int mb_pending(mb_ctx *c)
{
    return c->fifo_n;
}

int mb_ready(mb_ctx *c)
{
    return c->fifo_n && vkGetFenceStatus(c->dev, c->sl[c->fifo[0]].fence) == VK_SUCCESS;
}

static slot *pop_slot(mb_ctx *c, int *cut)
{
    if (!c->fifo_n)
        return NULL;
    int i = c->fifo[0];
    memmove(&c->fifo[0], &c->fifo[1], (size_t)(c->fifo_n - 1) * sizeof(int));
    c->fifo_n--;
    slot *sl = &c->sl[i];
    slot_wait(c, i);
    double e = ((uint32_t *)sl->statcopy.map)[0] * STAT_SCALE / ((double)c->bw[0] * c->bh[0] * 256);
    *cut = ((uint32_t *)sl->statcopy.map)[2] != 0;
    c->st.mean_err = e;
    c->st.scene_cut = *cut;
    c->st.interp_ms = sl->gpu_ms;
    return sl;
}

int mb_retrieve(mb_ctx *c, mb_image *dst)
{
    int cut;
    slot *sl = pop_slot(c, &cut);
    if (!sl)
        return -1;
    if (sl->xidx >= 0) {
        mb_image s = xbuf_image(c, &c->xb[sl->xidx]);
        copy_image(c, dst, &s);
        atomic_store(&c->xp->busy[sl->xidx], 0);
        sl->xidx = -1;
        return cut;
    }
    read_out(c, sl, dst);
    return cut;
}

int mb_retrieve_dmabuf(mb_ctx *c, mb_dmabuf *out)
{
    int cut;
    slot *sl = pop_slot(c, &cut);
    if (!sl)
        return -1;
    if (sl->xidx < 0)
        return -1;
    xfill_desc(c, sl->xidx, out);
    sl->xidx = -1;
    return cut;
}

int mb_interpolate(mb_ctx *c, double t, mb_image *dst)
{
    if (mb_submit(c, t) < 0)
        return -1;
    return mb_retrieve(c, dst);
}

void mb_reset(mb_ctx *c)
{
    cmd_end(c, 0);
    for (int i = 0; i < NSLOT; i++) {
        slot_wait(c, i);
        if (c->sl[i].xidx >= 0 && c->xp)
            atomic_store(&c->xp->busy[c->sl[i].xidx], 0);
        c->sl[i].xidx = -1;
    }
    c->fifo_n = 0;
    c->nfr = 0;
    c->me_valid = 0;
    c->have_prev = 0;
    c->have_q = 0;
    c->eos = 0;
    c->hist_clear = 1;
    c->st.scene_cut = 0;
}

static void default_params(params *p)
{
    p->pen_parent = 4;
    p->pen_temporal = 8;
    p->pen_update = 16;
    p->pen_zero = 8;
    p->pen_cross = 4;
    p->pen_len = 4;
    p->lam_full = 2;
    p->dc_lo = 0.5f;
    p->dc_hi = 1;
    p->coarse_r = 16;
    p->skip = 0.06f;
    p->iters = 3;
    p->iters0 = 1;
    p->refine = 0;
    p->sel_passes = 1;
    p->pen_inc = 256;
    p->sigma = 250;
    p->pen_occ = 384;
    p->pen_bad = 256;
    p->wprior = 0.25f;
    p->cthr = 8;
    p->pen_fb = 1e6f;
    p->fb_spread = 0.7f;
    p->fb_avg = 1;
    p->fb_ilo = 2;
    p->fb_ihi = 6;
    p->fg_sel = 1024;
    p->fg_pix = 60;
    p->pen_one = 512;
    p->one_rel = 0.5f;
    p->one_min = 2048;
    p->one_tex = 0.75f;
    p->one_sw = 4;
    p->pen_fbm = 768;
    p->unc_min = 4000;
    p->uncov = 1;
    p->unc_cont = 50;
    p->xfix = 2;
    p->tsup = 1;
    p->xloc = 1;
    p->tex = 1;
    p->tref = 8;
    p->twaive = 1;
    p->xchk = 1;
    p->xbad = 1500;
    p->xnoise = 1.5f;
    p->xr = 4;
    p->xs = 2;
    p->tsup_rel = 0.25f;
    p->tsup_abs = 4;
    p->tsup_pen = 512;
    p->tsup_min = 8;
    p->pen_xfix = 4;
    p->xrel = 0.7f;
    p->xabs = 400;
    p->xmin = 4;
    p->vm_thr = 8;
    p->ws_lo = 8;
    p->ws_hi = 24;
    p->sub_thr = 2;
    p->pen_sub = 0;
    p->sub_ref = 2;
    p->pen_sref = 16;
    p->la = 1;
    p->bar_wait = 48;
    p->bars = 1;
    p->tz_pen = 2048;
    p->ag_lo = 24;
    p->ag_hi = 48;
    p->ag_flat = 32;
    p->ag_r = 8;
    p->ag_sat = 48;
    p->ag_sw = 16;
    p->ag_src = 8;
    p->scut = 40;
    p->scut_rel = 4;
    p->zcut_rel = 2;
    p->zcut_min = 1;
    p->cut_sd = 45;
    p->cut_kmin = 0.1f;
}

int mb_set_param(mb_ctx *c, const char *name, double v)
{
    params *p = &c->p;
#define P_(N) if (!strcmp(name, #N)) { p->N = v; return 0; }
    if (!strcmp(name, "sel_passes") || !strcmp(name, "iters0")) {
        if (v < 1)
            return -1;
    }
    P_(pen_parent) P_(pen_temporal) P_(pen_update) P_(pen_zero) P_(pen_cross) P_(pen_len) P_(lam_full) P_(dc_lo) P_(dc_hi) P_(skip)
    P_(coarse_r) P_(iters) P_(iters0) P_(refine) P_(sel_passes) P_(pen_inc) P_(sigma) P_(pen_occ) P_(pen_bad)
    P_(wprior) P_(cthr) P_(pen_fb) P_(fb_spread) P_(fb_avg) P_(fg_sel) P_(fg_pix) P_(pen_one) P_(one_rel) P_(one_min) P_(one_tex) P_(one_sw) P_(pen_fbm) P_(unc_min) P_(uncov) P_(unc_cont) P_(xfix) P_(tsup) P_(xloc) P_(tex) P_(tref) P_(twaive) P_(xchk) P_(xbad) P_(xnoise) P_(xr) P_(xs) P_(tsup_rel) P_(tsup_abs) P_(tsup_pen) P_(tsup_min) P_(pen_xfix) P_(xrel) P_(xabs) P_(xmin) P_(vm_thr) P_(ws_lo) P_(ws_hi) P_(sub_thr) P_(pen_sub) P_(sub_ref) P_(pen_sref) P_(la) P_(bar_wait) P_(bars) P_(tz_pen) P_(ag_lo) P_(ag_hi) P_(ag_flat) P_(ag_r) P_(ag_sat) P_(ag_sw) P_(ag_src) P_(fb_ilo) P_(fb_ihi) P_(scut) P_(scut_rel) P_(zcut_rel) P_(zcut_min) P_(cut_sd) P_(cut_kmin)
#undef P_
    return -1;
}

void mb_get_stats(mb_ctx *c, mb_stats *st)
{
    *st = c->st;
}

void mb_destroy(mb_ctx *c)
{
    if (!c)
        return;
    if (c->prof)
        for (int k = 0; k < c->prof_n; k++)
            fprintf(stderr, "motionblade profile: %-10s %8.1f ms total\n", c->prof_name[k], c->prof_ms[k]);
    if (c->dev) {
        vkDeviceWaitIdle(c->dev);
        res_geom_free(c);
        for (int i = 0; i < NSLOT; i++)
            slot_release(&c->sl[i]);
        for (int i = 0; i < c->nimp; i++)
            img_free(c, &c->imp[i].im);
        img_free(c, &c->bgy);
        img_free(c, &c->bgc);
        buf_free(c, &c->bflags);
        pipe_free(c, &c->p_bars);
        pipe_free(c, &c->p_agree);
        pipe_free(c, &c->p_gmed);
        pipe_free(c, &c->p_agspread);
        for (int i = 0; i < c->nx; i++)
            xbuf_free(c, &c->xb[i]);
        if (c->xp && atomic_fetch_sub(&c->xp->refs, 1) == 1)
            free(c->xp);
        for (int i = 0; i < NSLOT; i++) {
            slot *sl = &c->sl[i];
            buf_free(c, &sl->down);
            buf_free(c, &sl->statcopy);
            if (sl->qp)
                vkDestroyQueryPool(c->dev, sl->qp, NULL);
            if (sl->fence)
                vkDestroyFence(c->dev, sl->fence, NULL);
        }
        buf_free(c, &c->up[0]);
        buf_free(c, &c->up[1]);
        pipe_free(c, &c->p_down);
        pipe_free(c, &c->p_norm);
        pipe_free(c, &c->p_full);
        pipe_free(c, &c->p_iter);
        pipe_free(c, &c->p_refine);
        pipe_free(c, &c->p_proj);
        pipe_free(c, &c->p_cross);
        pipe_free(c, &c->p_local);
        pipe_free(c, &c->p_sel);
        pipe_free(c, &c->p_vmed);
        pipe_free(c, &c->p_sub);
        pipe_free(c, &c->p_conf);
        pipe_free(c, &c->p_cutdec);
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
    free(c->tmpl);
    free(c);
}

mb_ctx *mb_create(const mb_format *fmt)
{
    if (fmt->w < 64 || fmt->h < 64 || fmt->depth < 8 || fmt->depth > 16 || fmt->cshift_x < 0 || fmt->cshift_x > 1 ||
        fmt->cshift_y < 0 || fmt->cshift_y > 1 || fmt->lsb_pad < 0 || fmt->lsb_pad > 8 ||
        (fmt->lsb_pad && fmt->depth != 16))
        return NULL;
    mb_ctx *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fmt = c->full = *fmt;
    c->bps = fmt->depth > 8 ? 2 : 1;
    c->nplanes = fmt->nv ? 2 : 3;
    c->lnorm = c->bps == 2 && fmt->depth < 16;
    default_params(&c->p);
    c->hist_clear = 1;
    set_geom(c);
    if (vk_init(c) < 0) {
        fprintf(stderr, "motionblade: Vulkan init failed\n");
        mb_destroy(c);
        return NULL;
    }
    if ((uint32_t)(c->bw[0] * c->bh[0] + 3) / 4 > c->max_groups) {
        fprintf(stderr, "motionblade: %dx%d is too large for this GPU\n", fmt->w, fmt->h);
        mb_destroy(c);
        return NULL;
    }
    if (res_fixed_init(c) < 0 || res_geom_init(c) < 0) {
        fprintf(stderr, "motionblade: Vulkan init failed\n");
        mb_destroy(c);
        return NULL;
    }
    c->ok = 1;
    return c;
}
