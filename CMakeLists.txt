project(dfcapture)

dfhack_plugin(dfcapture_public
    src/building_zone.cpp
    src/client_state.cpp
    src/dfcapture.cpp
    src/diagnostics.cpp
    src/hud.cpp
    src/http_server.cpp
    src/info_panel.cpp
    src/interaction.cpp
    src/image_encoder.cpp
    src/json_util.cpp
    src/labor.cpp
    src/lua_bridge.cpp
    src/notifications.cpp
    src/overlay_control.cpp
    src/placement.cpp
    src/sdl_capture.cpp
    src/stockpile_panel.cpp
    src/unit_portrait.cpp
    src/unit_sheet.cpp
    src/web_assets.cpp
    src/work_orders.cpp
    LINK_LIBRARIES gdiplus ole32 ws2_32 lua)

target_include_directories(dfcapture_public PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../dfplex-src/server/cpp-httplib")

set_target_properties(dfcapture_public PROPERTIES OUTPUT_NAME dfcapture)

if(INSTALL_DATA_FILES)
    install(FILES dfcapture.lua
        DESTINATION ${DFHACK_LUA_DESTINATION}/plugins)
endif()
