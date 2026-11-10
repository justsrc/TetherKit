# tk_rndis -- the RNDIS protocol layer
#
# Pure-logic module: only wire-format codec work and state-machine stepping,
# **touches no I/O at all**. This constraint is deliberate -- it lets the entire
# RNDIS implementation be fully covered by unit tests on machines without a real
# device (this project's dev machine has neither a USB device nor root).

add_library(
  tk_rndis STATIC
  protocol.cc
  messages.cc
  state_machine.cc)

target_link_libraries(tk_rndis PUBLIC tetherkit_base tk_common)
