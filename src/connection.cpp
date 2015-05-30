/*
    Copyright 2015, Proteus Tech Co Ltd. http://www.kirit.com/Rask
    Distributed under the Boost Software License, Version 1.0.
    See accompanying file LICENSE_1_0.txt or copy at
        http://www.boost.org/LICENSE_1_0.txt
*/


#include "connection.hpp"
#include <rask/workers.hpp>

#include <fost/log>

#include <mutex>


namespace {
    std::mutex g_mutex;
    std::vector<std::weak_ptr<rask::connection>> g_connections;
}


rask::connection::connection(workers &w)
: cnx(w.low_latency.io_service), sender(w.low_latency.io_service) {
}


void rask::connection::version() {
    static unsigned char data[] = {0x03, 0x80, 0x01};
    async_write(cnx, boost::asio::buffer(data), sender.wrap(
        [](const boost::system::error_code& error, std::size_t bytes) {
            fostlib::log::debug("Version block sent", int(data[2]));
        }));
}

