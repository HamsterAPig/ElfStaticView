struct FancyName {
    int value;
};

static FancyName global_name = {42};

int use_name()
{
    FancyName local = global_name;
    return local.value;
}

extern "C" int main()
{
    return use_name();
}
