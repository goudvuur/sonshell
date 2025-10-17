#!/usr/bin/env bash

set -euo pipefail

COLOR_RESET=""
COLOR_DEBUG=""
COLOR_ERROR=""
if [[ -t 2 ]]; then
  if command -v tput >/dev/null 2>&1; then
    COLOR_RESET=$(tput sgr0 || true)
    COLOR_DEBUG=$(tput setaf 6 || true)
    COLOR_ERROR=$(tput setaf 1 || true)
  elif [[ ${TERM:-} != "dumb" ]]; then
    COLOR_RESET=$'\e[0m'
    COLOR_DEBUG=$'\e[36m'
    COLOR_ERROR=$'\e[31m'
  fi
fi

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] <media files...>
Options:
  -v               enable verbose debug output
  -m MIN_SCORE     minimum normalized score (0-100, default 30)
  -b, --backend B  share backend to use (default: kdeconnect)
  -h, --help       show this help and exit
EOF
}

print_debug() {
  if (( verbose )); then
    if [[ -n $COLOR_DEBUG && -n $COLOR_RESET ]]; then
      printf '%s[debug]%s %s\n' "$COLOR_DEBUG" "$COLOR_RESET" "$1" >&2
    else
      printf '[debug] %s\n' "$1" >&2
    fi
  fi
}

print_error() {
  if [[ -n $COLOR_ERROR && -n $COLOR_RESET ]]; then
    printf '%s%s%s\n' "$COLOR_ERROR" "$1" "$COLOR_RESET" >&2
  else
    printf '%s\n' "$1" >&2
  fi
}

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    if [[ -n $COLOR_ERROR && -n $COLOR_RESET ]]; then
      printf '%sRequired command not found:%s %s\n' "$COLOR_ERROR" "$COLOR_RESET" "$1" >&2
    else
      printf 'Required command not found: %s\n' "$1" >&2
    fi
    exit 1
  fi
}

abs_path() {
  local target="$1"
  if command -v realpath >/dev/null 2>&1; then
    realpath "$target" 2>/dev/null && return
  fi
  if command -v readlink >/dev/null 2>&1; then
    readlink -f "$target" 2>/dev/null && return
  fi
  local dir="${target%/*}"
  if [[ $dir == "$target" ]]; then
    dir='.'
  fi
  local base="${target##*/}"
  (cd "$dir" 2>/dev/null && printf '%s/%s\n' "$(pwd -P)" "$base") 2>/dev/null && return
  printf '%s\n' "$target"
}

ext_lower() {
  local f="$1"
  f="${f##*.}"
  printf '%s\n' "${f,,}"
}

guess_media_kind() {
  case "$(ext_lower "$1")" in
    jpg|jpeg|png|webp|heic|heif) echo image ;;
    mp4|mov|m4v|mkv|webm) echo video ;;
    *) echo unknown ;;
  esac
}

normalize_token() {
  printf '%s\n' "$1" | tr '[:upper:]' '[:lower:]' | tr -cd '[:alnum:]'
}

SUPPORTED_BACKENDS=(kdeconnect)

backend_supported() {
  local candidate="$1"
  local backend_name
  for backend_name in "${SUPPORTED_BACKENDS[@]}"; do
    if [[ $candidate == "$backend_name" ]]; then
      return 0
    fi
  done
  return 1
}

print_supported_backends() {
  printf '%s\n' "Supported backends: ${SUPPORTED_BACKENDS[*]}"
}

