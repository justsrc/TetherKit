// Unit tests for the C ABI layer.
//
// Covers cases that "do not crash immediately when wrong, but show up as garbled text or
// security holes in the GUI": UTF-8 boundary truncation, interface-name validation, and
// the log ring buffer's drop semantics.
//
// Session and network configuration require root and a real device, so they are not tested
// here -- only their argument-validation branches.
#include "tethernet/capi/tethernet_c.h"
#include "tethernet/common/logging.h"

#include <doctest.h>

#include <array>
#include <cstring>
#include <string>

#include "capi_support.h"
#include "process_runner.h"

using tethernet::capi::CopyText;
using tethernet::capi::IsValidFethName;

TEST_SUITE("capi.support") {
  TEST_CASE("CopyText copies normally and adds a terminator") {
    std::array<char, 16> buffer{};
    CopyText(buffer.data(), buffer.size(), "feth0");
    CHECK(std::string{buffer.data()} == "feth0");
  }

  TEST_CASE("CopyText writes nothing when the capacity is 0 or the destination is null") {
    CopyText(nullptr, 16, "x");  // No crash means it passed.
    std::array<char, 4> buffer{'a', 'b', 'c', 'd'};
    CopyText(buffer.data(), 0, "xyz");
    CHECK(buffer[0] == 'a');
  }

  TEST_CASE("CopyText truncates on a UTF-8 character boundary") {
    // "€€" = 6 bytes (3 bytes per character). With a 6-byte capacity (5 usable bytes +
    // terminator), only the first character fits; a hard byte cut would leave half of the
    // second "€", and the whole string would become a replacement character on the Swift side.
    std::array<char, 6> buffer{};
    CopyText(buffer.data(), buffer.size(), "€€");
    CHECK(std::string{buffer.data()} == "€");
    CHECK(std::strlen(buffer.data()) == 3);
  }

  TEST_CASE("CopyText does not truncate when it fits exactly") {
    std::array<char, 7> buffer{};
    CopyText(buffer.data(), buffer.size(), "€€");
    CHECK(std::string{buffer.data()} == "€€");
  }

  TEST_CASE(
      "CopyText yields an empty string rather than half a character when the first does not fit") {
    std::array<char, 3> buffer{};  // 2 usable bytes, not enough for the 3-byte "€"
    CopyText(buffer.data(), buffer.size(), "€€");
    CHECK(std::string{buffer.data()}.empty());
  }

  TEST_CASE("IsValidFethName accepts only feth plus digits") {
    CHECK(IsValidFethName("feth0"));
    CHECK(IsValidFethName("feth12"));

    SUBCASE("rejects real physical interfaces to avoid overwriting the user's Wi-Fi config") {
      CHECK_FALSE(IsValidFethName("en0"));
      CHECK_FALSE(IsValidFethName("bridge0"));
    }
    SUBCASE("rejects a missing index and non-digit suffixes") {
      CHECK_FALSE(IsValidFethName("feth"));
      CHECK_FALSE(IsValidFethName("fethX"));
      CHECK_FALSE(IsValidFethName("feth0a"));
    }
    SUBCASE("rejects shell metacharacters and path separators") {
      CHECK_FALSE(IsValidFethName("feth0; rm -rf /"));
      CHECK_FALSE(IsValidFethName("../feth0"));
      CHECK_FALSE(IsValidFethName(""));
    }
    SUBCASE("rejects names longer than IFNAMSIZ") {
      CHECK_FALSE(IsValidFethName("feth123456789012345"));
    }
  }

namespace {

using tethernet::capi::DeviceIdentity;
using tethernet::capi::ReconcileDeviceStrings;
using tethernet::capi::RememberedDeviceStrings;

/// Build an enumeration result with identity and strings filled in. Pass an empty string
/// to mean "not read this time".
tn_device_info_t DeviceInfo(const DeviceIdentity& identity, const char* manufacturer,
                            const char* product, const char* serial) {
  tn_device_info_t info{};
  info.vendor_id = identity.vendor_id;
  info.product_id = identity.product_id;
  info.bus_number = identity.bus_number;
  info.device_address = identity.device_address;
  CopyText(info.manufacturer, manufacturer);
  CopyText(info.product, product);
  CopyText(info.serial, serial);
  return info;
}

}  // namespace

TEST_CASE("ReconcileDeviceStrings backfills the last successfully read name when unreadable") {
  // The scenario is that real GUI defect: after connecting, the device is claimed exclusively
  // and its strings cannot be read, so "vivo iQOO Z10x" degrades to "USB device 2d95:600b".
  const DeviceIdentity phone{.bus_number = 0, .device_address = 1,
                             .vendor_id = 0x2d95, .product_id = 0x600b};
  std::vector<RememberedDeviceStrings> memory;
  const std::vector<DeviceIdentity> present{phone};

  // First time: read while idle -> remember it.
  std::vector<tn_device_info_t> infos{DeviceInfo(phone, "vivo", "iQOO Z10x", "10AFAC2X72005KT")};
  ReconcileDeviceStrings(memory, present, infos);
  REQUIRE(memory.size() == 1);

  // Second time: session starts, reads are skipped (all empty) -> backfill.
  infos = {DeviceInfo(phone, "", "", "")};
  ReconcileDeviceStrings(memory, present, infos);
  CHECK(std::string{infos[0].manufacturer} == "vivo");
  CHECK(std::string{infos[0].product} == "iQOO Z10x");
  CHECK(std::string{infos[0].serial} == "10AFAC2X72005KT");

  SUBCASE("overwrites the memory when a new value is read again") {
    infos = {DeviceInfo(phone, "vivo", "iQOO Z10x", "NEWSERIAL")};
    ReconcileDeviceStrings(memory, present, infos);
    infos = {DeviceInfo(phone, "", "", "")};
    ReconcileDeviceStrings(memory, present, infos);
    CHECK(std::string{infos[0].serial} == "NEWSERIAL");
  }
}

TEST_CASE(
    "ReconcileDeviceStrings does not give the old name to another device at the same address") {
  const DeviceIdentity old_phone{.bus_number = 0, .device_address = 1,
                                 .vendor_id = 0x2d95, .product_id = 0x600b};
  std::vector<RememberedDeviceStrings> memory;
  std::vector<tn_device_info_t> infos{DeviceInfo(old_phone, "vivo", "iQOO Z10x", "SN1")};
  ReconcileDeviceStrings(memory, {std::vector<DeviceIdentity>{old_phone}}, infos);
  REQUIRE(memory.size() == 1);

  SUBCASE("same address but different VID:PID -> different identity, no backfill") {
    const DeviceIdentity other{.bus_number = 0, .device_address = 1,
                               .vendor_id = 0x18d1, .product_id = 0x4ee4};
    infos = {DeviceInfo(other, "", "", "")};
    ReconcileDeviceStrings(memory, {std::vector<DeviceIdentity>{other}}, infos);
    CHECK(std::string{infos[0].product}.empty());
  }

  SUBCASE(
      "device unplugged (not present) -> memory cleared; no backfill if unreadable after replug") {
    // Unplugged: present is empty.
    infos.clear();
    ReconcileDeviceStrings(memory, {}, infos);
    CHECK(memory.empty());

    // Replugged with the same identity (possibly another device of the same model),
    // unreadable -> better to show VID:PID.
    infos = {DeviceInfo(old_phone, "", "", "")};
    ReconcileDeviceStrings(memory, {std::vector<DeviceIdentity>{old_phone}}, infos);
    CHECK(std::string{infos[0].product}.empty());
  }
}

TEST_CASE("ReconcileDeviceStrings keeps an honest empty string for a device with no strings") {
  const DeviceIdentity mute{.bus_number = 2, .device_address = 7,
                            .vendor_id = 0x1234, .product_id = 0x5678};
  std::vector<RememberedDeviceStrings> memory;
  std::vector<tn_device_info_t> infos{DeviceInfo(mute, "", "", "")};
  ReconcileDeviceStrings(memory, {std::vector<DeviceIdentity>{mute}}, infos);
  // Do not remember (nothing was read) and do not backfill (nothing from nowhere).
  CHECK(memory.empty());
  CHECK(std::string{infos[0].product}.empty());
}

TEST_CASE(
    "ReconcileDeviceStrings does not clear memory for a present device that was not filled in") {
  // When the caller's array capacity is insufficient, present is longer than infos -- the
  // extra entries were simply not filled in, not unplugged.
  const DeviceIdentity first{.bus_number = 0, .device_address = 1,
                             .vendor_id = 0x2d95, .product_id = 0x600b};
  const DeviceIdentity second{.bus_number = 0, .device_address = 2,
                              .vendor_id = 0x18d1, .product_id = 0x4ee4};
  std::vector<RememberedDeviceStrings> memory;
  const std::vector<DeviceIdentity> both{first, second};

  std::vector<tn_device_info_t> infos{DeviceInfo(first, "vivo", "iQOO Z10x", "SN1"),
                                      DeviceInfo(second, "Google", "Pixel", "SN2")};
  ReconcileDeviceStrings(memory, both, infos);
  REQUIRE(memory.size() == 2);

  // Capacity drops to 1: only the first device is filled in, the second is still present.
  infos = {DeviceInfo(first, "", "", "")};
  ReconcileDeviceStrings(memory, both, infos);
  CHECK(memory.size() == 2);

  // When the second device is filled in again later and unreadable, its memory is still
  // there and can be backfilled.
  infos = {DeviceInfo(second, "", "", "")};
  ReconcileDeviceStrings(memory, both, infos);
  CHECK(std::string{infos[0].product} == "Pixel");
}

}  // TEST_SUITE("capi.support")

