#!/usr/bin/env bash
# ------------------------------------------------------------------------------
# post2ig.sh — push photo(s)/video(s) to Android and open Instagram composer
#
# Examples:
#   # Single photo → Feed composer
#   ./post2ig.sh ~/Pictures/DSC05510.JPG
#
#   # Multiple photos → carousel (no mixing images/videos)
#   ./post2ig.sh ~/Pics/pic1.jpg ~/Pics/pic2.jpg ~/Pics/pic3.jpg
#
#   # Prefill caption (copied to phone clipboard; paste in IG)
#   ./post2ig.sh -c "Sunset vibes #nofilter" ~/Pictures/DSC05510.JPG
#
#   # Stories composer (photo or video)
#   ./post2ig.sh --stories ~/Videos/clip.mp4
#
#   # Use Android’s share chooser instead of targeting Instagram directly
#   ./post2ig.sh --chooser ~/Pictures/DSC05510.JPG
#
#   # Change remote directory on device
#   ./post2ig.sh -d /sdcard/Pictures ~/Pictures/DSC05510.JPG
#
# Notes:
# - Requires adb (USB or wireless) and Instagram installed on the phone.
# - Final publishing is manual in the Instagram UI (compliant with IG rules).
# - Tested on Android 10–14. If media doesn’t attach, rerun (indexing delay).
# ------------------------------------------------------------------------------

set -euo pipefail

# Defaults (DCIM/Camera indexes reliably across OEMs)
TARGET_DIR="/sdcard/DCIM/Camera"
TARGET_ACTIVITY="com.instagram.android/com.instagram.share.handleractivity.ShareHandlerActivity"  # Feed
USE_CHOOSER=0
STORIES=0
CAPTION=""

usage() {
  cat <<EOF
Usage: $0 [options] <file1> [file2 ...]
Options:
  -d <remote-dir>   Remote directory on device (default: $TARGET_DIR)
  -c <caption>      Prefill caption to phone clipboard
  --stories         Open Instagram Stories composer instead of Feed
  --chooser         Use Android share chooser (don't target Instagram directly)
  -h, --help        Show this help

Rules:
- Supports single or multiple files (carousel). All files must be images OR videos (no mixing).
- Recommended formats: JPG/PNG/WEBP for images, MP4 for videos.
EOF
}

# --- Parse args ---
ARGS=()
while (( $# )); do
  case "${1:-}" in
    -d) TARGET_DIR="${2:?Missing value for -d}"; shift 2;;
    -c) CAPTION="${2:-}"; shift 2;;
    --stories) STORIES=1; shift;;
    --chooser) USE_CHOOSER=1; shift;;
    -h|--help) usage; exit 0;;
    --) shift; break;;
    -*) echo "Unknown option: $1" >&2; usage; exit 2;;
    *)  ARGS+=("$1"); shift;;
  esac
done
FILES=("${ARGS[@]}")

if [[ ${#FILES[@]} -eq 0 ]]; then usage; exit 2; fi

# --- Basic checks ---
# TODO ip="192.168.10.102"; for port in $(seq 37000 42000); do nc -z -w1 $ip $port 2>/dev/null && echo "Found open port: $port" && adb connect $ip:$port && break; done
if ! adb get-state >/dev/null 2>&1; then
  echo "adb isn't connected. Run 'adb devices' and ensure your phone shows up." >&2
  exit 1
fi

if (( STORIES )); then
  TARGET_ACTIVITY="com.instagram.android/com.instagram.share.handleractivity.StoryShareHandlerActivity"
fi

# --- Helpers ---

ext_lower() { local f="$1"; f="${f##*.}"; echo "${f,,}"; }

guess_mime() {
  case "$(ext_lower "$1")" in
    jpg|jpeg) echo "image/jpeg" ;;
    png)      echo "image/png" ;;
    webp)     echo "image/webp" ;;
    mp4)      echo "video/mp4" ;;
    mov)      echo "video/quicktime" ;;
    *)        echo "application/octet-stream" ;;
  esac
}

