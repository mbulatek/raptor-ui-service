cmake_minimum_required(VERSION 3.21)

project(raptor-native-ui VERSION 0.1.0 LANGUAGES C CXX)

option(RAPTOR_UI_BUILD_SERVICE "Build the UI background service" ON)
option(RAPTOR_UI_BUILD_SPLASH "Build the splash-screen helper" ON)
option(RAPTOR_UI_INSTALL_SYSTEMD_UNIT "Install the systemd service file" ON)
option(RAPTOR_UI_INSTALL_DEFAULT_CONFIG "Install the default YAML configuration file" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

pkg_check_modules(LIBZMQ REQUIRED IMPORTED_TARGET libzmq)
pkg_check_modules(LIBGPIOD REQUIRED IMPORTED_TARGET libgpiod)

find_library(MATH_LIBRARY NAMES m)

set(WAVESHARE_INCLUDE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/include/waveshare")
set(WAVESHARE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src/waveshare")

add_library(waveshare_vendor STATIC
    ${WAVESHARE_SOURCE_ROOT}/Config/raptor_waveshare_dev_config.c
    src/waveshare_shim.c
    ${WAVESHARE_SOURCE_ROOT}/GUI/GUI_Paint.c
    ${WAVESHARE_SOURCE_ROOT}/GUI/GUI_BMPfile.c
    ${WAVESHARE_SOURCE_ROOT}/Fonts/font8.c
    ${WAVESHARE_SOURCE_ROOT}/Fonts/font12.c
    ${WAVESHARE_SOURCE_ROOT}/Fonts/font16.c
    ${WAVESHARE_SOURCE_ROOT}/Fonts/font20.c
    ${WAVESHARE_SOURCE_ROOT}/Fonts/font24.c
    ${WAVESHARE_SOURCE_ROOT}/OLED/OLED_0in91.c
    ${WAVESHARE_SOURCE_ROOT}/OLED/OLED_1in3.c
    ${WAVESHARE_SOURCE_ROOT}/OLED/OLED_1in32.c
    ${WAVESHARE_SOURCE_ROOT}/OLED/OLED_1in5.c
    ${WAVESHARE_SOURCE_ROOT}/OLED/OLED_1in5_b.c
    ${WAVESHARE_SOURCE_ROOT}/OLED/OLED_1in51.c
    ${WAVESHARE_SOURCE_ROOT}/OLED/OLED_1in54.c
    ${WAVESHARE_SOURCE_ROOT}/OLED/OLED_2in42.c
)

target_include_directories(waveshare_vendor
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${WAVESHARE_INCLUDE_ROOT}/Config
        ${WAVESHARE_INCLUDE_ROOT}/Fonts
        ${WAVESHARE_INCLUDE_ROOT}/GUI
        ${WAVESHARE_INCLUDE_ROOT}/OLED
)

target_compile_definitions(waveshare_vendor PRIVATE USE_DEV_LIB=1 DEBUG=0)

target_link_libraries(waveshare_vendor PUBLIC PkgConfig::LIBGPIOD)
if(MATH_LIBRARY)
    target_link_libraries(waveshare_vendor PUBLIC ${MATH_LIBRARY})
endif()

add_library(raptor_ui_core
    src/config.cpp
    src/display_backend.cpp
    src/logger.cpp
    src/page_controller.cpp
    src/ui_runtime.cpp
    src/view.cpp
)

target_include_directories(raptor_ui_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_options(raptor_ui_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)

target_link_libraries(raptor_ui_core
    PUBLIC
        Threads::Threads
        spdlog::spdlog
        yaml-cpp
        waveshare_vendor
)

add_library(raptor_ui_service_core
    src/application.cpp
    src/control_server.cpp
    src/ipc.cpp
)

target_include_directories(raptor_ui_service_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_options(raptor_ui_service_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)

target_link_libraries(raptor_ui_service_core
    PUBLIC
        raptor_ui_core
        nlohmann_json::nlohmann_json
        PkgConfig::LIBZMQ
)

if(RAPTOR_UI_BUILD_SERVICE)
    add_executable(raptor-native-ui src/main.cpp)
    target_link_libraries(raptor-native-ui PRIVATE raptor_ui_service_core)
endif()

if(RAPTOR_UI_BUILD_SPLASH)
    add_executable(raptor-native-splash-screen src/splash_main.cpp)
    target_link_libraries(raptor-native-splash-screen PRIVATE raptor_ui_core)
endif()

install(TARGETS waveshare_vendor raptor_ui_core raptor_ui_service_core
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(RAPTOR_UI_BUILD_SERVICE)
    install(TARGETS raptor-native-ui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(RAPTOR_UI_BUILD_SPLASH)
    install(TARGETS raptor-native-splash-screen RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(RAPTOR_UI_INSTALL_DEFAULT_CONFIG)
    install(FILES config/raptor-native-ui.example.yaml
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/raptor-native-ui
        RENAME raptor-native-ui.yaml
    )
endif()

if(RAPTOR_UI_INSTALL_SYSTEMD_UNIT)
    install(FILES systemd/raptor-native-ui.service DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
endif()
