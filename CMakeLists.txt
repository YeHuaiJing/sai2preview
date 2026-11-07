cmake_minimum_required(VERSION 3.12)
project(sai2preview LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(sai2preview STATIC src/sai2_preview.c)
target_include_directories(sai2preview PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(sai2export main.c)
target_link_libraries(sai2export PRIVATE sai2preview)

include(GNUInstallDirs)
install(TARGETS sai2preview sai2export
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES include/sai2_preview.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE THIRD_PARTY_NOTICES.md LICENSES/libsai-MIT.txt
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/sai2preview
)
