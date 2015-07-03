/*
    Copyright 2015, Proteus Tech Co Ltd. http://www.kirit.com/Rask
    Distributed under the Boost Software License, Version 1.0.
    See accompanying file LICENSE_1_0.txt or copy at
        http://www.boost.org/LICENSE_1_0.txt
*/


#include "sweep.inodes.hpp"
#include "tree.hpp"
#include <rask/tenant.hpp>

#include <fost/counter>
#include <fost/log>


namespace {
    fostlib::performance p_directory(rask::c_fost_rask, "inode", "directory");
    fostlib::performance p_file(rask::c_fost_rask, "inode", "file");
    fostlib::performance p_unknown(rask::c_fost_rask, "inode", "unknown");

    struct closure {
        std::shared_ptr<rask::tenant> tenant;
        boost::filesystem::path folder;
        rask::tree::const_iterator position;
        rask::tree::const_iterator end;

        closure(std::shared_ptr<rask::tenant> t, boost::filesystem::path f)
        : tenant(t), folder(std::move(f)),
                position(t->inodes().begin()), end(t->inodes().end()) {
        }
    };
    void check_block(rask::workers &w, std::shared_ptr<closure> c) {
        for ( ; c->position != c->end; ++c->position ) {
            auto inode = *c->position;
            auto filetype = inode["filetype"];
            auto filename = fostlib::coerce<boost::filesystem::path>(c->position.key());
            if ( filetype == rask::tenant::directory_inode ) {
                ++p_directory;
                w.notify.watch(c->tenant, filename);
            } else if ( filetype == rask::tenant::move_inode_out ) {
                ++p_file;
            } else {
                ++p_unknown;
                fostlib::log::error(rask::c_fost_rask)
                    ("", "Sweeping inodes -- unknown filetype")
                    ("filetype", filetype)
                    ("inode", inode);
            }
        }
    }
}


void rask::sweep_inodes(
    workers &w, std::shared_ptr<tenant> t, boost::filesystem::path f
) {
    auto c = std::make_shared<closure>(t, std::move(f));
    w.high_latency.get_io_service().post(
        [&w, c]() {
            check_block(w, c);
        });
}

