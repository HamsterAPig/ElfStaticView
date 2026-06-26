struct RefTarget {
    int left;
    int right;
};

static RefTarget global_value = {1, 2};

int consume()
{
    RefTarget local = global_value;
    return local.left + local.right;
}

extern "C" int main()
{
    return consume();
}
