/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Red Hat, Inc. */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <ftw.h>
#include <getopt.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#ifdef HAVE_PIDFD_OPEN
#include <sys/syscall.h>
#endif
#ifdef HAVE_LIBCAP
#include <sys/capability.h>
#endif
#include <sys/epoll.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>

#include <valgrind/valgrind.h>

#include "spa/utils/ansi.h"
#include "spa/utils/string.h"
#include "spa/utils/defs.h"
#include "spa/utils/list.h"
#include "spa/support/plugin.h"

#include "pipewire/array.h"
#include "pipewire/utils.h"
#include "pipewire/properties.h"

#include "pwtest.h"

#include "pwtest-compat.c"

#define pwtest_log(...) dprintf(testlog_fd, __VA_ARGS__)
#define pwtest_vlog(format_, args_) vdprintf(testlog_fd, format_, args_)

static bool verbose = false;

/** the global context object */
static struct pwtest_context *ctx;

/**
 * The various pwtest_assert() etc. functions write to this fd, collected
 * separately in the log.
 */
static int testlog_fd = STDOUT_FILENO;

enum pwtest_logfds {
	FD_STDOUT,
	FD_STDERR,
	FD_LOG,
	FD_DAEMON,
	_FD_LAST,
};

struct pwtest_test {
	struct spa_list link;
	const char *name;
	enum pwtest_result (*func)(struct pwtest_test *test);

	int iteration;

	/* env vars changed by pwtest. These will be restored after the test
	 * run to get close to the original environment. */
	struct pw_properties *env;

	/* Arguments during pwtest_add() */
	struct {
		int signal;
		struct {
			int min, max;
		} range;
		struct pw_properties *props;
		struct pw_properties *env;
		bool pw_daemon;
	} args;

	/* Results */
	enum pwtest_result result;
	int sig_or_errno;
	struct pw_array logs[_FD_LAST];
};

struct pwtest_suite {
	struct spa_list link;
	const struct pwtest_suite_decl *decl;
	enum pwtest_result result;

	struct spa_list tests;
};

struct pwtest_context {
	struct spa_list suites;
	unsigned int timeout;
	bool no_fork;
	bool terminate;
	struct spa_list cleanup_pids;

	const char *test_filter;
	bool has_iteration_filter;
	int iteration_filter;
	char *xdg_dir;
};

struct cleanup_pid {
	struct spa_list link;
	pid_t pid;
};

struct pwtest_context *pwtest_get_context(struct pwtest_test *t)
{
	return ctx;
}

int pwtest_get_iteration(struct pwtest_test *t)
{
	return t->iteration;
}

struct pw_properties *pwtest_get_props(struct pwtest_test *t)
{
	return t->args.props;
}

static void replace_env(struct pwtest_test *t, const char *prop, const char *value)
{
	const char *oldval = getenv(prop);

	pw_properties_set(t->env, prop, oldval ? oldval : "pwtest-null");
	if (value)
		setenv(prop, value, 1);
	else
		unsetenv(prop);
}

static void restore_env(struct pwtest_test *t)
{
	const char *env;
	void *state = NULL;

	while ((env = pw_properties_iterate(t->env, &state))) {
		const char *value = pw_properties_get(t->env, env);
		if (spa_streq(value, "pwtest-null"))
			unsetenv(env);
		else
			setenv(env, value, 1);
	}
}

static int add_cleanup_pid(struct pwtest_context *ctx, pid_t pid)
{
	struct cleanup_pid *cpid;

	if (pid == 0)
		return -EINVAL;

	cpid = calloc(1, sizeof(struct cleanup_pid));
	if (cpid == NULL)
		return -errno;

	cpid->pid = pid;
	spa_list_append(&ctx->cleanup_pids, &cpid->link);

	return 0;
}

static void remove_cleanup_pid(struct pwtest_context *ctx, pid_t pid)
{
	struct cleanup_pid *cpid, *t;

	spa_list_for_each_safe(cpid, t, &ctx->cleanup_pids, link) {
		if (cpid->pid == pid) {
			spa_list_remove(&cpid->link);
			free(cpid);
		}
	}
}

static void terminate_cleanup_pids(struct pwtest_context *ctx)
{
	struct cleanup_pid *cpid;
	spa_list_for_each(cpid, &ctx->cleanup_pids, link) {
		/* Don't free here, to be signal-safe */
		if (cpid->pid != 0) {
			kill(cpid->pid, SIGTERM);
			cpid->pid = 0;
		}
	}
}

static void free_cleanup_pids(struct pwtest_context *ctx)
{
	struct cleanup_pid *cpid;
	spa_list_consume(cpid, &ctx->cleanup_pids, link) {
		spa_list_remove(&cpid->link);
		free(cpid);
	}
}

static void pwtest_backtrace(pid_t p)
{
#ifdef HAVE_GSTACK
	char pid[11];
	pid_t parent, child;
	int status;

	if (RUNNING_ON_VALGRIND)
		return;

	parent = p == 0 ? getpid() : p;
	child = fork();
	if (child == 0) {
		assert(testlog_fd > 0);
		/* gstack writes the backtrace to stdout, we re-route to our
		 * custom fd */
		dup2(testlog_fd, STDOUT_FILENO);

		spa_scnprintf(pid, sizeof(pid), "%d", (uint32_t)parent);
		execlp("gstack", "gstack", pid, NULL);
		exit(errno);
	}

	/* parent */
	waitpid(child, &status, 0);
#endif
}

