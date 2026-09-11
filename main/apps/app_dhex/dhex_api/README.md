# dhex application API

`dhex.c` is written against a small, stable C API rather than against any
one firmware. This directory holds that API, together with the
implementation of it for this firmware, which is what lets the application
source be carried in unmodified.

The API:

- `dhex_gfx.h`   - immediate-mode drawing on a monochrome-styled surface
- `dhex_app.h`   - application lifecycle (`dhex_app_t`), the per-run
                   context, and key/tick events
- `dhex_keys.h`  - key codes for non-printable keys
- `dhex_log.h`   - logging macros
- `dhex_board.h` - board pin and peripheral macros
- `dhex_uart.h`  - UART service declarations

Everything below the API is host-specific. Here it is implemented on top
of M5GFX (`LGFX_Sprite`) and this firmware's HAL:

- `dhex_gfx_lgfx.cpp` binds a `LGFX_Sprite` to a `dhex_gfx_t` handle and
  implements the drawing calls against it
- `dhex_context.c` implements the context accessors the application needs

Carrying the application to another firmware means supplying those two
files for it, and wrapping it in whatever that firmware calls an
application -- here a mooncake `AppAbility`, see `../app_dhex.cpp`.
