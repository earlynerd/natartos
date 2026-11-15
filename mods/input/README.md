# mod_input — programmatic key injection

Injects key/touch events into the DJI EW (Embedded Wizard) UI by calling
`CoreRoot_DriveKeyboardHitting`. Driven by signal 36 + a command file
(`/data/local/tmp/natartos_input_cmd`, e.g. `key 19`). EW key codes:
UP=1 DOWN=2 LEFT=3 RIGHT=4 OK=6 MENU=18 EXIT=19 POWER=20.

See [[input-key-injection]] memory for the shell recipe.

## ⚠️ Danger (current implementation)

**Do not treat the current mod as safe.** `input_worker` (a pthread worker)
calls `CoreRoot_DriveKeyboardHitting` from a **background thread**. EW is a
**single-threaded** UI framework running on dji_glasses' UI thread, so a
cross-thread call races it and can corrupt EW state → the whole system hangs
(black screen, no input, no sound, ADB drops). Observed in practice.

`find_root()` also *guesses* the root object by scanning `/proc/self/mem` for
the `__vmt_DuFPVApplication` vtable pointer; a wrong/stale hit dereferences
garbage.

## TODO — make it a safe, reusable input API

Goal: let **other mods** request button presses, and expose the same from the
**web UI** (toolya) for automated UI testing.

1. **Inject on the UI thread, not a worker.** Deliver the key from within EW's
   own event loop (hook a per-frame / idle callback that runs on the UI thread),
   so the `DriveKeyboardHitting` call never races the renderer. This is the fix
   that removes the hang.
2. **Reliable root.** Get the root from EW itself (an exported accessor / a hook
   that captures `self` on a known EW call) instead of the heap scan.
3. **Exported API for mods.** `natartos_input_press(int ew_key)` (and a
   held/released variant) that other mods (menu, overlay exit, etc.) can call —
   e.g. overlay could repaint-kick the UI on app exit *safely* this way.
4. **Web + autotest.** Surface it over ADB (a tiny command protocol or the
   existing signal+file) so toolya web can simulate button presses — scripted UI
   walkthroughs and automated regression tests of the on-device UI.
