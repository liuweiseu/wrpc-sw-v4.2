#ifndef __LIBWR_HW_UTIL_H
#define __LIBWR_HW_UTIL_H

#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define atoidef(argc, argv, param_i, def) (argc > (param_i) ? \
					    atoi(argv[(param_i)]) : (def))
#define strtoldef(argc, argv, param_i, def) (argc > (param_i) ? \
				    strtol(argv[(param_i)], NULL, 0) : (def))

void shw_udelay_init(void);
void shw_udelay(uint32_t microseconds);
/* get monotonic number of useconds */
uint64_t get_monotonic_us(void);
/* get monotonic number of seconds */
time_t get_monotonic_sec(void);

/* Change endianess of the string, for example when accessing strings in
 * the SoftPLL */
void strncpy_e(char *d, char *s, int len);

/* Create map */
void *create_map(unsigned long address, unsigned long size);

#endif /* __LIBWR_HW_UTIL_H */
