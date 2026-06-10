// SPDX-License-Identifier: GPL-2.0
/*
 * Memory bandwidth monitoring and allocation library
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Authors:
 *    Sai Praneeth Prakhya <sai.praneeth.prakhya@intel.com>,
 *    Fenghua Yu <fenghua.yu@intel.com>
 */
#include "resctrl.h"

#define UNCORE_IMC		"uncore_imc"
#define READ_FILE_NAME		"events/cas_count_read"
#define DYN_PMU_PATH		"/sys/bus/event_source/devices"
#define SCALE			0.00006103515625
#define MAX_BW_COUNTERS		20
#define MAX_TOKENS		5

/*
 * NVIDIA SoC uncore/SCF (System Coherency Fabric) PMU: the ARM64 analog of the
 * Intel iMC reference counters used to cross-check resctrl MBM. On Grace/Vera
 * the fabric PMU registers generically as "nvidia_uncore_pmu_<n>"; the SCF
 * instances (one per socket) are identified by the PMIIDR product-id field
 * (0x2cf), independent of the device name. The driver does not register named
 * events on this silicon, so the read (LLC refill) and write (LLC writeback)
 * bandwidth events are encoded raw. Each event counts 64-byte cacheline
 * transfers, i.e. the same SCALE as Intel CAS counts. Summing read+write
 * matches resctrl's MBM total bytes (which includes reads and writes).
 */
#define UNCORE_NV_SCF		"nvidia_uncore_pmu"
#define NV_SCF_PRODID		0x2cf
#define NV_SCF_EVENT_READ	0xF1	/* scf_cache_refill */
#define NV_SCF_EVENT_WRITE	0xF3	/* scf_cache_wb */
#define CPU_PKG_ID_PATH		"/sys/devices/system/cpu/cpu%d/topology/physical_package_id"

#define CON_MBM_LOCAL_BYTES_PATH		\
	"%s/%s/mon_data/mon_L3_%02d/mbm_local_bytes"

/*
 * MPAM exposes only total (read+write) bandwidth, under MB_MON, and per the
 * NUMA-node-keyed MB domain rather than the L3 cache id.
 */
#define CON_MBM_TOTAL_BYTES_PATH		\
	"%s/%s/mon_data/mon_MB_%02d/mbm_total_bytes"

/*
 * In MPAM "mbm_event" (ABMC) counter-assignment mode a hardware counter must
 * be explicitly assigned to a group+domain+event before the bandwidth file
 * reads a value; otherwise it returns "Unassigned".
 */
#define MBM_MB_ASSIGN_PATH			\
	"%s/%s/mbm_MB_assignments"

struct membw_read_format {
	__u64 value;         /* The value of the event */
	__u64 time_enabled;  /* if PERF_FORMAT_TOTAL_TIME_ENABLED */
	__u64 time_running;  /* if PERF_FORMAT_TOTAL_TIME_RUNNING */
	__u64 id;            /* if PERF_FORMAT_ID */
};

struct mem_bw_counter {
	__u32 type;
	__u64 event;
	__u64 umask;
	struct perf_event_attr pe;
	struct membw_read_format return_value;
	int fd;
	int cpu;
};

/*
 * mem_bw_backend - Source of the independent reference bandwidth that
 *		    validates resctrl MBM
 * @name:		Human-readable PMU name
 * @detect:		True if this backend's PMU is present on the system
 * @setup_counters:	Populate bw_counters[] / nr_bw_counters with the perf
 *			events measuring memory bandwidth generated from
 *			@bench_cpu. Returns 0 on success, < 0 on failure.
 */
struct mem_bw_backend {
	const char *name;
	bool (*detect)(void);
	int (*setup_counters)(int bench_cpu);
};

static char mbm_total_path[1024];
static int nr_bw_counters;
static struct mem_bw_counter bw_counters[MAX_BW_COUNTERS];
static const struct resctrl_test *current_test;
static const struct mem_bw_backend *mem_bw_backend;

