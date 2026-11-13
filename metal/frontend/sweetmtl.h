/* The C face of sweetmtl-bridge (bridge/lib.rs). Opaque objects, released by their *_release. MIT. */
#ifndef SWEETMTL_H
#define SWEETMTL_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct smtl_device smtl_device;
typedef struct smtl_queue smtl_queue;
typedef struct smtl_buffer smtl_buffer;
typedef struct smtl_cmdbuf smtl_cmdbuf;
typedef struct smtl_blit smtl_blit;

const char *smtl_last_error(void);

smtl_device *smtl_device_new(const char *turnip_path, const char *shader_cache_dir);
void smtl_device_release(smtl_device *d);
void smtl_device_name(const smtl_device *d, char *out, size_t len);
uint64_t smtl_device_registry_id(const smtl_device *d);

smtl_queue *smtl_queue_new(const smtl_device *d);
void smtl_queue_release(smtl_queue *q);

smtl_buffer *smtl_buffer_new(const smtl_device *d, uint64_t length, uint64_t mtl_resource_options);
void smtl_buffer_release(smtl_buffer *b);
void *smtl_buffer_contents(const smtl_buffer *b);
uint64_t smtl_buffer_length(const smtl_buffer *b);

smtl_cmdbuf *smtl_cmdbuf_new(const smtl_queue *q);
void smtl_cmdbuf_release(smtl_cmdbuf *c);
int smtl_cmdbuf_commit(const smtl_cmdbuf *c);
int smtl_cmdbuf_wait(const smtl_cmdbuf *c);
uint64_t smtl_cmdbuf_status(const smtl_cmdbuf *c);

smtl_blit *smtl_blit_new(const smtl_cmdbuf *c);
int smtl_blit_fill(smtl_blit *e, const smtl_buffer *b, uint64_t start, uint64_t end, uint8_t value);
int smtl_blit_end(smtl_blit *e);    /* consumes the encoder */

/* milestone 2: libraries, functions, compute */
typedef struct smtl_library smtl_library;
typedef struct smtl_function smtl_function;
typedef struct smtl_cpipe smtl_cpipe;
typedef struct smtl_compute smtl_compute;

smtl_library *smtl_library_new_data(const smtl_device *d, const void *bytes, size_t len);
void smtl_library_release(smtl_library *l);
smtl_function *smtl_function_new(const smtl_library *l, const char *name);
void smtl_function_release(smtl_function *f);
smtl_cpipe *smtl_compute_pipeline_new(const smtl_device *d, const smtl_function *f);
void smtl_compute_pipeline_release(smtl_cpipe *p);
uint32_t smtl_compute_pipeline_max_threads(const smtl_cpipe *p);
uint32_t smtl_compute_pipeline_width(const smtl_cpipe *p);
smtl_compute *smtl_compute_new(const smtl_cmdbuf *c);
int smtl_compute_set_pipeline(smtl_compute *e, const smtl_cpipe *p);
int smtl_compute_set_buffer(smtl_compute *e, const smtl_buffer *b, uint64_t offset, uint32_t index);
int smtl_compute_set_bytes(smtl_compute *e, const void *bytes, size_t len, uint32_t index);
int smtl_compute_dispatch_groups(smtl_compute *e, uint64_t gx, uint64_t gy, uint64_t gz, uint64_t tx, uint64_t ty, uint64_t tz);
int smtl_compute_dispatch_threads(smtl_compute *e, uint64_t x, uint64_t y, uint64_t z, uint64_t tx, uint64_t ty, uint64_t tz);
int smtl_compute_end(smtl_compute *e);   /* consumes the encoder */

/* milestone 3: textures, samplers, depth/stencil, render pipelines and passes (bridge/render.rs).
 * Enumerations are Metal's raw numbers. stage: 0 vertex, 1 fragment. */
