CC := gcc
CFLAGS := -Os -g -std=c11 -fPIC $(CFLAGS)
LDFLAGS := -Wl,--as-needed $(LDFLAGS)
LIBS := -lncurses -ltinfo -ldl

include src/Makefile

all: memstrack

.PHONY: clean install uninstall dracut-module-install dracut-module-uninstall test
clean:
	rm -f $(DEP_FILES:.o=.d)
	rm -f $(OBJ_FILES)
	rm -f $(OUT_FILES)
	rm -f memstrack