SPA_PRINTF_FUNC(6, 7)
SPA_NORETURN
void _pwtest_fail_condition(int exitstatus,
			    const char *file, int line, const char *func,
			    const char *condition, const char *message, ...)
{
	pwtest_log("FAILED: %s\n", condition);

	if (message) {
		va_list args;
		va_start(args, message);
		pwtest_vlog(message, args);
		va_end(args);
		pwtest_log("\n");
	}

	pwtest_log("in %s() (%s:%d)\n", func, file, line);
	pwtest_backtrace(0);
	exit(exitstatus);
}

SPA_NORETURN
void _pwtest_fail_comparison_bool(const char *file, int line, const char *func,
				 const char *operator, bool a, bool b,
				 const char *astr, const char *bstr)
{
	pwtest_log("FAILED COMPARISON: %s %s %s\n", astr, operator, bstr);
	pwtest_log("Resolved to: %s %s %s\n", a ? "true" : "false", operator, b ? "true" : "false");
	pwtest_log("in %s() (%s:%d)\n", func, file, line);
	pwtest_backtrace(0);
	exit(PWTEST_FAIL);
}

SPA_NORETURN
void _pwtest_fail_errno(const char *file, int line, const char *func,
			int expected, int err_no)
{
	pwtest_log("FAILED ERRNO CHECK: expected %d (%s), got %d (%s)\n",
		   expected, strerror(expected), err_no, strerror(err_no));
	pwtest_log("in %s() (%s:%d)\n", func, file, line);
	pwtest_backtrace(0);
	exit(PWTEST_FAIL);
}


SPA_NORETURN
void _pwtest_fail_comparison_int(const char *file, int line, const char *func,
				 const char *operator, int a, int b,
				 const char *astr, const char *bstr)
{
	pwtest_log("FAILED COMPARISON: %s %s %s\n", astr, operator, bstr);
	pwtest_log("Resolved to: %d %s %d\n", a, operator, b);
	pwtest_log("in %s() (%s:%d)\n", func, file, line);
	pwtest_backtrace(0);
	exit(PWTEST_FAIL);
}

SPA_NORETURN
void _pwtest_fail_comparison_double(const char *file, int line, const char *func,
				   const char *operator, double a, double b,
				   const char *astr, const char *bstr)
{
	pwtest_log("FAILED COMPARISON: %s %s %s\n", astr, operator, bstr);
	pwtest_log("Resolved to: %.3f %s %.3f\n", a, operator, b);
	pwtest_log("in %s() (%s:%d)\n", func, file, line);
	pwtest_backtrace(0);
	exit(PWTEST_FAIL);
}

SPA_NORETURN
void _pwtest_fail_comparison_ptr(const char *file, int line, const char *func,
				const char *comparison)
{
	pwtest_log("FAILED COMPARISON: %s\n", comparison);
	pwtest_log("in %s() (%s:%d)\n", func, file, line);
	pwtest_backtrace(0);
	exit(PWTEST_FAIL);
}

SPA_NORETURN
void _pwtest_fail_comparison_str(const char *file, int line, const char *func,
				 const char *comparison, const char *a, const char *b)
{
	pwtest_log("FAILED COMPARISON: %s, expanded (\"%s\" vs \"%s\")\n", comparison, a, b);
	pwtest_log("in %s() (%s:%d)\n", func, file, line);
	pwtest_backtrace(0);
	exit(PWTEST_FAIL);
}

struct pwtest_spa_plugin *
pwtest_spa_plugin_new(void)
{
	return calloc(1, sizeof(struct pwtest_spa_plugin));
}

void
pwtest_spa_plugin_destroy(struct pwtest_spa_plugin *plugin)
{
	SPA_FOR_EACH_ELEMENT_VAR(plugin->handles, hnd) {
		if (*hnd) {
			spa_handle_clear(*hnd);
			free(*hnd);
		}
	}
	SPA_FOR_EACH_ELEMENT_VAR(plugin->dlls, dll) {
		if (*dll)
			dlclose(*dll);
	}
	free(plugin);
}

int
pwtest_spa_plugin_try_load_interface(struct pwtest_spa_plugin *plugin,
				     void **iface_return,
				     const char *libname,
				     const char *factory_name,
				     const char *interface_name,
				     const struct spa_dict *info)
{
	char *libdir = getenv("SPA_PLUGIN_DIR");
	char path[PATH_MAX];
	void *hnd, *iface;
	spa_handle_factory_enum_func_t enum_func;
	const struct spa_handle_factory *factory;
	uint32_t index = 0;
	int r;
	bool found = false;
	struct spa_handle *handle;

	spa_assert_se(libdir != NULL);
	spa_scnprintf(path, sizeof(path), "%s/%s.so", libdir, libname);

        hnd = dlopen(path, RTLD_NOW);
	if (hnd == NULL)
		return -ENOENT;

	enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	pwtest_ptr_notnull(enum_func);

	while ((r = enum_func(&factory, &index)) > 0) {
		pwtest_int_ge(factory->version, 1U);
		if (spa_streq(factory->name, factory_name)) {
			found = true;
			break;
		}
	}
	pwtest_neg_errno_ok(r);
	if (!found) {
		dlclose(hnd);
		return -EINVAL;
	}

	handle = calloc(1, spa_handle_factory_get_size(factory, info));
	pwtest_ptr_notnull(handle);

	r = spa_handle_factory_init(factory, handle, info, plugin->support, plugin->nsupport);
	pwtest_neg_errno_ok(r);
	if ((r = spa_handle_get_interface(handle, interface_name, &iface)) != 0) {
		spa_handle_clear(handle);
		free(handle);
		dlclose(hnd);
		return -ENOSYS;
	}

	plugin->dlls[plugin->ndlls++] = hnd;
	plugin->handles[plugin->nhandles++] = handle;
	plugin->support[plugin->nsupport++] = SPA_SUPPORT_INIT(interface_name, iface);

	*iface_return = iface;
	return 0;
}

