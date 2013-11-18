#include "libxl_osdeps.h" /* must come before any other headers */

#define VNUMA_NO_NODE ~((unsigned int)0)

/* 
 * Max vNUMA node size in Mb is taken 64Mb even now Linux lets
 * 32Mb, thus letting some slack. Will be modified to match Linux.
 */
#define MIN_VNODE_SIZE  64U

#define MAX_VNUMA_NODES (unsigned int)1 << 10

