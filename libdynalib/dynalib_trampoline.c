/*
 * dynalib_trampoline.c — JIT stubs for libdynalib.
 *
 * Emits the tiny executable pages the ABI layer needs:
 *
 *   - a call stub that forwards its first N register arguments to a target
 *     address (the `dynalib(file, offset, sym, abi)` path), and
 *   - a callback stub that stamps a slot id and jumps to the dispatcher the
 *     engine installed (the C-calls-back-into-JS path).
 *
 * Page discipline is W^X: the mapping is created writable, the bytes are
 * written, then it is flipped executable. The caller never sees a page that
 * is both, and the page lives outside the bump arena so the Sunat collector
 * cannot move it from under a live C loop.
 *
 * Status, stated plainly: the x86_64 emitters are exercised by the test
 * harness on this host (both MS64 and SysV). The AArch64 emitter is built
 * from the same table but has not been executed on an AArch64 host here, so
 * treat it as compile-clean, not run-verified.
 */
#include "dynalib.h"
#include "dynalib_internal.h"

#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <sys/types.h>
#endif

/* ================================================================== */
/* code buffer                                                       */
/* ================================================================== */

#define DYNALIB_MAX_STUB 512

typedef struct {
    unsigned char b[DYNALIB_MAX_STUB];
    size_t        n;
} CodeBuf;

static void cb_byte(CodeBuf *c, unsigned v)
{
    if (c->n < sizeof c->b) c->b[c->n++] = (unsigned char)v;
}

static void cb_u16(CodeBuf *c, unsigned v)
{
    cb_byte(c, v & 0xff);
    cb_byte(c, (v >> 8) & 0xff);
}

static void cb_u32(CodeBuf *c, unsigned v)
{
    cb_byte(c, v & 0xff);
    cb_byte(c, (v >> 8) & 0xff);
    cb_byte(c, (v >> 16) & 0xff);
    cb_byte(c, (v >> 24) & 0xff);
}

static void cb_u64(CodeBuf *c, uintptr_t v)
{
    cb_u32(c, (unsigned)v);
    cb_u32(c, (unsigned)(v >> 32));
}

/* 64-bit register encodings (x86_64) */
enum {
    X_RAX = 0, X_RCX = 1, X_RDX = 2, X_RBX = 3,
    X_RSP = 4, X_RBP = 5, X_RSI = 6, X_RDI = 7,
    X_R8 = 8, X_R9 = 9, X_R10 = 10, X_R11 = 11
};

/* physical slot order per convention (index = canonical slot ordinal) */
static int slot_reg(DynalibAbiKind kind, size_t slot)
{
    switch (kind) {
    case DYNALIB_ABI_MS64:
        switch (slot) {
        case 0: return X_RCX;
        case 1: return X_RDX;
        case 2: return X_R8;
        case 3: return X_R9;
        }
        return -1;
    case DYNALIB_ABI_SYSV:
        switch (slot) {
        case 0: return X_RDI;
        case 1: return X_RSI;
        case 2: return X_RDX;
        case 3: return X_RCX;
        case 4: return X_R8;
        case 5: return X_R9;
        }
        return -1;
    default:
        return -1;
    }
}

static DynalibAbiKind platform_default(void)
{
#if defined(_WIN32)
    return DYNALIB_ABI_MS64;
#else
    return DYNALIB_ABI_SYSV;
#endif
}

/* mov [base+disp32], r64 */
static void enc_mem_store_base(CodeBuf *c, int src, int base, int32_t disp)
{
    unsigned rex = 0x48;
    if (src >= 8) rex |= 0x04;                 /* REX.R — reg field */
    cb_byte(c, rex);
    cb_byte(c, 0x89);                          /* mov r/m64, r64 */
    /* mod=10 (disp32); r/m=101 -> rbp */
    if (base == X_RSP) {
        /* [rsp+disp] needs a SIB byte: index=none, base=rsp */
        cb_byte(c, 0x80 | ((src & 7) << 3) | 0x04);
        cb_byte(c, 0x24);                      /* scale=1, index=none, base=rsp */
    } else {
        cb_byte(c, 0x80 | ((src & 7) << 3) | 0x05);
    }
    cb_u32(c, (unsigned)disp);
}

/* mov [rbp+disp32], r64 */
static void enc_mem_store(CodeBuf *c, int src, int32_t disp)
{
    enc_mem_store_base(c, src, X_RBP, disp);
}

