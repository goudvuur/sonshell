#!/usr/bin/env python3
# Generates prop_names_generated.h from a specific CrDeviceProperty.h.
# Usage:
#   python3 gen_prop_names.py --header /absolute/path/to/CrDeviceProperty.h -o <out.h>
import argparse, re, sys
from pathlib import Path

def parse_enum(text, enum_name):
    m = re.search(r'enum\s+' + re.escape(enum_name) + r'\s*:\s*\w+\s*\{(.*?)\};', text, re.S)
    if not m:
        m = re.search(r'enum\s+' + re.escape(enum_name) + r'\s*\{(.*?)\};', text, re.S)
        if not m:
            sys.exit(f"Could not find enum {enum_name}")
    body = m.group(1)
    entries = []
    current_val = None
    for raw in body.splitlines():
        line = raw.strip()
        if not line or line.startswith('//'):
            continue
        if '/*' in line: line = line.split('/*', 1)[0].strip()
        if '//' in line: line = line.split('//', 1)[0].strip()
        if not line: continue
        if line.endswith(','): line = line[:-1]
        if not line: continue
        if '=' in line:
            name, val = [x.strip() for x in line.split('=', 1)]
            if val.lower().startswith('0x'):
                current_val = int(val, 16)
            else:
                try:
                    current_val = int(val)
                except ValueError:
                    # alias to previously defined symbol
                    current_val = next((v for (n, v) in entries if n == val), None)
            entries.append((name, current_val))
        else:
            current_val = 0 if current_val is None else current_val + 1
            entries.append((line, current_val))
    # de-dup by numeric value (keep first)
    code_to_name = {}
    for name, val in entries:
        if val is None:
            continue
        if val not in code_to_name:
            code_to_name[val] = name
    return code_to_name

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--header", required=True, help="Absolute path to CrDeviceProperty.h")
    ap.add_argument("-o", "--out", required=True, help="Output header path")
    args = ap.parse_args()

    header = Path(args.header)
    if not header.exists():
        sys.exit(f"Header not found: {header}")

    text = header.read_text(errors="ignore")
    mapping = parse_enum(text, "CrDevicePropertyCode")
    items = sorted(mapping.items())

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w") as f:
        f.write("// Auto-generated from Sony CRSDK CrDeviceProperty.h. Do not edit.\n")
        f.write("#pragma once\n")
        f.write("#include <unordered_map>\n")
        f.write("#include \"CRSDK/CrTypes.h\"\n\n")
        f.write(f"// SOURCE: {header}\n")
        f.write(f"#define PROP_NAMES_SOURCE \"{header}\"\n")
        f.write(f"#define PROP_NAMES_COUNT {len(items)}\n\n")
        f.write("namespace crsdk_util {\n")
        f.write("static const std::unordered_map<CrInt32u, const char*> kPropNames = {\n")
        for code, name in items:
            f.write(f"    {{ (CrInt32u)0x{code:04x}, \"{name}\" }},\n")
        f.write("};\n\n")
        f.write("static inline const char* prop_code_to_name(CrInt32u code) {\n")
        f.write("    auto it = kPropNames.find(code);\n")
        f.write("    return it == kPropNames.end() ? \"DeviceProperty\" : it->second;\n")
        f.write("}\n")
        f.write("} // namespace crsdk_util\n")
    print(f"Wrote {out} with {len(items)} entries from {header}")

if __name__ == '__main__':
    main()
