#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include "tcp_serial.h"
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <netdb.h>

#define CONNECT_RETRY_TIME EVT_ms2tv(10*1000)

#define READBUFFER_SIZE 4096
#define WRITEBUFFER_SIZE 4096

#define PRIV(arg) ((struct tcpSerialInterfacePriv *) (arg))

static int initiate_remote_connection_event(void *arg);
static int sock_write_callback(int fd, char type, void *arg);
static int close_connection_event(void *arg);

struct WriteNode {
   int data_len;
   struct WriteNode *next;
   char data[1];
};

struct tcpSerialInterfacePriv {
   int (*cleanup)(struct tcpSerialInterfacePriv *self);
   int (*write)(struct tcpSerialInterfacePriv *self, void *src, int bytes);

   // Private fields
   int sockfd; // serial device FD
   struct sockaddr_in server_addr;
   char *server_name;
   void *connect_event, *close_event;

   serialConnectCB connectCallback;
   serialReadCB readCB; // callback up controlling context
   struct EventState *evt_loop; // Pointer to proclib process context
   char writeBuff[WRITEBUFFER_SIZE]; // Write buffer bytes
   char readBuff[READBUFFER_SIZE + 1]; // Write buffer bytes
   int readBytes;
   uint32_t writeBytes; // Bytes to write field
   void *opaque;
   char *eolMarker;
   int write_reg, read_reg, connect_reg;
   struct WriteNode *writes;
   struct WriteNode *writes_tail;
};

static int tcpReadEvent(int fd, char type, void *si)
{
   struct tcpSerialInterfacePriv *self = PRIV(si);
   int bytesread;
   char *eol;
   int i;

   // Perform the read
   bytesread = read(self->sockfd, self->readBuff + self->readBytes,
         READBUFFER_SIZE - self->readBytes);
   if (bytesread < 0) {
      if (errno == EAGAIN)
         return EVENT_KEEP;

      perror ("Read error");
      self->read_reg = 0;
      if (!self->close_event)
         self->close_event = EVT_sched_add(self->evt_loop,
            EVT_ms2tv(0), &close_connection_event, self);

      if (self->connectCallback)
         (*self->connectCallback)(0, self->opaque);

      return EVENT_REMOVE;
   }
   else if (bytesread == 0) {
      printf("Remote end closed connection\n");
      self->read_reg = 0;
      if (!self->close_event)
         self->close_event = EVT_sched_add(self->evt_loop,
            EVT_ms2tv(0), &close_connection_event, self);

      if (self->connectCallback)
         (*self->connectCallback)(0, self->opaque);

      return EVENT_REMOVE;
   }

   self->readBytes += bytesread;
   self->readBuff[self->readBytes] = 0;

   // Pass data back to callback
   if (self->readCB) {
      // No EOL marker provided, call the callback 
      if (!self->eolMarker) {
         self->readCB(PRIV(si)->readBuff, bytesread, self->opaque);
         self->readBytes = 0;
         return EVENT_KEEP;
      }

      while ((eol = strstr(self->readBuff, self->eolMarker))) {
         bytesread = eol - self->readBuff;
         *eol = 0;
         self->readCB(self->readBuff, bytesread, self->opaque);
         bytesread += strlen(self->eolMarker);
         self->readBytes -= bytesread;
         for (i = 0; i < self->readBytes; i++)
            self->readBuff[i] = self->readBuff[i + bytesread];
         self->readBuff[self->readBytes] = 0;
      }
   }
   else // Discard all bytes if no read callback
      self->readBytes = 0;

   return EVENT_KEEP;
}

static int tcpSerialCleanup(struct serialInterface *si)
{
   struct tcpSerialInterfacePriv *self = PRIV(si);

   if (self->eolMarker)
      free(self->eolMarker);

   if (self->write_reg) {
      EVT_fd_remove(self->evt_loop, self->sockfd, EVENT_FD_WRITE);
      self->write_reg = 0;
   }

   if (self->connect_reg) {
      EVT_fd_remove(self->evt_loop, self->sockfd, EVENT_FD_WRITE);
      self->connect_reg = 0;
   }

   if (self->read_reg) {
      EVT_fd_remove(self->evt_loop, self->sockfd, EVENT_FD_READ);
      self->read_reg = 0;
   }

   if (self->server_name) {
      free(self->server_name);
      self->server_name = NULL;
   }

   if (self->close_event)
      EVT_sched_remove(self->evt_loop, self->close_event);
   self->close_event = NULL;

   if (self->connect_event)
      EVT_sched_remove(self->evt_loop, self->connect_event);
   self->connect_event = NULL;

   if (self->sockfd) {
      close(self->sockfd);
      self->sockfd = 0;
   }

   if (self->connectCallback)
      (*self->connectCallback)(0, self->opaque);

   free(si);

   return 0;
}

