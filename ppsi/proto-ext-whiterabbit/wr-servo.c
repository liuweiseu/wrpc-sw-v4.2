#include <ppsi/ppsi.h>
#include <ppsi/assert.h>
#include "wr-api.h"
#include <libwr/shmem.h>

#ifdef CONFIG_ARCH_WRS
#define ARCH_IS_WRS 1
#else
#define ARCH_IS_WRS 0
#endif

#ifdef CONFIG_WRPC_FAULTS
#define HAS_FAULTS 1
#else
#define HAS_FAULTS 0
#endif

struct pp_time faulty_stamps[6]; /* if unused, dropped at link time */

static void apply_faulty_stamp(struct wr_servo_state *s, int index)
{
	if (HAS_FAULTS) {
		assert(index >= 1 && index <= 6, "Wrong T index %i\n", index);
		pp_time_add(&s->t1 + index - 1, faulty_stamps + index - 1);
	}
}


#define WR_SERVO_OFFSET_STABILITY_THRESHOLD 60 /* psec */

#define FIX_ALPHA_FRACBITS 40

/* Define threshold values for SNMP */
#define SNMP_MAX_OFFSET_PS 500
#define SNMP_MAX_DELTA_RTT_PS 1000

static const char *servo_name[] = {
	[WR_UNINITIALIZED] = "Uninitialized",
	[WR_SYNC_NSEC] = "SYNC_NSEC",
	[WR_SYNC_TAI] = "SYNC_SEC",
	[WR_SYNC_PHASE] = "SYNC_PHASE",
	[WR_TRACK_PHASE] = "TRACK_PHASE",
	[WR_WAIT_OFFSET_STABLE] = "WAIT_OFFSET_STABLE",
};

/* Enable tracking by default. Disabling the tracking is used for demos. */
static int tracking_enabled = 1;
extern struct wrs_shm_head *ppsi_head;

void wr_servo_enable_tracking(int enable)
{
	tracking_enabled = enable;
}

/* my own timestamp arithmetic functions */

static void dump_timestamp(struct pp_instance *ppi, char *what,
			   struct pp_time ts)
{
	pp_diag(ppi, servo, 2, "%s = %ld:%09ld:%03ld\n", what, (long)ts.secs,
		(long)(ts.scaled_nsecs >> 16),
		/* unlikely what we had earlier, third field is not phase */
		((long)(ts.scaled_nsecs & 0xffff) * 1000 + 0x8000) >> 16);
}

static int64_t ts_to_picos(struct pp_time *ts)
{
	return ts->secs * PP_NSEC_PER_SEC
		+ ((ts->scaled_nsecs * 1000 + 0x8000) >> 16);
}

static void picos_to_ts(int64_t picos, struct pp_time *ts)
{
	uint64_t sec, nsec;
	int phase;
	int sign = (picos < 0 ? -1 : 1);

	picos *= sign;
	nsec = picos;
	phase = __div64_32(&nsec, 1000);
	sec = nsec;

	ts->scaled_nsecs = ((int64_t)__div64_32(&sec, PP_NSEC_PER_SEC)) << 16;
	ts->scaled_nsecs += (phase << 16) / 1000;
	ts->scaled_nsecs *= sign;
	ts->secs = sec * sign;
}

/* "Hardwarizes" the timestamp - e.g. makes the nanosecond field a multiple
 * of 8/16ns cycles and puts the extra nanoseconds in the picos result */
static void ts_hardwarize(struct pp_time *time, int clock_period_ps,
			  int32_t *ticks, int32_t *picos)
{
	int32_t s, ns, ps, clock_ns;

	/* clock_period_ps *must* be a multiple of 1000 -- assert()? */
	clock_ns = clock_period_ps / 1000;

	/*
	 * In pp_time, both sec/nsec are positive, or both negative.
	 * Only 0 secs can have positive or negative nsecs.
	 *
	 * Here we need a positive count for both tick and picos. Or not.
	 * The code here replicates what found in original WR code.
	 */
	s = time->secs; /* a difference: known to fit 32 bits (really?) */
	ps = time->scaled_nsecs & 0xffff; /* fractional nano */
	ps = (ps * 1000) >> 16; /* now picoseconds 0..999 -- positive*/
	ns = time->scaled_nsecs >> 16;
	if (ns > 0) {
		ps += (ns % clock_ns) * 1000;
		ns -= (ns % clock_ns);
	}
	if (ns < 0) {
		s--;
		ns += PP_NSEC_PER_SEC;
	}
	if (s == -1 && ns > 0) {
		s++;
		ns -= PP_NSEC_PER_SEC;
	}
	if (ns < 0 && s == 0 && ns >= -clock_ns) {
		/* originally, ns was a multiple of clock_ns, code differs */
		ps += ns * 1000;
		ns = 0;
	}
	*ticks = ns;
	*picos = ps;
}

