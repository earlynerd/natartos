# Decision Log

Forward-facing, append-only record of architectural and behavioral decisions.
Newer applicable entries supersede older decisions.

## 2026-11-17 — Keep the boot marker experiment separate from activation

- **Decision:** develop a getter-forwarding GPS proxy on `codex/boot-marker-probe`,
  based on upstream `master` revision `82f1a7d`. Its constructor may append one
  bounded diagnostic record to an existing marker; the stock getter's argument
  and returned interface remain unchanged. Keep it outside release packaging
  and installation flows.
- **Why:** establish execution and its actual security context before designing
  unattended launcher activation. This experiment does not start Natartos,
  change SELinux, access physical memory or provide a generic HAL replacement.
  Original-library availability and real GPS behavior need separate bench checks.
- **Affects:** `src/boot_probe/`, its authored fixtures, build/test tools and
  `docs/BOOT_MARKER_PROBE.md`. Device deployment is outside this change.
