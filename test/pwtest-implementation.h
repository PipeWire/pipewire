/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Red Hat, Inc. */
/* SPDX-License-Identifier: MIT */

#ifndef PWTEST_IMPLEMENTATION_H
#define PWTEST_IMPLEMENTATION_H

#include <math.h>

/* This header should never be included on its own, it merely exists to make
 * the user-visible pwtest.h header more readable */

void
_pwtest_fail_condition(int exitstatus, const char *file, int line, const char *func,
		      const char *condition, const char *message, ...);
void
_pwtest_fail_comparison_int(const char *file, int line, const char *func,
			   const char *operator, int a, int b,
			   const char *astr, const char *bstr);
void
_pwtest_fail_comparison_double(const char *file, int line, const char *func,
			      const char *operator, double a, double b,
			      const char *astr, const char *bstr);
void
_pwtest_fail_comparison_ptr(const char *file, int line, const char *func,
			   const char *comparison);

void
_pwtest_fail_comparison_str(const char *file, int line, const char *func,
			   const char *comparison, const char *a, const char *b);

void
_pwtest_fail_comparison_bool(const char *file, int line, const char *func,
			     const char *operator, bool a, bool b,
			     const char *astr, const char *bstr);

void
_pwtest_fail_errno(const char *file, int line, const char *func,
		   int expected, int err_no);

#define pwtest_errno_check(r_, errno_) \
	do { \
		int _r = r_; \
		int _e = errno_; \
		if (_e == 0) { \
			if (_r == -1) \
				_pwtest_fail_errno(__FILE__, __LINE__, __func__, _e, errno); \
		} else { \
			if (_r != -1 || errno != _e) \
				_pwtest_fail_errno(__FILE__, __LINE__, __func__, _e, errno); \
		} \
	} while(0)

#define pwtest_neg_errno_check(r_, errno_) \
	do { \
		int _r = r_; \
		int _e = errno_; \
		if (_e == 0) { \
			if (_r < 0) \
				_pwtest_fail_errno(__FILE__, __LINE__, __func__, _e, -_r); \
		} else { \
			if (_r >= 0 || _r != _e) \
				_pwtest_fail_errno(__FILE__, __LINE__, __func__, -_e, _r >= 0 ? 0 : -_r); \
		} \
	} while(0)

#define pwtest_comparison_bool_(a_, op_, b_) \
	do { \
		bool _a = !!(a_); \
		bool _b = !!(b_); \
		if (!(_a op_ _b)) \
			_pwtest_fail_comparison_bool(__FILE__, __LINE__, __func__,\
						#op_, _a, _b, #a_, #b_); \
	} while(0)

#define pwtest_comparison_int_(a_, op_, b_) \
	do { \
		__typeof__(a_) _a = a_; \
		__typeof__(b_) _b = b_; \
		if (trunc(_a) != _a || trunc(_b) != _b) \
			pwtest_error_with_msg("pwtest_int_* used for non-integer value\n"); \
		if (!((_a) op_ (_b))) \
			_pwtest_fail_comparison_int(__FILE__, __LINE__, __func__,\
						#op_, _a, _b, #a_, #b_); \
	} while(0)

#define pwtest_comparison_ptr_(a_, op_, b_) \
	do { \
		__typeof__(a_) _a = a_; \
		__typeof__(b_) _b = b_; \
		if (!((_a) op_ (_b))) \
			_pwtest_fail_comparison_ptr(__FILE__, __LINE__, __func__,\
						#a_ " " #op_ " " #b_); \
	} while(0)

#define pwtest_comparison_double_(a_, op_, b_) \
	do { \
		const double EPSILON = 1.0/256; \
		__typeof__(a_) _a = a_; \
		__typeof__(b_) _b = b_; \
		if (!((_a) op_ (_b)) && fabs((_a) - (_b)) > EPSILON)  \
			_pwtest_fail_comparison_double(__FILE__, __LINE__, __func__,\
						#op_, _a, _b, #a_, #b_); \
	} while(0)

void _pwtest_add(struct pwtest_context *ctx,
		 struct pwtest_suite *suite,
		 const char *funcname, const void *func,
		 ...) SPA_SENTINEL;

struct pwtest_suite_decl {
	const char *name;
	enum pwtest_result (*setup)(struct pwtest_context *, struct pwtest_suite *);
};


#endif /* PWTEST_IMPLEMENTATION_H */
