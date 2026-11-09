idf_component_register(    SRCS
        "app_main.cpp"
        "domain/Payload.cpp"
        "domain/Config.cpp"
        "domain/DiagPayload.cpp"
        "drivers/I2CBus.cpp"
        "drivers/Bme280.cpp"
        "drivers/Opt3001.cpp"
        "drivers/Lis3dh.cpp"
        "drivers/Pa1010d.cpp"
        "drivers/BoardPower.cpp"
        "drivers/minmea/minmea.c"
        "app/App.cpp"
        "app/Sleep.cpp"
        "app/Stores.cpp"
        "app/History.cpp"
        "app/Buffer.cpp"
        "app/Ota.cpp"
        "app/P2pGateway.cpp"
        "domain/P2pFrame.cpp"
        "domain/P2pSchedule.cpp"
        "transports/lorawan/LoRaWanTransport.cpp"
        "transports/ble/BleTransport.cpp"
        "transports/wifi/WifiTransport.cpp"
        "transports/p2p/P2pRadio.cpp"
        "transports/zigbee/ZigbeeTransport.cpp"
    INCLUDE_DIRS "." "drivers" "app" "domain" "transports"
    # NOTE: no REQUIRES — the main component implicitly depends on all
    # components; listing them here would REMOVE the implicit deps.
)

# C++20 for the whole application component
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++2b" APPEND)

# minmea: silence warnings from vendored third-party code
set_source_files_properties("drivers/minmea/minmea.c" PROPERTIES
    COMPILE_OPTIONS "-Wno-implicit-fallthrough;-Wno-sign-compare;-Wno-type-limits")
