#include "creds.h"
#include "devcert.h"

const char *creds_device_name(void)
{
    /* The client certificate decides the name: the broker takes the identity from its CN. */
    const char *cn = devcert_cn();
    return cn ? cn : "unprovisioned";
}
