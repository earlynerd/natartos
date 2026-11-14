# Menu icons

Format: `NC01` header (4 bytes magic + u32 LE width + u32 LE height) + RGBA pixel data.

Stock menu icon size: **104×104** (from `bmp_frame_size()` on stock menu items).
File size: 12 + 104×104×4 = **43276 bytes**.

Deploy to `/data/local/tmp/natartos_icon_<name>.rgba` on device.
hook_menu loads them at Reload time (when user opens settings menu).
