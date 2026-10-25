#include "render/overlay_renderer.h"

#include "diagnostics.h"
#include "game/game_window.h"
#include "input/window_input.h"
#include "log.h"
#include "render/gpu_context.h"
#include "render/gpu_timer.h"
#include "render/rml/rml_renderer.h"
#include "render/surface_query.h"
#include "render/vk_functions.h"
#include "ui/ui_frame.h"

namespace overlay_renderer {

namespace {

constexpr uint32_t kMaxImages = 8;

// Per-image fences and semaphores. Independent of format and size, so created once and kept
// across swapchain recreations.
struct FrameSync {
    VkFence fences[kMaxImages] = {};
    VkSemaphore drawn[kMaxImages] = {};
    bool created = false;

    bool create() {
        if (created) return true;
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // the first wait on each image returns at once
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (uint32_t i = 0; i < kMaxImages; ++i) {
            if (vk.CreateFence(gpu.device, &fence, nullptr, &fences[i]) != VK_SUCCESS ||
                vk.CreateSemaphore(gpu.device, &semaphore, nullptr, &drawn[i]) != VK_SUCCESS) {
                LOG_ERROR("renderer: could not create sync objects for image %u.", i);
                return false;
            }
        }
        return created = true;
    }
};

// Everything that belongs to one swapchain.
struct SwapchainTargets {
    SurfaceInfo surface;   // format is UNDEFINED until queried
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    uint32_t imageCount = 0;
    VkImageView views[kMaxImages] = {};
    VkFramebuffer framebuffers[kMaxImages] = {};
    VkCommandBuffer commands[kMaxImages] = {};

