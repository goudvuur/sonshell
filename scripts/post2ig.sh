#!/usr/bin/env bash
# ------------------------------------------------------------------------------
# post2ig.sh — copy media to Android over SSH/Termux and post an IG launcher
# notification.
#
# Workflow:
#   1. Media files are pushed to the phone (default /sdcard/DCIM/Camera).
#   2. The files are indexed so they appear in Instagram's picker.
#   3. A high-priority Termux notification is posted; tapping it opens Instagram.
#
# Requirements:
# - Termux with the `openssh` and `termux-api` packages installed.
# - Termux SSH server reachable from this machine.
# - Instagram installed on the phone (you publish manually).
#
# Examples:
#   TERMUX_HOST=192.168.1.42 ./post2ig.sh ~/Pictures/IMG_0001.JPG
#   ./post2ig.sh --host phone.local --caption "Sunset vibes" img1.jpg img2.jpg
#   ./post2ig.sh -H 192.168.1.42 -d /sdcard/Pictures share.png
# ------------------------------------------------------------------------------

set -euo pipefail

echo "[debug] post2ig start" >&2

# Defaults
TARGET_DIR="/sdcard/DCIM/Camera"
CAPTION=""

# SSH/Termux defaults
SSH_HOST="${TERMUX_HOST:-}"
SSH_USER="${TERMUX_USER:-termux}"
SSH_PORT="${TERMUX_PORT:-8022}"
SSH_BIN="${SSH_BIN:-ssh}"
SCP_BIN="${SCP_BIN:-scp}"
declare -a SSH_EXTRA_OPTS=()
declare -a SCP_EXTRA_OPTS=()
CMD_BIN="${CMD_BIN:-}"
AM_BIN="${AM_BIN:-}"
TERMUX_MEDIA_SCAN_BIN="${TERMUX_MEDIA_SCAN_BIN:-}"
TERMUX_NOTIFICATION_BIN="${TERMUX_NOTIFICATION_BIN:-}"
LAST_URI=""
SHOW_SCP_PROGRESS="${SHOW_SCP_PROGRESS:-1}"

