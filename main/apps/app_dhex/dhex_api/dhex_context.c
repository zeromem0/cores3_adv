/*
 * Host implementation of the application context.
 *
 * The application is normally run from a shell, where the context
 * carries a terminal, a shell session, launch policy and so on. A ported
 * application only ever reaches for the graphics surface, its launch
 * arguments and the exit request, so that is all this keeps.
 */
#include "dhex_app.h"

#include <stdlib.h>
#include <string.h>

struct dhex_context {
    dhex_gfx_t *gfx;
    bool graphics_active;
    bool exit_requested;
    int argc;
    char argv[DHEX_APP_ARG_MAX][DHEX_APP_ARG_LEN];
};

dhex_context_t *dhex_host_context_create(dhex_gfx_t *gfx)
{
    dhex_context_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }
    ctx->gfx = gfx;
    return ctx;
}

void dhex_host_context_destroy(dhex_context_t *ctx)
{
    free(ctx);
}

void dhex_host_context_set_args(dhex_context_t *ctx, int argc, const char *const *argv)
{
    if (ctx == NULL) {
        return;
    }
    ctx->argc = 0;
    if (argv == NULL || argc <= 0) {
        return;
    }
    if (argc > DHEX_APP_ARG_MAX) {
        argc = DHEX_APP_ARG_MAX;
    }
    for (int i = 0; i < argc; ++i) {
        if (argv[i] == NULL) {
            ctx->argv[i][0] = '\0';
            continue;
        }
        strncpy(ctx->argv[i], argv[i], DHEX_APP_ARG_LEN - 1);
        ctx->argv[i][DHEX_APP_ARG_LEN - 1] = '\0';
    }
    ctx->argc = argc;
}

bool dhex_host_context_exit_requested(const dhex_context_t *ctx)
{
    return ctx != NULL && ctx->exit_requested;
}

dhex_gfx_t *dhex_context_gfx(dhex_context_t *ctx)
{
    return ctx != NULL ? ctx->gfx : NULL;
}

void dhex_context_set_graphics_active(dhex_context_t *ctx, bool active)
{
    if (ctx != NULL) {
        ctx->graphics_active = active;
    }
}

void dhex_context_request_exit(dhex_context_t *ctx)
{
    if (ctx != NULL) {
        ctx->exit_requested = true;
    }
}

int dhex_context_argc(const dhex_context_t *ctx)
{
    return ctx != NULL ? ctx->argc : 0;
}

const char *dhex_context_argv(const dhex_context_t *ctx, int index)
{
    if (ctx == NULL || index < 0 || index >= ctx->argc) {
        return NULL;
    }
    return ctx->argv[index];
}
