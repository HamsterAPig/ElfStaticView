extern int read_unit_a_value(void);
extern int read_unit_b_value(void);

int main(void) {
  return read_unit_a_value() + read_unit_b_value();
}
