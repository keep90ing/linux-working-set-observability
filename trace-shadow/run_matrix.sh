#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
CGROUP_ROOT=${CGROUP_ROOT:-/sys/fs/cgroup}
DEBUGFS_ROOT=${DEBUGFS_ROOT:-/sys/kernel/debug}
DATA_PARENT=${DATA_PARENT:-/var/tmp}
KEEP=${KEEP:-0}
MEMORY_STAT_SETTLE=${MEMORY_STAT_SETTLE:-3}
LRU_GEN_FILE="$DEBUGFS_ROOT/lru_gen"
LRU_GEN_FULL="$DEBUGFS_ROOT/lru_gen_full"
SOURCE="$SCRIPT_DIR/mglru_shadow_workload.c"
RUN_NAME="mglru-shadow-matrix-$$"
CGROUP_BASE="$CGROUP_ROOT/$RUN_NAME"
WORK_DIR=
HELPER=
CREATED_BASE=0
LEAVES=()
TARGETS=()

die()
{
	printf 'error: %s\n' "$*" >&2
	exit 1
}

cleanup()
{
	local i

	if [[ "$KEEP" == 1 ]]; then
		printf 'KEEP=1: retained cgroups at %s and files at %s\n' \
			"$CGROUP_BASE" "${WORK_DIR:-<not-created>}" >&2
		return
	fi

	for ((i = ${#LEAVES[@]} - 1; i >= 0; i--)); do
		if ! rmdir -- "${LEAVES[i]}" 2>/dev/null; then
			printf 'warning: cgroup remains: %s\n' "${LEAVES[i]}" >&2
		fi
	done
	if ((CREATED_BASE)); then
		if ! rmdir -- "$CGROUP_BASE" 2>/dev/null; then
			printf 'warning: cgroup remains: %s\n' "$CGROUP_BASE" >&2
		fi
	fi

	for ((i = 0; i < ${#TARGETS[@]}; i++)); do
		rm -f -- "${TARGETS[i]}"
	done
	if [[ -n "$HELPER" ]]; then
		rm -f -- "$HELPER"
	fi
	if [[ -n "$WORK_DIR" ]]; then
		rmdir -- "$WORK_DIR" 2>/dev/null || true
	fi
}
trap cleanup EXIT

require_command()
{
	command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

first_allowed_cpu()
{
	local list first

	list=$(awk '$1 == "Cpus_allowed_list:" { print $2; exit }' /proc/self/status)
	[[ -n "$list" ]] || return 1
	first=${list%%,*}
	printf '%s\n' "${first%%-*}"
}

node_for_cpu()
{
	local path

	for path in "/sys/devices/system/cpu/cpu$1"/node[0-9]*; do
		if [[ -e "$path" ]]; then
			printf '%s\n' "${path##*node}"
			return
		fi
	done
	printf '0\n'
}

controller_enabled()
{
	local controls

	controls=" $(<"$CGROUP_ROOT/cgroup.subtree_control") "
	[[ "$controls" == *" memory "* ]]
}

new_leaf()
{
	NEW_LEAF="$CGROUP_BASE/$1"

	mkdir -- "$NEW_LEAF"
	LEAVES+=("$NEW_LEAF")
	printf '%u\n' $((64 * 1024 * 1024)) > "$NEW_LEAF/memory.max"
	if [[ -f "$NEW_LEAF/memory.swap.max" ]]; then
		printf '0\n' > "$NEW_LEAF/memory.swap.max"
	fi
}

new_target()
{
	NEW_TARGET="$WORK_DIR/$1.bin"

	truncate -s "$PAGE_SIZE" -- "$NEW_TARGET"
	TARGETS+=("$NEW_TARGET")
}

run_in_cgroup()
{
	local leaf=$1
	shift

	bash -e -u -o pipefail -c '
		leaf=$1
		cpu=$2
		shift 2
		printf "%s\n" "$$" > "$leaf/cgroup.procs"
		exec taskset -c "$cpu" "$@"
	' mglru-cgroup-runner "$leaf" "$CPU" "$@"
}

memcg_id_for()
{
	local leaf=$1
	local relative=${leaf#"$CGROUP_ROOT"}
	local id=
	local attempt

	for ((attempt = 0; attempt < 100; attempt++)); do
		id=$(awk -v path="$relative" \
			'$1 == "memcg" && $3 == path { print $2; exit }' \
			"$LRU_GEN_FULL")
		if [[ -n "$id" ]]; then
			printf '%s\n' "$id"
			return
		fi
		sleep 0.02
	done

	return 1
}

# Print: file_min_seq max_seq for one memcg/node lruvec.
lru_state()
{
	local memcg_id=$1
	local nid=$2

	awk -v wanted_memcg="$memcg_id" -v wanted_node="$nid" '
		$1 == "memcg" {
			in_memcg = ($2 == wanted_memcg)
			in_node = 0
			next
		}
		in_memcg && $1 == "node" {
			in_node = ($2 == wanted_node)
			next
		}
		in_node && NF == 4 && $1 ~ /^[0-9]+$/ {
			if (file_min == "" && $4 !~ /x$/)
				file_min = $1
			max_seq = $1
		}
		END {
			if (file_min == "" || max_seq == "")
				exit 1
			print file_min, max_seq
		}
	' "$LRU_GEN_FULL"
}

# Print: public_memcg_id file_min_seq max_seq.  A fresh lruvec must have the
# full four-generation window so an immediate refault has distance three.
fresh_state()
{
	local leaf=$1
	local memcg_id state file_min max_seq

	memcg_id=$(memcg_id_for "$leaf") || die "cannot find $leaf in lru_gen_full"
	state=$(lru_state "$memcg_id" "$NID") || \
		die "cannot read memcg=$memcg_id node=$NID from lru_gen_full"
	read -r file_min max_seq <<< "$state"
	if ((max_seq - file_min != 3)); then
		die "memcg=$memcg_id node=$NID is not fresh: min=$file_min max=$max_seq"
	fi
	printf '%s %s %s\n' "$memcg_id" "$file_min" "$max_seq"
}

require_state()
{
	local memcg_id=$1
	local nid=$2
	local expected_min=$3
	local expected_max=$4
	local state file_min max_seq

	state=$(lru_state "$memcg_id" "$nid") || \
		die "cannot verify memcg=$memcg_id node=$nid"
	read -r file_min max_seq <<< "$state"
	if ((file_min != expected_min || max_seq != expected_max)); then
		die "generation race: expected min/max=$expected_min/$expected_max, observed $file_min/$max_seq"
	fi
}

age_to()
{
	local memcg_id=$1
	local nid=$2
	local target=$3
	local state file_min max_seq

	while :; do
		state=$(lru_state "$memcg_id" "$nid") || \
			die "cannot reread memcg=$memcg_id node=$nid"
		read -r file_min max_seq <<< "$state"
		if ((max_seq == target)); then
			printf 'aged memcg=%s nid=%s file_min=%s max=%s\n' \
				"$memcg_id" "$nid" "$file_min" "$max_seq"
			return
		fi
		if ((max_seq > target)); then
			die "generation overshot: wanted max=$target, observed max=$max_seq"
		fi
		printf '+ %s %s %s 0 1\n' "$memcg_id" "$nid" "$max_seq" > \
			"$LRU_GEN_FILE"
	done
}

memory_snapshot()
{
	local leaf=$1

	awk '
		$1 == "workingset_refault_file" { refault = $2 }
		$1 == "workingset_activate_file" { activate = $2 }
		$1 == "workingset_restore_file" { restore = $2 }
		END { print refault + 0, activate + 0, restore + 0 }
	' "$leaf/memory.stat"
}

report_delta()
{
	local label=$1
	local before=$2
	local after=$3
	local expected_refault=$4
	local expected_activate=$5
	local expected_restore=$6
	local b_refault b_activate b_restore a_refault a_activate a_restore
	local d_refault d_activate d_restore

	read -r b_refault b_activate b_restore <<< "$before"
	read -r a_refault a_activate a_restore <<< "$after"
	d_refault=$((a_refault - b_refault))
	d_activate=$((a_activate - b_activate))
	d_restore=$((a_restore - b_restore))
	printf '%s memory.stat delta: refault_file=%d activate_file=%d restore_file=%d\n' \
		"$label" "$d_refault" "$d_activate" "$d_restore"
	if ((d_refault != expected_refault || d_activate != expected_activate ||
	     d_restore != expected_restore)); then
		die "$label counter delta mismatch; expected $expected_refault/$expected_activate/$expected_restore"
	fi
}

settle_memory_stats()
{
	#
	# A one-page mod_lruvec_state() update stays in per-CPU memcg rstat until
	# the periodic forced flush.  memory.stat only triggers a synchronous flush
	# after a much larger update threshold, so an immediate read can report a
	# false zero.  The kernel's periodic interval is two seconds; wait one full
	# interval plus a default one-second margin before checking the delta.
	#
	printf 'waiting %ss for memory.stat rstat propagation\n' \
		"$MEMORY_STAT_SETTLE"
	sleep "$MEMORY_STAT_SETTLE"
}

print_case()
{
	local label=$1
	local target=$2
	local leaf=$3
	local memcg_id=$4
	local file_min=$5
	local max_seq=$6
	local dev ino

	read -r dev ino < <(stat -c '%d %i' -- "$target")
	printf '\nCase %s: dev=%s ino=%s index=0 memcg=%s path=%s nid=%s min=%s max=%s\n' \
		"$label" "$dev" "$ino" "$memcg_id" "${leaf#"$CGROUP_ROOT"}" \
		"$NID" "$file_min" "$max_seq"
}

[[ $EUID -eq 0 ]] || die "run this script as root"
[[ "$MEMORY_STAT_SETTLE" =~ ^[0-9]+([.][0-9]+)?$ ]] || \
	die "MEMORY_STAT_SETTLE must be a non-negative number"
require_command awk
require_command cc
require_command getconf
require_command mktemp
require_command sleep
require_command stat
require_command taskset
require_command truncate

[[ $(stat -f -c '%T' -- "$CGROUP_ROOT") == cgroup2fs ]] || \
	die "$CGROUP_ROOT is not a cgroup v2 mount"
[[ -r "$CGROUP_ROOT/cgroup.controllers" ]] || die "cannot read cgroup.controllers"
[[ " $(<"$CGROUP_ROOT/cgroup.controllers") " == *" memory "* ]] || \
	die "the cgroup v2 memory controller is unavailable"
controller_enabled || die "enable memory first: echo +memory > $CGROUP_ROOT/cgroup.subtree_control"

[[ -r "$LRU_GEN_FULL" && -w "$LRU_GEN_FILE" ]] || \
	die "mount debugfs and ensure lru_gen plus lru_gen_full are accessible"
[[ -r /sys/kernel/mm/lru_gen/enabled ]] || die "MGLRU sysfs interface is unavailable"
MGLRU_ENABLED=$(</sys/kernel/mm/lru_gen/enabled)
((MGLRU_ENABLED & 1)) || die "MGLRU core is disabled"

TRACEFS=/sys/kernel/tracing
[[ -d "$TRACEFS/events/workingset" ]] || TRACEFS="$DEBUGFS_ROOT/tracing"
[[ -e "$TRACEFS/events/workingset/mm_workingset_mglru_shadow_refault/id" ]] || \
	die "custom workingset tracepoints are not present in the running kernel"

CPU=${CPU:-$(first_allowed_cpu)}
[[ -n "$CPU" ]] || die "cannot select an allowed CPU"
[[ "$CPU" =~ ^[0-9]+$ ]] || die "CPU must name exactly one logical CPU"
taskset -c "$CPU" true
NID=$(node_for_cpu "$CPU")
[[ "$NID" =~ ^[0-9]+$ ]] || die "NID must name exactly one NUMA node"

mkdir -- "$CGROUP_BASE"
CREATED_BASE=1
printf '+memory\n' > "$CGROUP_BASE/cgroup.subtree_control"

WORK_DIR=$(mktemp -d -- "$DATA_PARENT/mglru-shadow.XXXXXX")
case $(stat -f -c '%T' -- "$WORK_DIR") in
	tmpfs | ramfs)
		die "DATA_PARENT must be disk-backed, not $(stat -f -c '%T' -- "$WORK_DIR")"
		;;
esac

HELPER="$WORK_DIR/mglru-shadow-workload"
CCACHE_DISABLE=1 cc -O2 -Wall -Wextra -Werror -o "$HELPER" "$SOURCE"
taskset -c "$CPU" "$HELPER" warm
PAGE_SIZE=$(getconf PAGESIZE)

printf 'observer: sudo env BPFTRACE_KERNEL_SOURCE=%s ' \
	"$(cd "$SCRIPT_DIR/../.." && pwd -P)"
printf 'BPFTRACE_KERNEL_BUILD=%s bpftrace %s/matrix_trace.bt\n' \
	"$(readlink -f "/lib/modules/$(uname -r)/build")" "$SCRIPT_DIR"
printf 'workload: cpu=%s expected_nid=%s data=%s cgroup=%s\n' \
	"$CPU" "$NID" "$WORK_DIR" "$CGROUP_BASE"
printf 'The trace must confirm order=0, type=1(file) and shadow_nid=refault_nid=%s.\n' "$NID"

# Case A: WS=0, same lruvec, immediate distance=3 refault.
new_leaf case_a
CG_A=$NEW_LEAF
new_target case_a
FILE_A=$NEW_TARGET
STATE=$(fresh_state "$CG_A")
read -r ID_A E_A MAX_A <<< "$STATE"
print_case A "$FILE_A" "$CG_A" "$ID_A" "$E_A" "$MAX_A"
run_in_cgroup "$CG_A" "$HELPER" evict-ws0 "$FILE_A"
require_state "$ID_A" "$NID" "$E_A" "$MAX_A"
BEFORE=$(memory_snapshot "$CG_A")
run_in_cgroup "$CG_A" "$HELPER" refault "$FILE_A"
settle_memory_stats
AFTER=$(memory_snapshot "$CG_A")
report_delta A "$BEFORE" "$AFTER" 1 0 0

# Case B: WS=0, same lruvec, advance max_seq once so distance becomes four.
new_leaf case_b
CG_B=$NEW_LEAF
new_target case_b
FILE_B=$NEW_TARGET
STATE=$(fresh_state "$CG_B")
read -r ID_B E_B MAX_B <<< "$STATE"
print_case B "$FILE_B" "$CG_B" "$ID_B" "$E_B" "$MAX_B"
run_in_cgroup "$CG_B" "$HELPER" evict-ws0 "$FILE_B"
require_state "$ID_B" "$NID" "$E_B" "$MAX_B"
age_to "$ID_B" "$NID" "$((E_B + 4))"
BEFORE=$(memory_snapshot "$CG_B")
run_in_cgroup "$CG_B" "$HELPER" refault "$FILE_B"
settle_memory_stats
AFTER=$(memory_snapshot "$CG_B")
report_delta B "$BEFORE" "$AFTER" 1 0 0

# Case C: the shadow belongs to src, while the replacement folio is charged to dst.
new_leaf case_c_src
CG_C_SRC=$NEW_LEAF
new_leaf case_c_dst
CG_C_DST=$NEW_LEAF
new_target case_c
FILE_C=$NEW_TARGET
STATE=$(fresh_state "$CG_C_SRC")
read -r ID_C_SRC E_C MAX_C <<< "$STATE"
ID_C_DST=$(memcg_id_for "$CG_C_DST") || die "cannot find C destination memcg"
print_case C "$FILE_C" "$CG_C_SRC" "$ID_C_SRC" "$E_C" "$MAX_C"
printf 'Case C destination: memcg=%s path=%s\n' \
	"$ID_C_DST" "${CG_C_DST#"$CGROUP_ROOT"}"
run_in_cgroup "$CG_C_SRC" "$HELPER" evict-ws0 "$FILE_C"
require_state "$ID_C_SRC" "$NID" "$E_C" "$MAX_C"
BEFORE_SRC=$(memory_snapshot "$CG_C_SRC")
BEFORE_DST=$(memory_snapshot "$CG_C_DST")
run_in_cgroup "$CG_C_DST" "$HELPER" refault "$FILE_C"
settle_memory_stats
AFTER_SRC=$(memory_snapshot "$CG_C_SRC")
AFTER_DST=$(memory_snapshot "$CG_C_DST")
report_delta C-src "$BEFORE_SRC" "$AFTER_SRC" 0 0 0
report_delta C-dst "$BEFORE_DST" "$AFTER_DST" 0 0 0

# Case D: saturate buffered-access refs until PG_workingset is set, then evict.
new_leaf case_d
CG_D=$NEW_LEAF
new_target case_d
FILE_D=$NEW_TARGET
STATE=$(fresh_state "$CG_D")
read -r ID_D E_D MAX_D <<< "$STATE"
print_case D "$FILE_D" "$CG_D" "$ID_D" "$E_D" "$MAX_D"
run_in_cgroup "$CG_D" "$HELPER" evict-ws1 "$FILE_D"
require_state "$ID_D" "$NID" "$E_D" "$MAX_D"
BEFORE=$(memory_snapshot "$CG_D")
run_in_cgroup "$CG_D" "$HELPER" refault "$FILE_D"
settle_memory_stats
AFTER=$(memory_snapshot "$CG_D")
report_delta D "$BEFORE" "$AFTER" 1 0 1

printf '\nExpected trace classifications: A=1 B=1 C=1 D=1.\n'
printf 'Expected memory.stat deltas (buffered refault): A=1/0/0 B=1/0/0 '
printf 'C-src=0/0/0 C-dst=0/0/0 D=1/0/1.\n'
