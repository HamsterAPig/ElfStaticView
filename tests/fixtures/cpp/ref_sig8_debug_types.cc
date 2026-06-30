struct FlagBits {
    unsigned enabled : 1;
    unsigned mode : 3;
    unsigned reserved : 4;
};

union FlagValue {
    unsigned all;
    FlagBits bits;
};

struct RefTarget {
    int left;
    int right;
    FlagValue flags[2];
};

static RefTarget global_value = {1, 2, {{0x05}, {0x0a}}};

int consume()
{
    RefTarget local = global_value;
    return local.left + local.right + static_cast<int>(local.flags[0].all);
}

extern "C" int main()
{
    return consume();
}
