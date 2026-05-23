struct Inner {
  int value;
};

struct GlobalStruct {
  int id;
  struct Inner inner;
};

int global_value = 42;
int global_array[2] = {1, 2};
int matrix[2][3] = {{1, 2, 3}, {4, 5, 6}};
static int file_static_value = 9;
const int const_value = 7;
volatile int volatile_value = 8;
struct GlobalStruct global_struct = {3, {99}};

int add_values(int lhs, int rhs) {
  static int function_static_value = 5;
  int local_sum = lhs + rhs;
  int local_buffer[2] = {lhs, rhs};
  function_static_value += local_buffer[0];
  return local_sum + function_static_value;
}

int main(void) {
  return add_values(global_value, file_static_value);
}