TEST_SUITE("capi.basics") {
  TEST_CASE("tn_version fills non-empty version strings") {
    tn_version_info_t version{};
    tn_version(&version);
    CHECK(std::strlen(version.text) > 0);
    CHECK(std::strlen(version.build) > 0);
    CHECK(std::strlen(version.libusb) > 0);
    CHECK(version.major + version.minor + version.patch > 0);

    SUBCASE("passing nullptr is a no-op and does not crash") {
      tn_version(nullptr);
    }
  }

  TEST_CASE("tn_check_environment always succeeds and expresses conclusions in fields only") {
    tn_environment_t environment{};
    CHECK(tn_check_environment(&environment) == TN_OK);
    // The test process is not root; this is certain.
    CHECK_FALSE(environment.is_root);
    // When sysctls are OK, there should be no explanatory text.
    if (environment.sysctls_ok) {
      CHECK(std::strlen(environment.sysctl_detail) == 0);
    }

    SUBCASE("passing nullptr returns invalid argument") {
      CHECK(tn_check_environment(nullptr) == TN_ERR_INVALID_ARGUMENT);
    }
  }

  TEST_CASE("tn_list_devices can count only") {
    std::size_t count = 0;
    tn_error_t error{};
    // The development machine usually has no RNDIS device inserted; only verify that the call
    // succeeds and does not write out of bounds, not the specific count.
    const tn_result_t result = tn_list_devices(nullptr, 0, &count, false, &error);
    CHECK(result == TN_OK);
    CHECK(std::strlen(error.message) == 0);

    SUBCASE("missing out_count is rejected") {
      CHECK(tn_list_devices(nullptr, 0, nullptr, false, nullptr) == TN_ERR_INVALID_ARGUMENT);
    }
  }

  TEST_CASE("repeated enumeration does not reinitialize libusb") {
    // The GUI refreshes the device list periodically. If each enumeration created a new libusb
    // context, it would start and stop an event thread and an IOKit runloop thread each time,
    // and logs would be flooded with "libusb initialized". This happened in reality; this test
    // pins down the fix.
    tn_enable_log_capture(true);
    const tethernet::LogLevel saved_level = tethernet::GetLogLevel();
    tethernet::SetLogLevel(tethernet::LogLevel::kInfo);

    std::size_t count = 0;
    std::array<tn_log_record_t, 64> records{};
    std::uint64_t dropped = 0;

    // Enumerate once and drain logs to ensure the shared context is already established (the
    // first initialization is expected).
    tn_list_devices(nullptr, 0, &count, false, nullptr);
    while (tn_drain_logs(records.data(), records.size(), &dropped) > 0) {
    }

    constexpr int kRepeats = 3;
    for (int i = 0; i < kRepeats; ++i) {
      CHECK(tn_list_devices(nullptr, 0, &count, false, nullptr) == TN_OK);
    }

    std::size_t initialization_lines = 0;
    std::size_t taken = 0;
    while ((taken = tn_drain_logs(records.data(), records.size(), &dropped)) > 0) {
      for (std::size_t i = 0; i < taken; ++i) {
        if (std::string{records[i].message}.find("libusb initialized") != std::string::npos) {
          ++initialization_lines;
        }
      }
    }
    CHECK(initialization_lines == 0);

    tethernet::SetLogLevel(saved_level);
    tn_enable_log_capture(false);
  }

}  // TEST_SUITE("capi.basics")

