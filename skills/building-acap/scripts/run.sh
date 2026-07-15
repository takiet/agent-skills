#!/bin/bash

usage() {
  echo "Usage $0 <executable> -a "<arguments>" "
  echo 
  echo "<executable> : an executable for testing"
  echo "-a <arguments> : pass all required arguments as a single string enclosed in double quotes. Ex: "arg1 arg2 arg3" "
  exit 1
}

if [ ! -e ./.env ]; then
  echo ".env missing"
  exit 1
fi

ARG=""
while (( $# > 0 ))
  do
    case $1 in
      -a)
        if [[ -z "$2" ]] || [[ "$2" =~ ^-+ ]]; then
          usage
        else
          ARG=$2
        fi
        ;;
      *)
        FILE=$1
        ;;
      -*)
        usage
        ;;
    esac
    shift
  done

. ./.env

sshpass -e scp ${FILE} ${SSHUSER}@{$DEVICE_IP}:/tmp

sshpass -e ssh ${SSHUSER}@${DEVICE_IP} "${FILE} ${ARG}" > output
