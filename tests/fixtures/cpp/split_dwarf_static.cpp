struct S {
  int value;
};

static S global_value{42};

int main() {
  static S local{7};
  return global_value.value + local.value;
}
