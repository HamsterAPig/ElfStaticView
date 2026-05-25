thread_local int tls_value = 7;
int plain_value = 3;

extern "C" int main() {
  return tls_value + plain_value;
}
