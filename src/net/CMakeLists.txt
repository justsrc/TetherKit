# tn_net -- virtual interface and link layer
#
# Only depends on macOS syscalls (ioctl / BPF); neither libusb nor the RNDIS
# protocol layer. LinkBackend is the module's public abstraction; BpfLink is the
# production implementation and LoopbackLink is a pure-memory implementation --
# the latter lets the bridge layer's end-to-end tests and throughput benchmarks
# run on machines without root and without a real interface.

add_library(
  tn_net STATIC
  feth_device.cc
  bpf_link.cc
  loopback_link.cc)

target_link_libraries(tn_net PUBLIC tethernet_base tn_common)
