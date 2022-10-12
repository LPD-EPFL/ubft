#!/bin/bash
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

cd "$SCRIPT_DIR"

rm -rf binaries.zip

cd reparent
./build.sh
cd ..

zip -j binaries.zip reparent/libreparent.so
zip -j binaries.zip ../crash-consensus/src/lib/release/libcrashconsensus.so

../build.py .
zip -uj binaries.zip build/bin/mu-client
zip -uj binaries.zip build/bin/mu-server
zip -uj binaries.zip build/bin/ubft-client
zip -uj binaries.zip build/bin/ubft-server

crashconsensus_path=$(ldd build/bin/mu-server | grep libcrashconsensus.so | awk '{ print $3 }')
zip -uj binaries.zip "$crashconsensus_path"

zip -uj binaries.zip config.sh
