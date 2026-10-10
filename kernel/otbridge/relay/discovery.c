extern long otr_sys_write(int, const unsigned char *, unsigned long);
void *GetOTauxtcpInstallInfo(void)
{
    static const unsigned char text[] = "otrelay I dependency-free getter tcp\n";
    (void)otr_sys_write(1, text, sizeof(text) - 1);
    return 0;
}
void *GetOTauxudpInstallInfo(void)
{
    static const unsigned char text[] = "otrelay I dependency-free getter udp\n";
    (void)otr_sys_write(1, text, sizeof(text) - 1);
    return 0;
}