void *
pwtest_spa_plugin_load_interface(struct pwtest_spa_plugin *plugin,
				 const char *libname,
				 const char *factory_name,
				 const char *interface_name,
				 const struct spa_dict *info)
{
	void *iface;
	int r = pwtest_spa_plugin_try_load_interface(plugin, &iface, libname,
						     factory_name, interface_name, info);
	pwtest_neg_errno_ok(r);
	return iface;
}

void
pwtest_mkstemp(char path[PATH_MAX])
{
	const char *tmpdir = getenv("TMPDIR");
	int r;

	if (tmpdir == NULL)
		pwtest_error_with_msg("tmpdir is unset");

	spa_scnprintf(path, PATH_MAX, "%s/%s", tmpdir, "tmp.XXXXXX");
	r = mkstemp(path);
	if (r == -1)
		pwtest_error_with_msg("Unable to create temporary file: %s", strerror(errno));
}

int
pwtest_spawn(const char *file, char *const argv[])
{
	int r;
	int status = -1;
	pid_t pid;
	const int fail_code = 121;

	pid = fork();
	if (pid == 0) {
		/* child process */
		execvp(file, (char **)argv);
		exit(fail_code);
	} else if (pid < 0)
		pwtest_error_with_msg("Unable to fork: %s", strerror(errno));

	add_cleanup_pid(ctx, pid);
	r = waitpid(pid, &status, 0);
	remove_cleanup_pid(ctx, pid);
	if (r <= 0)
		pwtest_error_with_msg("waitpid failed: %s", strerror(errno));

	if (WEXITSTATUS(status) == fail_code)
		pwtest_error_with_msg("exec %s failed", file);

	return status;
}

void _pwtest_add(struct pwtest_context *ctx, struct pwtest_suite *suite,
		 const char *funcname, const void *func, ...)
{
	struct pwtest_test *t;
	va_list args;

	if (ctx->test_filter != NULL && fnmatch(ctx->test_filter, funcname, 0) != 0)
		return;

	t = calloc(1, sizeof *t);
	t->result = PWTEST_SYSTEM_ERROR;
	t->name = funcname;
	t->func = func;
	t->args.range.min = 0;
	t->args.range.max = 1;
	t->args.env = pw_properties_new("PWTEST", "1", NULL);
	t->env = pw_properties_new(NULL, NULL);
	for (size_t i = 0; i < SPA_N_ELEMENTS(t->logs); i++)
		pw_array_init(&t->logs[i], 1024);

	va_start(args, func);
	while (true) {
		const char *key, *value;

		enum pwtest_arg arg = va_arg(args, enum pwtest_arg);
		if (!arg)
			break;
		switch (arg) {
		case PWTEST_NOARG:
			break;
		case PWTEST_ARG_SIGNAL:
			if (RUNNING_ON_VALGRIND)
				t->result = PWTEST_SKIP;
			t->args.signal = va_arg(args, int);
			break;
		case PWTEST_ARG_RANGE:
			t->args.range.min = va_arg(args, int);
			t->args.range.max = va_arg(args, int);
			break;
		case PWTEST_ARG_PROP:
			key = va_arg(args, const char *);
			value = va_arg(args, const char *);
			if (t->args.props == NULL) {
				t->args.props = pw_properties_new(key, value, NULL);
			} else {
				pw_properties_set(t->args.props, key, value);
			}
			break;
		case PWTEST_ARG_ENV:
			key = va_arg(args, const char *);
			value = va_arg(args, const char *);
			pw_properties_set(t->args.env, key, value);
			break;
		case PWTEST_ARG_DAEMON:
			if (RUNNING_ON_VALGRIND)
				t->result = PWTEST_SKIP;
			t->args.pw_daemon = true;
			break;
		}
	}
	va_end(args);

	spa_list_append(&suite->tests, &t->link);
}

extern const struct pwtest_suite_decl __start_pwtest_suite_section;
extern const struct pwtest_suite_decl __stop_pwtest_suite_section;

static void add_suite(struct pwtest_context *ctx,
		      const struct pwtest_suite_decl *decl)
{
	struct pwtest_suite *c = calloc(1, sizeof *c);

	c->decl = decl;
	spa_list_init(&c->tests);
	spa_list_append(&ctx->suites, &c->link);
}

static void free_test(struct pwtest_test *t)
{
	spa_list_remove(&t->link);
	for (size_t i = 0; i < SPA_N_ELEMENTS(t->logs); i++)
		pw_array_clear(&t->logs[i]);
	pw_properties_free(t->args.props);
	pw_properties_free(t->args.env);
	pw_properties_free(t->env);
	free(t);
}

static void free_suite(struct pwtest_suite *c)
{
	struct pwtest_test *t, *tmp;

	spa_list_for_each_safe(t, tmp, &c->tests, link)
		free_test(t);

	spa_list_remove(&c->link);
	free(c);
}

static void find_suites(struct pwtest_context *ctx, const char *suite_filter)
{
	const struct pwtest_suite_decl *c;

	for (c = &__start_pwtest_suite_section; c < &__stop_pwtest_suite_section; c++) {
		if (suite_filter == NULL || fnmatch(suite_filter, c->name, 0) == 0)
			add_suite(ctx, c);
	}
}

static void add_tests(struct pwtest_context *ctx)
{
	struct pwtest_suite *c;

	spa_list_for_each(c, &ctx->suites, link) {
		c->result = c->decl->setup(ctx, c);
		spa_assert_se(c->result >= PWTEST_PASS && c->result <= PWTEST_SYSTEM_ERROR);
	}
}

