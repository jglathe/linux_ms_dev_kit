#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

readonly SP11_BASE_COMMIT="8f953dd060bc6e8fb86ca2ea8a92f258141c0169"
readonly SP11_RANGE="${SP11_BASE_COMMIT}...HEAD"

die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}

git rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
	die "run this check from the repository worktree"

git cat-file -e "${SP11_BASE_COMMIT}^{commit}" 2>/dev/null ||
	die "required Surface Pro 11 base commit is unavailable"

git merge-base --is-ancestor "${SP11_BASE_COMMIT}" HEAD ||
	die "HEAD does not descend from the approved Surface Pro 11 base commit"

# When HEAD is the upstream-sync merge (two parents), the SP11 delta to
# validate is the first-parent line; the second parent is upstream's own
# code, which may carry pre-existing style patterns.
CHECKPATCH_RANGE="${SP11_RANGE}"
if git rev-parse -q --verify HEAD^2 >/dev/null 2>&1; then
	CHECKPATCH_RANGE="${SP11_BASE_COMMIT}...HEAD^1"
fi

git diff --check "${SP11_RANGE}" --

all_changed_paths=()
while IFS= read -r -d '' path; do
	all_changed_paths+=("${path}")
done < <(git diff --name-only -z --diff-filter=ACMRTUXBD "${SP11_RANGE}" --)

if ((${#all_changed_paths[@]} == 0)); then
	printf 'No changes from the approved Surface Pro 11 base commit.\n'
	exit 0
fi

present_paths=()
while IFS= read -r -d '' path; do
	present_paths+=("${path}")
done < <(git diff --name-only -z --diff-filter=ACMRTUXB "${SP11_RANGE}" --)

artifact_path_pattern='^(build|out|artifacts?|dist)/|(^|/)(\.config|Module\.symvers|modules\.order|System\.map|vmlinux|Image(\.gz)?)(/|$)|\.(a|bin|bz2|deb|ddeb|dwo|dtb|dtbo|dylib|efi|elf|gz|img|iso|ko|lz4|o|qcow2|rlib|rmeta|rpm|so|tar|tgz|xz|zip|zst)$'
private_file_pattern='(^|/)(\.env|id_rsa|id_ed25519|credentials?\.json)$|\.(p12|pfx)$'

if ((${#present_paths[@]} > 0)); then
	for path in "${present_paths[@]}"; do
		[[ "${path}" =~ ${artifact_path_pattern} ]] &&
			die "generated build or binary artifact is tracked: ${path}"
		[[ "${path}" =~ ${private_file_pattern} ]] &&
			die "possible private credential file is tracked: ${path}"
	done
fi

binary_paths="$(
	git diff --numstat --diff-filter=ACMRTUXB "${SP11_RANGE}" -- |
		awk -F '\t' '$1 == "-" && $2 == "-" { print $3 }'
)"
[[ -z "${binary_paths}" ]] ||
	die "binary changes are not accepted in the integration delta: ${binary_paths}"

added_lines="$(mktemp)"
trap 'rm -f "${added_lines}"' EXIT

git diff --no-ext-diff --unified=0 "${SP11_RANGE}" -- |
	awk '/^\+\+\+ / { next } /^\+/ { sub(/^\+/, ""); print }' >"${added_lines}"

local_path_pattern='/U'"sers/"'|/ho'"me/"'[[:alnum:]_.-]+/'
tunnel_pattern='ng'"rok"'|ts'"sh"'[[:space:]]'
private_key_pattern='-----BEGIN [A-Z0-9 ]*PRIV'"ATE KEY-----"
token_pattern='gh[pousr]_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16}'
fine_grained_token_pattern='git'"hub_pat_"'[A-Za-z0-9_]{20,}'
lfs_pointer_pattern='version https://git-'"lfs.github.com/spec/v1"
private_content_pattern="(${local_path_pattern}|${tunnel_pattern}|${private_key_pattern}|${token_pattern}|${fine_grained_token_pattern}|${lfs_pointer_pattern})"

if LC_ALL=C grep -En -- "${private_content_pattern}" "${added_lines}"; then
	die "possible workstation path, private endpoint, or credential in added content"
fi

kernel_changed=false
for path in "${all_changed_paths[@]}"; do
	case "${path}" in
	*.c | *.h | *.S | *.rs | *.dts | *.dtsi | *.patch | \
		arch/*/configs/* | */Kconfig* | Kconfig* | */Makefile | Makefile | \
		Documentation/devicetree/bindings/*.yaml)
		kernel_changed=true
		break
		;;
	esac
done

if [[ "${kernel_changed}" == true ]]; then
	[[ -x scripts/checkpatch.pl ]] ||
		die "scripts/checkpatch.pl is required for kernel-source changes"
	kernel_pathspecs=(
		':(top,glob)**/*.c'
		':(top,glob)**/*.h'
		':(top,glob)**/*.S'
		':(top,glob)**/*.rs'
		':(top,glob)**/*.dts'
		':(top,glob)**/*.dtsi'
		':(top,glob)**/*.patch'
		':(top,glob)arch/*/configs/**'
		':(top,glob)**/Kconfig*'
		':(top,glob)**/Makefile'
		':(top,glob)Documentation/devicetree/bindings/**/*.yaml'
	)
	# File changes are reviewed, but their generic MAINTAINERS reminder is
	# not a style defect. --no-tree lets checkpatch run without a full
	# kernel tree checkout (the workflow uses a sparse checkout).  Only
	# checkpatch ERROR-level findings fail the gate; WARNING and CHECK
	# levels are reported for review but do not block, since upstream
	# code may carry pre-existing style patterns.
	checkpatch_output="$(git diff --no-ext-diff "${CHECKPATCH_RANGE}" -- "${kernel_pathspecs[@]}" |
		scripts/checkpatch.pl --no-tree --strict --show-types --ignore FILE_PATH_CHANGES - || true)"
	echo "${checkpatch_output}"
	if echo "${checkpatch_output}" | grep -qE '^ERROR:'; then
		die "checkpatch.pl reported ERROR-level findings (see above)"
	fi
else
	printf 'No kernel-source changes require checkpatch.pl.\n'
fi

printf 'Surface Pro 11 integration checks passed for %s.\n' "$(git rev-parse HEAD)"
