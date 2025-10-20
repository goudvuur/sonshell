#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<USAGE
Usage: $(basename "$0") [--single] <file>
  --single    Close any previous viewer launched by this script and reuse the
              default desktop image viewer (mirrors the legacy show_single.sh
              behaviour).
  -h, --help  Show this help message.
USAGE
}

single_mode=0
file=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --single)
      single_mode=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      file="$1"
      shift
      break
      ;;
  esac
done

if [[ -z $file && $# -gt 0 ]]; then
  file="$1"
  shift
fi

if [[ -z $file ]]; then
  usage >&2
  exit 1
fi

open_simple() {
  local path="$1"
  if [[ ! -e "$path" ]]; then
    echo "No such file: $path" >&2
    exit 1
  fi
  xdg-open "$path" >/dev/null 2>&1 &
}

kill_previous_viewer() {
  local pidfile="$1"
  if [[ -f "$pidfile" ]]; then
    local oldpid
    oldpid=$(cat "$pidfile")
    if [[ -n ${oldpid:-} ]] && kill -0 "$oldpid" 2>/dev/null; then
      kill "$oldpid" 2>/dev/null || true
      for _ in {1..20}; do
        kill -0 "$oldpid" 2>/dev/null || break
        sleep 0.05
      done
      kill -9 "$oldpid" 2>/dev/null || true
    fi
    rm -f "$pidfile"
  fi
}

resolve_desktop_command() {
  local file="$1"
  local mime
  mime=$(file --mime-type -Lb -- "$file")

  local desktop_id
  desktop_id=$(xdg-mime query default "$mime" || true)
  if [[ -z $desktop_id ]]; then
    echo "No default application found for MIME type: $mime" >&2
    exit 1
  fi

  local desktop_file=""
  for dir in "$HOME/.local/share/applications" /usr/local/share/applications /usr/share/applications; do
    if [[ -r "$dir/$desktop_id" ]]; then
      desktop_file="$dir/$desktop_id"
      break
    fi
  done

  if [[ -z $desktop_file ]]; then
    echo "Could not locate desktop file for: $desktop_id" >&2
    exit 1
  fi

  local exec_line
  exec_line=$(awk -F= '$1=="Exec"{print substr($0,index($0,$2))}' "$desktop_file" | head -n1)
  if [[ -z $exec_line ]]; then
    echo "No Exec= line found in $desktop_file" >&2
    exit 1
  fi

  local cmd="$exec_line"
  local file_quoted
  printf -v file_quoted '%q' "$file"

  cmd="${cmd//%F/$file_quoted}"
  cmd="${cmd//%U/$file_quoted}"
  cmd="${cmd//%f/$file_quoted}"
  cmd="${cmd//%u/$file_quoted}"
  cmd="${cmd//%i/}"
  cmd="${cmd//%c/}"
  cmd="${cmd//%k/}"
  cmd="$(sed 's/%[dDnNvVm]//g' <<<"$cmd")"

  if ! grep -q -- "$(printf '%s' "$file" | sed 's/[].[^$*\\/]/\\&/g')" <<<"$cmd"; then
    cmd="$cmd $file_quoted"
  fi

  printf '%s\n' "$cmd"
}

open_single_instance() {
  local path="$1"
  if [[ ! -e "$path" ]]; then
    echo "No such file: $path" >&2
    exit 1
  fi

  local resolved="$path"
  if command -v realpath >/dev/null 2>&1; then
    resolved=$(realpath "$path")
  fi

  local script_name
  script_name="$(basename "$0")"
  local pidfile="/tmp/${script_name}.pid"

  kill_previous_viewer "$pidfile"

  local command
  command=$(resolve_desktop_command "$resolved")

  local safe_name=${script_name^^}
  safe_name=${safe_name//[^A-Z0-9_]/_}
  [[ $safe_name =~ ^[0-9] ]] && safe_name="_$safe_name"
  local tag_env="LAUNCHED_BY_${safe_name}"
  printf -v "$tag_env" '%s' "1"
  export "$tag_env"

  bash -c "$command" >/dev/null 2>&1 &
  local child=$!
  echo "$child" > "$pidfile"
  disown "$child"
}

if (( single_mode )); then
  open_single_instance "$file"
else
  open_simple "$file"
fi
