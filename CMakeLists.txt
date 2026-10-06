cmake_minimum_required(VERSION 3.25)
project(Island VERSION 1.2.4 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Island requires Windows 10 1809 or newer")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INSTALL_BINDIR ".")
set(CMAKE_INSTALL_LIBDIR ".")
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Test)

add_library(island_core STATIC
    native/AppAssets.cpp native/AppAssets.h resources/island_assets.qrc
    native/AnimationClock.cpp native/AnimationClock.h
    native/ConfigStore.cpp native/ConfigStore.h
    native/MediaBridge.cpp native/MediaBridge.h
    native/SessionVolume.cpp native/SessionVolume.h
    native/WindowsIntegration.cpp native/WindowsIntegration.h
    native/UpdateService.cpp native/UpdateService.h
    native/ReleaseNotes.cpp native/ReleaseNotes.h resources/release_notes.qrc
    native/Layout.cpp native/Layout.h
    native/Squircle.cpp native/Squircle.h
    native/HudIcons.cpp native/HudIcons.h
    native/IslandMenu.cpp native/IslandMenu.h
    native/IslandToolTip.cpp native/IslandToolTip.h
    native/IslandDialogs.cpp native/IslandDialogs.h
    native/SmoothScroll.cpp native/SmoothScroll.h
    native/HudWindow.cpp native/HudWindow.h
    native/SettingsControls.cpp native/SettingsControls.h
    native/SettingsWindow.cpp native/SettingsWindow.h
)
target_include_directories(island_core PUBLIC native)
target_compile_definitions(island_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(island_core PUBLIC Qt6::Widgets Qt6::Network windowsapp CoreMessaging dwmapi gdi32 ole32 user32 shell32 advapi32)
if(MSVC)
    target_compile_options(island_core PUBLIC /utf-8 /W4 /permissive- /EHsc)
endif()
qt_add_executable(Island WIN32 native/main.cpp native/Application.cpp native/Application.h resources/island.rc resources/island.manifest)
target_link_libraries(Island PRIVATE island_core)
set_target_properties(Island PROPERTIES OUTPUT_NAME Island)

include(CTest)
option(ISLAND_DESKTOP_TESTS "Build compositor checks requiring an interactive Windows desktop" OFF)
if(BUILD_TESTING)
    foreach(test_name config layout hud platform media animation assets settings settings_controls settings_workflows session_volume release_notes)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/native/tests/test_${test_name}.cpp")
            qt_add_executable(test_${test_name} native/tests/test_${test_name}.cpp)
            set_target_properties(test_${test_name} PROPERTIES WIN32_EXECUTABLE FALSE)
            target_link_libraries(test_${test_name} PRIVATE island_core Qt6::Test)
            add_test(NAME ${test_name} COMMAND test_${test_name} -o "${CMAKE_CURRENT_BINARY_DIR}/test_${test_name}.txt,txt")
            set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        endif()
    endforeach()
    if(ISLAND_DESKTOP_TESTS)
        qt_add_executable(test_desktop native/tests/test_desktop.cpp)
        set_target_properties(test_desktop PROPERTIES WIN32_EXECUTABLE FALSE)
        target_link_libraries(test_desktop PRIVATE island_core Qt6::Test)
        add_test(NAME desktop_composition COMMAND test_desktop)
        set_tests_properties(desktop_composition PROPERTIES LABELS desktop RUN_SERIAL TRUE TIMEOUT 60)
    endif()
endif()

install(TARGETS Island RUNTIME DESTINATION .)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
include(InstallRequiredSystemLibraries)
qt_generate_deploy_app_script(TARGET Island OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
install(DIRECTORY licenses/ DESTINATION licenses)
set(CPACK_PACKAGE_NAME "Island")
set(CPACK_PACKAGE_VENDOR "scarrymany")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "SCARP ISLAND - Windows music overlay")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Island")
set(CPACK_PACKAGE_FILE_NAME "Island-Setup")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_NSIS_DISPLAY_NAME "SCARP ISLAND")
set(CPACK_NSIS_PACKAGE_NAME "SCARP ISLAND")
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
set(CPACK_NSIS_INSTALLED_ICON_NAME "Island.exe")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_INSTALL_ROOT "$LOCALAPPDATA")
set(CPACK_NSIS_MUI_FINISHPAGE_RUN "Island.exe")
set(CPACK_PACKAGE_EXECUTABLES "Island" "SCARP ISLAND")
set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/scarrymany/Island")
include(cmake/IslandInstaller.cmake)
include(CPack)
