#!/bin/sh
# Root-owned Barista session wrapper. No network namespace survives a healthy
# session. The host's default routes, firewall and other adapters are untouched.
set -u
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
[ "$#" -ge 3 ] || { echo "appliance: usage: IFACE ENGINE ARGS..." >&2; exit 2; }
iface=$1
engine=$2
shift 2
case "$iface" in *[!a-zA-Z0-9_.-]*|"" ) echo "appliance: invalid interface" >&2; exit 2;; esac
[ -e "/sys/class/net/$iface/phy80211" ] || { echo "appliance: radio absent" >&2; exit 1; }
phy=$(basename "$(readlink -f "/sys/class/net/$iface/phy80211")")
case "$phy" in phy[0-9]*) ;; *) echo "appliance: invalid PHY" >&2; exit 1;; esac
ns="barista-$$"
created=no
moved=no
child=

cleanup() {
    if [ "$moved" = yes ]; then
        # Move the entire wireless PHY back, not merely one virtual interface.
        if ! ip netns exec "$ns" iw phy "$phy" set netns 1; then
            echo "appliance: CRITICAL: radio could not be returned to host." >&2
            echo "appliance: recovery: ip netns exec $ns iw phy $phy set netns 1" >&2
            echo "appliance: namespace retained for recovery" >&2
            return 1
        fi
        moved=no
    fi
    if [ "$created" = yes ]; then
        ip netns delete "$ns" || return 1
        created=no
    fi
}
on_term() {
    trap - TERM INT
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
    fi
    exit 143
}
trap 'cleanup || :' EXIT
trap on_term TERM INT
ip netns add "$ns" || exit 1
created=yes
# Wireless PHYs, unlike ordinary Ethernet interfaces, must be moved with iw.
iw phy "$phy" set netns name "$ns" || exit 1
moved=yes
ip netns exec "$ns" "$engine" "$@" &
child=$!
result=0
wait "$child" || result=$?
child=
cleanup || exit 1
trap - EXIT
exit "$result"
