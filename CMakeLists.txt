cmake_minimum_required(VERSION 3.22)

# Component sources live in ./src instead of the default ./main
set(EXTRA_COMPONENT_DIRS src)

set(PROJECT_VER "0.5.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wavedemo)
