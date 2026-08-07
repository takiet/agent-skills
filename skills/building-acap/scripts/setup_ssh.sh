#!/bin/bash

# Set the password of the app's dedicated SSH user on the device.
#
# Installing an ACAP app creates an SSH user named acap-<appName>, but with no
# password — until one is set, run.sh cannot log in. Run this once after the
# first install.

usage() {
  echo "Usage: $0 <appName>"
  echo
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

# Refuse to set an empty password on the device: it would leave run.sh failing
# to log in with no indication that .env was never filled in.
if [ -z "${SSH_PASS}" ]; then
  echo "SSH_PASS is empty in .env — choose a password there first"
  exit 1
fi

# Derived after sourcing, so a stale .env that still defines APP_NAME/SSH_USER
# cannot override the name given on the command line.
APP_NAME=$1
SSH_USER=acap-${APP_NAME}

# The payload must be double-quoted so ${SSH_PASS} is expanded. With single
# quotes curl sends the literal text ${SSH_PASS} as the password, which then
# silently disagrees with the value run.sh authenticates with.
curl --no-progress-meter --request PATCH \
  --anyauth \
  --user "${WEB_USER}:${WEB_PASS}" \
  --header "Content-Type: application/json" \
  "http://${DEVICE}/config/rest/ssh/v2/users/${SSH_USER}" \
  --data "{
    \"data\": {
      \"password\": \"${SSH_PASS}\",
      \"comment\": \"For Development\"
    }
  }"
