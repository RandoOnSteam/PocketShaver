cmake_minimum_required(VERSION 3.10)
project(SheepBench C CXX)
add_application(SheepBench main.c sbkernels.c CONSOLE CREATOR "ShBn")
target_compile_options(SheepBench PRIVATE -O2 -Wall -Wno-multichar -ffp-contract=off -Wno-pointer-sign -Wno-unused-function)
