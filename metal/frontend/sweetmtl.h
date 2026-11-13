/* The C face of sweetmtl-bridge (bridge/lib.rs). Opaque objects, released by their *_release. MIT. */
#ifndef SWEETMTL_H
#define SWEETMTL_H
#include <stddef.h>
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

#endif
