//! Milestone 3: textures, samplers, depth/stencil states, render pipelines, render passes and render
//! encoders, plus the blit copies between buffers and textures.
//!
//! Metal's enumerations cross as their raw Metal numbers (MTLPixelFormat, MTLPrimitiveType, ...) and are
//! checked here with metal2vulkan's `TryFrom<u32>`; an unknown number fails the call.

use crate::{fail, timed};
use metal2vulkan::metal::state::{DepthStencilDescriptor, StencilDescriptor};
use metal2vulkan::metal::{
    BlendFactor, BlendOperation, ColorWriteMask, CompareFunction, CullMode, IndexType, PixelFormat, PrimitiveType,
    SamplerAddressMode, SamplerMinMagFilter, SamplerMipFilter, StencilOperation, TextureType, TextureUsage,
    TriangleFillMode, VertexFormat, VertexStepFunction, Winding,
};
use metal2vulkan::pipeline::{
    ColorAttachmentDescriptor, RenderPipelineDescriptor, StageSource, VertexAttributeDescriptor,
    VertexBufferLayoutDescriptor, VertexDescriptor,
};
use metal2vulkan_runtime::{
    BlitCommandEncoder, BlitOption, Buffer, ClearColor, CommandBuffer, DepthStencilState, Device, Function,
    LoadAction, Origin, PipelineOptions, Region, RenderCommandEncoder, RenderPassDescriptor, RenderPipelineState,
    SamplerState, ScissorRect, Size, StoreAction, Texture, TextureDescriptor, Viewport,
};
use std::ffi::c_void;
use std::ptr;

fn boxed<T, E: std::fmt::Display>(what: &str, r: Result<T, E>) -> *mut T {
    match r {
        Ok(v) => Box::into_raw(Box::new(v)),
        Err(e) => {
            fail(what, e);
            ptr::null_mut()
        }
    }
}

fn done<E: std::fmt::Display>(what: &str, r: Result<(), E>) -> i32 {
    match r {
        Ok(()) => 0,
        Err(e) => fail(what, e),
    }
}

/// A raw Metal number as metal2vulkan's enum, or the failing call's -1.
macro_rules! raw {
    ($ty:ty, $v:expr, $what:expr) => {
        match <$ty>::try_from($v as u32) {
            Ok(x) => x,
            Err(e) => return fail($what, e).into(),
        }
    };
}

/// Like `raw!` for functions returning a pointer.
macro_rules! raw_p {
    ($ty:ty, $v:expr, $what:expr) => {
        match <$ty>::try_from($v as u32) {
            Ok(x) => x,
            Err(e) => {
                fail($what, e);
                return ptr::null_mut();
            }
        }
    };
}

macro_rules! release_fn {
    ($name:ident, $ty:ty) => {
        #[no_mangle]
        pub unsafe extern "C" fn $name(p: *mut $ty) {
            if !p.is_null() {
                drop(Box::from_raw(p));
            }
        }
    };
}

unsafe fn opt<'a, T>(p: *const T) -> Option<&'a T> {
    if p.is_null() { None } else { Some(&*p) }
}

// ---- textures ----

/// newTextureWithDescriptor: (type, pixelFormat, size, mipmapLevelCount, arrayLength, sampleCount,
/// usage and resourceOptions as Metal numbers them).
#[no_mangle]
pub unsafe extern "C" fn smtl_texture_new(
    d: *const Device, texture_type: u32, pixel_format: u32, width: u64, height: u64, depth: u64, mipmaps: u64,
    array_length: u64, samples: u64, usage: u64, options: u64,
) -> *mut Texture {
    let mut t = TextureDescriptor::default();
    t.texture_type = raw_p!(TextureType, texture_type, "texture type");
    t.pixel_format = raw_p!(PixelFormat, pixel_format, "pixel format");
    t.width = width.max(1);
    t.height = height.max(1);
    t.depth = depth.max(1);
    t.mipmap_level_count = mipmaps.max(1);
    t.array_length = array_length.max(1);
    t.sample_count = samples.max(1);
    t.usage = raw_p!(TextureUsage, usage & 0x37, "texture usage");
    t.resource_options = crate::resource_options(options);
    boxed("new_texture", (*d).new_texture(&t))
}
release_fn!(smtl_texture_release, Texture);

/// replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:
#[no_mangle]
pub unsafe extern "C" fn smtl_texture_replace(
    t: *const Texture, x: u64, y: u64, z: u64, w: u64, h: u64, dd: u64, level: u64, slice: u64, bytes: *const u8,
    bytes_per_row: u64, bytes_per_image: u64,
) -> i32 {
    let rows = h.max(1) * dd.max(1);
    let len = if bytes_per_image != 0 { bytes_per_image * dd.max(1) } else { bytes_per_row * rows };
    let region = Region { origin: Origin::new(x, y, z), size: Size::new(w, h, dd) };
    done(
        "replace_region",
        (*t).replace_region(region, level, slice, std::slice::from_raw_parts(bytes, len as usize), bytes_per_row, bytes_per_image),
    )
}

/// getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:
#[no_mangle]
pub unsafe extern "C" fn smtl_texture_get(
    t: *const Texture, out: *mut u8, bytes_per_row: u64, bytes_per_image: u64, x: u64, y: u64, z: u64, w: u64,
    h: u64, dd: u64, level: u64, slice: u64,
) -> i32 {
    let rows = h.max(1) * dd.max(1);
    let len = if bytes_per_image != 0 { bytes_per_image * dd.max(1) } else { bytes_per_row * rows };
    let region = Region { origin: Origin::new(x, y, z), size: Size::new(w, h, dd) };
    done(
        "get_bytes",
        (*t).get_bytes(std::slice::from_raw_parts_mut(out, len as usize), bytes_per_row, bytes_per_image, region, level, slice),
    )
}

// ---- samplers, depth/stencil states ----

#[no_mangle]
pub unsafe extern "C" fn smtl_sampler_new(
    d: *const Device, min: u32, mag: u32, mip: u32, s: u32, t: u32, r: u32, normalized: bool, lod_min: f32,
    lod_max: f32, anisotropy: u32, compare: u32,
) -> *mut SamplerState {
    let mut sd = metal2vulkan::bind::SamplerDescriptor::default();
    sd.min_filter = raw_p!(SamplerMinMagFilter, min, "min filter");
    sd.mag_filter = raw_p!(SamplerMinMagFilter, mag, "mag filter");
    sd.mip_filter = raw_p!(SamplerMipFilter, mip, "mip filter");
    sd.s_address_mode = raw_p!(SamplerAddressMode, s, "address mode");
    sd.t_address_mode = raw_p!(SamplerAddressMode, t, "address mode");
    sd.r_address_mode = raw_p!(SamplerAddressMode, r, "address mode");
    sd.normalized_coordinates = normalized;
    sd.lod_min_clamp = lod_min;
    sd.lod_max_clamp = lod_max;
    sd.max_anisotropy = anisotropy.max(1);
    sd.compare_function = raw_p!(CompareFunction, compare, "compare function");
    boxed("new_sampler_state", (*d).new_sampler_state(&sd))
}
release_fn!(smtl_sampler_release, SamplerState);

/// One face's MTLStencilDescriptor: compare, stencil fail, depth fail, pass, read mask, write mask.
#[repr(C)]
pub struct SmtlStencil {
    pub compare: u32,
    pub stencil_fail: u32,
    pub depth_fail: u32,
    pub pass: u32,
    pub read_mask: u32,
    pub write_mask: u32,
}

unsafe fn stencil(s: *const SmtlStencil) -> Result<StencilDescriptor, String> {
    let mut d = StencilDescriptor::default();
    if let Some(s) = opt(s) {
        let e = |x: metal2vulkan::error::Error| x.to_string();
        d.stencil_compare_function = CompareFunction::try_from(s.compare).map_err(e)?;
        d.stencil_failure_operation = StencilOperation::try_from(s.stencil_fail).map_err(e)?;
        d.depth_failure_operation = StencilOperation::try_from(s.depth_fail).map_err(e)?;
        d.depth_stencil_pass_operation = StencilOperation::try_from(s.pass).map_err(e)?;
        d.read_mask = s.read_mask;
        d.write_mask = s.write_mask;
    }
    Ok(d)
}

