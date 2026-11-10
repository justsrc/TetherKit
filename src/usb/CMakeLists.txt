# tk_usb -- USB transport layer
#
# Wraps libusb: device discovery and identification, interface claiming,
# control channel, interrupt notifications, and the asynchronous bulk transfer
# pool. It depends on tk_rndis only for protocol constants (interface
# signatures, control request codes, notification format) -- not the state
# machine.

add_library(
  tk_usb STATIC
  context.cc
  device.cc
  data_channel.cc)

target_link_libraries(tk_usb PUBLIC tetherkit_base tk_common tk_rndis)
target_link_libraries(tk_usb PUBLIC LibUSB::LibUSB)
