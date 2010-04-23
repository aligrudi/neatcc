CC = dietcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -g

all: cc
.c.o:
	$(CC) -c $(CFLAGS) $<
cc: cc.o tok.o out.o
	$(CC) $(LDFLAGS) -o $@ $^

clean:
	rm -f cc *.o
