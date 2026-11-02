// ---- 3D shapes drawn by the mod (Vulkan)
// The game renders with Vulkan and loads it at run time: it gets vkGetInstanceProcAddr
// with GetProcAddress (its only import of it). The mod patches that import, hands out
// wrappers for vkGetInstanceProcAddr / vkGetDeviceProcAddr and through them for the
// functions it needs: vkCreateSwapchainKHR (to know the window's images) and
// vkQueuePresentKHR. Before each present it records a small command buffer that draws
// the area box (see shaders/box.vert) and the blocks the action will touch (shaders/cells.vert)
// into the image being presented, waits for the game's own semaphores and makes the present
// wait for it instead. The game's scene depth image is sampled so parts hidden by the world
// are drawn dimmer; the camera comes from the game thread's cursor system (main.cpp).
// Included from main.cpp (single translation unit).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "box_vert.h"
#include "box_frag.h"
#include "box_frag_depth.h"
#include "cells_vert.h"
#include "cells_frag.h"
#include "cells_frag_depth.h"

static FARPROC (WINAPI* g_realGPA)(HMODULE, LPCSTR) = nullptr;
static PFN_vkGetInstanceProcAddr g_vkGIPA = nullptr;
static PFN_vkGetDeviceProcAddr g_vkGDPA = nullptr;
static PFN_vkCreateSwapchainKHR g_vkCreateSwapchain = nullptr;
static PFN_vkDestroySwapchainKHR g_vkDestroySwapchain = nullptr;
static PFN_vkQueuePresentKHR g_vkPresent = nullptr;
static PFN_vkGetDeviceQueue g_vkGetQueue = nullptr;
static PFN_vkGetDeviceQueue2 g_vkGetQueue2 = nullptr;
static VkInstance g_vkInst = VK_NULL_HANDLE;      // the game's instance and the GPU its device runs on
static VkPhysicalDevice g_vkPhys = VK_NULL_HANDLE;
static PFN_vkCreateDevice g_oCreateDevice = nullptr;
static volatile LONG g_vkSeen = 0;               // 1 = the game fetched vkGetInstanceProcAddr through us
static volatile LONG g_vkPresents = 0, g_vkDraws = 0;
static CRITICAL_SECTION g_vkLock;

// ---- The game's scene depth, so the box can tell which of its parts the world hides.
// Image and view creation is noted (attachments only) to know each view's image, format
// and usage; dynamic-rendering passes then tell which depth image belongs to the world.
struct HInfo { uint64_t key; uint32_t a[12]; };
struct HTable {
    HInfo* t; uint32_t mask;
    HInfo* get(uint64_t k, bool add) {
        if (!k || !t) return nullptr;
        uint32_t i = (uint32_t)((k >> 4) * 0x9E3779B1u) & mask;
        for (uint32_t n = 0; n <= mask; n++, i = (i + 1) & mask) {
            if (t[i].key == k) return &t[i];
            if (!t[i].key) { if (!add) return nullptr; t[i].key = k; return &t[i]; }
        }
        return nullptr;
    }
};
static HInfo g_imgT[32768], g_viewT[32768];
static HTable g_imgs = { g_imgT, 32767 }, g_views = { g_viewT, 32767 };
static CRITICAL_SECTION g_imgLock;

static PFN_vkCreateImage g_oCreateImage; static PFN_vkCreateImageView g_oCreateView;
static PFN_vkCmdBeginRendering g_oBeginR, g_oBeginRK;
static PFN_vkCmdPipelineBarrier g_oBarrier; static PFN_vkCmdPipelineBarrier2 g_oBarrier2, g_oBarrier2K;

// The game's scene depth: the depth image of its last world pass with an HDR colour target
// (R16G16B16A16 float) each frame, and the layout its last barrier left it in.
static volatile uint64_t g_depthImg = 0;
static volatile uint32_t g_depthFmt = 0, g_depthW = 0, g_depthH = 0;
static volatile LONG g_depthLayout = -1;

static VkResult VKAPI_CALL h_CreateImage(VkDevice d, const VkImageCreateInfo* ci, const VkAllocationCallbacks* a, VkImage* out) {
    VkResult r = g_oCreateImage(d, ci, a, out);
    if (r == VK_SUCCESS && ci && out && ci->extent.width >= 640 &&
        (ci->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
        EnterCriticalSection(&g_imgLock);
        if (HInfo* h = g_imgs.get((uint64_t)*out, true)) { h->a[0] = ci->format; h->a[1] = ci->extent.width; h->a[2] = ci->extent.height; h->a[3] = ci->usage; h->a[4] = ci->samples; }
        LeaveCriticalSection(&g_imgLock);
    }
    return r;
}
static VkResult VKAPI_CALL h_CreateView(VkDevice d, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a, VkImageView* out) {
    VkResult r = g_oCreateView(d, ci, a, out);
    if (r == VK_SUCCESS && ci && out) {
        EnterCriticalSection(&g_imgLock);
        if (g_imgs.get((uint64_t)ci->image, false))
            if (HInfo* h = g_views.get((uint64_t)*out, true)) { h->a[0] = ci->format; uint64_t im = (uint64_t)ci->image; h->a[1] = (uint32_t)im; h->a[2] = (uint32_t)(im >> 32); }
        LeaveCriticalSection(&g_imgLock);
    }
    return r;
}
static uint32_t view_fmt(VkImageView v, uint64_t* img) {
    HInfo* h = g_views.get((uint64_t)v, false);
    if (!h) return 0xffffffff;
    if (img) *img = (uint64_t)h->a[1] | ((uint64_t)h->a[2] << 32);
    return h->a[0];
}
static void depth_note(const VkRenderingInfo* ri) {
    if (!ri || !ri->pDepthAttachment || !ri->pDepthAttachment->imageView || ri->renderArea.extent.width < 640) return;
    bool hdr = false;
    EnterCriticalSection(&g_imgLock);
    for (uint32_t i = 0; i < ri->colorAttachmentCount && !hdr; i++)
        hdr = view_fmt(ri->pColorAttachments[i].imageView, nullptr) == VK_FORMAT_R16G16B16A16_SFLOAT;
    uint64_t img = 0;
    uint32_t fmt = hdr ? view_fmt(ri->pDepthAttachment->imageView, &img) : 0xffffffff;
    HInfo* h = hdr && img ? g_imgs.get(img, false) : nullptr;
    LeaveCriticalSection(&g_imgLock);
    if (!h || fmt == 0xffffffff || !(h->a[3] & VK_IMAGE_USAGE_SAMPLED_BIT)) return;
    if (img != g_depthImg) { g_depthLayout = -1; g_depthImg = img; }
    g_depthFmt = fmt; g_depthW = h->a[1]; g_depthH = h->a[2];
}
static void note_layout(VkImage img, VkImageLayout nl) { if (img && (uint64_t)img == g_depthImg) g_depthLayout = (LONG)nl; }
static void VKAPI_CALL h_Barrier(VkCommandBuffer cb, VkPipelineStageFlags a, VkPipelineStageFlags b, VkDependencyFlags c, uint32_t nm,
    const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* bb, uint32_t ni, const VkImageMemoryBarrier* ib) {
    if (g_depthImg && ib) for (uint32_t i = 0; i < ni; i++) note_layout(ib[i].image, ib[i].newLayout);
    g_oBarrier(cb, a, b, c, nm, m, nb, bb, ni, ib);
}
static void barrier2(PFN_vkCmdPipelineBarrier2 o, VkCommandBuffer cb, const VkDependencyInfo* di) {
    if (g_depthImg && di && di->pImageMemoryBarriers)
        for (uint32_t i = 0; i < di->imageMemoryBarrierCount; i++) note_layout(di->pImageMemoryBarriers[i].image, di->pImageMemoryBarriers[i].newLayout);
    o(cb, di);
}
static void VKAPI_CALL h_Barrier2(VkCommandBuffer cb, const VkDependencyInfo* di) { barrier2(g_oBarrier2, cb, di); }
static void VKAPI_CALL h_Barrier2K(VkCommandBuffer cb, const VkDependencyInfo* di) { barrier2(g_oBarrier2K, cb, di); }
static void VKAPI_CALL h_BeginR(VkCommandBuffer cb, const VkRenderingInfo* ri) { if (g_cfg.drawBoxDepth) depth_note(ri); g_oBeginR(cb, ri); }
static void VKAPI_CALL h_BeginRK(VkCommandBuffer cb, const VkRenderingInfo* ri) { if (g_cfg.drawBoxDepth) depth_note(ri); g_oBeginRK(cb, ri); }

// device functions the drawing needs
#define VKD_FUNCS(X) X(vkGetSwapchainImagesKHR) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateRenderPass) \
    X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCreateFence) X(vkDestroyFence) \
    X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkQueueSubmit) X(vkDeviceWaitIdle) \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdBindDescriptorSets) X(vkCmdPipelineBarrier) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory)
