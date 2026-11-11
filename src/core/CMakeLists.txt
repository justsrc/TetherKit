# tn_core -- data-path bridging and runtime orchestration
#
# Wires tn_usb's RNDIS data channel to tn_net's link backend and manages the
# data-path threads. It is the only layer that depends on all three of
# usb / net / rndis at once.

add_library(
  tn_core STATIC
  bridge.cc
  runtime.cc)

target_link_libraries(tn_core PUBLIC tethernet_base tn_common tn_rndis tn_net tn_usb)
