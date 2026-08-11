# Repository guidance

## Project scope

`crepl` is an incremental C++20 REPL built on the experimental LLVM/Clang
Interpreter API. Keep changes focused and preserve ordinary declarations,
expressions, incremental execution, and Clang's fallback `Value` printer.

The implementation remains in `crepl.cpp`, but its boundaries are explicit:
`Terminal` owns libedit, `run_frontend()` owns commands/execution metadata,
session helpers own Interpreter creation/startup, and renderer functions own
value output. Preserve those boundaries until files are deliberately split.

## Build and test

Use Clang/LLVM 22 or newer. The primary development workflow is:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The regression suite can also be run against an existing binary:

```bash
CREPL_BINARY="$PWD/build/bin/crepl" tests/regression.sh
```

When diagnosing the legacy one-file build, use the complete command documented
in `README.md`; do not guess LLVM link components from an older release.

Every behavior change should add or update a black-box case in
`tests/regression.sh`. Run `git diff --check` in addition to the relevant build
and tests.

## LLVM and execution invariants

- Treat installed LLVM/Clang headers as authoritative; Interpreter APIs are
  experimental and version-coupled.
- Include `clang/Frontend/CompilerInstance.h` wherever destruction of
  `std::unique_ptr<clang::CompilerInstance>` requires the complete type.
- Keep temporary `clang::Value` objects in an inner scope so they are destroyed
  before calling `Interpreter::Undo()` on their temporary PTU.
- `Undo()` removes incremental compilation units; it does not revert runtime
  memory writes or other side effects.
- Introspection commands such as `%type` must remain unevaluated. Commands that
  intentionally inspect values may evaluate expressions, but should document
  that behavior.
- Print a user expression result before refreshing watches. A trailing
  semicolon may intentionally produce no `Value` under Clang's discarded-value
  rules.

## Rendering and memory safety

- Preserve Clang-style `(type) value` output and use the custom bits/hex view
  only for supported integers. Let unsupported values fall back safely.
- Rendering and fingerprints must have global, shared budgets. Do not reset a
  256-element budget at each nesting level.
- Keep display text separate from watch/snapshot fingerprints. Never use a
  fixed placeholder such as `<value>` as the state identity of an object.
- Validate `std::vector` layout through the AST. Only interpret the supported
  `std::vector<T, std::allocator<T>>` native-pointer representation; otherwise
  use the Clang fallback.
- Enforce the 65,536-byte `%mem` limit after the final region size is known,
  including the default `sizeof(object)` path.
- Copy inspected memory into a local buffer before rendering. Linux uses
  `/proc/self/maps` plus `process_vm_readv()`; Windows uses `VirtualQuery` plus
  `ReadProcessMemory`. Both are best-effort, not debugger snapshots.
- Put platform-specific headers and implementations behind narrow `_WIN32` or
  POSIX guards. Linux behavior must not regress when extending Windows probes.

## Terminal behavior

Interactive color is enabled only for a displayed terminal when `NO_COLOR` is
absent and `TERM` is not `dumb`. Mark invisible prompt escapes with libedit's
`EL_PROMPT_ESC`; piped output and tests must remain plain text. During editing,
Ctrl-C must clear and redraw the same execution region without `^C`, history,
or Interpreter changes. Restore normal termios immediately outside line input.

Completion candidates must come from `ReplCodeCompleter` or the live Clang AST,
never a static keyword/name list. Persistent editor history and per-session
execution metadata are distinct: only submitted code enters the former, while
`%history`, `$n`, reset, and undo use the latter's documented semantics.

## Documentation and Git

Update `README.md` whenever commands, output, platform support, limits, ABI
assumptions, or LLVM coupling changes. Do not claim stronger portability or
memory safety than the tested implementation provides.

Use Conventional Commits, for example `fix: ...`, `feat: ...`, `test: ...`,
`build: ...`, and `docs: ...`. Keep unrelated changes in separate commits.
Before merging a PR, inspect review threads and CI, rebase or merge the latest
target branch when needed, rerun the local regression suite, and leave the
working tree clean.
