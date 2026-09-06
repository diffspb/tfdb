cmake_minimum_required(VERSION 3.16)
project(tfdb_consumer LANGUAGES CXX)

find_package(TFDB 1 CONFIG REQUIRED)
add_executable(tfdb_consumer main.cpp)
target_link_libraries(tfdb_consumer PRIVATE TFDB::tfdb)
