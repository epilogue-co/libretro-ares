add_library(
  ares_libretro
  SHARED
  libretro.cpp
  program.cpp
)

target_include_directories(
  ares_libretro
  PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/ares/n64/vulkan/parallel-rdp/parallel-rdp
    ${CMAKE_SOURCE_DIR}/ares/n64/vulkan/parallel-rdp/volk
    ${CMAKE_SOURCE_DIR}/ares/n64/vulkan/parallel-rdp/vulkan
    ${CMAKE_SOURCE_DIR}/ares/n64/vulkan/parallel-rdp/vulkan-headers/include
    ${CMAKE_SOURCE_DIR}/ares/n64/vulkan/parallel-rdp/util
)

target_link_libraries(
  ares_libretro
  PRIVATE ares::ares ares::mia ares::nall
)

set_target_properties(
  ares_libretro
  PROPERTIES
    OUTPUT_NAME ares_libretro
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/cores"
)

if(APPLE)
  target_link_options(
    ares_libretro
    PRIVATE
      "-Wl,-rpath,@loader_path"
      "-Wl,-rpath,@loader_path/../Frameworks"
  )
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES libretro.cpp program.cpp program.hpp)
