/*
 * Emulation of the ARMv8.1 atomic instructions (FEAT_LSE) for user space, on cores that do not
 * have them (the Cortex-A53/A73 class: Snapdragon 665 and other ARMv8.0 SoCs).
 *
 * iOS 16 is built for the A11 and newer, which all have LSE, and some of its libraries use the
 * instructions without asking the commpage first (libdispatch, libsystem_pthread,
 * libsystem_platform, libsystem_malloc: about a thousand places in the shared cache). On an
 * ARMv8.0 core each of them is an undefined instruction. handle_uncategorized() (sleh.c) hands
 * those to lse_emulate_user(), which does the operation with load/store-exclusive and steps over
 * the instruction.
 *
 * Every operation becomes a compare-and-swap loop on the aligned 32- or 64-bit word that holds
 * the operand (copy_cas_user32/64, copyio.c), with a full barrier on both sides: atomic, and at
 * least as ordered as any acquire/release variant. Bytes and halfwords are swapped inside their
 * word. CASP of two 64-bit registers uses a load/store-exclusive pair (copy_casp_user64).
 *
 * Boot-arg lse_emul=0 turns it off (the instructions then crash the process as before).
 */

#include <arm/thread.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/startup.h>
#include <mach/machine/thread_status.h>
#include <os/atomic_private.h>
#include <sys/errno.h>

extern int copy_cas_user32(user_addr_t user_addr, uint32_t expected, uint32_t desired, uint32_t *old);
extern int copy_cas_user64(user_addr_t user_addr, uint64_t expected, uint64_t desired, uint64_t *old);
extern int copy_casp_user64(user_addr_t user_addr, const uint64_t expected[2], const uint64_t desired[2],
    uint64_t old[2]);

bool lse_emulate_user(arm_saved_state_t *state, uint32_t instr, int *error);

static TUNABLE(uint32_t, lse_emul_enabled, "lse_emul", 1);

/* How many instructions were emulated; the first few are logged, to show it is happening. */
static uint64_t lse_emul_count;
#define LSE_EMUL_LOGGED 8

/* The operations of LDADD and friends (opc), SWP, and CAS. */
enum lse_op {
	LSE_ADD, LSE_CLR, LSE_EOR, LSE_SET, LSE_SMAX, LSE_SMIN, LSE_UMAX, LSE_UMIN,
	LSE_SWP, LSE_CAS,
};

/* Register n of the instruction as a source: 31 is the zero register. */
static uint64_t
reg_or_zero(const arm_saved_state_t *state, unsigned n)
{
	return n == 31 ? 0 : (uint64_t)get_saved_state_reg(state, n);
}

/* ... and as a destination: writes to 31 are thrown away (the ST* aliases of the LD* forms). */
static void
set_reg_or_zero(arm_saved_state_t *state, unsigned n, uint64_t value)
{
	if (n != 31) {
		set_saved_state_reg(state, n, (register_t)value);
	}
}

/* Rn as a base register: 31 is the stack pointer. */
static uint64_t
base_register(const arm_saved_state_t *state, unsigned n)
{
	return n == 31 ? get_saved_state_sp(state) : (uint64_t)get_saved_state_reg(state, n);
}

static int64_t
sign_extend(uint64_t value, unsigned bits)
{
	return bits == 64 ? (int64_t)value : (int64_t)(value << (64 - bits)) >> (64 - bits);
}

/* What the memory holds after the operation, given what it held (old) and the register (operand). */
static uint64_t
lse_apply(enum lse_op op, uint64_t old, uint64_t operand, unsigned bits)
{
	switch (op) {
	case LSE_ADD:  return old + operand;
	case LSE_CLR:  return old & ~operand;
	case LSE_EOR:  return old ^ operand;
	case LSE_SET:  return old | operand;
	case LSE_SMAX: return sign_extend(old, bits) > sign_extend(operand, bits) ? old : operand;
	case LSE_SMIN: return sign_extend(old, bits) < sign_extend(operand, bits) ? old : operand;
	case LSE_UMAX: return old > operand ? old : operand;
	case LSE_UMIN: return old < operand ? old : operand;
	case LSE_SWP:
	case LSE_CAS:  return operand;
	}
	return old;
}

/*
 * One atomic read-modify-write of `bits` bits at addr. For LSE_CAS, compare is what the memory
 * must hold for the store to happen. *old gets what the memory held, zero-extended.
 */
static int
lse_rmw(user_addr_t addr, unsigned bits, enum lse_op op, uint64_t operand, uint64_t compare, uint64_t *old)
{
	uint64_t mask = bits == 64 ? ~0ULL : (1ULL << bits) - 1;
	int error;

	if (addr & (bits / 8 - 1)) {
		return EINVAL;  /* misaligned: an alignment fault on ARMv8.1 without LSE2 too */
	}
	operand &= mask;
	compare &= mask;

	if (bits == 64) {
		uint64_t seen, expected;

		error = copyin_atomic64(addr, &expected);
		for (; error == 0; expected = seen) {
			if (op == LSE_CAS && expected != compare) {
				*old = expected;  /* no store: the read was atomic, as a failed CAS's is */
				return 0;
			}
			error = copy_cas_user64(addr, expected, lse_apply(op, expected, operand, 64), &seen);
			if (error == 0 && seen == expected) {
				*old = seen;
				return 0;
			}
		}
		return error;
	} else {
		/* 8, 16 and 32 bits: the aligned word around them, little-endian */
		user_addr_t word = addr & ~(user_addr_t)3;
		unsigned shift = (unsigned)(addr & 3) * 8;
		uint32_t seen, expected, value;

		error = copyin_atomic32(word, &expected);
		for (; error == 0; expected = seen) {
			value = (uint32_t)((expected >> shift) & mask);
			if (op == LSE_CAS && value != compare) {
				*old = value;
				return 0;
			}
			uint32_t updated = (uint32_t)(lse_apply(op, value, operand, bits) & mask);
			uint32_t desired = (expected & ~(uint32_t)(mask << shift)) | (updated << shift);
			error = copy_cas_user32(word, expected, desired, &seen);
			if (error == 0 && seen == expected) {
				*old = value;
				return 0;
			}
		}
		return error;
	}
}

