/* Atomic operations */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_ATOMIC_H
#define SPA_ATOMIC_H

#ifdef __cplusplus
extern "C" {
#endif

/** \defgroup spa_atomic Atomic access primitives
 *
 *
 * \brief Primitives for atomic operations
 *
 * Atomic operations are useful in a number of cases where traditional
 * mutex locks may be too expensive or otherwise problematic.
 *
 * Most SPA atomic primitives use sequentially consistent ordering (seq_cst,
 * also called total ordering). Note that this makes each individual
 * operation an acquire point (loads) or release point (stores) that also
 * participates in a single global order. It does not make each operation a
 * standalone full memory barrier. A full bidirectional barrier would require
 * `__atomic_thread_fence(__ATOMIC_SEQ_CST)`.
 *
 * Basic atomic primitives are provided for the following operations:
 *
 * - Load & store
 * - Increment & decrement (returning the newly incremented/decremented value)
 * - Exchange
 * - Compare-and-swap (CAS)
 *
 * In addition, primitives that implement a sequence lock ("seqlock")
 * mechanism are present.  Seqlocks are useful for scenarios with 1
 * writer and >=1 readers. Example:
 *
 * Writer:
 *
 * \code{.c}
 * SPA_SEQ_WRITE(s);
 * // Perform write operations here
 * SPA_SEQ_WRITE(s);
 * \endcode
 *
 * Reader:
 *
 * \code{.c}
 * do {
 *   s1 = SPA_SEQ_READ(s);
 *   // Perform read operations here
 *   s2 = SPA_SEQ_READ(s);
 * } while (!SPA_SEQ_READ_SUCCESS(s1, s2));
 * \endcode
 *
 * Sequence numbers (`s`, `s1`, `s2` in the examples above) must be
 * of an unsigned integer type, like `uintptr_t`.
 *
 * The \ref SPA_SEQ_WRITE and \ref SPA_SEQ_READ macros both enforce CST
 * ordering. \ref SPA_SEQ_WRITE is a read-modify-write and thus a combined
 * release/acquire point, so the two writer increments fully enclose the
 * protected stores: none can be reordered before the opening increment or
 * after the closing one. \ref SPA_SEQ_READ is a load and thus an acquire
 * point only.
 *
 * \note The opening read keeps the protected loads from being raised above it,
 * but an acquire load does not generally, on its own, keep earlier loads from
 * being reordered after the closing read. Correctness of the reader therefore
 * also relies on the protected loads completing before the closing
 * \ref SPA_SEQ_READ observes the sequence number. This holds for the plain
 * (non-atomic) accesses used inside seqlock sections on the architectures
 * PipeWire targets, but it is not guaranteed by the load's acquire semantics
 * alone.
 *
 * No locks are involved. Readers detect an inconsistent state via a sequence
 * number mismatch, and try again in such a situation.
 *
 * (Note that more than 1 writer is a problematic case -
 * \ref SPA_SEQ_WRITE_SUCCESS is meant for diagnostics only.) */
/** @{ */

/**
 * Performs a compare-and-swap (CAS) operation.
 *
 * The given atomic variable's current value is compared with `ov`. If these
 * match, the variable's value is set to `nv`, and true is returned. Otherwise,
 * the variable's value remains unchanged, and false is returned. This entire
 * sequence happens atomically.
 *
 * This macro is guaranteed to enforce sequentially consistent (CST) ordering. */
#define SPA_ATOMIC_CAS(v,ov,nv)						\
({									\
	__typeof__(v) __ov = (ov);					\
	__atomic_compare_exchange_n(&(v), &__ov, (nv),			\
			0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);		\
})

/**
 * Atomically decreases the value in the given variable and then returns the decremented value.
 *
 * This macro is guaranteed to enforce sequentially consistent (CST) ordering.
 */
#define SPA_ATOMIC_DEC(s)		__atomic_sub_fetch(&(s), 1, __ATOMIC_SEQ_CST)

/**
 * Atomically increases the value in the given variable and then returns the incremented value.
 *
 * This macro is guaranteed to enforce sequentially consistent (CST) ordering.
 */
#define SPA_ATOMIC_INC(s)		__atomic_add_fetch(&(s), 1, __ATOMIC_SEQ_CST)

/**
 * Atomically loads the value of the given variable and then returns the value.
 *
 * This macro is guaranteed to enforce sequentially consistent (CST) ordering.
 */
#define SPA_ATOMIC_LOAD(s)		__atomic_load_n(&(s), __ATOMIC_SEQ_CST)

/**
 * Reads the given variable exactly once, with relaxed ordering.
 */
#define SPA_LOAD_ONCE(s)		__atomic_load_n(&(s), __ATOMIC_RELAXED)

/**
 * Writes `v` into the given variable exactly once, with relaxed ordering.
 */
#define SPA_STORE_ONCE(s,v)		__atomic_store_n(&(s), (v), __ATOMIC_RELAXED)

/**
 * Atomically stores the given value into the given variable.
 *
 * This macro is guaranteed to enforce sequentially consistent (CST) ordering.
 */
#define SPA_ATOMIC_STORE(s,v)		__atomic_store_n(&(s), (v), __ATOMIC_SEQ_CST)
/**
 * Atomically stores the given value into the given variable, then returns the
 * variable's previous value.
 *
 * This macro is guaranteed to enforce sequentially consistent (CST) ordering.
 */
#define SPA_ATOMIC_XCHG(s,v)		__atomic_exchange_n(&(s), (v), __ATOMIC_SEQ_CST)

/**
 * Increments an atomic sequence number as part of a seqlock writer's sequence,
 * then returns the incremented number.
 *
 * This macro is guaranteed to enforce sequentially consistent (CST) ordering.
 *
 * See the explanation above for how to use it in a seqlock implementation.
 */
#define SPA_SEQ_WRITE(s)		SPA_ATOMIC_INC(s)
/**
 * Verifies the consistency of a write operation in seqlock by comparing sequence numbers.
 *
 * This is _not_ an atomic operation. Instead, it verifies the result of two
 * preceding \ref SPA_SEQ_WRITE operations.
 *
 * See the explanation above for how to use it in a seqlock implementation.
 *
 * NOTE: This is purely meant to be used for diagnostics, since seqlocks are not
 * usually meant for situations with multiple writers.
 */
#define SPA_SEQ_WRITE_SUCCESS(s1,s2)	((s1) + 1 == (s2) && ((s2) & 1) == 0)

/**
 * Returns an atomic sequence number as part of a seqlock reader's sequence.
 *
 * This macro is guaranteed to enforce sequentially consistent (CST) ordering.
 *
 * See the explanation above for how to use it in a seqlock implementation.
 */
#define SPA_SEQ_READ(s)			SPA_ATOMIC_LOAD(s)
/**
 * Verifies the consistency of a read operation in seqlock by comparing sequence numbers.
 *
 * This is _not_ an atomic operation. Instead, it verifies the result of two
 * preceding \ref SPA_SEQ_READ operations.
 *
 * See the explanation above for how to use it in a seqlock implementation.
 */
#define SPA_SEQ_READ_SUCCESS(s1,s2)	((s1) == (s2) && ((s2) & 1) == 0)

/**
 * @}
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_ATOMIC_H */
