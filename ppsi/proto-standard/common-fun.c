/*
 * Copyright (C) 2011 CERN (www.cern.ch)
 * Author: Aurelio Colosimo
 * Based on PTPd project v. 2.1.0 (see AUTHORS for details)
 *
 * Released according to the GNU LGPL, version 2.1 or any later version.
 */
#include <ppsi/ppsi.h>
#include "common-fun.h"
#include "../lib/network_types.h"
#include "../proto-ext-whiterabbit/wr-api.h" /* FIXME: phase_to_cf_units */

#ifdef CONFIG_ARCH_WRS
#define ARCH_IS_WRS 1
#else
#define ARCH_IS_WRS 0
#endif

void *msg_copy_header(MsgHeader *dest, MsgHeader *src)
{
	return memcpy(dest, src, sizeof(MsgHeader));
}

static void *__align_pointer(void *p)
{
	unsigned long ip, align = 0;

	ip = (unsigned long)p;
	if (ip & 3)
		align = 4 - (ip & 3);
	return p + align;
}

void pp_prepare_pointers(struct pp_instance *ppi)
{
	/*
	 * Horrible thing: when we receive vlan, we get standard eth header,
	 * but when we send we must fill the complete vlan header.
	 * So we reserve a different number of bytes.
	 */
	switch(ppi->proto) {
	case PPSI_PROTO_RAW:
		ppi->tx_offset = ETH_HLEN; /* 14, I know! */
		ppi->rx_offset = ETH_HLEN;
	#ifdef CONFIG_ARCH_WRPC
		ppi->tx_offset = 0; /* Currently, wrpc has a separate header */
		ppi->rx_offset = 0;
	#endif
		break;
	case PPSI_PROTO_VLAN:
		ppi->tx_offset = sizeof(struct pp_vlanhdr);
		ppi->rx_offset = ETH_HLEN;
		break;
	case PPSI_PROTO_UDP:
		ppi->tx_offset = 0;
		ppi->rx_offset = 0;
		break;
	}
	ppi->tx_ptp = __align_pointer(ppi->__tx_buffer + ppi->tx_offset);
	ppi->rx_ptp = __align_pointer(ppi->__rx_buffer + ppi->rx_offset);

	/* Now that ptp payload is aligned, get back the header */
	ppi->tx_frame = ppi->tx_ptp - ppi->tx_offset;
	ppi->rx_frame = ppi->rx_ptp - ppi->rx_offset;

	if (0) { /* enable to verify... it works for me though */
		pp_printf("%p -> %p %p\n",
			  ppi->__tx_buffer, ppi->tx_frame, ppi->tx_ptp);
		pp_printf("%p -> %p %p\n",
			  ppi->__rx_buffer, ppi->rx_frame, ppi->rx_ptp);
	}
}

/* Called by listening, passive, slave, uncalibrated */
int st_com_execute_slave(struct pp_instance *ppi)
{
	int ret = 0;

	if (pp_hooks.execute_slave)
		ret = pp_hooks.execute_slave(ppi);
	if (ret == 1) /* done: just return */
		return 0;
	if (ret < 0)
		return ret;

	if (pp_timeout(ppi, PP_TO_ANN_RECEIPT)
	    || pp_timeout(ppi, PP_TO_FAULT)) {
		/* 
		 * Note: TO_FAULTY == SYNCHRONIZATION_FAULT
		 * should move us to UNCALIBRATED (not implemented)
		 */
		ppi->frgn_rec_num = 0;
		if (DSDEF(ppi)->clockQuality.clockClass != PP_CLASS_SLAVE_ONLY
		    && (ppi->role != PPSI_ROLE_SLAVE)) {
			ppi->next_state = PPS_MASTER;
		} else {
			ppi->next_state = PPS_LISTENING;
			pp_timeout_set(ppi, PP_TO_ANN_RECEIPT);
		}
	}
	return 0;
}


