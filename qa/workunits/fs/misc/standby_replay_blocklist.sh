#!/bin/sh -x

# Blocklisting a client when the mds is in 'standby-replay' state should not journal the
# blocklist which would hit the assert 'ceph_assert(!mds->is_any_replay())' in
# 'MDLog::_submit_entry'

set -e

# addr format 192.168.1.9:0/3611819609",
addr=$(ceph tell mds.* client ls 2>/dev/null | grep inst | tail -1 | awk '{print $3}')

# Remove '",' from the addr above
addr=${addr::-2}

# allow standby replay
ceph fs set a allow_standby_replay true

# Blocklisting the client should not assert
ceph blocklist add $addr

# Clean up
ceph blocklist clear
df -h

echo OK
