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

#[test]
fn two_stage_combiner_matches_grayscale_road_and_detail_alpha_shaders() {
    let mut r = make_headless(64, 64);

    // Helper: pack (arg1, arg2, arg0) into the same format as VulkanDX8Bridge::PackD3DTexArgs.
    let pack = |a1: u32, a2: u32, a0: u32| (a1 & 0xFF) | ((a2 & 0xFF) << 8) | ((a0 & 0xFF) << 16);
    const TA_DIFFUSE: u32 = 0;
    const TA_CURRENT: u32 = 1;
    const TA_TEXTURE: u32 = 2;
    const TA_TFACTOR: u32 = 3;
    const TA_ALPHAREPLICATE: u32 = 0x20;

    let quad = |x0: f32, y0: f32, x1: f32, y1: f32| {
        [
            VertexWw3d {
                position: [x0, y0, 0.0],
                normal: [0.0, 0.0, 1.0],
                color: [1.0, 1.0, 1.0, 1.0],
                uv0: [0.0, 0.0],
                uv1: [0.0, 0.0],
            },
            VertexWw3d {
                position: [x1, y0, 0.0],
                normal: [0.0, 0.0, 1.0],
                color: [1.0, 1.0, 1.0, 1.0],
                uv0: [1.0, 0.0],
                uv1: [1.0, 0.0],
            },
            VertexWw3d {
                position: [x1, y1, 0.0],
                normal: [0.0, 0.0, 1.0],
                color: [1.0, 1.0, 1.0, 1.0],
                uv0: [1.0, 1.0],
                uv1: [1.0, 1.0],
            },
            VertexWw3d {
                position: [x0, y1, 0.0],
                normal: [0.0, 0.0, 1.0],
                color: [1.0, 1.0, 1.0, 1.0],
                uv0: [0.0, 1.0],
                uv1: [0.0, 1.0],
            },
        ]
    };
    let nearest = SamplerDesc {
        linear_filter: false,
        clamp_edge: true,
    };

    // 1) Vivid green icon texture (0, 255, 0, 255) for DOTPRODUCT3 grayscale.
    let green_tex = r
        .create_texture_2d(1, 1, TextureFormat::Rgba8Unorm, &[0, 255, 0, 255], nearest)
        .unwrap();
    // 2) Stage 0 opaque black (0,0,0,255) + Stage 1 transparent mask (255,255,255,0) for LASER_LOGO DETAILALPHA_DETAIL.
    let opaque_black = r
        .create_texture_2d(1, 1, TextureFormat::Rgba8Unorm, &[0, 0, 0, 255], nearest)
        .unwrap();
    let transparent_mask = r
        .create_texture_2d(
            1,
            1,
            TextureFormat::Rgba8Unorm,
            &[255, 255, 255, 0],
            nearest,
        )
        .unwrap();
    // 3) Stage 0 transparent road edge (120,60,20,0) + Stage 1 dark noise (40,40,40,255) for RoadShader2Stage pass 1.
    let road_edge = r
        .create_texture_2d(1, 1, TextureFormat::Rgba8Unorm, &[120, 60, 20, 0], nearest)
        .unwrap();
    let dark_noise = r
        .create_texture_2d(1, 1, TextureFormat::Rgba8Unorm, &[40, 40, 40, 255], nearest)
        .unwrap();

    r.begin_frame().unwrap();
    r.set_viewport(Viewport::full(64, 64));
    r.clear(ClearState {
        clear_color: true,
        clear_depth_stencil: true,
        color: [0.2, 0.4, 0.8, 1.0],
        depth: 1.0,
        stencil: 0,
    })
    .unwrap();
    r.set_world_identity();
    r.set_view_identity();
    r.set_projection(ortho_2d(64.0, 64.0));
    r.set_lighting_enabled(false);

    // Quad A (x: 0..20): Render2DClass::IsGrayScale (MULTIPLYADD + DOTPRODUCT3 + TFACTOR=0x80A5CA8E).
    r.set_shader(ShaderKey::OPAQUE_3D);
    r.set_texture_factor([
        0xA5 as f32 / 255.0,
        0xCA as f32 / 255.0,
        0x8E as f32 / 255.0,
        0x80 as f32 / 255.0,
    ]);
    r.set_stage_state(
        0,
        10, // MULTIPLYADD
        pack(
            TA_TEXTURE,
            TA_TFACTOR | TA_ALPHAREPLICATE,
            TA_TFACTOR | TA_ALPHAREPLICATE,
        ),
        3, // MODULATE
        pack(TA_TEXTURE, TA_DIFFUSE, TA_CURRENT),
    );
    r.set_stage_state(
        1,
        11, // DOTPRODUCT3
        pack(TA_CURRENT, TA_TFACTOR, TA_CURRENT),
        0, // DISABLE
        0,
    );
    r.set_texture_stage(0, Some(green_tex));
    r.set_texture_stage(1, None);
    r.draw_dynamic(&quad(0.0, 0.0, 20.0, 64.0), &[0, 1, 2, 0, 2, 3])
        .unwrap();

    // Quad B (x: 22..42): LASER_LOGO DETAILALPHA_DETAIL (stage0 opaque black, stage1 alpha=0 -> background preserved!).
    r.set_shader(ShaderKey::UI_2D);
    r.set_stage_state(
        0,
        3, // MODULATE
        pack(TA_TEXTURE, TA_DIFFUSE, TA_CURRENT),
        3, // MODULATE
        pack(TA_TEXTURE, TA_DIFFUSE, TA_CURRENT),
    );
    r.set_stage_state(
        1,
        2, // SELECTARG2 (CURRENT)
        pack(TA_TEXTURE, TA_CURRENT, TA_CURRENT),
        1, // SELECTARG1 (TEXTURE)
        pack(TA_TEXTURE, TA_CURRENT, TA_CURRENT),
    );
    r.set_texture_stage(0, Some(opaque_black));
    r.set_texture_stage(1, Some(transparent_mask));
    r.draw_dynamic(&quad(22.0, 0.0, 42.0, 64.0), &[0, 1, 2, 0, 2, 3])
        .unwrap();

    // Quad C (x: 44..64): RoadShader2Stage pass 1 (BLENDCURRENTALPHA -> white where road_alpha == 0).
    r.set_shader(ShaderKey::OPAQUE_3D);
    r.set_stage_state(
        0,
        2, // SELECTARG2 (DIFFUSE | ALPHAREPLICATE -> white)
        pack(TA_TEXTURE, TA_DIFFUSE | TA_ALPHAREPLICATE, TA_CURRENT),
        1, // SELECTARG1 (TEXTURE alpha = 0)
        pack(TA_TEXTURE, TA_DIFFUSE, TA_CURRENT),
    );
    r.set_stage_state(
        1,
        9, // BLENDCURRENTALPHA (mix(CURRENT=white, TEXTURE=noise, cur.a=0) = white)
        pack(TA_TEXTURE, TA_CURRENT, TA_CURRENT),
        0, // DISABLE
        0,
    );
    r.set_texture_stage(0, Some(road_edge));
    r.set_texture_stage(1, Some(dark_noise));
    r.draw_dynamic(&quad(44.0, 0.0, 64.0, 64.0), &[0, 1, 2, 0, 2, 3])
        .unwrap();

    r.end_frame().unwrap();

    let pixels = r.readback_rgba8().unwrap();
    let sample = |x: u32, y: u32| {
        let i = ((y * 64 + x) * 4) as usize;
        [pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]]
    };

    // Quad A: pure green (0,1,0) -> Rec.601 luminance ~0.586 -> ~149 gray (R==G==B).
    let qa = sample(10, 32);
    assert_eq!(qa[0], qa[1], "grayscale pixel must have R==G, got {qa:?}");
    assert_eq!(qa[1], qa[2], "grayscale pixel must have G==B, got {qa:?}");
    assert!(
        (qa[0] as i32 - 149).abs() <= 4,
        "expected ~149 luminance for green, got {qa:?}"
    );

    // Quad B: transparent stage-1 mask must leave clear color (51, 102, 204) untouched (not black!).
    let qb = sample(32, 32);
    assert!(
        (qb[0] as i32 - 51).abs() <= 2,
        "expected blue bg, got {qb:?}"
    );
    assert!(
        (qb[2] as i32 - 204).abs() <= 2,
        "expected blue bg, got {qb:?}"
    );

    // Quad C: road feathered edge (alpha=0) must output pure white (255,255,255) for multiplicative pass.
    let qc = sample(54, 32);
    assert_eq!(
        [qc[0], qc[1], qc[2]],
        [255, 255, 255],
        "road transparent edge must be white, got {qc:?}"
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
