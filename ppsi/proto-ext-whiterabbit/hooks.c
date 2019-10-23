#include <ppsi/ppsi.h>
#include "wr-api.h"
#include "../proto-standard/common-fun.h"

/* ext-whiterabbit must offer its own hooks */

static int wr_init(struct pp_instance *ppi, unsigned char *pkt, int plen)
{
	struct wr_dsport *wrp = WR_DSPOR(ppi);

	pp_diag(ppi, ext, 2, "hook: %s\n", __func__);
	wrp->wrStateTimeout = WR_DEFAULT_STATE_TIMEOUT_MS;
	wrp->calPeriod = WR_DEFAULT_CAL_PERIOD;
	wrp->wrModeOn = 0;
	wrp->parentWrConfig = NON_WR;
	wrp->parentWrModeOn = 0;
	wrp->calibrated = !WR_DEFAULT_PHY_CALIBRATION_REQUIRED;

	if ((wrp->wrConfig & WR_M_AND_S) == WR_M_ONLY)
		wrp->ops->enable_timing_output(ppi, 1);
	else
		wrp->ops->enable_timing_output(ppi, 0);
	return 0;
}

static int wr_open(struct pp_globals *ppg, struct pp_runtime_opts *rt_opts)
{
	int i;

	pp_diag(NULL, ext, 2, "hook: %s\n", __func__);
	/* If current arch (e.g. wrpc) is not using the 'pp_links style'
	 * configuration, just assume there is one ppi instance,
	 * already configured properly by the arch's main loop */
	if (ppg->nlinks == 0) {
		INST(ppg, 0)->ext_data = ppg->global_ext_data;
		return 0;
	}

	for (i = 0; i < ppg->nlinks; i++) {
		struct pp_instance *ppi = INST(ppg, i);

		/* FIXME check if correct: assign to each instance the same
		 * wr_data. May I move it to pp_globals? */
		INST(ppg, i)->ext_data = ppg->global_ext_data;

		if (ppi->cfg.ext == PPSI_EXT_WR) {
			switch (ppi->role) {
				case PPSI_ROLE_MASTER:
					WR_DSPOR(ppi)->wrConfig = WR_M_ONLY;
					break;
				case PPSI_ROLE_SLAVE:
					WR_DSPOR(ppi)->wrConfig = WR_S_ONLY;
					break;
				default:
					WR_DSPOR(ppi)->wrConfig = WR_M_AND_S;
			}
		}
		else
			WR_DSPOR(ppi)->wrConfig = NON_WR;
	}

	return 0;
}

static int wr_listening(struct pp_instance *ppi, unsigned char *pkt, int plen)
{
	struct wr_dsport *wrp = WR_DSPOR(ppi);

	pp_diag(ppi, ext, 2, "hook: %s\n", __func__);
	wrp->wrMode = NON_WR;
	return 0;
}

static int wr_master_msg(struct pp_instance *ppi, unsigned char *pkt, int plen,
			 int msgtype)
{
	MsgSignaling wrsig_msg;
	struct pp_time *time = &ppi->last_rcv_time;

	if (msgtype != PPM_NO_MESSAGE)
		pp_diag(ppi, ext, 2, "hook: %s\n", __func__);

	switch (msgtype) {

	/* This case is modified from the default one */
	case PPM_DELAY_REQ:
		msg_issue_delay_resp(ppi, time); /* no error check */
		msgtype = PPM_NO_MESSAGE;
		break;

	case PPM_PDELAY_REQ:
		/* nothing to do */
		break;

	/* This is missing in the standard protocol */
	case PPM_SIGNALING:
		msg_unpack_wrsig(ppi, pkt, &wrsig_msg,
				 &(WR_DSPOR(ppi)->msgTmpWrMessageID));
		if ((WR_DSPOR(ppi)->msgTmpWrMessageID == SLAVE_PRESENT) &&
		    (WR_DSPOR(ppi)->wrConfig & WR_M_ONLY)) {
			/* We must start the handshake as a WR master */
			wr_handshake_init(ppi, PPS_MASTER);
		}
		msgtype = PPM_NO_MESSAGE;
		break;
	}

	return msgtype;
}

static int wr_new_slave(struct pp_instance *ppi, unsigned char *pkt, int plen)
{
	pp_diag(ppi, ext, 2, "hook: %s\n", __func__);
	wr_servo_init(ppi);
	return 0;
}

static int wr_handle_resp(struct pp_instance *ppi)
{
	struct pp_time *ofm = &DSCUR(ppi)->offsetFromMaster;
	struct wr_dsport *wrp = WR_DSPOR(ppi);

	pp_diag(ppi, ext, 2, "hook: %s\n", __func__);

	/* This correction_field we received is already part of t4 */

	/*
	 * If no WR mode is on, run normal code, if T2/T3 are valid.
	 * After we adjusted the pps counter, stamps are invalid, so
	 * we'll have the Unix time instead, marked by "correct"
	 */
	if (!wrp->wrModeOn) {
		if (is_incorrect(&ppi->t2) || is_incorrect(&ppi->t3)) {
			pp_diag(ppi, servo, 1,
				"T2 or T3 incorrect, discarding tuple\n");
			return 0;
		}
		pp_servo_got_resp(ppi);
		/*
		 * pps always on if offset less than 1 second,
		 * until ve have a configurable threshold */
		if (ofm->secs)
			wrp->ops->enable_timing_output(ppi, 0);
		else
			wrp->ops->enable_timing_output(ppi, 1);

	}
	wr_servo_got_delay(ppi);
	wr_servo_update(ppi);
	return 0;
}

