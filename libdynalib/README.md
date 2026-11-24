# libdynalib

A C11 dynamic-linking subsystem for the MochiBerry / SEVM engine family. One API
covers loading a container, extracting its export table, compensating for ASLR,
and emitting an ABI-correct trampoline — so the JS-side `dynalib(...)` call can
reach into a `.so`, a `.dll`, a host executable's export directory, or a raw
file offset, without the engine itself being recompiled.

This is deliberately freestanding-friendly C: no third-party dependency, no
allocator hook, and the trampoline pages live outside the bump arena so the
Sunat collector cannot move them out from under a running C loop.

## Layout

| file | lines | role |
| --- | --- | --- |
| `dynalib.h` | 271 | public C API |
| `dynalib.c` | 669 | load/resolve, PE + ELF export parsers, anti-ASLR base, `exports()` |
| `dynalib_abi.c` | 340 | calling-convention grammar, slot order, pinning, reverse callbacks |
| `dynalib_trampoline.c` | 394 | x86_64 / AArch64 JIT stub emitters on W^X pages |
| `dynalib_internal.h` | 37 | interface between the four parts above |
| `tests/dynalib_test.c` | 529 | T1–T11 harness |
| `tests/dbtest.c` | 37 | throwaway DLL fixture with args, a return value and a native caller |
| `tests/dbg.c`, `stub_dump.c`, `crash_locate.c` | 262 | diagnostic tools |

## Build and test

```sh
bash build.sh
```

needs a native-PE gcc (on this host: WinLibs MCF with `gcc -shared`). Point at
a different SQLite build with `SQLITE_DLL=/path/sqlite3.dll bash build.sh`.

Current state on this host: **60 passed, 0 failed.**

## What the tests actually exercise

- **T1** — PE export parsing over a real `sqlite3.dll`: 287 exports,
  `sqlite3_libversion` found by name, every entry carrying the ordinal the
  loader would use.
- **T2** — `dynalib_open` + `dynalib_resolve` + a live call:
  `sqlite3_libversion()` returned **`3.50.4`** and
  `sqlite3_libversion_number()` returned a sane value.
- **T3** — offset-only resolution (the stripped path) yields the same address
  as the symbol path, which is the point of the anti-ASLR base.
- **T4** — `dynalib_exports()` enumerating from the *live mapping*, i.e. RVAs
  resolved as `base + rva` rather than through file offsets.
- **T5/T6** — call stubs emitted onto a W^X page: `stub(3,4) == 7`,
  `stub(41,1) == 42`, `stub(-5,5) == 0`; plus a zero-argument stub returning a
  string pointer.
- **T7** — the reverse path: C calling back through a callback stub into the
  engine-installed dispatcher, with the slot id, `argc`, and both argument
  values arriving correctly.
- **T8** — calling-convention grammar (`sysv`, `ms_abi`, `stdcall`,
  `u64(ptr, u32, u32)`, `void(f32, f32)`, `i32(str)`, and the custom
  `reg:rcx,rdx->rax` form), plus rejection of malformed input.
- **T9** — GC pinning with a counting hook: zero-copy, idempotent re-pin.
- **T10** — ELF `.dynsym` parsing over a synthetic `ET_DYN` image built in
  memory: both symbols, both RVAs and the base RVA.
- **T11** — container sniffing, and `.lbrct` being reported as a documented
  hook rather than passing silently.

## Container formats

| format | status |
| --- | --- |
| PE / COFF (`.dll`, `.exe`) | run-verified, on-disk *and* live mapping |
| ELF (`.so`, `ET_DYN`, PIE) | run-verified for the on-disk `.dynsym` path |
| `.lbrct` (UBerryNix / CherryReVA) | **documented hook**: `dynalib_detect_format` recognises the magic, but the parser is a deliberate stub that returns an error string, so nobody mistakes "not implemented" for "loaded fine" |
| direct syscall table | not implemented |

## Stated limitations

- The **AArch64 emitter** is built from the same slot tables and compiles
  clean, but has **not** been executed on an AArch64 host here. Treat it as
  compile-clean, not run-verified.
- A **live ELF mapping** is not supported: an exported symbol's address in a
  loaded `ET_DYN` still needs its `PT_DYNAMIC` / `DT_*` entries relocated,
  which is reported as an error rather than guessed.
- The x86_64 emitters use `mov rbp,rsp` and restore the frame from it; the
  epilogue displacement is sensitive to whether that store exists, so the two
  must change together.

## Files worth knowing

`tests/dbg.exe pe <file>` prints the section table and export directory of a
real DLL; `tests/dbg.exe elf <file>` walks `PT_DYNAMIC` and `DT_*`. Those were
the fastest way to distinguish "my offsets are wrong" from "my test image is
wrong", and they remain the first thing to run when a parser regresses.
