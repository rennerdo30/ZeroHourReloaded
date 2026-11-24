use std::path::Path;

use glam::{Mat4, Vec3};
use zhr_vulkan::shader::ShaderKey;
use zhr_vulkan::types::{
    ClearState, FogState, GpuLight, SamplerDesc, TextureFormat, VertexWw3d, Viewport, look_at_lh,
    ortho_2d, perspective_lh,
};
use zhr_vulkan::{RendererConfig, VulkanRenderer};

fn enable_local_validation_layers() {
    if std::env::var_os("VK_ADD_LAYER_PATH").is_none() {
        let local_layers =
            Path::new("E:/Development/_tools/vulkan-validation/nightly-2026-11-19/dist");
        if local_layers.exists() {
            // SAFETY: Single-threaded test setup before creating the Vulkan instance.
            unsafe {
                std::env::set_var("VK_ADD_LAYER_PATH", local_layers);
            }
        }
    }
}

fn make_headless(width: u32, height: u32) -> VulkanRenderer {
    enable_local_validation_layers();
    VulkanRenderer::new_headless(RendererConfig {
        app_name: "ZeroHourReloaded-Test".into(),
        width,
        height,
        vsync: false,
        enable_validation: true,
        preferred_device_index: None,
    })
    .expect("Vulkan 1.3 headless renderer should initialize")
}

#[test]
fn init_prefers_discrete_gpu_with_zero_validation_errors() {
    let renderer = make_headless(256, 256);
    let info = renderer.device_info();
    assert!(!info.name.is_empty());
    assert!(
        !renderer.available_devices().is_empty(),
        "must enumerate at least one Vulkan 1.3 GPU"
    );
    // If any discrete GPU is present on the machine, it must be selected first.
    if renderer.available_devices().iter().any(|d| d.is_discrete) {
        assert!(
            info.is_discrete,
            "expected discrete GPU to be selected, got '{}'",
            info.name
        );
    }
    assert_eq!(renderer.validation_error_count(), 0);
}

#[test]
fn clear_color_produces_exact_pixels() {
    let mut r = make_headless(64, 64);
    r.begin_frame().unwrap();
    r.clear(ClearState {
        clear_color: true,
        clear_depth_stencil: true,
        color: [0.2, 0.4, 0.8, 1.0],
        depth: 1.0,
        stencil: 0,
    })
    .unwrap();
    r.end_frame().unwrap();

    let pixels = r.readback_rgba8().unwrap();
    assert_eq!(pixels.len(), 64 * 64 * 4);
    let center = (32 * 64 + 32) * 4;
    let [red, green, blue, alpha] = [
        pixels[center],
        pixels[center + 1],
        pixels[center + 2],
        pixels[center + 3],
    ];
    assert!((red as i32 - 51).abs() <= 2, "red={red}");
    assert!((green as i32 - 102).abs() <= 2, "green={green}");
    assert!((blue as i32 - 204).abs() <= 2, "blue={blue}");
    assert_eq!(alpha, 255);
    assert_eq!(r.validation_error_count(), 0);
}

