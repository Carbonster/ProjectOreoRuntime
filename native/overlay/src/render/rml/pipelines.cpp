#include "render/rml/pipelines.h"

#include <RmlUi/Core/Vertex.h>

#include "log.h"
#include "render/gpu_context.h"
#include "render/rml/shader_binaries.h"
#include "render/rml/targets.h"
#include "render/vk_functions.h"

namespace pipelines {

namespace {

using namespace shader_binaries;

enum class Pass { Layer, Post, Frame };
enum class Blend { Premultiplied, None };
enum class Stencil { Test, Set, Increment, None };

struct Shader {
    const uint32_t* code;
    size_t bytes;
};
#define SHADER(name) Shader{name, sizeof(name)}

struct Spec {
    Program program;
    Shader vertex, fragment;
    Pass pass;
    Blend blend;
    Stencil stencil;   // Test: a clipped twin is built as well
};

const Spec kSpecs[] = {
    {Program::Colour, SHADER(kGeometryVert), SHADER(kColourFrag), Pass::Layer, Blend::Premultiplied, Stencil::Test},
    {Program::Texture, SHADER(kGeometryVert), SHADER(kTextureFrag), Pass::Layer, Blend::Premultiplied, Stencil::Test},
    {Program::Gradient, SHADER(kGeometryVert), SHADER(kGradientFrag), Pass::Layer, Blend::Premultiplied, Stencil::Test},
    {Program::Composite, SHADER(kFullscreenVert), SHADER(kCopyFrag), Pass::Layer, Blend::Premultiplied, Stencil::Test},
    {Program::CompositeReplace, SHADER(kFullscreenVert), SHADER(kCopyFrag), Pass::Layer, Blend::None, Stencil::Test},
    {Program::ClipSet, SHADER(kGeometryVert), SHADER(kColourFrag), Pass::Layer, Blend::None, Stencil::Set},
    {Program::ClipIntersect, SHADER(kGeometryVert), SHADER(kColourFrag), Pass::Layer, Blend::None, Stencil::Increment},
    {Program::Copy, SHADER(kFullscreenVert), SHADER(kCopyFrag), Pass::Post, Blend::None, Stencil::None},
    {Program::CopyBlend, SHADER(kFullscreenVert), SHADER(kCopyFrag), Pass::Post, Blend::Premultiplied, Stencil::None},
    {Program::ColourMatrix, SHADER(kFullscreenVert), SHADER(kColourMatrixFrag), Pass::Post, Blend::None, Stencil::None},
    {Program::DropShadow, SHADER(kFullscreenVert), SHADER(kDropShadowFrag), Pass::Post, Blend::None, Stencil::None},
    {Program::Blur, SHADER(kBlurVert), SHADER(kBlurFrag), Pass::Post, Blend::None, Stencil::None},
    {Program::BlendMask, SHADER(kFullscreenVert), SHADER(kBlendMaskFrag), Pass::Post, Blend::None, Stencil::None},
    {Program::Visor, SHADER(kFullscreenVert), SHADER(kVisorFrag), Pass::Post, Blend::None, Stencil::None},
    {Program::Present, SHADER(kFullscreenVert), SHADER(kCopyFrag), Pass::Frame, Blend::Premultiplied, Stencil::None},
};
static_assert(sizeof(kSpecs) / sizeof(kSpecs[0]) == (size_t)Program::Count, "one spec per program");

struct Pipelines {
    VkDescriptorSetLayout texture = VK_NULL_HANDLE;
    VkDescriptorSetLayout uniform = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline plain[(size_t)Program::Count] = {};
    VkPipeline clipped[(size_t)Program::Count] = {};
};
Pipelines g_pipelines;

VkDescriptorSetLayout create_set_layout(VkDescriptorType type) {
    VkDescriptorSetLayoutBinding binding{};
    binding.descriptorType = type;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 1;
    info.pBindings = &binding;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vk.CreateDescriptorSetLayout(gpu.device, &info, nullptr, &layout) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return layout;
}

bool create_layouts() {
    g_pipelines.texture = create_set_layout(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    g_pipelines.uniform = create_set_layout(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
    if (!g_pipelines.texture || !g_pipelines.uniform) return false;
    VkDescriptorSetLayout sets[3] = {g_pipelines.texture, g_pipelines.uniform, g_pipelines.texture};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushBytes};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 3;
    info.pSetLayouts = sets;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    return vk.CreatePipelineLayout(gpu.device, &info, nullptr, &g_pipelines.layout) == VK_SUCCESS;
}

VkShaderModule create_module(Shader shader) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = shader.bytes;
    info.pCode = shader.code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vk.CreateShaderModule(gpu.device, &info, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

VkPipelineColorBlendAttachmentState blend_state(const Spec& spec) {
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = spec.blend == Blend::Premultiplied;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    bool writesColour = spec.stencil != Stencil::Set && spec.stencil != Stencil::Increment;
    blend.colorWriteMask = writesColour ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
                                        : 0;
    return blend;
}

// Test compares against the reference the renderer sets; Set and Increment write the mask.
VkPipelineDepthStencilStateCreateInfo stencil_state(Stencil stencil, bool clipped) {
    VkPipelineDepthStencilStateCreateInfo state{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkStencilOpState op{};
    op.failOp = VK_STENCIL_OP_KEEP;
    op.depthFailOp = VK_STENCIL_OP_KEEP;
    op.passOp = VK_STENCIL_OP_KEEP;
    op.compareOp = VK_COMPARE_OP_ALWAYS;
    op.compareMask = 0xFF;
    op.writeMask = 0xFF;
    if (stencil == Stencil::Test && clipped) op.compareOp = VK_COMPARE_OP_EQUAL;
    if (stencil == Stencil::Set) op.passOp = VK_STENCIL_OP_REPLACE;
    if (stencil == Stencil::Increment) op.passOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    state.stencilTestEnable = stencil != Stencil::None && (stencil != Stencil::Test || clipped);
    state.front = op;
    state.back = op;
    return state;
}

VkPipeline create_pipeline(const Spec& spec, bool clipped, VkRenderPass pass) {
    VkShaderModule vertex = create_module(spec.vertex);
    VkShaderModule fragment = create_module(spec.fragment);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
    };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";

    bool geometry = spec.vertex.code == kGeometryVert;
    VkVertexInputBindingDescription binding{0, sizeof(Rml::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Rml::Vertex, position)},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Rml::Vertex, colour)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Rml::Vertex, tex_coord)},
    };
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (geometry) {
        input.vertexBindingDescriptionCount = 1;
        input.pVertexBindingDescriptions = &binding;
        input.vertexAttributeDescriptionCount = 3;
        input.pVertexAttributeDescriptions = attributes;
    }
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples =
        spec.pass == Pass::Layer ? targets::samples() : VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil = stencil_state(spec.stencil, clipped);
    VkPipelineColorBlendAttachmentState blend = blend_state(spec);
    VkPipelineColorBlendStateCreateInfo colour{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colour.attachmentCount = 1;
    colour.pAttachments = &blend;
    VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = spec.pass == Pass::Layer ? 3 : 2;
    dynamicState.pDynamicStates = dynamic;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = spec.pass == Pass::Layer ? &depthStencil : nullptr;
    info.pColorBlendState = &colour;
    info.pDynamicState = &dynamicState;
    info.layout = g_pipelines.layout;
    info.renderPass = pass;

    VkPipeline pipeline = VK_NULL_HANDLE;
    bool ok = vertex && fragment &&
              vk.CreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) ==
                  VK_SUCCESS;
    if (vertex) vk.DestroyShaderModule(gpu.device, vertex, nullptr);
    if (fragment) vk.DestroyShaderModule(gpu.device, fragment, nullptr);
    if (!ok) LOG_ERROR("pipelines: could not build program %d.", (int)spec.program);
    return ok ? pipeline : VK_NULL_HANDLE;
}

