#!/bin/bash
# ccache-wrap.sh REAL_COMPILER ARGS...
# xnu passes "-MJ FILE" (a per-object compile_commands.json entry) on every compile, and ccache refuses -MJ
# ("unsupported compiler option"), so the CI cache never stored anything. Drop it, leave an empty FILE (only the
# compile_commands.json stitching in MakeInc.kernel reads them) and let ccache handle the rest.
real=$1
shift
args=()
mj=
while [ $# -gt 0 ]; do
	if [ "$1" = -MJ ]; then
		mj=$2
		shift 2
		continue
	fi
	args+=("$1")
	shift
done
[ -n "$mj" ] && : > "$mj"
exec ccache "$real" "${args[@]}"