struct VkD {
    VkDevice dev = VK_NULL_HANDLE;
#define X(n) PFN_##n n = nullptr;
    VKD_FUNCS(X)
#undef X
};
static VkD g_vd;

struct QueueFam { VkQueue q; uint32_t fam; };
static QueueFam g_vkQueues[16];
static volatile LONG g_vkQueueN = 0;
static void vk_note_queue(VkQueue q, uint32_t fam) {
    LONG n = g_vkQueueN; if (n > 16) n = 16;
    for (LONG i = 0; i < n; i++) if (g_vkQueues[i].q == q) return;
    LONG k = InterlockedIncrement(&g_vkQueueN) - 1;
    if (k < 16) { g_vkQueues[k].q = q; g_vkQueues[k].fam = fam; }
}
static bool vk_queue_family(VkQueue q, uint32_t* fam) {
    LONG n = g_vkQueueN; if (n > 16) n = 16;
    for (LONG i = 0; i < n; i++) if (g_vkQueues[i].q == q) { *fam = g_vkQueues[i].fam; return true; }
    return false;
}

static const uint32_t kVkMaxImages = 8;
struct VkSc {
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkExtent2D ext = {};
    uint32_t n = 0;
    VkImage img[kVkMaxImages] = {};
    VkImageView view[kVkMaxImages] = {};
    VkFramebuffer fb[kVkMaxImages] = {};
    VkCommandBuffer cb[kVkMaxImages] = {};
    VkFence fence[kVkMaxImages] = {};
    VkSemaphore sem[kVkMaxImages] = {};
    VkRenderPass rp = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline fill = VK_NULL_HANDLE, fillDepth = VK_NULL_HANDLE, cells = VK_NULL_HANDLE, cellsDepth = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet set[kVkMaxImages] = {};
    VkImageView setView[kVkMaxImages] = {};          // depth view written into set[i]
    uint64_t depthImg[4] = {}; VkImageView depthView[4] = {};   // own views of the game's depth images
    // which cells already hold blocks: one bit per cell, host visible, shared by all frames (set 1)
    VkDescriptorSetLayout occLayout = VK_NULL_HANDLE;
    VkDescriptorPool occPool = VK_NULL_HANDLE;
    VkDescriptorSet occSet = VK_NULL_HANDLE;
    VkBuffer occBuf = VK_NULL_HANDLE;
    VkDeviceMemory occMem = VK_NULL_HANDLE;
    uint32_t* occMap = nullptr;
    uint32_t maxPush = 0;
    uint32_t poolFam = 0xffffffff;
    bool ready = false, failed = false;
};
static VkSc g_vs;

static bool vk_load_device(VkDevice dev) {
    if (g_vd.dev == dev && g_vd.vkQueueSubmit) return true;
    if (!g_vkGDPA) return false;
    VkD d; d.dev = dev;
#define X(n) d.n = (PFN_##n)g_vkGDPA(dev, #n); if (!d.n) { LOG(.str("vulkan: no " #n)); return false; }
    VKD_FUNCS(X)
#undef X
    g_vd = d;
    return true;
}

