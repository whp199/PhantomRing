#!/bin/bash
# usage: ppair.sh <tag> <prefixX> "<argsX>" <marker> <prefixY> "<argsY>"
# start X in its own Proton container, wait for <marker> in its output, then run Y in another container; wait for both.
S=${WORK:?export WORK=<scratch dir holding the built exes>}; HERE=$(dirname "$(readlink -f "$0")")
tag=$1; px=$2; ax=$3; marker=$4; py=$5; ay=$6
ox=$S/proton/res_${tag}_X.log; oy=$S/proton/res_${tag}_Y.log; rm -f $ox $oy
(SHMTEST_OUT=$ox timeout 150 $HERE/prun.sh $px $ax > /dev/null 2>&1 &)
for i in $(seq 1 300); do grep -q -- "$marker" $ox 2>/dev/null && break; sleep 0.1; done
SHMTEST_OUT=$oy VERB=${VERBY:-waitforexitandrun} timeout 150 $HERE/prun.sh $py $ay > /dev/null 2>&1
for i in $(seq 1 600); do pgrep -f "shmtest-gui" >/dev/null || break; sleep 0.25; done
echo "##### $tag  X[$px]: $ax"; cat $ox; echo "##### $tag  Y[$py]: $ay"; cat $oy
