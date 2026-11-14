# Stream mod — Research log

## Frame injection to DMA-buf (2026-11-14)

### Problem
Overlay inject thread writes to DMA-buf, but DJI EW UI also writes via push_frame at 60fps.
Single frame gets overwritten instantly — splash screen was invisible.

### Approaches tested

| Approach | How | Result |
|---|---|---|
| Variant A: suppress push_frame (return 0) | `if (g_inject_active) return 0;` | **BREAKS render loop** — UI thread stops calling push_frame permanently |
| Variant B: substitute frame_buf | pass g_frame_buf_copy with data_ptr→g_pixel_buf to g_real_push_frame | **No effect** — g_pixel_buf is anonymous mmap, display controller can't read from it |
| Variant C: memcpy to frame_buf data | overwrite buffer at frame_buf+0x20 with our pixels | **CRASH** — frame_buf+0x20 is a GPU buffer, not writable from CPU |
| Continuous DMA-buf inject | inject thread writes every 16ms (not just on new counter) | **WORKS** — beats UI compositor's 60Hz writes |

### Key findings

1. **frame_buf struct layout** (from debug dump):
   - +0x00..0x18: all zeros
   - +0x20: pointer to GPU buffer (NOT a DMA-buf from /proc/self/maps)
   - +0x28: 0x4E2F (20015 — unknown, not stride which is 0x1E00)
   - +0x30: zero

2. **DMA-bufs in /proc/self/maps are NOT what push_frame writes to.**
   push_frame goes through GPU pipeline → compositor → DMA-buf.
   Direct DMA-buf writes bypass the GPU pipeline.

3. **Continuous 60Hz write wins** because inject thread and UI compositor
   race for the same DMA-buf. With both at ~60fps, inject occupies the buffer
   roughly half the time. Screencap consistently catches inject's frame.

4. **DMA-buf zeroing in cleanup breaks recovery.** After zeroing all 4 DMA-bufs,
   UI compositor doesn't refill them. Removed zeroing from cleanup — last frame
   persists until compositor catches up.

5. **Anti-brick note:** Variant A (return 0) causes 3 rapid restarts → SAFE MODE.
   Always reset bootcount when testing push_frame modifications.

### Working injection flow

```
fb_receiver (stdin JPEG decode) → fb_shared (counter + BGRA)
  → overlay inject thread (pread every 16ms) → DMA-buf × 4
  → display controller reads DMA-buf → goggles screen
```

Inject thread changes (overlay mod.c):
- Removed `counter == g_inject_last_counter` skip — writes every iteration when counter > 0
- This means even a static fb_shared (single frame) fills the display
- For actual streaming, counter increments per frame from fb_receiver
