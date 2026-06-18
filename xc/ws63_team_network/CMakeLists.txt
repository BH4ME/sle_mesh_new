#===============================================================================
# WS63 SLE team network sample.
# Copy this directory under BearPi SDK application/samples/products/sle_team_network.
#===============================================================================

if(NOT DEFINED SLE_MESH_ROOT)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../include/sle_team_packet.h)
        set(SLE_MESH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
    elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/sle_mesh/include/sle_team_packet.h)
        set(SLE_MESH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/sle_mesh)
    else()
        message(FATAL_ERROR "SLE_MESH_ROOT is not set and sle_mesh protocol sources were not found")
    endif()
endif()

set(SLE_TEAM_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
set(SLE_TEAM_UART_SERVER_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server)
set(SLE_TEAM_UART_CLIENT_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_client)
set(SLE_TEAM_LVGL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl)
set(SLE_TEAM_LV_CONF ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h)

set(SOURCES_LIST
    ${SLE_MESH_ROOT}/src/sle_team_packet.c
    ${SLE_MESH_ROOT}/src/sle_team_nmea.c
    ${SLE_MESH_ROOT}/src/sle_team_node.c
    ${SLE_MESH_ROOT}/src/sle_team_location.c
    ${SLE_MESH_ROOT}/src/sle_team_relay_optimizer.c
    ${SLE_MESH_ROOT}/src/sle_team_cli.c
    ${SLE_MESH_ROOT}/src/sle_team_web_api.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server/sle_uart_server_adv.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_server/sle_uart_server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sle_uart_client/sle_uart_client.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ws63_st7789_display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ws63_ws2812.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ws63_team_status_led.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ws63_team_gps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ws63_team_power.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ws63_team_http.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ws63_team_network_app.c
)

if(EXISTS ${SLE_TEAM_LVGL_ROOT}/lvgl.h)
    if(NOT EXISTS ${SLE_TEAM_LV_CONF})
        message(FATAL_ERROR "LVGL is enabled but ${SLE_TEAM_LV_CONF} is missing")
    endif()
    file(GLOB_RECURSE SLE_TEAM_LVGL_SOURCES
        ${SLE_TEAM_LVGL_ROOT}/src/*.c
    )
    set_source_files_properties(${SLE_TEAM_LVGL_SOURCES}
        PROPERTIES COMPILE_FLAGS "-Wno-error"
    )
    list(APPEND SOURCES_LIST ${SLE_TEAM_LVGL_SOURCES})
    include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${SLE_TEAM_LVGL_ROOT})
    add_definitions(-DLV_CONF_INCLUDE_SIMPLE)
else()
    message(FATAL_ERROR "LVGL source not found under ${SLE_TEAM_LVGL_ROOT}; run git submodule update --init --recursive")
endif()

include_directories(${SLE_TEAM_UART_SERVER_HEADERS} ${SLE_TEAM_UART_CLIENT_HEADERS})

set(SOURCES "${SOURCES}" ${SOURCES_LIST} PARENT_SCOPE)
set(PUBLIC_HEADER
    "${PUBLIC_HEADER}"
    ${SLE_MESH_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SLE_TEAM_UART_SERVER_HEADERS}
    ${SLE_TEAM_UART_CLIENT_HEADERS}
    ${SLE_TEAM_LVGL_ROOT}
    PARENT_SCOPE
)
