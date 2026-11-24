use std::collections::HashMap;
use std::mem::offset_of;

use ash::vk;

use crate::error::Result;
use crate::shader::{ShaderKey, WW3D2_UBER_WGSL, compile_wgsl_to_spirv, create_shader_module};
use crate::types::VertexWw3d;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
struct PipelineLookupKey {
    shader: ShaderKey,
    color_format: vk::Format,
    depth_format: vk::Format,
}

/// Caches Vulkan 1.3 dynamic-rendering graphics pipelines keyed by `WW3D2::ShaderKey`.
pub struct PipelineCache {
    pub set0_uniform_layout: vk::DescriptorSetLayout,
    pub set1_texture_layout: vk::DescriptorSetLayout,
    pub pipeline_layout: vk::PipelineLayout,
    vs_module: vk::ShaderModule,
    fs_module: vk::ShaderModule,
    pipelines: HashMap<PipelineLookupKey, vk::Pipeline>,
}

impl PipelineCache {
    pub fn new(device: &ash::Device) -> Result<Self> {
        let set0_bindings = [vk::DescriptorSetLayoutBinding::default()
            .binding(0)
            .descriptor_type(vk::DescriptorType::UNIFORM_BUFFER_DYNAMIC)
            .descriptor_count(1)
            .stage_flags(vk::ShaderStageFlags::VERTEX | vk::ShaderStageFlags::FRAGMENT)];
        let set0_info = vk::DescriptorSetLayoutCreateInfo::default().bindings(&set0_bindings);
        // SAFETY: `device` is valid.
        let set0_uniform_layout = unsafe { device.create_descriptor_set_layout(&set0_info, None)? };

        let set1_bindings = [
            vk::DescriptorSetLayoutBinding::default()
                .binding(0)
                .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                .descriptor_count(1)
                .stage_flags(vk::ShaderStageFlags::FRAGMENT),
            vk::DescriptorSetLayoutBinding::default()
                .binding(1)
                .descriptor_type(vk::DescriptorType::SAMPLER)
                .descriptor_count(1)
                .stage_flags(vk::ShaderStageFlags::FRAGMENT),
        ];
        let set1_info = vk::DescriptorSetLayoutCreateInfo::default().bindings(&set1_bindings);
        // SAFETY: `device` is valid.
        let set1_texture_layout = unsafe { device.create_descriptor_set_layout(&set1_info, None)? };

        let set_layouts = [
            set0_uniform_layout,
            set1_texture_layout,
            set1_texture_layout,
        ];
        let layout_info = vk::PipelineLayoutCreateInfo::default().set_layouts(&set_layouts);
        // SAFETY: Both descriptor set layouts belong to `device`.
        let pipeline_layout = unsafe { device.create_pipeline_layout(&layout_info, None)? };

        let vs_spv = compile_wgsl_to_spirv(WW3D2_UBER_WGSL, naga::ShaderStage::Vertex, "vs_main")?;
        let fs_spv =
            compile_wgsl_to_spirv(WW3D2_UBER_WGSL, naga::ShaderStage::Fragment, "fs_main")?;
        let vs_module = create_shader_module(device, &vs_spv)?;
        let fs_module = create_shader_module(device, &fs_spv)?;

        Ok(Self {
            set0_uniform_layout,
            set1_texture_layout,
            pipeline_layout,
            vs_module,
            fs_module,
            pipelines: HashMap::new(),
        })
    }