static void wr_s1(struct pp_instance *ppi, MsgHeader *hdr, MsgAnnounce *ann)
{
	pp_diag(ppi, ext, 2, "hook: %s\n", __func__);
	WR_DSPOR(ppi)->parentIsWRnode =
		((ann->ext_specific & WR_NODE_MODE) != NON_WR);
	WR_DSPOR(ppi)->parentWrModeOn =
		(ann->ext_specific & WR_IS_WR_MODE) ? TRUE : FALSE;
	WR_DSPOR(ppi)->parentCalibrated =
			((ann->ext_specific & WR_IS_CALIBRATED) ? 1 : 0);
	WR_DSPOR(ppi)->parentWrConfig = ann->ext_specific & WR_NODE_MODE;
	DSCUR(ppi)->primarySlavePortNumber =
		DSPOR(ppi)->portIdentity.portNumber;
}

static int wr_execute_slave(struct pp_instance *ppi)
{
	pp_diag(ppi, ext, 2, "hook: %s\n", __func__);

	if (pp_timeout(ppi, PP_TO_FAULT))
		wr_servo_reset(ppi); /* the caller handles ptp state machine */

	/* The doRestart thing is not  used, it seems */
	if (!WR_DSPOR(ppi)->doRestart)
		return 0;
	ppi->next_state = PPS_INITIALIZING;
	WR_DSPOR(ppi)->doRestart = FALSE;
	return 1; /* the caller returns too */
}

static int wr_handle_announce(struct pp_instance *ppi)
{
	pp_diag(ppi, ext, 2, "hook: %s\n", __func__);
	if ((WR_DSPOR(ppi)->wrConfig & WR_S_ONLY) &&
	    (1 /* FIXME: Recommended State, see page 33*/) &&
	    (WR_DSPOR(ppi)->parentWrConfig & WR_M_ONLY) &&
	    (!WR_DSPOR(ppi)->wrModeOn || !WR_DSPOR(ppi)->parentWrModeOn)) {
		/* We must start the handshake as a WR slave */
		wr_handshake_init(ppi, PPS_SLAVE);
	}
	return 0;
}

static int wr_handle_followup(struct pp_instance *ppi,
			      struct pp_time *t1) /* t1 == &ppi->t1 */
{
	pp_diag(ppi, ext, 2, "hook: %s\n", __func__);
	if (!WR_DSPOR(ppi)->wrModeOn)
		return 0;

	wr_servo_got_sync(ppi, t1, &ppi->t2);

	if (CONFIG_HAS_P2P && ppi->mech == PP_P2P_MECH)
		wr_servo_update(ppi);

	return 1; /* the caller returns too */
}

static __attribute__((used)) int wr_handle_presp(struct pp_instance *ppi)
{
	struct wr_dsport *wrp = WR_DSPOR(ppi);
	struct pp_time *ofm = &DSCUR(ppi)->offsetFromMaster;

	/*
	 * If no WR mode is on, run normal code, if T2/T3 are valid.
	 * After we adjusted the pps counter, stamps are invalid, so
	 * we'll have the Unix time instead, marked by "correct"
	 */

	if (!wrp->wrModeOn) {
		if (is_incorrect(&ppi->t3) || is_incorrect(&ppi->t6)) {
			pp_diag(ppi, servo, 1,
				"T3 or T6 incorrect, discarding tuple\n");
			return 0;
		}
		pp_servo_got_presp(ppi);
		/*
		 * pps always on if offset less than 1 second,
		 * until ve have a configurable threshold */
		if (ofm->secs)
			wrp->ops->enable_timing_output(ppi, 0);
		else
			wrp->ops->enable_timing_output(ppi, 1);

		return 0;
	}

	/* FIXME: verify that last-received cField is already accounted for */
	wr_servo_got_delay(ppi);
	return 0;
}

static int wr_pack_announce(struct pp_instance *ppi)
{
	pp_diag(ppi, ext, 2, "hook: %s\n", __func__);
	if (WR_DSPOR(ppi)->wrConfig != NON_WR &&
		WR_DSPOR(ppi)->wrConfig != WR_S_ONLY) {
		msg_pack_announce_wr_tlv(ppi);
		return WR_ANNOUNCE_LENGTH;
	}
	return PP_ANNOUNCE_LENGTH;
}

static void wr_unpack_announce(void *buf, MsgAnnounce *ann)
{
	int msg_len = htons(*(UInteger16 *) (buf + 2));

	pp_diag(NULL, ext, 2, "hook: %s\n", __func__);
	if (msg_len > PP_ANNOUNCE_LENGTH)
		msg_unpack_announce_wr_tlv(buf, ann);
}


struct pp_ext_hooks pp_hooks = {
	.init = wr_init,
	.open = wr_open,
	.listening = wr_listening,
	.master_msg = wr_master_msg,
	.new_slave = wr_new_slave,
	.handle_resp = wr_handle_resp,
	.s1 = wr_s1,
	.execute_slave = wr_execute_slave,
	.handle_announce = wr_handle_announce,
	.handle_followup = wr_handle_followup,
#if CONFIG_HAS_P2P
	.handle_presp = wr_handle_presp,
#endif
	.pack_announce = wr_pack_announce,
	.unpack_announce = wr_unpack_announce,
};
