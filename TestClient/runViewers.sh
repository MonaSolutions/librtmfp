#!/bin/bash
#
# Test script for starting many group viewers

URL="rtmfp://127.0.0.1/test"
GROUP="G:027f02010101000103010c050e74657374011b00"
LOG=8
DUMP=""
UPDATEPERIOD=100
WINDOWDURATION=8000
NB=1

for i in "$@"
do
case $i in
    --url=*)
    URL="${i#*=}"
    shift # past argument=value
    ;;
    --group=*)
    GROUP="${i#*=}"
    shift # past argument=value
    ;;
    --log=*)
    LOG="${i#*=}"
    shift # past argument=value
    ;;
    --dump)
    DUMP="--dump"
    ;;
    --nb=*)
    NB="${i#*=}"
    shift # past argument=value
    ;;
    --updatePeriod=*)
    UPDATEPERIOD="${i#*=}"
    shift # past argument=value
    ;;
    --windowDuration=*)
    WINDOWDURATION="${i#*=}"
    shift # past argument=value
    ;;
    *)
    echo "Unknown option $i"
    exit -1
    ;;
esac
done


for (( i=1; i<=$NB; i++ ))
do
  echo "./TestClient --url=$URL --netgroup=$GROUP --log=$LOG $DUMP --updatePeriod=$UPDATEPERIOD --windowDuration=$WINDOWDURATION --logFile=testPlay$i.log --mediaFile=out$i.flv &>/dev/null &"
  ./TestClient --url=$URL --netgroup=$GROUP --log=$LOG $DUMP --updatePeriod=$UPDATEPERIOD --windowDuration=$WINDOWDURATION --logFile=testPlay$i.log --mediaFile=out$i.flv &>/dev/null &
done