static void vk_destroy_sc() {
    VkSc& s = g_vs;
    g_vkDrawOk = 0;
    if (!g_vd.dev) { s = VkSc(); return; }
    VkDevice d = g_vd.dev;
    if (s.ready || s.pool) g_vd.vkDeviceWaitIdle(d);
    for (uint32_t i = 0; i < s.n; i++) {
        if (s.fb[i]) g_vd.vkDestroyFramebuffer(d, s.fb[i], nullptr);
        if (s.view[i]) g_vd.vkDestroyImageView(d, s.view[i], nullptr);
        if (s.fence[i]) g_vd.vkDestroyFence(d, s.fence[i], nullptr);
        if (s.sem[i]) g_vd.vkDestroySemaphore(d, s.sem[i], nullptr);
    }
    if (s.fill) g_vd.vkDestroyPipeline(d, s.fill, nullptr);
    if (s.fillDepth) g_vd.vkDestroyPipeline(d, s.fillDepth, nullptr);
    if (s.cells) g_vd.vkDestroyPipeline(d, s.cells, nullptr);
    if (s.cellsDepth) g_vd.vkDestroyPipeline(d, s.cellsDepth, nullptr);
    for (int k = 0; k < 4; k++) if (s.depthView[k]) g_vd.vkDestroyImageView(d, s.depthView[k], nullptr);
    if (s.descPool) g_vd.vkDestroyDescriptorPool(d, s.descPool, nullptr);
    if (s.occPool) g_vd.vkDestroyDescriptorPool(d, s.occPool, nullptr);
    if (s.occLayout) g_vd.vkDestroyDescriptorSetLayout(d, s.occLayout, nullptr);
    if (s.occBuf) g_vd.vkDestroyBuffer(d, s.occBuf, nullptr);
    if (s.occMem) g_vd.vkFreeMemory(d, s.occMem, nullptr);     // unmaps too
    if (s.setLayout) g_vd.vkDestroyDescriptorSetLayout(d, s.setLayout, nullptr);
    if (s.sampler) g_vd.vkDestroySampler(d, s.sampler, nullptr);
    if (s.layout) g_vd.vkDestroyPipelineLayout(d, s.layout, nullptr);
    if (s.rp) g_vd.vkDestroyRenderPass(d, s.rp, nullptr);
    if (s.pool) g_vd.vkDestroyCommandPool(d, s.pool, nullptr);    // frees the command buffers
    s = VkSc();
}

struct VkPush { float mvp[16]; float size[4]; float eye[4]; float capA[4]; float capB[4]; float cells[4]; };   // see shaders/box.frag, cells.vert   // size: metres xyz, seconds w; eye: camera in box units

