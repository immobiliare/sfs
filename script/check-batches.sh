#!/usr/bin/env bash

# Check batches created by the SFS fuse process.

set -e

PROG="$0"

function usage() {
	echo "Usage: "
	echo -e "  Nagios syntax: $PROG -m mode -w warn -c crit -b batchdir"
	echo -e "  Human syntax:  $PROG -x -f thisnode -b batchdir"
	echo -e "  JSON syntax:   $PROG -j -f thisnode -b batchdir"
	echo -e "-x\t\tHuman syntax"
	echo -e "-j\t\tJSON syntax"
	echo -e "-m mode\t\tEither count, oldestage or all (default: all)"
	echo -e "-w warn\t\tWarning threshold"
	echo -e "-c crit\t\tCritical threshold"
	echo -e "-b batchdir\tBatches directory"
	echo -e "-f node\t\tFrom node name"
	echo -e "-n node\t\tShow only information relative to a given node"
}

SYNTAX="nagios"
MODE="all"

ARGS=$(getopt -o "xjf:m:n:w:c:s:b:" -n "$0" -- "$@")
eval set -- "$ARGS"

while true; do
	case "$1" in
	-x)
		SYNTAX="human"
		shift
		;;
	-j)
		SYNTAX="json"
		shift
		;;
	-m)
		MODE="$2"
		shift 2
		;;
	-f)
		THISNODE="$2"
		shift 2
		;;
	-n)
		NODE="$2"
		shift 2
		;;
	-w)
		WARN="$2"
		shift 2
		;;
	-c)
		CRIT="$2"
		shift 2
		;;
	-b)
		BATCHDIR="$2"
		shift 2
		;;
	*)
		break
		;;
	esac
done

if [ -z "$BATCHDIR" ]; then
	usage
	exit 3
fi

if [ ! -d "$BATCHDIR" ]; then
	echo "error: '$BATCHDIR' is not a valid batches directory"
	usage
	exit 3
fi

if [ "$MODE" != "all" -a "$MODE" != "count" -a "$MODE" != "oldestage" ]; then
	echo "error: mode must be either 'count' or 'oldestage'"
	usage
	exit 3
fi

if [ "$SYNTAX" = "nagios" ]; then
	if [ "$MODE" = "all" -o -z "$WARN" -o -z "$CRIT" ]; then
		usage
		exit 3
	fi
else
	if [ -z "$THISNODE" ]; then
		usage
		exit 3
	fi
fi

if [ -z "$NODE" ]; then
	NODE="*"
fi

function safels() {
	ls $@ 2>/dev/null || true
}

function max() {
	local m=0
	for v in "$@"; do
		m=$(($v>$m ? $v : $m))
	done
	echo $m
}

# Modes
CURTIME=$(date +%s)
function mode_oldestage() {
	local last=$(safels -v $@|grep batch|head -n1)
	if [ -n "$last" ]; then
		local time=$(stat -c %Y "$@/$last" 2>/dev/null)
		if [[ -z "$time" ]]; then
			echo 0
			return;
		fi
		local delta=$(($CURTIME-$time))
		echo ${delta}
	else
		echo 0
	fi
}

function mode_count() {
	safels -U $@|wc -l
}

function mode_all() {
	local count=$(mode_count $@)
	local oldest=$(mode_oldestage $@)
	if [ "$SYNTAX" = "human" ]; then
		echo "$count/${oldest}s"
	else
		echo "{ \"Count\": $count, \"Seconds\": $oldest }"
	fi
}

FUNC="mode_$MODE"

function printHuman() {
	local node=$1
	PULL=$($FUNC "$BATCHDIR"/pull/$node/)
	PUSH=$($FUNC "$BATCHDIR"/push/$node/)
	echo "$THISNODE->$node: $PUSH | $node->$THISNODE: $PULL"
}

function printJson() {
	local node=$1
	PULL=$($FUNC "$BATCHDIR"/pull/$node/)
	PUSH=$($FUNC "$BATCHDIR"/push/$node/)
	echo "{ \"Remote\": \"$node\", \"Push\": $PUSH, \"Pull\": $PULL }"
}

if [ "$SYNTAX" = "nagios" ]; then
	PULL=$($FUNC "$BATCHDIR"/pull/$NODE/)
	PUSH=$($FUNC "$BATCHDIR"/push/$NODE/)
	LONGMSG="pull $PULL;\npush $PUSH;"
	LONGPERF="pull=$PULL;$WARN;$CRIT;0\npush=$PUSH;$WARN;$CRIT;0"
	
	if [ "$NODE" = "*" ]; then
		TOSCHED=$($FUNC "$BATCHDIR"/)
		PENDINGTMP=$($FUNC "$BATCHDIR"/tmp/)
		LONGMSG="$LONGMSG\ntosched $TOSCHED;\npendingtmp $PENDINGTMP;"
		LONGPERF="$LONGPERF\ntosched=$TOSCHED;$WARN;$CRIT;0\npendingtmp=$PENDINGTMP;$WARN;$CRIT;0"
	fi
	
	MAX=$(max $PULL $PUSH $TOSCHED $PENDINGTMP)
	if [ "$MAX" -lt "$WARN" ]; then
		RES="OK"
		RET=0
	elif [ "$MAX" -lt "$CRIT" ]; then
		RES="WARNING"
		RET=1
	else
		RES="CRITICAL"
		RET=2
	fi
	echo "$RES - max: $MAX; | max=$MAX;$WARN;$CRIT;0"
	echo -e "$LONGMSG | $LONGPERF"
	exit $RET

else
	TOSCHED=$($FUNC "$BATCHDIR"/)
	PENDINGTMP=$($FUNC "$BATCHDIR"/tmp/)
	if [ "$SYNTAX" = "human" ]; then
		curtime=$(date +'%d %T')
		echo "SFS Node: $THISNODE       [$curtime]"
		echo "---------------------------------"
		if [ "$NODE" = "*" ]; then
			for node in $(safels "$BATCHDIR"/push/); do
				printHuman $(basename $node)
			done
		else
			printHuman $NODE
		fi
		echo "[Temp: $PENDINGTMP | Sched: $TOSCHED]"
	else
		curtime=$(date -R)
		echo "{ \"Local\": \"$THISNODE\", \"Date\": \"$curtime\", \"Nodes\": ["
		if [ "$NODE" = "*" ]; then
			first=true
			for node in $(safels "$BATCHDIR"/push/); do
				if ! $first; then
					echo ","
				else
					first=false
				fi
				printJson $(basename $node)
			done
		else
			printJson $NODE
		fi
		echo "], \"Temp\": $PENDINGTMP, \"Sched\": $TOSCHED }"
	fi
fi
