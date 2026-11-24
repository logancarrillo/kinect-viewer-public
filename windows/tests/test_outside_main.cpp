// W8 Phase A (outside the home): the standalone modules' tests, one binary. Synthetic only: a fake router, an
// injected clock and new temp folders; nothing reaches the real router, the network or the real state folder.
#include "kv_test.hpp"

int main(int argc, char** argv) { return kvtest::runAll(argc, argv); }
