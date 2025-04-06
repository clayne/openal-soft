
#include "config.h"

#include "helpers.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "almalloc.h"
#include "alnumeric.h"
#include "alspan.h"
#include "alstring.h"
#include "filesystem.h"
#include "logging.h"
#include "strutils.h"


namespace {

using namespace std::string_view_literals;

std::mutex gSearchLock;

void DirectorySearch(const fs::path &path, const std::string_view ext,
    std::vector<std::string> *const results)
{
    const auto base = results->size();

    try {
        auto fpath = path.lexically_normal();
        if(!fs::exists(fpath))
            return;

        TRACE("Searching {} for *{}", al::u8_as_char(fpath.u8string()), ext);
        for(auto&& dirent : fs::directory_iterator{fpath})
        {
            auto&& entrypath = dirent.path();
            if(!entrypath.has_extension())
                continue;

            if(fs::status(entrypath).type() != fs::file_type::regular)
                continue;
            const auto u8ext = entrypath.extension().u8string();
            if(al::case_compare(al::u8_as_char(u8ext), ext) == 0)
                results->emplace_back(al::u8_as_char(entrypath.u8string()));
        }
    }
    catch(std::exception& e) {
        ERR("Exception enumerating files: {}", e.what());
    }

    const auto newlist = al::span{*results}.subspan(base);
    std::sort(newlist.begin(), newlist.end());
    for(const auto &name : newlist)
        TRACE(" got {}", name);
}

} // namespace

#ifdef _WIN32

#include <cctype>
#include <shlobj.h>

const PathNamePair &GetProcBinary()
{
    auto get_procbin = []
    {
#if !ALSOFT_UWP
        DWORD pathlen{256};
        auto fullpath = std::wstring(pathlen, L'\0');
        DWORD len{GetModuleFileNameW(nullptr, fullpath.data(), pathlen)};
        while(len == fullpath.size())
        {
            pathlen <<= 1;
            if(pathlen == 0)
            {
                /* pathlen overflow (more than 4 billion characters??) */
                len = 0;
                break;
            }
            fullpath.resize(pathlen);
            len = GetModuleFileNameW(nullptr, fullpath.data(), pathlen);
        }
        if(len == 0)
        {
            ERR("Failed to get process name: error {}", GetLastError());
            return PathNamePair{};
        }

        fullpath.resize(len);
#else
        if(__argc < 1 || !__wargv)
        {
            ERR("Failed to get process name: __argc = {}, __wargv = {}", __argc,
                static_cast<void*>(__wargv));
            return PathNamePair{};
        }
        const WCHAR *exePath{__wargv[0]};
        if(!exePath)
        {
            ERR("Failed to get process name: __wargv[0] == nullptr");
            return PathNamePair{};
        }
        std::wstring fullpath{exePath};
#endif
        std::replace(fullpath.begin(), fullpath.end(), L'/', L'\\');

        PathNamePair res{};
        if(auto seppos = fullpath.rfind(L'\\'); seppos < fullpath.size())
        {
            res.path = wstr_to_utf8(std::wstring_view{fullpath}.substr(0, seppos));
            res.fname = wstr_to_utf8(std::wstring_view{fullpath}.substr(seppos+1));
        }
        else
            res.fname = wstr_to_utf8(fullpath);

        TRACE("Got binary: {}, {}", res.path, res.fname);
        return res;
    };
    static const PathNamePair procbin{get_procbin()};
    return procbin;
}

namespace {

#if !ALSOFT_UWP && !defined(_GAMING_XBOX)
struct CoTaskMemDeleter {
    void operator()(void *mem) const { CoTaskMemFree(mem); }
};
#endif

} // namespace

auto SearchDataFiles(const std::string_view ext) -> std::vector<std::string>
{
    auto srchlock = std::lock_guard{gSearchLock};

    /* Search the app-local directory. */
    auto results = std::vector<std::string>{};
    if(auto localpath = al::getenv(L"ALSOFT_LOCAL_PATH"))
        DirectorySearch(*localpath, ext, &results);
    else if(auto curpath = fs::current_path(); !curpath.empty())
        DirectorySearch(curpath, ext, &results);

    return results;
}

