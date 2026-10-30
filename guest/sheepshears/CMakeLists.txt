cmake_minimum_required(VERSION 3.10)
project(SheepShears C CXX)
add_application(SheepShears sheepshears.c sheepshears.r CREATOR "ShSh")
target_compile_options(SheepShears PRIVATE -O2 -Wall -Wno-multichar)
