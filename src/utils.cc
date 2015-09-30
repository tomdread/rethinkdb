// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "utils.hpp"

#include <math.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32 // TODO ATN
#include "windows.hpp"
#include <io.h>
#include <direct.h>
#include <filesystem>
#include <random>
#else
#include <ftw.h>
#include <sys/time.h>
#include <sys/resource.h>
#endif

#include <google/protobuf/stubs/common.h>

#include "errors.hpp"
#include <boost/date_time.hpp>

#include "arch/io/disk.hpp"
#include "arch/runtime/coroutines.hpp"
#include "arch/runtime/runtime.hpp"
#include "clustering/administration/main/directory_lock.hpp"
#include "config/args.hpp"
#include "containers/archive/archive.hpp"
#include "containers/archive/file_stream.hpp"
#include "containers/printf_buffer.hpp"
#include "debug.hpp"
#include "logger.hpp"
#include "rdb_protocol/ql2.pb.h"
#include "thread_local.hpp"

void run_generic_global_startup_behavior() {
    install_generic_crash_handler();
    install_new_oom_handler();

    // Set the locale to C, because some ReQL terms may produce different
    // results in different locales, and we need to avoid data divergence when
    // two servers in the same cluster have different locales.
    setlocale(LC_ALL, "C");

#if !defined(_WIN32) // ATN: TODO: use feature macro. also TODO: extend limit on windows
    rlimit file_limit;
    int res = getrlimit(RLIMIT_NOFILE, &file_limit);
    guarantee_err(res == 0, "getrlimit with RLIMIT_NOFILE failed");

    // We need to set the file descriptor limit maximum to a higher value.  On
    // OS X, rlim_max is RLIM_INFINITY and, with RLIMIT_NOFILE, it's illegal to
    // set rlim_cur to RLIM_INFINITY.  On Linux, maybe the same thing is
    // illegal, but rlim_max is set to a finite value (65K - 1) anyway.  OS X
    // has OPEN_MAX defined to limit the highest possible file descriptor value,
    // and that's what'll end up being the new rlim_cur value.  (The man page on
    // OS X suggested it.)  I don't know if Linux has a similar thing, or other
    // platforms, so we just go with rlim_max, and if we ever see a warning,
    // we'll fix it.  Users can always deal with the problem on their end for a
    // while using ulimit or whatever.)

#ifdef __MACH__
    file_limit.rlim_cur = std::min<rlim_t>(OPEN_MAX, file_limit.rlim_max);
#else
    file_limit.rlim_cur = file_limit.rlim_max;
#endif
    res = setrlimit(RLIMIT_NOFILE, &file_limit);

    if (res != 0) {
        logWRN("The call to set the open file descriptor limit failed (errno = %d - %s)\n",
            get_errno(), errno_string(get_errno()).c_str());
    }
#endif

#ifdef _WIN32
	// ATN TODO
	WSADATA wsa_data;
	DWORD res = WSAStartup(MAKEWORD(2, 2), &wsa_data);
        guarantee_winerr(res == NO_ERROR, "WSAStartup failed");
#endif
}

startup_shutdown_t::startup_shutdown_t() {
    run_generic_global_startup_behavior();
}

startup_shutdown_t::~startup_shutdown_t() {
    google::protobuf::ShutdownProtobufLibrary();
}


