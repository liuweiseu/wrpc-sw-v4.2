/*
 * Copyright (C) 2011 CERN (www.cern.ch)
 * Author: Aurelio Colosimo
 * Based on PTPd project v. 2.1.0 (see AUTHORS for details)
 *
 * Released according to the GNU LGPL, version 2.1 or any later version.
 */

#include <limits.h>
#include <ppsi/ppsi.h>

void normalize_pp_time(struct pp_time *t)
{
	/* no 64b division please, we'll rather loop a few times */
	#define SNS_PER_S ((1000LL * 1000 * 1000) << 16)

	/* For whatever reason we perform a normalization on an incorrect
	 * timestamp, don't treat is as an negative value. */
	int64_t sign = ((t->secs < 0 && !is_incorrect(t))
			|| (t->secs == 0 && t->scaled_nsecs < 0))
		? -1 : 1;

	/* turn into positive, to make code shorter (don't replicate loops) */
	t->secs *= sign;
	t->scaled_nsecs *= sign;

	while (t->scaled_nsecs < 0) {
		t->secs--;
		t->scaled_nsecs += SNS_PER_S;
	}
	while (t->scaled_nsecs > SNS_PER_S) {
		t->secs++;
		t->scaled_nsecs -= SNS_PER_S;
	}
	t->secs *= sign;
	t->scaled_nsecs *= sign;
}

void pp_time_add(struct pp_time *t1, struct pp_time *t2)
{
	t1->secs += t2->secs;
	t1->scaled_nsecs += t2->scaled_nsecs;
	normalize_pp_time(t1);
}

void pp_time_sub(struct pp_time *t1, struct pp_time *t2)
{
	t1->secs -= t2->secs;
	t1->scaled_nsecs -= t2->scaled_nsecs;
	normalize_pp_time(t1);
}

void pp_time_div2(struct pp_time *t)
{
	int sign = (t->secs < 0) ? -1 : 1;

	t->scaled_nsecs >>= 1;
	if (t->secs &1)
		t->scaled_nsecs += sign * SNS_PER_S / 2;
	t->secs >>= 1;
}
