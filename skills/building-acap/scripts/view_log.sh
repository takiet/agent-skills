#!/bin/bash

usage() {
  echo "Usage: $0 <binary-name>"
  echo
  echo "<binary-name>: appName or test binary"
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

APP_NAME=$1

curl --no-progress-meter --anyauth -u ${WEB_USER}:${WEB_PASS} \
  "http://${DEVICE}/axis-cgi/admin/systemlog.cgi?appname=${APP_NAME}"
