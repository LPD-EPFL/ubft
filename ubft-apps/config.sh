#!/bin/bash
BINARIES_DIR=$1

export DORY_LIB_REPARENT_PATH=$BINARIES_DIR/libreparent.so
export LD_LIBRARY_PATH="$BINARIES_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SPDLOG_LEVEL=UbftServer=OFF,RequestLog=OFF,RpcClientRequestIngress=OFF