/* Called by slave and uncalibrated */
int st_com_slave_handle_sync(struct pp_instance *ppi, unsigned char *buf,
			     int len)
{
	MsgHeader *hdr = &ppi->received_ptp_header;
	MsgSync sync;

	if (!(ppi->flags & PPI_FLAG_FROM_CURRENT_PARENT))
		return 0;

	/* t2 may be overriden by follow-up, save it immediately */
	ppi->t2 = ppi->last_rcv_time;
	msg_unpack_sync(buf, &sync);

	if ((hdr->flagField[0] & PP_TWO_STEP_FLAG) != 0) {
		ppi->flags |= PPI_FLAG_WAITING_FOR_F_UP;
		ppi->recv_sync_sequence_id = hdr->sequenceId;
		/* for two-step, the stamp comes later */
		ppi->t1 = hdr->cField; /* most likely 0 */
		return 0;
	}
	/* one-step folllows */
	ppi->flags &= ~PPI_FLAG_WAITING_FOR_F_UP;
	ppi->t1 = sync.originTimestamp;
	pp_time_add(&ppi->t1, &hdr->cField);
	ppi->syncCF = 0;
	if (CONFIG_HAS_P2P && ppi->mech == PP_P2P_MECH)
		pp_servo_got_psync(ppi);
	else
		pp_servo_got_sync(ppi);
	return 0;
}

static int presp_call_servo(struct pp_instance *ppi)
{
	int ret = 0;

	if (is_incorrect(&ppi->t4))
		return 0; /* not an error, just no data */

	pp_timeout_set(ppi, PP_TO_FAULT);
	if (pp_hooks.handle_presp)
		ret = pp_hooks.handle_presp(ppi);
	else
		pp_servo_got_presp(ppi);

	return ret;
}

int st_com_peer_handle_pres(struct pp_instance *ppi, unsigned char *buf,
			    int len)
{
	MsgPDelayResp resp;
	MsgHeader *hdr = &ppi->received_ptp_header;
	int e = 0;

	msg_unpack_pdelay_resp(buf, &resp);

	if ((memcmp(&DSPOR(ppi)->portIdentity.clockIdentity,
		    &resp.requestingPortIdentity.clockIdentity,
		    PP_CLOCK_IDENTITY_LENGTH) == 0) &&
	    ((ppi->sent_seq[PPM_PDELAY_REQ]) ==
	     hdr->sequenceId) &&
	    (DSPOR(ppi)->portIdentity.portNumber ==
	     resp.requestingPortIdentity.portNumber) &&
	    (ppi->flags & PPI_FLAG_FROM_CURRENT_PARENT)) {

		ppi->t4 = resp.requestReceiptTimestamp;
		pp_time_add(&ppi->t4, &hdr->cField);
		/* WARNING: should be "sub" (see README-cfield::BUG)  */
		ppi->t6 = ppi->last_rcv_time;
		if ((hdr->flagField[0] & PP_TWO_STEP_FLAG) != 0)
			ppi->flags |= PPI_FLAG_WAITING_FOR_RF_UP;
		else {
			ppi->flags &= ~PPI_FLAG_WAITING_FOR_RF_UP;
			/*
			 * Make sure responseOriginTimestamp is forced to 0
			 * for one-step responders
			 */
			memset(&ppi->t5, 0, sizeof(ppi->t5));
		}

		if (!(hdr->flagField[0] & PP_TWO_STEP_FLAG))
			e = presp_call_servo(ppi);
	} else {
		pp_diag(ppi, frames, 2, "pp_pclock : "
			"PDelay Resp doesn't match PDelay Req\n");
	}
	return e;
}


int st_com_peer_handle_pres_followup(struct pp_instance *ppi,
				     unsigned char *buf, int plen)
{
	MsgHeader *hdr = &ppi->received_ptp_header;
	MsgPDelayRespFollowUp respFllw;
	int e = 0;

	msg_unpack_pdelay_resp_follow_up(buf, &respFllw);

	if ((memcmp(&DSPOR(ppi)->portIdentity.clockIdentity,
		    &respFllw.requestingPortIdentity.clockIdentity,
		    PP_CLOCK_IDENTITY_LENGTH) == 0) &&
	    ((ppi->sent_seq[PPM_PDELAY_REQ]) ==
	     hdr->sequenceId) &&
	    (DSPOR(ppi)->portIdentity.portNumber ==
	     respFllw.requestingPortIdentity.portNumber) &&
	    (ppi->flags & PPI_FLAG_FROM_CURRENT_PARENT)) {

		ppi->t5 = respFllw.responseOriginTimestamp;
		pp_time_add(&ppi->t5, &hdr->cField);

		e = presp_call_servo(ppi);
	} else {
		pp_diag(ppi, frames, 2, "%s: "
			"PDelay Resp F-up doesn't match PDelay Req\n",
			__func__);
	}
	return e;
}

