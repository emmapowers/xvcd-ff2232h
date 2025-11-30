#ifndef FTDI_XVC_CORE_H
#define FTDI_XVC_CORE_H

#include <stdint.h>
#include <ftdi.h>

/** Opaque XVC context handle */
typedef struct ftdi_xvc_ctx ftdi_xvc_ctx;

/** Create a new XVC context */
ftdi_xvc_ctx *ftdi_xvc_create(unsigned int verbosity);

/** Destroy an XVC context */
void ftdi_xvc_destroy(ftdi_xvc_ctx *ctx);

/** Open the FTDI device */
int ftdi_xvc_open_device(ftdi_xvc_ctx *ctx, int vendor, int product,
                         const char *serial, enum ftdi_interface iface);

/** Close the FTDI device */
void ftdi_xvc_close_device(ftdi_xvc_ctx *ctx);

/** Initialize the MPSSE engine */
int ftdi_xvc_init_mpsse(ftdi_xvc_ctx *ctx, unsigned int freq_hz);

/** Set TCK period dynamically, returns actual achieved period in ns */
uint32_t ftdi_xvc_set_tck_period(ftdi_xvc_ctx *ctx, uint32_t period_ns);

/** Handle a shift command */
int ftdi_xvc_shift_command(ftdi_xvc_ctx *ctx, unsigned int len,
                           unsigned char *buffer, unsigned char *result);

#endif // FTDI_XVC_CORE_H
