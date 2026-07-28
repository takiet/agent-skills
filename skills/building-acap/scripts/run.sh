#!/bin/bash

# Run a test binary that has been packaged into the app's .eap and installed on
# the device. Runs it over SSH from the installed package directory (as the SSH
# login user) and saves its output to the `output` file.
#
# Build & install first (make build && deploy.sh); this script only runs.

usage() {
  echo "Usage: $0 <appName> <test-binary> [-a \"<arguments>\"]"
  echo
  echo "<appName>      : the installed ACAP app that packages the test binary"
  echo "<test-binary>  : name of the test executable inside the installed package"
  echo "-a <arguments> : optional; pass all arguments as a single quoted string, e.g. \"arg1 arg2\""
  exit 1
}

if [ ! -e ./.env ]; then
  echo ".env missing"
  exit 1
fi

ARG=""
POSITIONAL=()
while (( $# > 0 )); do
  case $1 in
    -a)
      if [[ -z "$2" ]] || [[ "$2" =~ ^- ]]; then
        usage
      fi
      ARG=$2
      shift 2
      ;;
    -*)
      usage
      ;;
    *)
      POSITIONAL+=("$1")
      shift
      ;;
  esac
done

if [ "${#POSITIONAL[@]}" -ne 2 ]; then
  usage
fi

APP_NAME=${POSITIONAL[0]}
BIN=${POSITIONAL[1]}

. ./.env
export SSHPASS=${SSH_PASS}

# Host key checking is disabled (see README): the device's host key changes on
# reflash/reinstall, so we neither store nor verify it. This drops MITM
# protection — only use on a trusted network.
sshpass -e ssh -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no \
  "${SSH_USER}@${DEVICE}" "/usr/local/packages/${APP_NAME}/${BIN} ${ARG}" > output
