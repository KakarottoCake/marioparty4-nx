#!/bin/bash

# Double-click this file in Finder. The real installer stays in the adjacent
# .sh file so it can also be run from Terminal.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec /bin/bash "$SCRIPT_DIR/deploy_to_sd.sh"