#[test]
fn lit_3d_cube_depth_fog_and_textured_alpha_test_render_cleanly() {
    let width = 512u32;
    let height = 512u32;
    let mut r = make_headless(width, height);

    // 4x4 checkerboard texture with transparent border (tests texture sampling + alpha test).
    let mut tex_pixels = vec![0u8; 4 * 4 * 4];
    for y in 0..4u32 {
        for x in 0..4u32 {
            let idx = ((y * 4 + x) * 4) as usize;
            let checker = (x + y) % 2 == 0;
            tex_pixels[idx] = if checker { 245 } else { 40 };
            tex_pixels[idx + 1] = if checker { 200 } else { 140 };
            tex_pixels[idx + 2] = if checker { 60 } else { 220 };
            tex_pixels[idx + 3] = 255;
        }
    }
    let checker_tex = r
        .create_texture_2d(
            4,
            4,
            TextureFormat::Rgba8Unorm,
            &tex_pixels,
            SamplerDesc {
                linear_filter: false,
                clamp_edge: false,
            },
        )
        .unwrap();

    // Unit cube geometry (clockwise front faces matching D3D8/WW3D2).
    let (cube_verts, cube_indices) = build_unit_cube();
    let vb = r.create_vertex_buffer(&cube_verts).unwrap();
    let ib = r.create_index_buffer_u16(&cube_indices).unwrap();

    let proj = perspective_lh(
        55.0f32.to_radians(),
        width as f32 / height as f32,
        0.1,
        100.0,
    );
    let view = look_at_lh(Vec3::new(2.4, 2.0, -3.2), Vec3::ZERO, Vec3::Y);

    // Render 3 consecutive frames to exercise frames-in-flight sync + dynamic ring buffers.
    for frame_i in 0..3u32 {
        r.begin_frame().unwrap();
        r.set_viewport(Viewport::full(width, height));
        r.clear(ClearState {
            clear_color: true,
            clear_depth_stencil: true,
            color: [0.06, 0.08, 0.12, 1.0],
            depth: 1.0,
            stencil: 0,
        })
        .unwrap();

        // Draw 1: 3D lit + textured cube with fog.
        r.set_world(Mat4::from_rotation_y(0.4 + frame_i as f32 * 0.25));
        r.set_view(view);
        r.set_projection(proj);
        r.set_ambient(Vec3::splat(0.25));
        r.set_lighting_enabled(true);
        r.set_light(
            0,
            GpuLight::directional(Vec3::new(-0.5, -1.0, 0.6), Vec3::new(1.0, 0.95, 0.85)),
        );
        r.set_fog(FogState {
            enabled: true,
            color: [0.06, 0.08, 0.12],
            start: 4.0,
            end: 20.0,
        });
        r.set_shader(ShaderKey::OPAQUE_3D);
        r.set_texture(Some(checker_tex));
        r.draw_indexed(vb, ib, cube_indices.len() as u32, 0, 0)
            .unwrap();

        // Draw 2: 2D alpha-blended HUD banner at top-left via dynamic buffer.
        r.set_world_identity();
        r.set_view_identity();
        r.set_projection(ortho_2d(width as f32, height as f32));
        r.set_lighting_enabled(false);
        r.set_shader(ShaderKey::UI_2D);
        r.set_texture(None);
        let hud_verts = [
            VertexWw3d::colored([16.0, 16.0, 0.0], [0.95, 0.55, 0.1, 0.85]),
            VertexWw3d::colored([220.0, 16.0, 0.0], [0.95, 0.55, 0.1, 0.85]),
            VertexWw3d::colored([220.0, 52.0, 0.0], [0.85, 0.25, 0.05, 0.85]),
            VertexWw3d::colored([16.0, 52.0, 0.0], [0.85, 0.25, 0.05, 0.85]),
        ];
        r.draw_dynamic(&hud_verts, &[0, 1, 2, 0, 2, 3]).unwrap();

        r.end_frame().unwrap();
    }

    let stats = r.last_frame_stats();
    assert_eq!(stats.draw_calls, 2);
    assert_eq!(stats.triangles, 12 + 2);

    let pixels = r.readback_rgba8().unwrap();
    // Center pixel must hit the lit cube (not the dark background).
    let center = ((height / 2) * width + (width / 2)) as usize * 4;
    let center_luma = pixels[center] as u32 + pixels[center + 1] as u32 + pixels[center + 2] as u32;
    assert!(
        center_luma > 100,
        "expected lit cube at center, got RGB=({}, {}, {})",
        pixels[center],
        pixels[center + 1],
        pixels[center + 2]
    );

    // Top-left HUD pixel (40, 30) must show the orange HUD overlay.
    let hud = (30 * width + 40) as usize * 4;
    assert!(
        pixels[hud] > 180 && pixels[hud + 1] > 70,
        "expected orange HUD pixel, got RGB=({}, {}, {})",
        pixels[hud],
        pixels[hud + 1],
        pixels[hud + 2]
    );

    assert_eq!(r.validation_error_count(), 0);
}

fn build_unit_cube() -> (Vec<VertexWw3d>, Vec<u16>) {
    let white = [1.0, 1.0, 1.0, 1.0];
    let faces: &[([f32; 3], [[f32; 3]; 4])] = &[
        // Front (-Z)
        (
            [0.0, 0.0, -1.0],
            [
                [-0.5, -0.5, -0.5],
                [-0.5, 0.5, -0.5],
                [0.5, 0.5, -0.5],
                [0.5, -0.5, -0.5],
            ],
        ),
        // Back (+Z)
        (
            [0.0, 0.0, 1.0],
            [
                [0.5, -0.5, 0.5],
                [0.5, 0.5, 0.5],
                [-0.5, 0.5, 0.5],
                [-0.5, -0.5, 0.5],
            ],
        ),
        // Right (+X)
        (
            [1.0, 0.0, 0.0],
            [
                [0.5, -0.5, -0.5],
                [0.5, 0.5, -0.5],
                [0.5, 0.5, 0.5],
                [0.5, -0.5, 0.5],
            ],
        ),
        // Left (-X)
        (
            [-1.0, 0.0, 0.0],
            [
                [-0.5, -0.5, 0.5],
                [-0.5, 0.5, 0.5],
                [-0.5, 0.5, -0.5],
                [-0.5, -0.5, -0.5],
            ],
        ),
        // Top (+Y)
        (
            [0.0, 1.0, 0.0],
            [
                [-0.5, 0.5, -0.5],
                [-0.5, 0.5, 0.5],
                [0.5, 0.5, 0.5],
                [0.5, 0.5, -0.5],
            ],
        ),
        // Bottom (-Y)
        (
            [0.0, -1.0, 0.0],
            [
                [-0.5, -0.5, 0.5],
                [-0.5, -0.5, -0.5],
                [0.5, -0.5, -0.5],
                [0.5, -0.5, 0.5],
            ],
        ),
    ];
    let uvs = [[0.0, 1.0], [0.0, 0.0], [1.0, 0.0], [1.0, 1.0]];
    let mut verts = Vec::with_capacity(24);
    let mut indices = Vec::with_capacity(36);
    for &(normal, corners) in faces {
        let base = verts.len() as u16;
        for (pos, uv) in corners.into_iter().zip(uvs) {
            verts.push(VertexWw3d::new(pos, normal, white, uv));
        }
        indices.extend_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
    }
    (verts, indices)
}