    pub fn get_or_create(
        &mut self,
        device: &ash::Device,
        shader: ShaderKey,
        color_format: vk::Format,
        depth_format: vk::Format,
    ) -> Result<vk::Pipeline> {
        let key = PipelineLookupKey {
            shader: shader.pipeline_state_key(),
            color_format,
            depth_format,
        };
        if let Some(&pso) = self.pipelines.get(&key) {
            return Ok(pso);
        }

        let stages = [
            vk::PipelineShaderStageCreateInfo::default()
                .stage(vk::ShaderStageFlags::VERTEX)
                .module(self.vs_module)
                .name(c"vs_main"),
            vk::PipelineShaderStageCreateInfo::default()
                .stage(vk::ShaderStageFlags::FRAGMENT)
                .module(self.fs_module)
                .name(c"fs_main"),
        ];

        let binding_descs = [vk::VertexInputBindingDescription::default()
            .binding(0)
            .stride(std::mem::size_of::<VertexWw3d>() as u32)
            .input_rate(vk::VertexInputRate::VERTEX)];

        let attr_descs = [
            vk::VertexInputAttributeDescription::default()
                .location(0)
                .binding(0)
                .format(vk::Format::R32G32B32_SFLOAT)
                .offset(offset_of!(VertexWw3d, position) as u32),
            vk::VertexInputAttributeDescription::default()
                .location(1)
                .binding(0)
                .format(vk::Format::R32G32B32_SFLOAT)
                .offset(offset_of!(VertexWw3d, normal) as u32),
            vk::VertexInputAttributeDescription::default()
                .location(2)
                .binding(0)
                .format(vk::Format::R32G32B32A32_SFLOAT)
                .offset(offset_of!(VertexWw3d, color) as u32),
            vk::VertexInputAttributeDescription::default()
                .location(3)
                .binding(0)
                .format(vk::Format::R32G32_SFLOAT)
                .offset(offset_of!(VertexWw3d, uv0) as u32),
            vk::VertexInputAttributeDescription::default()
                .location(4)
                .binding(0)
                .format(vk::Format::R32G32_SFLOAT)
                .offset(offset_of!(VertexWw3d, uv1) as u32),
        ];

        let vertex_input = vk::PipelineVertexInputStateCreateInfo::default()
            .vertex_binding_descriptions(&binding_descs)
            .vertex_attribute_descriptions(&attr_descs);

        let input_assembly = vk::PipelineInputAssemblyStateCreateInfo::default()
            .topology(vk::PrimitiveTopology::TRIANGLE_LIST);

        let viewport_state = vk::PipelineViewportStateCreateInfo::default()
            .viewport_count(1)
            .scissor_count(1);

        let rasterizer = vk::PipelineRasterizationStateCreateInfo::default()
            .polygon_mode(vk::PolygonMode::FILL)
            .cull_mode(if key.shader.cull_enabled() {
                vk::CullModeFlags::BACK
            } else {
                vk::CullModeFlags::NONE
            })
            .front_face(if key.shader.cull_cw() {
                vk::FrontFace::COUNTER_CLOCKWISE
            } else {
                vk::FrontFace::CLOCKWISE
            })
            .line_width(1.0);

        let multisampling = vk::PipelineMultisampleStateCreateInfo::default()
            .rasterization_samples(vk::SampleCountFlags::TYPE_1);

        let depth_test = key.shader.depth_compare() != crate::shader::DepthCompare::Always
            || key.shader.depth_write();
        let depth_stencil = vk::PipelineDepthStencilStateCreateInfo::default()
            .depth_test_enable(depth_test)
            .depth_write_enable(key.shader.depth_write())
            .depth_compare_op(key.shader.depth_compare().to_vk());

        let write_mask = if key.shader.color_write() {
            vk::ColorComponentFlags::RGBA
        } else {
            vk::ColorComponentFlags::empty()
        };
        let blend_attachment = [vk::PipelineColorBlendAttachmentState::default()
            .color_write_mask(write_mask)
            .blend_enable(key.shader.blend_enabled())
            .src_color_blend_factor(key.shader.src_blend().to_vk())
            .dst_color_blend_factor(key.shader.dst_blend().to_vk())
            .color_blend_op(vk::BlendOp::ADD)
            .src_alpha_blend_factor(vk::BlendFactor::ONE)
            .dst_alpha_blend_factor(vk::BlendFactor::ONE_MINUS_SRC_ALPHA)
            .alpha_blend_op(vk::BlendOp::ADD)];
        let color_blending =
            vk::PipelineColorBlendStateCreateInfo::default().attachments(&blend_attachment);

        let dynamic_states = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
        let dynamic_state =
            vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dynamic_states);

        let color_formats = [color_format];
        let has_stencil = matches!(
            depth_format,
            vk::Format::D32_SFLOAT_S8_UINT | vk::Format::D24_UNORM_S8_UINT
        );
        let mut rendering_info = vk::PipelineRenderingCreateInfo::default()
            .color_attachment_formats(&color_formats)
            .depth_attachment_format(depth_format)
            .stencil_attachment_format(if has_stencil {
                depth_format
            } else {
                vk::Format::UNDEFINED
            });

        let pipeline_info = [vk::GraphicsPipelineCreateInfo::default()
            .stages(&stages)
            .vertex_input_state(&vertex_input)
            .input_assembly_state(&input_assembly)
            .viewport_state(&viewport_state)
            .rasterization_state(&rasterizer)
            .multisample_state(&multisampling)
            .depth_stencil_state(&depth_stencil)
            .color_blend_state(&color_blending)
            .dynamic_state(&dynamic_state)
            .layout(self.pipeline_layout)
            .push_next(&mut rendering_info)];

        // SAFETY: All pipeline state pointers and shader modules are valid.
        let pipeline = unsafe {
            device
                .create_graphics_pipelines(vk::PipelineCache::null(), &pipeline_info, None)
                .map_err(|(_, e)| e)?[0]
        };

        self.pipelines.insert(key, pipeline);
        Ok(pipeline)
    }

    pub fn destroy_all(&mut self, device: &ash::Device) {
        // SAFETY: Called on renderer shutdown after device idle.
        unsafe {
            for (_, pso) in self.pipelines.drain() {
                device.destroy_pipeline(pso, None);
            }
            device.destroy_shader_module(self.vs_module, None);
            device.destroy_shader_module(self.fs_module, None);
            device.destroy_pipeline_layout(self.pipeline_layout, None);
            device.destroy_descriptor_set_layout(self.set0_uniform_layout, None);
            device.destroy_descriptor_set_layout(self.set1_texture_layout, None);
        }
    }
}