auto SearchDataFiles(const std::string_view ext, const std::string_view subdir)
    -> std::vector<std::string>
{
    std::lock_guard<std::mutex> srchlock{gSearchLock};

    /* If the path is absolute, use it directly. */
    std::vector<std::string> results;
    auto path = fs::path(al::char_as_u8(subdir));
    if(path.is_absolute())
    {
        DirectorySearch(path, ext, &results);
        return results;
    }

#if !ALSOFT_UWP && !defined(_GAMING_XBOX)
    /* Search the local and global data dirs. */
    for(const auto &folderid : std::array{FOLDERID_RoamingAppData, FOLDERID_ProgramData})
    {
        std::unique_ptr<WCHAR,CoTaskMemDeleter> buffer;
        const HRESULT hr{SHGetKnownFolderPath(folderid, KF_FLAG_DONT_UNEXPAND, nullptr,
            al::out_ptr(buffer))};
        if(FAILED(hr) || !buffer || !*buffer)
            continue;

        DirectorySearch(fs::path{buffer.get()}/path, ext, &results);
    }
#endif

    return results;
}

void SetRTPriority()
{
#if !ALSOFT_UWP
    if(RTPrioLevel > 0)
    {
        if(!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
            ERR("Failed to set priority level for thread");
    }
#endif
}

#else

#include <cerrno>
#include <dirent.h>
#include <unistd.h>
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif
#ifdef __HAIKU__
#include <FindDirectory.h>
#endif
#ifdef HAVE_PROC_PIDPATH
#include <libproc.h>
#endif
#if defined(HAVE_PTHREAD_SETSCHEDPARAM) && !defined(__OpenBSD__)
#include <pthread.h>
#include <sched.h>
#endif
#if HAVE_RTKIT
#include <sys/resource.h>

#include "dbus_wrap.h"
#include "rtkit.h"
#ifndef RLIMIT_RTTIME
#define RLIMIT_RTTIME 15
#endif
#endif

const PathNamePair &GetProcBinary()
{
    auto get_procbin = []
    {
        std::string pathname;
#ifdef __FreeBSD__
        size_t pathlen{};
        std::array<int,4> mib{{CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1}};
        if(sysctl(mib.data(), mib.size(), nullptr, &pathlen, nullptr, 0) == -1)
            WARN("Failed to sysctl kern.proc.pathname: {}",
                std::generic_category().message(errno));
        else
        {
            auto procpath = std::vector<char>(pathlen+1, '\0');
            sysctl(mib.data(), mib.size(), procpath.data(), &pathlen, nullptr, 0);
            pathname = procpath.data();
        }
#endif
#ifdef HAVE_PROC_PIDPATH
        if(pathname.empty())
        {
            std::array<char,PROC_PIDPATHINFO_MAXSIZE> procpath{};
            const pid_t pid{getpid()};
            if(proc_pidpath(pid, procpath.data(), procpath.size()) < 1)
                ERR("proc_pidpath({}, ...) failed: {}", pid,
                    std::generic_category().message(errno));
            else
                pathname = procpath.data();
        }
#endif
#ifdef __HAIKU__
        if(pathname.empty())
        {
            std::array<char,PATH_MAX> procpath{};
            if(find_path(B_APP_IMAGE_SYMBOL, B_FIND_PATH_IMAGE_PATH, NULL, procpath.data(), procpath.size()) == B_OK)
                pathname = procpath.data();
        }
#endif
#ifndef __SWITCH__
        if(pathname.empty())
        {
            const std::array SelfLinkNames{
                "/proc/self/exe"sv,
                "/proc/self/file"sv,
                "/proc/curproc/exe"sv,
                "/proc/curproc/file"sv,
            };

            for(const std::string_view name : SelfLinkNames)
            {
                try {
                    if(!fs::exists(name))
                        continue;
                    if(auto path = fs::read_symlink(name); !path.empty())
                    {
                        pathname = al::u8_as_char(path.u8string());
                        break;
                    }
                }
                catch(std::exception& e) {
                    WARN("Exception getting symlink {}: {}", name, e.what());
                }
            }
        }
#endif

        PathNamePair res{};
        if(auto seppos = pathname.rfind('/'); seppos < pathname.size())
        {
            res.path = std::string_view{pathname}.substr(0, seppos);
            res.fname = std::string_view{pathname}.substr(seppos+1);
        }
        else
            res.fname = pathname;

        TRACE("Got binary: \"{}\", \"{}\"", res.path, res.fname);
        return res;
    };
    static const PathNamePair procbin{get_procbin()};
    return procbin;
}

auto SearchDataFiles(const std::string_view ext) -> std::vector<std::string>
{
    auto srchlock = std::lock_guard{gSearchLock};

    /* Search the app-local directory. */
    auto results = std::vector<std::string>{};
    if(auto localpath = al::getenv("ALSOFT_LOCAL_PATH"))
        DirectorySearch(*localpath, ext, &results);
    else if(auto curpath = fs::current_path(); !curpath.empty())
        DirectorySearch(curpath, ext, &results);

    return results;
}

auto SearchDataFiles(const std::string_view ext, const std::string_view subdir)
    -> std::vector<std::string>
{
    std::lock_guard<std::mutex> srchlock{gSearchLock};

    std::vector<std::string> results;
    auto path = fs::path(al::char_as_u8(subdir));
    if(path.is_absolute())
    {
        DirectorySearch(path, ext, &results);
        return results;
    }

    /* Search local data dir */
    if(auto datapath = al::getenv("XDG_DATA_HOME"))
        DirectorySearch(fs::path{*datapath}/path, ext, &results);
    else if(auto homepath = al::getenv("HOME"))
        DirectorySearch(fs::path{*homepath}/".local/share"/path, ext, &results);

    /* Search global data dirs */
    std::string datadirs{al::getenv("XDG_DATA_DIRS").value_or("/usr/local/share/:/usr/share/")};

    size_t curpos{0u};
    while(curpos < datadirs.size())
    {
        size_t nextpos{datadirs.find(':', curpos)};

        std::string_view pathname{(nextpos != std::string::npos)
            ? std::string_view{datadirs}.substr(curpos, nextpos++ - curpos)
            : std::string_view{datadirs}.substr(curpos)};
        curpos = nextpos;

        if(!pathname.empty())
            DirectorySearch(fs::path{pathname}/path, ext, &results);
    }

#ifdef ALSOFT_INSTALL_DATADIR
    /* Search the installation data directory */
    if(auto instpath = fs::path{ALSOFT_INSTALL_DATADIR}; !instpath.empty())
        DirectorySearch(instpath/path, ext, &results);
#endif

    return results;
}

namespace {

bool SetRTPriorityPthread(int prio [[maybe_unused]])
{
    int err{ENOTSUP};
#if defined(HAVE_PTHREAD_SETSCHEDPARAM) && !defined(__OpenBSD__)
    /* Get the min and max priority for SCHED_RR. Limit the max priority to
     * half, for now, to ensure the thread can't take the highest priority and
     * go rogue.
     */
    int rtmin{sched_get_priority_min(SCHED_RR)};
    int rtmax{sched_get_priority_max(SCHED_RR)};
    rtmax = (rtmax-rtmin)/2 + rtmin;

    struct sched_param param{};
    param.sched_priority = std::clamp(prio, rtmin, rtmax);
#ifdef SCHED_RESET_ON_FORK
    err = pthread_setschedparam(pthread_self(), SCHED_RR|SCHED_RESET_ON_FORK, &param);
    if(err == EINVAL)
#endif
        err = pthread_setschedparam(pthread_self(), SCHED_RR, &param);
    if(err == 0) return true;
#endif
    WARN("pthread_setschedparam failed: {} ({})", std::generic_category().message(err), err);
    return false;
}

bool SetRTPriorityRTKit(int prio [[maybe_unused]])
{
#if HAVE_RTKIT
    if(!HasDBus())
    {
        WARN("D-Bus not available");
        return false;
    }
    dbus::Error error;
    dbus::ConnectionPtr conn{dbus_bus_get(DBUS_BUS_SYSTEM, &error.get())};
    if(!conn)
    {
        WARN("D-Bus connection failed with {}: {}", error->name, error->message);
        return false;
    }

    /* Don't stupidly exit if the connection dies while doing this. */
    dbus_connection_set_exit_on_disconnect(conn.get(), false);

    int nicemin{};
    int err{rtkit_get_min_nice_level(conn.get(), &nicemin)};
    if(err == -ENOENT)
    {
        err = std::abs(err);
        ERR("Could not query RTKit: {} ({})", std::generic_category().message(err), err);
        return false;
    }
    int rtmax{rtkit_get_max_realtime_priority(conn.get())};
    TRACE("Maximum real-time priority: {}, minimum niceness: {}", rtmax, nicemin);

    auto limit_rttime = [](DBusConnection *c) -> int
    {
        using ulonglong = unsigned long long;
        long long maxrttime{rtkit_get_rttime_usec_max(c)};
        if(maxrttime <= 0) return static_cast<int>(std::abs(maxrttime));
        const ulonglong umaxtime{static_cast<ulonglong>(maxrttime)};

        struct rlimit rlim{};
        if(getrlimit(RLIMIT_RTTIME, &rlim) != 0)
            return errno;

        TRACE("RTTime max: {} (hard: {}, soft: {})", umaxtime, rlim.rlim_max, rlim.rlim_cur);
        if(rlim.rlim_max > umaxtime)
        {
            rlim.rlim_max = static_cast<rlim_t>(std::min<ulonglong>(umaxtime,
                std::numeric_limits<rlim_t>::max()));
            rlim.rlim_cur = std::min(rlim.rlim_cur, rlim.rlim_max);
            if(setrlimit(RLIMIT_RTTIME, &rlim) != 0)
                return errno;
        }
        return 0;
    };
    if(rtmax > 0)
    {
        if(AllowRTTimeLimit)
        {
            err = limit_rttime(conn.get());
            if(err != 0)
                WARN("Failed to set RLIMIT_RTTIME for RTKit: {} ({})",
                    std::generic_category().message(err), err);
        }

        /* Limit the maximum real-time priority to half. */
        rtmax = (rtmax+1)/2;
        prio = std::clamp(prio, 1, rtmax);

        TRACE("Making real-time with priority {} (max: {})", prio, rtmax);
        err = rtkit_make_realtime(conn.get(), 0, prio);
        if(err == 0) return true;

        err = std::abs(err);
        WARN("Failed to set real-time priority: {} ({})",
            std::generic_category().message(err), err);
    }
    /* Don't try to set the niceness for non-Linux systems. Standard POSIX has
     * niceness as a per-process attribute, while the intent here is for the
     * audio processing thread only to get a priority boost. Currently only
     * Linux is known to have per-thread niceness.
     */
#ifdef __linux__
    if(nicemin < 0)
    {
        TRACE("Making high priority with niceness {}", nicemin);
        err = rtkit_make_high_priority(conn.get(), 0, nicemin);
        if(err == 0) return true;

        err = std::abs(err);
        WARN("Failed to set high priority: {} ({})", std::generic_category().message(err), err);
    }
#endif /* __linux__ */

#else

    WARN("D-Bus not supported");
#endif
    return false;
}

} // namespace

void SetRTPriority()
{
    if(RTPrioLevel <= 0)
        return;

    if(SetRTPriorityPthread(RTPrioLevel))
        return;
    if(SetRTPriorityRTKit(RTPrioLevel))
        return;
}

#endif