usage() {
  cat <<EOF
Usage: $0 [options] <file1> [file2 ...]
Options:
  -d <remote-dir>   Remote directory on device (default: $TARGET_DIR)
  -c <caption>      Prefill caption to the device clipboard
  -H, --host <host> Termux SSH host (default: TERMUX_HOST env var)
  -U, --user <user> Termux SSH user (default: ${SSH_USER:-termux})
  -P, --port <port> Termux SSH port (default: ${SSH_PORT:-8022})
  --ssh-option <o>  Extra option passed to ssh/scp (may be repeated)
  -h, --help        Show this help

Rules:
- Accepts one or more media files (all must be images OR all videos).
- Recommended formats: JPG/PNG/WEBP for images, MP4 for videos.
- Termux must have storage access (run \`termux-setup-storage\` once).
EOF
}

die() {
  echo "$*" >&2
  exit 1
}

q() {
  printf '%q' "$1"
}

shell_quote_single() {
  local s="$1"
  printf "'%s'" "${s//\'/\'\\\'\'}"
}

remote_eval() {
  local script="$1"
  "$SSH_BIN" "${SSH_OPTS[@]}" "$SSH_DEST" "bash -lc $(printf %q "$script")"
}

remote_capture() {
  local script="$1"
  remote_eval "$script"
}

remote_command_path() {
  local name="$1"
  remote_capture "command -v $(q "$name") 2>/dev/null || true" | tr -d '\r' | head -n1
}

ensure_remote_executable() {
  local path="$1"
  remote_eval "[[ -x $(q "$path") ]]" >/dev/null 2>&1
}

remote_clipboard_set() {
  local text="$1"
  local payload
  payload=$(printf %q "$text")
  remote_eval "termux-clipboard-set $payload" >/dev/null 2>&1 || \
    remote_eval "$(q "$CMD_BIN") clipboard set $payload" >/dev/null 2>&1 || true
}

push_and_index() {
  local src="$1" base remote dir_escaped
  LAST_URI=""
  base="$(basename "$src")"
  remote="$TARGET_DIR/$base"
  remote_eval "mkdir -p $(q "$TARGET_DIR")" >/dev/null 2>&1 || true

  local remote_target
  printf -v remote_target '%s:%q' "$SSH_DEST" "$TARGET_DIR/"

  local -a scp_cmd=("$SCP_BIN")
  scp_cmd+=("${SCP_OPTS[@]}")
  scp_cmd+=("--" "$src" "$remote_target")
  "${scp_cmd[@]}" >/dev/null

  remote_eval "$(q "$AM_BIN") broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d $(q "file://$remote")" >/dev/null 2>&1 || true

  local uri=""
  if [[ -n "$TERMUX_MEDIA_SCAN_BIN" ]]; then
    local scan_cmd="$TERMUX_MEDIA_SCAN_BIN -p $(printf %q "$remote")"
    local scan_output
    scan_output="$(remote_capture "$scan_cmd" || true)"
    uri="$(printf '%s\n' "$scan_output" | tr -d '\r' | sed -n 's/.*"uri":"\([^"\\]*\(\\\/[^"\\]*\)*\)".*/\1/p' | head -n1 || true)"
    uri="${uri//\\\//\/}"
  fi
  if [[ -z "$uri" ]]; then
    uri="$(document_uri_from_remote "$remote" || true)"
  fi
  LAST_URI="$uri"

  sleep 0.4
  echo "$remote"
}

post_notification() {
  local uris_name="$1"
  local mime="$2"
  local -a uris_ref_copy=()
  if [[ -n "$uris_name" ]] && declare -p "$uris_name" >/dev/null 2>&1; then
    eval "uris_ref_copy=(\"\${${uris_name}[@]}\")"
  fi

  local default_action
  if (( ${#uris_ref_copy[@]} )); then
    local share_cmd
    share_cmd="$(build_instagram_share_command uris_ref_copy "$mime")"
    default_action="bash -lc $(shell_quote_single "$share_cmd")"
  else
    local launch_cmd
    launch_cmd="$AM_BIN start -n com.instagram.android/.activity.MainTabActivity"
    default_action="bash -lc $(shell_quote_single "$launch_cmd")"
  fi

  local id="${NOTIFY_ID:-iglaunch}"
  local title="${NOTIFY_TITLE:-Instagram}"
  local content="${NOTIFY_CONTENT:-Tap to open}"
  local priority="${NOTIFY_PRIORITY:-high}"
  local action="${NOTIFY_ACTION:-$default_action}"

  local -a parts=("$TERMUX_NOTIFICATION_BIN" --id "$id" --title "$title" --content "$content" --priority "$priority" --action "$action")
  local cmd=""
  for arg in "${parts[@]}"; do
    cmd+=" $(printf %q "$arg")"
  done
  cmd="${cmd# }"
  remote_eval "$cmd"
}

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

url_encode_component() {
  local s="$1" out="" c
  for (( i=0; i<${#s}; i++ )); do
    c="${s:i:1}"
    case "$c" in
      [a-zA-Z0-9._~-]) out+="$c";;
      *) printf -v out '%s%%%02X' "$out" "'$c";;
    esac
  done
  printf '%s\n' "$out"
}

document_uri_from_remote() {
  local remote="$1" tail doc_id encoded
  tail="${remote#/sdcard/}"
  if [[ "$tail" == "$remote" ]]; then
    return 1
  fi
  doc_id="primary:${tail}"
  encoded="$(url_encode_component "$doc_id")"
  printf 'content://com.android.externalstorage.documents/document/%s\n' "$encoded"
}

build_instagram_share_command() {
  local -n uris_ref="$1"
  local mime="$2"
  local action="android.intent.action.SEND"
  if (( ${#uris_ref[@]} > 1 )); then
    action="android.intent.action.SEND_MULTIPLE"
  fi

  local -a parts=("$AM_BIN" "start" "-n" "com.instagram.android/com.instagram.share.handleractivity.ShareHandlerActivity" "-a" "$action" "-t" "$mime")
  if (( ${#uris_ref[@]} == 1 )); then
    parts+=("--eu" "android.intent.extra.STREAM" "${uris_ref[0]}")
  else
    local uri
    for uri in "${uris_ref[@]}"; do
      parts+=("--eu" "android.intent.extra.STREAM" "$uri")
    done
  fi
  parts+=("--grant-read-uri-permission")

  local cmd=""
  local arg
  for arg in "${parts[@]}"; do
    cmd+=" $(printf %q "$arg")"
  done
  printf '%s\n' "${cmd# }"
}

# --- Parse args ---
ARGS=()
while (( $# )); do
  case "${1:-}" in
    -d) TARGET_DIR="${2:?Missing value for -d}"; shift 2;;
    -c) CAPTION="${2:-}"; shift 2;;
    -H|--host) SSH_HOST="${2:?Missing value for --host}"; shift 2;;
    -U|--user) SSH_USER="${2:?Missing value for --user}"; shift 2;;
    -P|--port) SSH_PORT="${2:?Missing value for --port}"; shift 2;;
    --ssh-option)
      [[ -n "${2:-}" ]] || die "Missing value for --ssh-option"
      SSH_EXTRA_OPTS+=("${2}")
      SCP_EXTRA_OPTS+=("${2}")
      shift 2
      ;;
    -h|--help) usage; exit 0;;
    --) shift; break;;
    -*) echo "Unknown option: $1" >&2; usage; exit 2;;
    *)  ARGS+=("$1"); shift;;
  esac
done
FILES=("${ARGS[@]}")

if [[ ${#FILES[@]} -eq 0 ]]; then usage; exit 2; fi

[[ -n "$SSH_HOST" ]] || die "Termux host not specified. Use --host or set TERMUX_HOST."

echo "[debug] parsed args: host=$SSH_HOST user=$SSH_USER port=$SSH_PORT files=${#FILES[@]}" >&2

SSH_DEST="${SSH_USER}@${SSH_HOST}"
declare -a SSH_OPTS=()
declare -a SCP_OPTS=()
if [[ -n "$SSH_PORT" ]]; then
  SSH_OPTS+=(-p "$SSH_PORT")
  SCP_OPTS+=(-P "$SSH_PORT")
fi
SSH_OPTS+=(-o BatchMode=yes)
SSH_OPTS+=("${SSH_EXTRA_OPTS[@]}")
SCP_OPTS+=("${SCP_EXTRA_OPTS[@]}")

command -v "$SSH_BIN" >/dev/null 2>&1 || die "ssh command not found: $SSH_BIN"
command -v "$SCP_BIN" >/dev/null 2>&1 || die "scp command not found: $SCP_BIN"

if ! "$SSH_BIN" "${SSH_OPTS[@]}" "$SSH_DEST" true >/dev/null 2>&1; then
  die "Unable to connect to $SSH_DEST via SSH. Ensure the Termux SSH server is running."
fi

echo "[debug] ssh connectivity check ok" >&2

if [[ -z "$CMD_BIN" ]]; then
  CMD_BIN="$(remote_command_path cmd)"
  [[ -n "$CMD_BIN" ]] || CMD_BIN="/system/bin/cmd"
fi
ensure_remote_executable "$CMD_BIN" || die "Cannot execute 'cmd' at $CMD_BIN. Set CMD_BIN to the correct absolute path (e.g. /system/bin/cmd)."

if [[ -z "$AM_BIN" ]]; then
  AM_BIN="$(remote_command_path am)"
  [[ -n "$AM_BIN" ]] || AM_BIN="/system/bin/am"
fi
ensure_remote_executable "$AM_BIN" || die "Cannot execute 'am' at $AM_BIN. Set AM_BIN to the correct absolute path (e.g. /system/bin/am)."

if [[ -z "$TERMUX_MEDIA_SCAN_BIN" ]]; then
  TERMUX_MEDIA_SCAN_BIN="$(remote_command_path termux-media-scan)"
fi
if [[ -z "$TERMUX_MEDIA_SCAN_BIN" ]]; then
  echo "termux-media-scan not found on device; falling back to broadcast-only indexing." >&2
fi

if [[ -z "$TERMUX_NOTIFICATION_BIN" ]]; then
  TERMUX_NOTIFICATION_BIN="$(remote_command_path termux-notification)"
fi
[[ -n "$TERMUX_NOTIFICATION_BIN" ]] || die "termux-notification not found on device. Install termux-api."
ensure_remote_executable "$TERMUX_NOTIFICATION_BIN" || die "termux-notification at $TERMUX_NOTIFICATION_BIN is not executable."

echo "[debug] remote command discovery complete" >&2

# --- Input validation ---

# Validate inputs and enforce media homogeneity
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
    echo "Cannot mix images and videos in one run." >&2; exit 2
  fi
  if [[ "$media_kind" == "video" && "$m" != video/* ]]; then
    echo "Cannot mix videos and images in one run." >&2; exit 2
  fi
done

MIME_TYPE="$first_mime"
if [[ ${#FILES[@]} -gt 1 ]]; then
  MIME_TYPE="${media_kind}/*"
fi

if [[ -n "$CAPTION" ]]; then
  remote_clipboard_set "$CAPTION"
fi

declare -a REMOTE_FILES
declare -a URIS
for src in "${FILES[@]}"; do
  echo "[debug] pushing $src" >&2
  remote_path="$(push_and_index "$src")"
  REMOTE_FILES+=("$remote_path")
  if [[ -n "$LAST_URI" ]]; then
    URIS+=("$LAST_URI")
  fi
done

post_notification URIS "$MIME_TYPE"

echo "[debug] notification posted" >&2
for remote_path in "${REMOTE_FILES[@]}"; do
  echo "[debug] uploaded to $remote_path" >&2
done
if [[ -n "$CAPTION" ]]; then
  printf 'Caption copied to phone clipboard.\n'
fi
