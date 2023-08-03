CFLAGS = -g -Wall -fPIC
LDFLAGS =

all: libmallochook.so malloctest

libmallochook.so: mallochook.o
	$(CC) -shared -o $@ $^ $(LDFLAGS) -ldl

mallochook.o: mallochook.c
	$(CC) -c $(CFLAGS) mallochook.c

testmallochook: testmallochook.c
	$(CC) -o $@ $(CFLAGS) -ldl testmallochook.c

test: libmallochook.so testmallochook
	LD_PRELOAD=./libmallochook.so ./testmallochook

clean:
	rm -f libmallochook.so mallochook.o testmallochook *.c~ *.h~ Makefile~

