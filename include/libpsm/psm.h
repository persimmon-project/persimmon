#ifndef LIBPSM_PSM_H
#define LIBPSM_PSM_H

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct psm;
typedef struct psm psm_t;

// The start of every log entry is cache line-aligned.
#define CACHE_LINE_SIZE_B 64u

#define PSM_LOG_SIZE_B (1u << 20u)
#define PSM_SGARRAY_MAXSIZE 10

typedef enum psm_mode {
    PSM_MODE_NO_PERSIST,
    PSM_MODE_UNDO,
    PSM_MODE_CHKPT,
} psm_mode_t;

typedef struct psm_chkpt_config {
    const char *imgs_dir;     // Directory to dump checkpoint in.
    const char *service_path; // Socket to criu service.
    const char *log_path;     // Background process stdout and stderr.
} psm_chkpt_config_t;

typedef int (*consume_func_t)(const void *);

typedef struct psm_config {
    bool use_sga;
    int pin_core; /* Pin background thread to this core (if not -1). */
    consume_func_t consume_func;
    psm_mode_t mode;
    const char *pmem_path; /* Path to a directory on a persistent memory FS. */
    union {
        struct {
            const char *criu_service_path; // Socket to criu service.
        } undo;
        psm_chkpt_config_t chkpt;
    };
} psm_config_t;

typedef struct psm_sgaseg {
    int len;
    const void *buf;
} psm_sgaseg_t;

typedef struct psm_sgarray {
    int8_t num_segs;
    psm_sgaseg_t segs[PSM_SGARRAY_MAXSIZE];
} psm_sgarray_t;

[[gnu::visibility("default")]] int psm_init(const psm_config_t *config);

/* FIXME(zhangwen): this naming is horrible. */

/* WARNING: the log entry must not start with a NUL byte. */
[[gnu::visibility("default")]] void *psm_reserve(size_t len);
[[gnu::visibility("default")]] void psm_push(const void *log_entry, size_t len);
[[gnu::visibility("default")]] void psm_push_sga(const psm_sgarray_t *sga);
[[gnu::visibility("default")]] void psm_commit();

#ifdef __cplusplus
}
#endif

#endif // PSM_PSM_H
