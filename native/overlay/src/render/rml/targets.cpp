#include "render/rml/targets.h"

#include "log.h"
#include "render/gpu_commands.h"
#include "render/gpu_context.h"
#include "render/rml/textures.h"
#include "render/vk_functions.h"

namespace targets {

namespace {

// Two samples smooth the edges of rounded boxes, as RmlUi's own renderers do, at half the
// memory of four: a 4K layer is 66 MB as it is.
constexpr VkSampleCountFlagBits kWantedSamples = VK_SAMPLE_COUNT_2_BIT;

const VkFormat kStencilFormats[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                    VK_FORMAT_D16_UNORM_S8_UINT, VK_FORMAT_S8_UINT};

struct Passes {
    VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkRenderPass layer = VK_NULL_HANDLE;
    VkRenderPass post = VK_NULL_HANDLE;
};
Passes g_passes;

struct Targets {
    VkExtent2D extent = {0, 0};
    gpu_memory::Image stencil;
    DrawTarget layers[kMaxLayers];
    DrawTarget posts[kPostCount + 1];
    bool failed = false;   // logged once per size
};
Targets g_targets;

VkImageAspectFlags stencil_aspect() {
    return g_passes.stencilFormat == VK_FORMAT_S8_UINT
               ? VK_IMAGE_ASPECT_STENCIL_BIT
               : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
}

bool choose_formats() {
    for (VkFormat format : kStencilFormats) {
        VkFormatProperties properties{};
        vk.GetPhysicalDeviceFormatProperties(gpu.physicalDevice, format, &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            g_passes.stencilFormat = format;
            break;
        }
    }
    if (g_passes.stencilFormat == VK_FORMAT_UNDEFINED) {
        LOG_ERROR("targets: the card has no stencil format; the UI cannot be drawn.");
        return false;
    }
    VkPhysicalDeviceProperties properties{};
    vk.GetPhysicalDeviceProperties(gpu.physicalDevice, &properties);
    VkSampleCountFlags supported = properties.limits.framebufferColorSampleCounts &
                                   properties.limits.framebufferStencilSampleCounts;
    g_passes.samples = (supported & kWantedSamples) ? kWantedSamples : VK_SAMPLE_COUNT_1_BIT;
    LOG("targets: stencil format %d, %d samples.", (int)g_passes.stencilFormat,
        (int)g_passes.samples);
    return true;
}

VkAttachmentDescription kept(VkFormat format, VkSampleCountFlagBits samples) {
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = samples;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    return attachment;
}

VkRenderPass create_pass(bool withStencil, VkSampleCountFlagBits samples) {
    VkAttachmentDescription attachments[2] = {kept(textures::kFormat, samples),
                                              kept(g_passes.stencilFormat, samples)};
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkAttachmentReference colour{0, VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference stencil{1, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colour;
    subpass.pDepthStencilAttachment = withStencil ? &stencil : nullptr;

    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = withStencil ? 2 : 1;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (vk.CreateRenderPass(gpu.device, &info, nullptr, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pass;
}

VkFramebuffer create_framebuffer(VkRenderPass pass, VkImageView colour, VkImageView stencil) {
    VkImageView views[2] = {colour, stencil};
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = pass;
    info.attachmentCount = stencil ? 2 : 1;
    info.pAttachments = views;
    info.width = g_targets.extent.width;
    info.height = g_targets.extent.height;
    info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (vk.CreateFramebuffer(gpu.device, &info, nullptr, &framebuffer) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return framebuffer;
}

void destroy_target(DrawTarget& target) {
    if (target.framebuffer) vk.DestroyFramebuffer(gpu.device, target.framebuffer, nullptr);
    textures::forget(target.set);
    gpu_memory::destroy(target.colour);
    target = DrawTarget();
}

bool make_ready(VkImage image, VkImageAspectFlags aspect) {
    return gpu_commands::run_now([&](VkCommandBuffer cmd) {
        gpu_commands::to_general(cmd, image, aspect);
    });
}

bool ensure_stencil() {
    if (g_targets.stencil.image) return true;
    gpu_memory::ImageDesc desc{g_targets.extent, g_passes.stencilFormat,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, stencil_aspect(),
                               g_passes.samples};
    return gpu_memory::create_image(desc, g_targets.stencil) &&
           make_ready(g_targets.stencil.image, stencil_aspect());
}

bool build_layer(DrawTarget& target) {
    gpu_memory::ImageDesc desc{g_targets.extent, textures::kFormat,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, g_passes.samples};
    if (!ensure_stencil() || !gpu_memory::create_image(desc, target.colour)) return false;
    target.framebuffer =
        create_framebuffer(g_passes.layer, target.colour.view, g_targets.stencil.view);
    return target.framebuffer && make_ready(target.colour.image, VK_IMAGE_ASPECT_COLOR_BIT);
}

bool build_post(DrawTarget& target) {
    gpu_memory::ImageDesc desc{g_targets.extent, textures::kFormat,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    if (!gpu_memory::create_image(desc, target.colour)) return false;
    target.framebuffer = create_framebuffer(g_passes.post, target.colour.view, VK_NULL_HANDLE);
    target.set = textures::describe(target.colour.view, textures::Sampling::Clamp);
    return target.framebuffer && target.set &&
           make_ready(target.colour.image, VK_IMAGE_ASPECT_COLOR_BIT);
}

// A failure is not retried until the size changes: it would stall every frame for nothing.
const DrawTarget* ensure(DrawTarget& target, bool (*build)(DrawTarget&)) {
    if (target.framebuffer) return &target;
    if (g_targets.failed || g_targets.extent.width == 0) return nullptr;
    if (build(target)) return &target;
    destroy_target(target);
    LOG_ERROR("targets: could not make a %ux%u target.", g_targets.extent.width,
              g_targets.extent.height);
    g_targets.failed = true;
    return nullptr;
}

}  // namespace

bool create_passes() {
    if (!choose_formats()) return false;
    g_passes.layer = create_pass(true, g_passes.samples);
    g_passes.post = create_pass(false, VK_SAMPLE_COUNT_1_BIT);
    if (!g_passes.layer || !g_passes.post) {
        LOG_ERROR("targets: could not create the render passes.");
        return false;
    }
    return true;
}

void destroy_passes() {
    if (g_passes.layer) vk.DestroyRenderPass(gpu.device, g_passes.layer, nullptr);
    if (g_passes.post) vk.DestroyRenderPass(gpu.device, g_passes.post, nullptr);
    g_passes = Passes();
}

VkRenderPass layer_pass() { return g_passes.layer; }
VkRenderPass post_pass() { return g_passes.post; }
VkSampleCountFlagBits samples() { return g_passes.samples; }

void resize(VkExtent2D extent) {
    if (extent.width == g_targets.extent.width && extent.height == g_targets.extent.height) return;
    destroy();
    g_targets.extent = extent;
}

void destroy() {
    for (DrawTarget& target : g_targets.layers) destroy_target(target);
    for (DrawTarget& target : g_targets.posts) destroy_target(target);
    gpu_memory::destroy(g_targets.stencil);
    g_targets = Targets();
}

VkExtent2D extent() { return g_targets.extent; }

const DrawTarget* layer(int index) {
    if (index < 0 || index >= kMaxLayers) return nullptr;
    return ensure(g_targets.layers[index], build_layer);
}

const DrawTarget* post(int index) {
    if (index < 0 || index > kMask) return nullptr;
    return ensure(g_targets.posts[index], build_post);
}

}  // namespace targets
