CC = cc
CFLAGS = -Wall -Os -g
LDFLAGS = -g

all: ncc
.c.o:
	$(CC) -c $(CFLAGS) $<
ncc: ncc.o tok.o gen.o out.o cpp.o tab.o
	$(CC) $(LDFLAGS) -o $@ $^

clean:
	rm -f cc *.o
