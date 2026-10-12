#ifndef NATIVE_CALL_FIXTURE_H
#define NATIVE_CALL_FIXTURE_H
#define NCF_ATTACH (('N' << 8) | 1)
#define NCF_DETACH (('N' << 8) | 2)
#define NCF_STATS (('N' << 8) | 3)
#define NCF_VALUE 0x12345678L
struct ncf_stats { unsigned long private_traps, private_intr, bad_intr, bad_signal, private_vur, forks, exits; };
#endif
