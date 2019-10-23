/*
 * Copyright (C) 2013 CERN (www.cern.ch)
 * Author: Alessandro Rubini
 *
 * Released according to the GNU LGPL, version 2.1 or any later version.
 */
#include <stdarg.h>
#include <ppsi/ppsi.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static char *thing_name[] = {
	[pp_dt_fsm]	= "diag-fsm",
	[pp_dt_time]	= "diag-time",
	[pp_dt_frames]	= "diag-frames",
	[pp_dt_servo]	= "diag-servo",
	[pp_dt_bmc]	= "diag-bmc",
	[pp_dt_ext]	= "diag-extension",
	[pp_dt_config]	= "diag-config",
};


void __pp_diag(struct pp_instance *ppi, enum pp_diag_things th,
		      int level, char *fmt, ...)
{
	va_list args;
	char *name = ppi ? ppi->port_name : "ppsi";

	if (!__PP_DIAG_ALLOW(ppi, th, level))
		return;

	/* Use the normal output channel for diagnostics */
	pp_printf("%s-%i-%s: ", thing_name[th], level, name);
	va_start(args, fmt);
	pp_vprintf(fmt, args);
	va_end(args);
}

unsigned long pp_diag_parse(char *diaglevel)
{
	unsigned long res = 0;
	int i = 28; /* number of bits to shift the nibble: 28..31 is first */
	int nthings = ARRAY_SIZE(thing_name);

	while (*diaglevel && i >= (32 - 4 * nthings)) {
		if (*diaglevel < '0' || *diaglevel > '3')
			break;
		res |= ((*diaglevel - '0') << i);
		i -= 4;
		diaglevel++;
	}
	if (*diaglevel)
		pp_printf("%s: error parsing \"%s\"\n", __func__, diaglevel);

	return res;
}
