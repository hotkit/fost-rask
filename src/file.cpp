/*
    Copyright 2015, Proteus Tech Co Ltd. http://www.kirit.com/Rask
    Distributed under the Boost Software License, Version 1.0.
    See accompanying file LICENSE_1_0.txt or copy at
        http://www.boost.org/LICENSE_1_0.txt
*/


#include "file.hpp"
#include <rask/configuration.hpp>

#include <fost/log>
#include <fost/exception/unexpected_eof.hpp>

#include <boost/filesystem.hpp>

#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>


namespace {


    template<typename F> inline
    int syscall(F f) {
        int result{};
        do {
            result = f();
        } while ( result == -1 && errno == EINTR );
        return result;
    }


}


fostlib::string rask::relative_path(
    const fostlib::string &root, const boost::filesystem::path &location
) {
    auto path = fostlib::coerce<fostlib::string>(location);
    if ( path.startswith(root) ) {
        path = path.substr(root.length());
    } else {
        fostlib::exceptions::not_implemented error(
            "Directory is not in tenant root");
        fostlib::insert(error.data(), "root", root);
        fostlib::insert(error.data(), "location", location);
        throw error;
    }
    return path;
}


void rask::allocate_file(const boost::filesystem::path &fn, std::size_t size) {
    if ( not boost::filesystem::exists(fn) ) {
        // Resize the file, probably overkill on the complexity front
        int opened = syscall([&]() {
                const int flags = O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW;
                // user read/write, group read/write, world read
                const int mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH;
                return open(fn.c_str(), flags, mode);
            });
        if ( opened >= 0 ) {
            const int alloc = syscall([&]() {
                    const int mode = 0u;
                    const off_t offset = 0u;
                    return fallocate(opened, mode, offset, size);
                });
            const auto alloc_err = errno;
            syscall([&]() { return close(opened); });
            if ( alloc == -1 ) {
                std::error_code error(alloc_err, std::system_category());
                fostlib::log::error(c_fost_rask)
                    ("", "rask::allocate_file - fallocate")
                    ("filename", fn)
                    ("size", size)
                    ("error",  error.message().c_str());
                throw fostlib::exceptions::unexpected_eof(
                    "Could not change allocate size of the hash database file",
                    error.message().c_str());
            }
        } else {
            std::error_code error(errno, std::system_category());
            fostlib::log::error(c_fost_rask, "open", error.message().c_str());
            throw fostlib::exceptions::not_implemented(
                "Bad file descriptor for hash database file", error.message().c_str());
        }
    } else {
        boost::filesystem::resize_file(fn, size);
    }
}


/*
    rask::stat
*/


rask::stat::stat(int64_t s, fostlib::timestamp ts)
: size(s), modified(ts) {
}


rask::stat::stat(const fostlib::json &j)
: size(fostlib::coerce<int64_t>(j["size"]["bytes"])),
    modified(fostlib::coerce<fostlib::timestamp>(j["modified"]))
{}


fostlib::json fostlib::coercer<fostlib::json, rask::stat>::coerce(const rask::stat &s) {
    fostlib::json j;
    fostlib::insert(j, "size", "bytes", s.size);
    fostlib::insert(j, "modified", s.modified);
    return j;
}


/*
    rask::file_stat
*/


rask::stat rask::file_stat(const boost::filesystem::path &filename) {
    struct ::stat status;
    const auto stat_call = syscall(
        [&filename, &status]() {
            return ::stat(filename.c_str(), &status);
        });
    if ( stat_call == 0 ) {
        return rask::stat{
            status.st_size,
            fostlib::timestamp(
                    boost::posix_time::from_time_t(status.st_mtim.tv_sec)) +
                boost::posix_time::microseconds(status.st_mtim.tv_nsec / 1'000)
        };
    } else {
        fostlib::log::error(c_fost_rask)
            ("", "file_stat")
            ("filename", filename);
        throw fostlib::exceptions::not_implemented(
            "Error when fetching stat for file");
    }
}