/* MPAM ABMC counter assignment state, see initialize_mem_bw_resctrl(). */
static char mbm_assign_path[1024];
static int mbm_assign_domain = -1;

static void read_mem_bw_initialize_perf_event_attr(int i)
{
	memset(&bw_counters[i].pe, 0,
	       sizeof(struct perf_event_attr));
	bw_counters[i].pe.type = bw_counters[i].type;
	bw_counters[i].pe.size = sizeof(struct perf_event_attr);
	bw_counters[i].pe.disabled = 1;
	bw_counters[i].pe.inherit = 1;
	bw_counters[i].pe.exclude_guest = 0;
	bw_counters[i].pe.config =
		bw_counters[i].umask << 8 |
		bw_counters[i].event;
	bw_counters[i].pe.sample_type = PERF_SAMPLE_IDENTIFIER;
	bw_counters[i].pe.read_format =
		PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
}

static void read_mem_bw_ioctl_perf_event_ioc_reset_enable(int i)
{
	ioctl(bw_counters[i].fd, PERF_EVENT_IOC_RESET, 0);
	ioctl(bw_counters[i].fd, PERF_EVENT_IOC_ENABLE, 0);
}

static void read_mem_bw_ioctl_perf_event_ioc_disable(int i)
{
	ioctl(bw_counters[i].fd, PERF_EVENT_IOC_DISABLE, 0);
}

/*
 * get_read_event_and_umask:	Parse config into event and umask
 * @cas_count_cfg:	Config
 * @count:		iMC number
 */
static void get_read_event_and_umask(char *cas_count_cfg, int count)
{
	char *token[MAX_TOKENS];
	int i = 0;

	token[0] = strtok(cas_count_cfg, "=,");

	for (i = 1; i < MAX_TOKENS; i++)
		token[i] = strtok(NULL, "=,");

	for (i = 0; i < MAX_TOKENS - 1; i++) {
		if (!token[i])
			break;
		if (strcmp(token[i], "event") == 0)
			bw_counters[count].event = strtol(token[i + 1], NULL, 16);
		if (strcmp(token[i], "umask") == 0)
			bw_counters[count].umask = strtol(token[i + 1], NULL, 16);
	}
}

static int open_perf_read_event(int i)
{
	bw_counters[i].fd =
		perf_event_open(&bw_counters[i].pe, -1,
				bw_counters[i].cpu, -1,
				PERF_FLAG_FD_CLOEXEC);

	if (bw_counters[i].fd == -1) {
		fprintf(stderr, "Error opening leader %llx\n",
			bw_counters[i].pe.config);

		return -1;
	}

	return 0;
}

/* Get type and config of an iMC counter's read event. */
static int read_from_imc_dir(char *imc_dir, int count)
{
	char cas_count_cfg[1024], imc_counter_cfg[1024], imc_counter_type[1024];
	FILE *fp;

	/* Get type of iMC counter */
	sprintf(imc_counter_type, "%s%s", imc_dir, "type");
	fp = fopen(imc_counter_type, "r");
	if (!fp) {
		ksft_perror("Failed to open iMC counter type file");

		return -1;
	}
	if (fscanf(fp, "%u", &bw_counters[count].type) <= 0) {
		ksft_perror("Could not get iMC type");
		fclose(fp);

		return -1;
	}
	fclose(fp);

	/* Get read config */
	sprintf(imc_counter_cfg, "%s%s", imc_dir, READ_FILE_NAME);
	fp = fopen(imc_counter_cfg, "r");
	if (!fp) {
		ksft_perror("Failed to open iMC config file");

		return -1;
	}
	if (fscanf(fp, "%1023s", cas_count_cfg) <= 0) {
		ksft_perror("Could not get iMC cas count read");
		fclose(fp);

		return -1;
	}
	fclose(fp);

	get_read_event_and_umask(cas_count_cfg, count);

	return 0;
}

