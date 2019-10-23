/*
 * Copyright (C) 2011 CERN (www.cern.ch)
 * Author: Aurelio Colosimo
 * Based on PTPd project v. 2.1.0 (see AUTHORS for details)
 *
 * Released according to the GNU LGPL, version 2.1 or any later version.
 */

#include <ppsi/ppsi.h>

static void pp_servo_mpd_fltr(struct pp_instance *, struct pp_avg_fltr *,
			      struct pp_time *);
static int pp_servo_offset_master(struct pp_instance *, struct pp_time *,
				   struct pp_time *, struct pp_time *);
static int64_t pp_servo_pi_controller(struct pp_instance *, struct pp_time *);


void pp_servo_init(struct pp_instance *ppi)
{
	int d;

	SRV(ppi)->mpd_fltr.s_exp = 0;	/* clears meanPathDelay filter */
	ppi->frgn_rec_num = 0;		/* no known master */
	DSPAR(ppi)->parentPortIdentity.portNumber = 0; /* invalid */

	if (ppi->t_ops->init_servo) {
		/* The system may pre-set us to keep current frequency */
		d = ppi->t_ops->init_servo(ppi);
		if (d == -1) {
			pp_diag(ppi, servo, 1, "error in t_ops->servo_init");
			d = 0;
		}
		SRV(ppi)->obs_drift = -d << 10; /* note "-" */
	} else {
		/* level clock */
		if (pp_can_adjust(ppi))
			ppi->t_ops->adjust(ppi, 0, 0);
		SRV(ppi)->obs_drift = 0;
	}

	pp_timeout_set(ppi, PP_TO_FAULT);
	pp_diag(ppi, servo, 1, "Initialized: obs_drift %lli\n",
		SRV(ppi)->obs_drift);
}

/* internal helper, returning static storage to be used immediately */
static char *fmt_ppt(struct pp_time *t)
{
	static char s[24];

	pp_sprintf(s, "%s%d.%09d",
		   (t->secs < 0 || (t->secs == 0 && t->scaled_nsecs < 0))
		   ? "-" : " ",
		   /* FIXME: this is wrong for some of the negatives */
		   (int)abs(t->secs), (int)abs(t->scaled_nsecs >> 16));
	return s;
}

/* Called by slave and uncalib when we have t1 and t2 */
void pp_servo_got_sync(struct pp_instance *ppi)
{
	struct pp_time *m_to_s_dly = &SRV(ppi)->m_to_s_dly;

	/*
	 * calc 'master_to_slave_delay'; no correction field
	 * appears in the formulas because it's already merged with t1
	 */
	*m_to_s_dly = ppi->t2;
	pp_time_sub(m_to_s_dly, &ppi->t1);
}

/* Called by slave and uncalib when we have t1 and t2 */
void pp_servo_got_psync(struct pp_instance *ppi)
{
	struct pp_time *m_to_s_dly = &SRV(ppi)->m_to_s_dly;
	struct pp_time *mpd = &DSCUR(ppi)->meanPathDelay;
	struct pp_time *ofm = &DSCUR(ppi)->offsetFromMaster;
	int adj32;

	pp_diag(ppi, servo, 2, "T1: %s\n", fmt_ppt(&ppi->t1));
	pp_diag(ppi, servo, 2, "T2: %s\n", fmt_ppt(&ppi->t2));

	/*
	 * calc 'master_to_slave_delay'; no correction field
	 * appears in the formulas because it's already merged with t1
	 */
	*m_to_s_dly = ppi->t2;
	pp_time_sub(m_to_s_dly, &ppi->t1);

	/* update 'offsetFromMaster' and possibly jump in time */
	if (pp_servo_offset_master(ppi, mpd, ofm, m_to_s_dly))
		return;

	/* PI controller returns a scaled_nsecs adjustment, so shift back */
	adj32 = (int)(pp_servo_pi_controller(ppi, ofm) >> 16);

	/* apply controller output as a clock tick rate adjustment, if
	 * provided by arch, or as a raw offset otherwise */
	if (pp_can_adjust(ppi)) {
		if (ppi->t_ops->adjust_freq)
			ppi->t_ops->adjust_freq(ppi, -adj32);
		else
			ppi->t_ops->adjust_offset(ppi, -adj32);
	}

	pp_diag(ppi, servo, 2, "Observed drift: %9i\n",
		(int)SRV(ppi)->obs_drift >> 10);
}