typedef struct smtl_texture smtl_texture;
typedef struct smtl_sampler smtl_sampler;
typedef struct smtl_dsstate smtl_dsstate;
typedef struct smtl_rpdesc smtl_rpdesc;
typedef struct smtl_rpipe smtl_rpipe;
typedef struct smtl_rpass smtl_rpass;
typedef struct smtl_render smtl_render;
typedef struct {
	uint32_t compare, stencil_fail, depth_fail, pass, read_mask, write_mask;
} smtl_stencil;

uint32_t smtl_function_type(const smtl_function *f);

smtl_texture *smtl_texture_new(const smtl_device *d, uint32_t type, uint32_t pixel_format, uint64_t width, uint64_t height,
    uint64_t depth, uint64_t mipmaps, uint64_t array_length, uint64_t samples, uint64_t usage, uint64_t options);
void smtl_texture_release(smtl_texture *t);
int smtl_texture_replace(const smtl_texture *t, uint64_t x, uint64_t y, uint64_t z, uint64_t w, uint64_t h, uint64_t d,
    uint64_t level, uint64_t slice, const void *bytes, uint64_t bytes_per_row, uint64_t bytes_per_image);
int smtl_texture_get(const smtl_texture *t, void *out, uint64_t bytes_per_row, uint64_t bytes_per_image, uint64_t x,
    uint64_t y, uint64_t z, uint64_t w, uint64_t h, uint64_t d, uint64_t level, uint64_t slice);
/* out[9]: type, pixel format, width, height, depth, mipmaps, array length, samples, usage */
void smtl_texture_info(const smtl_texture *t, uint64_t *out);

smtl_sampler *smtl_sampler_new(const smtl_device *d, uint32_t min, uint32_t mag, uint32_t mip, uint32_t s, uint32_t t,
    uint32_t r, bool normalized, float lod_min, float lod_max, uint32_t anisotropy, uint32_t compare);
void smtl_sampler_release(smtl_sampler *s);
smtl_dsstate *smtl_depth_stencil_new(const smtl_device *d, uint32_t compare, bool write, const smtl_stencil *front,
    const smtl_stencil *back);
void smtl_depth_stencil_release(smtl_dsstate *s);

smtl_rpdesc *smtl_rpdesc_new(const smtl_function *vertex, const smtl_function *fragment);
void smtl_rpdesc_release(smtl_rpdesc *p);
int smtl_rpdesc_color(smtl_rpdesc *p, uint32_t i, uint32_t pixel_format, bool blending, uint32_t src_rgb, uint32_t dst_rgb,
    uint32_t op_rgb, uint32_t src_a, uint32_t dst_a, uint32_t op_a, uint32_t write_mask);
int smtl_rpdesc_depth_stencil(smtl_rpdesc *p, uint32_t depth, uint32_t stencil, uint32_t samples);
int smtl_rpdesc_vertex_attribute(smtl_rpdesc *p, uint32_t i, uint32_t format, uint32_t offset, uint32_t buffer);
int smtl_rpdesc_vertex_layout(smtl_rpdesc *p, uint32_t i, uint32_t stride, uint32_t step_function, uint32_t step_rate);
smtl_rpipe *smtl_render_pipeline_new(const smtl_device *d, const smtl_rpdesc *p);
void smtl_render_pipeline_release(smtl_rpipe *p);

smtl_rpass *smtl_rpass_new(void);
void smtl_rpass_release(smtl_rpass *p);
int smtl_rpass_color(smtl_rpass *p, uint32_t i, const smtl_texture *t, uint64_t level, uint64_t slice, uint64_t depth_plane,
    uint32_t load, uint32_t store, double r, double g, double b, double a, const smtl_texture *resolve);
int smtl_rpass_depth(smtl_rpass *p, const smtl_texture *t, uint64_t level, uint64_t slice, uint32_t load, uint32_t store,
    double clear);
int smtl_rpass_stencil(smtl_rpass *p, const smtl_texture *t, uint64_t level, uint64_t slice, uint32_t load, uint32_t store,
    uint32_t clear);
