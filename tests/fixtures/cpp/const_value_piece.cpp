static const int const_static = 123;
static int global_pair[2] = {1, 2};

struct Pair {
  int left;
  int right;
};

int consume_pair() {
  Pair pair = {global_pair[0], global_pair[1]};
  int sum = pair.left + pair.right;
  return sum + const_static;
}

extern "C" int main() {
  return consume_pair();
}