/* called by slave states when delay_resp is received (all t1..t4 are valid) */
void pp_servo_got_resp(struct pp_instance *ppi)
{
	struct pp_time *m_to_s_dly = &SRV(ppi)->m_to_s_dly;
	struct pp_time *s_to_m_dly = &SRV(ppi)->s_to_m_dly;
	struct pp_time *mpd = &DSCUR(ppi)->meanPathDelay;
	struct pp_time *ofm = &DSCUR(ppi)->offsetFromMaster;
	struct pp_avg_fltr *mpd_fltr = &SRV(ppi)->mpd_fltr;
	int adj32;


	/* We sometimes enter here before we got sync/f-up */
	if (ppi->t1.secs == 0 && ppi->t1.scaled_nsecs == 0) {
		pp_diag(ppi, servo, 2, "discard T3/T4: we miss T1/T2\n");
		return;
	}
	/*
	 * calc 'slave_to_master_delay', removing delay_resp correction field
	 * added by transparent clocks in the path.
	 */
	*s_to_m_dly = ppi->t4;
	pp_time_sub(s_to_m_dly, &ppi->t3);

	pp_diag(ppi, servo, 2, "T1: %s\n", fmt_ppt(&ppi->t1));
	pp_diag(ppi, servo, 2, "T2: %s\n", fmt_ppt(&ppi->t2));
	pp_diag(ppi, servo, 2, "T3: %s\n", fmt_ppt(&ppi->t3));
	pp_diag(ppi, servo, 2, "T4: %s\n", fmt_ppt(&ppi->t4));
	pp_diag(ppi, servo, 1, "Master to slave: %s\n", fmt_ppt(m_to_s_dly));
	pp_diag(ppi, servo, 1, "Slave to master: %s\n", fmt_ppt(s_to_m_dly));

	/* Calc mean path delay, used later to calc "offset from master" */
	*mpd = SRV(ppi)->m_to_s_dly;
	pp_time_add(mpd, &SRV(ppi)->s_to_m_dly);
	pp_time_div2(mpd);
	pp_diag(ppi, servo, 1, "meanPathDelay: %s\n", fmt_ppt(mpd));

	if (mpd->secs) /* Hmm.... we called this "bad event" */
		return;

	/* mean path delay filtering */
	pp_servo_mpd_fltr(ppi, mpd_fltr, mpd);

	/* update 'offsetFromMaster' and possibly jump in time */
	if (pp_servo_offset_master(ppi, mpd, ofm, m_to_s_dly))
		return;

	/* PI controller */
	adj32 = (int)(pp_servo_pi_controller(ppi, ofm) >> 16);

	/* apply controller output as a clock tick rate adjustment, if
	 * provided by arch, or as a raw offset otherwise */
	if (pp_can_adjust(ppi)) {
		if (ppi->t_ops->adjust_freq)
			ppi->t_ops->adjust_freq(ppi, -adj32);
		else
			ppi->t_ops->adjust_offset(ppi, -adj32);
	}

	pp_diag(ppi, servo, 2, "Observed drift: %9i\n",
		(int)SRV(ppi)->obs_drift >> 10);
}

/* called by slave states when delay_resp is received (all t1..t4 are valid) */
void pp_servo_got_presp(struct pp_instance *ppi)
{
	struct pp_time *m_to_s_dly = &SRV(ppi)->m_to_s_dly;
	struct pp_time *s_to_m_dly = &SRV(ppi)->s_to_m_dly;
	struct pp_time *mpd = &DSCUR(ppi)->meanPathDelay;
	struct pp_avg_fltr *mpd_fltr = &SRV(ppi)->mpd_fltr;

	/*
	 * calc 'slave_to_master_delay', removing the correction field
	 * added by transparent clocks in the path.
	 */
	*s_to_m_dly = ppi->t6;
	pp_time_sub(s_to_m_dly, &ppi->t5);

	*m_to_s_dly = ppi->t4;
	pp_time_sub(m_to_s_dly, &ppi->t3);

	pp_diag(ppi, servo, 2, "T3: %s\n", fmt_ppt(&ppi->t3));
	pp_diag(ppi, servo, 2, "T4: %s\n", fmt_ppt(&ppi->t4));
	pp_diag(ppi, servo, 2, "T5: %s\n", fmt_ppt(&ppi->t5));
	pp_diag(ppi, servo, 2, "T6: %s\n", fmt_ppt(&ppi->t6));
	pp_diag(ppi, servo, 1, "Master to slave: %s\n", fmt_ppt(m_to_s_dly));
	pp_diag(ppi, servo, 1, "Slave to master: %s\n", fmt_ppt(s_to_m_dly));

	/* Calc mean path delay, used later to calc "offset from master" */
	*mpd = SRV(ppi)->m_to_s_dly;
	pp_time_add(mpd, &SRV(ppi)->s_to_m_dly);
	pp_time_div2(mpd);
	pp_diag(ppi, servo, 1, "meanPathDelay: %s\n", fmt_ppt(mpd));

	if (mpd->secs) /* Hmm.... we called this "bad event" */
		return;

	pp_servo_mpd_fltr(ppi, mpd_fltr, mpd);
}

static
void pp_servo_mpd_fltr(struct pp_instance *ppi, struct pp_avg_fltr *mpd_fltr,
		       struct pp_time *mpd)
{
	int s;
	uint64_t y;

