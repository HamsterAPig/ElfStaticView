struct SupTarget {
  int value;
};

static SupTarget sup_value{42};

extern "C" int main() {
  return sup_value.value;
}