/* end my own timestamp arithmetic functions */

static int got_sync = 0;

void wr_servo_reset(struct pp_instance *ppi)
{
	/* values from servo_state to be preserved */
	uint32_t n_err_state;
	uint32_t n_err_offset;
	uint32_t n_err_delta_rtt;

	struct wr_servo_state *s;

	s = &((struct wr_data *)ppi->ext_data)->servo_state;
	if (!s) {
		/* Don't clean servo state when is not available */
		return;
	}
	/* shmem lock */
	wrs_shm_write(ppsi_head, WRS_SHM_WRITE_BEGIN);
	ppi->flags = 0;

	/* preserve some values from servo_state */
	n_err_state = s->n_err_state;
	n_err_offset = s->n_err_offset;
	n_err_delta_rtt = s->n_err_delta_rtt;
	/* clear servo_state to display empty fields in wr_mon and SNMP */
	memset(s, 0, sizeof(struct wr_servo_state));
	/* restore values from servo_state */
	s->n_err_state = n_err_state;
	s->n_err_offset = n_err_offset;
	s->n_err_delta_rtt = n_err_delta_rtt;

	/* shmem unlock */
	wrs_shm_write(ppsi_head, WRS_SHM_WRITE_END);
}

static inline int32_t delta_to_ps(struct FixedDelta d)
{
	UInteger64 *sps = &d.scaledPicoseconds; /* ieee type :( */

	return (sps->lsb >> 16) | (sps->msb << 16);
}

int wr_servo_init(struct pp_instance *ppi)
{
	struct wr_dsport *wrp = WR_DSPOR(ppi);
	struct wr_servo_state *s =
			&((struct wr_data *)ppi->ext_data)->servo_state;
	/* shmem lock */
	wrs_shm_write(ppsi_head, WRS_SHM_WRITE_BEGIN);
	/* Determine the alpha coefficient */
	if (wrp->ops->read_calib_data(ppi, 0, 0,
		&s->fiber_fix_alpha, &s->clock_period_ps) != WR_HW_CALIB_OK)
		return -1;

	wrp->ops->enable_timing_output(ppi, 0);

	strncpy(s->if_name, ppi->cfg.iface_name, sizeof(s->if_name));
	/*
	 * Do not reset cur_setpoint, but trim it to be less than one tick.
	 * The softpll code uses the module anyways, but if we unplug-replug
	 * the fiber it will always increase, so don't scare the user
	 */
	if (s->cur_setpoint > s->clock_period_ps)
		s->cur_setpoint %= s->clock_period_ps;
	wrp->ops->adjust_phase(s->cur_setpoint);
	s->missed_iters = 0;
	s->state = WR_UNINITIALIZED;

	s->delta_tx_m = delta_to_ps(wrp->otherNodeDeltaTx);
	s->delta_rx_m = delta_to_ps(wrp->otherNodeDeltaRx);
	s->delta_tx_s = delta_to_ps(wrp->deltaTx);
	s->delta_rx_s = delta_to_ps(wrp->deltaRx);

	strcpy(s->servo_state_name, "Uninitialized");

	s->flags |= WR_FLAG_VALID;
	s->update_count = 0;
	ppi->t_ops->get(ppi, &s->update_time);
	s->tracking_enabled = tracking_enabled;

	got_sync = 0;

	/* shmem unlock */
	wrs_shm_write(ppsi_head, WRS_SHM_WRITE_END);
	return 0;
}

int wr_servo_got_sync(struct pp_instance *ppi, struct pp_time *t1,
		      struct pp_time *t2)
{
	struct wr_servo_state *s =
			&((struct wr_data *)ppi->ext_data)->servo_state;

	s->t1 = *t1; apply_faulty_stamp(s, 1);
	s->t2 = *t2; apply_faulty_stamp(s, 2);
	got_sync = 1;
	return 0;
}

