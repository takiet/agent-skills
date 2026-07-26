ARCH        ?= aarch64
VERSION     ?= 12.11.0
OUTPUT_DIR  := build

.PHONY: build clean

build:
	DOCKER_BUILDKIT=1 docker build \
	  --build-arg ARCH=$(ARCH) \
	  --build-arg VERSION=$(VERSION) \
	  --target export \
	  --output type=local,dest=$(OUTPUT_DIR) \
	  .

clean:
	rm -rf $(OUTPUT_DIR)
