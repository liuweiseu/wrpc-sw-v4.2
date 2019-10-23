/*
 * Copyright (C) 2011 CERN (www.cern.ch)
 * Author: Aurelio Colosimo
 * Based on PTPd project v. 2.1.0 (see AUTHORS for details)
 *
 * Released according to the GNU LGPL, version 2.1 or any later version.
 */

#include <ppsi/ppsi.h>
#include "common-fun.h"

int pp_passive(struct pp_instance *ppi, unsigned char *pkt, int plen)
{
	int e = 0; /* error var, to check errors in msg handling */

	/* when the clock is using peer-delay, listening must send it too */
	if (CONFIG_HAS_P2P && ppi->mech == PP_P2P_MECH)
		e  = pp_lib_may_issue_request(ppi);

	switch (ppi->received_ptp_header.messageType) {

	case PPM_ANNOUNCE:
		e = pp_lib_handle_announce(ppi, pkt, plen);
		break;

	case PPM_SYNC:
		e = st_com_master_handle_sync(ppi, pkt, plen);
		break;

	case PPM_PDELAY_REQ:
		if (CONFIG_HAS_P2P)
			st_com_peer_handle_preq(ppi, pkt, plen);
		break;

	case PPM_PDELAY_RESP:
		if (CONFIG_HAS_P2P)
			e = st_com_peer_handle_pres(ppi, pkt, plen);
		break;

	case PPM_PDELAY_R_FUP:
		if (CONFIG_HAS_P2P)
			e = st_com_peer_handle_pres_followup(ppi, pkt, plen);
		break;

	default:
		/* disreguard, nothing to do */
		break;

	}

	if (e == 0)
		e = st_com_execute_slave(ppi);

	if (e != 0) {
		/* ignore: a lost frame is not the end of the world */
	}

	ppi->next_delay = PP_DEFAULT_NEXT_DELAY_MS;

	return 0;
}