void print_hexddump(const void *vbuf, size_t offset, size_t ulength) {
#ifndef _MSC_VER // ATN: TODO
    flockfile(stderr);
#endif

    if (ulength == 0) {
        fprintf(stderr, "(data length is zero)\n");
    }

    const char *buf = reinterpret_cast<const char *>(vbuf);
    ssize_t length = ulength;

    uint8_t bd_sample[16] = { 0xBD, 0xBD, 0xBD, 0xBD,
                              0xBD, 0xBD, 0xBD, 0xBD,
                              0xBD, 0xBD, 0xBD, 0xBD,
                              0xBD, 0xBD, 0xBD, 0xBD };
    uint8_t zero_sample[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t ff_sample[16] = { 0xff, 0xff, 0xff, 0xff,
                              0xff, 0xff, 0xff, 0xff,
                              0xff, 0xff, 0xff, 0xff,
                              0xff, 0xff, 0xff, 0xff };

    bool skipped_last = false;
    while (length > 0) {
        bool skip = length >= 16 && (
                    memcmp(buf, bd_sample, 16) == 0 ||
                    memcmp(buf, zero_sample, 16) == 0 ||
                    memcmp(buf, ff_sample, 16) == 0);
        if (skip) {
            if (!skipped_last) fprintf(stderr, "*\n");
        } else {
            fprintf(stderr, "%.8x  ", (unsigned int)offset);
            for (ssize_t i = 0; i < 16; ++i) {
                if (i < length) {
                    fprintf(stderr, "%.2hhx ", buf[i]);
                } else {
                    fprintf(stderr, "   ");
                }
            }
            fprintf(stderr, "| ");
            for (ssize_t i = 0; i < 16; ++i) {
                if (i < length) {
                    if (isprint(buf[i])) {
                        fputc(buf[i], stderr);
                    } else {
                        fputc('.', stderr);
                    }
                } else {
                    fputc(' ', stderr);
                }
            }
            fprintf(stderr, "\n");
        }
        skipped_last = skip;

        offset += 16;
        buf += 16;
        length -= 16;
    }

#ifndef _MSC_VER // ATN: TODO
    funlockfile(stderr);
#endif
}

void format_time(struct timespec time, printf_buffer_t *buf, local_or_utc_time_t zone) {
    struct tm t;
    if (zone == local_or_utc_time_t::utc) {
        boost::posix_time::ptime as_ptime = boost::posix_time::from_time_t(time.tv_sec);
        t = boost::posix_time::to_tm(as_ptime);
    } else {
#ifndef _MSC_VER
        struct tm *res1;
        res1 = localtime_r(&time.tv_sec, &t);
        guarantee_err(res1 == &t, "localtime_r() failed.");
#else
		errno_t res = localtime_s(&t, &time.tv_sec);
		guarantee_xerr(res == 0, res, "localtime_s() failed.");
#endif
    }
    buf->appendf(
        "%04d-%02d-%02dT%02d:%02d:%02d.%09ld",
        t.tm_year+1900,
        t.tm_mon+1,
        t.tm_mday,
        t.tm_hour,
        t.tm_min,
        t.tm_sec,
        time.tv_nsec);
}

std::string format_time(struct timespec time, local_or_utc_time_t zone) {
    printf_buffer_t buf;
    format_time(time, &buf, zone);
    return std::string(buf.c_str());
}

bool parse_time(const std::string &str, local_or_utc_time_t zone,
                struct timespec *out, std::string *errmsg_out) {
    struct tm t;
    struct timespec time;
    int res1 = sscanf(str.c_str(),
        "%04d-%02d-%02dT%02d:%02d:%02d.%09ld",
        &t.tm_year,
        &t.tm_mon,
        &t.tm_mday,
        &t.tm_hour,
        &t.tm_min,
        &t.tm_sec,
        &time.tv_nsec);
    if (res1 != 7) {
        *errmsg_out = "badly formatted time";
        return false;
    }
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    t.tm_isdst = -1;
    if (zone == local_or_utc_time_t::utc) {
        boost::posix_time::ptime as_ptime = boost::posix_time::ptime_from_tm(t);
        boost::posix_time::ptime epoch(boost::gregorian::date(1970, 1, 1));
        /* Apparently `(x-y).total_seconds()` is returning the numeric difference in the
        POSIX timestamps, which approximates the difference in solar time. This is weird
        (I'd expect it to return the difference in atomic time) but it turns out to give
        the correct behavior in this case. */
        time.tv_sec = (as_ptime - epoch).total_seconds();
    } else {
        time.tv_sec = mktime(&t);
        if (time.tv_sec == -1) {
            *errmsg_out = "invalid time";
            return false;
        }
    }
    *out = time;
    return true;
}

with_priority_t::with_priority_t(int priority) {
    rassert(coro_t::self() != NULL);
    previous_priority = coro_t::self()->get_priority();
    coro_t::self()->set_priority(priority);
}
with_priority_t::~with_priority_t() {
    rassert(coro_t::self() != NULL);
    coro_t::self()->set_priority(previous_priority);
}

void *raw_malloc_aligned(size_t size, size_t alignment) {
    void *ptr = NULL;
#ifndef _MSC_VER
    int res = posix_memalign(&ptr, alignment, size);  // NOLINT(runtime/rethinkdb_fn)
    if (res != 0) {
        if (res == EINVAL) {
            crash_or_trap("posix_memalign with bad alignment: %zu.", alignment);
        } else if (res == ENOMEM) {
            crash_oom();
        } else {
            crash_or_trap("posix_memalign failed with unknown result: %d.", res);
        }
    }
#else
	ptr = _aligned_malloc(size, alignment);
	if (ptr == NULL) {
		crash_oom();
	}
#endif
    return ptr;
}

void raw_free_aligned(void *ptr) {
#ifdef _MSC_VER
	_aligned_free(ptr);
#else
	free(ptr);
#endif
}

void *rmalloc(size_t size) {
    void *res = malloc(size);  // NOLINT(runtime/rethinkdb_fn)
    if (res == NULL && size != 0) {
        crash_oom();
    }
    return res;
}

void *rrealloc(void *ptr, size_t size) {
    void *res = realloc(ptr, size);  // NOLINT(runtime/rethinkdb_fn)
    if (res == NULL && size != 0) {
        crash_oom();
    }
    return res;
}

bool risfinite(double arg) {
    // isfinite is a macro on OS X in math.h, so we can't just say std::isfinite.
    using namespace std; // NOLINT(build/namespaces) due to platform variation
    return isfinite(arg);
}

rng_t::rng_t(int seed) {
#ifndef NDEBUG
    if (seed == -1) {
#ifdef _MSC_VER
		seed = std::random_device{}();
#else
		seed = get_secs();
#endif
    }
#else
    seed = 314159;
#endif
#ifdef _MSC_VER
	state.seed(seed);
#else
    state[2] = seed / (1 << 16);
    state[1] = seed % (1 << 16);
    state[0] = 0x330E;
#endif
}

int rng_t::randint(int n) {
    guarantee(n > 0, "non-positive argument for randint's [0,n) interval");
#ifndef _MSC_VER
    long x = nrand48(state.data());  // NOLINT(runtime/int)
#else
    unsigned long x = state();
#endif
    return x % static_cast<unsigned int>(n);
}

uint64_t rng_t::randuint64(uint64_t n) {
    guarantee(n > 0, "non-positive argument for randint's [0,n) interval");
#ifndef _WIN32
    uint32_t x_low = jrand48(state.data());  // NOLINT(runtime/int)
    uint32_t x_high = jrand48(state.data());  // NOLINT(runtime/int)
    uint64_t x = x_high;
    x <<= 32;
    x += x_low;
    return x % n;
#else
    std::uniform_int_distribution<uint64_t> dist(0, n);
    return dist(state);
#endif
}

double rng_t::randdouble() {
    uint64_t x = rng_t::randuint64(1LL << 53);
    double res = x;
    return res / (1LL << 53);
}

TLS(rng_t, rng)

void system_random_bytes(void *out, int64_t nbytes) {
#ifndef _WIN32
    blocking_read_file_stream_t urandom;
    guarantee(urandom.init("/dev/urandom"), "failed to open /dev/urandom to initialize thread rng");
    int64_t readres = force_read(&urandom, out, nbytes);
    guarantee(readres == nbytes);
#else
    HCRYPTPROV hProv;
    BOOL res = CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT);
    guarantee_winerr(res, "CryptAcquireContext failed");
    defer_t cleanup([&]{ CryptReleaseContext(hProv, 0); });
    res = CryptGenRandom(hProv, nbytes, static_cast<BYTE*>(out));
    guarantee_winerr(res, "CryptGenRandom failed");
#endif
}