static int remove_file(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
	char *tmpdir = getenv("TMPDIR");
	int r;

	/* Safety check: bail out if somehow we left TMPDIR */
	spa_assert_se(tmpdir != NULL);
	spa_assert_se(spa_strneq(fpath, tmpdir, strlen(tmpdir)));

	r = remove(fpath);
	if (r)
		fprintf(stderr, "Failed to remove %s: %m", fpath);

	return r;
}

static void remove_xdg_runtime_dir(const char *xdg_dir)
{
	char *tmpdir = getenv("TMPDIR");
	char path[PATH_MAX];
	int r;

	if (xdg_dir == NULL)
		return;

	/* Safety checks, we really don't want to recursively remove a
	 * random directory due to a bug */
	spa_assert_se(tmpdir != NULL);
	spa_assert_se(spa_strneq(xdg_dir, tmpdir, strlen(tmpdir)));
	r = spa_scnprintf(path, sizeof(path), "%s/pwtest.dir", xdg_dir);
	spa_assert_se((size_t)r == strlen(xdg_dir) + 11);
	if (access(path, F_OK) != 0) {
		fprintf(stderr, "XDG_RUNTIME_DIR changed, cannot clean up\n");
		return;
	}

	nftw(xdg_dir, remove_file, 16, FTW_DEPTH | FTW_PHYS);
}

static void cleanup(struct pwtest_context *ctx)
{
	struct pwtest_suite *c, *tmp;

	terminate_cleanup_pids(ctx);
	free_cleanup_pids(ctx);

	spa_list_for_each_safe(c, tmp, &ctx->suites, link) {
		free_suite(c);
	}

	remove_xdg_runtime_dir(ctx->xdg_dir);
	free(ctx->xdg_dir);
}

static void sighandler(int signal)
{
	struct sigaction act;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = SIG_DFL;
	sigaction(signal, &act, NULL);

	pwtest_backtrace(0);
	terminate_cleanup_pids(ctx);
	raise(signal);
}

static inline void log_append(struct pw_array *buffer, int fd)
{
	int r = 0;
	const int sz = 65536;

	while (true) {
		r = pw_array_ensure_size(buffer, sz);
		spa_assert_se(r == 0);
		r = read(fd, pw_array_end(buffer), sz);
		if (r <= 0)
			break;
		/* We've read directly into the array's buffer, we just add
		 * now to update the array */
		pw_array_add(buffer, r);
	}
}

static bool collect_child(struct pwtest_test *t, pid_t pid)
{
	int r;
	int status;

	r = waitpid(pid, &status, WNOHANG);
	if (r <= 0)
		return false;

	if (WIFEXITED(status)) {
		t->result = WEXITSTATUS(status);
		switch (t->result) {
			case PWTEST_PASS:
			case PWTEST_SKIP:
			case PWTEST_FAIL:
			case PWTEST_TIMEOUT:
			case PWTEST_SYSTEM_ERROR:
				break;
			default:
				spa_assert_se(!"Invalid test result");
				break;
		}
		return true;
	}

	if (WIFSIGNALED(status)) {
		t->sig_or_errno = WTERMSIG(status);
		t->result = (t->sig_or_errno == t->args.signal) ? PWTEST_PASS : PWTEST_FAIL;
	} else {
		t->result = PWTEST_FAIL;
	}
	return true;
}

static pid_t start_pwdaemon(struct pwtest_test *t, int stderr_fd, int log_fd)
{
	static unsigned int count;
	const char *daemon = BUILD_ROOT "/src/daemon/pipewire-uninstalled";
	pid_t pid;
	char pw_remote[64];
	int status;
	int r;

	spa_scnprintf(pw_remote, sizeof(pw_remote), "pwtest-pw-%u\n", count++);
	replace_env(t, "PIPEWIRE_REMOTE", pw_remote);

	pid = fork();
	if (pid == 0) {
		/* child */
		setpgid(0, 0);

		setenv("PIPEWIRE_CORE", pw_remote, 1);
		setenv("PIPEWIRE_DEBUG", "4", 0);
		setenv("WIREPLUMBER_DEBUG", "4", 0);

		r = dup2(stderr_fd, STDERR_FILENO);
		spa_assert_se(r != -1);
		r = dup2(stderr_fd, STDOUT_FILENO);
		spa_assert_se(r != -1);

		execl(daemon, daemon, (char*)NULL);
		return -errno;

	} else if (pid < 0) {
		return pid;
	}

	add_cleanup_pid(ctx, -pid);

	/* parent */
	sleep(1); /* FIXME how to wait for pw to be ready? */
	if (waitpid(pid, &status, WNOHANG) > 0) {
		if (WIFEXITED(status)) {
			dprintf(log_fd, "pipewire daemon exited with %d before test started\n", WEXITSTATUS(status));
			return -ESRCH;
		} else if (WIFSIGNALED(status)) {
			dprintf(log_fd, "pipewire daemon terminated with %d (SIG%s) before test started\n", WTERMSIG(status),
				sigabbrev_np(WTERMSIG(status)));
			return -EHOSTDOWN;
		}
	}

	return pid;
}

static void make_xdg_runtime_test_dir(char dir[PATH_MAX], const char *prefix)
{
	static size_t counter;
	int r;

	r = spa_scnprintf(dir, PATH_MAX, "%s/%zd", prefix, counter++);
	spa_assert_se(r >= (int)(strlen(prefix) + 2));
	r = mkdir(dir, 0777);
	if (r == -1) {
		fprintf(stderr, "Failed to make XDG_RUNTIME_DIR %s (%m)\n", dir);
		spa_assert_se(r != -1);
	}
}

