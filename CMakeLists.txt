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

# querylib {{{

function(querylib flag type pkg out_library out_include_dirs)
  if(${flag})
    if(${type} STREQUAL "cmake")
      find_package(${pkg} REQUIRED)
      string(TOUPPER ${pkg} pkg_upper)
      list(APPEND ${out_library} ${${pkg_upper}_LIBRARY})
      list(APPEND ${out_include_dirs} ${${pkg_upper}_INCLUDE_DIR})
    elseif(${type} STREQUAL "pkg-config")
      find_package(PkgConfig REQUIRED)
      pkg_check_modules(PKG_${flag} REQUIRED ${pkg})

      # Set packet version so that it can be used in the summary
      set(${flag}_VERSION ${PKG_${flag}_VERSION} PARENT_SCOPE)
      list(APPEND ${out_library} ${PKG_${flag}_LIBRARIES})
      list(APPEND ${out_include_dirs} ${PKG_${flag}_INCLUDE_DIRS})
    else()
      message(FATAL_ERROR "Invalid lookup type '${type}'")
    endif()
    set(${out_library} ${${out_library}} PARENT_SCOPE)
    set(${out_include_dirs} ${${out_include_dirs}} PARENT_SCOPE)
  endif()
endfunction()

querylib(WITH_XRM "pkg-config" xcb-xrm libs dirs)

add_compile_options(-Wall -Wextra -Wpedantic)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(2bwm 2bwm.c)
add_executable(hidden hidden.c)

install(TARGETS 2bwm DESTINATION ${BINDIR})
install(TARGETS hidden DESTINATION ${BINDIR})

target_sources(2bwm PRIVATE
	2bwm.c
	list.h
	config.h
	types.h
)

target_sources(hidden PRIVATE
	hidden.c
)

target_include_directories(2bwm SYSTEM PUBLIC ${X11_INCLUDE_DIR} ${X11_Xrandr_INCLUDE_PATH})
target_include_directories(hidden SYSTEM PUBLIC ${X11_INCLUDE_DIR} ${X11_Xrandr_LIB})
target_link_libraries(2bwm PUBLIC ${X11_LIBRARIES}  xcb-xrm)
target_link_libraries(hidden PUBLIC ${X11_LIBRARIES})

target_include_directories(2bwm PUBLIC "${PROJECT_BINARY_DIR}")
target_include_directories(hidden PUBLIC "${PROJECT_BINARY_DIR}")