int randint(int n) {
    return TLS_get_rng().randint(n);
}

uint64_t randuint64(uint64_t n) {
    return TLS_get_rng().randuint64(n);
}

size_t randsize(size_t n) {
    guarantee(n > 0, "non-positive argument for randint's [0,n) interval");
    size_t ret = 0;
    size_t i = SIZE_MAX;
    while (i != 0) {
        int x = randint(0x10000);
        ret = ret * 0x10000 + x;
        i /= 0x10000;
    }
    return ret % n;
}

double randdouble() {
	return TLS_get_rng().randdouble();

}

bool begins_with_minus(const char *string) {
    while (isspace(*string)) string++;
    return *string == '-';
}

int64_t strtoi64_strict(const char *string, const char **end, int base) {
    CT_ASSERT(sizeof(long long) == sizeof(int64_t));  // NOLINT(runtime/int)
    long long result = strtoll(string, const_cast<char **>(end), base);  // NOLINT(runtime/int)
    if ((result == LLONG_MAX || result == LLONG_MIN) && get_errno() == ERANGE) {
        *end = string;
        return 0;
    }
    return result;
}

uint64_t strtou64_strict(const char *string, const char **end, int base) {
    if (begins_with_minus(string)) {
        *end = string;
        return 0;
    }
    CT_ASSERT(sizeof(unsigned long long) == sizeof(uint64_t));  // NOLINT(runtime/int)
    unsigned long long result = strtoull(string, const_cast<char **>(end), base);  // NOLINT(runtime/int)
    if (result == ULLONG_MAX && get_errno() == ERANGE) {
        *end = string;
        return 0;
    }
    return result;
}

