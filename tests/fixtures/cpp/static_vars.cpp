namespace demo {

struct Base {
  int base_id;
};

struct Item {
  int value;
};

struct Derived : Base {
  static int shared;
  Item item;
  int values[2];
};

int Derived::shared = 88;

Derived global_object{{1}, {77}, {3, 4}};
Derived object_array[2] = {{{2}, {11}, {5, 6}}, {{3}, {12}, {7, 8}}};

int use_object(int factor) {
  int local_value = factor + global_object.item.value;
  return local_value + Derived::shared;
}

}  // namespace demo

extern "C" int main() {
  return demo::use_object(3);
}