static VkPipeline vk_make_pipeline(VkShaderModule vs, VkShaderModule fs, bool lines) {
    VkSc& s = g_vs;
    VkPipelineShaderStageCreateInfo st[2] = {};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {}; vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia = {}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {}; vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms = {}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = {};
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {}; cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = {}; ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp = {}; gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.pDynamicState = &ds;
    gp.layout = s.layout; gp.renderPass = s.rp; gp.subpass = 0;
    VkPipeline p = VK_NULL_HANDLE;
    if (g_vd.vkCreateGraphicsPipelines(g_vd.dev, VK_NULL_HANDLE, 1, &gp, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
}

// One small host-visible buffer for the occupied-cells bits. Needs the GPU's memory types
// (instance function on the physical device the game created its device on).
static const uint32_t kOccWords = 1250;              // 40000 cells
static void vk_init_occ(VkDevice dev) {
    VkSc& s = g_vs;
    if (!s.occSet || !g_vkInst || !g_vkPhys || !g_vkGIPA) { LOG(.str("vulkan: block bits not available (no GPU handle)")); return; }
    auto getMem = (PFN_vkGetPhysicalDeviceMemoryProperties)g_vkGIPA(g_vkInst, "vkGetPhysicalDeviceMemoryProperties");
    auto getProps = (PFN_vkGetPhysicalDeviceProperties)g_vkGIPA(g_vkInst, "vkGetPhysicalDeviceProperties");
    if (getProps) {
        static VkPhysicalDeviceProperties pp;      // large; not on the stack
        getProps(g_vkPhys, &pp);
        s.maxPush = pp.limits.maxPushConstantsSize;
    }
    if (!getMem) return;
    VkPhysicalDeviceMemoryProperties mp = {};
    getMem(g_vkPhys, &mp);
    VkBufferCreateInfo bi = {}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = kOccWords * 4; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_vd.vkCreateBuffer(dev, &bi, nullptr, &s.occBuf) != VK_SUCCESS) { s.occBuf = VK_NULL_HANDLE; return; }
    VkMemoryRequirements mr = {};
    g_vd.vkGetBufferMemoryRequirements(dev, s.occBuf, &mr);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = 0xffffffff;
    for (uint32_t t = 0; t < mp.memoryTypeCount && t < 32; t++)
        if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) { type = t; break; }
    if (type == 0xffffffff) { LOG(.str("vulkan: block bits: no host memory type")); return; }
    VkMemoryAllocateInfo ai = {}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size; ai.memoryTypeIndex = type;
    void* map = nullptr;
    if (g_vd.vkAllocateMemory(dev, &ai, nullptr, &s.occMem) != VK_SUCCESS) { s.occMem = VK_NULL_HANDLE; return; }
    if (g_vd.vkBindBufferMemory(dev, s.occBuf, s.occMem, 0) != VK_SUCCESS ||
        g_vd.vkMapMemory(dev, s.occMem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS || !map) return;
    memset(map, 0, kOccWords * 4);
    VkDescriptorBufferInfo dbi = { s.occBuf, 0, kOccWords * 4 };
    VkWriteDescriptorSet w = {}; w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = s.occSet; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &dbi;
    g_vd.vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    s.occMap = (uint32_t*)map;
    LOG(.str("vulkan: block bits ready (memory type ").u(type).str("), max push constants ").u(s.maxPush));
}

// Everything that depends on the swapchain (not the queue): images, views, render pass,
// framebuffers, pipelines, fences, semaphores.
static bool vk_init_sc(VkDevice dev, VkSwapchainKHR sc, const VkSwapchainCreateInfoKHR* ci) {
    if (!vk_load_device(dev)) return false;
    VkSc& s = g_vs;
    s.sc = sc; s.fmt = ci->imageFormat; s.ext = ci->imageExtent;
    if (!(ci->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) { LOG(.str("vulkan: swapchain images are not render targets")); return false; }
    uint32_t n = 0;
    g_vd.vkGetSwapchainImagesKHR(dev, sc, &n, nullptr);
    if (n == 0 || n > kVkMaxImages) { LOG(.str("vulkan: swapchain image count ").u(n)); return false; }
    g_vd.vkGetSwapchainImagesKHR(dev, sc, &n, s.img);
    s.n = n;
    VkAttachmentDescription at = {};
    at.format = s.fmt; at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; at.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = {}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &ref;
    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi = {}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1; rpi.pAttachments = &at; rpi.subpassCount = 1; rpi.pSubpasses = &sp; rpi.dependencyCount = 1; rpi.pDependencies = &dep;
    if (g_vd.vkCreateRenderPass(dev, &rpi, nullptr, &s.rp) != VK_SUCCESS) { LOG(.str("vulkan: render pass failed")); return false; }
    for (uint32_t i = 0; i < n; i++) {
        VkImageViewCreateInfo vi = {}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = s.img[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = s.fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        if (g_vd.vkCreateImageView(dev, &vi, nullptr, &s.view[i]) != VK_SUCCESS) { LOG(.str("vulkan: image view failed")); return false; }
        VkFramebufferCreateInfo fi = {}; fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = s.rp; fi.attachmentCount = 1; fi.pAttachments = &s.view[i];
        fi.width = s.ext.width; fi.height = s.ext.height; fi.layers = 1;
        if (g_vd.vkCreateFramebuffer(dev, &fi, nullptr, &s.fb[i]) != VK_SUCCESS) { LOG(.str("vulkan: framebuffer failed")); return false; }
        VkFenceCreateInfo fc = {}; fc.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo sc2 = {}; sc2.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (g_vd.vkCreateFence(dev, &fc, nullptr, &s.fence[i]) != VK_SUCCESS ||
            g_vd.vkCreateSemaphore(dev, &sc2, nullptr, &s.sem[i]) != VK_SUCCESS) { LOG(.str("vulkan: sync objects failed")); return false; }
    }
    {   // depth sampling: one descriptor set per swapchain image (updated after its fence)
        VkSamplerCreateInfo si = {}; si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkDescriptorSetLayoutBinding b = {}; b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dl = {}; dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dl.bindingCount = 1; dl.pBindings = &b;
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, n };
        VkDescriptorPoolCreateInfo dp = {}; dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dp.maxSets = n; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
        VkDescriptorSetLayout sl[kVkMaxImages];
        if (g_vd.vkCreateSampler(dev, &si, nullptr, &s.sampler) != VK_SUCCESS ||
            g_vd.vkCreateDescriptorSetLayout(dev, &dl, nullptr, &s.setLayout) != VK_SUCCESS ||
            g_vd.vkCreateDescriptorPool(dev, &dp, nullptr, &s.descPool) != VK_SUCCESS) { LOG(.str("vulkan: depth sampling objects failed")); return false; }
        for (uint32_t i = 0; i < n; i++) sl[i] = s.setLayout;
        VkDescriptorSetAllocateInfo ai = {}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = s.descPool; ai.descriptorSetCount = n; ai.pSetLayouts = sl;
        if (g_vd.vkAllocateDescriptorSets(dev, &ai, s.set) != VK_SUCCESS) { LOG(.str("vulkan: descriptor sets failed")); return false; }
    }
    {   // the occupied-cells bits (set 1, read by the blocks' vertex shader)
        VkDescriptorSetLayoutBinding b = {}; b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo dl = {}; dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dl.bindingCount = 1; dl.pBindings = &b;
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
        VkDescriptorPoolCreateInfo dp = {}; dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dp.maxSets = 1; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
        if (g_vd.vkCreateDescriptorSetLayout(dev, &dl, nullptr, &s.occLayout) != VK_SUCCESS) { LOG(.str("vulkan: block bits layout failed")); return false; }
        if (g_vd.vkCreateDescriptorPool(dev, &dp, nullptr, &s.occPool) == VK_SUCCESS) {
            VkDescriptorSetAllocateInfo ai = {}; ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = s.occPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &s.occLayout;
            if (g_vd.vkAllocateDescriptorSets(dev, &ai, &s.occSet) != VK_SUCCESS) s.occSet = VK_NULL_HANDLE;
        }
        vk_init_occ(dev);
    }
    if (s.maxPush && s.maxPush < sizeof(VkPush)) {   // the box needs 144 bytes of push constants (most GPUs have 256)
        LOG(.str("vulkan: GPU allows only ").u(s.maxPush).str(" bytes of push constants, drawing off (edge highlights instead)"));
        return false;
    }
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VkPush) };
    VkDescriptorSetLayout sets2[2] = { s.setLayout, s.occLayout };
    VkPipelineLayoutCreateInfo li = {}; li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
    li.setLayoutCount = 2; li.pSetLayouts = sets2;
    if (g_vd.vkCreatePipelineLayout(dev, &li, nullptr, &s.layout) != VK_SUCCESS) { LOG(.str("vulkan: pipeline layout failed")); return false; }
    VkShaderModuleCreateInfo mi = {}; mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    mi.codeSize = sizeof(kBoxVert); mi.pCode = kBoxVert;
    g_vd.vkCreateShaderModule(dev, &mi, nullptr, &vs);
    mi.codeSize = sizeof(kBoxFrag); mi.pCode = kBoxFrag;
    g_vd.vkCreateShaderModule(dev, &mi, nullptr, &fs);
    if (vs && fs) s.fill = vk_make_pipeline(vs, fs, false);
    if (fs) g_vd.vkDestroyShaderModule(dev, fs, nullptr);
    fs = VK_NULL_HANDLE;
    mi.codeSize = sizeof(kBoxFragDepth); mi.pCode = kBoxFragDepth;
    g_vd.vkCreateShaderModule(dev, &mi, nullptr, &fs);
    if (vs && fs) s.fillDepth = vk_make_pipeline(vs, fs, false);
    if (!s.fillDepth) LOG(.str("vulkan: depth pipeline failed (box drawn without depth)"));
    if (vs) g_vd.vkDestroyShaderModule(dev, vs, nullptr);
    if (fs) g_vd.vkDestroyShaderModule(dev, fs, nullptr);
    {   // the affected blocks
        VkShaderModule cv = VK_NULL_HANDLE, cf = VK_NULL_HANDLE, cfd = VK_NULL_HANDLE;
        mi.codeSize = sizeof(kCellsVert); mi.pCode = kCellsVert; g_vd.vkCreateShaderModule(dev, &mi, nullptr, &cv);
        mi.codeSize = sizeof(kCellsFrag); mi.pCode = kCellsFrag; g_vd.vkCreateShaderModule(dev, &mi, nullptr, &cf);
        mi.codeSize = sizeof(kCellsFragDepth); mi.pCode = kCellsFragDepth; g_vd.vkCreateShaderModule(dev, &mi, nullptr, &cfd);
        if (cv && cf) s.cells = vk_make_pipeline(cv, cf, false);
        if (cv && cfd) s.cellsDepth = vk_make_pipeline(cv, cfd, false);
        if (cv) g_vd.vkDestroyShaderModule(dev, cv, nullptr);
        if (cf) g_vd.vkDestroyShaderModule(dev, cf, nullptr);
        if (cfd) g_vd.vkDestroyShaderModule(dev, cfd, nullptr);
        if (!s.cells || !s.cellsDepth) LOG(.str("vulkan: block pipelines failed (box only)"));
    }
    fs = VK_NULL_HANDLE;
    if (fs) g_vd.vkDestroyShaderModule(dev, fs, nullptr);
    if (!s.fill) { LOG(.str("vulkan: pipelines failed")); return false; }
    s.ready = true;
    LOG(.str("vulkan: drawing ready, ").u(n).str(" images ").u(s.ext.width).c('x').u(s.ext.height).str(" format ").u(s.fmt));
    return true;
}

