#!/bin/sh -x

set -e
sudo mkdir -p testdir

file=test_chmod.$$

sudo echo "foo" > testdir/${file}
if test $? != 0; then
	echo "ERROR: Failed to create file ${file}"
	exit 1
fi

sudo chmod 600 testdir 
if test $? != 0; then
	echo "ERROR: Failed to change mode of ${file}"
	exit 1
fi

sudo cat testdir/${file}
if test $? != 0; then
	echo "ERROR: Failed to read file ${file}, Directory read/write DAC override failed for directory"
	exit 1
fi
