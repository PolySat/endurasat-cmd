#include Make.rules.arm

override CFLAGS+=-Wall -std=gnu99 -g -I/usr/local/include

PROGRAM=endurasat-cmd
SRC=serial.c tcp_serial.c endura-cmd.c
ARCH=i386

LIBS=-rdynamic -lproc -ldl -lm

OBJ=$(SRC:%.c=objs-$(ARCH)/%.o) $(CPP_SRC:%.cpp=objs-$(ARCH)/%.o)

all: $(PROGRAM)

$(PROGRAM): objs-$(ARCH) $(OBJ) $(COM_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $(OBJ) $(COM_OBJ) $(LIBS)

objs-$(ARCH):
	mkdir -p objs-$(ARCH)

objs-$(ARCH)/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf *.o *.gch $(PROGRAM) objs-* sat_ops

.PHONY: clean objs-$(ARCH)
