#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

cd "$SCRIPT_DIR"

rm -rf mu-build/mu
mu-build/scripts/prepare.sh
mu-build/scripts/compile.sh
cp -r mu-build/mu/crash-consensus/libgen/prebuilt-lib/include/* src/include/
cp -r mu-build/mu/crash-consensus/libgen/prebuilt-lib/lib/* src/lib/
