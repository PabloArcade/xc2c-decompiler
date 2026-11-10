# xc2cdump -- build with any c99 compiler and the C library only.
#   make            Linux (or any POSIX) binary: xc2cdump
#   make windows    Windows x86-64 binary with mingw-w64: xc2cdump.exe
# The Windows binary is linked with a 32 MB stack reserve: the largest designs
# recurse deeper than the 1 MB a Windows thread gets by default. It is stripped
# and carries no link time stamp, so a rebuild gives the same bytes.
# The device tables are compiled in (zia_tables_gen.h, globalnets_gen.h,
# pintable.c): decoding needs no other program and no data file.
CC      = cc
CFLAGS  = -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -O2
TARGET  = xc2cdump
SRCS    = main.c compat.c jedgeom.c model.c modelgen.c generator.c emit.c \
          ucf.c ucfemit.c decodeemit.c pintable.c globalnets.c util.c json.c
OBJS    = $(SRCS:.c=.o)
HDRS    = jed2vhdl.h jedgeom.h json.h compat.h zia_tables_gen.h globalnets_gen.h

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

WINCC     = x86_64-w64-mingw32-gcc
WINSTACK  = 33554432
WINTARGET = xc2cdump.exe

windows:
	$(WINCC) $(CFLAGS) -Wl,--strip-all -Wl,--no-insert-timestamp -Wl,--stack,$(WINSTACK) -o $(WINTARGET) $(SRCS)

clean:
	rm -f $(OBJS) $(TARGET) $(WINTARGET)

.PHONY: clean windows
