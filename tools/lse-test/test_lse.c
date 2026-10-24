/* Host test of osfmk/arm64/lse_emulate.c: the kernel's interfaces replaced by a fake user memory. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

typedef uint64_t user_addr_t;
typedef int64_t register_t;
typedef struct { uint64_t x[31], sp, pc; } arm_saved_state_t;

static inline bool is_saved_state64(const arm_saved_state_t *s) { (void)s; return true; }
static inline register_t get_saved_state_reg(const arm_saved_state_t *s, unsigned r) { return (register_t)(r < 31 ? s->x[r] : s->sp); }
static inline void set_saved_state_reg(arm_saved_state_t *s, unsigned r, register_t v) { if (r < 31) s->x[r] = (uint64_t)v; }
static inline uint64_t get_saved_state_sp(const arm_saved_state_t *s) { return s->sp; }
static inline uint64_t get_saved_state_pc(const arm_saved_state_t *s) { return s->pc; }
static inline void add_saved_state_pc(arm_saved_state_t *s, int d) { s->pc += d; }
#define TUNABLE(type, name, key, def) type name = def
#define os_atomic_inc_orig(p, o) ((*(p))++)
#define os_atomic_thread_fence(o) __atomic_thread_fence(__ATOMIC_SEQ_CST)

#define BASE 0x100000000ULL
static uint8_t mem[64];
static int stores;

static int check(user_addr_t a, unsigned n) { return (a < BASE || a + n > BASE + sizeof(mem)) ? EFAULT : ((a & (n - 1)) ? EINVAL : 0); }
int copyin_atomic32(user_addr_t a, uint32_t *v) { int e = check(a, 4); if (!e) memcpy(v, &mem[a - BASE], 4); return e; }
int copyin_atomic64(user_addr_t a, uint64_t *v) { int e = check(a, 8); if (!e) memcpy(v, &mem[a - BASE], 8); return e; }
int copy_cas_user32(user_addr_t a, uint32_t exp, uint32_t des, uint32_t *old)
{ int e = check(a, 4); if (e) return e; memcpy(old, &mem[a - BASE], 4); if (*old == exp) { memcpy(&mem[a - BASE], &des, 4); stores++; } return 0; }
int copy_cas_user64(user_addr_t a, uint64_t exp, uint64_t des, uint64_t *old)
{ int e = check(a, 8); if (e) return e; memcpy(old, &mem[a - BASE], 8); if (*old == exp) { memcpy(&mem[a - BASE], &des, 8); stores++; } return 0; }
int copy_casp_user64(user_addr_t a, const uint64_t exp[2], const uint64_t des[2], uint64_t old[2])
{ int e = check(a, 16); if (e) return e; memcpy(old, &mem[a - BASE], 16); if (old[0] == exp[0] && old[1] == exp[1]) { memcpy(&mem[a - BASE], des, 16); stores++; } return 0; }

#include "lse_emulate.c"

static int failures;
#define EXPECT(cond) do { if (!(cond)) { printf("  FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static arm_saved_state_t st;
static void reset(void) { memset(&st, 0, sizeof(st)); memset(mem, 0, sizeof(mem)); st.pc = 0x1000; stores = 0; }
static void put32(unsigned off, uint32_t v) { memcpy(&mem[off], &v, 4); }
static void put64(unsigned off, uint64_t v) { memcpy(&mem[off], &v, 8); }
static uint32_t get32(unsigned off) { uint32_t v; memcpy(&v, &mem[off], 4); return v; }
static uint64_t get64(unsigned off) { uint64_t v; memcpy(&v, &mem[off], 8); return v; }
static bool run(uint32_t insn, int *err) { return lse_emulate_user(&st, insn, err); }

#include "cases.h"

int main(void)
{
	run_cases();
	printf("%s (%d failures)\n", failures ? "FAILED" : "all passed", failures);
	return failures != 0;
}