/* mov r64, [base+disp32] */
static void enc_mem_load_base(CodeBuf *c, int dst, int base, int32_t disp)
{
    unsigned rex = 0x48;
    if (dst >= 8) rex |= 0x04;                 /* REX.R */
    cb_byte(c, rex);
    cb_byte(c, 0x8B);                          /* mov r64, r/m64 */
    if (base == X_RSP) {
        cb_byte(c, 0x80 | ((dst & 7) << 3) | 0x04);
        cb_byte(c, 0x24);                      /* scale=1, index=none, base=rsp */
    } else {
        cb_byte(c, 0x80 | ((dst & 7) << 3) | 0x05);
    }
    cb_u32(c, (unsigned)disp);
}

/* mov r64, [rbp+disp32] */
static void enc_mem_load(CodeBuf *c, int dst, int32_t disp)
{
    enc_mem_load_base(c, dst, X_RBP, disp);
}

/* mov r64, imm64 (reg 0..7 without an extra REX.B) */
static void enc_reg_imm64(CodeBuf *c, int reg, uintptr_t v)
{
    cb_byte(c, 0x48 | (reg >= 8 ? 0x01 : 0x00));
    cb_byte(c, 0xB8 + (reg & 7));
    cb_u64(c, v);
}

/* mov r64, imm32 (C7 /0) — the target register lives in the ModRM r/m
 * field, so the extension bit is REX.B; REX.R would change the opcode
 * extension from /0 to /1 and break the instruction entirely.            */
static void enc_reg_imm32(CodeBuf *c, int reg, unsigned v)
{
    unsigned rex = 0x48;                    /* REX.W: 64-bit destination   */
    if (reg >= 8) rex |= 0x01;             /* REX.B — r/m field           */
    cb_byte(c, rex);
    cb_byte(c, 0xC7);
    cb_byte(c, 0xC0 | (reg & 7));
    cb_u32(c, v);
}

/* lea r64, [rbp+disp8] */
static void enc_lea_disp8(CodeBuf *c, int reg, int8_t disp)
{
    unsigned rex = 0x48;
    if (reg >= 8) rex |= 0x04;                 /* REX.R */
    cb_byte(c, rex);
    cb_byte(c, 0x8D);
    /* the register goes in the ModRM reg field, the base in r/m:
     * lea r64, [rbp+disp8] = mod=01, reg=reg, r/m=rbp                     */
    cb_byte(c, 0x40 | ((reg & 7) << 3) | 0x05);
    cb_byte(c, disp & 0xff);
}

/* ================================================================== */
/* W^X page management                                                 */
/* ================================================================== */

#define DYNALIB_STUB_SLOTS 16

typedef struct {
    void   *base;
    size_t  size;
} DynalibStubRec;

static DynalibStubRec g_stubs[DYNALIB_STUB_SLOTS];
static size_t         g_stub_top = 0;

static void *stub_alloc(size_t size, void **pend)
{
    void *p;
#if defined(_WIN32)
    p = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    p = mmap(NULL, size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) p = NULL;
#endif
    if (!p)
        return NULL;
    if (g_stub_top < DYNALIB_STUB_SLOTS) {
        g_stubs[g_stub_top].base = p;
        g_stubs[g_stub_top].size = size;
        g_stub_top++;
    }
    if (pend) *pend = p;
    return p;
}

static void *stub_exec(void *p)
{
#if defined(_WIN32)
    DWORD old;
    if (!VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old))
        return NULL;
#else
    if (mprotect(p, 4096, PROT_READ | PROT_EXEC) != 0)
        return NULL;
#endif
    return p;
}

void dynalib_trampoline_free(void *stub, void *pend)
{
    size_t i;
    (void)pend;
    for (i = 0; i < g_stub_top; i++) {
        if (g_stubs[i].base == stub) {
#if defined(_WIN32)
            VirtualFree(stub, 0, MEM_RELEASE);
#else
            munmap(stub, g_stubs[i].size);
#endif
            g_stubs[i] = g_stubs[--g_stub_top];
            return;
        }
    }
}

/* ================================================================== */
/* call stub: forward N register args + stack args to `target`       */
/* ================================================================== */

