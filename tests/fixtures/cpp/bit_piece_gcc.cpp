struct PackedBits {
    unsigned char low : 1;
    unsigned char mid : 2;
    unsigned char high : 5;
};

int consume(unsigned char seed)
{
    PackedBits bits{};
    bits.low = seed & 0x1;
    bits.mid = (seed >> 1) & 0x3;
    bits.high = (seed >> 3) & 0x1f;
    return bits.low + bits.mid + bits.high;
}

extern "C" int main()
{
    return consume(13);
}
