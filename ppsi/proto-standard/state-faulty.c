/*
 * Copyright (C) 2011 CERN (www.cern.ch)
 * Author: Aurelio Colosimo
 * Based on PTPd project v. 2.1.0 (see AUTHORS for details)
 *
 * Released according to the GNU LGPL, version 2.1 or any later version.
 */

#include <ppsi/ppsi.h>

/*
 * Fault troubleshooting. Now only comes back to
 * PTP_INITIALIZING state after a grace period.
 */

int pp_faulty(struct pp_instance *ppi, unsigned char *pkt, int plen)
{
	if (pp_timeout(ppi, PP_TO_FAULT)) {
		ppi->next_state = PPS_INITIALIZING;
		return 0;
	}
	ppi->next_delay = pp_next_delay_1(ppi, PP_TO_FAULT);
	return 0;
}
