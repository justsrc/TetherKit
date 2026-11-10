// Unit tests for error.h / logging.h.
#include <cerrno>
#include <string>

#include <doctest.h>

#include "tetherkit/common/error.h"
#include "tetherkit/common/logging.h"

using tetherkit::Error;
using tetherkit::ErrorDomain;
using tetherkit::LogLevel;
using tetherkit::Ok;
using tetherkit::Result;
using tetherkit::Status;

namespace {

Status FailingStep() {
  return std::unexpected(Error::FromErrno(ENOENT, "failed to open /dev/bpf0"));
}

Status SucceedingStep() {
  return Ok();
}

/// Demonstrates the TETHERKIT_RETURN_IF_ERROR propagation chain.
Status PipelineWithFailure() {
  TETHERKIT_RETURN_IF_ERROR(SucceedingStep());
  TETHERKIT_RETURN_IF_ERROR(FailingStep());
  return Ok();
}

Result<int> ProducingStep(bool succeed) {
  if (!succeed) {
    return std::unexpected(Error::Generic("production failed"));
  }
  return 42;
}

Result<int> ConsumingPipeline(bool succeed) {
  TETHERKIT_ASSIGN_OR_RETURN(const int value, ProducingStep(succeed));
  return value * 2;
}

}  // namespace

TEST_SUITE("common.error") {
  TEST_CASE("errno domain renders symbol name and numeric value") {
    const Error error = Error::FromErrno(EACCES, "failed to open BPF device");
    CHECK(error.Domain() == ErrorDomain::kErrno);
    CHECK(error.Code() == EACCES);
    const std::string text = error.ToString();
    CHECK(text.find("failed to open BPF device") != std::string::npos);
    CHECK(text.find("errno") != std::string::npos);
    CHECK(text.find(std::to_string(EACCES)) != std::string::npos);
  }

  TEST_CASE("passing 0 as errno reads the global errno automatically") {
    errno = EPERM;
    const Error error = Error::FromErrno(0, "failed to create feth");
    CHECK(error.Code() == EPERM);
  }

  TEST_CASE("libusb domain renders the libusb error name") {
    // LIBUSB_ERROR_ACCESS == -3 is the most common failure in this project (not running as root).
    const Error error = Error::FromLibUsb(-3, "failed to claim RNDIS data interface");
    CHECK(error.Domain() == ErrorDomain::kLibUsb);
    const std::string text = error.ToString();
    CHECK(text.find("LIBUSB_ERROR_ACCESS") != std::string::npos);
  }

  TEST_CASE("RNDIS domain outputs the raw status code value") {
    // tk_common deliberately does not know the symbolic names of RNDIS status codes -- that
    // table lives in only one place, rndis/protocol.cc (single source, avoids inconsistencies).
    // Therefore Error::ToString() here only outputs the hexadecimal value; the symbolic name is
    // appended to the context string by the rndis layer when constructing the Error (see
    // MakeStatusError in rndis/messages.cc).
    const Error error = Error::FromRndisStatus(0xC00000BBU, "failed to query OID");
    const std::string text = error.ToString();
    CHECK(text.find("failed to query OID") != std::string::npos);
    CHECK(text.find("0xc00000bb") != std::string::npos);
    CHECK(error.Domain() == ErrorDomain::kRndis);
    CHECK(error.Code() == 0xC00000BB);
  }

  TEST_CASE("WithContext forms an outer-to-inner cause chain") {
    Error inner = Error::FromErrno(ENODEV, "ioctl(BIOCSETIF) failed");
    const Error outer = std::move(inner).WithContext("failed to attach BPF to feth1");
    const std::string text = outer.ToString();
    const auto outer_pos = text.find("failed to attach BPF to feth1");
    const auto inner_pos = text.find("ioctl(BIOCSETIF) failed");
    REQUIRE(outer_pos != std::string::npos);
    REQUIRE(inner_pos != std::string::npos);
    CHECK(outer_pos < inner_pos);  // the outer cause comes first
  }

  TEST_CASE("RETURN_IF_ERROR propagates the first failure") {
    const Status status = PipelineWithFailure();
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().Code() == ENOENT);
  }

  TEST_CASE("ASSIGN_OR_RETURN takes the value on success and propagates on failure") {
    const Result<int> good = ConsumingPipeline(true);
    REQUIRE(good.has_value());
    CHECK(good.value() == 84);

    const Result<int> bad = ConsumingPipeline(false);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().Context() == "production failed");
  }

  TEST_CASE("log level setting and checking") {
    const LogLevel original = tetherkit::GetLogLevel();

    tetherkit::SetLogLevel(LogLevel::kWarn);
    CHECK(tetherkit::GetLogLevel() == LogLevel::kWarn);
    CHECK_FALSE(tetherkit::detail::IsLogLevelEnabled(LogLevel::kInfo));
    CHECK(tetherkit::detail::IsLogLevelEnabled(LogLevel::kWarn));
    CHECK(tetherkit::detail::IsLogLevelEnabled(LogLevel::kError));

    tetherkit::SetLogLevel(LogLevel::kOff);
    CHECK_FALSE(tetherkit::detail::IsLogLevelEnabled(LogLevel::kError));

    tetherkit::SetLogLevel(original);
  }

  TEST_CASE("extract the file name from __FILE__") {
    CHECK(tetherkit::detail::BaseName("/a/b/c/bpf_link.cc") == "bpf_link.cc");
    CHECK(tetherkit::detail::BaseName("bpf_link.cc") == "bpf_link.cc");
    CHECK(tetherkit::detail::BaseName("") == "");
  }

}  // TEST_SUITE("common.error")
