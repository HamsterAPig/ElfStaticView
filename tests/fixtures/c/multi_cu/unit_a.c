static int shared_value = 11;
static int unit_private_value = 101;
int shared_global = 303;

int read_unit_a_value(void)
{
    static int shared_counter = 5;
    shared_counter += 1;
    return shared_value + unit_private_value + shared_counter + shared_global;
}

int read_shared_global(void)
{
    return shared_global;
}
