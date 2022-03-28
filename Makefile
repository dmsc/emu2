CC=cc
CFLAGS=-O3 -flto -Wall -g -Werror=implicit-function-declaration -Werror=int-conversion -DEMS_SUPPORT
LDLIBS=-lm
INSTALL=install
PREFIX=/usr

OBJS=\
 cpu.o\
 loader.o\
 main.o\
 codepage.o\
 dosnames.o\
 dis.o\
 dos.o\
 keyb.o\
 dbg.o\
 timer.o\
 utils.o\
 video.o\
 ems.o\


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

install: emu2
	$(INSTALL) -d $(DESTDIR)${PREFIX}/bin
	$(INSTALL) -s emu2 $(DESTDIR)${PREFIX}/bin

uninstall:
	rm -f $(DESTDIR)${PREFIX}/bin/emu2

# Generated with gcc -MM src/*.c
obj/codepage.o: src/codepage.c src/codepage.h src/dbg.h src/env.h
obj/cpu.o: src/cpu.c src/cpu.h src/dbg.h src/dis.h src/emu.h src/ems.h
obj/dbg.o: src/dbg.c src/dbg.h src/env.h
obj/dis.o: src/dis.c src/dis.h src/emu.h src/ems.h
obj/dos.o: src/dos.c src/codepage.h src/dos.h src/dbg.h src/dosnames.h \
 src/emu.h src/env.h src/keyb.h src/loader.h src/timer.h src/utils.h \
 src/video.h src/ems.h
obj/dosnames.o: src/dosnames.c src/dbg.h src/dosnames.h src/emu.h src/env.h \
 src/ems.h
obj/keyb.o: src/keyb.c src/keyb.h src/dbg.h src/emu.h src/codepage.h
obj/loader.o: src/loader.c src/loader.h src/dbg.h src/emu.h
obj/main.o: src/main.c src/dbg.h src/dos.h src/dosnames.h src/emu.h \
 src/keyb.h src/timer.h src/video.h src/ems.h
obj/timer.o: src/timer.c src/dbg.h src/timer.h src/emu.h
obj/utils.o: src/utils.c src/utils.h src/dbg.h
obj/video.o: src/video.c src/video.h src/dbg.h src/emu.h src/codepage.h
obj/ems.o: src/ems.c src/emu.h src/ems.h
