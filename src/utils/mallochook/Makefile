CFLAGS = -g -Wall -fPIC
LDFLAGS =

all: testmallochook

testmallochook: testmallochook.c
	$(CC) -o $@ $(CFLAGS) -I../../java.base/linux/native/libmallochooks -ldl testmallochook.c

clean:
	rm -f testmallochook

