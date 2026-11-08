# android-common/tests

Host-side tests for headers shared by the Android capture plugins.

## Why a mock `obs.h` lives here

`mock/` contains a **stub** `obs.h` / `util/platform.h` / `util/threading.h` so that
`reader-ctx.h` can be compiled and exercised on a build host without linking libobs.
It is only on the include path of `scripts/test-android-reader-ctx.ps1`, and that
script passes `-I obs-studio/plugins/android-common/tests/mock` **before** any real
libobs include directory, so the stub always wins inside the test.

Do not add this directory to any production include path.

## What the stub has to get right

The mock is deliberately not a no-op: it reproduces the refcount semantics that
`reader-ctx.h` depends on, taken from the real implementation:

| Behaviour | Real source |
| --- | --- |
| `obs_weak_source_t` and `obs_source_t` share one allocation (`get_weak()` == `context.control`) | `obs-source.c:34`, `obs-internal.h:618-626`, `:779-782` |
| `refs` / `weak_refs` start at 0 (`bzalloc`) | `obs.c:2697` |
| `obs_ref_release()` returns true when `refs` decrements to `-1` | `obs-internal.h:685-688` |
| `obs_weak_source_get_source()` returns NULL once `refs < 0` | `obs-internal.h:700-710` |

If the real semantics change, update the mock in the same commit -- otherwise the
test silently stops proving anything.

## Covered invariants

`reader-ctx-test.c` asserts the four properties the capture plugins rely on:

1. `reader_ctx_enter()` fails after `reader_ctx_retire()` (no new callback may enter
   once shutdown starts -- that is the double-capture entry point).
2. `reader_ctx_wait_idle(ctx, 0)` reports false while a callback is still inside the
   work area, and true after it leaves.
3. `reader_ctx_source()` returns NULL once the source has been destroyed -- this is
   the direct regression for the `c->source` use-after-free.
4. `reader_ctx_set_owner(ctx, NULL)` makes `reader_ctx_owner()` return NULL.

`scripts/test-android-reader-ctx.ps1` additionally asserts a **source-order**
invariant that a unit test cannot catch: `reader_ctx_create()` must appear *after*
`AImageReader_new()` inside `open_capture_locked()` / `open_capture()`. Creating the
context before the reader leaves a fresh, un-retired context behind on any early
failure path, which makes the next `wait_idle()` pass unconditionally and disables
the shutdown gate.