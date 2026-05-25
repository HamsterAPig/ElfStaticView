namespace demo {

struct Base {
  int base_id;
};

struct Item {
  int value;
};

using MemberPointer = int Item::*;
MemberPointer global_item_member_ptr = &Item::value;

struct Derived : Base {
  static int shared;
  static int counter;
  Item item;
  int values[2];
};

int Derived::shared = 88;
int Derived::counter = 89;

struct Holder {
  static int shared;
  Item item;
};

int Holder::shared = 144;

Derived global_object{{1}, {77}, {3, 4}};
Derived* global_object_ptr = &global_object;
Derived object_array[2] = {{{2}, {11}, {5, 6}}, {{3}, {12}, {7, 8}}};
Holder holder{{22}};

int use_object(int factor) {
  int local_value = factor + global_object.item.value;
  return local_value + global_object_ptr->base_id + Derived::shared + Derived::counter +
         Holder::shared + holder.item.value;
}

}  // namespace demo

extern "C" int main() {
  return demo::use_object(3);
}
