#include <ppsi/ppsi.h>
#include "common-fun.h"
#include "wr-api.h"

/*
 * This is similar to master state, but it only sends sync, that
 * is going to be timestampedboth internally and externally (on a scope).
 * We should send it as close as possible to the pps signal.
 */


/* Calculate when is the next pps going to happen */
static int next_pps_ms(struct pp_instance *ppi, struct pp_time *t)
{
	int nsec;

	ppi->t_ops->get(ppi, t);
	nsec = t->scaled_nsecs >> 16;
	return  1000 - (nsec / 1000 / 1000);
}

/*
 * This is using a software loop during the last 10ms in order to get
 * right after the pps event
 */
int wr_abscal(struct pp_instance *ppi, uint8_t *pkt, int plen)
{
	struct pp_time t;
	int len, i;

	if (ppi->is_new_state) {
		/* add 1s to be enough in the future, the first time */
		__pp_timeout_set(ppi, PP_TO_EXT_0, 990 + next_pps_ms(ppi, &t));
		return 0;
	}

	i = next_pps_ms(ppi, &t) - 10;
	if (pp_timeout(ppi, PP_TO_EXT_0)) {
		uint64_t secs = t.secs;

		/* Wait for the second to tick */
		while( ppi->t_ops->get(ppi, &t), t.secs == secs)
			;

		/* Send sync, no f-up -- actually we could send any frame */
		ppi->t_ops->get(ppi, &t);
		len = msg_pack_sync(ppi, &t);
		__send_and_log(ppi, len, PP_NP_EVT);

		/* And again next second */
		__pp_timeout_set(ppi, PP_TO_EXT_0, next_pps_ms(ppi, &t) - 10);
		ppi->next_delay = next_pps_ms(ppi, &t) - 10;
		return 0;
	}
	/* no timeout: wait according to next_pps_ms calculated earlier */
	ppi->next_delay = i > 0 ? i : 0;
	return 0;
}
