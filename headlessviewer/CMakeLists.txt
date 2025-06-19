include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/osc_strict_compiler_options.cmake)  # OSC_STRICT_COMPILER_OPTIONS

add_executable(headlessviewer
    main.cpp
)
set_target_properties(headlessviewer PROPERTIES
    CXX_EXTENSIONS OFF
)
target_compile_features(headlessviewer PUBLIC
    cxx_std_23
)
target_compile_options(headlessviewer PRIVATE
    ${OSC_STRICT_COMPILER_OPTIONS}
)
target_include_directories(headlessviewer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..      # so that `#include "liboscar-demos/HEADER.h"` works
)
target_link_libraries(headlessviewer PRIVATE
    OpenSimCreator
    oscar
    $<$<BOOL:${OSC_BUNDLE_OSCAR_DEMOS}>:oscar_demos>
    stb
)


# for development on Windows, copy all runtime dlls to the exe directory
# (because Windows doesn't have an RPATH)
#
# see: https://cmake.org/cmake/help/latest/manual/cmake-generator-expressions.7.html?highlight=runtime#genex:TARGET_RUNTIME_DLLS
# if (WIN32)
#     add_custom_command(
#         TARGET hellooscar
#         PRE_BUILD
#         COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:hellooscar> $<TARGET_RUNTIME_DLLS:hellooscar>
#         COMMAND_EXPAND_LISTS
#     )
# endif()
