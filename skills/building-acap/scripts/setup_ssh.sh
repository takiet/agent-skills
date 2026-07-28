#!/bin/bash

# Set the password of the app's dedicated SSH user on the device.
#
# Installing an ACAP app creates an SSH user named after the app, but with no
# password — until one is set, run.sh cannot log in. Run this once after the
# first install.

if [ ! -e ./.env ]; then
  echo ".env missing"
  exit 1
fi

. ./.env

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
