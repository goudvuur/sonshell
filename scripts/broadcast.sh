#!/usr/bin/env bash

#/************************************************************
# * SonShell Broadcast Dispatcher                            *
# * -------------------------------------------------------- *
# * Routes camera event argument lists to configurable       *
# * handler scripts defined in broadcast.yml. The YAML tree  *
# * mirrors the positional arguments, supports glob keys,    *
# * multi-handler fanout, and placeholder substitution for   *
# * tailored argument lists. The dispatcher itself remains   *
# * quiet unless --verbose is supplied.                      *
# *                                                          *
# * Requirements:                                            *
# *   - python3                                              *
# *   - PyYAML (install via: pip install --user pyyaml)      *
# *   - Optional: realpath/readlink (for handler scripts)    *
# ************************************************************/

set -euo pipefail

if ! command -v python3 >/dev/null 2>&1; then
    echo "broadcast.sh requires python3 to be installed." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_CONFIG="$SCRIPT_DIR/broadcast.yml"
VERBOSE=0
CONFIG_PATH="$DEFAULT_CONFIG"

# ANSI colors (enable only when stdout is a terminal)
if [[ -t 1 ]]; then
    COLOR_INFO="\033[34m"   # blue
    COLOR_SUCCESS="\033[32m" # green
    COLOR_WARN="\033[33m"    # yellow
    COLOR_ERROR="\033[31m"   # red
    COLOR_RESET="\033[0m"
else
    COLOR_INFO=""
    COLOR_SUCCESS=""
    COLOR_WARN=""
    COLOR_ERROR=""
    COLOR_RESET=""
fi

usage() {
    cat <<USAGE
Usage: $(basename "$0") [--verbose] [--config PATH] -- <event args...>

Options:
  -v, --verbose       Enable verbose logging.
  -c, --config PATH   Path to YAML configuration (default: $DEFAULT_CONFIG).
      --help          Show this help message.

YAML format (each nesting level matches the argument index):
  rules:
    "*":
      playback:
        rating:
          "*":
            "3": handle_three_star.sh

Handlers can be written as strings or lists. If a handler command contains
placeholders like {1} or {2}, only the referenced arguments are passed; otherwise
all event arguments are appended after any static tokens.
USAGE
}

# Parse options before the event arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose)
            VERBOSE=1
            shift
            ;;
        -c|--config)
            if [[ $# -lt 2 ]]; then
                echo -e "${COLOR_ERROR}Missing value for --config option.${COLOR_RESET}" >&2
                exit 2
            fi
            CONFIG_PATH="$2"
            shift 2
            ;;
        --help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -*)
            echo -e "${COLOR_ERROR}Unknown option: $1${COLOR_RESET}" >&2
            usage
            exit 2
            ;;
        *)
            break
            ;;
    esac
done

