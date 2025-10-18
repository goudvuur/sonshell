#!/usr/bin/env bash

#/************************************************************
# * gmic.sh                                                  *
# * -------------------------------------------------------- *
# * Apply a configurable GMIC preset to an image, save the   *
# * processed copy next to the original, and display         *
# * before/after using show_single.sh. Future hooks will     *
# * introduce additional pre/post-processing steps.          *
# ************************************************************/

set -euo pipefail

VERBOSE=0
SHOW_IMAGES=1
COLOR_INFO=""
COLOR_WARN=""
COLOR_RESET=""
HAVE_MOGRIFY=1
GMIC_BIN=${GMIC_BIN:-gmic}
PRESET=${PRESET:-tensiongreen_1}
STRENGTH=${STRENGTH:-40}
BRIGHTNESS=${BRIGHTNESS:-0}
CONTRAST=${CONTRAST:-15}
GAMMA=${GAMMA:-8}
HUE=${HUE:-0}
SATURATION=${SATURATION:-0}
VIGNETTE=${VIGNETTE:-50}
PRE_RESIZE=${PRE_RESIZE:-}
PRE_RESIZE_WIDTH=0
PRE_RESIZE_HEIGHT=0
POST_JPEG_QUALITY=${POST_JPEG_QUALITY:-92}
POST_STRIP_METADATA=${POST_STRIP_METADATA:-1}
POST_INTERLACE_MODE=${POST_INTERLACE_MODE:-JPEG}

init_colors() {
  if [[ -t 2 ]]; then
    if command -v tput >/dev/null 2>&1; then
      COLOR_RESET=$(tput sgr0 || true)
      COLOR_INFO=$(tput setaf 6 || true)
      COLOR_WARN=$(tput setaf 3 || true)
    else
      COLOR_RESET=$'\033[0m'
      COLOR_INFO=$'\033[36m'
      COLOR_WARN=$'\033[33m'
    fi
  fi
}

log_info() {
  if (( VERBOSE )); then
    printf '%s[info]%s %s\n' "$COLOR_INFO" "$COLOR_RESET" "$1" >&2
  fi
}

log_warn() {
  printf '%s[warn]%s %s\n' "$COLOR_WARN" "$COLOR_RESET" "$1" >&2
}

usage() {
  cat <<USAGE
Usage: $(basename "$0") [-v] [--no-show] [--gmic-bin <path>] <image-file>

Options:
  -v                 Enable verbose script logging (gmic stays quiet).
  --no-show          Skip viewer preview/after display.
  --gmic-bin <path>  Use the specified gmic executable (default: '$GMIC_BIN').
  --preset <name>    Preset/CLUT to apply (default: '$PRESET').
  --strength <val>   Blend strength 0-1 (or 0-100 for percent, default: '$STRENGTH').
  --brightness <val> Brightness adjustment (default: '$BRIGHTNESS').
  --contrast <val>   Contrast adjustment (default: '$CONTRAST').
  --gamma <val>      Gamma adjustment (default: '$GAMMA').
  --hue <val>        Hue shift (default: '$HUE').
  --saturation <val> Saturation adjustment (default: '$SATURATION').
  --vignette <val>   Vignette strength 0-100 (default: '$VIGNETTE').
  --pre-resize <WxH> Resize a working copy before grading (maintains aspect, ImageMagick).
  --post-quality <n|none> JPEG quality (default: '$POST_JPEG_QUALITY').
  --post-strip <on|off> Strip metadata (default: 'on').
  --post-interlace <mode|none> JPEG interlace mode (default: '$POST_INTERLACE_MODE').
USAGE
}

