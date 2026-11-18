# Debug log

## 2026-11-18 — Remove input dependency from unattended boot

- **Observation:** the first enforcing cold boot produced a fresh GPS proxy
  marker and a normal display, but the qualifier logged `startup bypass`
  before its read-only permission probe.
- **Root cause:** `src/boot_activate/supervisor.c:303` at `80993d9` made
  `key_bypass()` a mandatory precondition for the probe. That function at line
  103 combined input-access failures and BACK state into the same result; the
  record did not establish that a button was held. Input state was an unwanted
  dependency for unattended qualification and activation.
- **Fix:** remove input-device checks from both modes; retain persistent
  enable/disable files, profile checks and automatic failure handling. Route
  profile failures to the bounded boot log for diagnosis.
- **Class:** unnecessary-startup-dependency
- **Recently-touched?** yes
- **Validation:** 32 supervision/handoff scenarios and the profile matrix passed
  on ARM64 and x86-64. Both boot binaries pass an audit excluding input-device
  paths. The corrected read-only helper passed a live warm check; enforcing
  cold qualification is being repeated.
