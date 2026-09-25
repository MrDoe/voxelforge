cmake_minimum_required(VERSION 3.24)
project(voxelforge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wno-missing-field-initializers)
# Native-arch tuning for the hot CPU paths (surfel bake, SVO synthesis,
# chunk sort). Same-machine gates, so -march=native is safe.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_compile_options(-march=native)
endif()

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
                     GIT_TAG v1.91.8 GIT_SHALLOW TRUE
                     PATCH_COMMAND sed -i -e "s/(uint32_t)(clip_max.x - clip_min.x)/(uint32_t)(clip_max.x - clip_min.x + 0.999f)/" -e "s/(uint32_t)(clip_max.y - clip_min.y)/(uint32_t)(clip_max.y - clip_min.y + 0.999f)/" "<SOURCE_DIR>/backends/imgui_impl_vulkan.cpp")
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

add_library(vf_core STATIC src/core/camera.cpp
                          src/voxel/layered_world.cpp src/voxel/voxel_field.cpp
                          src/voxel/chunk_store.cpp
                          src/voxel/live_editor.cpp
                          src/voxel/surfelize.cpp
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

# Procedural tree generator (space colonization + pipe model). Explicit tool:
# never wired into the default build; run it to regenerate assets/<name>.vxw.
add_executable(vf_trees tools/tree_gen.cpp)
target_include_directories(vf_trees PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vf_trees PRIVATE vf_core)

# STL/OBJ -> voxel converter. Explicit tool: never wired into the default
# build; run it to write assets/<name>.vxw from a mesh file.
add_executable(vf_mesh2vox tools/mesh_to_voxel.cpp)
target_include_directories(vf_mesh2vox PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vf_mesh2vox PRIVATE vf_core)

# ---- MCP server -----------------------------------------------------------------
add_executable(vf_mcp src/ai/mcp_server.cpp)
target_include_directories(vf_mcp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vf_mcp PRIVATE vf_core)

set(VF_HEIGHTMAP_PNG ${CMAKE_CURRENT_SOURCE_DIR}/assets/heightmap.png)
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
add_custom_command(OUTPUT ${VF_HEIGHTMAP_PNG} ${VF_WORLD_LAYERS}
    COMMAND heightmap_gen ${VF_HEIGHTMAP_PNG} ${CMAKE_CURRENT_SOURCE_DIR}/assets/world.vxw
    DEPENDS heightmap_gen
    COMMENT "heightmap_gen -> assets/heightmap.png + layer family + world.json"
    VERBATIM)
add_custom_target(vf_heightmap DEPENDS ${VF_HEIGHTMAP_PNG} ${VF_WORLD_LAYERS})

add_library(vf_rhi STATIC src/rhi/context.cpp src/rhi/resources.cpp src/rhi/swapchain.cpp)
target_link_libraries(vf_rhi PUBLIC vf_platform vf_vma spdlog::spdlog)

add_library(vf_render STATIC src/render/svo_pass.cpp src/render/taa_pass.cpp src/render/post_pass.cpp src/render/splat_pass.cpp
    src/render/ssr_pass.cpp src/render/ssao_pass.cpp src/render/volumetric_fog_pass.cpp
    src/render/motion_blur_pass.cpp src/render/dof_pass.cpp src/render/environment_pass.cpp
    src/render/texture_atlas.cpp)
target_include_directories(vf_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external)
target_compile_definitions(vf_render PUBLIC
    VOXELFORGE_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders"
    VOXELFORGE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
target_link_libraries(vf_render PUBLIC vf_rhi)
add_dependencies(vf_render vf_shaders)

# ---- shaders ------------------------------------------------------------------
find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang REQUIRED)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/shaders)

set(VF_SHADER_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/svo_raymarch.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/post.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/taa_resolve.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/splat.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/splat.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/splat_cull.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/splat_hiz.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/splat_tile_bin.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/splat_tile_scan.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/splat_tile_base.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/splat_tile_render.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ssr.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ssao.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ssao_apply.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/volumetric_fog.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/motion_blur.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/depth_of_field.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/env_map.comp)
set(VF_SHADER_SPVS "")
foreach(src_file ${VF_SHADER_SRCS})
    get_filename_component(name "${src_file}" NAME)
    set(out_file ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.spv)
    add_custom_command(OUTPUT ${out_file}
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.3 -I${CMAKE_CURRENT_SOURCE_DIR}/shaders -o ${out_file} ${src_file}
        DEPENDS ${src_file} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/common_base.glsl ${CMAKE_CURRENT_SOURCE_DIR}/shaders/common_svo.glsl ${CMAKE_CURRENT_SOURCE_DIR}/shaders/common_splat.glsl ${CMAKE_CURRENT_SOURCE_DIR}/shaders/common_surfel.glsl
        COMMENT "glslangValidator ${name}")
    list(APPEND VF_SHADER_SPVS ${out_file})
