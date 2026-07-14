#!/bin/bash

. ./.env

FILE=$1
ARG=$2

sshpass -e scp ${FILE} ${SSHUSER}@{$DEVICE_IP}:/tmp

sshpass -e ssh ${SSHUSER}@${DEVICE_IP} "${FILE} ${ARG}" > output

usage() {
  echo "Usage $0 <executable> "<arguments>" "
  echo 
  echo "<executable> : an executable for testing"
  echo "<arguments> : pass all required arguments as a single string enclosed in double quotes. Ex: "arg1 arg2 arg3" "
  exit 1
}
