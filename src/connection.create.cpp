/*
    Copyright 2015, Proteus Tech Co Ltd. http://www.kirit.com/Rask
    Distributed under the Boost Software License, Version 1.0.
    See accompanying file LICENSE_1_0.txt or copy at
        http://www.boost.org/LICENSE_1_0.txt
*/


#include <rask/tenant.hpp>


rask::connection::out rask::create_directory_out(
    rask::tenant &tenant, const rask::tick &priority,
    const fostlib::string &name
) {
    connection::out packet(0x91);
    packet << priority << tenant.name() << name;
    return std::move(packet);
}


void rask::create_directory(rask::connection::in &packet) {
    auto logger(fostlib::log::info(c_fost_rask));
    logger("", "Create directory");
    auto priority(packet.read<tick>());
    logger("priority", priority);
    auto tenant(known_tenant(packet.read<fostlib::string>()));
    auto name(packet.read<fostlib::string>());
    logger
        ("tenant", tenant->name())
        ("name", name);
    packet.socket->workers.high_latency.get_io_service().post(
        [tenant, name = std::move(name), priority](){
            auto location = tenant->local_path() /
                fostlib::coerce<boost::filesystem::path>(name);
            tenant->remote_change(location, tenant::directory_inode, priority);
            boost::filesystem::create_directories(location);
        });
}