// command pool + buffers for the present queue's family (first present)
static bool vk_init_pool(VkQueue q) {
    VkSc& s = g_vs;
    uint32_t fam = 0;
    if (!vk_queue_family(q, &fam)) { fam = 0; }
    if (s.pool && s.poolFam == fam) return true;
    if (s.pool) return false;                           // presents from another family: not handled
    VkCommandPoolCreateInfo pc = {}; pc.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pc.queueFamilyIndex = fam;
    if (g_vd.vkCreateCommandPool(g_vd.dev, &pc, nullptr, &s.pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai = {}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = s.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = s.n;
    if (g_vd.vkAllocateCommandBuffers(g_vd.dev, &ai, s.cb) != VK_SUCCESS) return false;
    s.poolFam = fam;
    LOG(.str("vulkan: present queue family ").u(fam));
    return true;
}

// ---- what to draw: the area box, camera relative, from the game thread
static volatile LONG g_dbSeq = 0;
static int64_t g_dbLo[3], g_dbHi[3];
static volatile DWORD g_dbTick = 0;
static float g_cellStep[3];                       // shape size of the area's cells (metres)
// Which cells of the area already hold blocks (remove / replace overlap), one bit per cell, written
// by the game thread (occ_update in main.cpp). g_occReady: the bits match the current area.
static volatile uint32_t g_occBits[1250];
static volatile LONG g_occReady = 0;
static bool draw_box_on() { return g_cfg.drawBox != 0 && g_vkSeen; }
static void set_draw_box(bool on, const int64_t* lo, const int64_t* hi) {
    if (!on) { g_dbTick = 0; return; }
    InterlockedIncrement(&g_dbSeq);
    memcpy(g_dbLo, lo, 24); memcpy(g_dbHi, hi, 24);
    InterlockedIncrement(&g_dbSeq);
    DWORD t = GetTickCount(); g_dbTick = t ? t : 1;
}

// Model-view-projection for the unit cube onto the box, camera relative (column major).
// Camera axes from its quaternion: right = R*(1,0,0), up = R*(0,1,0), forward = R*(0,0,1).
static bool box_mvp(VkPush& P, float aspect, bool* inside) {
    float* out = P.mvp; float* size = P.size; float* eye = P.eye;
    DWORD t = g_dbTick;
    if (!t || GetTickCount() - t > 300) return false;
    if (GetTickCount() - g_camSysTick > 300) return false;
    int64_t lo[3], hi[3], cam[3];
    float q[4];
    LONG a0 = g_dbSeq;
    if (a0 & 1) return false;
    memcpy(lo, g_dbLo, 24); memcpy(hi, g_dbHi, 24);
    if (g_dbSeq != a0) return false;
    {   // the camera of the frame on screen: DrawBoxDelay100 hundredths of a game frame back, or ahead
        // (negative: predicted from the last two frames); torn copies retried
        LONG n = g_camN, d = g_cfg.drawBoxDelay < -400 ? -400 : (g_cfg.drawBoxDelay > 1200 ? 1200 : g_cfg.drawBoxDelay);
        LONG back = d > 0 ? d / 100 : 0;
        double k = d > 0 ? -(double)(d - back * 100) / 100.0 : -(double)d / 100.0;   // along (newest - previous)
        if (n <= back + 1) return false;
        int64_t cam2[3]; float q2[4];
        bool ok = false;
        for (int tries = 0; tries < 4 && !ok; tries++) {
            const CamSnap& c = g_camRing[(n - 1 - back) & 15];
            const CamSnap& p = g_camRing[(n - 2 - back) & 15];
            LONG s0 = c.seq, s1 = p.seq;
            if ((s0 | s1) & 1) continue;
            memcpy(cam, c.cam, 24); memcpy(q, c.rot, 16);
            memcpy(cam2, p.cam, 24); memcpy(q2, p.rot, 16);
            ok = c.seq == s0 && p.seq == s1;
        }
        if (!ok) return false;
        if (k != 0) {
            for (int i = 0; i < 3; i++) cam[i] += (int64_t)((double)(cam[i] - cam2[i]) * k);
            float dot = q[0] * q2[0] + q[1] * q2[1] + q[2] * q2[2] + q[3] * q2[3];
            float sg = dot < 0 ? -1.f : 1.f, len = 0;
            for (int i = 0; i < 4; i++) { q[i] += (float)k * (q[i] - sg * q2[i]); len += q[i] * q[i]; }
            if (len < 1e-6f) return false;
            // 1/sqrt without a C runtime: Newton steps from 1 (len is close to 1)
            float r = 1.f; for (int it = 0; it < 4; it++) r = r * (1.5f - 0.5f * len * r * r);
            for (int i = 0; i < 4; i++) q[i] *= r;
        }
    }
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double R[3] = { 1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y) };
    double U[3] = { 2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x) };
    double F[3] = { 2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y) };
    double L[3], S[3];
    for (int i = 0; i < 3; i++) { L[i] = (double)(lo[i] - cam[i]) * (1.0 / 4294967296.0); S[i] = (double)(hi[i] - lo[i]) * (1.0 / 4294967296.0); }
    for (int i = 0; i < 3; i++) { size[i] = (float)S[i]; eye[i] = S[i] > 0 ? (float)(-L[i] / S[i]) : 0.5f; }
    double fov = (g_cfg.drawBoxFov >= 10 && g_cfg.drawBoxFov <= 170 ? g_cfg.drawBoxFov : 60) * 3.14159265358979 / 180.0;
    // tan(fov/2) without a C runtime: sin/cos series is enough for 10..170 degrees
    double h = fov * 0.5, h2 = h * h;
    double sn = h * (1 - h2 / 6 * (1 - h2 / 20 * (1 - h2 / 42 * (1 - h2 / 72 * (1 - h2 / 110)))));
    double cs = 1 - h2 / 2 * (1 - h2 / 12 * (1 - h2 / 30 * (1 - h2 / 56 * (1 - h2 / 90 * (1 - h2 / 132)))));
    double ty = sn / cs;
    double sy = 1.0 / ty, sx = sy / (aspect > 0 ? aspect : 1.f);
    const double nearZ = 0.05;
    double m[4][4] = {};                       // [row][col]
    for (int i = 0; i < 3; i++) {
        m[0][i] = sx * R[i] * S[i];
        m[1][i] = -sy * U[i] * S[i];
        m[3][i] = F[i] * S[i];
    }
    m[0][3] = sx * (R[0] * L[0] + R[1] * L[1] + R[2] * L[2]);
    m[1][3] = -sy * (U[0] * L[0] + U[1] * L[1] + U[2] * L[2]);
    m[2][3] = nearZ;
    m[3][3] = F[0] * L[0] + F[1] * L[1] + F[2] * L[2];
    for (int col = 0; col < 4; col++) for (int row = 0; row < 4; row++) out[col * 4 + row] = (float)m[row][col];
    // The character: a capsule below the camera's look-at point (camera distance along the view).
    double dist = g_camDist, T[3];
    for (int i = 0; i < 3; i++) T[i] = F[i] * dist;              // camera relative, world axes (y up)
    const double below = 1.7, above = 0.3, rad = 0.3;
    double a3[3] = { T[0], T[1] - below, T[2] }, b3[3] = { T[0], T[1] + above, T[2] };
    auto view = [&](const double* w, float* o) {
        o[0] = (float)(R[0] * w[0] + R[1] * w[1] + R[2] * w[2]);
        o[1] = (float)(U[0] * w[0] + U[1] * w[1] + U[2] * w[2]);
        o[2] = (float)(F[0] * w[0] + F[1] * w[1] + F[2] * w[2]);
    };
    view(a3, P.capA); view(b3, P.capB);
    P.capA[3] = (float)(1.0 / sx); P.capB[3] = (float)(1.0 / sy);
    *inside = T[0] > L[0] - rad && T[0] < L[0] + S[0] + rad && T[2] > L[2] - rad && T[2] < L[2] + S[2] + rad &&
              b3[1] > L[1] && a3[1] < L[1] + S[1];
    return true;
}

