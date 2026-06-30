namespace demo {

using NullType = decltype(nullptr);
NullType global_null = nullptr;

int use()
{
    return global_null == nullptr ? 1 : 0;
}

} // namespace demo

extern "C" int main()
{
    return demo::use();
}
