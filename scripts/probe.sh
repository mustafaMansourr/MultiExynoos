#!/system/bin/sh
# MultiExynoos Phase-0 probe — run ON DEVICE (adb shell sh < probe.sh).
# Captures: kernel, GPU id, DRM nodes, stock Vulkan extension surface.
# Output: multiexynoos-probe-<date>.txt in the app/cwd. No writes outside cwd.
OUT="multiexynoos-probe-$(date +%Y%m%d-%H%M%S).txt"
{
  echo "== date =="; date
  echo "== kernel =="; uname -a
  echo "== soc/gpu props =="; getprop ro.board.platform; getprop ro.hardware; getprop ro.soc.model
  echo "== gpu id (sysfs) =="
  for d in /sys/class/drm/renderD*/device; do
    echo "-- $d"; cat "$d/gpu_id" 2>/dev/null; cat "$d/uevent" 2>/dev/null | head -20
  done
  echo "== dri nodes =="; ls -l /dev/dri 2>/dev/null || echo "no /dev/dri (SELinux?)"
  echo "== vulkaninfo extensions =="
  if command -v vulkaninfo >/dev/null 2>&1; then
    vulkaninfo --summary 2>&1
  else
    echo "vulkaninfo not on device — pull extension list from the emulator container instead"
  fi
} > "$OUT" 2>&1
echo "wrote $OUT"
