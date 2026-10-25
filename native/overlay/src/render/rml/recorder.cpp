#include "render/rml/recorder.h"

#include <algorithm>
#include <utility>

#include "render/gpu_commands.h"
#include "render/rml/targets.h"
#include "render/vk_functions.h"

namespace recorder {

namespace {

enum class Open { None, Layer, Scratch };

struct State {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkExtent2D extent = {0, 0};
    Open open = Open::None;
    int openIndex = -1;
    int scratch[targets::kPostCount] = {0, 1, 2};
};
State g_state;

VkRect2D to_rect(Rml::Rectanglei region) {
    int w = (int)g_state.extent.width, h = (int)g_state.extent.height;
    if (!region.Valid()) return {{0, 0}, g_state.extent};
    int x0 = std::clamp(region.Left(), 0, w), y0 = std::clamp(region.Top(), 0, h);
    int x1 = std::clamp(region.Right(), x0, w), y1 = std::clamp(region.Bottom(), y0, h);
    return {{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}

VkOffset3D corner(int32_t x, int32_t y) { return {x, y, 0}; }

const VkImageSubresourceLayers kColour = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};

bool begin_pass(Open kind, int index, VkRenderPass pass, const DrawTarget* target) {
    if (g_state.open == kind && g_state.openIndex == index) return true;
    close();
    if (!target) return false;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = pass;
    begin.framebuffer = target->framebuffer;
    begin.renderArea.extent = g_state.extent;
    vk.CmdBeginRenderPass(g_state.cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    g_state.open = kind;
    g_state.openIndex = index;
    return true;
}

}  // namespace

void begin(VkCommandBuffer cmd, VkExtent2D extent) {
    g_state = State();
    g_state.cmd = cmd;
    g_state.extent = extent;
}

void end() { close(); }

VkCommandBuffer cmd() { return g_state.cmd; }
VkExtent2D extent() { return g_state.extent; }

bool open_layer(int layer) {
    return begin_pass(Open::Layer, layer, targets::layer_pass(), targets::layer(layer));
}

bool open_scratch(Scratch scratch) {
    int index = scratch_index(scratch);
    return begin_pass(Open::Scratch, index, targets::post_pass(), targets::post(index));
}

bool open_mask() {
    return begin_pass(Open::Scratch, targets::kMask, targets::post_pass(),
                      targets::post(targets::kMask));
}

void close() {
    if (g_state.open == Open::None) return;
    vk.CmdEndRenderPass(g_state.cmd);
    gpu_commands::barrier(g_state.cmd);
    g_state.open = Open::None;
    g_state.openIndex = -1;
}

int scratch_index(Scratch scratch) { return g_state.scratch[(int)scratch]; }

void swap_primary() { std::swap(g_state.scratch[0], g_state.scratch[1]); }

void set_scissor(Rml::Rectanglei region) {
    VkRect2D rect = to_rect(region);
    vk.CmdSetScissor(g_state.cmd, 0, 1, &rect);
}

void set_viewport(VkExtent2D size) {
    VkViewport viewport{0.0f, 0.0f, (float)size.width, (float)size.height, 0.0f, 1.0f};
    vk.CmdSetViewport(g_state.cmd, 0, 1, &viewport);
}

void set_stencil_reference(uint32_t reference) {
    vk.CmdSetStencilReference(g_state.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, reference);
}

void clear_colour(Rml::Rectanglei region) {
    VkClearAttachment clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    VkClearRect rect{to_rect(region), 0, 1};
    if (rect.rect.extent.width && rect.rect.extent.height)
        vk.CmdClearAttachments(g_state.cmd, 1, &clear, 1, &rect);
}

void clear_stencil(uint32_t value) {
    VkClearAttachment clear{VK_IMAGE_ASPECT_STENCIL_BIT, 0, {}};
    clear.clearValue.depthStencil = {0.0f, value};
    VkClearRect rect{{{0, 0}, g_state.extent}, 0, 1};
    vk.CmdClearAttachments(g_state.cmd, 1, &clear, 1, &rect);
}

void bind(Program program, bool clipped) {
    vk.CmdBindPipeline(g_state.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                       pipelines::get(program, clipped));
}

void bind_set(uint32_t index, VkDescriptorSet set) {
    vk.CmdBindDescriptorSets(g_state.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines::layout(),
                             index, 1, &set, 0, nullptr);
}

void bind_uniform(VkDescriptorSet set, uint32_t offset) {
    vk.CmdBindDescriptorSets(g_state.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines::layout(),
                             pipelines::kUniformSet, 1, &set, 1, &offset);
}

void push(const void* data, uint32_t bytes, uint32_t offset) {
    vk.CmdPushConstants(g_state.cmd, pipelines::layout(),
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, offset, bytes,
                        data);
}

void draw_fullscreen() { vk.CmdDraw(g_state.cmd, 3, 1, 0, 0); }

void draw_indexed(VkBuffer buffer, VkDeviceSize vertices, VkDeviceSize indices, uint32_t count) {
    vk.CmdBindVertexBuffers(g_state.cmd, 0, 1, &buffer, &vertices);
    vk.CmdBindIndexBuffer(g_state.cmd, buffer, indices, VK_INDEX_TYPE_UINT32);
    vk.CmdDrawIndexed(g_state.cmd, count, 1, 0, 0, 0);
}

void resolve_layer(int layer, Scratch to, Rml::Rectanglei region) {
    close();
    const DrawTarget* source = targets::layer(layer);
    const DrawTarget* target = targets::post(scratch_index(to));
    VkRect2D rect = to_rect(region);
    if (!source || !target || !rect.extent.width || !rect.extent.height) return;
    VkOffset3D at = corner(rect.offset.x, rect.offset.y);
    VkExtent3D size = {rect.extent.width, rect.extent.height, 1};
    if (targets::samples() == VK_SAMPLE_COUNT_1_BIT) {
        VkImageCopy copy{kColour, at, kColour, at, size};
        vk.CmdCopyImage(g_state.cmd, source->colour.image, VK_IMAGE_LAYOUT_GENERAL,
                        target->colour.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    } else {
        VkImageResolve resolve{kColour, at, kColour, at, size};
        vk.CmdResolveImage(g_state.cmd, source->colour.image, VK_IMAGE_LAYOUT_GENERAL,
                           target->colour.image, VK_IMAGE_LAYOUT_GENERAL, 1, &resolve);
    }
    gpu_commands::barrier(g_state.cmd);
}

void copy_to_origin(Scratch from, Rml::Rectanglei region, VkImage to) {
    close();
    const DrawTarget* source = targets::post(scratch_index(from));
    VkRect2D rect = to_rect(region);
    if (!source || !rect.extent.width || !rect.extent.height) return;
    VkImageCopy copy{kColour, corner(rect.offset.x, rect.offset.y), kColour, corner(0, 0),
                     {rect.extent.width, rect.extent.height, 1}};
    vk.CmdCopyImage(g_state.cmd, source->colour.image, VK_IMAGE_LAYOUT_GENERAL, to,
                    VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    gpu_commands::barrier(g_state.cmd);
}

void blit(Scratch from, Rml::Rectanglei source, Scratch to, Rml::Rectanglei destination) {
    close();
    const DrawTarget* a = targets::post(scratch_index(from));
    const DrawTarget* b = targets::post(scratch_index(to));
    VkRect2D src = to_rect(source), dst = to_rect(destination);
    if (!a || !b || !src.extent.width || !src.extent.height || !dst.extent.width ||
        !dst.extent.height)
        return;
    VkImageBlit blit{};
    blit.srcSubresource = kColour;
    blit.srcOffsets[0] = corner(src.offset.x, src.offset.y);
    blit.srcOffsets[1] = {src.offset.x + (int32_t)src.extent.width,
                          src.offset.y + (int32_t)src.extent.height, 1};
    blit.dstSubresource = kColour;
    blit.dstOffsets[0] = corner(dst.offset.x, dst.offset.y);
    blit.dstOffsets[1] = {dst.offset.x + (int32_t)dst.extent.width,
                          dst.offset.y + (int32_t)dst.extent.height, 1};
    vk.CmdBlitImage(g_state.cmd, a->colour.image, VK_IMAGE_LAYOUT_GENERAL, b->colour.image,
                    VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
    gpu_commands::barrier(g_state.cmd);
}

}  // namespace recorder