static void set_test_env(struct pwtest_context *ctx, struct pwtest_test *t)
{
	char xdg_runtime_dir[PATH_MAX];

	make_xdg_runtime_test_dir(xdg_runtime_dir, ctx->xdg_dir);
	replace_env(t, "XDG_RUNTIME_DIR", xdg_runtime_dir);
	replace_env(t, "TMPDIR", xdg_runtime_dir);

	replace_env(t, "SPA_PLUGIN_DIR", BUILD_ROOT "/spa/plugins");
	replace_env(t, "SPA_DATA_DIR", SOURCE_ROOT "/spa/plugins");
	replace_env(t, "PIPEWIRE_CONFIG_DIR", BUILD_ROOT "/src/daemon");
	replace_env(t, "PIPEWIRE_MODULE_DIR", BUILD_ROOT "/src/modules");
	replace_env(t, "ACP_PATHS_DIR", SOURCE_ROOT "/spa/plugins/alsa/mixer/paths");
	replace_env(t, "ACP_PROFILES_DIR", SOURCE_ROOT "/spa/plugins/alsa/mixer/profile-sets");
	replace_env(t, "PIPEWIRE_LOG_SYSTEMD", "false");
}

static void close_pipes(int fds[_FD_LAST])
{
	for (int i = 0; i < _FD_LAST; i++) {
		if (fds[i] >= 0)
			close(fds[i]);
		fds[i] = -1;
	}
}

static int init_pipes(int read_fds[_FD_LAST], int write_fds[_FD_LAST])
{
	int r;
	int i;
	int pipe_max_size = 4194304;

	for (i = 0; i < _FD_LAST; i++) {
		read_fds[i] = -1;
		write_fds[i] = -1;
	}

#ifdef __linux__
	{
		FILE *f;
		f = fopen("/proc/sys/fs/pipe-max-size", "re");
		if (f) {
			if (fscanf(f, "%d", &r) == 1)
				pipe_max_size = SPA_MIN(r, pipe_max_size);
			fclose(f);
		}
	}
#endif

	for (i = 0; i < _FD_LAST; i++) {
		int pipe[2];

		r = pipe2(pipe, O_CLOEXEC | O_NONBLOCK);
		if (r < 0)
			goto error;
		read_fds[i] = pipe[0];
		write_fds[i] = pipe[1];
#ifdef __linux__
		/* Max pipe buffers, to avoid scrambling if reading lags.
		 * Can't use blocking write fds, since reading too slow
		 * then affects execution.
		 */
		fcntl(write_fds[i], F_SETPIPE_SZ, pipe_max_size);
#endif
	}

	return 0;
error:
	r = -errno;
	close_pipes(read_fds);
	close_pipes(write_fds);
	return r;
}

static void start_test_nofork(struct pwtest_test *t)
{
	const char *env;
	void *state = NULL;

	/* This is going to mess with future tests */
	while ((env = pw_properties_iterate(t->args.env, &state)))
		replace_env(t, env, pw_properties_get(t->args.env, env));

	/* The actual test function */
	t->result = t->func(t);
}

static int start_test_forked(struct pwtest_test *t, int read_fds[_FD_LAST], int write_fds[_FD_LAST])
{
	pid_t pid;
	enum pwtest_result result;
	struct sigaction act;
	const char *env;
	void *state = NULL;
	int r;

	pid = fork();
	if (pid < 0) {
		r = -errno;
		close_pipes(read_fds);
		close_pipes(write_fds);
		return r;
	}

	if (pid > 0) { /* parent */
		close_pipes(write_fds);
		return pid;
	}

	/* child */

	close_pipes(read_fds);

	/* Reset cleanup pid list */
	free_cleanup_pids(ctx);

	/* Catch any crashers so we can insert a backtrace */
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = sighandler;
	sigaction(SIGSEGV, &act, NULL);
	sigaction(SIGBUS, &act, NULL);
	sigaction(SIGSEGV, &act, NULL);
	sigaction(SIGABRT, &act, NULL);
	/* SIGALARM is used for our timeout */
	sigaction(SIGALRM, &act, NULL);

	r = dup2(write_fds[FD_STDERR], STDERR_FILENO);
	spa_assert_se(r != -1);
	setlinebuf(stderr);
	r = dup2(write_fds[FD_STDOUT], STDOUT_FILENO);
	spa_assert_se(r != -1);
	setlinebuf(stdout);

	/* For convenience in the tests, let this be a global variable. */
	testlog_fd = write_fds[FD_LOG];

	while ((env = pw_properties_iterate(t->args.env, &state)))
		setenv(env, pw_properties_get(t->args.env, env), 1);

	/* The actual test function */
	result = t->func(t);

	for (int i = 0; i < _FD_LAST; i++)
		fsync(write_fds[i]);

	exit(result);
}

