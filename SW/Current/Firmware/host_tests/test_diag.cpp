// Diagnostics uplink codec tests.

#include "testfw.hpp"

#include "domain/DiagPayload.hpp"

static void test_encode_layout()
{
    minilora::Diagnostics d{};
    d.fwMajor = 2;
    d.fwMinor = 2;
    d.fwPatch = 0;
    d.bootCount = 1234;
    d.unexpectedResets = 3;
    d.lastResetReason = 5;
    d.gpsTimeouts = 65535;
    d.loraJoinFailures = 7;
    d.loraSendFailures = 8;
    d.wifiFailures = 9;
    d.zigbeeFailures = 10;
    d.sensorReadFailures = 11;
    d.gpsConsecFails = 3;
    d.powerFlags = 0x01;
    d.gpsValid = 1;
    d.imageFlags = minilora::kDiagFlagRecovery;
    d.lastTtff = 12;
    d.linkMargin = 7;
    d.gwCnt = 2;
    d.otaStatus = minilora::kOtaStatusStaged;
    d.configFlags = minilora::kCfgFlagWifi | minilora::kCfgFlagGnss |
                    minilora::kCfgFlagBle;

    // Failure-counter pattern mirrors icd_v3.json "diag_struggling"; the
    // identity/boot fields differ (this test pins the layout, not the vector).
    uint8_t out[minilora::kDiagPayloadSize] = {};
    CHECK_EQ(minilora::encodeDiagnostics(d, out), minilora::kDiagPayloadSize);
    CHECK_EQ(out[0], 0x05);                 // schema byte (ICD §2)
    CHECK_EQ(out[1], 2);                    // fw 2.2.0
    CHECK_EQ(out[2], 2);
    CHECK_EQ(out[3], 0);
    CHECK_EQ(out[4], 1234 & 0xFF);          // bootCount LE
    CHECK_EQ(out[5], (1234 >> 8) & 0xFF);
    CHECK_EQ(out[6], 3);                    // unexpectedResets
    CHECK_EQ(out[7], 0);
    CHECK_EQ(out[8], 5);                    // reset reason
    CHECK_EQ(out[9], 0xFF);                 // gpsTimeouts 65535
    CHECK_EQ(out[10], 0xFF);
    CHECK_EQ(out[21], 3);                   // gpsConsecFails
    CHECK_EQ(out[22], 0x01);                // powerFlags: degraded
    CHECK_EQ(out[23], 0x05);                // flags2: gpsValid | recovery
    CHECK_EQ(out[24], 12);                  // lastTtff
    CHECK_EQ(out[25], 7);                   // linkMargin
    CHECK_EQ(out[26], 2);                   // gwCnt
    CHECK_EQ(out[27], minilora::kOtaStatusStaged);  // otaStatus
    CHECK_EQ(out[28], 0x0D);                // configFlags: WiFi|BLE|GNSS (v5)
}

void run_diag_tests()
{
    RUN(test_encode_layout);
}
