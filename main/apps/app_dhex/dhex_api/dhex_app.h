/*
 * The application API dhex is written against, reduced to the parts
 * it actually uses.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "dhex_gfx.h"

#define DHEX_APP_ARG_MAX 8
#define DHEX_APP_ARG_LEN 24

#define DHEX_APP_FLAG_RESUMABLE (1U << 0)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dhex_context dhex_context_t;
typedef struct dhex_app dhex_app_t;

typedef enum {
    DHEX_EVENT_CHAR,
    DHEX_EVENT_TICK,
    DHEX_EVENT_RESUME,
} dhex_event_type_t;

typedef struct {
    dhex_event_type_t type;
    union {
        char ch;
        uint32_t tick_ms;
    } data;
} dhex_event_t;

struct dhex_app {
    const char *name;
    const char *summary;
    uint32_t flags;
    esp_err_t (*start)(dhex_context_t *ctx);
    void (*suspend)(dhex_context_t *ctx);
    void (*resume)(dhex_context_t *ctx);
    void (*stop)(dhex_context_t *ctx);
    bool (*event)(dhex_context_t *ctx, const dhex_event_t *event);
    void (*title)(dhex_context_t *ctx, char *buffer, size_t buffer_len);
    uint32_t worker_stack_bytes;
    bool worker_stack_external;
    uint32_t tick_interval_ms;
    uint32_t tick_deadline_ms;
};

/* Context accessors. The context itself is owned by the host; see
 * dhex_host_context.c for this firmware's implementation. */
dhex_gfx_t *dhex_context_gfx(dhex_context_t *ctx);
void dhex_context_set_graphics_active(dhex_context_t *ctx, bool active);
void dhex_context_request_exit(dhex_context_t *ctx);
int dhex_context_argc(const dhex_context_t *ctx);
const char *dhex_context_argv(const dhex_context_t *ctx, int index);

/* Host-side helpers, not part of the application API. */
dhex_context_t *dhex_host_context_create(dhex_gfx_t *gfx);
void dhex_host_context_destroy(dhex_context_t *ctx);
void dhex_host_context_set_args(dhex_context_t *ctx, int argc, const char *const *argv);
bool dhex_host_context_exit_requested(const dhex_context_t *ctx);

#ifdef __cplusplus
}
#endif
