#!/bin/bash
# -----------------------------------------------------------------------------
# show_single.sh
#
# This script opens an image file with your desktop's *default image viewer*,
# but unlike plain `xdg-open`, it does so in a way that:
#   - Tracks the viewer process (PID) so it can be killed next time.
#   - Ensures only images launched by *this script* are closed, not unrelated
#     viewer windows.
#   - Works dynamically: it queries the default app via `xdg-mime` and parses
#     its `.desktop` file instead of hardcoding a viewer.
#   - Adds an environment tag (LAUNCHED_BY_<SCRIPTNAME>) to the child process
#     for easier debugging.
#
# The reason the script is so extended is because `xdg-open` itself detaches
# and doesn’t give you the viewer’s PID. To manage "kill previous / show next"
# behavior safely, we need to resolve the default viewer command ourselves.
# -----------------------------------------------------------------------------

set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $(basename "$0") <image-file>" >&2
  exit 1
fi

FILE=$1
if [[ ! -e "$FILE" ]]; then
  echo "No such file: $FILE" >&2
  exit 1
fi

SCRIPT_NAME=$(basename "$0")
PIDFILE="/tmp/${SCRIPT_NAME}.pid"

# --- Kill previously launched viewer from this script (if still running) ---
if [[ -f "$PIDFILE" ]]; then
  oldpid=$(cat "$PIDFILE")
  if kill -0 "$oldpid" 2>/dev/null; then
    kill "$oldpid" 2>/dev/null || true
    # If it lingers, wait briefly, then SIGKILL
    for _ in {1..20}; do
      kill -0 "$oldpid" 2>/dev/null || break
      sleep 0.05
    done
    kill -9 "$oldpid" 2>/dev/null || true
  fi
  rm -f "$PIDFILE"
fi

# --- Resolve default viewer .desktop for the file's MIME type ---
mime=$(file --mime-type -Lb -- "$FILE")

desktop_id=$(xdg-mime query default "$mime" || true)
if [[ -z "${desktop_id:-}" ]]; then
  echo "No default application found for MIME type: $mime" >&2
  exit 1
fi

# Locate the .desktop file
desktop_file=""
for d in "$HOME/.local/share/applications" /usr/local/share/applications /usr/share/applications; do
  if [[ -r "$d/$desktop_id" ]]; then
    desktop_file="$d/$desktop_id"
    break
  fi
done

if [[ -z "$desktop_file" ]]; then
  echo "Could not locate desktop file for: $desktop_id" >&2
  exit 1
fi

# --- Extract and adapt the Exec= line ---
exec_line=$(awk -F= '$1=="Exec"{print substr($0,index($0,$2))}' "$desktop_file" | head -n1)
if [[ -z "$exec_line" ]]; then
  echo "No Exec= line found in $desktop_file" >&2
  exit 1
fi

cmd="$exec_line"
file_quoted=$(printf '%q' "$FILE")

cmd="${cmd//%F/$file_quoted}"
cmd="${cmd//%U/$file_quoted}"
cmd="${cmd//%f/$file_quoted}"
cmd="${cmd//%u/$file_quoted}"
cmd="${cmd//%i/}"
cmd="${cmd//%c/}"
cmd="${cmd//%k/}"
cmd="$(sed 's/%[dDnNvVm]//g' <<<"$cmd")"

# If after substitution the file path isn’t present, append it
if ! grep -q -- "$(printf '%s' "$FILE" | sed 's/[].[^$*\/]/\\&/g')" <<<"$cmd"; then
  cmd="$cmd $file_quoted"
fi

echo "Showing image $FILE"

# --- Tag environment variable for child process ---
SAFE_NAME=${SCRIPT_NAME^^}                # uppercase
SAFE_NAME=${SAFE_NAME//[^A-Z0-9_]/_}      # replace non-allowed chars
[[ $SAFE_NAME =~ ^[0-9] ]] && SAFE_NAME="_$SAFE_NAME"
TAG_ENV="LAUNCHED_BY_${SAFE_NAME}"

printf -v "$TAG_ENV" %s "1"
export "$TAG_ENV"

# --- Launch viewer and track PID ---
bash -c "$cmd" >/dev/null 2>&1 &
child=$!
echo "$child" > "$PIDFILE"
disown "$child"