    bool queried() const { return surface.format != VK_FORMAT_UNDEFINED; }
};

SwapchainTargets g_targets;
FrameSync g_sync;
GpuTimer g_timer;
bool g_ready = false;
// Once per device: what is made here outlives every swapchain.
enum class DeviceObjects { NotMade, Made, Failed };
DeviceObjects g_deviceObjects = DeviceObjects::NotMade;
// Known only if the game made its swapchain after our hooks went in; outlives every reset.
VkFormat g_gameFormat = VK_FORMAT_UNDEFINED;

// Loads the game's finished frame, draws over it, leaves it ready to present.
bool create_render_pass() {
    VkAttachmentDescription attachment{};
    attachment.format = g_targets.surface.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference colour{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colour;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    if (vk.CreateRenderPass(gpu.device, &info, nullptr, &g_targets.renderPass) != VK_SUCCESS) {
        LOG_ERROR("renderer: vkCreateRenderPass failed.");
        return false;
    }
    return true;
}

bool create_framebuffers(VkSwapchainKHR swapchain) {
    uint32_t count = 0;
    vk.GetSwapchainImagesKHR(gpu.device, swapchain, &count, nullptr);
    if (count == 0 || count > kMaxImages) {
        LOG_ERROR("renderer: bad swapchain image count %u.", count);
        return false;
    }
    VkImage images[kMaxImages] = {};
    vk.GetSwapchainImagesKHR(gpu.device, swapchain, &count, images);
    g_targets.imageCount = count;
    const VkExtent2D extent = g_targets.surface.extent;
    LOG("renderer: swapchain images: %u, extent %ux%u, format %d", count, extent.width,
        extent.height, (int)g_targets.surface.format);

    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = g_targets.surface.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vk.CreateImageView(gpu.device, &view, nullptr, &g_targets.views[i]) != VK_SUCCESS) {
            LOG_ERROR("renderer: vkCreateImageView[%u] failed.", i);
            return false;
        }
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = g_targets.renderPass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &g_targets.views[i];
        framebuffer.width = extent.width;
        framebuffer.height = extent.height;
        framebuffer.layers = 1;
        if (vk.CreateFramebuffer(gpu.device, &framebuffer, nullptr, &g_targets.framebuffers[i]) !=
            VK_SUCCESS) {
            LOG_ERROR("renderer: vkCreateFramebuffer[%u] failed.", i);
            return false;
        }
    }
    return true;
}

bool create_command_buffers() {
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = gpu.graphicsFamily;
    if (vk.CreateCommandPool(gpu.device, &pool, nullptr, &g_targets.commandPool) != VK_SUCCESS) {
        LOG_ERROR("renderer: vkCreateCommandPool failed.");
        return false;
    }
    VkCommandBufferAllocateInfo buffers{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    buffers.commandPool = g_targets.commandPool;
    buffers.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    buffers.commandBufferCount = g_targets.imageCount;
    if (vk.AllocateCommandBuffers(gpu.device, &buffers, g_targets.commands) != VK_SUCCESS) {
        LOG_ERROR("renderer: vkAllocateCommandBuffers failed.");
        return false;
    }
    return true;
}

bool create_device_objects() {
    if (g_deviceObjects == DeviceObjects::NotMade) {
        window_input::attach(game_window::find());
        g_deviceObjects = rml_renderer::create_device_objects() ? DeviceObjects::Made
                                                                : DeviceObjects::Failed;
    }
    return g_deviceObjects == DeviceObjects::Made;
}

bool build(VkSwapchainKHR swapchain) {
    if (!create_render_pass() || !create_framebuffers(swapchain) || !create_command_buffers() ||
        !g_sync.create() || !create_device_objects() ||
        !rml_renderer::create_frame_objects(g_targets.renderPass))
        return false;
    g_timer.create(kMaxImages);
    LOG("renderer: resources built.");
    return true;
}

bool ensure_ready(VkSwapchainKHR swapchain) {
    if (g_ready) return true;
    if (!g_targets.queried()) {
        if (!query_surface(game_window::find(), g_targets.surface)) return false;
        // Drawing in a format other than the swapchain's swaps red and blue.
        if (g_gameFormat != VK_FORMAT_UNDEFINED) g_targets.surface.format = g_gameFormat;
        LOG("renderer: drawing as format %d (%s).", (int)g_targets.surface.format,
            g_gameFormat != VK_FORMAT_UNDEFINED ? "the game's swapchain" : "guessed from the surface");
    }
    g_ready = build(swapchain);
    if (!g_ready) {
        LOG_ERROR("renderer: build failed; resources will be queried again.");
        reset("resource build failure", VK_ERROR_INITIALIZATION_FAILED);
    }
    return g_ready;
}

// Waits only for this image's previous submit, which also makes its timestamps readable. The
// fence is reset only by the submit that will signal it again: a frame with nothing to draw
// submits nothing, and must leave the fence signalled.
void wait_for_image(uint32_t image) {
    vk.WaitForFences(gpu.device, 1, &g_sync.fences[image], VK_TRUE, UINT64_MAX);
    double ms = 0.0;
    if (g_timer.collect(image, ms)) diagnostics::record_gpu_ms(ms);
}

void record(VkCommandBuffer cmd, uint32_t image) {
    vk.ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.BeginCommandBuffer(cmd, &begin);
    g_timer.begin(cmd, image);
    rml_renderer::begin_frame(cmd, g_targets.surface.extent);
    ui_frame::render();
    bool drawn = rml_renderer::end_frame();

    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = g_targets.renderPass;
    pass.framebuffer = g_targets.framebuffers[image];
    pass.renderArea.extent = g_targets.surface.extent;
    vk.CmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    if (drawn) rml_renderer::present();
    vk.CmdEndRenderPass(cmd);

    g_timer.end(cmd, image);
    vk.EndCommandBuffer(cmd);
}

// Chained on the GPU without a stall: game render -> our draw -> present. Our submit takes over
// the semaphores present was going to wait on and signals one of our own instead.
VkSemaphore submit(VkCommandBuffer cmd, uint32_t image, const VkPresentInfoKHR& present) {
    VkSemaphore waits[8];
    VkPipelineStageFlags stages[8];
    uint32_t waitCount = present.waitSemaphoreCount < 8 ? present.waitSemaphoreCount : 8;
    if (!present.pWaitSemaphores) waitCount = 0;
    for (uint32_t i = 0; i < waitCount; ++i) {
        waits[i] = present.pWaitSemaphores[i];
        stages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    // No semaphore to chain on: never draw over an unfinished frame. Not seen in NMS.
    if (waitCount == 0) vk.QueueWaitIdle(gpu.queue);

    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.waitSemaphoreCount = waitCount;
    info.pWaitSemaphores = waitCount ? waits : nullptr;
    info.pWaitDstStageMask = waitCount ? stages : nullptr;
    info.commandBufferCount = 1;
    info.pCommandBuffers = &cmd;
    info.signalSemaphoreCount = 1;
    info.pSignalSemaphores = &g_sync.drawn[image];
    vk.ResetFences(gpu.device, 1, &g_sync.fences[image]);
    vk.QueueSubmit(gpu.queue, 1, &info, g_sync.fences[image]);
    return g_sync.drawn[image];
}

}  // namespace

void reset(const char* reason, VkResult status) {
    g_ready = false;
    g_timer.forget();

    bool hadResources = g_targets.renderPass || g_targets.commandPool || g_targets.imageCount > 0;
    if (hadResources && gpu.device) {
        VkResult waited = vk.DeviceWaitIdle(gpu.device);
        if (waited != VK_SUCCESS) LOG_ERROR("renderer: vkDeviceWaitIdle failed (result=%d).", (int)waited);
    }
    if (gpu.device) rml_renderer::destroy_frame_objects();
    if (gpu.device) {
        for (uint32_t i = 0; i < kMaxImages; ++i) {
            if (g_targets.framebuffers[i]) vk.DestroyFramebuffer(gpu.device, g_targets.framebuffers[i], nullptr);
            if (g_targets.views[i]) vk.DestroyImageView(gpu.device, g_targets.views[i], nullptr);
        }
        if (g_targets.commandPool) vk.DestroyCommandPool(gpu.device, g_targets.commandPool, nullptr);
        if (g_targets.renderPass) vk.DestroyRenderPass(gpu.device, g_targets.renderPass, nullptr);
    }
    g_targets = SwapchainTargets();

    LOG("renderer: swapchain resources reset (reason=%s result=%d hadResources=%d).",
        reason ? reason : "unknown", (int)status, (int)hadResources);
}

void use_game_format(VkFormat format) { g_gameFormat = format; }

VkSemaphore draw(VkSwapchainKHR swapchain, const VkPresentInfoKHR& present) {
    if (!gpu.device || !swapchain || !ensure_ready(swapchain)) return VK_NULL_HANDLE;
    uint32_t image = present.pImageIndices ? present.pImageIndices[0] : 0;
    if (image >= g_targets.imageCount) return VK_NULL_HANDLE;

    wait_for_image(image);
    rml_renderer::after_fence(image);
    if (!ui_frame::update(g_targets.surface.extent)) {
        rml_renderer::frame_done(false);
        return VK_NULL_HANDLE;
    }
    VkCommandBuffer cmd = g_targets.commands[image];
    record(cmd, image);
    VkSemaphore drawn = submit(cmd, image, present);
    rml_renderer::frame_done(true);
    return drawn;
}

}  // namespace overlay_renderer