static int monitor_test_forked(struct pwtest_test *t, pid_t pid, int read_fds[_FD_LAST])
{
	int pidfd = -1, timerfd = -1, epollfd = -1;
	struct epoll_event ev[10];
	size_t nevents = 0;
	int r;

#ifdef HAVE_PIDFD_OPEN
	pidfd = syscall(SYS_pidfd_open, pid, 0);
#else
	errno = ENOSYS;
#endif
	/* If we don't have pidfd, we use a timerfd to ping us every 20ms */
	if (pidfd < 0 && errno == ENOSYS) {
		pidfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
		if (pidfd == -1)
			goto error;
		r = timerfd_settime(pidfd, 0,
				    &((struct itimerspec ){
				      .it_interval.tv_nsec = 20 * 1000 * 1000,
				      .it_value.tv_nsec = 20 * 1000 * 1000,
				      }), NULL);
		if (r < 0)
			goto error;
	}

	/* Each test has an epollfd with:
	 *   - a timerfd so we can kill() it if it hangs
	 *   - a pidfd so we get notified when the test exits
	 *   - a pipe for stdout and a pipe for stderr
	 *   - a pipe for logging (the various pwtest functions)
	 *   - a pipe for the daemon's stdout
	 */
	timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (timerfd < 0)
		goto error;
	timerfd_settime(timerfd, 0, &((struct itimerspec ){ .it_value.tv_sec = ctx->timeout}), NULL);

	epollfd = epoll_create(1);
	if (epollfd < 0)
		goto error;
	ev[nevents++] = (struct epoll_event){ .events = EPOLLIN, .data.fd = pidfd };
	ev[nevents++] = (struct epoll_event){ .events = EPOLLIN, .data.fd = read_fds[FD_STDOUT] };
	ev[nevents++] = (struct epoll_event){ .events = EPOLLIN, .data.fd = read_fds[FD_STDERR] };
	ev[nevents++] = (struct epoll_event){ .events = EPOLLIN, .data.fd = read_fds[FD_LOG] };
	ev[nevents++] = (struct epoll_event){ .events = EPOLLIN, .data.fd = timerfd };
	if (t->args.pw_daemon)
		ev[nevents++] = (struct epoll_event){ .events = EPOLLIN, .data.fd = read_fds[FD_DAEMON] };

	for (size_t i = 0; i < nevents; i++) {
		r = epoll_ctl(epollfd, EPOLL_CTL_ADD, ev[i].data.fd, &ev[i]);
		if (r < 0)
			goto error;
	}

	while (true) {
		struct epoll_event e;

		r = epoll_wait(epollfd, &e, 1, (ctx->timeout * 2) * 1000);
		if (r == 0)
			break;
		if (r == -1) {
			goto error;
		}

		if (e.data.fd == pidfd) {
			uint64_t buf;
			int ignore SPA_UNUSED;
			ignore = read(pidfd, &buf, sizeof(buf)); /* for timerfd fallback */
			if (collect_child(t, pid))
				break;
		} else if (e.data.fd == timerfd) {
			/* SIGALARM so we get the backtrace */
			kill(pid, SIGALRM);
			t->result = PWTEST_TIMEOUT;
			waitpid(pid, NULL, 0);
			break;
		} else {
			for (int i = 0; i < _FD_LAST; i++) {
				if (e.data.fd == read_fds[i]) {
					log_append(&t->logs[i], e.data.fd);
				}

			}
		}
	}
	errno = 0;
error:
	r = errno;
	close(epollfd);
	close(timerfd);
	close(pidfd);

	return -r;
}

static void run_test(struct pwtest_context *ctx, struct pwtest_suite *c, struct pwtest_test *t)
{
	pid_t pid;
	pid_t pw_daemon = 0;
	int read_fds[_FD_LAST], write_fds[_FD_LAST];
	int r;
	const char *tmpdir;

	if (t->result == PWTEST_SKIP) {
		char *buf = pw_array_add(&t->logs[FD_LOG], 64);
		spa_scnprintf(buf, 64, "pwtest: test skipped by pwtest\n");
		return;
	}

	t->result = PWTEST_SYSTEM_ERROR;

	r = init_pipes(read_fds, write_fds);
	if (r < 0) {
		t->sig_or_errno = r;
		return;
	}

	set_test_env(ctx, t);
	tmpdir = getenv("TMPDIR");
	spa_assert_se(tmpdir != NULL);
	r = chdir(tmpdir);
	if (r < 0) {
		char *buf = pw_array_add(&t->logs[FD_LOG], 256);
		spa_scnprintf(buf, 256, "pwtest: failed to chdir to '%s'\n", tmpdir);
		t->sig_or_errno = -errno;
		goto error;
	}

	if (t->args.pw_daemon) {
		pw_daemon = start_pwdaemon(t, write_fds[FD_DAEMON], write_fds[FD_LOG]);
		if (pw_daemon < 0) {
			errno = -pw_daemon;
			goto error;
		}
	} else {
		replace_env(t, "PIPEWIRE_REMOTE", "test-has-no-daemon");
	}

	if (ctx->no_fork) {
		start_test_nofork(t);
	} else {
		pid = start_test_forked(t, read_fds, write_fds);
		if (pid < 0) {
			errno = -r;
			goto error;
		}
		add_cleanup_pid(ctx, pid);

		r = monitor_test_forked(t, pid, read_fds);
		if (r < 0) {
			errno = -r;
			goto error;
		}
		remove_cleanup_pid(ctx, pid);
	}

	errno = 0;
error:
	if (errno)
		t->sig_or_errno = -errno;

	if (ctx->terminate) {
		char *buf = pw_array_add(&t->logs[FD_LOG], 64);
		spa_scnprintf(buf, 64, "pwtest: tests terminated by signal\n");
		t->result = PWTEST_SYSTEM_ERROR;
	}

	for (size_t i = 0; i < SPA_N_ELEMENTS(read_fds); i++) {
		log_append(&t->logs[i], read_fds[i]);
	}

	if (pw_daemon > 0) {
		int status;

		kill(-pw_daemon, SIGTERM);
		remove_cleanup_pid(ctx, -pw_daemon);

		/* blocking read. the other end closes when done */
		close_pipes(write_fds);
		fcntl(read_fds[FD_DAEMON], F_SETFL, O_CLOEXEC);
		do {
			log_append(&t->logs[FD_DAEMON], read_fds[FD_DAEMON]);
		} while ((r = waitpid(pw_daemon, &status, WNOHANG)) == 0);

		if (r > 0) {
			/* write_fds are closed in the parent process, so we append directly */
			char *buf = pw_array_add(&t->logs[FD_DAEMON], 64);

			if (WIFEXITED(status)) {
				spa_scnprintf(buf, 64, "pwtest: pipewire daemon exited with status %d\n",
					 WEXITSTATUS(status));
			} else if (WIFSIGNALED(status)) {
				spa_scnprintf(buf, 64, "pwtest: pipewire daemon crashed with signal %d (SIG%s)\n",
					WTERMSIG(status), sigabbrev_np(WTERMSIG(status)));
			}
		}
	}

	for (size_t i = 0; i < SPA_N_ELEMENTS(t->logs); i++) {
		char *e = pw_array_add(&t->logs[i], 1);
		spa_assert_se(e);
		*e = '\0';
	}

	close_pipes(read_fds);
	close_pipes(write_fds);

	restore_env(t);
}