TEST_SUITE("capi.log_ring") {
  TEST_CASE("no records are produced when log capture is off") {
    tn_enable_log_capture(false);
    TETHERNET_ERROR("this must not be captured");

    std::array<tn_log_record_t, 4> records{};
    std::uint64_t dropped = 0;
    CHECK(tn_drain_logs(records.data(), records.size(), &dropped) == 0);
  }

  TEST_CASE("logs can be retrieved in order after capture is enabled") {
    tn_enable_log_capture(true);
    const tethernet::LogLevel saved_level = tethernet::GetLogLevel();
    tethernet::SetLogLevel(tethernet::LogLevel::kInfo);

    TETHERNET_INFO("first");
    TETHERNET_WARN("second");

    std::array<tn_log_record_t, 8> records{};
    std::uint64_t dropped = 0;
    const std::size_t taken = tn_drain_logs(records.data(), records.size(), &dropped);

    CHECK(taken == 2);
    CHECK(dropped == 0);
    CHECK(std::string{records[0].message} == "first");
    CHECK(records[0].level == TN_LOG_INFO);
    CHECK(std::string{records[1].message} == "second");
    CHECK(records[1].level == TN_LOG_WARN);
    CHECK(records[0].wall_nanos > 0);

    SUBCASE("draining again after empty returns 0") {
      CHECK(tn_drain_logs(records.data(), records.size(), &dropped) == 0);
    }

    tethernet::SetLogLevel(saved_level);
    tn_enable_log_capture(false);
  }

  TEST_CASE("drops the oldest entries and reports the drop count when the buffer is full") {
    tn_enable_log_capture(true);
    const tethernet::LogLevel saved_level = tethernet::GetLogLevel();
    tethernet::SetLogLevel(tethernet::LogLevel::kInfo);

    // Capacity is 256 (see log_ring.cc); log 10 extra entries to push the oldest out.
    constexpr int kOverflow = 10;
    constexpr int kTotal = 256 + kOverflow;
    for (int i = 0; i < kTotal; ++i) {
      TETHERNET_INFO("entry {}", i);
    }

    std::array<tn_log_record_t, 512> records{};
    std::uint64_t dropped = 0;
    const std::size_t taken = tn_drain_logs(records.data(), records.size(), &dropped);

    CHECK(taken == 256);
    CHECK(dropped == kOverflow);
    // The remaining entries should be the newest 256, so the first one is entry 10.
    CHECK(std::string{records[0].message} == "entry 10");
    CHECK(std::string{records[taken - 1].message} == "entry 265");

    SUBCASE("the drop count resets to zero after being drained") {
      std::uint64_t again = 1;
      tn_drain_logs(records.data(), records.size(), &again);
      CHECK(again == 0);
    }

    tethernet::SetLogLevel(saved_level);
    tn_enable_log_capture(false);
  }

}  // TEST_SUITE("capi.log_ring")

