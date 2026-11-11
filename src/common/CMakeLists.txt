# tn_common -- the infrastructure layer
#
# Depends on no higher-level module, nor on libusb / network syscalls. Its only
# external dependency is libusb's headers (error.cc needs libusb_error_name to
# translate error codes), so it is linked PRIVATEly and libusb is not propagated
# to tn_common's consumers.

add_library(
  tn_common STATIC
  error.cc
  i18n.cc
  logging.cc
  scheduling.cc)

target_link_libraries(tn_common PUBLIC tethernet_base)
target_link_libraries(tn_common PRIVATE LibUSB::LibUSB)
