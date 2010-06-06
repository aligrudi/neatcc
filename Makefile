CC = cc
CFLAGS = -Wall -O2 -g
LDFLAGS = -g

all: ncc
.c.o:
	$(CC) -c $(CFLAGS) $<
ncc: ncc.o tok.o gen.o out.o cpp.o
	$(CC) $(LDFLAGS) -o $@ $^

clean:
	rm -f cc *.o
