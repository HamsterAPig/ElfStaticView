static int storage = 7;

struct Bits {
    unsigned char low;
    unsigned char high;
};

int use_bits(int seed)
{
    Bits bits{};
    bits.low = 1;
    bits.high = static_cast<unsigned char>(seed & 1);
    return bits.low + bits.high + storage;
}

extern "C" int main()
{
    return use_bits(3);
}
