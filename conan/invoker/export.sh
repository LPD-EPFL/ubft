#!/bin/bash

set -e

CONANDEP=$1

# The package has already been exported during check-changes.
# No further action is needed.

mkdir -p "$(dirname $CONANDEP)" && touch $CONANDEP