static int tcpSerialWrite(struct serialInterface *si, void *src, int bytes)
{
   struct tcpSerialInterfacePriv *self = PRIV(si);
   struct WriteNode *wr;

   if (!self->read_reg)
      return 0;

   wr = malloc(sizeof(*wr) + bytes);
   if (!wr)
      return 0;

   wr->data_len = bytes;
   wr->next = NULL;
   memcpy(wr->data, src, bytes);

   if (!self->writes)
      self->writes = self->writes_tail = wr;
   else {
      self->writes_tail->next = wr;
      self->writes_tail = wr;
   }

   if (!self->write_reg) {
      EVT_fd_add(self->evt_loop, self->sockfd, EVENT_FD_WRITE,
         &sock_write_callback, self);
      self->write_reg = 1;
   }

   return 0;
}

static int close_connection_event(void *arg)
{
   struct tcpSerialInterfacePriv *self = PRIV(arg);

   if (self->write_reg) {
      EVT_fd_remove(self->evt_loop, self->sockfd, EVENT_FD_WRITE);
      self->write_reg = 0;
   }

   if (self->connect_reg) {
      EVT_fd_remove(self->evt_loop, self->sockfd, EVENT_FD_WRITE);
      self->connect_reg = 0;
   }

   if (self->read_reg) {
      EVT_fd_remove(self->evt_loop, self->sockfd, EVENT_FD_READ);
      self->read_reg = 0;
   }

   if (self->connect_event)
      EVT_sched_remove(self->evt_loop, self->connect_event);
   self->connect_event = NULL;

   if (self->sockfd) {
      close(self->sockfd);
      self->sockfd = 0;
   }

   self->connect_event = EVT_sched_add(self->evt_loop,
     CONNECT_RETRY_TIME, &initiate_remote_connection_event, self);

   self->close_event = NULL;

   if (self->connectCallback)
      (*self->connectCallback)(0, self->opaque);

   return EVENT_REMOVE;
}

static int sock_write_callback(int fd, char type, void *arg)
{
   struct tcpSerialInterfacePriv *self = PRIV(arg);
   struct WriteNode *wr;

   wr = self->writes;
   if (wr) {
      self->writes = wr->next;
      if (!self->writes)
         self->writes_tail = NULL;

      write(self->sockfd, wr->data, wr->data_len);
      //printf("TX Packet length %d / %d\n", len, wr->data_len);
      free(wr);
   }

   if (!self->writes) {
      self->write_reg = 0;
      return EVENT_REMOVE;
   }

   return EVENT_KEEP;
}


int sock_notify_connect(void *arg)
{
   struct tcpSerialInterfacePriv *self = PRIV(arg);

   if (self->connectCallback)
      (*self->connectCallback)(1, self->opaque);

   return EVENT_REMOVE;
}

static int sock_connect_callback(int fd, char type, void *arg)
{
   struct tcpSerialInterfacePriv *self = PRIV(arg);
   int sockerr;
   socklen_t len = sizeof(sockerr);
   if (getsockopt (self->sockfd, SOL_SOCKET, SO_ERROR, &sockerr,
            &len) < 0) {
      perror("Error reading sockopt, fatal!");

      self->connect_reg = 0;
      close(self->sockfd);
      self->sockfd = 0;
      if (!self->connect_event)
         self->connect_event = EVT_sched_add(self->evt_loop,
            CONNECT_RETRY_TIME, &initiate_remote_connection_event, self);

      if (self->connectCallback)
         (*self->connectCallback)(0, self->opaque);

      return EVENT_REMOVE;
   }

   if (sockerr != 0) {
      printf("sockerr %d\n", sockerr);

      self->connect_reg = 0;
      close(self->sockfd);
      self->sockfd = 0;
      if (!self->connect_event)
         self->connect_event = EVT_sched_add(self->evt_loop,
            CONNECT_RETRY_TIME, &initiate_remote_connection_event, self);

      if (self->connectCallback)
         (*self->connectCallback)(0, self->opaque);

      return EVENT_REMOVE;
   }

   EVT_fd_add(self->evt_loop, self->sockfd, EVENT_FD_READ,
      &tcpReadEvent, self);

   self->read_reg = 1;
   self->connect_reg = 0;

   if (self->connectCallback) {
      EVT_sched_add(self->evt_loop,
            EVT_ms2tv(0), &sock_notify_connect, self);
   }

   return EVENT_REMOVE;
}