TEST_SUITE("capi.session") {
  TEST_CASE("tn_session_config_init fills usable defaults") {
    tn_session_config_t config{};
    tn_session_config_init(&config);

    CHECK(config.mtu == 1500);
    CHECK(config.adopt_device_mac);
    CHECK(config.rx_transfer_count > 0);
    CHECK(config.tx_transfer_count > 0);
    CHECK(config.rx_transfer_kib > 0);
    CHECK(config.max_transfer_kib > 0);
    CHECK(config.bpf_buffer_kib > 0);
    // Device filtering is unrestricted by default, otherwise the GUI would find no device
    // on startup.
    CHECK(config.vendor_id == 0);
    CHECK(config.product_id == 0);
  }

  TEST_CASE("session can be created, queried, and destroyed (no root required)") {
    tn_session_config_t config{};
    tn_session_config_init(&config);

    tn_error_t error{};
    tn_session_t* session = tn_session_create(&config, &error);
    REQUIRE(session != nullptr);
    CHECK(std::strlen(error.message) == 0);

    SUBCASE("IDLE before start, with all fields cleanly zeroed") {
      tn_session_status_t status{};
      REQUIRE(tn_session_status_get(session, &status) == TN_OK);
      CHECK(status.run_state == TN_RUN_IDLE);
      CHECK(status.rndis_state == TN_RNDIS_UNINITIALIZED);
      CHECK_FALSE(status.link_up);
      CHECK(std::strlen(status.system_interface) == 0);
      CHECK(std::strlen(status.fatal) == 0);
      CHECK(status.rx_frames == 0);
      CHECK(status.monotonic_nanos > 0);
    }

    SUBCASE("event queue is empty before start") {
      std::array<tn_event_t, 8> events{};
      CHECK(tn_session_poll_events(session, events.data(), events.size()) == 0);
    }

    SUBCASE("starting as non-root returns TN_ERR_PERMISSION rather than a generic failure") {
      // The GUI relies on this code to distinguish "prompt for authorization" from "a real
      // error occurred".
      CHECK(tn_session_start(session, &error) == TN_ERR_PERMISSION);
      CHECK(std::strlen(error.message) > 0);
    }

    SUBCASE("repeated stop is idempotent") {
      CHECK(tn_session_stop(session) == TN_OK);
      CHECK(tn_session_stop(session) == TN_OK);
    }

    tn_session_destroy(session);
  }

  TEST_CASE("session API is safe for null pointers") {
    tn_error_t error{};
    CHECK(tn_session_create(nullptr, &error) == nullptr);
    CHECK(std::strlen(error.message) > 0);

    CHECK(tn_session_start(nullptr, nullptr) == TN_ERR_INVALID_ARGUMENT);
    CHECK(tn_session_stop(nullptr) == TN_ERR_INVALID_ARGUMENT);
    CHECK(tn_session_status_get(nullptr, nullptr) == TN_ERR_INVALID_ARGUMENT);
    CHECK(tn_session_poll_events(nullptr, nullptr, 4) == 0);
    tn_session_destroy(nullptr);  // No crash means it passed.
    tn_session_config_init(nullptr);
  }

}  // TEST_SUITE("capi.session")

