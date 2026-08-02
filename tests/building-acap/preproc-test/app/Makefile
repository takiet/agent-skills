PROG      = $(shell jq -r '.acapPackageConf.setup.appName' manifest.json)
OBJS      = $(PROG).c capture.c preproc.c
TEST_PREPROC_PROG = test_preproc
TEST_PREPROC_OBJS = test_preproc.c capture.c preproc.c
DEBUG_DIR = debug

PKGS  = gio-2.0 gio-unix-2.0 liblarod vdostream

CFLAGS += -Wall -Wextra -Wformat=2 -Wpointer-arith \
          -Wbad-function-cast -Wstrict-prototypes -Wmissing-prototypes -Werror
CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))
LDLIBS += -lm

all: $(PROG) $(TEST_PREPROC_PROG)

$(PROG): $(OBJS)
	install -d $(DEBUG_DIR)
	$(CC) $^ $(CFLAGS) $(LIBS) $(LDFLAGS) $(LDLIBS) -o $(DEBUG_DIR)/$@
	cp $(DEBUG_DIR)/$@ .
	$(STRIP) $@

$(TEST_PREPROC_PROG): $(TEST_PREPROC_OBJS)
	install -d $(DEBUG_DIR)
	$(CC) $^ $(CFLAGS) $(LIBS) $(LDFLAGS) $(LDLIBS) -o $(DEBUG_DIR)/$@
	cp $(DEBUG_DIR)/$@ .
	$(STRIP) $@

clean:
	rm -rf $(PROG) $(TEST_PREPROC_PROG) *.o *.eap* *_LICENSE.txt package.conf* param.conf tmp* $(DEBUG_DIR)
