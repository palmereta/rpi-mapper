PKGS = gbm egl glesv2 libdrm mpv libcjson
CFLAGS ?= -O2 -pipe -mcpu=cortex-a53
CFLAGS += -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lm

mapper: mapper.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f mapper
