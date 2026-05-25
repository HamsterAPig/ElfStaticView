namespace demo {

struct NestedLeaf {
  int leaf;
  int guard;
};

struct Item {
  int index;
  NestedLeaf nested;
};

struct GlobalObject {
  Item items[2];
  int tail;
};

GlobalObject global_object{{{1, {11, 101}}, {2, {22, 202}}}, 99};

int use_object() {
  return global_object.items[0].nested.leaf + global_object.items[1].nested.leaf +
         global_object.tail;
}

}  // namespace demo

extern "C" int main() {
  return demo::use_object();
}
