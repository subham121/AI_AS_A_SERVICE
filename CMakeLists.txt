cmake_minimum_required(VERSION 3.20)
project(edge_ai_service LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(CTest)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED gio-2.0 glib-2.0 gobject-2.0)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
pkg_check_modules(CURL REQUIRED libcurl)

set(EDGEAI_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include)
set(EDGEAI_STATE_DIR ${PROJECT_SOURCE_DIR}/var/state)
set(EDGEAI_STAGING_DIR ${PROJECT_SOURCE_DIR}/var/staging)
set(EDGEAI_INSTALL_DIR ${PROJECT_SOURCE_DIR}/var/installed)
set(EDGEAI_CATALOG_URL "http://10.221.31.77:5000/apiv1")

add_library(edgeai_common
    src/common/fs_utils.cpp
    src/common/http_client.cpp
    src/common/json_utils.cpp
    src/common/process_utils.cpp
    src/edge_gateway/capability_router.cpp
    src/edge_gateway/pack_manifest.cpp
    src/edge_gateway/pack_runtime.cpp
    src/edge_gateway/pack_manager.cpp
    src/edge_gateway/ai_gateway_service.cpp
)

target_include_directories(edgeai_common
    PUBLIC
        ${EDGEAI_INCLUDE_DIR}
        ${GLIB_INCLUDE_DIRS}
        ${JSONCPP_INCLUDE_DIRS}
        ${SQLITE3_INCLUDE_DIRS}
        ${CURL_INCLUDE_DIRS}
)
target_link_directories(edgeai_common
    PUBLIC
        ${GLIB_LIBRARY_DIRS}
        ${JSONCPP_LIBRARY_DIRS}
        ${SQLITE3_LIBRARY_DIRS}
        ${CURL_LIBRARY_DIRS}
)

target_compile_options(edgeai_common PRIVATE
    ${GLIB_CFLAGS_OTHER}
    ${JSONCPP_CFLAGS_OTHER}
    ${SQLITE3_CFLAGS_OTHER}
    ${CURL_CFLAGS_OTHER}
)

target_link_libraries(edgeai_common
    PUBLIC
        ${GLIB_LIBRARIES}
        ${JSONCPP_LIBRARIES}
        ${SQLITE3_LIBRARIES}
        ${CURL_LIBRARIES}
        dl
)

target_compile_definitions(edgeai_common
    PUBLIC
        EDGEAI_DEFAULT_STATE_DIR="${EDGEAI_STATE_DIR}"
        EDGEAI_DEFAULT_STAGING_DIR="${EDGEAI_STAGING_DIR}"
        EDGEAI_DEFAULT_INSTALL_DIR="${EDGEAI_INSTALL_DIR}"
        EDGEAI_DEFAULT_CATALOG_URL="${EDGEAI_CATALOG_URL}"
)

add_executable(edge_gatewayd src/edge_gateway/main.cpp)
target_link_libraries(edge_gatewayd PRIVATE edgeai_common)

add_library(next_word_pack MODULE
    packs/next_word/src/next_word_pack.cpp
)

target_include_directories(next_word_pack PRIVATE
    ${EDGEAI_INCLUDE_DIR}
)
target_link_libraries(next_word_pack PRIVATE edgeai_common)
set_target_properties(next_word_pack PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/packs/next_word/lib
)

set(NEXT_WORD_BUNDLE_DIR ${CMAKE_BINARY_DIR}/packs/next_word/bundle)
set(NEXT_WORD_ARTIFACT ${PROJECT_SOURCE_DIR}/artifacts/next_word_pack.tar.gz)

add_custom_command(
    TARGET next_word_pack
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${NEXT_WORD_BUNDLE_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${NEXT_WORD_BUNDLE_DIR}/lib
    COMMAND ${CMAKE_COMMAND} -E make_directory ${NEXT_WORD_BUNDLE_DIR}/runtime
    COMMAND ${CMAKE_COMMAND} -E make_directory ${NEXT_WORD_BUNDLE_DIR}/model
    COMMAND ${CMAKE_COMMAND} -E make_directory ${NEXT_WORD_BUNDLE_DIR}/config
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_SOURCE_DIR}/artifacts
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:next_word_pack> ${NEXT_WORD_BUNDLE_DIR}/lib/
    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/packs/next_word/manifest.json ${NEXT_WORD_BUNDLE_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/packs/next_word/runtime/next_word_helper.py ${NEXT_WORD_BUNDLE_DIR}/runtime/
    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/packs/next_word/model/next_word_bigram.onnx ${NEXT_WORD_BUNDLE_DIR}/model/
    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/packs/next_word/model/vocab.json ${NEXT_WORD_BUNDLE_DIR}/model/
    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/packs/next_word/model/transitions.json ${NEXT_WORD_BUNDLE_DIR}/model/
    COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/packs/next_word/config/default.json ${NEXT_WORD_BUNDLE_DIR}/config/
    COMMAND ${CMAKE_COMMAND} -E chdir ${NEXT_WORD_BUNDLE_DIR} ${CMAKE_COMMAND} -E tar cfvz ${NEXT_WORD_ARTIFACT} --format=gnutar manifest.json lib runtime model config
)

add_custom_target(pack_artifacts ALL DEPENDS next_word_pack)

add_test(
    NAME edge_gateway_help
    COMMAND edge_gatewayd --help
)