static inline void print_lines(FILE *fp, const char *log, const char *prefix)
{
	const char *state = NULL;
	const char *s;
	size_t len;

	while (true) {
		if ((s = pw_split_walk(log, "\n", &len, &state)) == NULL)
			break;
		fprintf(fp, "%s%.*s\n", prefix, (int)len, s);
	}
}

static void log_test_result(struct pwtest_test *t)
{
	const struct status *s;
	const struct status {
		const char *status;
		const char *color;
	} statuses[] = {
		{ "PASS", SPA_ANSI_BOLD_GREEN },
		{ "FAIL", SPA_ANSI_BOLD_RED },
		{ "SKIP", SPA_ANSI_BOLD_YELLOW },
		{ "TIMEOUT", SPA_ANSI_BOLD_CYAN },
		{ "ERROR", SPA_ANSI_BOLD_MAGENTA },
	};

	spa_assert_se(t->result >= PWTEST_PASS);
	spa_assert_se(t->result <= PWTEST_SYSTEM_ERROR);
	s = &statuses[t->result - PWTEST_PASS];

	fprintf(stderr, "    status: %s%s%s\n",
		isatty(STDERR_FILENO) ? s->color : "",
		s->status,
		isatty(STDERR_FILENO) ? "\x1B[0m" : "");

	switch (t->result) {
		case PWTEST_PASS:
		case PWTEST_SKIP:
			if (!verbose)
				return;
			break;
		default:
			break;
	}

	if (t->sig_or_errno > 0)
		fprintf(stderr, "    signal: %d # SIG%s \n", t->sig_or_errno,
			sigabbrev_np(t->sig_or_errno));
	else if (t->sig_or_errno < 0)
		fprintf(stderr, "    errno: %d # %s\n", -t->sig_or_errno,
			strerror(-t->sig_or_errno));
	if (t->logs[FD_LOG].size) {
		fprintf(stderr, "    log: |\n");
		print_lines(stderr, t->logs[FD_LOG].data, "      ");
	}
	if (t->logs[FD_STDOUT].size) {
		fprintf(stderr, "    stdout: |\n");
		print_lines(stderr, t->logs[FD_STDOUT].data, "      ");
	}
	if (t->logs[FD_STDERR].size) {
		fprintf(stderr, "    stderr: |\n");
		print_lines(stderr, t->logs[FD_STDERR].data, "      ");
	}
	if (t->logs[FD_DAEMON].size) {
		fprintf(stderr, "    daemon: |\n");
		print_lines(stderr, t->logs[FD_DAEMON].data, "      ");
	}
}

static char* make_xdg_runtime_dir(void)
{
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	char *dir;
	const char *tmpdir = getenv("TMPDIR");
	char path[PATH_MAX];
	FILE *fp;

	if (!tmpdir)
		tmpdir = "/tmp";

	int r = asprintf(&dir, "%s/pwtest-%02d:%02d-XXXXXX", tmpdir, tm->tm_hour, tm->tm_min);
	spa_assert_se((size_t)r == strlen(tmpdir) + 20); /* rough estimate */
	spa_assert_se(mkdtemp(dir) != NULL);

	/* Marker file to avoid removing a random directory during cleanup */
	r = spa_scnprintf(path, sizeof(path), "%s/pwtest.dir", dir);
	spa_assert_se((size_t)r == strlen(dir) + 11);
	fp = fopen(path, "we");
	spa_assert_se(fp);
	fprintf(fp, "pwtest\n");
	fclose(fp);

	return dir;
}

static int run_tests(struct pwtest_context *ctx)
{
	int r = EXIT_SUCCESS;
	struct pwtest_suite *c;
	struct pwtest_test *t;

	fprintf(stderr, "pwtest:\n");
	spa_list_for_each(c, &ctx->suites, link) {
		if (c->result != PWTEST_PASS)
			continue;

		fprintf(stderr, "- suite: \"%s\"\n", c->decl->name);
		fprintf(stderr, "  tests:\n");
		spa_list_for_each(t, &c->tests, link) {
			int min = t->args.range.min,
			    max = t->args.range.max;
			bool have_range = min != 0 || max != 1;

			for (int iteration = min; iteration < max; iteration++) {
				if (ctx->has_iteration_filter &&
				    ctx->iteration_filter != iteration)
					continue;

				fprintf(stderr, "  - name: \"%s\"\n", t->name);
				if (have_range)
					fprintf(stderr, "    iteration: %d  # %d - %d\n",
						iteration, min, max);
				t->iteration = iteration;
				run_test(ctx, c, t);
				log_test_result(t);

				switch (t->result) {
					case PWTEST_PASS:
					case PWTEST_SKIP:
						break;
					default:
						r = EXIT_FAILURE;
						break;
				}

				if (ctx->terminate) {
					r = EXIT_FAILURE;
					return r;
				}
			}
		}
	}
	return r;
}