/*
 * A system can have 'n' number of iMC (Integrated Memory Controller)
 * counters, get that 'n'. Discover the properties of the available
 * counters in support of needed performance measurement via perf.
 * For each iMC counter get it's type and config. Also obtain each
 * counter's event and umask for the memory read events that will be
 * measured.
 *
 * Enumerate all these details into an array of structures.
 *
 * Return: >= 0 on success. < 0 on failure.
 */
static int num_of_imcs(void)
{
	char imc_dir[512], *temp;
	unsigned int count = 0;
	struct dirent *ep;
	int ret;
	DIR *dp;

	dp = opendir(DYN_PMU_PATH);
	if (dp) {
		while ((ep = readdir(dp))) {
			temp = strstr(ep->d_name, UNCORE_IMC);
			if (!temp)
				continue;

			/*
			 * imc counters are named as "uncore_imc_<n>", hence
			 * increment the pointer to point to <n>. Note that
			 * sizeof(UNCORE_IMC) would count for null character as
			 * well and hence the last underscore character in
			 * uncore_imc'_' need not be counted.
			 */
			temp = temp + sizeof(UNCORE_IMC);

			/*
			 * Some directories under "DYN_PMU_PATH" could have
			 * names like "uncore_imc_free_running", hence, check if
			 * first character is a numerical digit or not.
			 */
			if (temp[0] >= '0' && temp[0] <= '9') {
				sprintf(imc_dir, "%s/%s/", DYN_PMU_PATH,
					ep->d_name);
				ret = read_from_imc_dir(imc_dir, count);
				if (ret) {
					closedir(dp);

					return ret;
				}
				count++;
			}
		}
		closedir(dp);
		if (count == 0) {
			ksft_print_msg("Unable to find iMC counters\n");

			return -1;
		}
	} else {
		ksft_perror("Unable to open PMU directory");

		return -1;
	}

	return count;
}

