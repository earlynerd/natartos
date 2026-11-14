#!/bin/bash
# Build natartos: loader + mods in Docker (no toolchain on NAS)
# Outputs: dist/ with .so files, manifest.json, SHA256SUMS
#
# Sources are mounted read-only; .so output goes directly to dist/
# to avoid Docker root-owned files and Makefile clobbering in mods/.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BASE_DIR="$SCRIPT_DIR/.."
SRC_DIR="$BASE_DIR/src"
MODS_DIR="$BASE_DIR/mods"
DIST_DIR="$BASE_DIR/dist"
DEPLOY_DIR="$BASE_DIR/deploy"

# Clean dist
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

CC="aarch64-linux-gnu-gcc"
CFLAGS="-shared -fPIC -O2 -fno-builtin -nostdlib"

# Cross-compile in Docker: sources RO, output to /dist
docker run --rm \
  -v "$SRC_DIR:/src:ro" \
  -v "$MODS_DIR:/mods:ro" \
  -v "$BASE_DIR/include:/include:ro" \
  -v "$DIST_DIR:/dist" \
  --platform linux/amd64 gcc:13 bash -c "
  apt-get update -qq && apt-get install -y -qq gcc-aarch64-linux-gnu >/dev/null 2>&1

  echo '=== Building loader ==='
  $CC $CFLAGS -I/include -Wl,-soname,natartos_loader.so -o /dist/natartos_loader.so /src/natartos_loader.c
  echo \"[natartos] natartos_loader.so (\$(stat -c%s /dist/natartos_loader.so) bytes)\"

  echo '=== Building mods ==='
  for src in /mods/*/mod.c; do
    dir=\$(basename \$(dirname \"\$src\"))
    # .so name from mod.json if present, else from dir name
    if [ -f /mods/\$dir/mod.json ]; then
      if grep -q '\"so\".*null' /mods/\$dir/mod.json; then
        echo \"[skip] \$dir (so=null)\"; continue
      fi
      so=\$(grep '\"so\"' /mods/\$dir/mod.json | head -1 | sed 's/.*\"so\"[^\"]*\"\\([^\"]*\\)\".*/\\1/')
    fi
    [ -z \"\$so\" ] && so=\"\${dir}.so\"
    # Mods with \"libc\":true need libc + pthread (e.g. stream)
    if grep -q '\"libc\".*true' /mods/\$dir/mod.json 2>/dev/null; then
      MODFLAGS=\"-shared -fPIC -O2 -lpthread -lm\"
    else
      MODFLAGS=\"$CFLAGS\"
    fi
    $CC \$MODFLAGS -I/include -I/mods -Wl,-soname,\$so -o /dist/\$so \"\$src\"
    echo \"[natartos] \$so (\$(stat -c%s /dist/\$so) bytes)\"
    so=
  done

  chmod 644 /dist/*.so

  echo '=== Building binaries ==='
  # fb_receiver: static aarch64 binary for PC streaming
  if [ -f /mods/stream/fb_receiver.c ]; then
    $CC -static -O2 -I/include -o /dist/fb_receiver /mods/stream/fb_receiver.c -lm
    chmod 755 /dist/fb_receiver
    echo \"[natartos] fb_receiver (\$(stat -c%s /dist/fb_receiver) bytes)\"
  fi
"

# Copy deploy scripts to dist (they go into the release too)
cp "$DEPLOY_DIR/inject.sh" "$DIST_DIR/"
cp "$DEPLOY_DIR/adb_persist.sh" "$DIST_DIR/"
cp "$DEPLOY_DIR/natartos.rc" "$DIST_DIR/"

# Generate SHA256SUMS
cd "$DIST_DIR"
sha256sum *.so inject.sh adb_persist.sh natartos.rc > SHA256SUMS

# --- Generate manifest.json (schema 1, toolya contract) ---
VERSION="${NATARTOS_VERSION:-dev}"

# Read init_rc content from natartos.rc (escape for JSON)
RC_CONTENT=$(python3 -c "
import json, sys
with open('natartos.rc') as f:
    print(json.dumps(f.read()), end='')
")

# Build mods array from mod.json + compiled .so in dist/
# Maps: provides (screen=hook_debug, background=background, splash=splash)
# default: hook_debug (health signal)
MODS_JSON=$(python3 -c "
import json, os, glob
mods_dir = '$MODS_DIR'
dist_dir = '.'
entries = []
for d in sorted(glob.glob(os.path.join(mods_dir, '*/mod.json'))):
    with open(d) as f:
        m = json.load(f)
    so = m.get('so')
    if so is None:
        continue
    if not os.path.isfile(os.path.join(dist_dir, so)):
        continue
    import hashlib
    with open(os.path.join(dist_dir, so), 'rb') as sf:
        sha = hashlib.sha256(sf.read()).hexdigest()
    # provides mapping
    provides_map = {
        'overlay': 'screen',
        'background': 'background',
        'splash': 'splash',
    }
    entry = {
        'id': m['id'],
        'name': m.get('name', m['id']),
        'description': m.get('notes', ''),
        'provides': provides_map.get(m['id'], None),
        'default': m['id'] == 'overlay',
        'file': so,
        'device_path': '/vendor/natartos/mods/' + so,
        'mode': '0644',
        'sha256': sha,
    }
    # strip None values
    entry = {k: v for k, v in entry.items() if v is not None}
    entries.append(entry)
print(json.dumps(entries, indent=4))
")

LOADER_SHA=$(sha256sum natartos_loader.so | cut -d' ' -f1)
INJECT_SHA=$(sha256sum inject.sh | cut -d' ' -f1)
ADB_SHA=$(sha256sum adb_persist.sh | cut -d' ' -f1)

RC_CONTENT="$RC_CONTENT" MODS_JSON="$MODS_JSON" \
LOADER_SHA="$LOADER_SHA" INJECT_SHA="$INJECT_SHA" ADB_SHA="$ADB_SHA" \
VERSION="$VERSION" \
python3 -c '
import json, os
manifest = {
    "schema": 1,
    "natartos_version": os.environ["VERSION"],
    "device": "E3T zv902",
    "init_rc": {
        "device_path": "/vendor/etc/init/natartos.rc",
        "content": json.loads(os.environ["RC_CONTENT"]),
    },
    "loader": {
        "file": "natartos_loader.so",
        "device_path": "/vendor/lib64/natartos_loader.so",
        "mode": "0644",
        "sha256": os.environ["LOADER_SHA"],
    },
    "files": [
        {
            "file": "inject.sh",
            "device_path": "/vendor/natartos/inject.sh",
            "mode": "0755",
            "sha256": os.environ["INJECT_SHA"],
        },
        {
            "file": "adb_persist.sh",
            "device_path": "/vendor/natartos/adb_persist.sh",
            "mode": "0755",
            "sha256": os.environ["ADB_SHA"],
        },
    ],
    "mods": json.loads(os.environ["MODS_JSON"]),
    "backup": [
        "/vendor/etc/init/dji_glasses_custom.rc",
        "/vendor/etc/selinux/precompiled_sepolicy",
    ],
    "remove": [
        "/vendor/etc/init/persist_adb.rc",
        "/vendor/lib64/hook_debug.so",
        "/vendor/lib64/hook_bitmap.so",
    ],
}
with open("manifest.json", "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
print(json.dumps(manifest, indent=2))
'

cd "$BASE_DIR"
echo ""
echo "=== dist/ ==="
ls -la "$DIST_DIR"