void smtl_rpass_size(smtl_rpass *p, uint64_t width, uint64_t height, uint64_t layers, uint64_t samples);

smtl_render *smtl_render_new(const smtl_cmdbuf *c, const smtl_rpass *p);
int smtl_render_set_pipeline(smtl_render *e, const smtl_rpipe *p);
int smtl_render_set_buffer(smtl_render *e, uint32_t stage, const smtl_buffer *b, uint64_t offset, uint32_t index);
int smtl_render_set_buffer_offset(smtl_render *e, uint32_t stage, uint64_t offset, uint32_t index);
int smtl_render_set_bytes(smtl_render *e, uint32_t stage, const void *bytes, size_t len, uint32_t index);
int smtl_render_set_texture(smtl_render *e, uint32_t stage, const smtl_texture *t, uint32_t index);
int smtl_render_set_sampler(smtl_render *e, uint32_t stage, const smtl_sampler *s, uint32_t index);
int smtl_render_set_viewport(smtl_render *e, double x, double y, double w, double h, double znear, double zfar);
int smtl_render_set_scissor(smtl_render *e, uint64_t x, uint64_t y, uint64_t w, uint64_t h);
int smtl_render_set_cull(smtl_render *e, uint32_t mode);
int smtl_render_set_winding(smtl_render *e, uint32_t winding);
int smtl_render_set_fill(smtl_render *e, uint32_t mode);
int smtl_render_set_depth_stencil(smtl_render *e, const smtl_dsstate *s);
void smtl_render_set_stencil_ref(smtl_render *e, uint32_t front, uint32_t back);
void smtl_render_set_blend_color(smtl_render *e, float r, float g, float b, float a);
int smtl_render_draw(smtl_render *e, uint32_t primitive, uint32_t start, uint32_t count, uint32_t instances, uint32_t base_instance);
int smtl_render_draw_indexed(smtl_render *e, uint32_t primitive, uint32_t count, uint32_t index_type, const smtl_buffer *b,
    uint64_t offset, uint32_t instances, int32_t base_vertex, uint32_t base_instance);
int smtl_render_end(smtl_render *e);   /* consumes the encoder */

int smtl_blit_copy_buffer(smtl_blit *e, const smtl_buffer *src, uint64_t src_offset, const smtl_buffer *dst, uint64_t dst_offset,
    uint64_t size);
int smtl_blit_buffer_to_texture(smtl_blit *e, const smtl_buffer *src, uint64_t offset, uint64_t bytes_per_row,
    uint64_t bytes_per_image, uint64_t w, uint64_t h, uint64_t d, const smtl_texture *dst, uint64_t slice, uint64_t level,
    uint64_t x, uint64_t y, uint64_t z);
int smtl_blit_texture_to_buffer(smtl_blit *e, const smtl_texture *src, uint64_t slice, uint64_t level, uint64_t x, uint64_t y,
    uint64_t z, uint64_t w, uint64_t h, uint64_t d, const smtl_buffer *dst, uint64_t offset, uint64_t bytes_per_row,
    uint64_t bytes_per_image);
int smtl_blit_texture_to_texture(smtl_blit *e, const smtl_texture *src, uint64_t src_slice, uint64_t src_level, uint64_t sx,
    uint64_t sy, uint64_t sz, uint64_t w, uint64_t h, uint64_t d, const smtl_texture *dst, uint64_t dst_slice,
    uint64_t dst_level, uint64_t dx, uint64_t dy, uint64_t dz);
int smtl_blit_mipmaps(smtl_blit *e, const smtl_texture *t);

/* A texture over an IOSurface's plane, the surface's own memory imported (no copy) where possible. */
smtl_texture *smtl_texture_new_iosurface(const smtl_device *d, uint32_t type, uint32_t pixel_format, uint64_t width,
    uint64_t height, uint64_t mipmaps, uint64_t usage, uint32_t surface, void *base, uint64_t alloc_size,
    uint64_t plane_offset, uint64_t bytes_per_row, bool planar);

#endif
