#!/bin/bash
# Regenerate SheepShaver x86_64 dyngen opcode headers with Homebrew GCC 16.
#
# Apple Clang cannot compile the dyngen opcode sources (fixed global
# registers + computed goto). The host dyngen extractor also needs a few
# Mach-O / GCC-16 accommodations that are applied to a private copy of
# dyngen.c; kpx_cpu is not modified.
#
# Usage (from anywhere):
#   SheepShaver/src/Unix/dyngen_precompiled/regenerate_x86_64.sh
#   SheepShaver/src/Unix/dyngen_precompiled/regenerate_x86_64.sh --no-install
#
# Requires: gcc-16, g++, perl, python3, otool, nm.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
SS=$ROOT/SheepShaver/src
KPX=$SS/kpx_cpu
PRE=$HERE

NO_INSTALL=0
if [[ "${1:-}" == "--no-install" ]]; then
  NO_INSTALL=1
fi

GCC=${GCC:-$(command -v gcc-16 || true)}
GXX=${GXX:-$(command -v g++-16 || true)}
if [[ -z "$GCC" || -z "$GXX" ]]; then
  echo "error: gcc-16 / g++-16 not on PATH (Homebrew gcc)" >&2
  exit 1
fi

OUT=${OUT:-$(mktemp -d /tmp/pocketshaver-dyngen-gcc.XXXXXX)}
echo "work dir: $OUT"
mkdir -p "$OUT/include/mach-o/ppc" "$OUT/include/cpu" "$OUT/obj" "$OUT/hdr"

# Modern SDK dropped mach-o/ppc/reloc.h. dyngen.c only needs the enum.
cat > "$OUT/include/mach-o/ppc/reloc.h" <<'EOF'
#ifndef _MACHO_PPC_RELOC_H_
#define _MACHO_PPC_RELOC_H_
enum reloc_type_ppc {
	PPC_RELOC_VANILLA,
	PPC_RELOC_PAIR,
	PPC_RELOC_BR14,
	PPC_RELOC_BR24,
	PPC_RELOC_HI16,
	PPC_RELOC_LO16,
	PPC_RELOC_HA16,
	PPC_RELOC_LO14,
	PPC_RELOC_SECTDIFF,
	PPC_RELOC_PB_LA_PTR,
	PPC_RELOC_HI16_SECTDIFF,
	PPC_RELOC_LO16_SECTDIFF,
	PPC_RELOC_HA16_SECTDIFF,
	PPC_RELOC_JBSR,
	PPC_RELOC_LO14_SECTDIFF,
	PPC_RELOC_LOCAL_SECTDIFF
};
#endif
EOF

# Opcode objects must emit a bare guest-address dereference so patch_jit.pl
# can wrap it. Strip the Apple/MEM_BULK kernel-window branch from the copy
# of vm.hpp used while compiling ops.
python3 - "$KPX/src/cpu/vm.hpp" "$OUT/include/cpu/vm.hpp" <<'PY'
import sys
from pathlib import Path
src = Path(sys.argv[1]).read_text()
idx = src.find("\treturn (uint8 *)(VMBaseDiff + a);")
if idx < 0:
    raise SystemExit("vm.hpp: VMBaseDiff return not found")
block_start = src.rfind("#if defined(__APPLE__)", 0, idx)
block_end = src.rfind("#endif", 0, idx)
if block_start < 0 or block_end < 0:
    raise SystemExit("vm.hpp: Apple/MEM_BULK block not found")
Path(sys.argv[2]).write_text(src[:block_start] + src[block_end + 6:])
PY

