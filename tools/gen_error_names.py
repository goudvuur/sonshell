
#!/usr/bin/env python3
# Generates error_names_generated.h from Sony's CRSDK/CrError.h (anonymous enums).
# It creates code->name maps for CrError_*, CrWarning_*, and CrNotify_*.
import argparse, re, sys
from pathlib import Path

def parse_anon_enum_blocks(text: str):
    # Find all anonymous `enum { ... };`
    return re.findall(r'enum\s*\{(.*?)\};', text, re.S)

def parse_entries(body: str):
    entries = []
    current_val = None
    for raw in body.splitlines():
        line = raw.strip()
        if not line or line.startswith('//'):
            continue
        # strip comments
        if '/*' in line:
            line = line.split('/*',1)[0].strip()
        if '//' in line:
            line = line.split('//',1)[0].strip()
        if not line:
            continue
        # trailing comma
        if line.endswith(','):
            line = line[:-1]
        if not line:
            continue
        if '=' in line:
            name, val = [x.strip() for x in line.split('=',1)]
            if val.lower().startswith('0x'):
                current_val = int(val, 16)
            else:
                try:
                    current_val = int(val)
                except ValueError:
                    # alias to earlier symbol
                    alias = next((v for (n,v) in entries if n == val), None)
                    current_val = alias
            entries.append((name, current_val))
        else:
            current_val = 0 if current_val is None else current_val + 1
            entries.append((line, current_val))
    return entries

def build_maps(entries):
    err = {}
    warn = {}
    note = {}
    for name, val in entries:
        if val is None: 
            continue
        if name.startswith('CrError_'):
            err.setdefault(val, name)
        elif name.startswith('CrWarning_'):
            warn.setdefault(val, name)
        elif name.startswith('CrNotify_'):
            note.setdefault(val, name)
    return err, warn, note

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--header", required=True, help="Absolute path to CrError.h")
    ap.add_argument("-o", "--out", required=True, help="Output header path")
    args = ap.parse_args()

    header = Path(args.header)
    if not header.exists():
        sys.exit(f"Header not found: {header}")

    text = header.read_text(errors="ignore")
    blocks = parse_anon_enum_blocks(text)
    if not blocks:
        print("No anonymous enums found in CrError.h", file=sys.stderr)
        sys.exit(3)
    entries = []
    for b in blocks:
        entries += parse_entries(b)
    err, warn, note = build_maps(entries)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w") as f:
        f.write("// Auto-generated from Sony CRSDK CrError.h. Do not edit.\n")
        f.write("#pragma once\n")
        f.write("#include <unordered_map>\n")
        f.write("#include \"CRSDK/CrTypes.h\"\n\n")
        f.write("namespace crsdk_err {\n")
        f.write("static const std::unordered_map<CrInt32u, const char*> kErrorNames = {\n")
        for code, name in sorted(err.items()):
            f.write(f"    {{ (CrInt32u)0x{code:04x}, \"{name}\" }},\n")
        f.write("};\n")
        f.write("static const std::unordered_map<CrInt32u, const char*> kWarningNames = {\n")
        for code, name in sorted(warn.items()):
            f.write(f"    {{ (CrInt32u)0x{code:04x}, \"{name}\" }},\n")
        f.write("};\n")
        f.write("static const std::unordered_map<CrInt32u, const char*> kNotifyNames = {\n")
        for code, name in sorted(note.items()):
            f.write(f"    {{ (CrInt32u)0x{code:04x}, \"{name}\" }},\n")
        f.write("};\n\n")
        f.write("static inline const char* error_to_name(CrInt32u code) {\n")
        f.write("    auto it = kErrorNames.find(code);\n")
        f.write("    return it == kErrorNames.end() ? \"Error\" : it->second;\n")
        f.write("}\n")
        f.write("static inline const char* warning_to_name(CrInt32u code) {\n")
        f.write("    auto it = kWarningNames.find(code);\n")
        f.write("    if (it != kWarningNames.end()) return it->second;\n")
        f.write("    auto in = kNotifyNames.find(code);\n")
        f.write("    return in == kNotifyNames.end() ? \"Warning\" : in->second;\n")
        f.write("}\n")
        f.write("} // namespace crsdk_err\n")
    print(f"Wrote {out} with {len(err)} errors, {len(warn)} warnings, {len(note)} notifies.")

if __name__ == '__main__':
    main()
