cmake_minimum_required(VERSION 3.21)

include(cmake/LpmMetadata.cmake)
project(${LILYGO_UI_CMAKE_PROJECT_NAME} LANGUAGES C CXX)

include(GNUInstallDirs)
include(CTest)
find_package(LilyGoUI CONFIG REQUIRED)
find_package(Threads REQUIRED)

configure_file(cmake/app_identity.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/app_identity.h @ONLY)

add_library(lilygo_ui_sensors_core STATIC
    src/pages/keyboard/keyboard_source.cpp
    src/pages/sensors/i2c_probe.cpp
    src/pages/sensors/microphone_source.cpp
    src/pages/sensors/sensor_source.cpp
    src/pages/sensors/sensors_model.cpp)
target_include_directories(lilygo_ui_sensors_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_features(lilygo_ui_sensors_core PUBLIC cxx_std_17)
target_compile_options(lilygo_ui_sensors_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(lilygo_ui_sensors_core PUBLIC Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(ALSA REQUIRED)
    target_link_libraries(lilygo_ui_sensors_core PRIVATE ALSA::ALSA)
endif()

add_library(lilygo_ui_sensors_ui STATIC
    src/pages/keyboard/keyboard_view.cpp
    src/app.cpp
    src/components/components.cpp
    src/pages/sensors/sensors_view_model.cpp
    src/pages/sensors/sensors_view.cpp)
target_compile_options(lilygo_ui_sensors_ui PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(lilygo_ui_sensors_ui PUBLIC
    lilygo_ui_sensors_core LilyGoUI::Runtime)

add_executable(lilygo_ui_sensors src/main.cpp)
target_compile_options(lilygo_ui_sensors PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(lilygo_ui_sensors PRIVATE lilygo_ui_sensors_ui)
set_target_properties(lilygo_ui_sensors PROPERTIES
    OUTPUT_NAME "${LILYGO_UI_EXECUTABLE_NAME}")

configure_file(data/launcher.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.json @ONLY)
configure_file(data/application.desktop.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.desktop @ONLY)
configure_file(data/application.metainfo.xml.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.metainfo.xml @ONLY)

install(TARGETS lilygo_ui_sensors
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT app)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.json
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/launcher/apps COMPONENT app)
lilygo_ui_install_launcher_icon("${LILYGO_UI_APP_ID}"
    "${CMAKE_CURRENT_SOURCE_DIR}/${LILYGO_UI_APP_ICON}")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications COMPONENT app)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo COMPONENT app)
lilygo_ui_install_font_licenses(
    "${CMAKE_INSTALL_DATAROOTDIR}/doc/${LILYGO_UI_PACKAGE_NAME}" app)
install(FILES LICENSE README.md
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/${LILYGO_UI_PACKAGE_NAME}
    COMPONENT app)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(microphone-source-tests tests/test_microphone.cpp)
    target_link_libraries(microphone-source-tests PRIVATE lilygo_ui_sensors_core)
    add_test(NAME microphone-source COMMAND microphone-source-tests)
    add_executable(keyboard-source-tests tests/test_keyboard_source.cpp)
    target_link_libraries(keyboard-source-tests PRIVATE lilygo_ui_sensors_core)
    add_test(NAME keyboard-source COMMAND keyboard-source-tests)
    add_executable(sensors-source-tests tests/test_source.cpp)
    target_link_libraries(sensors-source-tests PRIVATE lilygo_ui_sensors_core)
    add_test(NAME sensors-source COMMAND sensors-source-tests)

    add_executable(sensors-core-tests tests/test_core.cpp)
    target_link_libraries(sensors-core-tests PRIVATE lilygo_ui_sensors_core)
    add_test(NAME sensors-core COMMAND sensors-core-tests)

    add_executable(sensors-view-model-tests tests/test_view_model.cpp)
    target_link_libraries(sensors-view-model-tests PRIVATE lilygo_ui_sensors_ui)
    add_test(NAME sensors-view-model COMMAND sensors-view-model-tests)

    add_executable(sensors-render-ui tests/render_ui.cpp)
    target_link_libraries(sensors-render-ui PRIVATE lilygo_ui_sensors_ui)
    foreach(test_target IN ITEMS microphone-source-tests keyboard-source-tests sensors-source-tests sensors-core-tests
            sensors-view-model-tests sensors-render-ui)
        target_compile_options(${test_target} PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    endforeach()
    foreach(viewport IN ITEMS 568x1232 1232x568 320x480 320x240)
        string(REPLACE "x" ";" dimensions "${viewport}")
        list(GET dimensions 0 width)
        list(GET dimensions 1 height)
        add_test(NAME sensors-render-${viewport} COMMAND sensors-render-ui
            ${width} ${height}
            ${CMAKE_CURRENT_BINARY_DIR}/sensors-${viewport}.ppm)
    endforeach()
endif()

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME "${LILYGO_UI_PACKAGE_NAME}")
set(CPACK_PACKAGE_VERSION "${LILYGO_UI_VERSION}")
if(LILYGO_UI_VERSION MATCHES "^([0-9]+:)?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(CPACK_PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_2}")
    set(CPACK_PACKAGE_VERSION_MINOR "${CMAKE_MATCH_3}")
    set(CPACK_PACKAGE_VERSION_PATCH "${CMAKE_MATCH_4}")
endif()
set(CPACK_PACKAGE_CONTACT "${LILYGO_UI_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_VENDOR "${LILYGO_UI_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${LILYGO_UI_APP_SUMMARY}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${LILYGO_UI_APP_HOMEPAGE}")
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_SET_DESTDIR ON)
set(CPACK_COMPONENTS_ALL app)
set(CPACK_DEBIAN_APP_PACKAGE_NAME "${LILYGO_UI_PACKAGE_NAME}")
set(CPACK_DEBIAN_APP_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_APP_PACKAGE_DEPENDS
    "libc6, libstdc++6, libdrm2, libfreetype6, libasound2t64 | libasound2, lilygo-ui-appkit-dev (>= ${LILYGO_UI_MIN_APPKIT_VERSION})")
set(CPACK_DEBIAN_APP_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${LILYGO_UI_PACKAGE_ARCHITECTURE}")
endif()
include(CPack)
