#!/bin/bash

DIR=`readlink -f ./deps`

# Compile SICM
make clean || true
make distclean || true
make uninstall || true
./autogen.sh
./configure --prefix=$DIR --with-jemalloc=$DIR --with-llvm=$(llvm-config-4.0 --prefix) --with-libpfm=$DIR
make -j5
make install