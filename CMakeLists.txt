cmake_minimum_required(VERSION 2.8.10 FATAL_ERROR)
project(cpu-info CXX)
option (
     ENABLE_CPU_INFO_LOGS
     "Enable CPU Info logs"
     OFF
)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
include(GNUInstallDirs)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/inc)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

set(DBUS_OBJECT_NAME "/xyz/openbmc_project/inventory/system/processor")
set(DBUS_INTF_NAME "xyz.openbmc_project.Inventory.Item")

add_definitions(-DDBUS_OBJECT_NAME="/${DBUS_OBJECT_NAME}")
add_definitions(-DDBUS_INTF_NAME="${DBUS_INTF_NAME}")
set(SRC_FILES cpu_info.cpp
    main.cpp )
set ( SERVICE_FILES
    xyz.openbmc_project.Inventory.Item.Cpu_info.service )

# import sdbusplus
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDBUSPLUSPLUS sdbusplus REQUIRED)
include_directories(${SDBUSPLUSPLUS_INCLUDE_DIRS})
link_directories(${SDBUSPLUSPLUS_LIBRARY_DIRS})
find_program(SDBUSPLUSPLUS sdbus++)

# import phosphor-logging
find_package(PkgConfig REQUIRED)
pkg_check_modules(LOGGING phosphor-logging REQUIRED)
include_directories(${LOGGING_INCLUDE_DIRS})
link_directories(${LOGGING_LIBRARY_DIRS})

# phosphor-dbus-interfaces
find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUSINTERFACE phosphor-dbus-interfaces REQUIRED)
include_directories(${DBUSINTERFACE_INCLUDE_DIRS})
link_directories(${DBUSINTERFACE_LIBRARY_DIRS})


add_executable(${PROJECT_NAME} ${SRC_FILES})
target_link_libraries(${PROJECT_NAME} ${DBUSINTERFACE_LIBRARIES} )
target_link_libraries(${PROJECT_NAME} "${SDBUSPLUSPLUS_LIBRARIES} -lstdc++fs -lphosphor_dbus")
target_link_libraries(${PROJECT_NAME} -lapml64)
target_link_libraries(${PROJECT_NAME} -li2c -lpthread -lm)
target_link_libraries(${PROJECT_NAME} gpiodcxx)
 
install (TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
target_compile_definitions (
    ${PROJECT_NAME} PRIVATE $<$<BOOL:${ENABLE_POWER_CAPPING_LOG}>: -DENABLE_CPU_INFO_LOGS>
)
install (FILES ${SERVICE_FILES} DESTINATION /lib/systemd/system/)

message(STATUS "Toolchain file defaulted to ......'${CMAKE_INATLL_BINDIR}'")
