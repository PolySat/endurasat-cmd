#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "serial.h"
#include <stdlib.h>

#define FEND 0xC0
#define FESC 0xDB
#define TFEND 0xDC
#define TFESC 0xDD

struct params {
   unsigned char *cmd;
   int cmdLen;
   struct serialInterface *si;
};

uint16_t crc16(char* pData, int length)
{
   uint8_t i;
   uint16_t wCrc = 0xffff;
   while (length--) {
      wCrc ^= *(unsigned char *)pData++ << 8;
      for (i=0; i < 8; i++)
         wCrc = wCrc & 0x8000 ? (wCrc << 1) ^ 0x1021 : wCrc << 1;
   }
   return wCrc & 0xffff;
}

static int exit_cb(void *arg)
{
   EVTHandler *evt = (EVTHandler*)arg;

   if (arg)
      EVT_exit_loop(evt);

   return EVENT_REMOVE;
}

void serial_read_cb(void *buffer, int len, void *arg)
{
   int i;
   unsigned char *b = (unsigned char*)buffer;

   printf("Recvd: ");
   for (i = 0; i < len; i++)
       printf("%02X ", b[i]);
   printf("\n");
}

void serial_connect_cb(int status, void *arg)
{
   struct params *p = (struct params*)arg;
   if (p && status) {
       if (p->si->write) {
          p->si->write(p->si, p->cmd, p->cmdLen);
          printf("Written!\n");
       }
   }
}

static void send_command(char *url, unsigned char *cmd, int len)
{
   EVTHandler *evt;
   struct serialInterface *si = NULL;
   struct params p;

   evt = EVT_create_handler();
   if (evt) {
       serialInit(&si, evt, &serial_read_cb, &serial_connect_cb,
               url, 9600, NULL, &p);
       p.si = si;
       p.cmd = cmd;
       p.cmdLen = len;

       EVT_sched_add(evt, EVT_ms2tv(5000), &exit_cb, evt);

       EVT_start_loop(evt);

       if (si && si->cleanup)
          si->cleanup(si);
       si = NULL;
       EVT_free_handler(evt);
   }
}

int main(int argc, char **argv)
{
   unsigned char cmd[1024];
   unsigned char kiss[1024];
   int cmdLen = 1, kissLen = 0;
   int ind;
   uint16_t crc;

   if (argc < 3) {
      printf("Usage: %s <kiss path> <cmd byte> "
             "[<cmd byte> ...]\n", argv[0]);
      return 0;
   }

   for (ind = 2; ind < argc && cmdLen < sizeof(cmd); ind++)
      cmd[cmdLen++] = strtol(argv[ind], NULL, 0);

   cmd[0] = cmdLen - 1;
   crc = crc16((char*)cmd, cmdLen);
   cmd[cmdLen++] = (crc >> 8) & 0xFF;
   cmd[cmdLen++] = crc & 0xFF;

   kiss[kissLen++] = FEND;
   kiss[kissLen++] = 0;
   for (ind = 0; ind < cmdLen; ind++) {
      if (cmd[ind] == FESC) {
         kiss[kissLen++] = FESC;
         kiss[kissLen++] = TFESC;
      }
      else if (cmd[ind] == FEND) {
         kiss[kissLen++] = FESC;
         kiss[kissLen++] = TFEND;
      }
      else
         kiss[kissLen++] = cmd[ind];
   }
   kiss[kissLen++] = FEND;

   for (ind = 0; ind < kissLen; ind++)
      printf("%02X ", kiss[ind]);
   printf("\n");

   send_command(argv[1], kiss, kissLen);

   return 0;
}
