static int shared_value = 22;
static int unit_private_value = 202;
extern int shared_global;

int read_unit_b_value(void) {
  static int shared_counter = 7;
  shared_counter += 2;
  return shared_value + unit_private_value + shared_counter + shared_global;
}