TEST_SUITE("capi.process") {
  TEST_CASE("RunTool collects child output and carries the exit code") {
    const auto result = tethernet::capi::RunTool("/bin/echo", {"hello", "world"});
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 0);
    CHECK(result->Succeeded());
    CHECK(result->output == "hello world\n");
  }

  TEST_CASE("RunTool merges stderr, and a non-zero exit is not treated as a call failure") {
    // sh -c 'echo boom >&2; exit 3': verify stderr is also collected and the exit code is
    // returned as-is.
    const auto result = tethernet::capi::RunTool("/bin/sh", {"-c", "echo boom >&2; exit 3"});
    REQUIRE(result.has_value());
    CHECK_FALSE(result->Succeeded());
    CHECK(result->exit_code == 3);
    CHECK(result->output == "boom\n");
  }

  TEST_CASE("RunTool can read output larger than the pipe buffer (no deadlock)") {
    // The pipe buffer is 64 KiB. We must drain it before waitpid, otherwise the child blocks
    // once the pipe is full while we wait in waitpid and both sides deadlock. This test pins
    // down that order.
    const auto result = tethernet::capi::RunTool(
        "/bin/sh", {"-c", "for i in $(seq 1 20000); do echo 0123456789; done"});
    REQUIRE(result.has_value());
    CHECK(result->exit_code == 0);
    CHECK(result->output.size() == 20000 * 11);
  }

  TEST_CASE("RunTool returns an error rather than crashing for a nonexistent executable") {
    const auto result = tethernet::capi::RunTool("/nonexistent/tethernet-test", {});
    CHECK_FALSE(result.has_value());
  }

}  // TEST_SUITE("capi.process")

