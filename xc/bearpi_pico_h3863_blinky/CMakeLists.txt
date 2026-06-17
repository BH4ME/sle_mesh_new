# BearPi Pico H3863 blinky sample
if(DEFINED CONFIG_BLINKY_USE_LITEOS)
    set(SOURCES
        "${SOURCES}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/blinky_liteos.c"
        PARENT_SCOPE
    )
else()
    set(SOURCES
        "${SOURCES}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/blinky_cmsis.c"
        PARENT_SCOPE
    )
endif()
