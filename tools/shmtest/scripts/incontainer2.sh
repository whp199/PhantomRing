#!/bin/sh
# executed from inside a Proton/SLR4 container; asks Steam's host-side launcher service to run a harmless command
d=${WORK:-/tmp}/proton
echo "container mnt_ns=$(readlink /proc/self/ns/mnt)" > $d/alongside_client.out
timeout 20 steam-runtime-launch-client --alongside-steam -- /bin/sh -c "echo host-side mnt_ns=\$(readlink /proc/self/ns/mnt) pid=\$\$ ppid=\$PPID > $d/alongside_host.out; ls -la /usr/bin/gamescope >> $d/alongside_host.out 2>&1" >> $d/alongside_client.out 2>&1
echo "launch-client rc=$?" >> $d/alongside_client.out
