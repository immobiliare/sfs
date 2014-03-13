#!/usr/bin/env bash
# Delete backup batches older than 1440 minutes

# FIXME: make this configurable from command line
MINUTES=1440

BKPBATCHDIR=$1

if [[ 'a'$BKPBATCHDIR == 'a' ]]; then
	echo "[$(tput setaf 1)ERROR$(tput sgr0)] Missing batches directory"
	echo
	echo "usage: $0 PATH_BATCHES_FILES"
	echo
	exit 1;
fi

if [ ! -d "$BKPBATCHDIR" ]; then
	echo
	echo "[$(tput setaf 1)ERROR$(tput sgr0)] '$BKPBATCHDIR' is not a valid batches directory"
	echo
	exit 3

fi

exec find "$BKPBATCHDIR" -type f -name "*.batch" -mmin +$MINUTES -delete -print
