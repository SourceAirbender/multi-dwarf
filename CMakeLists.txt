project(dfcapture_public)

dfhack_plugin(dfcapture_public
    src/client_state.cpp
    src/dfcapture.cpp
    src/diagnostics.cpp
    src/hud.cpp
    src/http_server.cpp
    src/image_encoder.cpp
    src/json_util.cpp
    src/notifications.cpp
    src/placement.cpp
    src/sdl_capture.cpp
    src/web_assets.cpp
    LINK_LIBRARIES gdiplus ole32 ws2_32)

target_include_directories(dfcapture_public PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../dfplex-src/server/cpp-httplib")
