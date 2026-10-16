#!/bin/sh
set -eu

orca_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$orca_root/resources/linux-launch-env.sh"
export DESKTOPINTEGRATION=false
exec "$orca_root/build/src/snapmaker-orca" "$@"
