static int global_seed = 42;

__attribute__((noinline)) int use_loclist(int seed) {
  int local = global_seed + seed;
  if (seed > 0) {
    local += 3;
  }
  return local;
}

extern "C" int main() {
  return use_loclist(1);
}
