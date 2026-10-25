/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Frame generation in libvulkan.prx (PW_FRAMEGEN=2..4, from a profile's
 * [display] framegen). Every frame a program presents is copied aside, and
 * MihawkGen (third_party/mihawkgen) makes the pictures between it and the
 * one before; they are drawn into extra images of the program's swapchain
 * and presented first, so the screen gets FACTOR pictures for each frame
 * the program draws. FIFO presentation, forced on the swapchain, paces
 * them: a 30 fps program fills a 60 Hz screen at FACTOR 2. Nothing changes for the program or Wine; a
 * swapchain the generator cannot serve (memory, format, the network) is
 * left as it was. Once a second it logs the program's frames and the
 * pictures presented; with PW_FRAMEGEN_HUD=1 (show_fps) it also draws them
 * in the top left of every picture ("30 -> 60"), after generation, so the
 * counter is never blended.
 *
 * GPL-3.0-or-later because it links MihawkGen; libvulkan.prx, the only
 * module it goes into, is distributed under the GPL as a whole
 * (THIRD_PARTY.md). Linked by tools/link_radv_prx.sh. */
#include "pw_framegen_vk.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <new>
#include "framegen.h"
#include "pw_framegen.h"

extern "C" PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
extern "C" PFN_vkVoidFunction VKAPI_CALL vk_common_GetDeviceProcAddr(VkDevice device, const char *name);
/* The trained network, embedded by tools/link_radv_prx.sh (pw_framegen_network.S). */
extern "C" const unsigned char pw_framegen_network[], pw_framegen_network_end[];

#define PW_FRAMEGEN_VK_DEFINE(name) PFN_vk##name pw_fgvk_##name;
extern "C" {
PW_FRAMEGEN_VK_DEVICE(PW_FRAMEGEN_VK_DEFINE)
PW_FRAMEGEN_VK_INSTANCE(PW_FRAMEGEN_VK_DEFINE)
}
#undef PW_FRAMEGEN_VK_DEFINE

namespace {

constexpr uint32_t kSlots = 2;      /* frames in flight */
constexpr uint32_t kMaxFactor = 4;
constexpr uint32_t kMaxImages = 16;
constexpr uint32_t kMaxChains = 8;

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

/* The counter's font: 5x7 glyphs in 6x9 cells, white on black, as one row
 * of an RGBA8 atlas blitted (scaled, nearest) onto the pictures. */
constexpr uint32_t kCellW = 6, kCellH = 9;
constexpr int kGlyphArrow = 10, kGlyphSpace = 11, kGlyphs = 12;
constexpr uint8_t kFont[kGlyphs][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x00, 0x04, 0x02, 0x1F, 0x02, 0x04, 0x00}, {0},
};
constexpr uint32_t kHudChars = 12;

struct Chain {
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkExtent2D extent{};
    uint32_t factor = 0;
    VkImage images[kMaxImages]{};
    uint32_t image_count = 0;
    fg_context *fg = nullptr;
    fg_method method = FG_METHOD_BLEND;
    Image history[2], output[kMaxFactor - 1];
    uint32_t cur = 0;
    bool have_prev = false;
    VkCommandPool pool = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    VkCommandBuffer cmd[kSlots]{};
    VkFence fence[kSlots]{};
    VkSemaphore real_done[kSlots]{}, acquired[kSlots][kMaxFactor - 1]{}, generated[kSlots][kMaxFactor - 1]{};
    uint32_t slot = 0;
    /* the last second's counts, and the one being counted */
    uint64_t window_start = 0;
    uint32_t real_count = 0, shown_count = 0, real_rate = 0, shown_rate = 0;
    bool measured = false;
    /* the counter's atlas and the buffer it was uploaded from */
    Image font;
    VkBuffer font_buffer = VK_NULL_HANDLE;
    VkDeviceMemory font_memory = VK_NULL_HANDLE;
    bool font_uploaded = false;
};

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
int factor_setting = -1, hud_setting = -1;
VkInstance instance_seen = VK_NULL_HANDLE;
bool device_loaded = false;
struct { VkDevice device; VkPhysicalDevice physical; } devices[8];
struct { VkQueue queue; uint32_t family; } queues[32];
Chain *chains[kMaxChains];

