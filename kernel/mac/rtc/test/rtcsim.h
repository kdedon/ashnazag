/* Host build of rtc.c: VIA1 port B goes to the RTC model in rtctest.c. */
#define VRD(r)		sim_rd(r)
#define VWR(r, v)	sim_wr((r), (v))
#define printf		sim_printf
#define hrestime	sim_hrestime
#define __amix_stime	sim_stime_orig
extern int sim_rd();
extern void sim_wr();
