#!/system/bin/sh
# dump_menu_symbols.sh — dump all Menu/Settings/vmt symbols from dji_glasses
# Run on goggles: sh /data/local/tmp/dump_menu_symbols.sh
# Output: /data/local/tmp/menu_symbols.txt

OUT=/data/local/tmp/menu_symbols.txt
echo "=== Menu symbol dump $(date) ===" > "$OUT"

PID=$(pidof dji_glasses)
if [ -z "$PID" ]; then
    echo "dji_glasses not running" >> "$OUT"
    exit 1
fi
echo "dji_glasses PID=$PID" >> "$OUT"

echo "" >> "$OUT"
echo "=== Loaded libraries ===" >> "$OUT"
cat /proc/$PID/maps | grep '\.so' | awk '{print $6}' | sort -u >> "$OUT"

echo "" >> "$OUT"
echo "=== __vmt symbols (all vtables) ===" >> "$OUT"
for lib in /system/bin/dji_glasses $(cat /proc/$PID/maps | grep '\.so' | awk '{print $6}' | sort -u); do
    [ -f "$lib" ] || continue
    syms=$(readelf -sW "$lib" 2>/dev/null | grep "__vmt" | awk '{print $NF}')
    if [ -n "$syms" ]; then
        echo "--- $lib ---" >> "$OUT"
        echo "$syms" >> "$OUT"
    fi
done

echo "" >> "$OUT"
echo "=== Menu-related symbols ===" >> "$OUT"
for lib in /system/bin/dji_glasses $(cat /proc/$PID/maps | grep '\.so' | awk '{print $6}' | sort -u); do
    [ -f "$lib" ] || continue
    syms=$(readelf -sW "$lib" 2>/dev/null | grep -iE "Menu|Settings|DefineMenu|AddItem|AddSubMenu|PopUp|MorePage|SettingsPage" | awk '{print $NF}')
    if [ -n "$syms" ]; then
        echo "--- $lib ---" >> "$OUT"
        echo "$syms" >> "$OUT"
    fi
done

echo "" >> "$OUT"
echo "=== MenuTree subclass vtables ===" >> "$OUT"
for lib in /system/bin/dji_glasses $(cat /proc/$PID/maps | grep '\.so' | awk '{print $6}' | sort -u); do
    [ -f "$lib" ] || continue
    syms=$(readelf -sW "$lib" 2>/dev/null | grep -i "MenuTree" | awk '{print $NF}')
    if [ -n "$syms" ]; then
        echo "--- $lib ---" >> "$OUT"
        echo "$syms" >> "$OUT"
    fi
done

echo "" >> "$OUT"
echo "=== Done ===" >> "$OUT"
echo "Saved to $OUT"
wc -l "$OUT"