# Host extractor: skip unknown Mach-O load commands (LC_BUILD_VERSION=0x32),
# accept GCC's lC* (no leading dot) constant symbols, and turn a couple of
# asserts into errors so a bad object fails loudly instead of heap-corrupting.
python3 - "$KPX/src/cpu/jit/dyngen.c" "$OUT/dyngen.c" <<'PY'
import sys
from pathlib import Path
dyc = Path(sys.argv[1]).read_text()
subs = [
    ('error("LC: unknown command: %#x", lc.cmd);', 'break;'),
    (
        "assert(sym_name[0] == '.');\n  snprintf(name, sizeof(name), \"dot_%s\", sym_name + 1);",
        "if (sym_name[0] == '.')\n    snprintf(name, sizeof(name), \"dot_%s\", sym_name + 1);\n"
        "  else\n    snprintf(name, sizeof(name), \"dot_%s\", sym_name);",
    ),
    (
        '} else if (strstart(sym_name, ".LC", NULL)) {',
        '} else if (strstart(sym_name, ".LC", NULL) || strstart(sym_name, "lC", NULL)) {',
    ),
    (
        '!strstart(sym_name, ".LC", NULL))',
        '!strstart(sym_name, ".LC", NULL) &&\n                    !strstart(sym_name, "lC", NULL))',
    ),
    ('assert(gen_switch == 3);', 'if (gen_switch != 3) error("gen_switch=%d", gen_switch);'),
    ('assert(out_type == OUT_GEN_OP_ALL);', 'if (out_type != OUT_GEN_OP_ALL) error("out_type=%d", out_type);'),
]
for old, new in subs:
    if old not in dyc:
        raise SystemExit(f"dyngen.c: needle not found:\n{old}")
    dyc = dyc.replace(old, new, 1)
Path(sys.argv[2]).write_text(dyc)
PY

INCS=(
  -I"$OUT/include"
  -I"$SS/Unix"
  -I"$SS/MacOSX/config"
  -I"$SS/include"
  -I"$SS/CrossPlatform"
  -I"$KPX/include"
  -I"$KPX/src"
  -I"$KPX/src/cpu/jit"
  -I"$ROOT/BasiliskII/src/include"
  -I"$ROOT/BasiliskII/src/CrossPlatform"
)
DEFS=(-DHAVE_CONFIG_H -DEMU_KHEPERIX -DEMULATED_PPC=1)
OPFLAGS=(
  -O2 -fomit-frame-pointer -fno-align-functions -fno-stack-protector
  -finline-functions -finline-limit=10000 -fno-exceptions -g0
  -fno-reorder-blocks -fno-optimize-sibling-calls -fno-reorder-blocks-and-partition
  -fno-pic
)

echo "=== extractor (gcc-16) ==="
"$GCC" -O0 -g -fno-stack-protector -fno-strict-aliasing -std=gnu99 \
  "${INCS[@]}" "${DEFS[@]}" -c "$OUT/dyngen.c" -o "$OUT/obj/dyngen.o"
"$GXX" -O0 -g -fno-stack-protector -std=gnu++14 \
  "${INCS[@]}" "${DEFS[@]}" -c "$KPX/src/cpu/jit/cxxdemangle.cpp" -o "$OUT/obj/cxxdemangle.o"
"$GXX" -O0 -g -fno-stack-protector -o "$OUT/dyngen" "$OUT/obj/dyngen.o" "$OUT/obj/cxxdemangle.o"

echo "=== basic-dyngen-ops (g++-16) ==="
"$GXX" "${OPFLAGS[@]}" -std=gnu++14 "${INCS[@]}" "${DEFS[@]}" \
  -c "$KPX/src/cpu/jit/basic-dyngen-ops.cpp" -o "$OUT/obj/basic-dyngen-ops.o"
"$OUT/dyngen" -o "$OUT/hdr/basic-dyngen-ops-x86_64.hpp" "$OUT/obj/basic-dyngen-ops.o"

echo "=== ppc-dyngen-ops (g++-16) ==="
cp "$OUT/hdr/basic-dyngen-ops-x86_64.hpp" "$OUT/hdr/basic-dyngen-ops.hpp"
"$GXX" "${OPFLAGS[@]}" -std=gnu++14 -I"$OUT/hdr" "${INCS[@]}" "${DEFS[@]}" \
  -c "$KPX/src/cpu/ppc/ppc-dyngen-ops.cpp" -o "$OUT/obj/ppc-dyngen-ops.o"
