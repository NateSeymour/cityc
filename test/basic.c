int add(int a, int b)
{
    return a + b;
}

int __entry()
{
    int x = 5;
    int y = 10;

    int subtotal = add(x, y);

    int z = 4;
    int a = subtotal + x + y + z;

    return add(add(a, a), 43 - 2);
}