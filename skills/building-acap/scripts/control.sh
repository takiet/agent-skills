#!/bin/bash

. ./.env

ACTION=$1
APP_NAME=$2

curl --anyauth -u ${WEB_USER}:${WEB_PASSWORD} \
  "http://${DEVICE_IP}/axis-cgi/applications/control.cgi?action=${ACTION}&package=${APP_NAME}"

usage() {
  echo "Usage: $0 <start|stop|restart|remove> <appName>"
  echo 
  exit 1
}