/* Read an unsigned value from a PMU device sysfs attribute. */
static int read_pmu_value(const char *pmu, const char *attr, int base,
			  unsigned long *val)
{
	char path[600], buf[64];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/%s/%s", DYN_PMU_PATH, pmu, attr);
	fp = fopen(path, "r");
	if (!fp)
		return -1;
	if (!fgets(buf, sizeof(buf), fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	*val = strtoul(buf, NULL, base);
	return 0;
}

/* physical_package_id (socket) of a logical CPU, < 0 on failure. */
static int cpu_package_id(int cpu)
{
	char path[128];
	int pkg = -1;
	FILE *fp;

	snprintf(path, sizeof(path), CPU_PKG_ID_PATH, cpu);
	fp = fopen(path, "r");
	if (!fp)
		return -1;
	if (fscanf(fp, "%d", &pkg) != 1)
		pkg = -1;
	fclose(fp);
	return pkg;
}

/* True if @pmu is an NVIDIA SCF instance (PMIIDR product id == NV_SCF_PRODID). */
static bool is_nvidia_scf_pmu(const char *pmu)
{
	unsigned long id;

	if (strncmp(pmu, UNCORE_NV_SCF, sizeof(UNCORE_NV_SCF) - 1))
		return false;
	if (read_pmu_value(pmu, "identifier", 16, &id))
		return false;
	return ((id >> 20) & 0xfff) == NV_SCF_PRODID;
}

/* True if any x86 uncore iMC PMU ("uncore_imc_<n>") is present. */
static bool intel_imc_present(void)
{
	struct dirent *ep;
	bool found = false;
	char *temp;
	DIR *dp;

	dp = opendir(DYN_PMU_PATH);
	if (!dp)
		return false;
	while ((ep = readdir(dp))) {
		temp = strstr(ep->d_name, UNCORE_IMC);
		if (!temp)
			continue;
		temp += sizeof(UNCORE_IMC);
		if (temp[0] >= '0' && temp[0] <= '9') {
			found = true;
			break;
		}
	}
	closedir(dp);
	return found;
}

/* True if any NVIDIA SCF PMU instance is present. */
static bool nvidia_scf_present(void)
{
	struct dirent *ep;
	bool found = false;
	DIR *dp;

	dp = opendir(DYN_PMU_PATH);
	if (!dp)
		return false;
	while ((ep = readdir(dp))) {
		if (is_nvidia_scf_pmu(ep->d_name)) {
			found = true;
			break;
		}
	}
	closedir(dp);
	return found;
}

/*
 * nvidia_scf_setup_counters - Configure SCF read+write bandwidth counters
 * @bench_cpu: CPU the benchmark is bound to
 *
 * The SCF PMU is per-socket; pick the instance(s) on the benchmark's socket and
 * configure two raw counters each (LLC refill = reads, LLC writeback = writes).
 * Per-socket so traffic from the throttled MB domain is attributed correctly.
 *
 * Return: 0 on success, < 0 on failure.
 */
static int nvidia_scf_setup_counters(int bench_cpu)
{
	static const __u64 events[] = { NV_SCF_EVENT_READ, NV_SCF_EVENT_WRITE };
	int bench_pkg = cpu_package_id(bench_cpu);
	struct dirent *ep;
	int n = 0;
	DIR *dp;

	if (bench_pkg < 0) {
		ksft_print_msg("Could not determine socket of CPU %d\n", bench_cpu);
		return -1;
	}

	dp = opendir(DYN_PMU_PATH);
	if (!dp) {
		ksft_perror("Unable to open PMU directory");
		return -1;
	}

	while ((ep = readdir(dp))) {
		unsigned long type, pmu_cpu;
		size_t e;

		if (!is_nvidia_scf_pmu(ep->d_name))
			continue;
		/* cpumask holds the representative CPU of this PMU instance. */
		if (read_pmu_value(ep->d_name, "cpumask", 10, &pmu_cpu))
			continue;
		if (cpu_package_id((int)pmu_cpu) != bench_pkg)
			continue;
		if (read_pmu_value(ep->d_name, "type", 10, &type))
			continue;

		for (e = 0; e < ARRAY_SIZE(events) && n < MAX_BW_COUNTERS; e++) {
			memset(&bw_counters[n], 0,
			       sizeof(bw_counters[n]));
			bw_counters[n].type = (__u32)type;
			bw_counters[n].event = events[e];
			bw_counters[n].umask = 0;
			bw_counters[n].cpu = (int)pmu_cpu;
			read_mem_bw_initialize_perf_event_attr(n);
			/* System-wide (pid == -1) CPU event: inherit is invalid. */
			bw_counters[n].pe.inherit = 0;
			n++;
		}
	}
	closedir(dp);

	if (n == 0) {
		ksft_print_msg("No NVIDIA SCF PMU found for socket %d\n", bench_pkg);
		return -1;
	}

	nr_bw_counters = n;
	return 0;
}

/*
 * imc_setup_counters - Configure iMC CAS-count-read counters
 * @bench_cpu: CPU the benchmark is bound to
 *
 * Discovers all iMC PMUs and configures one read-bandwidth counter per iMC,
 * opened on the benchmark CPU (each iMC counts socket-wide regardless).
 *
 * Return: 0 on success, < 0 on failure.
 */
static int imc_setup_counters(int bench_cpu)
{
	int i, count;

	count = num_of_imcs();
	if (count <= 0)
		return -1;

	nr_bw_counters = count;
	for (i = 0; i < nr_bw_counters; i++) {
		read_mem_bw_initialize_perf_event_attr(i);
		bw_counters[i].cpu = bench_cpu;
	}

	return 0;
}

static const struct mem_bw_backend mem_bw_backends[] = {
	{
		.name = "Intel iMC",
		.detect = intel_imc_present,
		.setup_counters = imc_setup_counters,
	},
	{
		.name = "NVIDIA SCF",
		.detect = nvidia_scf_present,
		.setup_counters = nvidia_scf_setup_counters,
	},
};

/*
 * mem_bw_ref_available - Is an independent reference-bandwidth PMU present?
 *
 * Used by the MBA/MBM feature checks so the test is SKIPped (not failed) on
 * hardware lacking a usable memory-bandwidth PMU.
 */
bool mem_bw_ref_available(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mem_bw_backends); i++) {
		if (mem_bw_backends[i].detect())
			return true;
	}

	return false;
}