static void list_tests(struct pwtest_context *ctx)
{
	struct pwtest_suite *c;
	struct pwtest_test *t;

	fprintf(stderr, "pwtest:\n");
	spa_list_for_each(c, &ctx->suites, link) {
		fprintf(stderr, "- suite: \"%s\"\n", c->decl->name);
		fprintf(stderr, "  tests:\n");
		spa_list_for_each(t, &c->tests, link) {
			fprintf(stderr, "  - { name: \"%s\" }\n", t->name);
		}
	}
}

static bool is_debugger_attached(void)
{
	bool rc = false;
#ifdef HAVE_LIBCAP
	int status;
	int pid = fork();

	if (pid == -1)
		return 0;

	if (pid == 0) {
		int ppid = getppid();
		cap_t caps = cap_get_pid(ppid);
		cap_flag_value_t cap_val;

		if (cap_get_flag(caps, CAP_SYS_PTRACE, CAP_EFFECTIVE, &cap_val) == -1 ||
		    cap_val != CAP_SET)
			_exit(false);

		if (ptrace(PTRACE_ATTACH, ppid, NULL, 0) == 0) {
			waitpid(ppid, NULL, 0);
			ptrace(PTRACE_CONT, ppid, NULL, 0);
			ptrace(PTRACE_DETACH, ppid, NULL, 0);
			rc = false;
		} else {
			rc = true;
		}
		_exit(rc);
	} else {
		waitpid(pid, &status, 0);
		rc = WEXITSTATUS(status);
	}

#endif
	return !!rc;
}

static void usage(FILE *fp, const char *progname)
{
	fprintf(fp, "Usage: %s [OPTIONS]\n"
		"  -h, --help		Show this help\n"
		"  --verbose		Verbose output\n"
		"  --list		List all available suites and tests\n"
		"  --timeout=N		Set the test timeout to N seconds (default: 15)\n"
		"  --filter-test=glob	Run only tests matching the given glob\n"
		"  --filter-suites=glob	Run only suites matching the given glob\n"
		"  --filter-iteration=N	Run only iteration N\n"
		"  --no-fork		Do not fork for the test (see note below)\n"
		"\n"
		"Using --no-fork allows for easy debugging of tests but should only be used.\n"
		"used with --filter-test. A test that modifies the process state will affect\n"
		"subsequent tests and invalidate test results.\n",
		progname);
}

static void sigterm_handler(int signo)
{
	terminate_cleanup_pids(ctx);
	ctx->terminate = true;
	if (ctx->no_fork) {
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		raise(signo);
	}
}

int main(int argc, char **argv)
{
	int r = EXIT_SUCCESS;
	enum {
		OPT_TIMEOUT = 10,
		OPT_LIST,
		OPT_VERBOSE,
		OPT_FILTER_TEST,
		OPT_FILTER_SUITE,
		OPT_FILTER_ITERATION,
		OPT_NOFORK,
	};
	static const struct option opts[] = {
		{ "help",		no_argument,		0, 'h' },
		{ "timeout",		required_argument,	0, OPT_TIMEOUT },
		{ "list",		no_argument,		0, OPT_LIST },
		{ "filter-test",	required_argument,	0, OPT_FILTER_TEST },
		{ "filter-suite",	required_argument,	0, OPT_FILTER_SUITE },
		{ "filter-iteration",	required_argument,	0, OPT_FILTER_ITERATION },
		{ "list",		no_argument,		0, OPT_LIST },
		{ "verbose",		no_argument,		0, OPT_VERBOSE },
		{ "no-fork",		no_argument,		0, OPT_NOFORK },
		{ NULL },
	};
	struct pwtest_context test_ctx = {
		.suites = SPA_LIST_INIT(&test_ctx.suites),
		.timeout = 15,
		.has_iteration_filter = false,
	};
	enum {
		MODE_TEST,
		MODE_LIST,
	} mode = MODE_TEST;
	const char *suite_filter = NULL;

	spa_list_init(&test_ctx.cleanup_pids);

	ctx = &test_ctx;

	while (1) {
		int c;
		int option_index = 0;

		c = getopt_long(argc, argv, "h", opts, &option_index);
		if (c == -1)
			break;
		switch(c) {
		case 'h':
			usage(stdout, argv[0]);
			exit(EXIT_SUCCESS);
		case OPT_TIMEOUT:
			ctx->timeout = atoi(optarg);
			break;
		case OPT_LIST:
			mode = MODE_LIST;
			break;
		case OPT_VERBOSE:
			verbose = true;
			break;
		case OPT_FILTER_TEST:
			ctx->test_filter = optarg;
			break;
		case OPT_FILTER_SUITE:
			suite_filter= optarg;
			break;
		case OPT_FILTER_ITERATION:
			ctx->has_iteration_filter = spa_atoi32(optarg, &ctx->iteration_filter, 10);
			break;
		case OPT_NOFORK:
			ctx->no_fork = true;
			break;
		default:
			usage(stderr, argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (RUNNING_ON_VALGRIND || is_debugger_attached())
		ctx->no_fork = true;

	find_suites(ctx, suite_filter);
	add_tests(ctx);

	if (getenv("TMPDIR") == NULL)
		setenv("TMPDIR", "/tmp", 1);

	ctx->xdg_dir = make_xdg_runtime_dir();

	switch (mode) {
	case MODE_LIST:
		list_tests(ctx);
		break;
	case MODE_TEST:
		setrlimit(RLIMIT_CORE, &((struct rlimit){0, 0}));
		signal(SIGTERM, sigterm_handler);
		signal(SIGINT, sigterm_handler);
		r = run_tests(ctx);
		break;
	}

	cleanup(ctx);

	return r;
}
