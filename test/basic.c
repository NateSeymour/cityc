int constant()
{
    return 34;
}

void other_routine()
{
    //int x = 34 + 2;
    return;
}

int __entry()
{
    int a = 1;
    int b = 2;
    int c = 3;
    int d = 4;
    int e = 5;
    int f = 6;

    constant();

    int subtotal = a + b - c;

    other_routine();

    int total = subtotal + d - e + f;

    return total;
}