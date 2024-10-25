SHELL=/bin/sh
CFLAGS?=-O3
LDLIBS?=-lm
INSTALL?=install
PREFIX?=/usr

include platform.mk

OBJS=\
 codepage.o\
 cpu.o\
 dbg.o\
 dis.o\
 dosnames.o\
 dos.o\
 keyb.o\
 loader.o\
 main.o\
 timer.o\
 utils.o\
 video.o\

.PHONY: all
all: emu2

emu2: $(OBJS:%=obj/%)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

obj/%.o: src/%.c obj
	$(CC) $(CFLAGS) -c -o $@ $<
obj:
	mkdir -p obj

.PHONY: clean distclean
clean distclean:
	rm -f .test.c .test.out $(OBJS:%=obj/%) emu2
	test -d obj && rmdir obj || true

.PHONY: install
install: emu2
	$(INSTALL) -d $(DESTDIR)${PREFIX}/bin
	$(INSTALL) -s emu2 $(DESTDIR)${PREFIX}/bin

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)${PREFIX}/bin/emu2

# Generated with gcc -MM src/*.c
obj/codepage.o: src/codepage.c src/codepage.h src/dbg.h src/os.h src/env.h
obj/cpu.o: src/cpu.c src/cpu.h src/dbg.h src/os.h src/dis.h src/emu.h
obj/dbg.o: src/dbg.c src/dbg.h src/os.h src/env.h src/version.h
obj/dis.o: src/dis.c src/dis.h src/emu.h
obj/dos.o: src/dos.c src/dos.h src/codepage.h src/dbg.h src/os.h \
 src/dosnames.h src/emu.h src/env.h src/keyb.h src/loader.h \
 src/timer.h src/utils.h src/video.h
obj/dosnames.o: src/dosnames.c src/dosnames.h src/dbg.h src/os.h src/emu.h \
 src/env.h
obj/keyb.o: src/keyb.c src/keyb.h src/codepage.h src/dbg.h src/os.h src/emu.h
obj/loader.o: src/loader.c src/loader.h src/dbg.h src/os.h src/emu.h
obj/main.o: src/main.c src/dbg.h src/os.h src/dos.h src/dosnames.h src/emu.h \
 src/keyb.h src/timer.h src/video.h
obj/timer.o: src/timer.c src/timer.h src/dbg.h src/os.h src/emu.h
obj/utils.o: src/utils.c src/utils.h src/dbg.h src/os.h
obj/video.o: src/video.c src/video.h src/codepage.h src/dbg.h src/os.h \
 src/emu.h src/env.h src/keyb.h
