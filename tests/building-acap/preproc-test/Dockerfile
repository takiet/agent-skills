ARG ARCH=aarch64
ARG VERSION=12.11.0
ARG REPO=axisecp

FROM ${REPO}/acap-native-sdk:${VERSION}-${ARCH} AS builder
COPY app /opt/app/
WORKDIR /opt/app
RUN . /opt/axis/acapsdk/environment-setup* && acap-build ./ -a test_preproc

# Extract only `.eap` file with no unnecessary layer left
FROM scratch AS export
COPY --from=builder /opt/app/* /
