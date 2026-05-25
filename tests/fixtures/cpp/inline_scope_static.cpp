static int file_static = 1;

__attribute__((always_inline)) inline int helper(int seed) {
  static int inline_static = 7;
  if (seed > 0) {
    static int block_static = 11;
    inline_static += block_static;
  }
  return inline_static + file_static;
}

int call_helper(int value) {
  int local_value = helper(value);
  return local_value;
}

extern "C" int main() {
  return call_helper(3);
}