/*
 * Stack-argument layout (measured on this host, MS64):
 *
 *   at function entry       [rsp]      = return address
 *                           [rsp+0x08] = shadow space (32 bytes)
 *                           [rsp+0x28] = first stack argument (arg 5)
 *
 * MS64 probe result: &arg5 - return_address_slot = 0x28 exactly.
 * SysV has no shadow space: the first stack argument (arg 7) sits at
 * [rsp+0x08] at entry.
 *
 * The stub therefore has to:
 *   1. reserve the callee's shadow space (MS64) and the outgoing stack
 *      argument area,
 *   2. copy the incoming stack arguments into the outgoing area,
 *   3. load the register arguments,
 *   4. call the target with a 16-byte aligned stack.
 */
static void emit_call_stub(CodeBuf *c, const void *target, DynalibAbiKind kind,
                           const DynalibAbiSpec *abi, size_t n)
{
    uintptr_t frame;
    size_t k, nreg = 0, kstack = 0;
    int dst;
    int32_t in_off, out_off;
    int custom = (abi && abi->kind == DYNALIB_ABI_CUSTOM_REG);

    /* how many arguments travel in registers */
    for (k = 0; k < n; k++) {
        int inc = slot_reg(kind, k);
        if (inc < 0) break;
        nreg++;
    }
    if (custom)
        nreg = n;               /* custom map: every slot is a register */

    kstack = n - nreg;

    /* frame: saved register args + callee shadow + outgoing stack args */
    frame = 8 * nreg;
    if (kind == DYNALIB_ABI_MS64)
        frame += 32;                          /* callee shadow space       */
    frame += 8 * kstack;                     /* outgoing stack arguments  */
    if (frame % 16)
        frame += (16 - (frame % 16));

    cb_byte(c, 0x55);                          /* push rbp               */
    /* rbp must be set up, not just pushed: the epilogue rebuilds rsp
     * from it, so a stale caller rbp would steer the return address    */
    cb_byte(c, 0x48); cb_byte(c, 0x89); cb_byte(c, 0xE5);  /* mov rbp, rsp */
    {
        unsigned f32 = (unsigned)frame;
        cb_byte(c, 0x48); cb_byte(c, 0x81); cb_byte(c, 0xEC);
        cb_u32(c, f32);                       /* sub rsp, frame         */
    }

    /* save the incoming register args, canonical slot order */
    for (k = 0; k < nreg; k++) {
        int inc = slot_reg(kind, k);
        if (inc < 0) break;
        enc_mem_store(c, inc, (int32_t)-8 * (int)(k + 1));
    }

    /*
     * Copy the incoming stack arguments into the outgoing area.
     *   MS64: source [rbp+0x30 + 8i], destination [rsp + 0x20 + 8i]
     *   SysV: source [rbp+0x10 + 8i], destination [rsp + 0x00 + 8i]
     *
     * The copy runs AFTER `sub rsp, frame`, so rsp already points at the
     * outgoing frame base (the value it will have at the `call`).
     */
    if (kstack) {
        in_off  = (kind == DYNALIB_ABI_MS64) ? 0x30 : 0x10;
        out_off = (kind == DYNALIB_ABI_MS64) ? 0x20 : 0x00;
        for (k = 0; k < kstack; k++) {
            int32_t src = (int32_t)(in_off + 8 * k);
            int32_t dst_disp = (int32_t)(out_off + 8 * k);
            enc_mem_load_base(c, X_R11, X_RBP, src);
            enc_mem_store_base(c, X_R11, X_RSP, dst_disp);
        }
    }

    /* load them into the target registers */
    for (k = 0; k < nreg; k++) {
        int32_t disp = (int32_t)-8 * (int)(k + 1);
        if (custom) {
            int mapped = slot_reg(kind, abi->arg_regs[k]);
            if (mapped < 0) break;
            dst = mapped;
        } else {
            dst = slot_reg(kind, k);
            if (dst < 0) break;
        }
        enc_mem_load(c, dst, disp);
    }

    enc_reg_imm64(c, X_RAX, (uintptr_t)target);  /* mov rax, target      */
    cb_byte(c, 0xFF); cb_byte(c, 0xD0);          /* call rax             */

    /* after `push rbp; mov rbp, rsp`, rbp already points at the saved
     * frame-pointer slot, so rsp must return there exactly: one slot
     * further down and `ret` pops the saved rbp instead of the address   */
    enc_lea_disp8(c, X_RSP, 0);                   /* lea rsp,[rbp]        */
    cb_byte(c, 0x5D);                            /* pop rbp              */
    cb_byte(c, 0xC3);                            /* ret                  */
}