endforeach()
add_custom_target(vf_shaders ALL DEPENDS ${VF_SHADER_SPVS})

# ---- app ------------------------------------------------------------------------
add_executable(voxelforge src/app/main.cpp src/app/chat_ui.cpp)
target_compile_definitions(voxelforge PRIVATE
    VOXELFORGE_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_link_libraries(voxelforge PRIVATE vf_render vf_core imgui_core)
add_dependencies(voxelforge vf_shaders)
add_custom_target(world DEPENDS ${VF_HEIGHTMAP_PNG} ${VF_WORLD_LAYERS})

# ---- tests -------------------------------------------------------------------------
enable_testing()
add_executable(vf_tests tests/main.cpp tests/test_camera.cpp
                        tests/test_world.cpp tests/test_worldfile.cpp
                        tests/test_authoring.cpp tests/test_chat_tools.cpp
                        tests/test_picking.cpp tests/test_editable.cpp
                        tests/test_surfelize.cpp tests/test_store.cpp
                        src/ai/ollama_client.cpp)
target_link_libraries(vf_tests PRIVATE vf_core vf_doctest)
target_compile_definitions(vf_tests PRIVATE DOCTEST_CONFIG_NO_MULTITHREADING)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(GROUP_GATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/group_gate.py)
add_test(NAME unit_tests
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} unit
            $<TARGET_FILE:vf_tests>)
add_test(NAME unit_surfel_tests
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} unit,surfel
            $<TARGET_FILE:vf_tests> --test-case=*surfel*)
add_test(NAME unit_store_tests
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} unit,store,live-edit
            $<TARGET_FILE:vf_tests> --test-case=*chunk*)
add_test(NAME unit_world_tests
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} unit,world
            $<TARGET_FILE:vf_tests> --test-case=*world*)
add_test(NAME unit_worldfile_tests
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} unit,world,worldfile
            $<TARGET_FILE:vf_tests> --test-case=*worldfile*)

# ---- material textures ----------------------------------------------------------
# The shipped photo set comes from tools/fetch_textures.py (ambientCG, CC0,
# mean-matched to the palette). This target regenerates the offline fallback
# set (assets/textures/proc_*.png); both appear in the in-app texture picker.
add_custom_target(vf_textures
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_textures.py
    COMMENT "gen_textures.py -> assets/textures/proc_*.png (offline set)")
add_test(NAME visual_check
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} visual
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/visual_check.py
            $<TARGET_FILE:voxelforge>)
add_test(NAME live_edit_check
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} live-edit
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_edit_check.py
            $<TARGET_FILE:voxelforge>)
add_test(NAME ssao_check
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} effects
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ssao_check.py
            $<TARGET_FILE:voxelforge>)
add_test(NAME texture_check
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} textures
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/texture_check.py
            $<TARGET_FILE:voxelforge>)
add_test(NAME fog_check
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} fog
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fog_check.py
            $<TARGET_FILE:voxelforge>)
add_test(NAME gpu_selftest
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} visual,surfel
            $<TARGET_FILE:voxelforge> --selftest --width 640 --height 360)

# Every registered test is group-gated by tests/group_gate.py. A bare CTest
# invocation therefore skips the checks instead of starting an accidental
# all-tests run; use the focused test-<group> targets below.

set_tests_properties(unit_tests
    PROPERTIES LABELS "unit" SKIP_RETURN_CODE 77)
set_tests_properties(unit_surfel_tests
    PROPERTIES LABELS "unit;surfel" SKIP_RETURN_CODE 77)
set_tests_properties(unit_store_tests
    PROPERTIES LABELS "unit;store;live-edit" SKIP_RETURN_CODE 77)
set_tests_properties(unit_world_tests
    PROPERTIES LABELS "unit;world" SKIP_RETURN_CODE 77)
set_tests_properties(unit_worldfile_tests
    PROPERTIES LABELS "unit;world;worldfile" SKIP_RETURN_CODE 77)
