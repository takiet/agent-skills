#!/bin/bash

usage() {
  echo "Usage: $0 <appName> <start|stop|restart|remove>"
  echo
  exit 1
}

if [ ! -e ./.env ]; then
  echo ".env missing"
  exit 1
fi

if [ "$#" -ne 2 ]; then
  usage
fi

. ./.env

APP_NAME=$1
ACTION=$2

curl --no-progress-meter --anyauth -u ${WEB_USER}:${WEB_PASS} \
  "http://${DEVICE}/axis-cgi/applications/control.cgi?action=${ACTION}&package=${APP_NAME}"
