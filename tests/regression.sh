#!/usr/bin/env bash

set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
binary=${CREPL_BINARY:-"$project_dir/build/bin/crepl"}

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    exit 1
}

run_crepl() {
    local input=$1
    printf '%s' "$input" | "$binary" 2>&1
}

contains() {
    local output=$1
    local expected=$2
    local description=$3
    grep -Fq -- "$expected" <<<"$output" || fail "$description"
}

cd "$project_dir"

[[ -x $binary ]] ||
    fail "crepl executable not found at $binary; run cmake --build --preset dev first"

layout_output=$(run_crepl $'struct B { int x; };\nstruct D : B { int y; };\n%layout D\nstruct V { virtual void f(); int x; };\n%layout V\n%quit\n')
layout_rejections=$(grep -Fc 'inheritance/vptr layout is not supported yet' <<<"$layout_output")
[[ $layout_rejections -eq 2 ]] || fail '%layout must reject inherited and dynamic classes'

type_output=$(run_crepl $'int x = 1;\n%type ++x\nx\n%undo\nx\n%quit\n')
type_values=$(grep -Fc '(int) 1' <<<"$type_output")
[[ $type_values -eq 2 ]] || fail '%type must not evaluate ++x or consume user undo history'
contains "$type_output" 'type     : int' '%type must report the canonical type'
[[ $type_output != *$'\033'* ]] || fail 'piped input and output must not contain ANSI colors'

if command -v socat >/dev/null 2>&1; then
    pty_output=$(
        printf '%%quit\n' |
            socat -,ignoreeof \
                EXEC:"env -u NO_COLOR TERM=alacritty $binary",pty,setsid,ctty
    )
    [[ $pty_output == *$'\033['* ]] ||
        fail 'an interactive alacritty PTY must contain ANSI colors'
fi

allocator_output=$(run_crepl $'#include <vector>\n#include <cstddef>\ntemplate<class T> struct Mine { using value_type=T; Mine()=default; template<class U> Mine(const Mine<U>&) {} T* allocate(std::size_t n) { return static_cast<T*>(::operator new(n*sizeof(T))); } void deallocate(T* p, std::size_t) { ::operator delete(p); } };\nstd::vector<int, Mine<int>> v = {1,2,3};\nv\n%index v\n%quit\n')
contains "$allocator_output" '(std::vector<int, Mine<int> > &) @' 'custom allocator vectors must use the Clang fallback printer'
contains "$allocator_output" 'expression is not a supported C array, std::array, or std::vector' '%index must reject an unsupported vector ABI'

sequence_output=$(run_crepl $'int a[300] = {};\na\n%index a\n%watch a\na[299] = 7;\na[0] = 1;\n%quit\n')
contains "$sequence_output" '... <44 more>' 'large sequences must report truncated elements'
watch_changes=$(grep -c '^a:$' <<<"$sequence_output" || true)
[[ $watch_changes -eq 1 ]] || fail 'watch fingerprints must be bounded to the displayed sequence prefix'

state_output=$(run_crepl $'int n = 1;\n%watch n\n%snapshot before\nn = 2;\n%snapshot after\n%undo\n%state\n%diff before after\n%quit\n')
contains "$state_output" 'saved snapshot before' 'the before snapshot must be retained'
contains "$state_output" 'saved snapshot after' 'the after snapshot must be retained'
contains "$state_output" '(int) 2' '%undo must preserve documented runtime side effects'
contains "$state_output" 'old  : 0000' 'snapshot diff must render its before state'
contains "$state_output" 'new  : 0000' 'snapshot diff must render its after state'

printf 'All crepl regression tests passed.\n'
