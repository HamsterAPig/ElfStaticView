_Atomic int global_atomic = 7;

int use_atomic(void) {
  return global_atomic;
}

int main(void) {
  return use_atomic();
}
