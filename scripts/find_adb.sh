#!/usr/bin/env bash
set -euo pipefail

# find_adb.sh
# Discover Android devices reachable over wireless ADB.
# - Reuses previously successful ip:port combos from a cache.
# - Scores ARP neighbours using mDNS, hostnames, and MAC OUIs to scan likely Android IPs first.
# - Scans the legacy 5555 range plus the TCP ephemeral range (default 32768-61000).
# - Stops after the first successful `adb connect` and prunes stale cache entries automatically.

usage() {
  cat <<USAGE
Usage: $(basename "$0") [-v] [-s START:END] [-m MIN_SCORE]
  -v  verbose; print progress details
  -s  scan port range START:END on each candidate IP (inclusive)
  -m  require normalized score >= MIN_SCORE (0-100) when considering IPs
USAGE
}

verbose=false
range_start=32768
range_end=61000
min_score=30

while getopts ":vhs:m:" opt; do
  case "$opt" in
    v) verbose=true ;;
    s)
      if [[ $OPTARG =~ ^([0-9]+):([0-9]+)$ ]]; then
        range_start=${BASH_REMATCH[1]}
        range_end=${BASH_REMATCH[2]}
        if (( range_start < 1 || range_end > 65535 || range_start > range_end )); then
          echo "Invalid scan range: $OPTARG" >&2
          exit 1
        fi
      else
        echo "Invalid scan range format: $OPTARG (expected START:END)" >&2
        exit 1
      fi
      ;;
    m)
      if [[ $OPTARG =~ ^[0-9]+$ ]] && (( OPTARG >= 0 && OPTARG <= 100 )); then
        min_score=$OPTARG
      else
        echo "Invalid minimum score: $OPTARG (expected 0-100)" >&2
        exit 1
      fi
      ;;
    h)
      usage
      exit 0
      ;;
    *)
      usage >&2
      exit 1
      ;;
  esac
done
shift $((OPTIND - 1))

print_info() {
  if [ "$verbose" = true ]; then
    printf '%s\n' "$1"
  fi
}

if ! command -v nc >/dev/null 2>&1; then
  echo "nc (netcat) is required but not installed" >&2
  exit 1
fi

if ! command -v adb >/dev/null 2>&1; then
  echo "adb is required but not installed" >&2
  exit 1
fi

has_nmap=false
if command -v nmap >/dev/null 2>&1; then
  has_nmap=true
fi

cache_dir=${XDG_CACHE_HOME:-"$HOME/.cache"}
cache_file="$cache_dir/find_adb_endpoints"

echo "Cache file: $cache_file"

print_cache() {
  if [ ! -f "$cache_file" ]; then
    print_info "Cache file not found: $cache_file"
    return
  fi

  print_info "Cached endpoints:"
  while IFS= read -r endpoint; do
    [ -n "$endpoint" ] || continue
    print_info "  $endpoint"
  done <"$cache_file"
}