/* An aligned load of `bits` bits, single-copy atomic, followed by a barrier (the acquire). */
static int
lse_load(user_addr_t addr, unsigned bits, uint64_t *value)
{
	int error;

	if (addr & (bits / 8 - 1)) {
		return EINVAL;
	}
	if (bits == 64) {
		error = copyin_atomic64(addr, value);
	} else {
		uint32_t word;

		error = copyin_atomic32(addr & ~(user_addr_t)3, &word);
		*value = (word >> ((addr & 3) * 8)) & (bits == 32 ? 0xFFFFFFFFULL : (1ULL << bits) - 1);
	}
	os_atomic_thread_fence(seq_cst);
	return error;
}

/*
 * The instruction at the user thread's pc, which the core did not know. true: it was an LSE
 * instruction, it has been done and the pc moved past it. false with *error set: it was one but
 * its memory access failed (EFAULT, or EINVAL for a misaligned address). false with *error 0:
 * it is not one of ours.
 */
bool
lse_emulate_user(arm_saved_state_t *state, uint32_t instr, int *error)
{
	unsigned rs = (instr >> 16) & 31, rn = (instr >> 5) & 31, rt = instr & 31;
	user_addr_t addr;
	uint64_t old;

	*error = 0;
	if (!lse_emul_enabled || !is_saved_state64(state)) {
		return false;
	}
	addr = (user_addr_t)base_register(state, rn);

	if ((instr & 0x3FA07C00) == 0x08A07C00) {
		/* CAS{A,L,AL}{B,H}: Rs is compared with memory, Rt stored, Rs gets what was there */
		unsigned bits = 8u << (instr >> 30);

		*error = lse_rmw(addr, bits, LSE_CAS, reg_or_zero(state, rt), reg_or_zero(state, rs), &old);
		if (*error) {
			return false;
		}
		set_reg_or_zero(state, rs, old);
	} else if ((instr & 0xBFA07C00) == 0x08207C00) {
		/* CASP{A,L,AL}: the same with the register pairs Rs, Rs+1 and Rt, Rt+1 (both even) */
		if ((rs & 1) || (rt & 1)) {
			return false;  /* unallocated: let it be the undefined instruction it is */
		}
		if (instr & (1u << 30)) {
			uint64_t expected[2] = { reg_or_zero(state, rs), reg_or_zero(state, rs + 1) };
			uint64_t desired[2] = { reg_or_zero(state, rt), reg_or_zero(state, rt + 1) };
			uint64_t seen[2];

			if (addr & 15) {
				*error = EINVAL;
				return false;
			}
			*error = copy_casp_user64(addr, expected, desired, seen);
			if (*error) {
				return false;
			}
			set_reg_or_zero(state, rs, seen[0]);
			set_reg_or_zero(state, rs + 1, seen[1]);
		} else {
			/* two 32-bit registers: one 64-bit compare-and-swap, the first register at the lower address */
			uint64_t compare = (reg_or_zero(state, rs) & 0xFFFFFFFF) | (reg_or_zero(state, rs + 1) << 32);
			uint64_t store = (reg_or_zero(state, rt) & 0xFFFFFFFF) | (reg_or_zero(state, rt + 1) << 32);

			*error = lse_rmw(addr, 64, LSE_CAS, store, compare, &old);
			if (*error) {
				return false;
			}
			set_reg_or_zero(state, rs, old & 0xFFFFFFFF);
			set_reg_or_zero(state, rs + 1, old >> 32);
		}
	} else if ((instr & 0x3F200C00) == 0x38200000) {
		/* LD<op>{A,L,AL}{B,H}, SWP{A,L,AL}{B,H}: Rs is the operand, Rt gets what was there */
		unsigned bits = 8u << (instr >> 30), opc = (instr >> 12) & 7;
		bool o3 = instr & (1u << 15);
		enum lse_op op;

		if (!o3) {
			op = (enum lse_op)opc;
		} else if (opc == 0) {
			op = LSE_SWP;
		} else if (opc == 4 && rs == 31 && !(instr & (1u << 22))) {
			/* LDAPR{B,H} (ARMv8.3): a load-acquire, of memory that may well be read-only */
			*error = lse_load(addr, bits, &old);
			if (*error) {
				return false;
			}
			set_reg_or_zero(state, rt, old);
			goto done;
		} else {
			return false;  /* unallocated encodings */
		}
		*error = lse_rmw(addr, bits, op, reg_or_zero(state, rs), 0, &old);
		if (*error) {
			return false;
		}
		set_reg_or_zero(state, rt, old);
	} else {
		return false;
	}

done:;
	uint64_t n = os_atomic_inc_orig(&lse_emul_count, relaxed);
	if (n < LSE_EMUL_LOGGED) {
		printf("lse_emul: %08x at 0x%llx done (%llu so far)\n", instr,
		    (unsigned long long)get_saved_state_pc(state), (unsigned long long)n + 1);
	}
	add_saved_state_pc(state, 4);
	return true;
}
