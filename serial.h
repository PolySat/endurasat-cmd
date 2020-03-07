#ifndef SERIAL_H
#define SERIAL_H

#include <polysat/polysat.h>

#ifdef __cplusplus
extern "C" {
#endif

// Generic interface for a serial device.
struct serialInterface {

   /* Write to serial device.
    * @param self a reference to the serial device being written to.
    * @param src a pointer to the bytes to be written.
    * @param bytes the number of bytes to write.
    * @return -1 on error, 0 on success. Check /var/log/syslog on error.
    */
   int (*write)(struct serialInterface *self, void *src, int bytes);

   /* Cleanup the serial device interface resources.
    * @param self a reference to the serial device being deconstructed.
    * @return -1 on error, 0 on success. Check /var/log/syslog on error.
    */
   int (*cleanup)(struct serialInterface *self);
};

/* Type definition of read callback for serial interface.
 * @param buffer a pointer to the bytes read.
 * @param bytes the number of bytes read.
 * @param opaque user supplied argument
 */
typedef void (*serialReadCB)(void *buffer, int bytes, void *opaque);

/* Type definition of connection callback for serial interface.
 * @param status true for connected, false for disconnected
 * @param opaque user supplied argument
 */
typedef void (*serialConnectCB)(int status, void *opaque);

/* Constructor for serial interface
 * @param si double pointer to the serial interface struct that will be
 *             allocated on a succesful call to serialInit.
 * @param proc a pointer to the process contex for which this interface will
 *             register events.
 * @param readCallback function pointer to callback function that handles reads.
 * @param readCallback function pointer to callback function that handles reads.
 * @param opaque pointer to whatever developer desires. Passed to read callback.
 * @return -1 on error, 0 on success. Check /var/log/syslog on error.
 */
int serialInit(struct serialInterface **si,
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
