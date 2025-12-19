project(dfcapture_public)

dfhack_plugin(dfcapture_public
    src/dfcapture.cpp
    src/http_server.cpp
    src/web_assets.cpp
    LINK_LIBRARIES ws2_32)

target_include_directories(dfcapture_public PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../dfplex-src/server/cpp-httplib")