int wr_servo_got_delay(struct pp_instance *ppi)
{
	struct wr_servo_state *s =
			&((struct wr_data *)ppi->ext_data)->servo_state;

	wrs_shm_write(ppsi_head, WRS_SHM_WRITE_BEGIN);

	s->t3 = ppi->t3; apply_faulty_stamp(s, 3);
	s->t4 = ppi->t4; apply_faulty_stamp(s, 4);

	if (CONFIG_HAS_P2P && ppi->mech == PP_P2P_MECH) {
		s->t5 = ppi->t5; apply_faulty_stamp(s, 5);
		s->t6 = ppi->t6; apply_faulty_stamp(s, 6);

		wr_p2p_delay(ppi, s);
	}

	wrs_shm_write(ppsi_head, WRS_SHM_WRITE_END);
	return 0;
}

int wr_p2p_delay(struct pp_instance *ppi, struct wr_servo_state *s)
{
	uint64_t big_delta_fix;
	static int errcount;

	if (is_incorrect(&s->t3) || is_incorrect(&s->t4)
	    || is_incorrect(&s->t5) || is_incorrect(&s->t6)) {
		errcount++;
		if (errcount > 5)	/* a 2-3 in a row are expected */
			pp_error("%s: TimestampsIncorrect: %d %d %d %d\n",
				 __func__, !is_incorrect(&s->t3),
				 !is_incorrect(&s->t4), !is_incorrect(&s->t5),
				 !is_incorrect(&s->t6));
		return 0;
	}
	errcount = 0;

	s->update_count++;
	ppi->t_ops->get(ppi, &s->update_time);

	{ /* avoid modifying stamps in place */
		struct pp_time mtime, stime;

		stime = s->t6; pp_time_sub(&stime, &s->t3);
		mtime = s->t5; pp_time_sub(&mtime, &s->t4);
		s->mu = stime; pp_time_sub(&s->mu, &mtime);
	}


	if (__PP_DIAG_ALLOW(ppi, pp_dt_servo, 1)) {
		dump_timestamp(ppi, "servo:t1", s->t1);
		dump_timestamp(ppi, "servo:t2", s->t2);
		dump_timestamp(ppi, "servo:t3", s->t3);
		dump_timestamp(ppi, "servo:t4", s->t4);
		dump_timestamp(ppi, "servo:t5", s->t5);
		dump_timestamp(ppi, "servo:t6", s->t6);
		dump_timestamp(ppi, "->mdelay", s->mu);
	}

	s->picos_mu = ts_to_picos(&s->mu);
	big_delta_fix = s->delta_tx_m + s->delta_tx_s
	    + s->delta_rx_m + s->delta_rx_s;

	s->delta_ms =
	    (((int64_t) (ts_to_picos(&s->mu) - big_delta_fix) *
	      (int64_t) s->fiber_fix_alpha) >> FIX_ALPHA_FRACBITS)
	    + ((ts_to_picos(&s->mu) - big_delta_fix) >> 1)
	    + s->delta_tx_m + s->delta_rx_s;

	return 1;
}

int wr_p2p_offset(struct pp_instance *ppi,
		  struct wr_servo_state *s, struct pp_time *ts_offset)
{
	static int errcount;
	struct pp_time time_ms;

	if (is_incorrect(&s->t1) || is_incorrect(&s->t2)) {
		errcount++;
		if (errcount > 5)	/* a 2-3 in a row are expected */
			pp_error("%s: TimestampsIncorrect: %d %d \n",
				 __func__, !is_incorrect(&s->t1),
				 !is_incorrect(&s->t2));
		return 0;
	}
	errcount = 0;
	got_sync = 0;

	s->update_count++;
	ppi->t_ops->get(ppi, &s->update_time);

	picos_to_ts(s->delta_ms, &time_ms);
	*ts_offset = s->t1;
	pp_time_sub(ts_offset, &s->t2);
	pp_time_add(ts_offset, &time_ms);

	/* is it possible to calculate it in client,
	 * but then t1 and t2 require shmem locks */
	s->offset = ts_to_picos(ts_offset);

	s->tracking_enabled =  tracking_enabled;

	return 1;
}

int wr_e2e_offset(struct pp_instance *ppi,
		  struct wr_servo_state *s, struct pp_time *ts_offset)
{
	struct wr_dsport *wrp = WR_DSPOR(ppi);
	uint64_t big_delta_fix;
	uint64_t delay_ms_fix;
	static int errcount;

	if (is_incorrect(&s->t1) || is_incorrect(&s->t2)
	    || is_incorrect(&s->t3) || is_incorrect(&s->t4)) {
		errcount++;
		if (errcount > 5) /* a 2-3 in a row are expected */
			pp_error("%s: TimestampsIncorrect: %d %d %d %d\n",
				 __func__, !is_incorrect(&s->t1),
				 !is_incorrect(&s->t2), !is_incorrect(&s->t3),
				 !is_incorrect(&s->t4));
		return 0;
	}