set_tests_properties(visual_check
    PROPERTIES LABELS "visual" SKIP_RETURN_CODE 77)
set_tests_properties(live_edit_check
    PROPERTIES LABELS "live-edit" SKIP_RETURN_CODE 77)
set_tests_properties(ssao_check
    PROPERTIES LABELS "effects;ssao" SKIP_RETURN_CODE 77)
set_tests_properties(texture_check
    PROPERTIES LABELS "textures" SKIP_RETURN_CODE 77)
set_tests_properties(fog_check
    PROPERTIES LABELS "fog" SKIP_RETURN_CODE 77)
set_tests_properties(gpu_selftest
    PROPERTIES LABELS "visual;surfel" SKIP_RETURN_CODE 77)

add_test(NAME fast_unit_tests
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} unit,camera,smoke
            $<TARGET_FILE:vf_tests> --test-case=*camera*)
add_test(NAME fast_placement_tests
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} unit,placement,smoke
            $<TARGET_FILE:vf_tests> --test-case=*placement*)
add_test(NAME fast_visual_check
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} visual,surfel,smoke
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/visual_check.py
            $<TARGET_FILE:voxelforge>)
add_test(NAME fast_live_edit_check
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} live-edit,store,surfel,smoke
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_edit_check.py
            $<TARGET_FILE:voxelforge>)
add_test(NAME fast_ssao_check
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} effects,ssao,smoke
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ssao_check.py
            $<TARGET_FILE:voxelforge>)
add_test(NAME fast_texture_check
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} textures,smoke
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/texture_check.py
            $<TARGET_FILE:voxelforge>)
add_test(NAME fast_fog_check
    COMMAND ${Python3_EXECUTABLE} ${GROUP_GATE} fog,effects,smoke
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/fog_check.py
            $<TARGET_FILE:voxelforge>)
set_tests_properties(fast_unit_tests
    PROPERTIES LABELS "unit;camera;smoke" ENVIRONMENT "VF_FAST_TESTS=1"
                     SKIP_RETURN_CODE 77)
set_tests_properties(fast_placement_tests
    PROPERTIES LABELS "unit;placement;smoke" ENVIRONMENT "VF_FAST_TESTS=1"
                     SKIP_RETURN_CODE 77)
set_tests_properties(fast_visual_check
    PROPERTIES LABELS "visual;surfel;smoke" ENVIRONMENT "VF_FAST_TESTS=1"
                     SKIP_RETURN_CODE 77)
set_tests_properties(fast_live_edit_check
    PROPERTIES LABELS "live-edit;store;surfel;smoke" ENVIRONMENT "VF_FAST_TESTS=1"
                     SKIP_RETURN_CODE 77)
set_tests_properties(fast_ssao_check
    PROPERTIES LABELS "effects;ssao;smoke" ENVIRONMENT "VF_FAST_TESTS=1"
                     SKIP_RETURN_CODE 77)
set_tests_properties(fast_texture_check
    PROPERTIES LABELS "textures;smoke" ENVIRONMENT "VF_FAST_TESTS=1"
                     SKIP_RETURN_CODE 77)
set_tests_properties(fast_fog_check
    PROPERTIES LABELS "fog;effects;smoke" ENVIRONMENT "VF_FAST_TESTS=1"
                     SKIP_RETURN_CODE 77)

function(vf_add_test_group group)
    add_custom_target(test-${group}
        COMMAND ${CMAKE_COMMAND} -E env VOXELFORGE_TEST_GROUPS=${group}
                ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR}
                --output-on-failure -L ${group}
        DEPENDS ${ARGN}
        USES_TERMINAL
        COMMENT "Voxelforge ${group} test group")
endfunction()

# Focused groups are the only supported test entry points. There is deliberately
# no test-all target: select the smallest group that covers the change.
vf_add_test_group(unit vf_tests)
vf_add_test_group(surfel vf_tests voxelforge)
vf_add_test_group(store vf_tests voxelforge)
vf_add_test_group(world vf_tests)
vf_add_test_group(live-edit vf_tests voxelforge)
vf_add_test_group(visual voxelforge)
vf_add_test_group(effects voxelforge)
vf_add_test_group(textures voxelforge)
vf_add_test_group(fog voxelforge)
vf_add_test_group(smoke vf_tests voxelforge)

# Keep the historical spelling as a compatibility alias; both run only the
# smoke group, never the complete suite.
add_custom_target(test-fast DEPENDS test-smoke)
