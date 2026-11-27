if(CONFIG_SHIELD_SOFLE_LEFT OR CONFIG_SHIELD_SOFLE_RIGHT)
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
endif()

# Custom OLED status screen for the Sofle central (left) half.
if(CONFIG_ZMK_DISPLAY AND CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM AND CONFIG_SHIELD_SOFLE_LEFT)
  zephyr_library_sources(sofle_status_screen.c)
  zephyr_library_sources(sofle_sync_central.c)
endif()

# The right half has no display; it only reports its USB power to the left half.
if(CONFIG_SHIELD_SOFLE_RIGHT)
  zephyr_library_sources(sofle_sync_peripheral.c)
endif()
