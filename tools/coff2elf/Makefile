# coff2elf: COFF (i386 0x14C, m68k 0x150) -> ELF32.
#
#   make                 build coff2elf and the i386 test fixture generator
#   make test            run test.sh (i386 fixtures need gcc -m32 and ld;
#                        m68k checks need AUXROOT=<extracted A/UX root>)
#
# coff2elf.c and mkfix.c are K&R C, so an old native cc builds them too.

B      = build
CC     = cc
CFLAGS = -std=gnu89 -g -w
M32    = gcc -m32

all: $(B)/coff2elf $(B)/mkfix

$(B)/coff2elf: coff2elf.c | $(B)
	$(CC) $(CFLAGS) -o $@ coff2elf.c
$(B)/mkfix: mkfix.c | $(B)
	$(CC) $(CFLAGS) -o $@ mkfix.c

# i386 Linux startup and syscall glue the i386 fixtures link against.
libcoh: $(B)/crt0.o $(B)/libcoh.a
$(B)/%.o: libcoh/%.s | $(B)
	$(M32) -c $< -o $@
$(B)/%.o: libcoh/%.c | $(B)
	$(M32) -c -std=gnu89 -w $< -o $@
$(B)/libcoh.a: $(B)/sys.o $(B)/mem.o $(B)/exit.o
	ar rc $@ $(B)/sys.o $(B)/mem.o $(B)/exit.o

$(B):
	mkdir -p $(B)

test: all
	-$(MAKE) libcoh
	B=$(B) sh test.sh

clean:
	rm -rf $(B)

.PHONY: all libcoh test clean
