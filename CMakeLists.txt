cmake_minimum_required(VERSION 3.19.0)
project(wmx VERSION 9.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# RM=/bin/rm
# set (PREFIX /usr/local)
# LIB_SUFFIX?=lib
# MANPREFIX?=$(PREFIX)/share/man
# set (TWOBWM_PATH ${PREFIX}/bin/2bwm)
# X11_INCLUDE?=/usr/local/include

find_program(CCACHE ccache)
if (CCACHE)
	message("using ccache")
	set (CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
else()
	message("ccache not found, cannot use")
endif()

find_package(X11 REQUIRED)

add_compile_options(-Wall -Wextra -Wpedantic -fpermissive)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(2bwm 2bwm.cxx)
add_executable(hidden hidden.cxx)

install(TARGETS 2bwm DESTINATION ${BINDIR})
install(TARGETS hidden DESTINATION ${BINDIR})

target_sources(2bwm PRIVATE
	2bwm.cxx
	config.hxx
)

target_sources(hidden PRIVATE
	hidden.cxx
)

target_include_directories(2bwm SYSTEM PUBLIC ${X11_INCLUDE_DIR} ${X11_Xrandr_INCLUDE_PATH})
target_include_directories(hidden SYSTEM PUBLIC ${X11_INCLUDE_DIR} ${X11_Xrandr_LIB})
target_link_libraries(2bwm PUBLIC ${X11_LIBRARIES})
target_link_libraries(hidden PUBLIC ${X11_LIBRARIES})

target_include_directories(2bwm PUBLIC "${PROJECT_BINARY_DIR}")
target_include_directories(hidden PUBLIC "${PROJECT_BINARY_DIR}")
