/*
 * Copyright (C) 2012 CERN (www.cern.ch)
 * Author: Aurelio Colosimo
 * Based on ptp-noposix project (see AUTHORS for details)
 *
 * Released to the public domain
 */

#include <ppsi/ppsi.h>
#include "wr-api.h"

/*
 * This is the last WR state: ack the other party and go master or slave.
 * There is no timeout nor a check for is_new_state: we just do things once
 */
int wr_link_on(struct pp_instance *ppi, unsigned char *pkt, int plen)
{
	struct wr_dsport *wrp = WR_DSPOR(ppi);
	int e = 0;

	wrp->wrModeOn = TRUE;
	wrp->ops->enable_ptracker(ppi);

	if (wrp->wrMode == WR_MASTER)
		e = msg_issue_wrsig(ppi, WR_MODE_ON);

	wrp->parentWrModeOn = TRUE;

	if (e != 0)
		return -1;

	if (wrp->wrMode == WR_SLAVE)
		ppi->next_state = PPS_SLAVE;
	else
		ppi->next_state = PPS_MASTER;

#ifdef CONFIG_ABSCAL
	/*
	 * absolute calibration only exists in arch-wrpc, so far, but
	 * we can't include wrpc headers, not available in wrs builds
	 */
	extern int ptp_mode;
	extern int ep_get_bitslide(void);

	if (ptp_mode == 4 /* WRC_MODE_ABSCAL */) {
		ppi->next_state = WRS_ABSCAL;
		/* print header for the serial port stream of stamps */
		pp_printf("### t4.phase is already corrected for bitslide\n");
		pp_printf("t1:                     t4:                  "
			  "bitslide: %d\n",ep_get_bitslide());
		pp_printf("      sec.       ns.pha       sec.       ns.pha\n");
	}
#endif

	return 0;
}
