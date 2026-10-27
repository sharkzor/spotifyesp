file(GLOB SOURCES "*.cpp")

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash driver
)

# cspot's vendored libraries (opus etc.) trip warnings that newer GCC treats as errors
idf_build_set_property(COMPILE_OPTIONS "-Wno-error;-Wno-sizeof-array-div;-Wno-overloaded-virtual" APPEND)

# Build cspot as a static library without its legacy-I2S audio sinks
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
set(BELL_DISABLE_SINKS ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../cspot/cspot" ${CMAKE_CURRENT_BINARY_DIR}/cspot)

target_link_libraries(${COMPONENT_LIB} PUBLIC cspot)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
