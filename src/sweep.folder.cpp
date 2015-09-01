/*
    Copyright 2015, Proteus Tech Co Ltd. http://www.kirit.com/Rask
    Distributed under the Boost Software License, Version 1.0.
    See accompanying file LICENSE_1_0.txt or copy at
        http://www.boost.org/LICENSE_1_0.txt
*/


#include "hash.hpp"
#include "sweep.folder.hpp"
#include <rask/configuration.hpp>
#include <rask/subscriber.hpp>
#include <rask/tenant.hpp>

#include <f5/threading/eventfd.hpp>
#include <fost/counter>
#include <fost/log>


namespace {
    fostlib::performance p_starts(rask::c_fost_rask, "sweep", "started");
    fostlib::performance p_completed(rask::c_fost_rask, "sweep", "completed");
    fostlib::performance p_swept(rask::c_fost_rask, "sweep", "folders");
    fostlib::performance p_paused(rask::c_fost_rask, "sweep", "pauses");

    void sweep(
        rask::workers &w, std::shared_ptr<rask::tenant> tenant,
        boost::filesystem::path folder
    ) {
        if ( !tenant->subscription ) {
            throw fostlib::exceptions::null(
                "Trying to sweep a tenant that has no subscription");
        }
        boost::asio::spawn(w.files.get_io_service(),
            [&w, tenant, folder = std::move(folder)](boost::asio::yield_context yield) {
                f5::eventfd::limiter limit(w.hashes.get_io_service(), yield, 2);
                ++p_swept;
                if ( !boost::filesystem::is_directory(folder) ) {
                    throw fostlib::exceptions::not_implemented(
                        "Trying to recurse into a non-directory",
                        fostlib::coerce<fostlib::string>(folder));
                }
                fostlib::log::debug(rask::c_fost_rask, "Sweep recursing into folder", folder);
                tenant->subscription->local_change(
                    folder, rask::tenant::directory_inode, rask::create_directory_out);
                w.notify.watch(tenant, folder);
                std::size_t files = 0, directories = 0, ignored = 0;
                using d_iter = boost::filesystem::recursive_directory_iterator;
                for ( auto inode = d_iter(folder), end = d_iter(); inode != end; ++inode ) {
                    fostlib::log::debug(rask::c_fost_rask, "Directory sweep", inode->path());
                    if ( inode->status().type() == boost::filesystem::directory_file ) {
                        ++directories;
                        auto directory = inode->path();
                        tenant->subscription->local_change(directory,
                            rask::tenant::directory_inode, rask::create_directory_out);
                        w.notify.watch(tenant, directory);
                    } else if ( inode->status().type() == boost::filesystem::regular_file ) {
                        ++files;
                        auto filename = inode->path();
                        auto task(++limit);
                        tenant->subscription->local_change(filename,
                            rask::tenant::file_inode, rask::file_exists_out,
                            [&w, filename, tenant, task](
                                const rask::tick &, fostlib::json inode
                            ) {
                                rask::rehash_file(w, *tenant->subscription, filename, inode,
                                    [task] () {
                                        task->done(
                                            [](const auto &error, auto bytes) {
                                                fostlib::log::error(rask::c_fost_rask)
                                                    ("", "Whilst notifying parent task "
                                                        "that this one has completed.")
                                                    ("error", error.message().c_str())
                                                    ("bytes", bytes);
                                            });
                                    });
                                /// There might be some worry that there is
                                /// a race here between this code and the
                                /// above call to `rehash_file`. This callback
                                /// is executed inside the transaction that
                                /// updates the beanbag which means that
                                /// it is guaranteed to finish executing
                                /// before the rehash gets its own shot at
                                /// updating the database hash.
                                return inode;
                            });
                    } else {
                        ++ignored;
                    }
                }
                fostlib::log::info(rask::c_fost_rask)
                    ("", "Swept folder")
                    ("folder", folder)
                    ("directories", directories)
                    ("files", files)
                    ("ignored", ignored);
            });
    }
}


void rask::start_sweep(
    workers &w, std::shared_ptr<tenant> tenant, boost::filesystem::path folder
) {
    w.files.get_io_service().post(
        [&w, tenant, folder = std::move(folder)]() {
            sweep(w, tenant, folder);
        });
}

