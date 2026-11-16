# Decision Log

Forward-facing, append-only record of architectural and behavioral decisions
for this fork. Newer applicable entries supersede older decisions.

## 2026-11-16 — Start SD launching with a standalone static runner

- **Decision:** implement package validation, verified internal staging and
  process supervision outside `dji_glasses`, using static ARM64 executables
  and an explicit versioned JSON manifest. Keep saves separate from the
  disposable cache; use direct argv execution and a minimal environment.
- **Why:** prove SD-to-internal execution and cleanup before taking display
  or input ownership. The first Hello sample is a console proof. Menu/picker,
  graphical handoff, installer integration and optional boot activation remain
  separate follow-up work. Initial activation remains phone-assisted.
- **Affects:** `mods/sd_apps/`, `examples/sd_apps/hello/`, build/test helpers,
  and `docs/SD_APP_RUNNER.md`. No security toggles or firmware writes are part
  of this runner. Device execution is still pending validation.