static bool vk_draw(VkQueue q, uint32_t i, const VkPresentInfoKHR* pi) {
    VkSc& s = g_vs;
    VkPush push = {};
    bool inside = false;
    if (!box_mvp(push, (float)s.ext.width / (float)(s.ext.height ? s.ext.height : 1), &inside)) return false;
    if (!vk_init_pool(q)) return false;
    VkDevice d = g_vd.dev;
    if (g_vd.vkWaitForFences(d, 1, &s.fence[i], VK_TRUE, 100000000ull) != VK_SUCCESS) return false;
    g_vd.vkResetFences(d, 1, &s.fence[i]);
    VkCommandBuffer cb = s.cb[i];
    g_vd.vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_vd.vkBeginCommandBuffer(cb, &bi);
    // the game's depth image, read in the fragment shader (made readable for the draw, then put back)
    VkImage dimg = VK_NULL_HANDLE;
    VkImageLayout dlay = VK_IMAGE_LAYOUT_UNDEFINED, readLay = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags dasp = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (g_cfg.drawBoxDepth && s.fillDepth) {
        uint64_t img = g_depthImg; LONG lay = g_depthLayout; uint32_t fmt = g_depthFmt;
        bool dfmt = fmt == VK_FORMAT_D32_SFLOAT || fmt == VK_FORMAT_D16_UNORM || fmt == VK_FORMAT_X8_D24_UNORM_PACK32 ||
                    fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
        if (img && lay > 0 && dfmt) {
            if (fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT) dasp |= VK_IMAGE_ASPECT_STENCIL_BIT;
            int slot = -1, freeSlot = -1;
            for (int k = 0; k < 4; k++) { if (s.depthImg[k] == img) slot = k; if (!s.depthImg[k] && freeSlot < 0) freeSlot = k; }
            if (slot < 0 && freeSlot >= 0) {
                VkImageViewCreateInfo vi = {}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                vi.image = (VkImage)img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = (VkFormat)fmt;
                vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
                if (g_vd.vkCreateImageView(d, &vi, nullptr, &s.depthView[freeSlot]) == VK_SUCCESS) {
                    s.depthImg[freeSlot] = img; slot = freeSlot;
                    LOG(.str("vulkan: scene depth ").x(img).c(' ').u(g_depthW).c('x').u(g_depthH).str(" format ").u(fmt).str(" layout ").i(lay));
                }
            }
            if (slot >= 0) {
                if (s.setView[i] != s.depthView[slot]) {
                    VkDescriptorImageInfo ii = { s.sampler, s.depthView[slot], VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
                    VkWriteDescriptorSet w = {}; w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w.dstSet = s.set[i]; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
                    g_vd.vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
                    s.setView[i] = s.depthView[slot];
                }
                dimg = (VkImage)img; dlay = (VkImageLayout)lay; readLay = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                if (dlay != readLay) {
                    VkImageMemoryBarrier b = {}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    b.oldLayout = dlay; b.newLayout = readLay;
                    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = dimg; b.subresourceRange.aspectMask = dasp; b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
                    g_vd.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
                }
            }
        }
    }
    VkRenderPassBeginInfo rb = {}; rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = s.rp; rb.framebuffer = s.fb[i]; rb.renderArea.extent = s.ext;
    g_vd.vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = { 0.f, 0.f, (float)s.ext.width, (float)s.ext.height, 0.f, 1.f };
    VkRect2D sc = { { 0, 0 }, s.ext };
    g_vd.vkCmdSetViewport(cb, 0, 1, &vp);
    g_vd.vkCmdSetScissor(cb, 0, 1, &sc);
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if (dimg) g_vd.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &s.set[i], 0, nullptr);
    // the blocks the action will touch (fill mode cells), drawn after the box
    uint32_t cellN = 0;
    if (g_cfg.drawBoxCells && g_cellStep[0] > 0.f && (dimg ? s.cellsDepth : s.cells)) {
        uint64_t nn = 1;
        for (int a = 0; a < 3; a++) {
            float c = push.size[a] / g_cellStep[a];
            int n = (int)(c + 0.5f); if (n < 1) n = 1;
            nn *= (uint64_t)n;
            push.cells[a] = g_cellStep[a];
        }
        LONG op = g_scOp == OP_REMOVE ? 2 : g_replace ? 1 : 0;
        const LONG boxFill = g_boxFill;
        push.cells[3] = (float)((g_fill >= core::FILL_ROUND ? 3 : g_fill & 3) + 4 * op + boxFill);
        if (nn <= 40000) cellN = (uint32_t)nn;
        bool onlyFull = op == 2 || (op == 1 && g_replaceStyle != STYLE_FULL);
        const bool isRound = g_fill == core::FILL_ROUND && (boxFill & (1 << 20)), isDiag = g_fill == core::FILL_DIAG && (boxFill & (1 << 21));
        if (cellN && (isRound || isDiag) && s.occMap) {
            // round and diagonal fills: the cells come from the same code that builds (core::round_keep, diag_keep),
            // as bits (with the existing blocks when only those count); the shader draws them as solid
            int n[3];
            for (int a = 0; a < 3; a++) { int k = (int)(push.size[a] / g_cellStep[a] + 0.5f); n[a] = k < 1 ? 1 : k; }
            static uint32_t bits[kOccWords]; static LONG key = -1; static int kn[3];
            if (key != boxFill || kn[0] != n[0] || kn[1] != n[1] || kn[2] != n[2]) {
                key = boxFill; kn[0] = n[0]; kn[1] = n[1]; kn[2] = n[2];
                const int shape = (boxFill >> 5) & 3, side = (boxFill >> 7) & 3, axis = (boxFill >> 12) & 3, t = ((boxFill >> 10) & 3) + 1;
                const bool hollow = (boxFill & 512) != 0;
                const int flip = (boxFill >> 5) & 1;
                int ua, va; core::round_axes(axis, &ua, &va);
                if (isDiag) { ua = (boxFill >> 6) & 3; va = (boxFill >> 8) & 3; }
                const int wa = 3 - ua - va;
                memset(bits, 0, sizeof(bits));
                int c[3];
                for (c[0] = 0; c[0] < n[0]; c[0]++)
                    for (c[2] = 0; c[2] < n[2]; c[2]++) {
                        if (wa != 1 && c[wa] > 0) continue;            // the plane once, copied along the axis below
                        for (c[1] = 0; c[1] < (wa == 1 ? 1 : n[1]); c[1]++) {
                            if (isDiag ? !core::diag_keep(flip, t, c[ua], c[va], n[ua], n[va])
                                       : !core::round_keep(shape, side, hollow, t, c[ua], c[va], n[ua], n[va])) continue;
                            int w = 3 - ua - va;
                            int d[3] = { c[0], c[1], c[2] };
                            for (d[w] = 0; d[w] < n[w]; d[w]++) {
                                int id = d[0] + n[0] * (d[1] + n[1] * d[2]);
                                bits[id >> 5] |= 1u << (id & 31);
                            }
                        }
                    }
            }
            if (onlyFull && !g_occReady) cellN = 0;   // existing blocks not read yet
            else {
                for (uint32_t k = 0; k < kOccWords; k++) s.occMap[k] = onlyFull ? bits[k] & g_occBits[k] : bits[k];
                push.cells[3] = (float)(4 * op + 16);   // solid fill, bits decide
                g_vd.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 1, 1, &s.occSet, 0, nullptr);
            }
            onlyFull = false;
        }
        if (cellN && onlyFull) {
            if (s.occMap && g_occReady) {
                memcpy(s.occMap, (const void*)g_occBits, sizeof(g_occBits));
                push.cells[3] += 16.f;
                g_vd.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 1, 1, &s.occSet, 0, nullptr);
            } else if (s.occMap) cellN = 0;          // bits for this area not read yet: no blocks rather than wrong ones
        }
    }
    push.eye[3] = (dimg ? (float)g_cfg.drawBoxDepth : 0.f) + (inside ? 4.f : 0.f);
    push.size[3] = (float)(GetTickCount() % 100000) * 0.001f;
    g_vd.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, dimg ? s.fillDepth : s.fill);
    g_vd.vkCmdPushConstants(cb, s.layout, stages, 0, sizeof(push), &push);
    g_vd.vkCmdDraw(cb, 36, 1, 0, 0);
    if (cellN) {   // the blocks over the box, so the box's blue does not tint them
        g_vd.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, dimg ? s.cellsDepth : s.cells);
        g_vd.vkCmdPushConstants(cb, s.layout, stages, 0, sizeof(push), &push);
        g_vd.vkCmdDraw(cb, 36, cellN, 0, 0);
    }
    g_vd.vkCmdEndRenderPass(cb);
    if (dimg && dlay != readLay) {   // back to the layout the game left it in
        VkImageMemoryBarrier b = {}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = readLay; b.newLayout = dlay;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = dimg; b.subresourceRange.aspectMask = dasp; b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
        g_vd.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    g_vd.vkEndCommandBuffer(cb);
    VkPipelineStageFlags waitStages[16];
    uint32_t nw = pi->waitSemaphoreCount > 16 ? 16 : pi->waitSemaphoreCount;
    for (uint32_t k = 0; k < nw; k++) waitStages[k] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = nw; si.pWaitSemaphores = pi->pWaitSemaphores; si.pWaitDstStageMask = waitStages;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s.sem[i];
    if (g_vd.vkQueueSubmit(q, 1, &si, s.fence[i]) != VK_SUCCESS) { LOG(.str("vulkan: submit failed")); s.failed = true; g_vkDrawOk = 0; return false; }
    InterlockedIncrement(&g_vkDraws);
    g_vkDrawOk = 1;
    return true;
}

