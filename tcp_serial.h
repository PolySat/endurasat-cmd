#ifndef TCP_SERIAL_H
#define TCP_SERIAL_H

#include "serial.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Constructor for TCP serial interface
 * @param si double pointer to the serial interface struct that will be
 *             allocated on a succesful call to serialInit.
 * @param proc a pointer to the process contex for which this interface will
 *             register events.
 * @param readCallback function pointer to callback function that handles reads.
 * @param readCallback function pointer to callback function that handles reads.
 * @param opaque pointer to whatever developer desires. Passed to read callback.
 * @return -1 on error, 0 on success. Check /var/log/syslog on error.
 */
int tcpSerialInit(struct serialInterface **si,
                  struct EventState *evl_loop,
                  serialReadCB readCallback,
                  serialConnectCB connectCallback,
                  const char *deviceFile,
                  int baudRate,
                  const char *eolMarker,
                  void *opaque);

#ifdef __cplusplus
}
#endif


#endif