	if (wrp->ops->servo_hook) /* FIXME: check this, missing in p2p */
		wrp->ops->servo_hook(s, WR_SERVO_ENTER);

	errcount = 0;

	s->update_count++;
	ppi->t_ops->get(ppi, &s->update_time);

	got_sync = 0;

	{ /* avoid modifying stamps in place */
		struct pp_time mtime, stime;

		mtime = s->t4; pp_time_sub(&mtime, &s->t1);
		stime = s->t3; pp_time_sub(&stime, &s->t2);
		s->mu = mtime; pp_time_sub(&s->mu, &stime);
	}

	if (__PP_DIAG_ALLOW(ppi, pp_dt_servo, 1)) {
		dump_timestamp(ppi, "servo:t1", s->t1);
		dump_timestamp(ppi, "servo:t2", s->t2);
		dump_timestamp(ppi, "servo:t3", s->t3);
		dump_timestamp(ppi, "servo:t4", s->t4);
		dump_timestamp(ppi, "->mdelay", s->mu);
	}

	s->picos_mu = ts_to_picos(&s->mu);
	big_delta_fix =  s->delta_tx_m + s->delta_tx_s
		       + s->delta_rx_m + s->delta_rx_s;

	if (s->picos_mu < (int64_t)big_delta_fix) {
		/* avoid negatives in calculations */
		s->picos_mu = big_delta_fix;
	}

	delay_ms_fix = (((int64_t)(s->picos_mu - big_delta_fix)
			* (int64_t)s->fiber_fix_alpha) >> FIX_ALPHA_FRACBITS)
			+ ((s->picos_mu - big_delta_fix) >> 1)
			+ s->delta_tx_m + s->delta_rx_s;

	{ /* again, use temps to avoid modifying tx in place */
		struct pp_time tmp = s->t1, tmp2;

		pp_time_sub(&tmp, &s->t2);
		picos_to_ts(delay_ms_fix, &tmp2);
		pp_time_add(&tmp, &tmp2);

		*ts_offset = tmp;
	}

	/* is it possible to calculate it in client,
	 * but then t1 and t2 require shmem locks */
	s->offset = ts_to_picos(ts_offset);

	s->tracking_enabled =  tracking_enabled;

	s->delta_ms = delay_ms_fix;

	return 1;
}