/*
 * initialize_mem_bw_ref - Select the reference-bandwidth backend for this system
 *
 * Counter discovery happens at perf-open time through the backend's
 * setup_counters() since it can depend on the benchmark CPU.
 *
 * Return: 0 on success, < 0 if no reference PMU is available.
 */
int initialize_mem_bw_ref(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mem_bw_backends); i++) {
		if (mem_bw_backends[i].detect()) {
			mem_bw_backend = &mem_bw_backends[i];
			ksft_print_msg("Using %s as memory bandwidth reference\n",
				       mem_bw_backend->name);
			return 0;
		}
	}

	ksft_print_msg("No memory bandwidth reference PMU found\n");
	return -1;
}

static void perf_close_mem_bw_counters(void)
{
	int i;

	for (i = 0; i < nr_bw_counters; i++) {
		if (bw_counters[i].fd != -1)
			close(bw_counters[i].fd);
	}
}

/*
 * perf_open_mem_bw_counters - Open perf fds for the reference-bandwidth counters
 * @cpu_no: CPU number that the benchmark PID is bound to
 *
 * Counters are (re)configured by the selected backend first since their
 * discovery can depend on the benchmark CPU (e.g. per-socket PMU instances).
 *
 * Return: = 0 on success. < 0 on failure.
 */
static int perf_open_mem_bw_counters(int cpu_no)
{
	int i, ret;

	if (!mem_bw_backend)
		return -1;

	ret = mem_bw_backend->setup_counters(cpu_no);
	if (ret < 0)
		return ret;

	for (i = 0; i < nr_bw_counters; i++)
		bw_counters[i].fd = -1;

	for (i = 0; i < nr_bw_counters; i++) {
		ret = open_perf_read_event(i);
		if (ret)
			goto close_fds;
	}

	return 0;

close_fds:
	perf_close_mem_bw_counters();
	return -1;
}

/*
 * do_mem_bw_test - Perform memory bandwidth test
 *
 * Runs memory bandwidth test over one second period. Also, handles starting
 * and stopping of the reference perf counters around the test.
 */
static void do_mem_bw_test(void)
{
	int i;

	for (i = 0; i < nr_bw_counters; i++)
		read_mem_bw_ioctl_perf_event_ioc_reset_enable(i);

	sleep(1);

	/* Stop counters after a second to get results. */
	for (i = 0; i < nr_bw_counters; i++)
		read_mem_bw_ioctl_perf_event_ioc_disable(i);
}

/*
 * get_mem_bw_ref - Memory bandwidth as reported by the reference PMU counters
 *
 * Sum all configured reference counters, scaled to MiB: read CAS counts on
 * the x86 iMC, LLC refill + writeback counts on the NVIDIA SCF.
 *
 * Return: = 0 on success. < 0 on failure.
 */
static int get_mem_bw_ref(float *bw_ref)
{
	float bw = 0, of_mul = 1;
	int i;

	/*
	 * Log event values from all reference counters into
	 * struct mem_bw_counter.
	 * Take overflow into consideration before calculating total bandwidth.
	 */
	for (i = 0; i < nr_bw_counters; i++) {
		struct mem_bw_counter *r = &bw_counters[i];

		if (read(r->fd, &r->return_value,
			 sizeof(struct membw_read_format)) == -1) {
			ksft_perror("Couldn't read reference bandwidth counter");
			return -1;
		}

		__u64 r_time_enabled = r->return_value.time_enabled;
		__u64 r_time_running = r->return_value.time_running;

		if (r_time_enabled != r_time_running)
			of_mul = (float)r_time_enabled /
					(float)r_time_running;

		bw += r->return_value.value * of_mul * SCALE;
	}

	*bw_ref = bw;
	return 0;
}

