/* libdla.so: a library t_dlopen links against */
int dla_data = 7;

int
dla_func(int x)
{
	return x + dla_data;
}

int
dla_dup(void)
{
	return 1;
}