VkRenderPass pass_for(Pass pass) {
    return pass == Pass::Layer ? targets::layer_pass() : targets::post_pass();
}

void destroy_pipeline(VkPipeline& pipeline) {
    if (pipeline) vk.DestroyPipeline(gpu.device, pipeline, nullptr);
    pipeline = VK_NULL_HANDLE;
}

}  // namespace

bool create() {
    if (!create_layouts()) {
        LOG_ERROR("pipelines: could not create the layouts.");
        return false;
    }
    for (const Spec& spec : kSpecs) {
        if (spec.pass == Pass::Frame) continue;
        size_t i = (size_t)spec.program;
        g_pipelines.plain[i] = create_pipeline(spec, false, pass_for(spec.pass));
        if (!g_pipelines.plain[i]) return false;
        if (spec.stencil != Stencil::Test) continue;
        g_pipelines.clipped[i] = create_pipeline(spec, true, pass_for(spec.pass));
        if (!g_pipelines.clipped[i]) return false;
    }
    return true;
}

void destroy() {
    destroy_present();
    for (VkPipeline& pipeline : g_pipelines.plain) destroy_pipeline(pipeline);
    for (VkPipeline& pipeline : g_pipelines.clipped) destroy_pipeline(pipeline);
    if (g_pipelines.layout) vk.DestroyPipelineLayout(gpu.device, g_pipelines.layout, nullptr);
    if (g_pipelines.texture) vk.DestroyDescriptorSetLayout(gpu.device, g_pipelines.texture, nullptr);
    if (g_pipelines.uniform) vk.DestroyDescriptorSetLayout(gpu.device, g_pipelines.uniform, nullptr);
    g_pipelines = Pipelines();
}

bool create_present(VkRenderPass framePass) {
    size_t i = (size_t)Program::Present;
    g_pipelines.plain[i] = create_pipeline(kSpecs[i], false, framePass);
    return g_pipelines.plain[i] != VK_NULL_HANDLE;
}

void destroy_present() { destroy_pipeline(g_pipelines.plain[(size_t)Program::Present]); }

VkPipelineLayout layout() { return g_pipelines.layout; }
VkDescriptorSetLayout texture_layout() { return g_pipelines.texture; }
VkDescriptorSetLayout uniform_layout() { return g_pipelines.uniform; }

VkPipeline get(Program program, bool clipped) {
    size_t i = (size_t)program;
    return clipped && g_pipelines.clipped[i] ? g_pipelines.clipped[i] : g_pipelines.plain[i];
}

}  // namespace pipelines
