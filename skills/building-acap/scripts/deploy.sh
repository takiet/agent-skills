#!/bin/bash

# Usage
usage() {
  echo "Usage: $0 <eap file>"
  exit 1
}

if [ ! -e ./.env ]; then
  echo ".env missing"
  exit 1
fi

if [ "$#" -ne 1 ]; then
  usage
fi

. ./.env

PKG=$1

curl --anyauth -u ${WEB_USER}:${WEB_PASS} \
  -F packfile=@${PKG} \
  "http://${DEVICE}/axis-cgi/applications/upload.cgi"
