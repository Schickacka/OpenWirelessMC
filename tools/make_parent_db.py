#!/usr/bin/env python3
import os
import sys

def parse_multidisc(txt_path):
    pairs = []
    with open(txt_path, "r", encoding="utf-8") as f:
        for line in f:
            # Strip comments and whitespace
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "#" in line:
                line = line.split("#")[0].strip()
            if "=" in line:
                parts = line.split("=")
                child = parts[0].strip()
                parent = parts[1].strip()
                if child and parent:
                    pairs.append((child, parent))
    return pairs

def check_pairs(pairs):
    children = set()
    parents = dict()
    for child, parent in pairs:
        if child == parent:
            print(f"ERROR: self-mapping {child} -> {parent}")
            return False
        if child in children:
            print(f"ERROR: duplicate child {child}")
            return False
        children.add(child)
        parents[child] = parent
    
    # Check for chains (A -> B -> C)
    for child, parent in pairs:
        if parent in parents:
            print(f"WARNING: chain detected {child} -> {parent} -> {parents[parent]}")
    return True

def generate_code(pairs, out_h, out_cc):
    # Sort pairs alphabetically by child
    pairs_sorted = sorted(pairs, key=lambda x: x[0])

    header_code = """#ifndef _PARENT_DB_H
#define _PARENT_DB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *FindParentGameID(const char *child_id);

#ifdef __cplusplus
}
#endif

#endif // _PARENT_DB_H
"""
    with open(out_h, "w", encoding="utf-8") as f:
        f.write(header_code)

    cc_lines = []
    cc_lines.append('#include "storage/parent_db.h"')
    cc_lines.append('#include <string.h>')
    cc_lines.append('')
    cc_lines.append('struct ParentEntry {')
    cc_lines.append('  const char *child;')
    cc_lines.append('  const char *parent;')
    cc_lines.append('};')
    cc_lines.append('')
    cc_lines.append('static const ParentEntry kParentDb[] = {')
    for child, parent in pairs_sorted:
        cc_lines.append(f'  {{"{child}", "{parent}"}},')
    cc_lines.append('};')
    cc_lines.append('static const size_t kParentDbCount = sizeof(kParentDb) / sizeof(kParentDb[0]);')
    cc_lines.append('')
    cc_lines.append('const char *FindParentGameID(const char *child_id)')
    cc_lines.append('{')
    cc_lines.append('  if (!child_id || child_id[0] == \'\\0\')')
    cc_lines.append('  {')
    cc_lines.append('    return NULL;')
    cc_lines.append('  }')
    cc_lines.append('  for (size_t i = 0; i < kParentDbCount; i++)')
    cc_lines.append('  {')
    cc_lines.append('    if (strcasecmp(child_id, kParentDb[i].child) == 0)')
    cc_lines.append('    {')
    cc_lines.append('      return kParentDb[i].parent;')
    cc_lines.append('    }')
    cc_lines.append('  }')
    cc_lines.append('  return NULL;')
    cc_lines.append('}')
    cc_lines.append('')

    with open(out_cc, "w", encoding="utf-8") as f:
        f.write("\n".join(cc_lines))

def main():
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    multidisc_path = os.path.join(base_dir, "sdcard_assets", "multidisc.txt")
    out_h = os.path.join(base_dir, "src", "storage", "parent_db.h")
    out_cc = os.path.join(base_dir, "src", "storage", "parent_db.cc")

    pairs = parse_multidisc(multidisc_path)
    print(f"Parsed {len(pairs)} pairs from {multidisc_path}")

    if not check_pairs(pairs):
        sys.exit(1)

    if "--check" in sys.argv:
        print("Check completed successfully.")
        return

    generate_code(pairs, out_h, out_cc)
    print(f"Generated {out_h} and {out_cc} ({len(pairs)} entries)")

if __name__ == "__main__":
    main()