lookup_vendor() {
  local mac=$1
  local prefix=${mac^^}
  prefix=${prefix//:/-}
  prefix=${prefix:0:8}

  local db
  for db in /usr/share/ieee-data/oui.txt /usr/share/misc/oui.txt; do
    if [ -f "$db" ]; then
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

remember_endpoint() {
  local endpoint=$1
  mkdir -p "$cache_dir" 2>/dev/null || true

  local tmp
  tmp=$(mktemp "${cache_file}.XXXXXX") || tmp=""

  if [ -n "$tmp" ]; then
    printf '%s\n' "$endpoint" >"$tmp"
    if [ -f "$cache_file" ]; then
      grep -vxF "$endpoint" "$cache_file" >>"$tmp" 2>/dev/null || true
    fi
    if ! mv "$tmp" "$cache_file"; then
      rm -f "$tmp"
    fi
  else
    printf '%s\n' "$endpoint" >"$cache_file"
  fi
}

prune_cache_entry() {
  local endpoint=$1
  [ -f "$cache_file" ] || return
  local tmp
  tmp=$(mktemp "${cache_file}.XXXXXX") || return
  grep -vxF "$endpoint" "$cache_file" >"$tmp" 2>/dev/null || true
  mv "$tmp" "$cache_file"
  if [ "$verbose" = true ]; then
    print_info "Pruned cached endpoint $endpoint"
  fi
}

is_successful_connect() {
  local message=$1
  if echo "$message" | grep -qiE '\b(connected to|already connected to|reconnected to)\b'; then
    return 0
  fi
  return 1
}

connect_endpoint() {
  local endpoint=$1
  local connect_output

  connect_output=$(adb connect "$endpoint" 2>&1 || true)
  printf '%s\n' "$connect_output"

  if is_successful_connect "$connect_output"; then
    remember_endpoint "$endpoint"
    return 0
  fi

  print_info "adb connect to $endpoint failed"
  prune_cache_entry "$endpoint"
  return 1
}

try_cached_endpoints() {
  print_cache

  if [ ! -f "$cache_file" ]; then
    return 1
  fi

  local had_entries=false
  while IFS= read -r endpoint; do
    [ -n "$endpoint" ] || continue
    had_entries=true
    print_info "Trying cached endpoint $endpoint"
    if connect_endpoint "$endpoint"; then
      return 0
    fi
  done <"$cache_file"

  if [ "$had_entries" = true ] && ! [ -s "$cache_file" ]; then
    print_info "Cache cleared of stale endpoints"
  fi

  return 1
}

if try_cached_endpoints; then
  exit 0
fi

mdns_stream=$( { adb mdns services 2>/dev/null || true; } \
  | awk '/_adb.*\._tcp/ && $2 ~ /^[0-9.]+$/ && $3 ~ /^[0-9]+$/ { printf "%s:%s\n", $2, $3 }' )

declare -A mdns_ips=()
if [ -n "$mdns_stream" ]; then
  while IFS=: read -r mdns_ip mdns_port; do
    [ -n "$mdns_ip" ] || continue
    mdns_ips[$mdns_ip]=1
  done <<< "$mdns_stream"
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
  [ -n "$ip" ] || continue
  if [ -z "${ip_seen[$ip]+x}" ] || { [ "${ip_state[$ip]:-}" != "REACHABLE" ] && [ "$state" = "REACHABLE" ]; }; then
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
  if [ -n "$local_host" ]; then
    ip_hostname[$ip]=$local_host
  fi

  mac=${ip_mac[$ip]:-}
  vendor=""
  if [ -n "$mac" ]; then
    vendor=$(lookup_vendor "$mac" || true)
    if [ -n "$vendor" ]; then
      ip_vendor[$ip]=$vendor
    fi
  fi

  raw_score=0
  if [ -n "${mdns_ips[$ip]+x}" ]; then
    raw_score=$((raw_score + 100))
  fi
  if [ -n "$local_host" ] && echo "$local_host" | grep -qiE 'android|pixel|google|oneplus|samsung|galaxy|xiaomi|redmi|huawei|motorola|moto|oppo|vivo|sony|xperia|nokia'; then
    raw_score=$((raw_score + 40))
  fi
  if [ -n "$vendor" ] && echo "$vendor" | grep -qiE 'google|android|htc|samsung|motorola|xiaomi|huawei|oneplus|oppo|vivo|sony|lg|nokia'; then
    raw_score=$((raw_score + 30))
  fi
  if [ "${ip_state[$ip]:-}" = "REACHABLE" ]; then
    raw_score=$((raw_score + 5))
  fi

  normalized_score=$(((raw_score * 100 + 87) / 175))

  if (( normalized_score >= min_score )); then
    ip_score_raw[$ip]=$raw_score
    ip_score_norm[$ip]=$normalized_score
    ip_entries+=("$raw_score|$ip")
  fi
done

ipv4_clients=()
if [ ${#ip_entries[@]} -gt 0 ]; then
  IFS=$'\n' read -r -d '' -a sorted_ip_entries < <(printf '%s\n' "${ip_entries[@]}" | sort -t'|' -k1,1nr -k2,2 && printf '\0')
  for entry in "${sorted_ip_entries[@]}"; do
    ipv4_clients+=("${entry#*|}")
  done
fi

if [ "$verbose" = true ]; then
  print_info "Minimum score: $min_score (normalized 0-100 scale)"
fi

if [ "$verbose" = true ] && [ ${#ipv4_clients[@]} -gt 0 ]; then
  print_info "Candidate IP order:"
  for ip in "${ipv4_clients[@]}"; do
    desc="  $ip (score=${ip_score_norm[$ip]} norm, raw=${ip_score_raw[$ip]}"
    if [ -n "${ip_hostname[$ip]:-}" ]; then
      desc="$desc, host=${ip_hostname[$ip]}"
    fi
    if [ -n "${ip_vendor[$ip]:-}" ]; then
      desc="$desc, vendor=${ip_vendor[$ip]}"
    fi
    if [ -n "${ip_mac[$ip]:-}" ]; then
      desc="$desc, mac=${ip_mac[$ip]}"
    fi
    desc="$desc)"
    print_info "$desc"
  done
fi

if [ ${#ipv4_clients[@]} -eq 0 ]; then
  print_info "No active IPv4 neighbours passed the score filter"
  exit 1
fi

ports=(5555 5554 5556)

declare -A seen_endpoints=()

scan_port() {
  local ip=$1
  local port=$2
  local endpoint="$ip:$port"

  if [ -n "${seen_endpoints[$endpoint]+x}" ]; then
    return 1
  fi

  if nc -w1 -z "$ip" "$port" >/dev/null 2>&1; then
    seen_endpoints[$endpoint]=1
    print_info "Found open TCP port $endpoint"
    if connect_endpoint "$endpoint"; then
      return 0
    fi
  fi

  return 1
}

scan_range_with_seq() {
  local ip=$1
  local start=$2
  local end=$3
  for port in $(seq "$start" "$end"); do
    if scan_port "$ip" "$port"; then
      return 0
    fi
  done
  return 1
}

scan_range_with_nmap() {
  local ip=$1
  local start=$2
  local end=$3
  local nmap_cmd=(nmap -sT -T5 -Pn -n -p "${start}-${end}" --min-rate 1000 --max-retries 1 --initial-rtt-timeout 100ms --max-rtt-timeout 600ms --host-timeout 1s -oG - "$ip")

  if [ "$verbose" = true ]; then
    print_info "Running: ${nmap_cmd[*]}"
  fi

  local nmap_output
  if ! nmap_output=$("${nmap_cmd[@]}" 2>/dev/null); then
    return 1
  fi

  local ports_found=()
  while IFS= read -r line; do
    case "$line" in
      Host*)
        if echo "$line" | grep -q "Status: Down"; then
          return 1
        fi
        ;;
      *Ports:*)
        IFS=',' read -ra fields <<<"${line#*Ports: }"
        for field in "${fields[@]}"; do
          port=$(printf '%s' "$field" | awk -F'/' '{print $1}')
          state=$(printf '%s' "$field" | awk -F'/' '{print $2}')
          if [ "$state" = "open" ]; then
            ports_found+=("$port")
          fi
        done
        ;;
    esac
  done <<< "$nmap_output"

  if [ ${#ports_found[@]} -eq 0 ]; then
    return 1
  fi

  for port in "${ports_found[@]}"; do
    if [[ $port =~ ^[0-9]+$ ]]; then
      if scan_port "$ip" "$port"; then
        return 0
      fi
    fi
  done

  return 1
}

scan_range() {
  local ip=$1
  local start=$2
  local end=$3
  if [ "$start" -gt "$end" ]; then
    return 1
  fi

  if [ "$has_nmap" = true ]; then
    if scan_range_with_nmap "$ip" "$start" "$end"; then
      return 0
    fi
    # Fallback to sequential scan if nmap found nothing.
  fi

  scan_range_with_seq "$ip" "$start" "$end"
}

for ip in "${ipv4_clients[@]}"; do
  [ -n "$ip" ] || continue
  print_info "Scanning $ip"
  for port in "${ports[@]}"; do
    if scan_port "$ip" "$port"; then
      exit 0
    fi
  done
  print_info "Scanning range ${range_start}-${range_end} on $ip"
  if scan_range "$ip" "$range_start" "$range_end"; then
    exit 0
  fi
done

if [ -n "$mdns_stream" ]; then
  print_info "mDNS advertised ADB endpoints detected"
  while IFS=: read -r ip port; do
    [ -n "$ip" ] || continue
    [ -n "$port" ] || continue
    print_info "Scanning $ip (mDNS)"
    if scan_port "$ip" "$port"; then
      exit 0
    fi
  done <<< "$mdns_stream"
elif [ "$verbose" = true ]; then
  print_info "No mDNS-advertised ADB endpoints detected"
fi

exit 1