/// newDepthStencilStateWithDescriptor: (front/back NULL = Metal's default stencil descriptor).
#[no_mangle]
pub unsafe extern "C" fn smtl_depth_stencil_new(
    d: *const Device, compare: u32, write: bool, front: *const SmtlStencil, back: *const SmtlStencil,
) -> *mut DepthStencilState {
    let mut ds = DepthStencilDescriptor::default();
    ds.depth_compare_function = raw_p!(CompareFunction, compare, "depth compare");
    ds.depth_write_enabled = write;
    match (stencil(front), stencil(back)) {
        (Ok(f), Ok(b)) => {
            ds.front_face_stencil = f;
            ds.back_face_stencil = b;
        }
        (Err(e), _) | (_, Err(e)) => {
            fail("stencil descriptor", e);
            return ptr::null_mut();
        }
    }
    boxed("new_depth_stencil_state", (*d).new_depth_stencil_state(&ds))
}
release_fn!(smtl_depth_stencil_release, DepthStencilState);

// ---- render pipelines ----

#[no_mangle]
pub unsafe extern "C" fn smtl_rpdesc_new(vertex: *const Function, fragment: *const Function) -> *mut RenderPipelineDescriptor {
    let mut desc = RenderPipelineDescriptor::new(StageSource::Function((*vertex).function_descriptor()));
    if let Some(f) = opt(fragment) {
        desc.fragment = Some(StageSource::Function(f.function_descriptor()));
    }
    Box::into_raw(Box::new(desc))
}
release_fn!(smtl_rpdesc_release, RenderPipelineDescriptor);

/// colorAttachments[i]: pixel format 0 (Invalid) clears the slot.
#[no_mangle]
pub unsafe extern "C" fn smtl_rpdesc_color(
    p: *mut RenderPipelineDescriptor, i: u32, pixel_format: u32, blending: bool, src_rgb: u32, dst_rgb: u32,
    op_rgb: u32, src_a: u32, dst_a: u32, op_a: u32, write_mask: u32,
) -> i32 {
    let Some(slot) = (*p).color_attachments.get_mut(i as usize) else {
        return fail("color attachment", "index past 8");
    };
    if pixel_format == 0 {
        *slot = None;
        return 0;
    }
    let mut c = ColorAttachmentDescriptor::new(raw!(PixelFormat, pixel_format, "pixel format"));
    c.blending_enabled = blending;
    c.source_rgb_blend_factor = raw!(BlendFactor, src_rgb, "blend factor");
    c.destination_rgb_blend_factor = raw!(BlendFactor, dst_rgb, "blend factor");
    c.rgb_blend_operation = raw!(BlendOperation, op_rgb, "blend operation");
    c.source_alpha_blend_factor = raw!(BlendFactor, src_a, "blend factor");
    c.destination_alpha_blend_factor = raw!(BlendFactor, dst_a, "blend factor");
    c.alpha_blend_operation = raw!(BlendOperation, op_a, "blend operation");
    c.write_mask = raw!(ColorWriteMask, write_mask, "write mask");
    *slot = Some(c);
    0
}

/// depthAttachmentPixelFormat, stencilAttachmentPixelFormat (0 = none), rasterSampleCount.
#[no_mangle]
pub unsafe extern "C" fn smtl_rpdesc_depth_stencil(p: *mut RenderPipelineDescriptor, depth: u32, stencil: u32, samples: u32) -> i32 {
    (*p).depth_format = if depth == 0 { None } else { Some(raw!(PixelFormat, depth, "depth format")) };
    (*p).stencil_format = if stencil == 0 { None } else { Some(raw!(PixelFormat, stencil, "stencil format")) };
    (*p).raster_sample_count = samples.max(1);
    0
}

fn vertex_descriptor(p: &mut RenderPipelineDescriptor) -> &mut VertexDescriptor {
    p.vertex_descriptor.get_or_insert_with(VertexDescriptor::new)
}

/// vertexDescriptor.attributes[i] (format 0 = Invalid removes it).
#[no_mangle]
pub unsafe extern "C" fn smtl_rpdesc_vertex_attribute(p: *mut RenderPipelineDescriptor, i: u32, format: u32, offset: u32, buffer: u32) -> i32 {
    let v = vertex_descriptor(&mut *p);
    if format == 0 {
        v.attributes.remove(&i);
        return 0;
    }
    let format = raw!(VertexFormat, format, "vertex format");
    v.attributes.insert(i, VertexAttributeDescriptor::new(format, offset, buffer));
    0
}

