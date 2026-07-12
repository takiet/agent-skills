# Set up

## Contents

- Makefile under the project root
- Dockerfile

## Makefile for outputting eap files

```makefile
ARCH        ?= aarch64
VERSION     ?= 12.11.0
OUTPUT_DIR  := output

build:
        DOCKER_BUILDKIT=1 docker build \
          --build-arg ARCH=$(ARCH) \
          --build-arg VERSION=$(VERSION) \
          --target export \
          --output type=local,dest=$(OUTPUT_DIR) \
          .
clean:
        rm -rf $(OUTPUT_DIR)
```

## Baseline Dockerfile (Multi stage)

```dockerfile
ARG ARCH=aarch64
ARG VERSION=12.11.0
ARG REPO=axisecp

FROM ${REPO}/acap-native-sdk:${VERSION}-${ARCH} AS builder
COPY app /opt/app/
WORKDIR /opt/app
RUN . /opt/axis/acapsdk/environment-setup* && acap-build ./

# Extract only `.eap` file with no unnecessary layer left
FROM scratch AS export
COPY --from=builder /opt/app/*.eap /
```
