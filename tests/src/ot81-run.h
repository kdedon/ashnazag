static void
ot81probe(pfx)
char *pfx;
{
	char b[65536], n[96], *q, *end;
	long stop = t_now_ms() + 60000L;
	int f, k, count = 0, done = 0, expected = access("/mac/lib/OTProvProbe", R_OK) == 0 ? 46 : 34;
	int data = access("/mac/lib/OTDataProbe", R_OK) == 0;
	int listen = access("/mac/lib/OTListenProbe", R_OK) == 0;
	int stock = access("/mac/lib/OTStockProbe", R_OK) == 0;
	int udp = -1, status = 0, ready = 0;
	int stockseen[6] = {0,0,0,0,0,0}, i;
	char *stocknames[] = {"stock_configuration", "stock_open", "stock_close",
	    "stock_reopen_configuration", "stock_reopen_open", "stock_reopen_close"};
	pid_t reaped;
	long deadline;
	unsigned char port[3];

	if (stock && expected == 46) expected += 20;
	if (data || listen) {
		expected += (data ? 83 : 0) + (listen ? 46 : 0);
		stop += 120000L;
		t_rearm(240);
	}
	if (data) {
		unlink("/tmp/otudp.ready");
		udp = fork();
		if (udp == 0) {
			execl("/tests/net/otudp", "otudp", (char *)0);
			_exit(127);
		}
		deadline = t_now_ms() + 5000L;
		while (udp > 0 && t_now_ms() < deadline) {
			f = open("/tmp/otudp.ready", O_RDONLY);
			k = f < 0 ? -1 : read(f, (char *)port, sizeof port);
			if (f >= 0) close(f);
			if (k == 2 && port[0] == 0x1b && port[1] == 0x5f) { ready = 1; break; }
			reaped = waitpid(udp, &status, WNOHANG);
			if (reaped == udp || (reaped < 0 && errno == ECHILD)) { udp = -1; break; }
			pause_ms(50L);
		}
		sprintf(n, "%s_udp_ready", pfx);
		t_check(n, ready, "UDP helper did not bind loopback7007");
	}

	b[0] = 0;
	unlink("/tmp/otprobe.out");
	host("key o"); host("key t"); host("key p"); host("key r");
	host("key o"); host("key b"); host("key e"); host("key meta_l+o");
	while (t_now_ms() < stop) {
		f = open("/tmp/otprobe.out", O_RDONLY);
		k = f < 0 ? -1 : read(f, b, sizeof b - 1);
		if (f >= 0) close(f);
		if (k > 0) {
			b[k] = 0;
			if (strstr(b, "otprobe done")) { done = 1; break; }
		}
		pause_ms(500L);
	}
	if (udp > 0) {
		kill(udp, SIGTERM);
		k = t_waitchild(udp, &status, 3);
		sprintf(n, "%s_udp_reaped", pfx);
		t_check(n, k == udp && WIFEXITED(status) && WEXITSTATUS(status) == 0,
		    "UDP helper pid%d wait%d status%#x", udp, k, status);
	}
	if (data) unlink("/tmp/otudp.ready");
	if (data || listen) t_rearm(120);
	sprintf(n, "%s_ot_complete", pfx);
	t_check(n, done, "OT helper did not finish");
	for (q = b; q && *q; q = end ? end + 1 : 0) {
		end = strchr(q, '\n');
		if (!end) break;
		*end = 0;
		if (!strncmp(q, "otprobe I ", 10))
			t_info("ot_probe", "%s", q);
		if (!strncmp(q, "otprobe P ", 10) || !strncmp(q, "otprobe F ", 10)) {
			sprintf(n, "%s_ot_%.60s", pfx, q + 10);
			t_check(n, q[8] == 'P', "%s", q);
			count++;
			for (i = 0; i < 6; i++)
				if (!strcmp(q + 10, stocknames[i])) stockseen[i]++;
		}
	}
	if (stock) for (i = 0; i < 6; i++) {
		sprintf(n, "%s_ot_%s_records", pfx, stocknames[i]);
		t_check(n, stockseen[i] == 2, "%d of 2 records", stockseen[i]);
	}
	sprintf(n, "%s_ot_contracts", pfx);
	t_check(n, count == expected, "%d of %d contracts", count, expected);
}
