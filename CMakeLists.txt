project(dfcapture_public)

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
    src/notifications.cpp
    src/placement.cpp
    src/sdl_capture.cpp
    src/stockpile_panel.cpp
    src/unit_sheet.cpp
    src/web_assets.cpp
    LINK_LIBRARIES gdiplus ole32 ws2_32)

target_include_directories(dfcapture_public PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../dfplex-src/server/cpp-httplib")