bool strtoi64_strict(const std::string &str, int base, int64_t *out_result) {
    const char *end;
    int64_t result = strtoi64_strict(str.c_str(), &end,  base);
    if (end != str.c_str() + str.length() || (result == 0 && end == str.c_str())) {
        *out_result = 0;
        return false;
    } else {
        *out_result = result;
        return true;
    }
}

bool strtou64_strict(const std::string &str, int base, uint64_t *out_result) {
    const char *end;
    uint64_t result = strtou64_strict(str.c_str(), &end,  base);
    if (end != str.c_str() + str.length() || (result == 0 && end == str.c_str())) {
        *out_result = 0;
        return false;
    } else {
        *out_result = result;
        return true;
    }
}

bool notf(bool x) {
    return !x;
}

std::string vstrprintf(const char *format, va_list ap) {
    printf_buffer_t buf(ap, format);

    return std::string(buf.data(), buf.data() + buf.size());
}

std::string strprintf(const char *format, ...) {
    std::string ret;

    va_list ap;
    va_start(ap, format);

    printf_buffer_t buf(ap, format);

    ret.assign(buf.data(), buf.data() + buf.size());

    va_end(ap);

    return ret;
}

bool hex_to_int(char c, int *out) {
    if (c >= '0' && c <= '9') {
        *out = c - '0';
        return true;
    } else if (c >= 'a' && c <= 'f') {
        *out = c - 'a' + 10;
        return true;
    } else if (c >= 'A' && c <= 'F') {
        *out = c - 'A' + 10;
        return true;
    } else {
        return false;
    }
}

char int_to_hex(int x) {
    rassert(x >= 0 && x < 16);
    if (x < 10) {
        return '0' + x;
    } else {
        return 'A' + x - 10;
    }
}

bool blocking_read_file(const char *path, std::string *contents_out) {
#ifndef _WIN32
    scoped_fd_t fd;

    {
        int res;
        do {
            res = open(path, O_RDONLY);
        } while (res == -1 && get_errno() == EINTR);

        if (res == -1) {
            return false;
        }
        fd.reset(res);
    }

    std::string ret;

    char buf[4096];
    for (;;) {
        ssize_t res;
        do {
            res = read(fd.get(), buf, sizeof(buf));
        } while (res == -1 && get_errno() == EINTR);

        if (res == -1) {
            return false;
        }

        if (res == 0) {
            *contents_out = std::move(ret);
            return true;
        }

        ret.append(buf, buf + res);
    }
#else
	HANDLE hFile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return false;
	defer_t cleanup([&] { CloseHandle(hFile); });
	LARGE_INTEGER fileSize;
	BOOL res = GetFileSizeEx(hFile, &fileSize);
	if (!res) return false;
	DWORD remaining = fileSize.QuadPart;
	std::string ret;
	ret.resize(remaining);
	size_t index = 0;
	while (remaining > 0) {
		DWORD consumed;
		res = ReadFile(hFile, &ret[index], remaining, &consumed, NULL);
		remaining -= consumed;
		index += consumed;
	}
	*contents_out = std::move(ret);
	return true;
#endif
}

std::string blocking_read_file(const char *path) {
    std::string ret;
    bool success = blocking_read_file(path, &ret);
    guarantee(success);
    return ret;
}


std::string sanitize_for_logger(const std::string &s) {
    std::string sanitized = s;
    for (size_t i = 0; i < sanitized.length(); ++i) {
        if (sanitized[i] == '\n' || sanitized[i] == '\t') {
            sanitized[i] = ' ';
        } else if (sanitized[i] < ' ' || sanitized[i] > '~') {
            sanitized[i] = '?';
        }
    }
    return sanitized;
}

std::string errno_string(int errsv) {
    char buf[250];
    const char *errstr = errno_string_maybe_using_buffer(errsv, buf, sizeof(buf));
    return std::string(errstr);
}

