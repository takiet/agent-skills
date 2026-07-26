ARG ARCH=aarch64
ARG VERSION=12.11.0
ARG REPO=axisecp

FROM ${REPO}/acap-native-sdk:${VERSION}-${ARCH} AS builder

ARG BUILD_DIR=/opt/build
ARG SKIA_VERSION=chrome/m137

#-------------------------------------------------------------------------------
# Build environment for Skia (used by the overlay) plus the font it draws with.
#
# fonts-dejavu-core is installed here rather than in a later layer on purpose:
# adding a package to this RUN would invalidate the Skia build below, which
# takes tens of minutes.
#-------------------------------------------------------------------------------
RUN DEBIAN_FRONTEND=noninteractive \
    apt-get update && apt-get install -y --no-install-recommends \
    generate-ninja ninja-build git fonts-dejavu-core && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

#-------------------------------------------------------------------------------
# Download and build the Skia graphics library
#
# These layers only depend on SKIA_VERSION, so they are cached and reused across
# rebuilds as long as the Skia version is unchanged. Editing the application
# source below does not trigger a Skia rebuild -- keep this before `COPY app`.
#
# The GN configuration follows the official axoverlay2-skia example, except for
# the font options: the example disables fonts entirely and only draws shapes,
# while this application also draws labels. skia_use_freetype=true with
# skia_use_system_freetype2=false builds Skia's bundled FreeType, so no
# sysroot pkg-config wiring is needed. Fontconfig stays off so the result does
# not depend on which fonts the device happens to have.
#
#   cc, cxx:            Force GN to use the SDK-provided cross compilers.
#   extra_cflags:       Disable NEON optimisations in Skia's bundled libpng.
#   target_cpu:         Prevent GN from guessing the target CPU and injecting
#                       the wrong -march option.
#   is_component_build: Produce a static Skia library.
#
# If git-sync-deps fails on an unreliable network (it starts one thread per
# dependency, which can trigger HTTP 429), serialize it by prepending:
#   sed -i 's/^\( *\)thread.start()/\1thread.start()\n\1thread.join()/' tools/git-sync-deps
#-------------------------------------------------------------------------------
WORKDIR ${BUILD_DIR}/skia

RUN git clone https://skia.googlesource.com/skia.git . \
        --branch ${SKIA_VERSION} --depth 1 && \
    python3 tools/git-sync-deps && \
    . /opt/axis/acapsdk/environment-setup* && \
    gn gen --root=. --args=" \
        cc=\"$CC\" \
        cxx=\"$CXX\" \
        extra_cflags=[\"-DPNG_ARM_NEON_OPT=0\"] \
        target_cpu=\"dont_worry_about_it\" \
        is_official_build=true \
        is_component_build=false \
        is_debug=false \
        skia_enable_ganesh=true \
        skia_enable_pdf=false \
        skia_enable_skshaper=false \
        skia_enable_skunicode=false \
        skia_enable_svg=false \
        skia_enable_tools=false \
        skia_pdf_subset_harfbuzz=false \
        skia_use_angle=false \
        skia_use_egl=true \
        skia_use_expat=false \
        skia_use_fontconfig=false \
        skia_use_freetype=true \
        skia_use_system_freetype2=false \
        skia_use_gl=true \
        skia_use_harfbuzz=false \
        skia_use_icu=false \
        skia_use_libjpeg_turbo_decode=false \
        skia_use_libjpeg_turbo_encode=false \
        skia_use_lua=false \
        skia_use_libpng_decode=false \
        skia_use_libpng_encode=true \
        skia_use_libwebp_decode=false \
        skia_use_libwebp_encode=false \
        skia_use_system_libpng=false \
        skia_use_system_zlib=false \
        skia_use_wuffs=false \
        skia_use_x11=false \
        skia_use_xps=false \
        skia_use_zlib=false \
        " build_skia && \
    ninja -C build_skia

#-------------------------------------------------------------------------------
# Build the ACAP application
#-------------------------------------------------------------------------------
WORKDIR /opt/app
COPY app /opt/app/

# Skia is statically linked, so it is only needed at build time and is kept
# outside the application directory to avoid bloating the package.
ENV SKIA_DIR=${BUILD_DIR}/skia
ENV SKIA_LIB=${BUILD_DIR}/skia/build_skia/libskia.a

# The overlay draws labels with this font. Skia is built without fontconfig, so
# the font has to travel with the application.
RUN cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf /opt/app/

RUN . /opt/axis/acapsdk/environment-setup* && acap-build ./ \
    -a test_capture \
    -a test_detect \
    -a test_overlay \
    -a models/yolov5s.tflite \
    -a models/labels.txt \
    -a sample/image.rgb \
    -a DejaVuSans.ttf

# Extract only `.eap` file with no unnecessary layer left
FROM scratch AS export
COPY --from=builder /opt/app/* /
