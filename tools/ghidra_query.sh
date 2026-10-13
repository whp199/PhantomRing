#!/usr/bin/env bash
# Query the analysed Ghidra projects (read-only) for PhantomRing RE.
#   tools/ghidra_query.sh er   dec:0x1403F2690 xref:0x143D69FF8 dis:0x14045EEF0
#   tools/ghidra_query.sh mgsv dec:0x141A6CAA0
# Addresses are full VAs (image base 0x140000000). Calls are serialized per project (flock),
# each takes ~1 min to open the project, so batch several commands per call.
set -euo pipefail
proj="${1:?er|mgsv}"; shift
case "$proj" in
	er) name=eldenring; prog=eldenring.exe ;;
	mgsv) name=mgsv; prog=mgsvtpp.exe ;;
	*) echo "unknown project $proj" >&2; exit 2 ;;
esac
G="${GHIDRA_HOME:-$HOME/bin/ghidra/build/dist/ghidra_12.3_DEV}"
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# Your own analysed Ghidra projects (not included): $GHIDRA_PROJECTS, default a ghidra-projects/ folder beside the repo.
P="${GHIDRA_PROJECTS:-$here/../../ghidra-projects}"
out="$(mktemp "${TMPDIR:-/tmp}/ghq.XXXXXX")"
(
	flock 9
	GHIDRA_HEADLESS_MAXMEM=16G "$G/support/analyzeHeadless" "$P" "$name" -process "$prog" -readOnly -noanalysis \
		-scriptPath "$here/ghidra_scripts" -postScript Decompile.java "$out" "$@" >"$out.log" 2>&1 || { tail -20 "$out.log" >&2; exit 1; }
) 9>"$P/.$name.query.lock"
cat "$out"
rm -f "$out" "$out.log"
