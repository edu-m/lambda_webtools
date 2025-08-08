# Makefile
# Targets:
#   make         -> dev build with ASan/UBSan/LSan, debug info, hardening
#   make release -> optimized release with FORTIFY & stack protector

CC ?= cc

COMMON_WARN := -Wall -Wextra -Wpedantic -Wshadow -Wcast-align -Wpointer-arith \
               -Wstrict-aliasing -Wwrite-strings -Wmissing-prototypes -Wswitch-enum \
               -Wformat=2 -Wvla

# Dev/debug: sanitizers + debug symbols + frame pointer
SAN_FLAGS := -O1 -g -fno-omit-frame-pointer \
             -fsanitize=address,undefined,leak

# Release: optimize but keep some hardening
REL_FLAGS := -O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fno-omit-frame-pointer

all: mdparse mdserve

mdparse: mdparse.c
	$(CC) $(COMMON_WARN) $(SAN_FLAGS) -o $@ $<

mdserve: mdserve.c
	$(CC) $(COMMON_WARN) $(SAN_FLAGS) -o $@ $<

release: mdparse_release mdserve_release

mdparse_release: mdparse.c
	$(CC) $(COMMON_WARN) $(REL_FLAGS) -o mdparse $<

mdserve_release: mdserve.c
	$(CC) $(COMMON_WARN) $(REL_FLAGS) -o mdserve $<

clean:
	rm -f mdparse mdserve