/*
 * initialize_mem_bw_resctrl:	Appropriately populate "mbm_total_path"
 * @param:	Parameters passed to resctrl_val()
 * @domain_id:	Domain ID (cache ID; for MB, L3 cache ID)
 *
 * x86 resctrl reports per-RMID local bandwidth under L3_MON. MPAM exposes only
 * total (read+write) bandwidth under MB_MON, indexed by the NUMA-node-keyed MB
 * domain, and in "mbm_event" (ABMC) mode a hardware counter must be assigned to
 * the group+domain before the file reads a value.
 */
void initialize_mem_bw_resctrl(const struct resctrl_val_param *param,
			       int domain_id)
{
	const char *grp = param->ctrlgrp ? param->ctrlgrp : "";

	if (resctrl_mon_feature_exists("L3_MON", "mbm_local_bytes")) {
		sprintf(mbm_total_path, CON_MBM_LOCAL_BYTES_PATH, RESCTRL_PATH,
			grp, domain_id);
		return;
	}

	/* MPAM: total bandwidth under MB_MON. */
	sprintf(mbm_total_path, CON_MBM_TOTAL_BYTES_PATH, RESCTRL_PATH,
		grp, domain_id);

	/* Assign an ABMC counter for this group+domain if in mbm_event mode. */
	snprintf(mbm_assign_path, sizeof(mbm_assign_path), MBM_MB_ASSIGN_PATH,
		 RESCTRL_PATH, grp);
	if (access(mbm_assign_path, W_OK) == 0) {
		FILE *fp = fopen(mbm_assign_path, "w");

		if (fp) {
			/* The assignment parser requires a trailing newline. */
			fprintf(fp, "mbm_total_bytes:%d=e\n", domain_id);
			fclose(fp);
			mbm_assign_domain = domain_id;
		}
	} else {
		/* Not in counter-assignment mode (or no MB_MON); nothing to do. */
		mbm_assign_path[0] = '\0';
	}
}

/*
 * mem_bw_ref_cleanup - Release an assigned ABMC counter and reset backend state
 *
 * Group removal does not reclaim assigned MPAM counters, so the test must
 * unassign explicitly. Safe to call when nothing was assigned.
 */
void mem_bw_ref_cleanup(void)
{
	if (mbm_assign_domain >= 0 && mbm_assign_path[0]) {
		FILE *fp = fopen(mbm_assign_path, "w");

		if (fp) {
			fprintf(fp, "mbm_total_bytes:%d=_\n", mbm_assign_domain);
			fclose(fp);
		}
	}
	mbm_assign_domain = -1;
	mbm_assign_path[0] = '\0';
	mem_bw_backend = NULL;
}

/*
 * Open file to read MBM local bytes from resctrl FS
 */
static FILE *open_mem_bw_resctrl(const char *mbm_bw_file)
{
	FILE *fp;

	fp = fopen(mbm_bw_file, "r");
	if (!fp)
		ksft_perror("Failed to open total memory bandwidth file");

	return fp;
}

/*
 * Get MBM Local bytes as reported by resctrl FS
 */
static int get_mem_bw_resctrl(FILE *fp, unsigned long *mbm_total)
{
	if (fscanf(fp, "%lu\n", mbm_total) <= 0) {
		ksft_perror("Could not get MBM local bytes");
		return -1;
	}
	return 0;
}

static pid_t bm_pid;

void ctrlc_handler(int signum, siginfo_t *info, void *ptr)
{
	/* Only kill child after bm_pid is set after fork() */
	if (bm_pid)
		kill(bm_pid, SIGKILL);
	umount_resctrlfs();
	if (current_test && current_test->cleanup)
		current_test->cleanup();
	ksft_print_msg("Ending\n\n");

	exit(EXIT_SUCCESS);
}

