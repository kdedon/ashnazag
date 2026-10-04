# elf2coff: wrap the Mac ELF kernel as an A/UX COFF kernel for launch.
#
#   make           build build/elf2coff
#   make clean
#
# elf2coff.c is K&R C, so an old native cc builds it too.

B      = build
CC     = cc
CFLAGS = -std=gnu89 -g -w

all: $(B)/elf2coff

$(B)/elf2coff: elf2coff.c | $(B)
	$(CC) $(CFLAGS) -o $@ elf2coff.c

$(B):
	mkdir -p $(B)

clean:
	rm -rf $(B)

.PHONY: all clean
