#include "test_helper.h"
int main(int argc, char** argv) {
  // GoogleTest removes its own flags before gflags rejects unknown arguments.
  ::testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  if (argc != 1 || !validate_options()) {
    return 2;
  }
  return RUN_ALL_TESTS();
}