parse_kdeconnect_devices() {
  local -n out_ids=$1
  local -n out_names=$2
  local line id name trimmed
  while IFS= read -r line; do
    line=${line%%$'\r'}
    [[ -n $line ]] || continue
    if [[ $line == *$'\t'* ]]; then
      IFS=$'\t' read -r id name <<<"$line"
    else
      id=${line%%[[:space:]]*}
      name=${line#"$id"}
      name=${name# }
    fi
    [[ -n $id ]] || continue
    if [[ ! $id =~ ^[0-9a-fA-F-]+$ ]]; then
      continue
    fi
    trimmed=${name%% *\(}
    trimmed=${trimmed%% }
    out_ids+=("$id")
    out_names+=("${trimmed:-$name}")
  done
}

backend_kdeconnect_prepare() {
  local ip="$1"
  require_cmd kdeconnect-cli

  kdeconnect_selected_device_id=""
  kdeconnect_selected_device_name=""

  local kde_out
  if ! kde_out=$(kdeconnect-cli --list-available --id-name-only --refresh 2>/dev/null); then
    print_error 'kdeconnect-cli failed to query devices. Ensure KDE Connect is running.'
    exit 3
  fi

  local -a kde_ids=()
  local -a kde_names=()
  parse_kdeconnect_devices kde_ids kde_names <<<"$kde_out"

  if (( ${#kde_ids[@]} == 0 )); then
    print_error 'No paired KDE Connect devices are currently reachable.'
    exit 3
  fi

  local host_hint="${ip_hostname[$ip]:-}"
  local host_norm=""
  if [[ -n $host_hint ]]; then
    host_norm=$(normalize_token "$host_hint")
    print_debug "Selected host name: $host_hint"
  fi

  local match_idx=-1
  if [[ -n $host_norm ]]; then
    local idx
    for idx in "${!kde_ids[@]}"; do
      local name_norm
      name_norm=$(normalize_token "${kde_names[$idx]}")
      if [[ $name_norm == "$host_norm" || $name_norm == *"$host_norm"* || $host_norm == *"$name_norm"* ]]; then
        match_idx=$idx
        break
      fi
    done
  fi

  if (( match_idx < 0 )) && (( ${#kde_ids[@]} == 1 )); then
    match_idx=0
  fi

  if (( match_idx < 0 )); then
    print_error 'No KDE Connect device matches the selected host. Check pairing status.'
    print_debug "Available KDE Connect devices: ${kde_names[*]}"
    exit 3
  fi

  kdeconnect_selected_device_id=${kde_ids[$match_idx]}
  kdeconnect_selected_device_name=${kde_names[$match_idx]}
  print_debug "KDE Connect device matched: ${kdeconnect_selected_device_name} (${kdeconnect_selected_device_id})"
}

backend_kdeconnect_share() {
  local files=("$@")
  if (( ${#files[@]} == 0 )); then
    files=("${abs_files[@]}")
  fi
  if [[ -z $kdeconnect_selected_device_id ]]; then
    print_error 'Internal error: no KDE Connect device selected.'
    exit 4
  fi

  local file output status
  for file in "${files[@]}"; do
    print_debug "Sharing $file via KDE Connect"
    output=$(kdeconnect-cli --device "$kdeconnect_selected_device_id" --share "$file" 2>&1)
    status=$?
    if (( status != 0 )); then
      print_error "Failed to share $file via KDE Connect."
      print_error "Command output: $output"
      exit 4
    elif (( verbose )); then
      print_debug "kdeconnect-cli output: $output"
    fi
  done
}

backend_prepare() {
  local backend_name="$1"
  local ip="$2"
  case "$backend_name" in
    kdeconnect)
      backend_kdeconnect_prepare "$ip"
      ;;
    *)
      print_error "Backend prepare not implemented: $backend_name"
      exit 2
      ;;
  esac
}

backend_share() {
  local backend_name="$1"
  shift
  case "$backend_name" in
    kdeconnect)
      backend_kdeconnect_share "$@"
      ;;
    *)
      print_error "Backend share not implemented: $backend_name"
      exit 2
      ;;
  esac
}

verbose=0
min_score=30
backend="kdeconnect"

while (( $# )); do
  case "${1:-}" in
    -v)
      verbose=1
      shift
      ;;
    -m)
      shift
      [[ ${1:-} =~ ^[0-9]+$ ]] && (( 0 <= 10#${1} && 10#${1} <= 100 )) || { usage >&2; exit 2; }
      min_score=$((10#${1}))
      shift
      ;;
    -b|--backend)
      shift
      [[ -n ${1:-} ]] || { print_error 'Missing value for --backend'; exit 2; }
      backend=${1,,}
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
      usage >&2
      exit 2
      ;;
    *)
      break
      ;;
  esac
done

if (( $# == 0 )); then
  usage >&2
  exit 2
fi

if ! backend_supported "$backend"; then
  print_error "Unsupported backend: $backend"
  print_supported_backends >&2
  exit 2
fi

files=("$@")

abs_path() {
  local target="$1"
  if command -v realpath >/dev/null 2>&1; then
    realpath "$target" 2>/dev/null && return
  fi
  if command -v readlink >/dev/null 2>&1; then
    readlink -f "$target" 2>/dev/null && return
  fi
  local dir="${target%/*}"
  if [[ $dir == "$target" ]]; then
    dir='.'
  fi
  local base="${target##*/}"
  (cd "$dir" 2>/dev/null && printf '%s/%s\n' "$(pwd -P)" "$base") 2>/dev/null && return
  printf '%s\n' "$target"
}

ext_lower() {
  local f="$1"
  f="${f##*.}"
  printf '%s\n' "${f,,}"
}

guess_media_kind() {
  case "$(ext_lower "$1")" in
    jpg|jpeg|png|webp|heic|heif) echo image ;;
    mp4|mov|m4v|mkv|webm) echo video ;;
    *) echo unknown ;;
  esac
}

normalize_token() {
  printf '%s\n' "$1" | tr '[:upper:]' '[:lower:]' | tr -cd '[:alnum:]'
}

SUPPORTED_BACKENDS=(kdeconnect)

backend_supported() {
  local candidate="$1"
  local backend_name
  for backend_name in "${SUPPORTED_BACKENDS[@]}"; do
    if [[ $candidate == "$backend_name" ]]; then
      return 0
    fi
  done
  return 1
}

print_supported_backends() {
  printf '%s\n' "Supported backends: ${SUPPORTED_BACKENDS[*]}"
}

backend_prepare() {
  local backend_name="$1"
  local ip="$2"
  case "$backend_name" in
    kdeconnect)
      backend_kdeconnect_prepare "$ip"
      ;;
    *)
      print_error "Backend prepare not implemented: $backend_name"
      exit 2
      ;;
  esac
}

backend_share() {
  local backend_name="$1"
  shift
  case "$backend_name" in
    kdeconnect)
      backend_kdeconnect_share "$@"
      ;;
    *)
      print_error "Backend share not implemented: $backend_name"
      exit 2
      ;;
  esac
}

print_debug() {
  if (( verbose )); then
    if [[ -n $COLOR_DEBUG && -n $COLOR_RESET ]]; then
      printf '%s[debug]%s %s\n' "$COLOR_DEBUG" "$COLOR_RESET" "$1" >&2
    else
      printf '[debug] %s\n' "$1" >&2
    fi
  fi
}

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    if [[ -n $COLOR_ERROR && -n $COLOR_RESET ]]; then
      printf '%sRequired command not found:%s %s\n' "$COLOR_ERROR" "$COLOR_RESET" "$1" >&2
    else
      printf 'Required command not found: %s\n' "$1" >&2
    fi
    exit 1
  fi
}

print_error() {
  if [[ -n $COLOR_ERROR && -n $COLOR_RESET ]]; then
    printf '%s%s%s\n' "$COLOR_ERROR" "$1" "$COLOR_RESET" >&2
  else
    printf '%s\n' "$1" >&2
  fi
}

parse_kdeconnect_devices() {
  local -n out_ids=$1
  local -n out_names=$2
  local line id name trimmed
  while IFS= read -r line; do
    line=${line%%$'\r'}
    [[ -n $line ]] || continue
    if [[ $line == *$'\t'* ]]; then
      IFS=$'\t' read -r id name <<<"$line"
    else
      id=${line%%[[:space:]]*}
      name=${line#"$id"}
      name=${name# }
    fi
    [[ -n $id ]] || continue
    if [[ ! $id =~ ^[0-9a-fA-F-]+$ ]]; then
      continue
    fi
    trimmed=${name%% *\(}
    trimmed=${trimmed%% }  # remove trailing space
    out_ids+=("$id")
    out_names+=("${trimmed:-$name}")
  done
}

backend_kdeconnect_prepare() {
  local ip="$1"
  require_cmd kdeconnect-cli

  kdeconnect_selected_device_id=""
  kdeconnect_selected_device_name=""

  local kde_out
  if ! kde_out=$(kdeconnect-cli --list-available --id-name-only --refresh 2>/dev/null); then
    print_error 'kdeconnect-cli failed to query devices. Ensure KDE Connect is running.'
    exit 3
  fi

  local -a kde_ids=()
  local -a kde_names=()
  parse_kdeconnect_devices kde_ids kde_names <<<"$kde_out"

  if (( ${#kde_ids[@]} == 0 )); then
    print_error 'No paired KDE Connect devices are currently reachable.'
    exit 3
  fi

  local host_hint="${ip_hostname[$ip]:-}"
  local host_norm=""
  if [[ -n $host_hint ]]; then
    host_norm=$(normalize_token "$host_hint")
    print_debug "Selected host name: $host_hint"
  fi

  local match_idx=-1
  if [[ -n $host_norm ]]; then
    local idx
    for idx in "${!kde_ids[@]}"; do
      local name_norm
      name_norm=$(normalize_token "${kde_names[$idx]}")
      if [[ $name_norm == "$host_norm" || $name_norm == *"$host_norm"* || $host_norm == *"$name_norm"* ]]; then
        match_idx=$idx
        break
      fi
    done
  fi

  if (( match_idx < 0 )) && (( ${#kde_ids[@]} == 1 )); then
    match_idx=0
  fi

  if (( match_idx < 0 )); then
    print_error 'No KDE Connect device matches the selected host. Check pairing status.'
    print_debug "Available KDE Connect devices: ${kde_names[*]}"
    exit 3
  fi

  kdeconnect_selected_device_id=${kde_ids[$match_idx]}
  kdeconnect_selected_device_name=${kde_names[$match_idx]}
  print_debug "KDE Connect device matched: ${kdeconnect_selected_device_name} (${kdeconnect_selected_device_id})"
}

backend_kdeconnect_share() {
  local files=("$@")
  if (( ${#files[@]} == 0 )); then
    files=("${abs_files[@]}")
  fi
  if [[ -z $kdeconnect_selected_device_id ]]; then
    print_error 'Internal error: no KDE Connect device selected.'
    exit 4
  fi

  local file output status
  for file in "${files[@]}"; do
    print_debug "Sharing $file via KDE Connect"
    output=$(kdeconnect-cli --device "$kdeconnect_selected_device_id" --share "$file" 2>&1)
    status=$?
    if (( status != 0 )); then
      print_error "Failed to share $file via KDE Connect."
      print_error "Command output: $output"
      exit 4
    elif (( verbose )); then
      print_debug "kdeconnect-cli output: $output"
    fi
  done
}

media_kind=""
declare -a abs_files=()
kdeconnect_selected_device_id=""
kdeconnect_selected_device_name=""
for file in "${files[@]}"; do
  if [[ ! -f $file ]]; then
    print_error "Missing file: $file"
    exit 2
  fi
  if [[ ! -r $file ]]; then
    print_error "File is not readable: $file"
    exit 2
  fi
  abs_file=$(abs_path "$file") || abs_file="$file"
  abs_files+=("$abs_file")
  kind=$(guess_media_kind "$file")
  if [[ $kind == unknown ]]; then
    print_error "Unsupported media type: $file"
    exit 2
  fi
  if [[ -z $media_kind ]]; then
    media_kind=$kind
  elif [[ $media_kind != "$kind" ]]; then
    print_error 'Cannot mix images and videos in one run.'
    exit 2
  fi
done

if (( verbose )); then
  print_debug "Share backend: $backend"
  print_debug "Media kind: $media_kind"
  for path in "${abs_files[@]}"; do
    print_debug "  $path"
  done
fi

lookup_vendor() {
  local mac=$1
  local prefix=${mac^^}
  prefix=${prefix//:/-}
  prefix=${prefix:0:8}

  local db
  for db in /usr/share/ieee-data/oui.txt /usr/share/misc/oui.txt; do
    if [[ -f $db ]]; then
      awk -v prefix="$prefix" 'BEGIN{IGNORECASE=1}
        $1==prefix {
          $1=""
          sub(/^[[:space:]]+/, "")
          gsub(/\r$/, "")
          print
          exit
        }
      ' "$db"
      return
    fi
  done
}

require_cmd ip

declare -A mdns_ips=()
if command -v adb >/dev/null 2>&1; then
  mdns_output=$(adb mdns services 2>/dev/null || true)
  while IFS=: read -r mdns_ip mdns_port; do
    [[ $mdns_ip =~ ^[0-9]+(\.[0-9]+){3}$ ]] || continue
    mdns_ips[$mdns_ip]=1
  done < <(awk '/_adb.*\._tcp/ && $2 ~ /^[0-9.]+$/ && $3 ~ /^[0-9]+$/ { printf "%s:%s\n", $2, $3 }' <<<"$mdns_output")
else
  print_debug "adb not found; skipping mDNS discovery"
fi

mapfile -t neighbour_rows < <(
  ip neigh show \
    | awk '/^[0-9]+(\.[0-9]+){3}/ {
        state=$NF
        if (state=="FAILED" || state=="INCOMPLETE") next
        mac=""
        for (i=1; i<=NF; i++) {
          if ($i=="lladdr") {
            mac=$(i+1)
            break
          }
        }
        printf "%s %s %s\n", $1, mac, state
      }'
)

declare -A ip_seen=()
declare -A ip_mac=()
declare -A ip_state=()

for row in "${neighbour_rows[@]}"; do
  read -r ip mac state <<<"$row"
  [[ -n $ip ]] || continue
  if [[ -z ${ip_seen[$ip]+x} || ( ${ip_state[$ip]:-} != REACHABLE && $state = REACHABLE ) ]]; then
    ip_seen[$ip]=1
    ip_mac[$ip]=$mac
    ip_state[$ip]=$state
  fi
done

declare -A ip_hostname=()
declare -A ip_vendor=()
declare -A ip_score_norm=()
declare -A ip_score_raw=()
ip_entries=()

for ip in "${!ip_seen[@]}"; do
  local_host=$(getent hosts "$ip" 2>/dev/null | awk '{print $2; exit}' || true)
  [[ -n $local_host ]] && ip_hostname[$ip]=$local_host

  mac=${ip_mac[$ip]:-}
  if [[ -n $mac ]]; then
    vendor=$(lookup_vendor "$mac" || true)
    [[ -n $vendor ]] && ip_vendor[$ip]=$vendor
  else
    vendor=""
  fi

  raw_score=0
  [[ -n ${mdns_ips[$ip]+x} ]] && raw_score=$((raw_score + 100))
  if [[ -n $local_host ]] && grep -qiE 'android|pixel|google|oneplus|samsung|galaxy|xiaomi|redmi|huawei|motorola|moto|oppo|vivo|sony|xperia|nokia' <<<"$local_host"; then
    raw_score=$((raw_score + 40))
  fi
  if [[ -n ${ip_vendor[$ip]:-} ]] && grep -qiE 'google|android|htc|samsung|motorola|xiaomi|huawei|oneplus|oppo|vivo|sony|lg|nokia' <<<"${ip_vendor[$ip]}"; then
    raw_score=$((raw_score + 30))
  fi
  [[ ${ip_state[$ip]:-} == REACHABLE ]] && raw_score=$((raw_score + 5))

  normalized_score=$(((raw_score * 100 + 87) / 175))

  if (( normalized_score >= min_score )); then
    ip_score_raw[$ip]=$raw_score
    ip_score_norm[$ip]=$normalized_score
    ip_entries+=("$raw_score|$ip")
  fi
done

ipv4_clients=()
if (( ${#ip_entries[@]} > 0 )); then
  IFS=$'\n' read -r -d '' -a sorted_ip_entries < <(printf '%s\n' "${ip_entries[@]}" | sort -t'|' -k1,1nr -k2,2 && printf '\0')
  for entry in "${sorted_ip_entries[@]}"; do
    ipv4_clients+=("${entry#*|}")
  done
fi

  print_debug "Minimum score: $min_score"

if (( verbose )); then
  if (( ${#ipv4_clients[@]} > 0 )); then
    print_debug "Candidate IP order:"
    for ip in "${ipv4_clients[@]}"; do
      desc="  $ip (score=${ip_score_norm[$ip]} norm, raw=${ip_score_raw[$ip]}"
      [[ -n ${ip_hostname[$ip]:-} ]] && desc+=" host=${ip_hostname[$ip]}"
      [[ -n ${ip_vendor[$ip]:-} ]] && desc+=" vendor=${ip_vendor[$ip]}"
      [[ -n ${ip_mac[$ip]:-} ]] && desc+=" mac=${ip_mac[$ip]}"
      desc+=')'
      print_debug "$desc"
    done
  else
    print_debug "No active IPv4 neighbours passed the score filter"
  fi
fi

if (( ${#ipv4_clients[@]} == 0 )); then
  print_error 'No active IPv4 neighbours passed the score filter.'
  exit 1
fi

selected_ip=${ipv4_clients[0]}
print_debug "Selected host IP: $selected_ip"

backend_prepare "$backend" "$selected_ip"

backend_share "$backend" "${abs_files[@]}"

exit 0
