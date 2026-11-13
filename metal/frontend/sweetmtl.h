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

#endif
