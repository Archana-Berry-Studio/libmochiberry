# dynalib

Dynamic loader API for the SEVM stack — the JavaScript face of the C99
`libdynalib` subsystem (PE/ELF export tables, ASLR-aware offsets, JIT
trampolines, W^X pages).

```js
import { dynalib } from './node_modules/dynalib/index.js';

const target = dynalib(file, reg_or_offset, symbol, abi_sig, opts);
```

## Two modes

**1. Direct** — `symbol` or `reg_or_offset` is given:

```js
const add = dynalib('dbtest.dll', 0, 'dbtest_add', 'i32(i32,i32)');
add.call(3, 4);          // -> 7   (a real native call)
add.ptr;                 // absolute address of the function
add.base;                // module base after ASLR
add.offset;              // RVA inside the module
add.abi;                 // parsed ABI description
add.close();
```

**2. Module** — both `reg_or_offset` and `symbol` are `null`:

```js
const mod = dynalib('C:/Python314/DLLs/sqlite3.dll', null, null);
mod.exports();           // [{ name, rva, ordinal }, ...]  (287 for sqlite3)
mod.base;                // mapped base address
mod.sym('sqlite3_libversion');   // -> direct-mode handle
mod.close();
```

## Parameters

| param | meaning |
|-------|---------|
| `file` | explicit path, or a short name searched in `mochi_modules/` (JS) and `./mochimodule/` (libdynalib's own C convention) |
| `reg_or_offset` | RVA relative to the module base (ASLR-compensated), or `null` |
| `symbol` | export name, or `null` for a pure offset jump |
| `abi_sig` | type contract: `"i32(i32,i32)"`, `"ptr()"`, `"u64(ptr,u32)"`, plus an optional ABI word (`"sysv"`, `"ms_abi"`, `"stdcall"`, `"syscall"`, or `"reg:rcx,rdx->rax"`) |
| `opts` | `{ rawAddr: true }` returns the raw address instead of a handle |

When both `offset` and `symbol` are given, the symbol is tried first and the
loader falls back to `base + offset` for stripped images.

## Limits (measured, not guessed)

- **Integer/pointer arguments only.** Floating-point arguments are rejected
  with an explicit error — the current trampoline emitter shuffles
  general-purpose registers only, so SSE arguments would be silently wrong.
- **At most 4 register arguments** (x86-64 SysV/MS64 have 4 GP argument
  registers); stack arguments are not forwarded.
- Return values come back as numbers; `i64`/`u64` results lose precision
  above 2^53.
- `opts.pinGc` and `opts.lazy` are accepted but not implemented yet.