int st_com_peer_handle_preq(struct pp_instance *ppi, unsigned char *buf,
			    int len)
{
	int e = 0;

	if (pp_hooks.handle_preq)
		e = pp_hooks.handle_preq(ppi);
	if (e)
		return e;

	msg_issue_pdelay_resp(ppi, &ppi->last_rcv_time);
	msg_issue_pdelay_resp_followup(ppi, &ppi->last_snt_time);

	return 0;
}

/* Called by slave and uncalibrated */
int st_com_slave_handle_followup(struct pp_instance *ppi, unsigned char *buf,
				 int len)
{
	MsgFollowUp follow;
	int ret = 0;

	MsgHeader *hdr = &ppi->received_ptp_header;

	if (!(ppi->flags & PPI_FLAG_FROM_CURRENT_PARENT)) {
		pp_error("%s: Follow up message is not from current parent\n",
			__func__);
		return 0;
	}

	if (!(ppi->flags & PPI_FLAG_WAITING_FOR_F_UP)) {
		pp_error("%s: Slave was not waiting a follow up message\n",
			__func__);
		return 0;
	}

	if (ppi->recv_sync_sequence_id != hdr->sequenceId) {
		pp_error("%s: SequenceID %d doesn't match last Sync message %d\n",
				 __func__, hdr->sequenceId, ppi->recv_sync_sequence_id);
		return 0;
	}

	msg_unpack_follow_up(buf, &follow);
	ppi->flags &= ~PPI_FLAG_WAITING_FOR_F_UP;
	/* t1 for calculations is T1 + Csyn + Cful -- see README-cfield */
	pp_time_add(&ppi->t1, &follow.preciseOriginTimestamp);
	pp_time_add(&ppi->t1, &hdr->cField);
	ppi->syncCF = hdr->cField.scaled_nsecs; /* for diag about TC */

	/* Call the extension; it may do it all and ask to return */
	if (pp_hooks.handle_followup)
		ret = pp_hooks.handle_followup(ppi, &ppi->t1);
	if (ret == 1)
		return 0;
	if (ret < 0)
		return ret;

	if (CONFIG_HAS_P2P && ppi->mech == PP_P2P_MECH)
		pp_servo_got_psync(ppi);
	else
		pp_servo_got_sync(ppi);

	return 0;
}

/*
 * Called by master, listenting, passive.
 * FIXME: this must be implemented to support one-step masters
 */
int st_com_master_handle_sync(struct pp_instance *ppi, unsigned char *buf,
			      int len)
{
	/* No more used: follow up is sent right after the corresponding sync */
	return 0;
}

int __send_and_log(struct pp_instance *ppi, int msglen, int chtype)
{
	int msgtype = ((char *)ppi->tx_ptp)[0] & 0xf;
	struct pp_time *t = &ppi->last_snt_time;
	int ret;

	ret = ppi->n_ops->send(ppi, ppi->tx_frame, msglen + ppi->tx_offset,
			       msgtype);
	if (ret == PP_SEND_DROP)
		return 0; /* don't report as error, nor count nor log as sent */
	if (ret < msglen) {
		pp_diag(ppi, frames, 1, "%s(%d) Message can't be sent\n",
			pp_msgtype_info[msgtype].name, msgtype);
		return PP_SEND_ERROR;
	}
	/* FIXME: diagnosticst should be looped back in the send method */
	pp_diag(ppi, frames, 1, "SENT %02d bytes at %d.%09d.%03d (%s)\n",
		msglen, (int)t->secs, (int)(t->scaled_nsecs >> 16),
		((int)(t->scaled_nsecs & 0xffff) * 1000) >> 16,
		pp_msgtype_info[msgtype].name);
	if (chtype == PP_NP_EVT && is_incorrect(&ppi->last_snt_time))
		return PP_SEND_NO_STAMP;

	/* count sent packets */
	ppi->ptp_tx_count++;

	return 0;
}
