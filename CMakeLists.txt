# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS ./src ./web_assets)
set(PROJECT_VER "1.5.2")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(openmc VERSION 1.5.2)
