cmake_minimum_required(VERSION 2.8)
project(ART)

set(ART_FILES
    Key.h
    N16.cpp
    N256.cpp
    N48.cpp
    N4.cpp
    N.cpp
    N.h
    Tree.cpp
    Tree.h
)
add_library(ARTSynchronized ${ART_FILES})
