/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_UTILS_OVERFLOW_H
#define SPA_UTILS_OVERFLOW_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Check for addition overflow
 *
 * Computes \a a + \a b and stores the result in \a *res.
 * \return true if the addition overflowed, false otherwise
 */
#define spa_overflow_add(a, b, res)	__builtin_add_overflow(a, b, res)

/**
 * \brief Check for subtraction overflow
 *
 * Computes \a a - \a b and stores the result in \a *res.
 * \return true if the subtraction overflowed, false otherwise
 */
#define spa_overflow_sub(a, b, res)	__builtin_sub_overflow(a, b, res)

/**
 * \brief Check for multiplication overflow
 *
 * Computes \a a * \a b and stores the result in \a *res.
 * \return true if the multiplication overflowed, false otherwise
 */
#define spa_overflow_mul(a, b, res)	__builtin_mul_overflow(a, b, res)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_UTILS_OVERFLOW_H */
