CC = gcc
CFLAGS = -O2 -Wall -I ..

all: adder head-adder

adder: adder.c
	$(CC) $(CFLAGS) -o adder adder.c
head-adder: head-adder.c
	$(CC) $(CFLAGS) -o head-adder head-adder.c
clean:
	rm -f adder head-adder *~