	if (mpd_fltr->s_exp < 1) {
		/* First time, keep what we have */
		mpd_fltr->y = mpd->scaled_nsecs;
		if (mpd->scaled_nsecs < 0)
			mpd_fltr->y = 0;
	}
	/* avoid overflowing filter: calculate number of bits */
	s = OPTS(ppi)->s;
	while (mpd_fltr->y >> (63 - s))
		--s;
	if (mpd_fltr->s_exp > 1LL << s)
		mpd_fltr->s_exp = 1LL << s;
	/* crank down filter cutoff by increasing 's_exp' */
	if (mpd_fltr->s_exp < 1LL << s)
		++mpd_fltr->s_exp;

	/*
	 * It may happen that mpd appears as negative. This happens when
	 * the slave clock is running fast to recover a late time: the
	 * (t3 - t2) measured in the slave appears longer than the (t4 - t1)
	 * measured in the master.  Ignore such values, by keeping the
	 * current average instead.
	 */
	if (mpd->scaled_nsecs < 0)
		mpd->scaled_nsecs = mpd_fltr->y;
	if (mpd->scaled_nsecs < 0)
		mpd->scaled_nsecs = 0;

	/*
	 * It may happen that mpd appears to be very big. This happens
	 * when we have software timestamps and there is overhead
	 * involved -- or when the slave clock is running slow.  In
	 * this case use a value just slightly bigger than the current
	 * average (so if it really got longer, we will adapt).  This
	 * kills most outliers on loaded networks.
	 * The constant multipliers have been chosed arbitrarily, but
	 * they work well in testing environment.
	 */
	if (mpd->scaled_nsecs > 3 * mpd_fltr->y) {
		pp_diag(ppi, servo, 1, "Trim too-long mpd: %i\n",
			(int)(mpd->scaled_nsecs >> 16));
		/* add fltr->s_exp to ensure we are not trapped into 0 */
		mpd->scaled_nsecs = mpd_fltr->y * 2 + mpd_fltr->s_exp + 1;
	}
	/* filter 'meanPathDelay' (running average) -- use an unsigned "y" */
	y = (mpd_fltr->y * (mpd_fltr->s_exp - 1) + mpd->scaled_nsecs);
	__div64_32(&y, mpd_fltr->s_exp);
	mpd->scaled_nsecs = mpd_fltr->y = y;

	pp_diag(ppi, servo, 1, "After avg(%i), meanPathDelay: %i\n",
		(int)mpd_fltr->s_exp, (int)(mpd->scaled_nsecs >> 16));
}

static
int pp_servo_offset_master(struct pp_instance *ppi, struct pp_time *mpd,
			    struct pp_time *ofm, struct pp_time *m_to_s_dly)
{
	struct pp_time time_tmp;
	*ofm = *m_to_s_dly;
	pp_time_sub(ofm, mpd);
	pp_diag(ppi, servo, 1, "Offset from master:     %s\n", fmt_ppt(ofm));

	if (!ofm->secs)
		return 0; /* proceeed with adjust */

	if (!pp_can_adjust(ppi))
		return 0; /* e.g., a loopback test run... "-t" on cmdline */

	ppi->t_ops->get(ppi, &time_tmp);
	pp_time_sub(&time_tmp, ofm);
	ppi->t_ops->set(ppi, &time_tmp);
	pp_servo_init(ppi);
	return 1; /* done */
}

static
int64_t pp_servo_pi_controller(struct pp_instance * ppi, struct pp_time *ofm)
{
	long long I_term;
	long long P_term;
	long long tmp;
	int I_sign;
	int P_sign;
	int64_t adj;

	/* the accumulator for the I component */
	SRV(ppi)->obs_drift += ofm->scaled_nsecs;

	/* Anti-windup. The PP_ADJ_FREQ_MAX value is multiplied by OPTS(ppi)->ai
	 * (which is the reciprocal of the integral gain of the controller).
	 * Then it's scaled by 16 bits to match our granularity and
	 * avoid bit losses */
	tmp = (((long long)PP_ADJ_FREQ_MAX) * OPTS(ppi)->ai) << 16;
	if (SRV(ppi)->obs_drift > tmp)
		SRV(ppi)->obs_drift = tmp;
	else if (SRV(ppi)->obs_drift < -tmp)
		SRV(ppi)->obs_drift = -tmp;

	/* calculation of the I component, based on obs_drift */
	I_sign = (SRV(ppi)->obs_drift > 0) ? 0 : -1;
	I_term = SRV(ppi)->obs_drift;
	if (I_sign)
		I_term = -I_term;
	__div64_32((uint64_t *)&I_term, OPTS(ppi)->ai);
	if (I_sign)
		I_term = -I_term;

	/* calculation of the P component */
	P_sign = (ofm->scaled_nsecs > 0) ? 0 : -1;
	/* alrady shifted 16 bits, so we avoid losses */
	P_term = ofm->scaled_nsecs;
	if (P_sign)
		P_term = -P_term;
	__div64_32((uint64_t *)&P_term, OPTS(ppi)->ap);
	if (P_sign)
		P_term = -P_term;

	/* calculate the correction of applied by the controller */
	adj = P_term + I_term;
	/* Return the scaled-nanos values; the caller is scaling back */

	return adj;
}