PFN_vkCreateDevice real_CreateDevice;
PFN_vkGetDeviceQueue real_GetDeviceQueue;
PFN_vkGetDeviceQueue2 real_GetDeviceQueue2;
PFN_vkCreateSwapchainKHR real_CreateSwapchainKHR;
PFN_vkDestroySwapchainKHR real_DestroySwapchainKHR;
PFN_vkQueuePresentKHR real_QueuePresentKHR;

uint32_t factor(void)
{
    if (factor_setting < 0) {
        const char *value = getenv("PW_FRAMEGEN");
        long n = value ? strtol(value, nullptr, 10) : 0;
        factor_setting = n >= 2 && n <= (long)kMaxFactor ? (int)n : 0;
        if (factor_setting) fprintf(stderr, "pw_framegen: factor %d\n", factor_setting);
    }
    return (uint32_t)factor_setting;
}

bool hud(void)
{
    if (hud_setting < 0) {
        const char *value = getenv("PW_FRAMEGEN_HUD");
        hud_setting = value && value[0] == '1';
    }
    return hud_setting;
}

uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

bool load_device(VkDevice device)
{
    if (device_loaded) return true;
#define PW_FRAMEGEN_VK_LOAD(name) \
    if (!(pw_fgvk_##name = (PFN_vk##name)vk_common_GetDeviceProcAddr(device, "vk" #name))) return false;
    PW_FRAMEGEN_VK_DEVICE(PW_FRAMEGEN_VK_LOAD)
#undef PW_FRAMEGEN_VK_LOAD
    if (!(pw_fgvk_GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)
              vk_icdGetInstanceProcAddr(instance_seen, "vkGetPhysicalDeviceMemoryProperties")))
        return false;
    return device_loaded = true;
}

VkPhysicalDevice physical_of(VkDevice device)
{
    for (auto &d : devices)
        if (d.device == device) return d.physical;
    return VK_NULL_HANDLE;
}

uint32_t family_of(VkQueue queue)
{
    for (auto &q : queues)
        if (q.queue == queue) return q.family;
    return 0;  /* RADV's first family does graphics, compute and transfer */
}

void remember_queue(VkQueue queue, uint32_t family)
{
    for (auto &q : queues)
        if (!q.queue || q.queue == queue) { q.queue = queue; q.family = family; return; }
}

uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

bool make_image(Chain *c, VkPhysicalDevice physical, VkImageUsageFlags usage, Image *out,
                VkExtent2D extent = {0, 0})
{
    if (!extent.width) extent = c->extent;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    if (vkCreateImage(c->device, &info, nullptr, &out->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c->device, out->image, &req);
    uint32_t type = memory_type(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(c->device, &alloc, nullptr, &out->memory) != VK_SUCCESS ||
        vkBindImageMemory(c->device, out->image, out->memory, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out->image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(c->device, &view, nullptr, &out->view) == VK_SUCCESS;
}

/* The counter's atlas, drawn into a host buffer here and copied into the
 * image by the first present (record_font_upload). */
bool make_font(Chain *c, VkPhysicalDevice physical)
{
    constexpr uint32_t width = kCellW * kGlyphs, height = kCellH;
    if (!make_image(c, physical, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &c->font,
                    {width, height}))
        return false;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = width * height * 4;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(c->device, &info, nullptr, &c->font_buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c->device, c->font_buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(physical, req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void *mapped = nullptr;
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(c->device, &alloc, nullptr, &c->font_memory) != VK_SUCCESS ||
        vkBindBufferMemory(c->device, c->font_buffer, c->font_memory, 0) != VK_SUCCESS ||
        vkMapMemory(c->device, c->font_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        return false;
    uint32_t *pixels = (uint32_t *)mapped;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            uint32_t glyph = x / kCellW, gx = x % kCellW, gy = y;
            bool on = gx >= 1 && gy >= 1 && gy <= 7 && (kFont[glyph][gy - 1] >> (5 - gx) & 1);
            pixels[y * width + x] = on ? 0xFFFFFFFFu : 0xFF000000u;  /* ABGR in memory: white, black */
        }
    vkUnmapMemory(c->device, c->font_memory);
    return true;
}

void free_image(VkDevice device, Image *image)
{
    if (image->view) vkDestroyImageView(device, image->view, nullptr);
    if (image->image) vkDestroyImage(device, image->image, nullptr);
    if (image->memory) vkFreeMemory(device, image->memory, nullptr);
    *image = Image{};
}

void free_chain(Chain *c)
{
    if (!c) return;
    if (device_loaded) {
        vkDeviceWaitIdle(c->device);
        if (c->fg) fg_context_destroy(c->fg);
        free_image(c->device, &c->history[0]);
        free_image(c->device, &c->history[1]);
        for (auto &image : c->output) free_image(c->device, &image);
        free_image(c->device, &c->font);
        if (c->font_buffer) vkDestroyBuffer(c->device, c->font_buffer, nullptr);
        if (c->font_memory) vkFreeMemory(c->device, c->font_memory, nullptr);
        for (uint32_t s = 0; s < kSlots; s++) {
            if (c->fence[s]) vkDestroyFence(c->device, c->fence[s], nullptr);
            if (c->real_done[s]) vkDestroySemaphore(c->device, c->real_done[s], nullptr);
            for (uint32_t k = 0; k < kMaxFactor - 1; k++) {
                if (c->acquired[s][k]) vkDestroySemaphore(c->device, c->acquired[s][k], nullptr);
                if (c->generated[s][k]) vkDestroySemaphore(c->device, c->generated[s][k], nullptr);
            }
        }
        if (c->pool) vkDestroyCommandPool(c->device, c->pool, nullptr);
    }
    delete c;
}

/* The generator's images, network and synchronisation for one swapchain. */
bool setup_chain(Chain *c)
{
    VkPhysicalDevice physical = physical_of(c->device);
    if (!physical || !load_device(c->device)) return false;
    uint32_t count = kMaxImages;
    if (vkGetSwapchainImagesKHR(c->device, c->handle, &count, c->images) < 0) return false;
    c->image_count = count;
    fg_create_info info{};
    info.device = c->device;
    info.physical_device = physical;
    info.width = c->extent.width;
    info.height = c->extent.height;
    info.slots = kSlots * (c->factor - 1);
    info.output_format = VK_FORMAT_R8G8B8A8_UNORM;
    char error[256];
    if (fg_context_create(&info, &c->fg, error, sizeof(error)) != VK_SUCCESS) {
        fprintf(stderr, "pw_framegen: no generator: %s\n", error);
        return false;
    }
    if (fg_context_load_network(c->fg, pw_framegen_network, (size_t)(pw_framegen_network_end - pw_framegen_network),
                                error, sizeof(error)) == VK_SUCCESS)
        c->method = FG_METHOD_NEURAL;
    else
        fprintf(stderr, "pw_framegen: no network (%s): blending instead\n", error);
    if (!make_image(c, physical, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &c->history[0]) ||
        !make_image(c, physical, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &c->history[1]))
        return false;
    for (uint32_t k = 0; k < c->factor - 1; k++)
        if (!make_image(c, physical, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &c->output[k]))
            return false;
    if (hud() && !make_font(c, physical)) {
        fprintf(stderr, "pw_framegen: no counter font: counting in the log only\n");
        free_image(c->device, &c->font);
    }
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t s = 0; s < kSlots; s++) {
        if (vkCreateFence(c->device, &fence, nullptr, &c->fence[s]) != VK_SUCCESS ||
            vkCreateSemaphore(c->device, &semaphore, nullptr, &c->real_done[s]) != VK_SUCCESS)
            return false;
        for (uint32_t k = 0; k < c->factor - 1; k++)
            if (vkCreateSemaphore(c->device, &semaphore, nullptr, &c->acquired[s][k]) != VK_SUCCESS ||
                vkCreateSemaphore(c->device, &semaphore, nullptr, &c->generated[s][k]) != VK_SUCCESS)
                return false;
    }
    fprintf(stderr, "pw_framegen: swapchain %ux%u, %u images, factor %u, %s%s\n", c->extent.width,
            c->extent.height, c->image_count, c->factor, c->method == FG_METHOD_NEURAL ? "neural" : "blend",
            c->font.view ? ", counter on screen" : "");
    return true;
}

/* The command pool and buffers, on the presenting queue's family. */
bool setup_commands(Chain *c, uint32_t family)
{
    if (c->pool && c->family == family) return true;
    if (c->pool) {
        vkDeviceWaitIdle(c->device);
        vkDestroyCommandPool(c->device, c->pool, nullptr);
        c->pool = VK_NULL_HANDLE;
    }
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = family;
    if (vkCreateCommandPool(c->device, &pool, nullptr, &c->pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = c->pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = kSlots;
    c->family = family;
    return vkAllocateCommandBuffers(c->device, &alloc, c->cmd) == VK_SUCCESS;
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
             VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void blit(VkCommandBuffer cmd, VkImage from, VkImage to, VkExtent2D extent)
{
    VkImageBlit region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = region.dstOffsets[1] = {(int32_t)extent.width, (int32_t)extent.height, 1};
    vkCmdBlitImage(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &region, VK_FILTER_NEAREST);
}

void record_font_upload(Chain *c, VkCommandBuffer cmd)
{
    barrier(cmd, c->font.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kCellW * kGlyphs, kCellH, 1};
    vkCmdCopyBufferToImage(cmd, c->font_buffer, c->font.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cmd, c->font.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    c->font_uploaded = true;
}

/* The counter, " 30>60 " in the font's cells, onto an image in
 * TRANSFER_DST layout; nothing before the first second is measured. */
void record_counter(Chain *c, VkCommandBuffer cmd, VkImage target)
{
    if (!c->font_uploaded || !c->measured) return;
    int glyphs[kHudChars], n = 0;
    char text[kHudChars + 1];
    snprintf(text, sizeof(text), " %u>%u ", c->real_rate, c->shown_rate);
    for (const char *p = text; *p && n < (int)kHudChars; p++)
        glyphs[n++] = *p == '>' ? kGlyphArrow : *p >= '0' && *p <= '9' ? *p - '0' : kGlyphSpace;
    uint32_t scale = c->extent.height / 270 > 2 ? c->extent.height / 270 : 2, margin = 2 * scale;
    if (margin + n * kCellW * scale > c->extent.width || margin + kCellH * scale > c->extent.height) return;
    VkImageBlit regions[kHudChars]{};
    for (int i = 0; i < n; i++) {
        VkImageBlit &r = regions[i];
        r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.srcOffsets[0] = {(int32_t)(glyphs[i] * kCellW), 0, 0};
        r.srcOffsets[1] = {(int32_t)((glyphs[i] + 1) * kCellW), (int32_t)kCellH, 1};
        r.dstOffsets[0] = {(int32_t)(margin + i * kCellW * scale), (int32_t)margin, 0};
        r.dstOffsets[1] = {(int32_t)(margin + (i + 1) * kCellW * scale), (int32_t)(margin + kCellH * scale), 1};
    }
    vkCmdBlitImage(cmd, c->font.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)n, regions, VK_FILTER_NEAREST);
}

/* Count a program frame and its pictures; once a second, the rates. */
void count_frame(Chain *c, uint32_t generated)
{
    uint64_t now = now_ns();
    if (!c->window_start) c->window_start = now;
    c->real_count++;
    c->shown_count += 1 + generated;
    uint64_t elapsed = now - c->window_start;
    if (elapsed < 1000000000u) return;
    c->real_rate = (uint32_t)((c->real_count * 1000000000ull + elapsed / 2) / elapsed);
    c->shown_rate = (uint32_t)((c->shown_count * 1000000000ull + elapsed / 2) / elapsed);
    c->measured = true;
    fprintf(stderr, "pw_framegen: %u frames/s from the program, %u pictures/s presented (%u generated each)\n",
            c->real_rate, c->shown_rate, generated);
    c->window_start = now;
    c->real_count = c->shown_count = 0;
}

Chain *find_chain(VkSwapchainKHR handle)
{
    for (auto *c : chains)
        if (c && c->handle == handle) return c;
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL fg_CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                               const VkAllocationCallbacks *allocator, VkDevice *device)
{
    VkResult result = real_CreateDevice(physical, info, allocator, device);
    if (result == VK_SUCCESS) {
        pthread_mutex_lock(&lock);
        for (auto &d : devices)
            if (!d.device) { d.device = *device; d.physical = physical; break; }
        pthread_mutex_unlock(&lock);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL fg_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue)
{
    real_GetDeviceQueue(device, family, index, queue);
    pthread_mutex_lock(&lock);
    remember_queue(*queue, family);
    pthread_mutex_unlock(&lock);
}

VKAPI_ATTR void VKAPI_CALL fg_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue)
{
    real_GetDeviceQueue2(device, info, queue);
    pthread_mutex_lock(&lock);
    remember_queue(*queue, info->queueFamilyIndex);
    pthread_mutex_unlock(&lock);
}

VKAPI_ATTR VkResult VKAPI_CALL fg_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                                     const VkAllocationCallbacks *allocator, VkSwapchainKHR *handle)
{
    VkSwapchainCreateInfoKHR ours = *info;
    ours.minImageCount += factor() - 1;
    ours.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    /* FIFO paces the pictures; mailbox or immediate would drop or tear them. */
    ours.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkResult result = real_CreateSwapchainKHR(device, &ours, allocator, handle);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "pw_framegen: swapchain refused with extra images (%d): without generation\n", (int)result);
        return real_CreateSwapchainKHR(device, info, allocator, handle);
    }
    Chain *c = new (std::nothrow) Chain;
    if (!c) return result;
    c->device = device;
    c->handle = *handle;
    c->extent = info->imageExtent;
    c->factor = factor();
    pthread_mutex_lock(&lock);
    bool ok = setup_chain(c);
    if (ok) {
        ok = false;
        for (auto *&slot : chains)
            if (!slot) { slot = c; ok = true; break; }
    }
    pthread_mutex_unlock(&lock);
    if (!ok) {
        fprintf(stderr, "pw_framegen: swapchain %ux%u without generation\n", c->extent.width, c->extent.height);
        free_chain(c);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL fg_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR handle,
                                                  const VkAllocationCallbacks *allocator)
{
    pthread_mutex_lock(&lock);
    for (auto *&slot : chains)
        if (slot && slot->handle == handle) { free_chain(slot); slot = nullptr; }
    pthread_mutex_unlock(&lock);
    real_DestroySwapchainKHR(device, handle, allocator);
}

/* One submission for the program's frame: copy it aside, then make and draw
 * each picture between it and the previous one into an acquired image. The
 * pictures are presented first, then the program's frame. */
VKAPI_ATTR VkResult VKAPI_CALL fg_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
    if (info->swapchainCount != 1) return real_QueuePresentKHR(queue, info);
    pthread_mutex_lock(&lock);
    Chain *c = find_chain(info->pSwapchains[0]);
    if (!c || !setup_commands(c, family_of(queue))) {
        pthread_mutex_unlock(&lock);
        return real_QueuePresentKHR(queue, info);
    }
    uint32_t s = c->slot;
    c->slot = (c->slot + 1) % kSlots;
    vkWaitForFences(c->device, 1, &c->fence[s], VK_TRUE, UINT64_MAX);
    vkResetFences(c->device, 1, &c->fence[s]);

    uint32_t generate = c->have_prev ? c->factor - 1 : 0, targets[kMaxFactor - 1];
    for (uint32_t k = 0; k < generate; k++) {
        VkResult acquired = vkAcquireNextImageKHR(c->device, c->handle, UINT64_MAX, c->acquired[s][k],
                                                  VK_NULL_HANDLE, &targets[k]);
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) { generate = k; break; }
    }

    VkCommandBuffer cmd = c->cmd[s];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &begin);
    bool counter = c->font.view != VK_NULL_HANDLE;
    if (counter && !c->font_uploaded) record_font_upload(c, cmd);
    VkImage frame = c->images[info->pImageIndices[0]];
    Image &now = c->history[c->cur], &before = c->history[c->cur ^ 1];
    barrier(cmd, frame, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(cmd, now.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    blit(cmd, frame, now.image, c->extent);
    if (counter) {
        /* the counter onto the program's frame, after its copy aside */
        barrier(cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        record_counter(c, cmd, frame);
        barrier(cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    } else {
        barrier(cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    barrier(cmd, now.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    for (uint32_t k = 0; k < generate; k++) {
        Image &out = c->output[k];
        /* after the previous frame's copy out of it */
        barrier(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        fg_frame_pair pair{};
        pair.prev_color = before.view;
        pair.next_color = now.view;
        pair.t = (float)(k + 1) / (float)c->factor;
        fg_record_generate(c->fg, cmd, s * (c->factor - 1) + k, c->method, &pair, out.view);
        barrier(cmd, out.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImage target = c->images[targets[k]];
        barrier(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        blit(cmd, out.image, target, c->extent);
        if (counter) {
            barrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            record_counter(c, cmd, target);
        }
        barrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    vkEndCommandBuffer(cmd);

    VkSemaphore waits[16 + kMaxFactor], signals[kMaxFactor];
    VkPipelineStageFlags stages[16 + kMaxFactor];
    uint32_t wait_count = 0, signal_count = 0;
    for (uint32_t i = 0; i < info->waitSemaphoreCount && wait_count < 16; i++, wait_count++) {
        waits[wait_count] = info->pWaitSemaphores[i];
        stages[wait_count] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    for (uint32_t k = 0; k < generate; k++, wait_count++) {
        waits[wait_count] = c->acquired[s][k];
        stages[wait_count] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    for (uint32_t k = 0; k < generate; k++) signals[signal_count++] = c->generated[s][k];
    signals[signal_count++] = c->real_done[s];
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = wait_count;
    submit.pWaitSemaphores = waits;
    submit.pWaitDstStageMask = stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = signal_count;
    submit.pSignalSemaphores = signals;
    VkResult result = vkQueueSubmit(queue, 1, &submit, c->fence[s]);
    if (result != VK_SUCCESS) {
        pthread_mutex_unlock(&lock);
        fprintf(stderr, "pw_framegen: submit failed (%d)\n", (int)result);
        return result;
    }
    uint32_t presented = 0;
    for (uint32_t k = 0; k < generate; k++) {
        VkPresentInfoKHR picture{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        picture.waitSemaphoreCount = 1;
        picture.pWaitSemaphores = &c->generated[s][k];
        picture.swapchainCount = 1;
        picture.pSwapchains = &c->handle;
        picture.pImageIndices = &targets[k];
        VkResult shown = real_QueuePresentKHR(queue, &picture);
        presented += shown == VK_SUCCESS || shown == VK_SUBOPTIMAL_KHR;
    }
    VkPresentInfoKHR real = *info;
    real.waitSemaphoreCount = 1;
    real.pWaitSemaphores = &c->real_done[s];
    c->cur ^= 1;
    c->have_prev = true;
    count_frame(c, presented);
    pthread_mutex_unlock(&lock);
    return real_QueuePresentKHR(queue, &real);
}

/* The wrapper for name, remembering the driver's own; NULL when there is none. */
PFN_vkVoidFunction wrap(const char *name, PFN_vkVoidFunction real)
{
#define PW_FRAMEGEN_WRAP(fn) \
    if (!strcmp(name, "vk" #fn)) { \
        if (!real_##fn) real_##fn = (PFN_vk##fn)real; \
        return (PFN_vkVoidFunction)fg_##fn; \
    }
    PW_FRAMEGEN_WRAP(CreateDevice)
    PW_FRAMEGEN_WRAP(GetDeviceQueue)
    PW_FRAMEGEN_WRAP(GetDeviceQueue2)
    PW_FRAMEGEN_WRAP(CreateSwapchainKHR)
    PW_FRAMEGEN_WRAP(DestroySwapchainKHR)
    PW_FRAMEGEN_WRAP(QueuePresentKHR)
#undef PW_FRAMEGEN_WRAP
    return nullptr;
}

}  // namespace

extern "C" PFN_vkVoidFunction pw_framegen_resolve(VkInstance instance, const char *name, PFN_vkVoidFunction real)
{
    if (!name || !real || !factor()) return real;
    if (instance) instance_seen = instance;
    PFN_vkVoidFunction ours = wrap(name, real);
    return ours ? ours : real;
}
