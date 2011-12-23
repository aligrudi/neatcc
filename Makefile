# for arm build
#ARCH = -DNEATCC_ARM
#GEN = arm.o

# for x86_64 build
#ARCH = -DNEATCC_X86
#GEN = x86.o

# for x86_64 build
ARCH = -DNEATCC_X64
GEN = x64.o

CC = cc
CFLAGS = -Wall -O2 $(ARCH)
LDFLAGS =

all: ncc npp
.c.o:
	$(CC) -c $(CFLAGS) $<
ncc: ncc.o tok.o $(GEN) out.o cpp.o tab.o gen.o
	$(CC) -o $@ $^ $(LDFLAGS)
npp: npp.o cpp.o tab.o
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	rm -f *.o ncc cpp
