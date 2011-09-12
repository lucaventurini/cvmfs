#!/bin/sh

CURL_VERSION=7.21.3
FUSE_VERSION=2.8.4
FUSE4X_VERSION=2.8.5
JEMALLOC_VERSION=2.2.1
ZLIB_VERSION=1.2.5

# put the extracted stuff out of source for compilation (location given by cmake)
outOfSource=$1

# check if bootstrapping already happened
if [ -f "$outOfSource/.decompressionDone" ]; then
	exit 0
fi

# CURL
cd 3rdParty/libcurl
tar xfz curl-${CURL_VERSION}.tar.gz 
mkdir -p "$outOfSource/libcurl/src"
mv curl-${CURL_VERSION}/* "$outOfSource/libcurl/src"
cp src/* "$outOfSource/libcurl/src"
rm -rf curl-${CURL_VERSION}
cd ../..

# FUSE
cd 3rdParty/libfuse
tar xfz fuse-${FUSE_VERSION}.tar.gz
mkdir -p "$outOfSource/libfuse/src"
mv fuse-${FUSE_VERSION}/* "$outOfSource/libfuse/src"
patch -d "$outOfSource/libfuse" -N -p0 < fuse-drainout.patch
cp src/* "$outOfSource/libfuse/src"
rm -rf fuse-${FUSE_VERSION}
cd ../..

# Fuse4x
cd 3rdParty/libfuse4x
tar xfz fuse4x-${FUSE4X_VERSION}.tar.gz
mkdir -p "$outOfSource/libfuse4x/src"
mv fuse4x-${FUSE4X_VERSION}/* "$outOfSource/libfuse4x/src"
patch -d "$outOfSource/libfuse4x" -N -p0 < fuse4x-drainout.patch
cp src/* "$outOfSource/libfuse4x/src"
rm -rf fuse4x-${FUSE4X_VERSION}
cd ../..

# Jemalloc
cd 3rdParty/jemalloc
tar xfj jemalloc-${JEMALLOC_VERSION}.tar.bz2
mv jemalloc-${JEMALLOC_VERSION}/configure.ac jemalloc-${JEMALLOC_VERSION}/configure.ac.vanilla
touch jemalloc-${JEMALLOC_VERSION}/configure.ac
mkdir -p "$outOfSource/jemalloc/src"
mv jemalloc-${JEMALLOC_VERSION}/* "$outOfSource/jemalloc/src"
patch -d "$outOfSource/jemalloc" -N -p0 < jemalloc-2.2.1-64bit_literals.patch
cp src/* "$outOfSource/jemalloc/src"
rm -rf jemalloc-${JEMALLOC_VERSION} 
cd ../..

# Zlib
cd 3rdParty/zlib
tar xfz zlib-${ZLIB_VERSION}.tar.gz
mkdir -p "$outOfSource/zlib/src"
mv zlib-${ZLIB_VERSION}/* "$outOfSource/zlib/src"
cp src/* "$outOfSource/zlib/src"
rm -rf zlib-${ZLIB_VERSION}
cd ../..

# sqlite3
cd 3rdParty/sqlite3
mkdir -p "$outOfSource/sqlite3/src"
cp src/* "$outOfSource/sqlite3/src"
cd ../..

# create a hint that bootstrapping is already done
touch "$outOfSource/.decompressionDone"