parse_args() {
  if [[ $# -lt 1 ]]; then
    usage >&2
    exit 1
  fi

  while [[ $# -gt 0 ]]; do
    case "$1" in
      -v)
        VERBOSE=1
        shift
        ;;
      --no-show)
        SHOW_IMAGES=0
        shift
        ;;
      --gmic|--gmic-bin)
        if [[ $# -lt 2 ]]; then
          log_warn "Option $1 requires a value."
          usage >&2
          exit 2
        fi
        GMIC_BIN="$2"
        shift 2
        ;;
      --preset)
        if [[ $# -lt 2 ]]; then
          log_warn "Option $1 requires a value."
          usage >&2
          exit 2
        fi
        PRESET="$2"
        shift 2
        ;;
      --strength)
        if [[ $# -lt 2 ]]; then
          log_warn "Option $1 requires a value."
          usage >&2
          exit 2
        fi
        STRENGTH="$2"
        shift 2
        ;;
      --brightness)
        if [[ $# -lt 2 ]]; then
          log_warn "Option $1 requires a value."
          usage >&2
          exit 2
        fi
        BRIGHTNESS="$2"
        shift 2
        ;;
      --contrast)
        if [[ $# -lt 2 ]]; then
          log_warn "Option $1 requires a value."
          usage >&2
          exit 2
        fi
        CONTRAST="$2"
        shift 2
        ;;
      --gamma)
        if [[ $# -lt 2 ]]; then
          log_warn "Option $1 requires a value."
          usage >&2
          exit 2
        fi
        GAMMA="$2"
        shift 2
        ;;
      --hue)
        if [[ $# -lt 2 ]]; then
          log_warn "Option $1 requires a value."
          usage >&2
          exit 2
        fi
        HUE="$2"
        shift 2
        ;;
      --saturation)
        if [[ $# -lt 2 ]]; then
          log_warn "Option $1 requires a value."
          usage >&2
          exit 2
        fi
        SATURATION="$2"
        shift 2
        ;;
      --vignette)
        if [[ $# -lt 2 ]]; then
          log_warn "Option $1 requires a value."
          usage >&2
          exit 2
        fi
        VIGNETTE="$2"
        shift 2
        ;;
      --pre-resize)
        if [[ $# -lt 2 ]]; then
          log_warn "Option $1 requires a value."
          usage >&2
          exit 2
        fi
        PRE_RESIZE="$2"
        shift 2
        ;;
      --post-quality)
        if [[ $# -lt 2 ]]; then
          log_warn "Option $1 requires a value."
          usage >&2
          exit 2
        fi
        POST_JPEG_QUALITY="$2"
        shift 2
        ;;
      --post-strip)
        if [[ $# -lt 2 ]]; then
          log_warn "Option $1 requires a value (on|off)."
          usage >&2
          exit 2
        fi
        POST_STRIP_METADATA="$2"
        shift 2
        ;;
      --post-interlace)
        if [[ $# -lt 2 ]]; then
          log_warn "Option $1 requires a value (mode|none)."
          usage >&2
          exit 2
        fi
        POST_INTERLACE_MODE="$2"
        shift 2
        ;;
      --)
        shift
        break
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      -*)
        log_warn "Unknown option: $1"
        usage >&2
        exit 2
        ;;
      *)
        break
        ;;
    esac
  done

  if [[ $# -lt 1 ]]; then
    usage >&2
    exit 1
  fi

  TARGET_FILE="$1"
}

normalize_strength() {
  STRENGTH=$(awk -v v="$STRENGTH" 'BEGIN {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v > 1) v /= 100;
    printf "%.4f", v;
  }')
}

normalize_post_processing() {
  if [[ -n $POST_JPEG_QUALITY ]]; then
    local lowered=${POST_JPEG_QUALITY,,}
    if [[ $lowered == none ]]; then
      POST_JPEG_QUALITY=""
    elif [[ $POST_JPEG_QUALITY =~ ^[0-9]+$ ]]; then
      if (( POST_JPEG_QUALITY <= 0 )); then
        POST_JPEG_QUALITY=""
      fi
    else
      log_warn "Invalid --post-quality value '$POST_JPEG_QUALITY' (use an integer or 'none')."
      exit 2
    fi
  fi

  case "${POST_STRIP_METADATA,,}" in
    ""|"1"|"true"|"on")
      POST_STRIP_METADATA=1
      ;;
    "0"|"false"|"off"|"none")
      POST_STRIP_METADATA=0
      ;;
    *)
      log_warn "Invalid --post-strip value '$POST_STRIP_METADATA' (use on/off)."
      exit 2
      ;;
  esac

  if [[ -n $POST_INTERLACE_MODE ]]; then
    local lowered=${POST_INTERLACE_MODE,,}
    if [[ $lowered == none ]]; then
      POST_INTERLACE_MODE=""
    fi
  fi
}

validate_pre_resize() {
  if [[ -z $PRE_RESIZE ]]; then
    PRE_RESIZE_WIDTH=0
    PRE_RESIZE_HEIGHT=0
    return
  fi

  if [[ $PRE_RESIZE =~ ^([0-9]+)x([0-9]+)$ ]]; then
    PRE_RESIZE_WIDTH=${BASH_REMATCH[1]}
    PRE_RESIZE_HEIGHT=${BASH_REMATCH[2]}
    if (( PRE_RESIZE_WIDTH == 0 || PRE_RESIZE_HEIGHT == 0 )); then
      log_warn "--pre-resize dimensions must be greater than zero."
      exit 2
    fi
  else
    log_warn "Invalid --pre-resize value '$PRE_RESIZE' (expected format WIDTHxHEIGHT)."
    exit 2
  fi
}

should_preprocess() {
  (( PRE_RESIZE_WIDTH > 0 ))
}

post_processing_requested() {
  [[ -n $POST_JPEG_QUALITY || $POST_STRIP_METADATA -eq 1 || -n $POST_INTERLACE_MODE ]]
}

prepare_working_source() {
  local source="$1"
  local extension
  extension="${source##*.}"
  local tmp
  tmp=$(mktemp --suffix=".${extension,,}")
  cp -- "$source" "$tmp"
  printf '%s\n' "$tmp"
}

pre_process() {
  local working_file="$1"

  if (( PRE_RESIZE_WIDTH > 0 )); then
    pre_resize "$working_file"
  fi
}

pre_resize() {
  local working_file="$1"

  if (( ! HAVE_MOGRIFY )); then
    log_warn "Skipping pre-resize; 'mogrify' unavailable."
    return
  fi

  log_info "Resizing working copy to fit ${PRE_RESIZE_WIDTH}x${PRE_RESIZE_HEIGHT}"
  if ! mogrify -resize "${PRE_RESIZE_WIDTH}x${PRE_RESIZE_HEIGHT}>" "$working_file"; then
    local status=$?
    log_warn "Pre-resize failed (exit ${status})."
    exit $status
  fi
}

ensure_dependencies() {
  if ! command -v "$GMIC_BIN" >/dev/null 2>&1; then
    log_warn "Required command '$GMIC_BIN' not found. Install it from source (see ext folder)."
    log_warn "Example build (see https://gmic.eu/download.html):"
    log_warn "  sudo apt install git build-essential libgimp2.0-dev libcurl4-openssl-dev libfftw3-dev libtiff-dev libjpeg-dev libopenexr-dev libwebp-dev qtbase5-dev qttools5-dev-tools"
    log_warn "  wget https://gmic.eu/files/source/gmic_3.6.3.tar.gz && tar zxvf gmic_3.6.3.tar.gz"
    log_warn "  cd gmic-3.6.3/src && make all"
    exit 127
  fi
  if ! command -v mogrify >/dev/null 2>&1; then
    HAVE_MOGRIFY=0
    if (( PRE_RESIZE_WIDTH > 0 )); then
      log_warn "Requested pre-resize but command 'mogrify' not found. Install ImageMagick (e.g. sudo apt install imagemagick)."
      exit 127
    fi
    if post_processing_requested; then
      log_warn "Requested post-processing but command 'mogrify' not found. Install ImageMagick (e.g. sudo apt install imagemagick)."
      exit 127
    fi
    log_warn "Optional command 'mogrify' not found. Install ImageMagick (e.g. sudo apt install imagemagick) to enable JPEG optimisation."
  fi
}

main() {
  init_colors
  parse_args "$@"
  normalize_strength
  validate_pre_resize
  normalize_post_processing
  ensure_dependencies

  local script_path="$0"
  local script_name
  script_name="$(basename "$script_path")"
  local script_dir
  script_dir="$(dirname "$script_path")"

  local source="$TARGET_FILE"
  local extension
  extension="${source##*.}"
  extension="${extension,,}"

  local preset_slug
  preset_slug=$(printf '%s' "$PRESET" | tr '[:upper:]' '[:lower:]' | tr -cs '[:alnum:]' '_')
  preset_slug=${preset_slug#_}
  preset_slug=${preset_slug%_}
  if [[ -z $preset_slug ]]; then
    preset_slug="preset"
  fi

  local suffix="gmic_${preset_slug}"
  local show_cmd="$script_dir/show_single.sh"
  local output="${source%.*}_${suffix}.$extension"

  local working_source="$source"
  local cleanup_work=0

  if should_preprocess; then
    working_source=$(prepare_working_source "$source")
    cleanup_work=1
    log_info "Prepared working copy for pre-processing: $working_source"
    pre_process "$working_source"
  fi

  if (( SHOW_IMAGES )); then
    log_info "Showing original image: $source"
    launch_viewer "$show_cmd" "$source"
  else
    log_info "Viewer disabled; processing without preview"
  fi

  log_info "Applying $PRESET preset -> $output"
  apply_preset "$working_source" "$output"

  log_info "Post-processing output ($extension)"
  post_process "$output" "$extension"

  if (( cleanup_work )); then
    rm -f -- "$working_source"
  fi

 if (( SHOW_IMAGES )); then
    log_info "Displaying graded image: $output"
    replace_viewer "$show_cmd" "$output"
  else
    if (( VERBOSE )); then
      log_info "Viewer disabled; graded image available at $output"
    fi
  fi

  printf '%s\n' "$output"
}

launch_viewer() {
  local show_cmd="$1"
  local file="$2"
  eval "${show_cmd} $file" &
}

apply_preset() {
  local source="$1"
  local output="$2"

  local strength
  strength=$(awk -v v="$STRENGTH" 'BEGIN { if (v < 0) v = 0; if (v > 1) v = 1; printf "%.4f", v }')

  local gmic_base=(
    "$source"
    input
    "[0]x1"
    "-map_clut[1]"
    "$PRESET"
    "-blend[0,1]"
    "alpha,${strength}"
    "-adjust_colors"
    "${BRIGHTNESS},${CONTRAST},${GAMMA},${HUE},${SATURATION},0,255"
  )

  if [[ $(awk -v v="$VIGNETTE" 'BEGIN { print (v > 0) ? 1 : 0 }') -eq 1 ]]; then
    gmic_base+=("-fx_vignette" "${VIGNETTE},70,95,0,0,0,255")
  fi

  gmic_base+=("-o" "$output")

  if (( VERBOSE )); then
    if ! "$GMIC_BIN" "${gmic_base[@]}"; then
      local status=$?
      (( status = status % 256 ))
      log_warn "gmic processing failed (exit $status)."
      if (( VERBOSE )); then
        log_warn "Tip: install resources with 'gmic -update'. If that fails, download them manually:"
        log_warn "     mkdir -p ~/.config/gmic"
        log_warn "     curl -L https://gmic.eu/files/gmic-community.gmic -o ~/.config/gmic/gmic-community.gmic"
        log_warn "     curl -L https://gmic.eu/files/gmic_cluts.gmic -o ~/.config/gmic/gmic_cluts.gmic"
      fi
      exit $status
    fi
    return
  fi

  if ! "$GMIC_BIN" "${gmic_base[@]}" >/dev/null 2>&1
  then
    local status=$?
    (( status = status % 256 ))
    log_warn "gmic processing failed (exit $status)."
    if (( VERBOSE )); then
      log_warn "Tip: install resources with 'gmic -update'. If that fails, download them manually:"
      log_warn "     mkdir -p ~/.config/gmic"
      log_warn "     curl -L https://gmic.eu/files/gmic-community.gmic -o ~/.config/gmic/gmic-community.gmic"
      log_warn "     curl -L https://gmic.eu/files/gmic_cluts.gmic -o ~/.config/gmic/gmic_cluts.gmic"
    fi
    exit $status
  fi
}

post_process() {
  local file="$1"
  local extension="$2"

  case "${extension,,}" in
    jpg|jpeg)
      if post_processing_requested; then
        if (( HAVE_MOGRIFY )); then
          local args=()
          if [[ -n $POST_JPEG_QUALITY ]]; then
            args+=("-quality" "$POST_JPEG_QUALITY")
          fi
          if (( POST_STRIP_METADATA )); then
            args+=("-strip")
          fi
          if [[ -n $POST_INTERLACE_MODE ]]; then
            args+=("-interlace" "$POST_INTERLACE_MODE")
          fi

          if (( ${#args[@]} )); then
            if ! mogrify "${args[@]}" "$file"; then
              local status=$?
              log_warn "Post-processing failed (exit ${status})."
              exit $status
            fi
          elif (( VERBOSE )); then
            log_info "Post-processing disabled; nothing to do"
          fi
        else
          log_warn "Skipping requested post-processing; 'mogrify' unavailable."
        fi
      elif (( VERBOSE )); then
        log_info "Post-processing disabled; nothing to do"
      fi
      ;;
    png)
      :
      ;;
    *)
      :
      ;;
  esac
}

replace_viewer() {
  local show_cmd="$1"
  local file="$2"
  eval "${show_cmd} $file"
}

if [[ $# -lt 1 ]]; then
  usage >&2
  exit 1
fi

main "$@"