push_and_scan() {
  local src="$1" base remote
  base="$(basename "$src")"
  remote="$TARGET_DIR/$base"
  adb shell mkdir -p "$TARGET_DIR" >/dev/null 2>&1 || true
  adb push "$src" "$TARGET_DIR/" >/dev/null
  adb shell am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d "file://$remote" >/dev/null
  sleep 0.4   # give the indexer a moment
  echo "$remote"
}

# Convert /sdcard/XYZ → LIKE '%/XYZ' to match MediaStore absolute path (/storage/emulated/0/XYZ)
resolve_content_uri() {
  local remote="$1" id
  local tail="${remote#/sdcard/}"     # e.g. "DCIM/Camera/IMG_1234.jpg"
  local like="%/${tail}"              # e.g. "%/DCIM/Camera/IMG_1234.jpg"

  # Try 'external' first
  id="$(
    adb shell "content query --uri content://media/external/file \
      --projection _id:_display_name:_data:media_type \
      --where \"_data LIKE '$like'\" \
      --sort 'date_added DESC'" \
    | sed -n 's/.*_id=\([0-9]\+\).*/\1/p' | head -n1 || true
  )"
  if [[ -n "$id" ]]; then
    echo "content://media/external/file/$id"; return 0
  fi
  # Fallback: external_primary
  id="$(
    adb shell "content query --uri content://media/external_primary/file \
      --projection _id:_display_name:_data:media_type \
      --where \"_data LIKE '$like'\" \
      --sort 'date_added DESC'" \
    | sed -n 's/.*_id=\([0-9]\+\).*/\1/p' | head -n1 || true
  )"
  if [[ -n "$id" ]]; then
    echo "content://media/external_primary/file/$id"; return 0
  fi
  return 1
}

# --- Main ---

# Validate inputs and media type homogeneity
for f in "${FILES[@]}"; do
  [[ -f "$f" ]] || { echo "Missing file: $f" >&2; exit 2; }
done

first_mime="$(guess_mime "${FILES[0]}")"
media_kind=""
[[ "$first_mime" == image/* ]] && media_kind="image"
[[ "$first_mime" == video/* ]] && media_kind="video"
[[ -z "$media_kind" ]] && { echo "Unsupported file type: ${FILES[0]}" >&2; exit 2; }

for f in "${FILES[@]}"; do
  m="$(guess_mime "$f")"
  if [[ "$media_kind" == "image" && "$m" != image/* ]]; then
    echo "Cannot mix images and videos in one post." >&2; exit 2
  fi
  if [[ "$media_kind" == "video" && "$m" != video/* ]]; then
    echo "Cannot mix videos and images in one post." >&2; exit 2
  fi
done

mime="$first_mime"
[[ ${#FILES[@]} -gt 1 ]] && mime="${media_kind}/*"

# Optional caption → device clipboard
if [[ -n "$CAPTION" ]]; then
  adb shell cmd clipboard set "$CAPTION" >/dev/null 2>&1 || true
fi

declare -a URIS
for src in "${FILES[@]}"; do
  remote="$(push_and_scan "$src")"
  uri="$(resolve_content_uri "$remote" || true)"
  if [[ -z "$uri" ]]; then
    echo "Could not resolve content URI for $remote. Open it once in Gallery and retry." >&2
    exit 3
  fi
  URIS+=("$uri")
done

ACTION="android.intent.action.SEND"
[[ ${#URIS[@]} -gt 1 ]] && ACTION="android.intent.action.SEND_MULTIPLE"

# Target app/component
COMPONENT_ARGS=()
if (( USE_CHOOSER == 0 )); then
  COMPONENT_ARGS=( -n "$TARGET_ACTIVITY" )
fi

# Build and launch intent
CMD=( adb shell am start "${COMPONENT_ARGS[@]}" -a "$ACTION" -t "$mime" )
if [[ "$ACTION" == "android.intent.action.SEND" ]]; then
  CMD+=( --eu android.intent.extra.STREAM "${URIS[0]}" )
else
  for u in "${URIS[@]}"; do CMD+=( --eu android.intent.extra.STREAM "$u" ); done
fi
CMD+=( --grant-read-uri-permission )

echo "Launching Instagram composer…"
"${CMD[@]}"

echo "Done. If Instagram opens without media, wait a second and rerun (some OEMs index slowly)."
echo "Tip: if you used -c, your caption is on the phone clipboard — paste it in Instagram."
