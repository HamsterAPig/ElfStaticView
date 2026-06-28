struct Box {
    static int value;
};

int Box::value = 123;

extern "C" int main()
{
    return Box::value;
}
