cmake_minimum_required(VERSION 3.24)
project(voxelforge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wno-missing-field-initializers)

find_package(Vulkan REQUIRED)

include(FetchContent)

# ---- spdlog ------------------------------------------------------------------
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git
                     GIT_TAG v1.14.1 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(spdlog)

# ---- GLFW -----------------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git
                     GIT_TAG 3.4 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw)

# ---- header-only fetches (populate only, no build) -------------------------
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git
                     GIT_TAG 1.0.1 GIT_SHALLOW TRUE)
FetchContent_GetProperties(glm)
if(NOT glm_POPULATED)
    FetchContent_Populate(glm)
    add_library(vf_glm INTERFACE)
    target_include_directories(vf_glm INTERFACE ${glm_SOURCE_DIR})
endif()

FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
                     GIT_TAG v3.1.0 GIT_SHALLOW TRUE)
FetchContent_GetProperties(vma)
if(NOT vma_POPULATED)
    FetchContent_Populate(vma)
    add_library(vf_vma INTERFACE)
    target_include_directories(vf_vma INTERFACE ${vma_SOURCE_DIR}/include)
endif()

FetchContent_Declare(doctest GIT_REPOSITORY https://github.com/doctest/doctest.git
                     GIT_TAG v2.4.12 GIT_SHALLOW TRUE)
FetchContent_GetProperties(doctest)
if(NOT doctest_POPULATED)
    FetchContent_Populate(doctest)
    add_library(vf_doctest INTERFACE)
    target_include_directories(vf_doctest INTERFACE ${doctest_SOURCE_DIR})
endif()

# ---- Dear ImGui -------------------------------------------------------------
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git
                     GIT_TAG v1.91.8 GIT_SHALLOW TRUE)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
    add_library(imgui_core STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
    target_include_directories(imgui_core PUBLIC ${imgui_SOURCE_DIR}
                                                 ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui_core PUBLIC glfw Vulkan::Vulkan)
    target_compile_options(imgui_core PRIVATE -Wno-attributes)
endif()

# ---- engine libraries -------------------------------------------------------
add_library(vf_platform STATIC src/platform/window.cpp)
target_include_directories(vf_platform PUBLIC src)
target_link_libraries(vf_platform PUBLIC glfw Vulkan::Vulkan vf_glm spdlog::spdlog)

add_library(vf_core STATIC src/core/camera.cpp src/voxel/volume.cpp src/voxel/world.cpp
                           src/voxel/heightmap.cpp src/voxel/worldfile.cpp
                           src/voxel/picking.cpp src/voxel/editable_world.cpp
                           src/ai/ollama_client.cpp)
target_include_directories(vf_core PUBLIC src PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external)
target_compile_definitions(vf_core PUBLIC
    VOXELFORGE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
target_link_libraries(vf_core PUBLIC vf_glm spdlog::spdlog vf_platform)

# ---- heightmap + world assets ---------------------------------------------------
add_executable(heightmap_gen tools/heightmap_gen.cpp)
target_include_directories(heightmap_gen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(heightmap_gen PRIVATE vf_core)

# ---- authoring tools ------------------------------------------------------------
add_executable(vf_slice tools/scene_slice.cpp)
target_include_directories(vf_slice PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vf_slice PRIVATE vf_core)

# ---- MCP server -----------------------------------------------------------------
add_executable(vf_mcp src/ai/mcp_server.cpp)
target_include_directories(vf_mcp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vf_mcp PRIVATE vf_core)

set(VF_HEIGHTMAP_PNG ${CMAKE_CURRENT_SOURCE_DIR}/assets/heightmap.png)
set(VF_WORLD_VXW ${CMAKE_CURRENT_SOURCE_DIR}/assets/world.vxw)
set(VF_WORLD_LAYERS ${CMAKE_CURRENT_SOURCE_DIR}/assets/world.json
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/landscape.vxw
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/house.vxw
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/tree1.vxw ${CMAKE_CURRENT_SOURCE_DIR}/assets/tree2.vxw
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/tree3.vxw ${CMAKE_CURRENT_SOURCE_DIR}/assets/tree4.vxw
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/tree5.vxw ${CMAKE_CURRENT_SOURCE_DIR}/assets/tree6.vxw
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/rock1.vxw ${CMAKE_CURRENT_SOURCE_DIR}/assets/rock2.vxw
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/rock3.vxw
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/bushes.vxw
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/alpaca.vxw
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/fence1.vxw)
add_custom_command(OUTPUT ${VF_HEIGHTMAP_PNG} ${VF_WORLD_VXW} ${VF_WORLD_LAYERS}
    COMMAND heightmap_gen ${VF_HEIGHTMAP_PNG} ${VF_WORLD_VXW}
    DEPENDS heightmap_gen
    COMMENT "heightmap_gen -> assets/heightmap.png + world.vxw + world.json layers"
    VERBATIM)
add_custom_target(vf_heightmap DEPENDS ${VF_HEIGHTMAP_PNG} ${VF_WORLD_VXW})

add_library(vf_rhi STATIC src/rhi/context.cpp src/rhi/resources.cpp src/rhi/swapchain.cpp)
target_link_libraries(vf_rhi PUBLIC vf_platform vf_vma spdlog::spdlog)

add_library(vf_render STATIC src/render/raymarch_pass.cpp src/render/splat_pass.cpp src/render/svo_pass.cpp src/render/taa_pass.cpp)
target_include_directories(vf_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external)
target_compile_definitions(vf_render PUBLIC
    VOXELFORGE_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders"
    VOXELFORGE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
target_link_libraries(vf_render PUBLIC vf_rhi)
add_dependencies(vf_render vf_shaders)

# ---- shaders ------------------------------------------------------------------
find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang REQUIRED)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/shaders)

set(VF_SHADER_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/raymarch.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/svo_raymarch.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/splat.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/splat.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/taa_resolve.comp)
set(VF_SHADER_SPVS "")
foreach(src_file ${VF_SHADER_SRCS})
    get_filename_component(name "${src_file}" NAME)
    set(out_file ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.spv)
    add_custom_command(OUTPUT ${out_file}
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.3 -o ${out_file} ${src_file}
        DEPENDS ${src_file}
        COMMENT "glslangValidator ${name}")
    list(APPEND VF_SHADER_SPVS ${out_file})
endforeach()
add_custom_target(vf_shaders ALL DEPENDS ${VF_SHADER_SPVS})

# ---- app ------------------------------------------------------------------------
add_executable(voxelforge src/app/main.cpp src/app/chat_ui.cpp)
target_compile_definitions(voxelforge PRIVATE
    VOXELFORGE_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_link_libraries(voxelforge PRIVATE vf_render vf_core imgui_core tbb)
add_dependencies(voxelforge vf_shaders)
add_custom_target(world DEPENDS ${VF_HEIGHTMAP_PNG} ${VF_WORLD_VXW})

# ---- tests -------------------------------------------------------------------------
enable_testing()
add_executable(vf_tests tests/main.cpp tests/test_volume.cpp tests/test_camera.cpp
                        tests/test_world.cpp tests/test_worldfile.cpp
                        tests/test_authoring.cpp tests/test_chat_tools.cpp
                        src/ai/ollama_client.cpp)
target_link_libraries(vf_tests PRIVATE vf_core vf_doctest)
target_compile_definitions(vf_tests PRIVATE DOCTEST_CONFIG_NO_MULTITHREADING)
add_dependencies(vf_tests vf_heightmap)
add_test(NAME unit_tests COMMAND vf_tests)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME visual_check
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/visual_check.py
            $<TARGET_FILE:voxelforge>)
