PROG = $(shell jq -r '.acapPackageConf.setup.appName' manifest.json)
OBJS = main.cc capture.cc detector.cc overlay.cc
TEST_CAPTURE = test_capture
TEST_CAPTURE_OBJS = test_capture.cc capture.cc
TEST_DETECT = test_detect
TEST_DETECT_OBJS = test_detect.cc detector.cc capture.cc
TEST_OVERLAY = test_overlay
TEST_OVERLAY_OBJS = test_overlay.cc overlay.cc
DEBUG_DIR = debug

PKGS = gio-2.0 gio-unix-2.0 liblarod vdostream axparameter

CXXFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))

CXXFLAGS += -Wall -Wextra -Wformat=2 -Wpointer-arith -Werror

# Skia and the GPU/overlay stack are only needed by the binaries that draw.
# SKIA_DIR and SKIA_LIB are exported by the Dockerfile; Skia is linked
# statically so it never reaches the device.
SKIA_DIR ?= skia
SKIA_LIB ?= build_skia/libskia.a
OVERLAY_PKGS = glib-2.0 glesv2 egl axoverlay2
OVERLAY_CXXFLAGS = $(CXXFLAGS) -std=c++20 -Wno-unused-parameter -I$(SKIA_DIR) \
	$(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(OVERLAY_PKGS))
OVERLAY_LDLIBS = $(LDLIBS) \
	$(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs-only-l $(OVERLAY_PKGS)) \
	$(SKIA_LIB)

all: $(PROG) $(TEST_CAPTURE) $(TEST_DETECT) $(TEST_OVERLAY)

# The app draws overlays, so it needs the same Skia/GPU flags as test_overlay.
$(PROG): $(OBJS)
	install -d $(DEBUG_DIR)
	$(CXX) $^ $(OVERLAY_CXXFLAGS) $(LIBS) $(LDFLAGS) $(OVERLAY_LDLIBS) -o $(DEBUG_DIR)/$@
	cp $(DEBUG_DIR)/$@ .
	$(STRIP) $@

$(TEST_CAPTURE): $(TEST_CAPTURE_OBJS)
	install -d $(DEBUG_DIR)
	$(CXX) $^ $(CXXFLAGS) $(LIBS) $(LDFLAGS) $(LDLIBS) -o $(DEBUG_DIR)/$@
	cp $(DEBUG_DIR)/$@ .
	$(STRIP) $@

$(TEST_DETECT): $(TEST_DETECT_OBJS)
	install -d $(DEBUG_DIR)
	$(CXX) $^ $(CXXFLAGS) $(LIBS) $(LDFLAGS) $(LDLIBS) -o $(DEBUG_DIR)/$@
	cp $(DEBUG_DIR)/$@ .
	$(STRIP) $@

$(TEST_OVERLAY): $(TEST_OVERLAY_OBJS)
	install -d $(DEBUG_DIR)
	$(CXX) $^ $(OVERLAY_CXXFLAGS) $(LIBS) $(LDFLAGS) $(OVERLAY_LDLIBS) -o $(DEBUG_DIR)/$@
	cp $(DEBUG_DIR)/$@ .
	$(STRIP) $@

clean:
	rm -rf $(PROG) $(TEST_CAPTURE) $(TEST_DETECT) $(TEST_OVERLAY) *.o *.eap* *_LICENSE.txt package.conf* param.conf tmp* $(DEBUG_DIR)
