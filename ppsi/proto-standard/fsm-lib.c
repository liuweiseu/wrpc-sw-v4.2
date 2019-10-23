/*
 * Copyright (C) 2011 CERN (www.cern.ch)
 * Author: Aurelio Colosimo
 * Copyright (C) 2014 GSI (www.gsi.de)
 * Author: Alessandro Rubini
 *
 * Released according to the GNU LGPL, version 2.1 or any later version.
 */
#include <ppsi/ppsi.h>

/* Local functions that build to nothing when Kconfig selects 0/1 vlans */
static int pp_vlan_issue_announce(struct pp_instance *ppi)
{
	int i, vlan = 0;

	if (CONFIG_VLAN_ARRAY_SIZE && ppi->nvlans == 1)
		vlan = ppi->vlans[0];

	if (CONFIG_VLAN_ARRAY_SIZE <= 1 || ppi->nvlans <= 1) {
		ppi->peer_vid = vlan;
		return msg_issue_announce(ppi);
	}

	/*
	 * If Kconfig selected 0/1 vlans, this code is not built.
	 * If we have several vlans, we replace peer_vid and proceed;
	 */
	for (i = 0; i < ppi->nvlans; i++) {
		ppi->peer_vid = ppi->vlans[i];
		msg_issue_announce(ppi);
		/* ignore errors: each vlan is separate */
	}
	return 0;
}

static int pp_vlan_issue_sync_followup(struct pp_instance *ppi)
{
	int i, vlan = 0;

	if (CONFIG_VLAN_ARRAY_SIZE && ppi->nvlans == 1)
		vlan = ppi->vlans[0];

	if (CONFIG_VLAN_ARRAY_SIZE <= 1 || ppi->nvlans <= 1) {
		ppi->peer_vid = vlan;
		return msg_issue_sync_followup(ppi);
	}

	/*
	 * If Kconfig selected 0/1 vlans, this code is not built.
	 * If we have several vlans, we replace peer_vid and proceed;
	 */
	for (i = 0; i < ppi->nvlans; i++) {
		ppi->peer_vid = ppi->vlans[i];
		msg_issue_sync_followup(ppi);
		/* ignore errors: each vlan is separate */
	}
	return 0;
}

/*
 * The following set of functions help the states in the state machine.
 * Ideally, we should manage to get to a completely table-driven fsm
 * implementation based on these helpers
 */

int pp_lib_may_issue_sync(struct pp_instance *ppi)
{
	int e;

	if (!pp_timeout(ppi, PP_TO_SYNC_SEND))
		return 0;

	pp_timeout_set(ppi, PP_TO_SYNC_SEND);
	e = pp_vlan_issue_sync_followup(ppi);
	if (e)
		pp_diag(ppi, frames, 1, "could not send sync\n");
	return e;
}

int pp_lib_may_issue_announce(struct pp_instance *ppi)
{
	int e;

	if (!pp_timeout(ppi, PP_TO_ANN_SEND))
		return 0;

	pp_timeout_set(ppi, PP_TO_ANN_SEND);
	e = pp_vlan_issue_announce(ppi);
	if (e)
		pp_diag(ppi, frames, 1, "could not send announce\n");
	return e;
}

int pp_lib_may_issue_request(struct pp_instance *ppi)
{
	int e = 0;

	if (!pp_timeout(ppi, PP_TO_REQUEST))
		return 0;

	pp_timeout_set(ppi, PP_TO_REQUEST);
	e = msg_issue_request(ppi); /* FIXME: what about multiple vlans? */
	ppi->t3 = ppi->last_snt_time;
	if (e == PP_SEND_ERROR) {
		pp_diag(ppi, frames, 1, "could not send request\n");
		return e;
	}
	return 0;
}

/* Called by this file, basically when an announce is got, all states */
static void __lib_add_foreign(struct pp_instance *ppi, unsigned char *buf)
{
	int i;
	MsgHeader *hdr = &ppi->received_ptp_header;

	/* Check if foreign master is already known */
	for (i = 0; i < ppi->frgn_rec_num; i++) {
		if (!memcmp(&hdr->sourcePortIdentity,
			    &ppi->frgn_master[i].port_id,
			    sizeof(hdr->sourcePortIdentity))) {
			/* already in Foreign master data set, update info */
			msg_copy_header(&ppi->frgn_master[i].hdr, hdr);
			msg_unpack_announce(buf, &ppi->frgn_master[i].ann);
			return;
		}
	}

	/* New foreign master */
	if (ppi->frgn_rec_num < PP_NR_FOREIGN_RECORDS)
		ppi->frgn_rec_num++;

	/* FIXME: replace the worst */
	i = ppi->frgn_rec_num - 1;

	/* Copy new foreign master data set from announce message */
	memcpy(&ppi->frgn_master[i].port_id,
	       &hdr->sourcePortIdentity, sizeof(hdr->sourcePortIdentity));

	/*
	 * header and announce field of each Foreign Master are
	 * useful to run Best Master Clock Algorithm
	 */
	msg_copy_header(&ppi->frgn_master[i].hdr, hdr);
	msg_unpack_announce(buf, &ppi->frgn_master[i].ann);

	pp_diag(ppi, bmc, 1, "New foreign Master %i added\n", i);
}

int pp_lib_handle_announce(struct pp_instance *ppi, unsigned char *buf, int len)
{
	__lib_add_foreign(ppi, buf);

	ppi->next_state = bmc(ppi); /* got a new announce: run bmc */
	pp_timeout_set(ppi, PP_TO_ANN_RECEIPT);

	if (pp_hooks.handle_announce)
		return pp_hooks.handle_announce(ppi);
	return 0;
}
