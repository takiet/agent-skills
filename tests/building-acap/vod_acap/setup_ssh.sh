#!/bin/bash

. ./.env

curl --request PATCH \
  --anyauth \
  --user "${WEB_USER}:${WEB_PASS}" \
  --header "Content-Type: application/json" \
  "http://${DEVICE}/config/rest/ssh/v2/users/${SSHUSER}" \
  --data '{
    "data": {
      "password": "${SSHPASS}",
      "comment": "For Development"
    }
  }'
