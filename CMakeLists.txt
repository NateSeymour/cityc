cmake_minimum_required(VERSION 3.29)
project(cityc)

set(CMAKE_CXX_STANDARD 23)

# External libs
include(FetchContent)
FetchContent_Declare(
        city
        GIT_REPOSITORY https://github.com/NateSeymour/city.git
        GIT_TAG v0.0.5
)
FetchContent_MakeAvailable(city)

FetchContent_Declare(
        tree-sitter
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
        GIT_TAG v0.25.1
        SOURCE_SUBDIR lib
)
FetchContent_MakeAvailable(tree-sitter)

FetchContent_Declare(
        tree-sitter-c
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c.git
        GIT_TAG v0.23.4
)
FetchContent_MakeAvailable(tree-sitter-c)

add_executable(cityc src/main.cpp
        test/basic.c)
target_link_libraries(cityc PUBLIC city tree-sitter tree-sitter-c)
target_include_directories(cityc PRIVATE ${tree-sitter_SOURCE_DIR}/lib/include)