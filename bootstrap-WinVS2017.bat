@echo off

call git submodule update --init --recursive

mkdir buildWinVS2017
cd buildWinVS2017
cmake -G "Visual Studio 15 2017 Win64" ..