/*
 * Register CTRL-C handler for parent, as it has to kill
 * child process before exiting.
 */
int signal_handler_register(const struct resctrl_test *test)
{
	struct sigaction sigact = {};
	int ret = 0;

	bm_pid = 0;

	current_test = test;
	sigact.sa_sigaction = ctrlc_handler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = SA_SIGINFO;
	if (sigaction(SIGINT, &sigact, NULL) ||
	    sigaction(SIGTERM, &sigact, NULL) ||
	    sigaction(SIGHUP, &sigact, NULL)) {
		ksft_perror("sigaction");
		ret = -1;
	}
	return ret;
}

/*
 * Reset signal handler to SIG_DFL.
 * Non-Value return because the caller should keep
 * the error code of other path even if sigaction fails.
 */
void signal_handler_unregister(void)
{
	struct sigaction sigact = {};

	current_test = NULL;
	sigact.sa_handler = SIG_DFL;
	sigemptyset(&sigact.sa_mask);
	if (sigaction(SIGINT, &sigact, NULL) ||
	    sigaction(SIGTERM, &sigact, NULL) ||
	    sigaction(SIGHUP, &sigact, NULL)) {
		ksft_perror("sigaction");
	}
}

/*
 * print_results_bw:	the memory bandwidth results are stored in a file
 * @filename:		file that stores the results
 * @bm_pid:		child pid that runs benchmark
 * @bw_ref:		reference PMU counter value
 * @bw_resc:		memory bandwidth value
 *
 * Return:		0 on success, < 0 on error.
 */
static int print_results_bw(char *filename, pid_t bm_pid, float bw_ref,
			    unsigned long bw_resc)
{
	unsigned long diff = fabs(bw_ref - bw_resc);
	FILE *fp;

	if (strcmp(filename, "stdio") == 0 || strcmp(filename, "stderr") == 0) {
		printf("Pid: %d \t Mem_BW_ref: %f \t ", (int)bm_pid, bw_ref);
		printf("Mem_BW_resc: %lu \t Difference: %lu\n", bw_resc, diff);
	} else {
		fp = fopen(filename, "a");
		if (!fp) {
			ksft_perror("Cannot open results file");

			return -1;
		}
		if (fprintf(fp,
			    "Pid: %d \t Mem_BW_ref: %f \t Mem_BW_resc: %lu \t Difference: %lu\n",
			    (int)bm_pid, bw_ref, bw_resc, diff) <= 0) {
			ksft_print_msg("Could not log results\n");
			fclose(fp);

			return -1;
		}
		fclose(fp);
	}

	return 0;
}

/*
 * measure_read_mem_bw - Measures read memory bandwidth numbers while benchmark runs
 * @uparams:		User supplied parameters
 * @param:		Parameters passed to resctrl_val()
 * @bm_pid:		PID that runs the benchmark
 *
 * Measure memory bandwidth from resctrl and from the independent reference
 * PMU (iMC on x86, SCF on NVIDIA ARM64). Compare the two values to validate
 * resctrl value. It takes 1 sec to measure the data.
 * resctrl does not distinguish between read and write operations so
 * its data includes all memory operations.
 */
int measure_read_mem_bw(const struct user_params *uparams,
			struct resctrl_val_param *param, pid_t bm_pid)
{
	unsigned long bw_resc, bw_resc_start, bw_resc_end;
	FILE *mem_bw_fp;
	float bw_ref;
	int ret;

	mem_bw_fp = open_mem_bw_resctrl(mbm_total_path);
	if (!mem_bw_fp)
		return -1;

	ret = perf_open_mem_bw_counters(uparams->cpu);
	if (ret < 0)
		goto close_fp;

	ret = get_mem_bw_resctrl(mem_bw_fp, &bw_resc_start);
	if (ret < 0)
		goto close_counters;