static VkResult VKAPI_CALL vk_present_hook(VkQueue q, const VkPresentInfoKHR* pi) {
    InterlockedIncrement(&g_vkPresents);
    if (pi && pi->swapchainCount == 1 && pi->waitSemaphoreCount <= 16 && draw_box_on()) {
        EnterCriticalSection(&g_vkLock);
        bool drawn = false;
        uint32_t i = pi->pImageIndices[0];
        if (g_vs.ready && !g_vs.failed && pi->pSwapchains[0] == g_vs.sc && i < g_vs.n) drawn = vk_draw(q, i, pi);
        if (drawn) {
            VkPresentInfoKHR p2 = *pi;
            p2.waitSemaphoreCount = 1; p2.pWaitSemaphores = &g_vs.sem[i];
            VkResult r = g_vkPresent(q, &p2);
            LeaveCriticalSection(&g_vkLock);
            return r;
        }
        LeaveCriticalSection(&g_vkLock);
    }
    return g_vkPresent(q, pi);
}

static VkResult VKAPI_CALL vk_create_swapchain_hook(VkDevice dev, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a,
                                                    VkSwapchainKHR* out) {
    VkResult r = g_vkCreateSwapchain(dev, ci, a, out);
    if (r != VK_SUCCESS || !ci || !out || g_dedicated) return r;
    EnterCriticalSection(&g_vkLock);
    vk_destroy_sc();
    if (!vk_init_sc(dev, *out, ci)) { LOG(.str("vulkan: drawing not available")); vk_destroy_sc(); }
    LeaveCriticalSection(&g_vkLock);
    return r;
}
static void VKAPI_CALL vk_destroy_swapchain_hook(VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks* a) {
    EnterCriticalSection(&g_vkLock);
    if (sc && sc == g_vs.sc) vk_destroy_sc();
    LeaveCriticalSection(&g_vkLock);
    g_vkDestroySwapchain(dev, sc, a);
}
static void VKAPI_CALL vk_get_queue_hook(VkDevice dev, uint32_t fam, uint32_t idx, VkQueue* q) {
    g_vkGetQueue(dev, fam, idx, q);
    if (q && *q) vk_note_queue(*q, fam);
}
static void VKAPI_CALL vk_get_queue2_hook(VkDevice dev, const VkDeviceQueueInfo2* qi, VkQueue* q) {
    g_vkGetQueue2(dev, qi, q);
    if (qi && q && *q) vk_note_queue(*q, qi->queueFamilyIndex);
}