"$OUT/dyngen" -o "$OUT/hdr/ppc-dyngen-ops-x86_64.hpp" "$OUT/obj/ppc-dyngen-ops.o"

PIPE=$PRE/gcc16_pipeline.py
python3 "$PIPE" sanitize \
  "$OUT/hdr/basic-dyngen-ops-x86_64.hpp" \
  "$OUT/hdr/ppc-dyngen-ops-x86_64.hpp"

echo "=== normalize GCC 16 addr32 encodings ==="
python3 "$PRE/normalize_gcc16.py" "$OUT/hdr"

echo "=== patch_jit + membulk ==="
python3 "$PIPE" adapt-patch-jit "$PRE/patch_jit.pl" "$OUT/hdr/patch_jit.pl"
python3 "$PIPE" adapt-membulk "$PRE/patch_jit_membulk.py" "$OUT/hdr/patch_jit_membulk.py"
(
  cd "$OUT/hdr"
  python3 patch_jit_membulk.py
  perl patch_jit.pl
)

nm -m "$OUT/obj/ppc-dyngen-ops.o" > "$OUT/ppc-nm.txt"
otool -s __TEXT __literal16 "$OUT/obj/ppc-dyngen-ops.o" > "$OUT/ppc-literal16.txt"
python3 "$PIPE" inject-literals \
  --header "$OUT/hdr/ppc-dyngen-ops-x86_64.hpp" \
  --header "$OUT/hdr/ppc-dyngen-ops-x86_64_macos.hpp" \
  --header "$OUT/hdr/ppc-dyngen-ops-x86_64_macos_membulk.hpp" \
  --nm "$OUT/ppc-nm.txt" \
  --otool "$OUT/ppc-literal16.txt"

echo "=== counts ==="
for f in basic-dyngen-ops-x86_64.hpp basic-dyngen-ops-x86_64_macos.hpp \
         basic-dyngen-ops-x86_64_macos_membulk.hpp \
         ppc-dyngen-ops-x86_64.hpp ppc-dyngen-ops-x86_64_macos.hpp \
         ppc-dyngen-ops-x86_64_macos_membulk.hpp; do
  printf "%s DEFINE_GEN=%s TRANS=%s\n" "$f" \
    "$(grep -c DEFINE_GEN "$OUT/hdr/$f")" \
    "$(grep -c TRANS_ "$OUT/hdr/$f" || true)"
done

python3 - <<PY
from pathlib import Path
import re
p = Path("$OUT/hdr/ppc-dyngen-ops-x86_64_macos_membulk.hpp")
t = p.read_text(encoding="latin-1", errors="replace")
m = re.search(r"DEFINE_GEN\\(gen_op_spcflags_set\\b.*?static const uint8 [A-Za-z0-9_]+_code\\[\\] = \\{(.*?)\\}", t, re.S)
body = " ".join(re.findall(r"0x([0-9a-fA-F]{2})", m.group(1))) if m else "MISSING"
print("spcflags_set:", body)
if "f0 09 85 b0 03" not in body:
    raise SystemExit("gen_op_spcflags_set is not lock-or; refusing to install")
PY

if [[ "$NO_INSTALL" -eq 0 ]]; then
  echo "=== install into $PRE ==="
  for f in basic-dyngen-ops-x86_64.hpp basic-dyngen-ops-x86_64_macos.hpp \
           basic-dyngen-ops-x86_64_macos_membulk.hpp \
           ppc-dyngen-ops-x86_64.hpp ppc-dyngen-ops-x86_64_macos.hpp \
           ppc-dyngen-ops-x86_64_macos_membulk.hpp; do
    cp "$OUT/hdr/$f" "$PRE/$f"
  done
fi

echo "done. work dir kept at $OUT"
