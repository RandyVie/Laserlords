/* Musashi glue: memory callbacks, TRAP and exception hooks. */
#include "ll.h"
#include "m68k.h"

const char *os9_cur_name(void);
uint32_t os9_cur_module(void);

#define EXC_STUBS 0x000400u   /* vector n points at EXC_STUBS + 2n, which holds ILLEGAL */

unsigned int m68k_read_memory_8(unsigned int a)  { return rd8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return rd16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return rd32(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return rd16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return rd32(a); }

uint32_t ll_watch_lo, ll_watch_hi;   /* debug: log CPU writes in [lo, hi) */

static void check_write(uint32_t a)
{
    a &= MEM_MASK;
    if (a >= ll_watch_lo && a < ll_watch_hi)
        ll_log("        !! watch: write %06x by %s pc %06x\n", a, os9_cur_name(), m68k_get_reg(NULL, M68K_REG_PPC));
    if (a < MODULE_BASE)
        ll_log("        !! write to low memory %06x (pc %06x)\n", a, m68k_get_reg(NULL, M68K_REG_PPC));
}
void m68k_write_memory_8(unsigned int a, unsigned int v)  { check_write(a); wr8(a, v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { check_write(a); wr16(a, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { check_write(a); wr32(a, v); }

int ll_trap_hook(int trap) { return os9_trap(trap); }

static const char *exc_name(int n)
{
    switch (n) {
    case 2: return "bus error"; case 3: return "address error"; case 4: return "illegal instruction";
    case 5: return "zero divide"; case 6: return "CHK"; case 7: return "TRAPV"; case 8: return "privilege";
    case 9: return "trace"; case 10: return "line A"; case 11: return "line F";
    }
    return n >= 32 && n < 48 ? "TRAP" : "exception";
}

void cpu_dump(const char *why)
{
    uint32_t base = os9_cur_module();
    uint32_t ppc = m68k_get_reg(NULL, M68K_REG_PPC);
    char dis[128];
    m68k_disassemble(dis, ppc, M68K_CPU_TYPE_68000);
    fprintf(stderr, "\n*** %s in %s at %06x (module offset %05x): %s\n", why, os9_cur_name(), ppc, ppc - base, dis);
    for (int i = 0; i < 8; i++)
        fprintf(stderr, "  D%d=%08x  A%d=%08x\n", i, m68k_get_reg(NULL, M68K_REG_D0 + i), i, m68k_get_reg(NULL, M68K_REG_A0 + i));
    fprintf(stderr, "  SR=%04x\n", m68k_get_reg(NULL, M68K_REG_SR));
    if (ll_trace && ll_trace != stderr)
        fprintf(ll_trace, "*** %s in %s at %06x (offset %05x): %s\n", why, os9_cur_name(), ppc, ppc - base, dis);
}

/* Unhandled exceptions arrive here: the vector table points every entry at an ILLEGAL stub. */
int ll_illegal_hook(int opcode)
{
    uint32_t pc = m68k_get_reg(NULL, M68K_REG_PPC);
    if (pc >= EXC_STUBS && pc < EXC_STUBS + 0x200) {
        int n = (pc - EXC_STUBS) / 2;
        /* stacked PC of the faulting instruction (format 0 frame: SR, PC) */
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
        m68k_set_reg(M68K_REG_PPC, rd32(sp + 2));
        char why[64];
        snprintf(why, sizeof why, "unhandled %s (vector %d)", exc_name(n), n);
        cpu_dump(why);
    } else {
        char why[64];
        snprintf(why, sizeof why, "ILLEGAL opcode %04x", opcode);
        cpu_dump(why);
    }
    ll_fatal("stopped");
    return 1;
}

void cpu_init(void)
{
    for (int n = 0; n < 256; n++) {
        wr32(n * 4, EXC_STUBS + n * 2);
        wr16(EXC_STUBS + n * 2, 0x4AFC);
    }
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
}
