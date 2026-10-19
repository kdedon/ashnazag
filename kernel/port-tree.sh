# port-tree.sh -- sourced: the 68040/68060 port tree is upstream at
# PORT_PIN plus PORT_DIFFS, nothing else.  Needs AUX.
PORT_URL=https://github.com/asokero/amix-040-060-port
PORT_PIN=54fba4de3da276e5d09d5ea09e2c76a13ec20ffa
PORT_DIFFS="$AUX/kernel/port-local.diff $AUX/kernel/port-segvn.diff
	$AUX/kernel/mac/hat-boundary/hat-chgprot040.diff $AUX/kernel/port-isp060.diff"

# port_index DIR INDEX: write the expected tree into git index file INDEX
port_index() {
	rm -f "$2"
	GIT_INDEX_FILE=$2 git -C "$1" read-tree "$PORT_PIN" || return 1
	for p in $PORT_DIFFS; do
		GIT_INDEX_FILE=$2 git -C "$1" apply --cached "$p" ||
			{ echo "$p does not apply to $PORT_PIN" >&2; return 1; }
	done
	GIT_INDEX_FILE=$2 git -C "$1" update-index -q --refresh > /dev/null || :
}

# port_check DIR: list the tracked files that differ from the expected tree
port_check() {
	set -- "$1" "$1/.git/expected.index"
	port_index "$1" "$2" || return 1
	set -- "$1" "$2" "$(GIT_INDEX_FILE=$2 git -C "$1" diff --name-only)"
	rm -f "$2"
	[ -z "$3" ] || { echo "$3"; return 1; }
}

# port_sync DIR: make DIR the expected tree; edits outside the diffs are
# kept in its git stash
port_sync() {
	port_check "$1" > /dev/null && return 0
	set -- "$1" "$(git -C "$1" stash create)"
	[ -z "$2" ] || git -C "$1" stash store -m "edits outside the port diffs" "$2" || return 1
	git -C "$1" checkout -q -f --detach "$PORT_PIN" &&
	port_index "$1" "$1/.git/expected.index" &&
	GIT_INDEX_FILE=$1/.git/expected.index git -C "$1" checkout-index -a -f &&
	rm -f "$1/.git/expected.index"
}
