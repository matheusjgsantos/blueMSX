
all: bluemsxnano

arch_linux.o: arch_linux.c arch.h
	gcc -O2 -c -o arch_linux.o arch_linux.c

z80.o: z80.c z80.h arch.h
	gcc -O2 -c -o z80.o z80.c

bluemsxnano.o: bluemsxnano.c arch.h z80.h
	gcc -O2 -c -o bluemsxnano.o bluemsxnano.c

bluemsxnano: bluemsxnano.o z80.o arch_linux.o
	gcc -o bluemsxnano bluemsxnano.o z80.o arch_linux.o

clean:
	rm bluemsxnano bluemsxnano.o z80.o arch_linux.o