int wr_servo_update(struct pp_instance *ppi)
{
	struct wr_dsport *wrp = WR_DSPOR(ppi);
	struct wr_servo_state *s =
	    &((struct wr_data *)ppi->ext_data)->servo_state;
	int remaining_offset;
	int64_t picos_mu_prev = 0;

	struct pp_time ts_offset;
	int32_t  ts_offset_ticks;
	int32_t  ts_offset_picos;
	int locking_poll_ret;

	if (!got_sync)
		return 0;

	/* shmem lock */
	wrs_shm_write(ppsi_head, WRS_SHM_WRITE_BEGIN);

	picos_mu_prev = s->picos_mu;
	if (CONFIG_HAS_P2P && ppi->mech == PP_P2P_MECH) {
		if (!wr_p2p_offset(ppi, s, &ts_offset))
			goto out;
	} else {
		if (!wr_e2e_offset(ppi, s, &ts_offset))
			goto out;
	}

	ts_hardwarize(&ts_offset, s->clock_period_ps,
		      &ts_offset_ticks, &ts_offset_picos);
	pp_diag(ppi, servo, 2, "offset_hw: %li.%09li (+%li)\n",
		(long)ts_offset.secs, (long)ts_offset_ticks,
		(long)ts_offset_picos);

	locking_poll_ret = wrp->ops->locking_poll(ppi, 0);
	if (locking_poll_ret != WR_SPLL_READY
	    && locking_poll_ret != WR_SPLL_CALIB_NOT_READY) {
		pp_diag(ppi, servo, 1, "PLL OutOfLock, should restart sync\n");
		wrp->ops->enable_timing_output(ppi, 0);
		/* TODO check
		 * DSPOR(ppi)->doRestart = TRUE; */
	}

	/* After each action on the hardware, we must verify if it is over. */
	if (!wrp->ops->adjust_in_progress()) {
		s->flags &= ~WR_FLAG_WAIT_HW;
	} else {
		pp_diag(ppi, servo, 1, "servo:busy\n");
		goto out;
	}

	/* So, we didn't return. Choose the right state */
	if (ts_offset.secs) /* so bad... */
		s->state = WR_SYNC_TAI;
	else if (ts_offset_ticks) /* not that bad */
		s->state = WR_SYNC_NSEC;
	else if (s->state == WR_UNINITIALIZED) {
		/* Likely a restart; already good, but force phase fsm */
		s->state = WR_SYNC_PHASE;
	}

	/* else, let the states below choose the sequence */

	pp_diag(ppi, servo, 1, "wr_servo state: %s%s\n",
		servo_name[s->state],
		s->flags & WR_FLAG_WAIT_HW ? " (wait for hw)" : "");

	/* update string state name */
	strcpy(s->servo_state_name, servo_name[s->state]);

	switch (s->state) {
	case WR_SYNC_TAI:
		wrp->ops->adjust_counters(ts_offset.secs, 0);
		s->flags |= WR_FLAG_WAIT_HW;
		/*
		 * If nsec wrong, code above forces SYNC_NSEC,
		 * Else, we must ensure we leave this status towards
		 * fine tuning
		 */
		s->state = WR_SYNC_PHASE;
		break;

	case WR_SYNC_NSEC:
		wrp->ops->adjust_counters(0, ts_offset_ticks);
		s->flags |= WR_FLAG_WAIT_HW;
		s->state = WR_SYNC_PHASE;
		break;

	case WR_SYNC_PHASE:
		pp_diag(ppi, servo, 2, "oldsetp %i, offset %i:%04i\n",
			s->cur_setpoint, ts_offset_ticks,
			ts_offset_picos);
		s->cur_setpoint += ts_offset_picos;
		wrp->ops->adjust_phase(s->cur_setpoint);

		s->flags |= WR_FLAG_WAIT_HW;
		s->state = WR_WAIT_OFFSET_STABLE;

		if (ARCH_IS_WRS) {
			/*
			 * Now, let's fix system time. We pass here
			 * once only, so that's the best place to do
			 * it. We can't use current WR time, as we
			 * still miss the method to get it (through IPC).
			 * So use T4, which is a good approximation.
			 */
			unix_time_ops.set(ppi, &ppi->t4);
			pp_diag(ppi, time, 1, "system time set to %li TAI\n",
				(long)ppi->t4.secs);
		}
		break;

	case WR_WAIT_OFFSET_STABLE:

		/* ts_to_picos() below returns phase alone */
		remaining_offset = abs(ts_offset_picos);
		if(remaining_offset < WR_SERVO_OFFSET_STABILITY_THRESHOLD) {
			wrp->ops->enable_timing_output(ppi, 1);
			s->delta_ms_prev = s->delta_ms;
			s->state = WR_TRACK_PHASE;
		} else {
			s->missed_iters++;
		}
		if (s->missed_iters >= 10) {
			s->missed_iters = 0;
			s->state = WR_SYNC_PHASE;
		}
		break;

	case WR_TRACK_PHASE:
		s->skew = s->delta_ms - s->delta_ms_prev;

		/* Can be disabled for manually tweaking and testing */
		if(tracking_enabled) {
			if (abs(ts_offset_picos) >
			    2 * WR_SERVO_OFFSET_STABILITY_THRESHOLD) {
				s->state = WR_SYNC_PHASE;
				break;
			}

			// adjust phase towards offset = 0 make ck0 0
			s->cur_setpoint += (ts_offset_picos / 4);

			wrp->ops->adjust_phase(s->cur_setpoint);
			pp_diag(ppi, time, 1, "adjust phase %i\n",
				s->cur_setpoint);

			s->delta_ms_prev = s->delta_ms;
		}
		break;

	}
	/* Increase number of servo updates with state different than
	 * WR_TRACK_PHASE. (Used by SNMP) */
	if (s->state != WR_TRACK_PHASE)
		s->n_err_state++;

	/* Increase number of servo updates with offset exceeded
	 * SNMP_MAX_OFFSET_PS (Used by SNMP) */
	if (abs(s->offset) > SNMP_MAX_OFFSET_PS)
		s->n_err_offset++;

	/* Increase number of servo updates with delta rtt exceeded
	 * SNMP_MAX_DELTA_RTT_PS (Used by SNMP) */
	if (abs(picos_mu_prev - s->picos_mu) > SNMP_MAX_DELTA_RTT_PS)
		s->n_err_delta_rtt++;

out:
	/* shmem unlock */
	wrs_shm_write(ppsi_head, WRS_SHM_WRITE_END);

	if (wrp->ops->servo_hook)
		wrp->ops->servo_hook(s, WR_SERVO_LEAVE);

	return 0;
}
