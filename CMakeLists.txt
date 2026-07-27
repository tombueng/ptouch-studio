cmake_minimum_required(VERSION 3.21)

project(ptouch-studio
    VERSION 0.1.0
    DESCRIPTION "Design and print labels for Brother P-touch tape cassettes"
    HOMEPAGE_URL "https://github.com/tombueng/ptouch-studio"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
include(GNUInstallDirs)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets)

set(APP_ID "io.github.tombueng.PtouchStudio")
configure_file(src/Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/Version.h @ONLY)

# ---------------------------------------------------------------- library
# Core without any interface dependency, so tests and the CLI can share it.
add_library(ptouch-core STATIC
    src/Engine.cpp
    src/Status.cpp
    src/Config.cpp
    src/Printer.cpp
    src/Provision.cpp)
target_include_directories(ptouch-core PUBLIC src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(ptouch-core PUBLIC Qt6::Core Qt6::Gui)

# ---------------------------------------------------------------- application
add_executable(ptouch-studio
    src/main.cpp
    src/MainWindow.cpp
    src/PreviewWidget.cpp
    src/SetupDialog.cpp
    src/Cli.cpp)
target_link_libraries(ptouch-studio PRIVATE ptouch-core Qt6::Widgets)

# ---------------------------------------------------------------- CUPS backend
# Own backend because neither the serial nor the file backend of CUPS can open an
# RFCOMM port (non-blocking open, missing modem control lines).
add_executable(cups-backend-rfcomm src/CupsBackend.cpp)
set_target_properties(cups-backend-rfcomm PROPERTIES OUTPUT_NAME rfcomm)

# ---------------------------------------------------------------- tests
include(CTest)
if(BUILD_TESTING)
    add_executable(ptouch-tests tests/EngineTests.cpp)
    target_link_libraries(ptouch-tests PRIVATE ptouch-core Qt6::Gui)
    add_test(NAME engine COMMAND ptouch-tests)
    set_tests_properties(engine PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# ---------------------------------------------------------------- installation
set(CUPS_BACKEND_DIR "${CMAKE_INSTALL_LIBDIR}/cups/backend"
    CACHE STRING "directory holding the CUPS backends")

install(TARGETS ptouch-studio RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS cups-backend-rfcomm RUNTIME DESTINATION ${CUPS_BACKEND_DIR}
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

install(FILES data/${APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES data/${APP_ID}.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)
install(FILES data/${APP_ID}.policy
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/polkit-1/actions)
install(FILES data/icons/${APP_ID}.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
foreach(size 16 22 24 32 48 64 128 256 512)
    install(FILES data/icons/${size}/${APP_ID}.png
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${size}x${size}/apps
            OPTIONAL)
endforeach()
install(FILES data/man/ptouch-studio.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 OPTIONAL)

# ---------------------------------------------------------------- packaging
set(CPACK_PACKAGE_NAME "ptouch-studio")
set(CPACK_PACKAGE_VENDOR "tombueng")
set(CPACK_PACKAGE_CONTACT "tombueng@gmail.com")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}_${PROJECT_VERSION}_${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_STRIP_FILES ON)

set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libqt6widgets6 (>= 6.4), libqt6gui6 (>= 6.4), libqt6core6 (>= 6.4), bluez, cups-client, printer-driver-ptouch")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "cups, policykit-1 | polkitd")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Productivity")
set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtbase-gui >= 6.4, bluez, cups, ptouch-driver")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
# do not claim directories that belong to other packages
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    "${CMAKE_INSTALL_FULL_DATAROOTDIR}/metainfo"
    "${CMAKE_INSTALL_FULL_DATAROOTDIR}/polkit-1"
    "${CMAKE_INSTALL_FULL_DATAROOTDIR}/polkit-1/actions"
    "/usr/lib/cups" "/usr/lib/cups/backend")

include(CPack)
