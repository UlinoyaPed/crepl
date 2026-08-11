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

wide_type_output=$(run_crepl $'%type (__int128)0\n%type (unsigned __int128)0\n%type (_BitInt(17))0\n%quit\n')
contains "$wide_type_output" 'min      : -170141183460469231731687303715884105728' 'signed __int128 must have the correct minimum'
contains "$wide_type_output" 'max      : 170141183460469231731687303715884105727' 'signed __int128 must have the correct maximum'
contains "$wide_type_output" 'max      : 340282366920938463463374607431768211455' 'unsigned __int128 must have the correct maximum'
contains "$wide_type_output" 'bits     : 17' '_BitInt must use its semantic integer width'
contains "$wide_type_output" 'min      : -65536' 'signed _BitInt(17) must have the correct minimum'

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

nested_watch_output=$(run_crepl $'#include <vector>\nstd::vector<std::vector<int>> nested{{1}};\n%watch nested\n%snapshot nested_before\nnested[0][0] = 2;\n%snapshot nested_after\n%diff nested_before nested_after\n%quit\n')
nested_changes=$(grep -c '^nested:$' <<<"$nested_watch_output" || true)
[[ $nested_changes -eq 2 ]] || fail 'nested vector changes must affect watch and snapshot fingerprints'
contains "$nested_watch_output" 'before:' 'nested sequence snapshot diff must include its before state'
contains "$nested_watch_output" 'after:' 'nested sequence snapshot diff must include its after state'

cube_output=$(run_crepl $'int cube[16][16][16] = {};\ncube\n%watch cube\ncube[0][0][0] = 1;\n%quit\n')
contains "$cube_output" '... <15 more>' 'multidimensional arrays must share one leaf rendering budget'
[[ ${#cube_output} -lt 3000 ]] || fail 'multidimensional array output must remain globally bounded'
cube_changes=$(grep -c '^cube:$' <<<"$cube_output" || true)
[[ $cube_changes -eq 1 ]] || fail 'bounded multidimensional arrays must remain watchable'

memory_limit_output=$(run_crepl $'char big[65537] = {};\n%mem big\n%quit\n')
contains "$memory_limit_output" 'byte count must be between 1 and 65536' 'default %mem object sizes must respect the display limit'
[[ $memory_limit_output != *'address :'* ]] || fail 'oversized default %mem requests must not read or render memory'

postincrement_output=$(run_crepl $'int watched = 1;\n%watch watched\nwatched++\n%quit\n')
postincrement_old=$(grep -Fc '(int) 1' <<<"$postincrement_output")
postincrement_new=$(grep -Fc '(int) 2' <<<"$postincrement_output")
[[ $postincrement_old -eq 1 && $postincrement_new -eq 1 ]] ||
    fail 'a watched post-increment must print both its result and new state'
result_line=$(grep -n -m1 -F '(int) 1' <<<"$postincrement_output" | cut -d: -f1)
watch_line=$(grep -n -m1 -F 'watched:' <<<"$postincrement_output" | cut -d: -f1)
[[ $result_line -lt $watch_line ]] || fail 'expression results must precede watch updates'

state_output=$(run_crepl $'int n = 1;\n%watch n\n%snapshot before\nn = 2;\n%snapshot after\n%undo\n%state\n%diff before after\n%quit\n')
contains "$state_output" 'saved snapshot before' 'the before snapshot must be retained'
contains "$state_output" 'saved snapshot after' 'the after snapshot must be retained'
contains "$state_output" '(int) 2' '%undo must preserve documented runtime side effects'
contains "$state_output" 'old  : 0000' 'snapshot diff must render its before state'
contains "$state_output" 'new  : 0000' 'snapshot diff must render its after state'

printf 'All crepl regression tests passed.\n'
