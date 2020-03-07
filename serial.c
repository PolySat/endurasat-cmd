#include <stdint.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include "serial.h"
#include "tcp_serial.h"

#define SERIAL_OPEN_FLAGS O_RDWR | O_NOCTTY
#define READBUFFER_SIZE 4096
#define WRITEBUFFER_SIZE 4096

#define PRIV(arg) ((struct serialInterfacePriv *) (arg))

struct serialInterfacePriv {
   int (*cleanup)(struct serialInterfacePriv *self);
   int (*write)(struct serialInterfacePriv *self, void *src, int bytes);

   // Private fields
   int fd; // serial device FD
   serialReadCB readCB; // callback up controlling context
   struct EventState *evt_loop; // Pointer to proclib process context
   char writeBuff[WRITEBUFFER_SIZE]; // Write buffer bytes
   char readBuff[READBUFFER_SIZE + 1]; // Write buffer bytes
   int readBytes;
   uint32_t writeBytes; // Bytes to write field
   void *opaque;
   char *eolMarker;
};

static int configureSerial(int fd, tcflag_t cflag, speed_t baudrate)
{
   struct termios tty;

   // Get current serial settings
   if (0 != tcgetattr (fd, &tty)) {
      DBG_print(DBG_LEVEL_WARN, "Error configuring serial device\n",
                                 strerror(errno));
      return -1;
   }

   // Flush serial interface
   if (0 != tcflush(fd, TCIOFLUSH)) {
      DBG_print(DBG_LEVEL_WARN, "Error flushing serial device\n",
                                 strerror(errno));
      return -1;
   }

   tty.c_cflag = cflag;
   tty.c_iflag = 0;
   tty.c_lflag = 0;
   tty.c_oflag = 0;
   cfsetospeed (&tty, baudrate);
   cfsetispeed (&tty, baudrate);

   // Set serial settings
   if (0 != tcsetattr (fd, TCSANOW, &tty)) {
      DBG_print(DBG_LEVEL_WARN, "Error configuring serial device\n",
                                 strerror(errno));
      return -1;
   }

   return 0;
}

static tcflag_t parseCFlag(const char *cflagstr) // TODO parse mas / better parse
{
   tcflag_t cflag = 0;

   if (strstr(cflagstr, "CS8"))
      cflag |= CS8;
   if (strstr(cflagstr, "CREAD"))
      cflag |= CREAD;
   if (strstr(cflagstr, "CLOCAL"))
      cflag |= CLOCAL;
   if (strstr(cflagstr, "CRTSCTS"))
      cflag |= CRTSCTS;

   return cflag;
}

static speed_t parseBaudrate(uint32_t baudrateInt)
{
   speed_t baudrate = 0;

   switch(baudrateInt) {

      case 50:
         baudrate = B50;
         break;

      case 75:
         baudrate = B75;
         break;

      case 110:
         baudrate = B110;
         break;

      case 134: // NOTE this is actually 134.5 baud
         baudrate = B134;
         break;

      case 150:
         baudrate = B150;
         break;

      case 200:
         baudrate = B200;
         break;

      case 300:
         baudrate = B300;
         break;

      case 600:
         baudrate = B600;
         break;

      case 1200:
         baudrate = B1200;
         break;

      case 1800:
         baudrate = B1800;
         break;

      case 2400:
         baudrate = B2400;
         break;

      case 4800:
         baudrate = B4800;
         break;

      case 9600:
         baudrate = B9600;
         break;

      case 19200:
         baudrate = B19200;
         break;

      case 38400:
         baudrate = B38400;
         break;

      case 57600:
         baudrate = B57600;
         break;

      case 115200:
         baudrate = B115200;
         break;

      case 230400:
         baudrate = B230400;
         break;

#ifndef __APPLE__
      case 460800:
         baudrate = B460800;
         break;
#endif

      default:
         DBG_print(DBG_LEVEL_WARN, "Unsupported baudrate (%i)\n", baudrateInt);
   }

   return baudrate;
}

static int readEvent(int fd, char type, void *si)
{
   int bytesread;
   char *eol;
   int i;

   // Perform the read
   bytesread = read(fd, PRIV(si)->readBuff + PRIV(si)->readBytes,
         READBUFFER_SIZE - PRIV(si)->readBytes);
   if (bytesread < 0) {
      DBG_print(DBG_LEVEL_WARN, "Error reading from serial device: %s\n",
                                 strerror(errno));
      return EVENT_REMOVE;
   }
   PRIV(si)->readBytes += bytesread;
   PRIV(si)->readBuff[PRIV(si)->readBytes] = 0;

   // Pass data back to callback
   if (PRIV(si)->readCB) {
      // No EOL marker provided, call the callback 
      if (!PRIV(si)->eolMarker) {
         PRIV(si)->readCB(PRIV(si)->readBuff, bytesread, PRIV(si)->opaque);
         PRIV(si)->readBytes = 0;
         return EVENT_KEEP;
      }

      while ((eol = strstr(PRIV(si)->readBuff, PRIV(si)->eolMarker))) {
         bytesread = eol - PRIV(si)->readBuff;
         *eol = 0;
         PRIV(si)->readCB(PRIV(si)->readBuff, bytesread, PRIV(si)->opaque);
         bytesread += strlen(PRIV(si)->eolMarker);
         PRIV(si)->readBytes -= bytesread;
         for (i = 0; i < PRIV(si)->readBytes; i++)
            PRIV(si)->readBuff[i] = PRIV(si)->readBuff[i + bytesread];
         PRIV(si)->readBuff[PRIV(si)->readBytes] = 0;
      }
   }
   else // Discard all bytes if no read callback
      PRIV(si)->readBytes = 0;

   return EVENT_KEEP;
}