if [[ $# -eq 0 ]]; then
    if (( VERBOSE )); then
        usage
    fi
    exit 0
fi

event_args=("$@")

if [[ ! -f "$CONFIG_PATH" ]]; then
    if (( VERBOSE )); then
        echo -e "${COLOR_WARN}Config file '$CONFIG_PATH' not found.${COLOR_RESET}" >&2
    fi
    exit 0
fi

CONFIG_DIR="$(cd "$(dirname "$CONFIG_PATH")" && pwd)"

python3 - "$CONFIG_PATH" "$CONFIG_DIR" "$SCRIPT_DIR" "$VERBOSE" "${COLOR_INFO}" "${COLOR_SUCCESS}" "${COLOR_WARN}" "${COLOR_ERROR}" "${COLOR_RESET}" "${event_args[@]}" <<'PY'
import fnmatch
import shlex
import os
import shutil
import subprocess
import sys
from typing import Dict, List, Sequence

try:
    import yaml  # type: ignore
except ImportError:  # pragma: no cover
    sys.stderr.write("broadcast.sh requires PyYAML (pip install pyyaml).\n")
    sys.exit(1)

CONFIG_PATH = sys.argv[1]
CONFIG_DIR = sys.argv[2]
SCRIPT_DIR = sys.argv[3]
VERBOSE = bool(int(sys.argv[4]))
COLOR_INFO, COLOR_SUCCESS, COLOR_WARN, COLOR_ERROR, COLOR_RESET = sys.argv[5:10]
EVENT_ARGS = sys.argv[10:]

USE_COLOR = sys.stderr.isatty() and any([COLOR_INFO, COLOR_SUCCESS, COLOR_WARN, COLOR_ERROR])


def colorize(color: str, message: str) -> str:
    if not USE_COLOR:
        return message
    return f"{color}{message}{COLOR_RESET}"


def log(message: str, level: str = "info", force: bool = False) -> None:
    if not VERBOSE and not force:
        return
    color = {
        "info": COLOR_INFO,
        "success": COLOR_SUCCESS,
        "warn": COLOR_WARN,
        "error": COLOR_ERROR,
    }.get(level, COLOR_INFO)
    sys.stderr.write(colorize(color, message) + "\n")


def ensure_mapping(node, context: str) -> Dict[str, object]:
    if node is None:
        return {}
    if isinstance(node, dict):
        return node
    log(f"Node '{context}' must be a mapping or script string; skipping.", "error", force=True)
    return {}


def normalize_scripts(value, context: str) -> List[str]:
    scripts: List[str] = []
    if value is None:
        return scripts
    if isinstance(value, str):
        scripts.append(value)
        return scripts
    if isinstance(value, list):
        for idx, entry in enumerate(value):
            if isinstance(entry, str):
                scripts.append(entry)
            else:
                log(
                    f"Script entry #{idx} in '{context}' must be a string; skipping.",
                    "warn",
                    force=True,
                )
        return scripts
    log(f"Script value in '{context}' must be string or list; skipping.", "warn", force=True)
    return scripts


def render_token(token: str) -> str:
    result: List[str] = []
    i = 0
    while i < len(token):
        char = token[i]
        if char == "{" :
            end = token.find("}", i + 1)
            if end == -1:
                result.append(char)
                i += 1
                continue
            placeholder = token[i + 1 : end]
            if placeholder.isdigit():
                idx = int(placeholder) - 1
                if 0 <= idx < len(EVENT_ARGS):
                    result.append(EVENT_ARGS[idx])
                else:
                    log(
                        f"Placeholder {{{placeholder}}} out of range for event arguments.",
                        "warn",
                        force=True,
                    )
            else:
                log(
                    f"Unsupported placeholder '{{{placeholder}}}' encountered; ignoring.",
                    "warn",
                    force=True,
                )
            i = end + 1
            continue
        if char == "\\" and i + 1 < len(token):
            result.append(token[i + 1])
            i += 2
            continue
        result.append(char)
        i += 1
    return "".join(result)


def prepare_command(script_entry: str):
    try:
        tokens = shlex.split(script_entry)
    except ValueError as exc:
        log(f"Unable to parse command '{script_entry}': {exc}", "error", force=True)
        return None

    if not tokens:
        log(f"Empty command produced from '{script_entry}'.", "warn", force=True)
        return None

    placeholder_used = any("{" in token and "}" in token for token in tokens)
    rendered_tokens = [render_token(token) for token in tokens]

    command = rendered_tokens[0]
    if not command:
        log(f"Command resolved to empty string in '{script_entry}'.", "error", force=True)
        return None
    extra_tokens = rendered_tokens[1:]

    if placeholder_used:
        final_args = extra_tokens
    else:
        final_args = extra_tokens + list(EVENT_ARGS)

    return command, final_args


def resolve_script(script_target: str) -> str | None:
    candidates: List[str] = []
    if os.path.isabs(script_target):
        candidates.append(script_target)
    else:
        if "/" in script_target:
            candidates.append(os.path.join(CONFIG_DIR, script_target))
            candidates.append(os.path.join(SCRIPT_DIR, script_target))
        else:
            candidates.append(os.path.join(SCRIPT_DIR, script_target))
            candidates.append(os.path.join(CONFIG_DIR, script_target))
        found_in_path = shutil.which(script_target)
        if found_in_path:
            candidates.append(found_in_path)
    for candidate in candidates:
        if os.path.exists(candidate):
            return os.path.abspath(candidate)
    return None


def execute_script(script_path: str | None, original_target: str, args: Sequence[str]):
    if script_path is None:
        log(f"Handler script '{original_target}' not found.", "error", force=True)
        return None
    if os.access(script_path, os.X_OK):
        cmd = [script_path, *args]
    else:
        cmd = ["bash", script_path, *args]
    try:
        proc = subprocess.Popen(cmd, start_new_session=True)
        if VERBOSE:
            log(f"Started PID {proc.pid} for '{original_target}'", "info")
        return proc
    except OSError as exc:
        log(f"Failed to execute '{script_path}': {exc}", "error", force=True)
        return None


def build_rules(config) -> List[Dict[str, object]]:
    rules: List[Dict[str, object]] = []

    def add_rule(name_parts: List[str], path: List[str], scripts: List[str]) -> None:
        if not scripts:
            return
        display = " > ".join(name_parts) if name_parts else "(root)"
        rules.append({
            "name": display,
            "scripts": list(scripts),
            "path": list(path),
        })

    def walk(node, path: List[str], name_parts: List[str]) -> None:
        context_name = " > ".join(name_parts) if name_parts else "(root)"

        if isinstance(node, (str, list)):
            scripts = normalize_scripts(node, context_name)
            add_rule(name_parts, path, scripts)
            return

        mapping = ensure_mapping(node, context_name if context_name != "(root)" else "root")
        if not mapping:
            return

        script_value = mapping.get("script")
        scripts = normalize_scripts(script_value, context_name)
        add_rule(name_parts, path, scripts)

        for key, child in mapping.items():
            if key == "script":
                continue
            key_str = str(key)
            walk(child, path + [key_str], name_parts + [key_str])

    root = config.get("rules", config)
    if isinstance(root, dict):
        for key, child in root.items():
            key_str = str(key)
            walk(child, [key_str], [key_str])
    elif isinstance(root, list):
        for idx, child in enumerate(root):
            key_str = str(idx)
            node = child
            if isinstance(child, dict) and "value" in child:
                key_str = str(child["value"])
                node = child.get("rule") if "rule" in child else {k: v for k, v in child.items() if k != "value"}
            walk(node, [key_str], [key_str])
    elif isinstance(root, str):
        walk(root, [], [])
    else:
        log("Top-level YAML must define 'rules' as mapping or list.", "error", force=True)

    return rules


def matches_path(expected_path: Sequence[str]) -> bool:
    if len(EVENT_ARGS) < len(expected_path):
        return False
    for idx, expected in enumerate(expected_path):
        actual = EVENT_ARGS[idx]
        if expected == "*":
            continue
        if not fnmatch.fnmatch(actual, expected):
            return False
    return True


def main() -> int:
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as fh:
            config = yaml.safe_load(fh) or {}
    except OSError as exc:
        log(f"Unable to read config '{CONFIG_PATH}': {exc}", "error", force=True)
        return 1

    rules = build_rules(config)
    if not rules:
        log("No rules defined in configuration.", "warn")

    matched = [rule for rule in rules if matches_path(rule["path"])]
    if not matched:
        log(f"No handlers matched arguments: {' '.join(EVENT_ARGS)}", "warn")
        return 0

    processes = []

    for rule in matched:
        for script in rule["scripts"]:
            log(f"Matched rule [{rule['name']}] -> {script}", "success")
            prepared = prepare_command(script)
            if prepared is None:
                continue
            command, final_args = prepared
            resolved = resolve_script(command)
            proc = execute_script(resolved, command, final_args)
            if proc is not None:
                processes.append(proc)

    if VERBOSE and processes:
        log(f"Launched {len(processes)} handler process(es) asynchronously.", "info")

    return 0


if __name__ == "__main__":
    sys.exit(main())
PY