int remove_directory_helper(const char *path, ...) {
    logNTC("In recursion: removing file %s\n", path);
#ifdef _WIN32
    DWORD attrs = GetFileAttributes(path);
    guarantee_winerr(attrs != INVALID_FILE_ATTRIBUTES);
    BOOL res;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        res = RemoveDirectory(path);
    } else {
        res = DeleteFile(path);
    }
    // guarantee_winerr(res, "failed to delete '%s'", path); // TODO ATN: this segfaults? 
    if (!res) {
        crash("failed to remove: %s: %s", path, winerr_string(GetLastError()).c_str());
    }
#else
    int res = ::remove(path);
    guarantee_err(res == 0, "Fatal error: failed to delete '%s'.", path);
#endif
    return 0;
}

void remove_directory_recursive(const char *dirpath) {
#ifndef _MSC_VER
    // max_openfd is ignored on OS X (which claims the parameter
    // specifies the maximum traversal depth) and used by Linux to
    // limit the number of file descriptors that are open (by opening
    // and closing directories extra times if it needs to go deeper
    // than that).
    const int max_openfd = 128;
    logNTC("Recursively removing directory %s\n", dirpath);
    int res = nftw(dirpath, remove_directory_helper, max_openfd, FTW_PHYS | FTW_MOUNT | FTW_DEPTH);
    guarantee_err(res == 0 || get_errno() == ENOENT, "Trouble while traversing and destroying temporary directory %s.", dirpath);
#else // ATN TODO
    using namespace std::tr2;
    auto go = [](sys::path dir){
        for (auto it : sys::directory_iterator(dir)) {
            remove_directory_helper(it.path().string().c_str());
        }
        remove_directory_helper(dir.string().c_str());
    };
    go(dirpath);
#endif
}

base_path_t::base_path_t(const std::string &path) : path_(path) { }

void base_path_t::make_absolute() {
#ifndef _MSC_VER // TODO ATN
    char absolute_path[PATH_MAX];
    char *res = realpath(path_.c_str(), absolute_path);
    guarantee_err(res != NULL, "Failed to determine absolute path for '%s'", path_.c_str());
    path_.assign(absolute_path);
#else
    char absolute_path[MAX_PATH];
    DWORD size = GetFullPathName(path_.c_str(), sizeof absolute_path, absolute_path, NULL);
    guarantee_winerr(size != 0, "GetFullPathName failed");
    if (size < sizeof absolute_path) {
      path_.assign(absolute_path);
      return;
    }
    std::string long_absolute_path;
    long_absolute_path.resize(size);
    DWORD new_size = GetFullPathName(path_.c_str(), size, &long_absolute_path[0], NULL);
    guarantee_winerr(size != 0, "GetFullPathName failed");
    guarantee(new_size < size, "GetFullPathName: name too long");
    path_ = std::move(long_absolute_path);
#endif
}

const std::string& base_path_t::path() const {
    guarantee(!path_.empty());
    return path_;
}

std::string temporary_directory_path(const base_path_t& base_path) {
    return base_path.path() + "/tmp";
}

bool is_rw_directory(const base_path_t& path) {
#ifndef _WIN32
	if (access(path.path().c_str(), R_OK | F_OK | W_OK) != 0)
        return false;
#else
	if (_access(path.path().c_str(), 06 /* read and write */) != 0)
		return false;
#endif
    struct stat details;
    if (stat(path.path().c_str(), &details) != 0)
        return false;
    return (details.st_mode & S_IFDIR) > 0;
}

void recreate_temporary_directory(const base_path_t& base_path) {
    const base_path_t path(temporary_directory_path(base_path));

    if (is_rw_directory(path) && check_dir_emptiness(path))
        return;
    remove_directory_recursive(path.path().c_str());

    int res;
#ifndef _WIN32
    do {
        res = mkdir(path.path().c_str(), 0755);
    } while (res == -1 && get_errno() == EINTR);
#else
	res = _mkdir(path.path().c_str());
#endif
    guarantee_err(res == 0, "mkdir of temporary directory %s failed",
                  path.path().c_str());

    // Call fsync() on the parent directory to guarantee that the newly
    // created directory's directory entry is persisted to disk.
    warn_fsync_parent_directory(path.path().c_str());
}

// GCC and CLANG are smart enough to optimize out strlen(""), so this works.
// This is the simplist thing I could find that gave warning in all of these
// cases:
// * RETHINKDB_VERSION=
// * RETHINKDB_VERSION=""
// * RETHINKDB_VERSION=1.2
// (the correct case is something like RETHINKDB_VERSION="1.2")
UNUSED static const char _assert_RETHINKDB_VERSION_nonempty = 1/(!!strlen(RETHINKDB_VERSION));
