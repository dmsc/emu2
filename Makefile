CC=gcc
CFLAGS=-O3 -flto -Wall -g -Werror=implicit-function-declaration -Werror=int-conversion
LDLIBS=-lm

OBJS=cpu.o loader.o main.o dosnames.o dis.o dos.o keyb.o dbg.o timer.o video.o

all: obj emu2

emu2: $(OBJS:%=obj/%)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

obj/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<
obj:
	mkdir -p obj

clean:
	rm -f $(OBJS:%=obj/%)
	rm -f emu2
	rmdir obj

# Generated with gcc -MM src/*.c
obj/cpu.o: src/cpu.c src/cpu.h src/dbg.h src/dis.h src/emu.h
obj/dbg.o: src/dbg.c src/dbg.h src/env.h
obj/dis.o: src/dis.c src/dis.h src/emu.h
obj/dos.o: src/dos.c src/dos.h src/dbg.h src/dosnames.h src/emu.h src/env.h \
 src/keyb.h src/loader.h src/timer.h src/video.h
obj/dosnames.o: src/dosnames.c src/dbg.h src/dosnames.h src/emu.h src/env.h
obj/keyb.o: src/keyb.c src/keyb.h src/dbg.h src/emu.h
obj/loader.o: src/loader.c src/loader.h src/dbg.h src/emu.h
obj/main.o: src/main.c src/dbg.h src/dos.h src/dosnames.h src/emu.h \
 src/keyb.h src/timer.h src/video.h
obj/timer.o: src/timer.c src/dbg.h src/timer.h src/emu.h
obj/video.o: src/video.c src/video.h src/dbg.h src/emu.h