	rewind(mem_bw_fp);

	do_mem_bw_test();

	ret = get_mem_bw_resctrl(mem_bw_fp, &bw_resc_end);
	if (ret < 0)
		goto close_counters;

	ret = get_mem_bw_ref(&bw_ref);
	if (ret < 0)
		goto close_counters;

	perf_close_mem_bw_counters();
	fclose(mem_bw_fp);

	bw_resc = (bw_resc_end - bw_resc_start) / MB;

	return print_results_bw(param->filename, bm_pid, bw_ref, bw_resc);

close_counters:
	perf_close_mem_bw_counters();
close_fp:
	fclose(mem_bw_fp);
	return ret;
}

/*
 * resctrl_val:	execute benchmark and measure memory bandwidth on
 *			the benchmark
 * @test:		test information structure
 * @uparams:		user supplied parameters
 * @param:		parameters passed to resctrl_val()
 *
 * Return:		0 when the test was run, < 0 on error.
 */
int resctrl_val(const struct resctrl_test *test,
		const struct user_params *uparams,
		struct resctrl_val_param *param)
{
	unsigned char *buf = NULL;
	cpu_set_t old_affinity;
	int domain_id;
	int ret = 0;
	pid_t ppid;

	if (strcmp(param->filename, "") == 0)
		sprintf(param->filename, "stdio");

	ret = get_domain_id(test->resource, uparams->cpu, &domain_id);
	if (ret < 0) {
		ksft_print_msg("Could not get domain ID\n");
		return ret;
	}

	ppid = getpid();

	/* Taskset test to specified CPU. */
	ret = taskset_benchmark(ppid, uparams->cpu, &old_affinity);
	if (ret)
		return ret;

	/* Write test to specified control & monitoring group in resctrl FS. */
	ret = write_bm_pid_to_resctrl(ppid, param->ctrlgrp, param->mongrp);
	if (ret)
		goto reset_affinity;

	if (param->init) {
		ret = param->init(param, domain_id);
		if (ret)
			goto reset_affinity;
	}

	/*
	 * If not running user provided benchmark, run the default
	 * "fill_buf". First phase of "fill_buf" is to prepare the
	 * buffer that the benchmark will operate on. No measurements
	 * are needed during this phase and prepared memory will be
	 * passed to next part of benchmark via copy-on-write thus
	 * no impact on the benchmark that relies on reading from
	 * memory only.
	 */
	if (param->fill_buf) {
		buf = alloc_buffer(param->fill_buf->buf_size,
				   param->fill_buf->memflush);
		if (!buf) {
			ret = -ENOMEM;
			goto reset_affinity;
		}
	}

	fflush(stdout);
	bm_pid = fork();
	if (bm_pid == -1) {
		ret = -errno;
		ksft_perror("Unable to fork");
		goto free_buf;
	}

	/*
	 * What needs to be measured runs in separate process until
	 * terminated.
	 */
	if (bm_pid == 0) {
		if (param->fill_buf)
			fill_cache_read(buf, param->fill_buf->buf_size, false);
		else if (uparams->benchmark_cmd[0])
			execvp(uparams->benchmark_cmd[0], (char **)uparams->benchmark_cmd);
		exit(EXIT_SUCCESS);
	}

	ksft_print_msg("Benchmark PID: %d\n", (int)bm_pid);

	/* Give benchmark enough time to fully run. */
	sleep(1);

	/* Test runs until the callback setup() tells the test to stop. */
	while (1) {
		ret = param->setup(test, uparams, param);
		if (ret == END_OF_TESTS) {
			ret = 0;
			break;
		}
		if (ret < 0)
			break;

		ret = param->measure(uparams, param, bm_pid);
		if (ret)
			break;
	}

	kill(bm_pid, SIGKILL);
free_buf:
	free(buf);
reset_affinity:
	taskset_restore(ppid, &old_affinity);
	return ret;
}
