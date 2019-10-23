#ifndef __ARCH_H__
#define __ARCH_H__
#include <assert.h> /* wrpc-sw includes assert already */

/* This arch exports wr functions, so include this for consistency checking */
#include "../proto-ext-whiterabbit/wr-api.h"

/* Architecture-specific defines, included by top-level stuff */

#ifndef htons /* If we build as host process, we have them LE already */
#  define htons(x)      (x)
#  define htonl(x)      (x)

#  define ntohs htons
#  define ntohl htonl
#endif

#define abs(x) ((x >= 0) ? x : -x)
#endif /* __ARCH_H__ */