static int initiate_remote_connection_event(void *arg)
{
   struct tcpSerialInterfacePriv *self = PRIV(arg);
   int res;
   int flags;

   self->connect_event = NULL;
   if (!self->server_addr.sin_addr.s_addr || !self->server_addr.sin_port) {
      return EVENT_REMOVE;
   }

   printf("Connecting to server %s:%d\n", inet_ntoa(self->server_addr.sin_addr),
      ntohs(self->server_addr.sin_port));

   if ((self->sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
      perror("Failed to allocate socket");
      self->connect_event = EVT_sched_add(self->evt_loop,
        CONNECT_RETRY_TIME, &initiate_remote_connection_event, self);
      if (self->connectCallback)
         (*self->connectCallback)(0, self->opaque);
      return EVENT_REMOVE;
   }

   flags = fcntl(self->sockfd, F_GETFL, 0);
   if (flags < 0) {
      perror("nonblock");
      close(self->sockfd);
      self->sockfd = 0;
      self->connect_event = EVT_sched_add(self->evt_loop,
        CONNECT_RETRY_TIME, &initiate_remote_connection_event, self);
      if (self->connectCallback)
         (*self->connectCallback)(0, self->opaque);
      return EVENT_REMOVE;
   }

   if (fcntl(self->sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
      perror("nonblock2");
      close(self->sockfd);
      self->sockfd = 0;
      self->connect_event = EVT_sched_add(self->evt_loop,
        CONNECT_RETRY_TIME, &initiate_remote_connection_event, self);
      if (self->connectCallback)
         (*self->connectCallback)(0, self->opaque);
      return EVENT_REMOVE;
   }

   flags = 1;
   res = setsockopt(self->sockfd,            /* socket affected */
                    IPPROTO_TCP,     /* set option at TCP level */
                    TCP_NODELAY,     /* name of option */
                    (char *) &flags,  /* the cast is historical cruft */
                    sizeof(flags));    /* length of option value */
   if (res < 0) {
      perror("setsockopt");
      close(self->sockfd);
      self->sockfd = 0;
      self->connect_event = EVT_sched_add(self->evt_loop,
        CONNECT_RETRY_TIME, &initiate_remote_connection_event, self);
      if (self->connectCallback)
         (*self->connectCallback)(0, self->opaque);
      return EVENT_REMOVE;
   }

   flags = 1;
   res = setsockopt(self->sockfd,            /* socket affected */
                    SOL_SOCKET,     /* set option at TCP level */
                    SO_KEEPALIVE,     /* name of option */
                    (char *) &flags,  /* the cast is historical cruft */
                    sizeof(flags));    /* length of option value */
   if (res < 0) {
      perror("setsockopt SO_KEEPALIVE");
      close(self->sockfd);
      self->sockfd = 0;
      self->connect_event = EVT_sched_add(self->evt_loop,
        CONNECT_RETRY_TIME, &initiate_remote_connection_event, self);
      if (self->connectCallback)
         (*self->connectCallback)(0, self->opaque);
      return EVENT_REMOVE;
   }

#ifndef __APPLE__
   flags = 6;
   res = setsockopt(self->sockfd,            /* socket affected */
                    SOL_TCP,     /* set option at TCP level */
                    TCP_KEEPCNT,     /* name of option */
                    (char *) &flags,  /* the cast is historical cruft */
                    sizeof(flags));    /* length of option value */
   if (res < 0) {
      perror("setsockopt TCP_KEEPCNT");
      close(self->sockfd);
      self->sockfd = 0;
      self->connect_event = EVT_sched_add(self->evt_loop,
        CONNECT_RETRY_TIME, &initiate_remote_connection_event, self);
      if (self->connectCallback)
         (*self->connectCallback)(0, self->opaque);
      return EVENT_REMOVE;
   }

   flags = 5;
   res = setsockopt(self->sockfd,            /* socket affected */
                    SOL_TCP,     /* set option at TCP level */
                    TCP_KEEPIDLE,     /* name of option */
                    (char *) &flags,  /* the cast is historical cruft */
                    sizeof(flags));    /* length of option value */
   if (res < 0) {
      perror("setsockopt TCP_KEEPIDLE");
      close(self->sockfd);
      self->sockfd = 0;
      self->connect_event = EVT_sched_add(self->evt_loop,
        CONNECT_RETRY_TIME, &initiate_remote_connection_event, self);
      if (self->connectCallback)
         (*self->connectCallback)(0, self->opaque);
      return EVENT_REMOVE;
   }

   flags = 5;
   res = setsockopt(self->sockfd,            /* socket affected */
                    SOL_TCP,     /* set option at TCP level */
                    TCP_KEEPINTVL,     /* name of option */
                    (char *) &flags,  /* the cast is historical cruft */
                    sizeof(flags));    /* length of option value */
   if (res < 0) {
      perror("setsockopt TCP_KEEPINTVL");
      close(self->sockfd);
      self->sockfd = 0;
      self->connect_event = EVT_sched_add(self->evt_loop,
        CONNECT_RETRY_TIME, &initiate_remote_connection_event, self);
      if (self->connectCallback)
         (*self->connectCallback)(0, self->opaque);
      return EVENT_REMOVE;
   }
#endif

   res = connect(self->sockfd, (struct sockaddr *)&self->server_addr,
         sizeof(self->server_addr));
   if (res < 0 && errno != EINPROGRESS) {
      perror("connect");
      close(self->sockfd);
      self->sockfd = 0;
      self->connect_event = EVT_sched_add(self->evt_loop,
        CONNECT_RETRY_TIME, &initiate_remote_connection_event, self);
      if (self->connectCallback)
         (*self->connectCallback)(0, self->opaque);
      return EVENT_REMOVE;
   }

   if (res == 0) {
      EVT_fd_add(self->evt_loop, self->sockfd, EVENT_FD_READ,
         &tcpReadEvent, self);

      self->read_reg = 1;

      if (self->connectCallback) {
         EVT_sched_add(self->evt_loop,
               EVT_ms2tv(0), &sock_notify_connect, self);
      }
   }
   else {
      EVT_fd_add(self->evt_loop, self->sockfd, EVENT_FD_WRITE,
         &sock_connect_callback, self);

      self->connect_reg = 1;
   }

   return EVENT_REMOVE;
}

int tcpSerialInit(struct serialInterface **si,
                  struct EventState *evt_loop,
                  serialReadCB readCallback,
                  serialConnectCB connectCallback,
                  const char *devFile,
                  int baudRate,
                  const char *eolMarker,
                  void *opaque)
{
   struct hostent *hp;

   if (!devFile && 0 != strncasecmp("tcp://", devFile, 6))
      return 0;

   // Allocate memory for serial struct
   *si = (struct serialInterface *) malloc(
               sizeof(struct tcpSerialInterfacePriv));
   if (*si == NULL) {
      DBG_print(DBG_LEVEL_WARN, "Insufficient memory\n");
      return -1;
   }
   memset(*si, 0, sizeof(struct tcpSerialInterfacePriv));
   struct tcpSerialInterfacePriv *self = PRIV(*si);

   // Copy the end-of-line marker
   if (eolMarker)
      PRIV(*si)->eolMarker = strdup(eolMarker);
   else
      PRIV(*si)->eolMarker = NULL;

   (*si)->write = tcpSerialWrite;
   (*si)->cleanup = tcpSerialCleanup;
   PRIV(*si)->readCB = readCallback;
   PRIV(*si)->evt_loop = evt_loop;
   PRIV(*si)->opaque = opaque;
   PRIV(*si)->readBytes = 0;
   PRIV(*si)->server_name = strdup(&devFile[6]);
   PRIV(*si)->connectCallback = connectCallback;

   char *split = strchr(self->server_name, ':');
   if (split) {
      PRIV(*si)->server_addr.sin_port = htons(atol(split + 1));
      *split = 0;
   }

   if ((hp = gethostbyname(self->server_name)) != NULL)
      self->server_addr.sin_addr = *(struct in_addr*)(hp->h_addr);

   self->server_addr.sin_family = AF_INET;


   self->evt_loop = evt_loop;
   PRIV(self)->connect_event = EVT_sched_add(self->evt_loop,
                     EVT_ms2tv(1), &initiate_remote_connection_event, self);

   return 0;
}