static VkResult VKAPI_CALL h_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    VkResult r = g_oCreateDevice(pd, ci, a, out);
    if (r == VK_SUCCESS) g_vkPhys = pd;
    return r;
}
static PFN_vkVoidFunction VKAPI_CALL vk_gipa_hook(VkInstance inst, const char* name);
static PFN_vkVoidFunction VKAPI_CALL vk_gdpa_hook(VkDevice dev, const char* name);
static PFN_vkVoidFunction vk_wrap(const char* name, PFN_vkVoidFunction r) {
    if (!r || !name) return r;
    if (!lstrcmpA(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)&vk_gipa_hook;
    if (!lstrcmpA(name, "vkGetDeviceProcAddr")) { g_vkGDPA = (PFN_vkGetDeviceProcAddr)r; return (PFN_vkVoidFunction)&vk_gdpa_hook; }
    if (!lstrcmpA(name, "vkQueuePresentKHR")) { g_vkPresent = (PFN_vkQueuePresentKHR)r; return (PFN_vkVoidFunction)&vk_present_hook; }
    if (!lstrcmpA(name, "vkCreateSwapchainKHR")) { g_vkCreateSwapchain = (PFN_vkCreateSwapchainKHR)r; return (PFN_vkVoidFunction)&vk_create_swapchain_hook; }
    if (!lstrcmpA(name, "vkDestroySwapchainKHR")) { g_vkDestroySwapchain = (PFN_vkDestroySwapchainKHR)r; return (PFN_vkVoidFunction)&vk_destroy_swapchain_hook; }
    if (!lstrcmpA(name, "vkGetDeviceQueue")) { g_vkGetQueue = (PFN_vkGetDeviceQueue)r; return (PFN_vkVoidFunction)&vk_get_queue_hook; }
#define W(fn, orig, hk) if (!lstrcmpA(name, fn)) { orig = (decltype(orig))r; return (PFN_vkVoidFunction)&hk; }
    W("vkCreateImage", g_oCreateImage, h_CreateImage) W("vkCreateImageView", g_oCreateView, h_CreateView)
    W("vkCmdBeginRendering", g_oBeginR, h_BeginR) W("vkCmdBeginRenderingKHR", g_oBeginRK, h_BeginRK)
    W("vkCmdPipelineBarrier", g_oBarrier, h_Barrier) W("vkCmdPipelineBarrier2", g_oBarrier2, h_Barrier2)
    W("vkCmdPipelineBarrier2KHR", g_oBarrier2K, h_Barrier2K) W("vkCreateDevice", g_oCreateDevice, h_CreateDevice)
#undef W
    if (!lstrcmpA(name, "vkGetDeviceQueue2")) { g_vkGetQueue2 = (PFN_vkGetDeviceQueue2)r; return (PFN_vkVoidFunction)&vk_get_queue2_hook; }
    return r;
}
static PFN_vkVoidFunction VKAPI_CALL vk_gipa_hook(VkInstance inst, const char* name) {
    if (inst) g_vkInst = inst;
    return vk_wrap(name, g_vkGIPA(inst, name));
}
static PFN_vkVoidFunction VKAPI_CALL vk_gdpa_hook(VkDevice dev, const char* name) {
    return vk_wrap(name, g_vkGDPA ? g_vkGDPA(dev, name) : nullptr);
}

static FARPROC WINAPI gpa_hook(HMODULE m, LPCSTR name) {
    FARPROC r = g_realGPA(m, name);
    if (r && ((uintptr_t)name >> 16) && !lstrcmpA(name, "vkGetInstanceProcAddr")) {
        if ((void*)r != (void*)&vk_gipa_hook) g_vkGIPA = (PFN_vkGetInstanceProcAddr)r;
        if (InterlockedExchange(&g_vkSeen, 1) == 0) LOG(.str("vulkan: the game loads Vulkan, drawing hooks in"));
        return (FARPROC)&vk_gipa_hook;
    }
    return r;
}

// Patches the game's import of GetProcAddress (kernel32). Called from DllMain, before
// the game sets up its renderer.
static void install_vk_hooks() {
    InitializeCriticalSection(&g_vkLock);
    InitializeCriticalSection(&g_imgLock);
    auto* dos = (IMAGE_DOS_HEADER*)g_base;
    auto* nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(g_base + dir.VirtualAddress); imp->Name; imp++) {
        if (lstrcmpiA((const char*)(g_base + imp->Name), "KERNEL32.dll")) continue;
        auto* names = (IMAGE_THUNK_DATA64*)(g_base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* funcs = (IMAGE_THUNK_DATA64*)(g_base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, funcs++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)(g_base + names->u1.AddressOfData);
            if (lstrcmpA((const char*)ibn->Name, "GetProcAddress")) continue;
            DWORD old;
            if (!VirtualProtect(&funcs->u1.Function, 8, PAGE_READWRITE, &old)) return;
            g_realGPA = (FARPROC (WINAPI*)(HMODULE, LPCSTR))funcs->u1.Function;
            funcs->u1.Function = (ULONGLONG)&gpa_hook;
            VirtualProtect(&funcs->u1.Function, 8, old, &old);
            return;
        }
    }
}

static void vk_diag() {
    static DWORD next = 0;
    if (!g_cfg.log || GetTickCount() < next) return;
    next = GetTickCount() + 10000;
    static LONG lastDraws = -1;
    LONG d = g_vkDraws;
    if (d == lastDraws) return;
    lastDraws = d;
    LOG(.str("vulkan: presents ").i(g_vkPresents).str(", box drawn in ").i(d).str(" frames"));
}

static size_t readable_len(const void* p, size_t want) {
    MEMORY_BASIC_INFORMATION mi;
    if ((uintptr_t)p < 0x10000 || (uintptr_t)p > 0x7fffffffffffull || !VirtualQuery(p, &mi, sizeof(mi))) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) return 0;
    size_t left = (uint8_t*)mi.BaseAddress + mi.RegionSize - (uint8_t*)p;
    return left < want ? left : want;
}
