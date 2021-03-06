@echo off
REM ########################################################
REM # This script downloads CoreCLR and the CPython code bases
REM # so that you can build them and use them with Pyjion.
REM # These are locked back to specific versions currently.

echo Grabbing CoreCLR...

REM ########################################################
REM # Clone CoreCLR code base
git clone https://github.com/dotnet/coreclr CoreCLR
pushd CoreCLR
git reset --hard 98c63c1612b2635a40d9a719c1505e0ad4bef08a

REM ########################################################
REM # Apply changes to disable COM interop support

echo Disabling COM interop support...

git apply ..\coreclr.diff
popd

REM ########################################################
REM # Clone CPython code base

echo Getting CPython...
hg clone -r 96609 https://hg.python.org/cpython/ Python

REM ########################################################
REM # Apply changes to integrate JIT support into CPython

echo Applying changes to enable JIT support...
pushd Python
hg import --no-commit ..\python.diff
popd