void *dynalib_trampoline_emit(const void *target, const DynalibAbiSpec *abi,
                              void **pend)
{
    CodeBuf c;
    DynalibAbiKind kind;
    size_t n = 0;
    void *p;
    if (!target)
        return NULL;
    kind = abi ? abi->kind : platform_default();
    if (kind == DYNALIB_ABI_DEFAULT)
        kind = platform_default();
    if (abi)
        n = abi->arg_count;
    if (n > 16)
        n = 16;
    memset(&c, 0, sizeof c);
    emit_call_stub(&c, target, kind, abi, n);
    p = stub_alloc((size_t)4096, pend);
    if (!p)
        return NULL;
    memcpy(p, c.b, c.n);
    return stub_exec(p);
}

/* ================================================================== */
/* callback stub: stamp a slot id, hand the args to the dispatcher    */
/* ================================================================== */

static void emit_cb_stub(CodeBuf *c, uintptr_t slot_id, const void *disp,
                         DynalibAbiKind kind, size_t n)
{
    uintptr_t frame;
    size_t k;
    int s0, s1, s2;
    int arg_regn[8];

    frame = 8 * n;
    if (kind == DYNALIB_ABI_MS64)
        frame += 32;
    if (frame % 16)
        frame += (16 - (frame % 16));

    s0 = slot_reg(kind, 0);
    s1 = slot_reg(kind, 1);
    s2 = slot_reg(kind, 2);
    if (s0 < 0 || s1 < 0 || s2 < 0)
        return;
    for (k = 0; k < n && k < 8; k++)
        arg_regn[k] = slot_reg(kind, k);

    cb_byte(c, 0x55);                          /* push rbp                 */
    cb_byte(c, 0x48); cb_byte(c, 0x89); cb_byte(c, 0xE5); /* mov rbp, rsp    */
    {
        unsigned f32 = (unsigned)frame;
        cb_byte(c, 0x48); cb_byte(c, 0x81); cb_byte(c, 0xEC);
        cb_u32(c, f32);                       /* sub rsp, frame           */
    }

    /* park the native args at [rbp-8n .. rbp) so the dispatcher can read
     * them as a flat array in canonical slot order: slot 0 goes in the
     * LOWEST slot, because the array base is &base[0]                    */
    for (k = 0; k < n && k < 8; k++) {
        if (arg_regn[k] < 0) break;
        enc_mem_store(c, arg_regn[k], (int32_t)(-8 * (int)(n - k)));
    }

    /* then lay them out contiguously at [rbp-8n .. rbp-8) — already so by
     * construction when n is contiguous, so the array base is the disp
     * of the last saved slot                                           */
    enc_reg_imm64(c, s0, slot_id);             /* slot id                 */
    if (n > 0)
        enc_lea_disp8(c, s1, (int8_t)-8 * (int)n);  /* args = &slot1      */
    else
        enc_reg_imm32(c, s1, 0);
    enc_reg_imm32(c, s2, (unsigned)n);        /* argc                    */

    enc_reg_imm64(c, X_RAX, (uintptr_t)disp);
    cb_byte(c, 0xFF); cb_byte(c, 0xD0);       /* call rax                */

    enc_lea_disp8(c, X_RSP, 0);
    cb_byte(c, 0x5D);                        /* pop rbp                 */
    cb_byte(c, 0xC3);                        /* ret                     */
}

void *dynalib_trampoline_cb(uintptr_t slot_id, const void *dispatcher,
                            const DynalibAbiSpec *abi, void **pend)
{
    CodeBuf c;
    DynalibAbiKind kind;
    void *p;
    if (!dispatcher)
        return NULL;
    kind = abi ? abi->kind : platform_default();
    if (kind == DYNALIB_ABI_DEFAULT)
        kind = platform_default();
    memset(&c, 0, sizeof c);
    emit_cb_stub(&c, slot_id, dispatcher, kind, abi ? abi->arg_count : 0);
    p = stub_alloc((size_t)4096, pend);
    if (!p)
        return NULL;
    memcpy(p, c.b, c.n);
    return stub_exec(p);
}