static int writeEvent(int fd, char type, void *si)
{
   if (PRIV(si)->writeBytes == 0)
      return EVENT_REMOVE;

   // Perform the write
   if (-1 == write(PRIV(si)->fd, PRIV(si)->writeBuff, PRIV(si)->writeBytes)) {
      DBG_print(DBG_LEVEL_WARN, "Error writing to serial device: %s\n",
                                 strerror(errno));
      PRIV(si)->writeBytes = 0;
      return EVENT_REMOVE;
   }

   PRIV(si)->writeBytes = 0;

   return EVENT_REMOVE;
}

static int serialCleanup(struct serialInterface *si)
{
   if (PRIV(si)->eolMarker)
      free(PRIV(si)->eolMarker);

   // Close the serial port file
   if (-1 == close(PRIV(si)->fd)) {
      DBG_print(DBG_LEVEL_WARN, "Unable to close serial device: %s\n",
                                    strerror(errno));
      free(si);
      return -1;
   }
   free(si);

   return 0;
}

static int serialWrite(struct serialInterface *si, void *src, int bytes)
{
   if (bytes > (WRITEBUFFER_SIZE - PRIV(si)->writeBytes) ) {
      DBG_print(DBG_LEVEL_WARN,
                  "Cannot write %i bytes, write buffer size = %i\n",
                  bytes, WRITEBUFFER_SIZE - PRIV(si)->writeBytes);
      return -1;
   }

   int regCallback = 0 == PRIV(si)->writeBytes;

   memcpy(&PRIV(si)->writeBuff[PRIV(si)->writeBytes], src, bytes);
   PRIV(si)->writeBytes += bytes;

   // Register write callback event handler
   if (regCallback)
      EVT_fd_add(PRIV(si)->evt_loop,
                 PRIV(si)->fd,
                 EVENT_FD_WRITE,
                 writeEvent,
                 (void *) si);

   return 0;
}

int serialInit(struct serialInterface **si,
                  struct EventState *evt_loop,
                  serialReadCB readCallback,
                  serialConnectCB connectCallback,
                  const char *devFile,
                  int baudRate,
                  const char *eolMarker,
                  void *opaque)
{
   tcflag_t cflag;
   speed_t baudrate;

   if (devFile && 0 == strncasecmp("tcp://", devFile, 6))
      return tcpSerialInit(si, evt_loop, readCallback, connectCallback,
            devFile, baudRate, eolMarker, opaque);

   cflag = parseCFlag("CS8");
   baudrate = parseBaudrate(baudRate);

   // Allocate memory for serial struct
   *si = (struct serialInterface *) malloc(sizeof(struct serialInterfacePriv));
   if (*si == NULL) {
      DBG_print(DBG_LEVEL_WARN, "Insufficient memory\n");
      return -1;
   }

   // Copy the end-of-line marker
   if (eolMarker)
      PRIV(*si)->eolMarker = strdup(eolMarker);
   else
      PRIV(*si)->eolMarker = NULL;

   // Open serial interface
   if (-1 == (PRIV(*si)->fd = open(devFile, SERIAL_OPEN_FLAGS))) {
      DBG_print(DBG_LEVEL_WARN, "Unable to open serial device: %s\n",
                                 strerror(errno));
      free(*si);
      *si = NULL;
      return -1;
   }

   // Configure serial interface
   if (-1 == configureSerial(PRIV(*si)->fd, cflag, baudrate)) {
      free(*si);
      *si = NULL;      
      return -1;
   }

   (*si)->write = serialWrite;
   (*si)->cleanup = serialCleanup;
   PRIV(*si)->readCB = readCallback;
   PRIV(*si)->evt_loop = evt_loop;
   PRIV(*si)->opaque = opaque;
   PRIV(*si)->readBytes = 0;

   // Register read callback event handler
   EVT_fd_add(evt_loop,
                 PRIV(*si)->fd,
                 EVENT_FD_READ,
                 readEvent,
                 (void *) *si);

   if (connectCallback)
      connectCallback(1, opaque);

   return 0;
}
