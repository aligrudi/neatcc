CC = cc
CFLAGS = -Wall -Os
LDFLAGS =

all: ncc npp
.c.o:
	$(CC) -c $(CFLAGS) $<
ncc: ncc.o tok.o gen.o out.o cpp.o tab.o
	$(CC) $(LDFLAGS) -o $@ $^
npp: npp.o cpp.o tab.o
	$(CC) $(LDFLAGS) -o $@ $^

clean:
	rm -f cc *.o
