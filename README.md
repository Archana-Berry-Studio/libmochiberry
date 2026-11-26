# libmochiberry

Native C libraries for the **MochiBerry** engine (SEVM stack).

The engine stays small (< 1.2 MB); everything native lives here and is
loaded at runtime — no recompiling the engine, no V8, no node-gyp.

```
libmochiberry/
├── libdynalib/       C99 dynamic loader: PE/ELF export tables,
│                     ASLR-aware RVAs, JIT call/callback trampolines
│   ├── dynalib.c           module open/close, export enumeration
│   ├── dynalib_abi.c       calling-convention grammar (ms64/sysv/custom)
│   ├── dynalib_trampoline.c  x86-64 machine-code emitter (W^X pages)
│   ├── dynalib.h           public API
│   └── tests/              86 assertions, fixtures, repro tools
├── mochinode/        MochiBerry engine plugin bridges (C)
│   ├── mbjs_runtime.h      runtime symbol bootstrap (no mbjs.exe import!)
│   ├── mochinode_dynalib.c exposes libdynalib to JavaScript
│   ├── mochinode_sqlite.c  SQLite bridge
│   └── mochinode_net.c     TCP/HTTP bridge
└── js/               the JavaScript face of the dynalib bridge
    └── index.js      dynalib(file, reg_or_offset, symbol, abi_sig, opts)
```

## Why this exists

Before `mochinode_dynalib.dll`, every new system library needed a hand
written C wrapper: a new `.c` file, `AddModuleExportList`, a recompile.
With the bridge in place, JavaScript can open any `.dll` / `.so` / `.lbrct`,
read its export table, compute ASLR-compensated offsets and call into it —
at runtime, with no C glue.

```js
import { dynalib } from './js/index.js';

const tick = dynalib('C:/Windows/System32/kernel32.dll', 0,
                     'GetTickCount64', 'u64()');
tick.call();                    // -> 182347031n  (BigInt, real Win32 call)

const metrics = dynalib('C:/Windows/System32/user32.dll', 0,
                         'GetSystemMetrics', 'i32(i32)');
metrics.call(0);                // -> 1143  (screen width)
```

## Build

```bash
# the library + its test harness (86 assertions)
cd libdynalib && ./build.sh

# the MochiBerry plugin bridge (needs the engine headers)
gcc -shared -O2 -o mochinode_dynalib.dll \
    mochinode/mochinode_dynalib.c \
    libdynalib/dynalib.c libdynalib/dynalib_abi.c libdynalib/dynalib_trampoline.c \
    -I<engine-source-dir> -Ilibdynalib
```

The engine must export the `MBJS_*` symbols (`build_manual/mbjs.exe` does;
the stripped `tier-core` build does not, so plugins cannot load there).

## What is verified, and what is not

Verified by measurement on this host (Windows x64, gcc 15.1 / WinLibs):

- 107/107 assertions pass in the library harness.
- Stack arguments work: 5, 7, 8 and 12 argument calls return the right
  values, and the slot order is preserved (`mix7(1..7) == 1234567`).
- 64-bit returns arrive in JavaScript as `BigInt` (`u64` → BigInt,
  `i64` → BigInt, `void` → `undefined`).
- Real Win32 calls succeed from JavaScript with no C wrapper:
  `GetTickCount64`, `GetCurrentProcessId`, `GetCurrentThreadId`,
  `GetSystemMetrics`, `GetVersion`.
- The MS64 stack-argument offset was measured, not assumed: the 5th
  argument sits at `[entry_rsp + 0x28]` (32 bytes of shadow space first).
- `[rsp+disp]` needs a SIB byte (`0x24`) — without it the CPU reads the
  displacement as a SIB and the store lands on a garbage address.

Honest limits:

- **Floating-point arguments and returns are rejected** with an explicit
  error. The emitter shuffles general-purpose registers only; an SSE
  argument would be silently wrong, so it is refused instead.
- **At most 16 arguments** per call (12 are exposed through the JS
  bridge), because the ABI contract table holds 16 entries.
- `i64`/`u64` **arguments** should be passed as `BigInt`; a JS Number loses
  precision above 2^53.
- Custom register maps (`reg:rcx,rdx->rax`) cover register slots only.
- The AArch64 emitter is compile-clean but has not been run on an AArch64
  host.
- Live-mapping an ELF image and parsing its dynamic exports is not
  implemented; PE export tables are.

## License

MIT — see `LICENSE`.

# Mochi Berry
![Mochi Bewwy](archanaberry/mochiberry.png)

# Mochi Berry Chan (Mascot Chan)
![mochi chan](archanaberry/mochichan.png)
