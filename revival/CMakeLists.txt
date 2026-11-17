cmake_minimum_required(VERSION 3.21)
project(yagf_revival VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets)
qt_standard_project_setup()
set(CMAKE_AUTORCC ON)

add_library(yagf_core STATIC
    src/core/ocrdocument.cpp src/core/ocrdocument.h src/core/preprocesssettings.h
    src/core/projectfile.cpp src/core/projectfile.h)
target_link_libraries(yagf_core PUBLIC Qt6::Core Qt6::Gui)
target_include_directories(yagf_core PUBLIC src/core)

add_library(yagf_processing STATIC
    src/processing/documentprocessor.cpp src/processing/documentprocessor.h
    src/processing/imageprocessor.cpp src/processing/imageprocessor.h)
target_link_libraries(yagf_processing PUBLIC yagf_core Qt6::Core Qt6::Gui)
target_include_directories(yagf_processing PUBLIC src/processing)

qt_add_executable(yagf-revival
    src/main.cpp src/ui/mainwindow.cpp src/ui/mainwindow.h
    src/ui/preprocessdialog.cpp src/ui/preprocessdialog.h
    src/ui/layoutdialog.cpp src/ui/layoutdialog.h resources/icons.qrc)
target_link_libraries(yagf-revival PRIVATE yagf_core yagf_processing Qt6::Widgets)

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(ocrdocument_tests tests/ocrdocument_tests.cpp)
    target_link_libraries(ocrdocument_tests PRIVATE yagf_core)
    add_test(NAME ocrdocument_tests COMMAND ocrdocument_tests)
    qt_add_executable(projectfile_tests tests/projectfile_tests.cpp)
    target_link_libraries(projectfile_tests PRIVATE yagf_core)
    add_test(NAME projectfile_tests COMMAND projectfile_tests)
    qt_add_executable(imageprocessor_tests tests/imageprocessor_tests.cpp)
    target_link_libraries(imageprocessor_tests PRIVATE yagf_processing)
    add_test(NAME imageprocessor_tests COMMAND imageprocessor_tests)
endif()
