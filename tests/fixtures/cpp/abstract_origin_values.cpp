static const int const_inline = 5;

__attribute__((always_inline)) inline int helper(int seed)
{
    static int inline_state = 9;
    const int folded = const_inline + seed;
    return inline_state + folded;
}

int call_helper(int value)
{
    return helper(value);
}

extern "C" int main()
{
    return call_helper(3);
}
