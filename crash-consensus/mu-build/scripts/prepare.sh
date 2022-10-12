#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

cd "$SCRIPT_DIR"/..

git clone https://github.com/LPD-EPFL/mu
cd mu

git revert -n 9b480342a28b2600f9d2ebec5e1cbb4b76148a00 &>/dev/null
git revert -n 1c846cd76b4d535e646bf581724d894d54b1591d &>/dev/null
git restore --staged crash-consensus/experiments/liquibook/liquibook_additions.tar.gz &>/dev/null
patch -p 1 < ../patches/thread_sleep.patch
patch -p 1 < ../patches/default_cores.patch
patch -p 1 < ../patches/fix_leader.patch
patch -p 1 < ../patches/add_constness.patch