TEST_SUITE("capi.net_config") {
  TEST_CASE("tn_ip_config_init defaults to DHCP and does not take over the global default route") {
    tn_ip_config_t config{};
    tn_ip_config_init(&config);
    CHECK(config.mode == TN_IP_MODE_DHCP);
    CHECK_FALSE(config.set_default_route);
    CHECK(config.dns_count == 0);
    CHECK(std::strlen(config.address) == 0);
  }

  TEST_CASE("rejects operating on non-feth interfaces") {
    tn_ip_config_t config{};
    tn_ip_config_init(&config);
    tn_error_t error{};

    // This is the most important guardrail in this module: passing en0 by mistake would
    // overwrite the user's Wi-Fi configuration.
    CHECK(tn_net_apply("en0", &config, &error) == TN_ERR_INVALID_ARGUMENT);
    CHECK(std::string{error.message}.find("en0") != std::string::npos);

    CHECK(tn_net_clear("en0", &error) == TN_ERR_INVALID_ARGUMENT);
    CHECK(tn_net_query("en0", nullptr, &error) == TN_ERR_INVALID_ARGUMENT);

    tn_net_state_t state{};
    CHECK(tn_net_query("bridge0", &state, &error) == TN_ERR_INVALID_ARGUMENT);
    CHECK(tn_net_apply(nullptr, &config, &error) == TN_ERR_INVALID_ARGUMENT);
    CHECK(tn_net_apply("feth0", nullptr, &error) == TN_ERR_INVALID_ARGUMENT);
  }

  TEST_CASE("write operations return TN_ERR_PERMISSION when not root") {
    tn_ip_config_t config{};
    tn_ip_config_init(&config);
    tn_error_t error{};

    CHECK(tn_net_apply("feth9", &config, &error) == TN_ERR_PERMISSION);
    CHECK(std::strlen(error.message) > 0);
    CHECK(tn_net_clear("feth9", &error) == TN_ERR_PERMISSION);

    std::size_t removed = 1;
    CHECK(tn_cleanup_orphan_interfaces(&removed, &error) == TN_ERR_PERMISSION);
    CHECK(removed == 0);
  }

  TEST_CASE("querying a nonexistent feth interface succeeds and is empty rather than failing") {
    // The GUI also refreshes network status before the session is up, when the interface does
    // not exist yet. That case must be "no address" rather than "query failed", otherwise the
    // UI would show a spurious error.
    tn_net_state_t state{};
    tn_error_t error{};
    CHECK(tn_net_query("feth99", &state, &error) == TN_OK);
    CHECK_FALSE(state.has_address);
    CHECK(state.dns_count == 0);
    CHECK_FALSE(state.is_primary_default_route);
    CHECK(std::strlen(error.message) == 0);
  }

}  // TEST_SUITE("capi.net_config")