/// vertexDescriptor.layouts[i].
#[no_mangle]
pub unsafe extern "C" fn smtl_rpdesc_vertex_layout(p: *mut RenderPipelineDescriptor, i: u32, stride: u32, step_function: u32, step_rate: u32) -> i32 {
    let step = raw!(VertexStepFunction, step_function, "step function");
    let mut l = VertexBufferLayoutDescriptor::new(Some(stride));
    l.step_function = step;
    l.step_rate = step_rate;
    vertex_descriptor(&mut *p).layouts.insert(i, l);
    0
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_pipeline_new(d: *const Device, p: *const RenderPipelineDescriptor) -> *mut RenderPipelineState {
    boxed(
        "new_render_pipeline_state",
        timed("render_pipeline", || (*d).new_render_pipeline_state(&*p, &PipelineOptions::default())),
    )
}
release_fn!(smtl_render_pipeline_release, RenderPipelineState);

// ---- render passes ----

fn load(v: u32) -> LoadAction {
    match v {
        1 => LoadAction::Load,
        2 => LoadAction::Clear,
        _ => LoadAction::DontCare,
    }
}

fn store(v: u32) -> StoreAction {
    match v {
        0 => StoreAction::DontCare,
        2 => StoreAction::MultisampleResolve,
        3 => StoreAction::StoreAndMultisampleResolve,
        _ => StoreAction::Store,
    }
}

#[no_mangle]
pub extern "C" fn smtl_rpass_new() -> *mut RenderPassDescriptor {
    Box::into_raw(Box::new(RenderPassDescriptor::new()))
}
release_fn!(smtl_rpass_release, RenderPassDescriptor);

/// colorAttachments[i] (load/store as MTLLoadAction/MTLStoreAction; texture NULL clears the slot).
#[no_mangle]
pub unsafe extern "C" fn smtl_rpass_color(
    p: *mut RenderPassDescriptor, i: u32, t: *const Texture, level: u64, slice: u64, depth_plane: u64,
    load_action: u32, store_action: u32, r: f64, g: f64, b: f64, a: f64, resolve: *const Texture,
) -> i32 {
    let Some(c) = (*p).color_attachments.get_mut(i as usize) else {
        return fail("color attachment", "index past the attachments");
    };
    c.texture = opt(t).cloned();
    c.level = level;
    c.slice = slice;
    c.depth_plane = depth_plane;
    c.load_action = load(load_action);
    c.store_action = store(store_action);
    c.clear_color = ClearColor::new(r, g, b, a);
    c.resolve_texture = opt(resolve).cloned();
    0
}

#[no_mangle]
pub unsafe extern "C" fn smtl_rpass_depth(
    p: *mut RenderPassDescriptor, t: *const Texture, level: u64, slice: u64, load_action: u32, store_action: u32, clear: f64,
) -> i32 {
    let d = &mut (*p).depth_attachment;
    d.texture = opt(t).cloned();
    d.level = level;
    d.slice = slice;
    d.load_action = load(load_action);
    d.store_action = store(store_action);
    d.clear_depth = clear;
    0
}

#[no_mangle]
pub unsafe extern "C" fn smtl_rpass_stencil(
    p: *mut RenderPassDescriptor, t: *const Texture, level: u64, slice: u64, load_action: u32, store_action: u32, clear: u32,
) -> i32 {
    let s = &mut (*p).stencil_attachment;
    s.texture = opt(t).cloned();
    s.level = level;
    s.slice = slice;
    s.load_action = load(load_action);
    s.store_action = store(store_action);
    s.clear_stencil = clear;
    0
}

/// renderTargetWidth/Height, renderTargetArrayLength, defaultRasterSampleCount.
#[no_mangle]
pub unsafe extern "C" fn smtl_rpass_size(p: *mut RenderPassDescriptor, width: u64, height: u64, layers: u64, samples: u64) {
    (*p).render_target_width = width;
    (*p).render_target_height = height;
    (*p).render_target_array_length = layers;
    (*p).default_raster_sample_count = samples;
}

// ---- render encoders ----

#[no_mangle]
pub unsafe extern "C" fn smtl_render_new(c: *const CommandBuffer, p: *const RenderPassDescriptor) -> *mut RenderCommandEncoder {
    boxed("render_command_encoder", timed("render_new", || (*c).render_command_encoder(&*p)))
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_pipeline(e: *mut RenderCommandEncoder, p: *const RenderPipelineState) -> i32 {
    done("set_render_pipeline_state", (*e).set_render_pipeline_state(&*p))
}

/// stage 0 = vertex, 1 = fragment.
#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_buffer(e: *mut RenderCommandEncoder, stage: u32, b: *const Buffer, offset: u64, index: u32) -> i32 {
    let r = if stage == 0 { (*e).set_vertex_buffer(opt(b), offset, index) } else { (*e).set_fragment_buffer(opt(b), offset, index) };
    done("set_buffer", r)
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_buffer_offset(e: *mut RenderCommandEncoder, stage: u32, offset: u64, index: u32) -> i32 {
    let r = if stage == 0 { (*e).set_vertex_buffer_offset(offset, index) } else { (*e).set_fragment_buffer_offset(offset, index) };
    done("set_buffer_offset", r)
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_bytes(e: *mut RenderCommandEncoder, stage: u32, bytes: *const u8, len: usize, index: u32) -> i32 {
    let bytes = std::slice::from_raw_parts(bytes, len);
    let r = if stage == 0 { (*e).set_vertex_bytes(bytes, index) } else { (*e).set_fragment_bytes(bytes, index) };
    done("set_bytes", r)
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_texture(e: *mut RenderCommandEncoder, stage: u32, t: *const Texture, index: u32) -> i32 {
    let r = if stage == 0 { (*e).set_vertex_texture(opt(t), index) } else { (*e).set_fragment_texture(opt(t), index) };
    done("set_texture", r)
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_sampler(e: *mut RenderCommandEncoder, stage: u32, s: *const SamplerState, index: u32) -> i32 {
    let r = if stage == 0 { (*e).set_vertex_sampler_state(opt(s), index) } else { (*e).set_fragment_sampler_state(opt(s), index) };
    done("set_sampler_state", r)
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_viewport(e: *mut RenderCommandEncoder, x: f64, y: f64, w: f64, h: f64, znear: f64, zfar: f64) -> i32 {
    done("set_viewport", (*e).set_viewport(Viewport::new(x, y, w, h, znear, zfar)))
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_scissor(e: *mut RenderCommandEncoder, x: u64, y: u64, w: u64, h: u64) -> i32 {
    done("set_scissor_rect", (*e).set_scissor_rect(ScissorRect::new(x, y, w, h)))
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_cull(e: *mut RenderCommandEncoder, mode: u32) -> i32 {
    (*e).set_cull_mode(raw!(CullMode, mode, "cull mode"));
    0
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_winding(e: *mut RenderCommandEncoder, winding: u32) -> i32 {
    (*e).set_front_facing_winding(raw!(Winding, winding, "winding"));
    0
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_fill(e: *mut RenderCommandEncoder, mode: u32) -> i32 {
    done("set_triangle_fill_mode", (*e).set_triangle_fill_mode(raw!(TriangleFillMode, mode, "fill mode")))
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_depth_stencil(e: *mut RenderCommandEncoder, s: *const DepthStencilState) -> i32 {
    done("set_depth_stencil_state", (*e).set_depth_stencil_state(opt(s)))
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_stencil_ref(e: *mut RenderCommandEncoder, front: u32, back: u32) {
    (*e).set_stencil_reference_values(front, back);
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_blend_color(e: *mut RenderCommandEncoder, r: f32, g: f32, b: f32, a: f32) {
    (*e).set_blend_color(r, g, b, a);
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_draw(
    e: *mut RenderCommandEncoder, primitive: u32, start: u32, count: u32, instances: u32, base_instance: u32,
) -> i32 {
    let prim = raw!(PrimitiveType, primitive, "primitive type");
    done("draw_primitives", (*e).draw_primitives(prim, start, count, instances, base_instance))
}

/// index_type: MTLIndexType (0 uint16, 1 uint32).
#[no_mangle]
pub unsafe extern "C" fn smtl_render_draw_indexed(
    e: *mut RenderCommandEncoder, primitive: u32, count: u32, index_type: u32, b: *const Buffer, offset: u64,
    instances: u32, base_vertex: i32, base_instance: u32,
) -> i32 {
    let prim = raw!(PrimitiveType, primitive, "primitive type");
    let it = raw!(IndexType, index_type, "index type");
    done(
        "draw_indexed_primitives",
        (*e).draw_indexed_primitives(prim, count, it, &*b, offset, instances, base_vertex, base_instance),
    )
}

/// endEncoding: consumes the encoder.
#[no_mangle]
pub unsafe extern "C" fn smtl_render_end(e: *mut RenderCommandEncoder) -> i32 {
    done("end_encoding", timed("render_end", || Box::from_raw(e).end_encoding()))
}

// ---- blit copies ----

#[no_mangle]
pub unsafe extern "C" fn smtl_blit_copy_buffer(e: *mut BlitCommandEncoder, src: *const Buffer, so: u64, dst: *const Buffer, dof: u64, size: u64) -> i32 {
    done("copy_from_buffer_to_buffer", (*e).copy_from_buffer_to_buffer(&*src, so, &*dst, dof, size))
}

#[no_mangle]
pub unsafe extern "C" fn smtl_blit_buffer_to_texture(
    e: *mut BlitCommandEncoder, src: *const Buffer, offset: u64, bytes_per_row: u64, bytes_per_image: u64, w: u64,
    h: u64, dd: u64, dst: *const Texture, slice: u64, level: u64, x: u64, y: u64, z: u64,
) -> i32 {
    done(
        "copy_from_buffer_to_texture",
        (*e).copy_from_buffer_to_texture(
            &*src, offset, bytes_per_row, bytes_per_image, Size::new(w, h, dd), &*dst, slice, level,
            Origin::new(x, y, z), BlitOption::None,
        ),
    )
}

#[no_mangle]
pub unsafe extern "C" fn smtl_blit_texture_to_buffer(
    e: *mut BlitCommandEncoder, src: *const Texture, slice: u64, level: u64, x: u64, y: u64, z: u64, w: u64, h: u64,
    dd: u64, dst: *const Buffer, offset: u64, bytes_per_row: u64, bytes_per_image: u64,
) -> i32 {
    done(
        "copy_from_texture_to_buffer",
        (*e).copy_from_texture_to_buffer(
            &*src, slice, level, Origin::new(x, y, z), Size::new(w, h, dd), &*dst, offset, bytes_per_row,
            bytes_per_image, BlitOption::None,
        ),
    )
}

#[no_mangle]
pub unsafe extern "C" fn smtl_blit_texture_to_texture(
    e: *mut BlitCommandEncoder, src: *const Texture, ss: u64, sl: u64, sx: u64, sy: u64, sz: u64, w: u64, h: u64,
    dd: u64, dst: *const Texture, ds: u64, dl: u64, dx: u64, dy: u64, dz: u64,
) -> i32 {
    done(
        "copy_from_texture_to_texture",
        (*e).copy_from_texture_to_texture(
            &*src, ss, sl, Origin::new(sx, sy, sz), Size::new(w, h, dd), &*dst, ds, dl, Origin::new(dx, dy, dz),
        ),
    )
}

#[no_mangle]
pub unsafe extern "C" fn smtl_blit_mipmaps(e: *mut BlitCommandEncoder, t: *const Texture) -> i32 {
    done("generate_mipmaps", (*e).generate_mipmaps(&*t))
}

// ---- texture facts the frontend answers ----

#[no_mangle]
pub unsafe extern "C" fn smtl_texture_info(t: *const Texture, out: *mut u64) {
    let t = &*t;
    let v = [
        u32::from(t.texture_type()) as u64,
        u32::from(t.pixel_format()) as u64,
        t.width(),
        t.height(),
        t.depth(),
        t.mipmap_level_count(),
        t.array_length(),
        t.sample_count(),
        u32::from(t.usage()) as u64,
    ];
    ptr::copy_nonoverlapping(v.as_ptr(), out, v.len());
}

/// MTLFunctionType (1 vertex, 2 fragment, 3 kernel, ...), or 0 when it cannot be read.
#[no_mangle]
pub unsafe extern "C" fn smtl_function_type(f: *const Function) -> u32 {
    match (*f).function_type() {
        Ok(t) => u32::from(t),
        Err(e) => {
            fail("function_type", e);
            0
        }
    }
}

// ---- IOSurface textures (newTextureWithDescriptor:iosurface:plane:) ----

type SurfaceFd = unsafe extern "C" fn(u32, u64) -> i32;

/// sweetdrm_surface_fd from Turnip (sweetdrm is linked into libvulkan_freedreno), or None.
fn surface_fd_fn() -> Option<SurfaceFd> {
    let h = *crate::TURNIP_HANDLE.get()?;
    // SAFETY: dlsym on the handle open_turnip got; the symbol has this C signature (sweetdrm.h).
    unsafe {
        let f = libc::dlsym(h as *mut c_void, c"sweetdrm_surface_fd".as_ptr());
        if f.is_null() { None } else { Some(std::mem::transmute::<*mut c_void, SurfaceFd>(f)) }
    }
}

/// A texture over plane `plane_offset`/`bytes_per_row` of IOSurface `surface`, whose memory this process maps at
/// `base` (`alloc_size` bytes). The surface's memory becomes a no-copy buffer whose pages the runtime imports as
/// dma-bufs (Turnip refuses host pointers, mesa patch 0006): sweetdrm hands out a descriptor standing for the
/// surface, and RedwoodGPU gives the surface's own memory to the GPU (no copy). Only requests that start at
/// the surface's first page are answered that way; any other (a plane further in) gets an error, and the
/// runtime makes that texture a shadow of the bytes instead (correct, with copies).
#[no_mangle]
pub unsafe extern "C" fn smtl_texture_new_iosurface(
    d: *const Device, texture_type: u32, pixel_format: u32, width: u64, height: u64, mipmaps: u64, usage: u64,
    surface: u32, base: *mut u8, alloc_size: u64, plane_offset: u64, bytes_per_row: u64, planar: bool,
) -> *mut Texture {
    use metal2vulkan_runtime::{DmaBufChunk, DmaBufRequest, DmaBufSource, HostMemory, IoSurfacePlane, ResourceOptions};
    use std::os::fd::{FromRawFd, OwnedFd};
    use std::sync::Arc;

    let Some(base_nn) = std::ptr::NonNull::new(base) else {
        fail("newTextureWithDescriptor:iosurface:", "surface not mapped");
        return ptr::null_mut();
    };
    let Some(make_fd) = surface_fd_fn() else {
        fail("newTextureWithDescriptor:iosurface:", "Turnip has no sweetdrm_surface_fd");
        return ptr::null_mut();
    };
    let base_addr = base as usize;
    let source: DmaBufSource = Arc::new(move |req: DmaBufRequest| {
        if req.start.as_ptr() as usize != base_addr {
            return Err(std::io::Error::new(std::io::ErrorKind::Unsupported, "pages not at the surface's start"));
        }
        // SAFETY: sweetdrm's C function; a new descriptor or -1.
        let fd = unsafe { make_fd(surface, req.length) };
        if fd < 0 {
            return Err(std::io::Error::last_os_error());
        }
        // SAFETY: a descriptor just made for this, owned from here on.
        Ok(vec![DmaBufChunk { fd: unsafe { OwnedFd::from_raw_fd(fd) }, length: req.length }])
    });
    let memory = HostMemory::with_dma_buf(base_nn, source);
    let buffer = match timed("iosurface_buffer", || (*d).new_buffer_with_bytes_no_copy(memory, alloc_size, ResourceOptions::shared(), None)) {
        Ok(b) => b,
        Err(e) => {
            fail("new_buffer_with_bytes_no_copy (IOSurface)", e);
            return ptr::null_mut();
        }
    };
    let mut t = TextureDescriptor::default();
    t.texture_type = raw_p!(TextureType, texture_type, "texture type");
    t.pixel_format = raw_p!(PixelFormat, pixel_format, "pixel format");
    t.width = width.max(1);
    t.height = height.max(1);
    t.mipmap_level_count = mipmaps.max(1);
    t.usage = raw_p!(TextureUsage, usage & 0x37, "texture usage");
    t.resource_options = ResourceOptions::shared();
    let plane = IoSurfacePlane::new(plane_offset, bytes_per_row, planar);
    boxed("new_texture_with_iosurface_plane", timed("iosurface_texture", || buffer.new_texture_with_iosurface_plane(&t, plane)))
}

// ---- function constants (newFunctionWithName:constantValues:error:, newFunctionWithDescriptor:error:) ----

/// The bytes of one value of Metal's data type `ty` (scalars and their vectors; `None` for any other type).
fn constant_size(ty: u32) -> Option<usize> {
    let (elem, first) = match ty {
        3..=6 => (4, 3),     // Float .. Float4
        16..=19 => (2, 16),  // Half .. Half4
        29..=32 => (4, 29),  // Int .. Int4
        33..=36 => (4, 33),  // UInt .. UInt4
        37..=40 => (2, 37),  // Short .. Short4
        41..=44 => (2, 41),  // UShort .. UShort4
        45..=48 => (1, 45),  // Char .. Char4
        49..=52 => (1, 49),  // UChar .. UChar4
        53..=56 => (1, 53),  // Bool .. Bool4
        81..=84 => (8, 81),  // Long .. Long4
        85..=88 => (8, 85),  // ULong .. ULong4
        _ => return None,
    };
    Some(elem * (ty - first + 1) as usize)
}

/// Function constant values being collected from an MTLFunctionConstantValues.
pub struct SmtlConstants(metal2vulkan::library::FunctionConstantValues);

#[no_mangle]
pub extern "C" fn smtl_constants_new() -> *mut SmtlConstants {
    Box::into_raw(Box::new(SmtlConstants(metal2vulkan::library::FunctionConstantValues::defaults())))
}

#[no_mangle]
pub unsafe extern "C" fn smtl_constants_release(c: *mut SmtlConstants) {
    if !c.is_null() {
        drop(Box::from_raw(c));
    }
}

unsafe fn constant_value(ty: u32, data: *const c_void) -> Result<metal2vulkan::library::FunctionConstantValue, String> {
    let size = constant_size(ty).ok_or_else(|| format!("constant of data type {ty}"))?;
    if data.is_null() {
        return Err("constant without data".into());
    }
    let t = metal2vulkan::metal::DataType::try_from(ty).map_err(|e| e.to_string())?;
    let bytes = std::slice::from_raw_parts(data as *const u8, size).to_vec();
    Ok(metal2vulkan::library::FunctionConstantValue::new(t, bytes))
}

/// One value by index; 0 or -1 (the type is not one Metal's constants take, see smtl_last_error).
#[no_mangle]
pub unsafe extern "C" fn smtl_constants_index(c: *mut SmtlConstants, index: u32, ty: u32, data: *const c_void) -> i32 {
    match constant_value(ty, data) {
        Ok(v) => {
            let values = std::mem::take(&mut (*c).0);
            (*c).0 = values.with_index(index, v);
            0
        }
        Err(e) => fail("function constant", e),
    }
}

/// One value by name; as smtl_constants_index.
#[no_mangle]
pub unsafe extern "C" fn smtl_constants_name(c: *mut SmtlConstants, name: *const std::ffi::c_char, ty: u32,
    data: *const c_void) -> i32 {
    let name = std::ffi::CStr::from_ptr(name).to_string_lossy().into_owned();
    match constant_value(ty, data) {
        Ok(v) => {
            let values = std::mem::take(&mut (*c).0);
            (*c).0 = values.with_name(name, v);
            0
        }
        Err(e) => fail("function constant", e),
    }
}

/// The library's function `name` specialized with `c`.
#[no_mangle]
pub unsafe extern "C" fn smtl_function_new_constants(l: *const metal2vulkan_runtime::Library,
    name: *const std::ffi::c_char, c: *const SmtlConstants) -> *mut Function {
    let name = std::ffi::CStr::from_ptr(name).to_string_lossy();
    boxed("new_function_with_name_constant_values",
        timed("function_constants", || (*l).new_function_with_name_constant_values(&name, &(*c).0)))
}

/// newTextureViewWithPixelFormat:
#[no_mangle]
pub unsafe extern "C" fn smtl_texture_view(t: *const Texture, pixel_format: u32) -> *mut Texture {
    let f = raw_p!(PixelFormat, pixel_format, "pixel format");
    boxed("new_texture_view", (*t).new_texture_view(f))
}

/// newTextureWithDescriptor:offset:bytesPerRow: on a buffer (and the private newLinearTextureWithDescriptor:...,
/// which CoreAnimation uses for layer images): a texture whose texels are the buffer's bytes.
#[no_mangle]
pub unsafe extern "C" fn smtl_buffer_texture(b: *const Buffer, texture_type: u32, pixel_format: u32, width: u64,
    height: u64, mipmaps: u64, usage: u64, offset: u64, bytes_per_row: u64) -> *mut Texture {
    let mut t = TextureDescriptor::default();
    t.texture_type = raw_p!(TextureType, texture_type, "texture type");
    t.pixel_format = raw_p!(PixelFormat, pixel_format, "pixel format");
    t.width = width.max(1);
    t.height = height.max(1);
    t.mipmap_level_count = mipmaps.max(1);
    t.usage = raw_p!(TextureUsage, usage & 0x37, "texture usage");
    t.resource_options = (*b).resource_options();
    boxed("buffer new_texture", (*b).new_texture(&t, offset, bytes_per_row))
}
