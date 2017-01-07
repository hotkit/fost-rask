/*
    Copyright 2015-2017, Proteus Tech Co Ltd. http://www.kirit.com/Rask
    Distributed under the Boost Software License, Version 1.0.
    See accompanying file LICENSE_1_0.txt or copy at
        http://www.boost.org/LICENSE_1_0.txt
*/


#include "peer.hpp"
#include <rask/clock.hpp>
#include <rask/configuration.hpp>
#include <rask/workers.hpp>

#include <fost/counter>
#include <fost/log>

#include <boost/asio/spawn.hpp>

#include <mutex>


namespace {
    std::mutex g_mutex;
    std::vector<std::weak_ptr<rask::connection>> g_connections;

    fostlib::performance p_queued(rask::c_fost_rask, "packets", "queued");
    fostlib::performance p_sends(rask::c_fost_rask, "packets", "sends");
    fostlib::performance p_spill(rask::c_fost_rask, "packets", "spills");
    fostlib::performance p_received(rask::c_fost_rask, "packets", "received");
    fostlib::performance p_processed(rask::c_fost_rask, "packets", "processed");
}


void rask::monitor_connection(std::shared_ptr<rask::connection> socket) {
    std::unique_lock<std::mutex> lock(g_mutex);
    /// Try to find an empty slot
    for ( auto &w : g_connections ) {
        auto slot(w.lock());
        if ( !slot ) {
            /// And if there is one then use it
            w = socket;
            return;
        }
    }
    /// Otherwise just stick this one onto the end
    g_connections.push_back(socket);
}


std::size_t rask::broadcast(std::function<rask::connection::out(void)> fn) {
    std::size_t to = 0;
    std::unique_lock<std::mutex> lock(g_mutex);
    for ( auto &w : g_connections ) {
        auto slot(w.lock());
        if ( slot ) {
            if ( slot->queue(fn) ) ++to;
        }
    }
    return to;
}


void rask::read_and_process(std::shared_ptr<rask::connection> socket) {
    socket->start_sending();
    boost::asio::spawn(socket->cnx.get_io_service(),
        [socket](boost::asio::yield_context yield) {
            while ( socket->cnx.is_open() ) {
                try {
                    fostlib::json size_bytes = fostlib::json::array_t();
                    boost::asio::async_read(socket->cnx, socket->input_buffer,
                        boost::asio::transfer_exactly(2), yield);
                    std::size_t packet_size = socket->input_buffer.sbumpc();
                    fostlib::push_back(size_bytes, int64_t(packet_size));
                    if ( packet_size > 0xf8 ) {
                        const int bytes = packet_size - 0xf8;
                        boost::asio::async_read(socket->cnx, socket->input_buffer,
                            boost::asio::transfer_exactly(bytes), yield);
                        packet_size = 0u;
                        for ( auto i = 0; i != bytes; ++i ) {
                            unsigned char byte = socket->input_buffer.sbumpc();
                            fostlib::push_back(size_bytes, int64_t(byte));
                            packet_size = (packet_size << 8) + byte;
                        }
                    } else if ( packet_size >= 0x80 ) {
                        socket->cnx.close();
                        throw fostlib::exceptions::not_implemented(
                            "Invalid packet size control byte",
                            fostlib::coerce<fostlib::string>(packet_size));
                    }
                    unsigned char control = socket->input_buffer.sbumpc();
                    boost::asio::async_read(socket->cnx, socket->input_buffer,
                        boost::asio::transfer_exactly(packet_size), yield);
                    fostlib::log::debug(c_fost_rask)
                        ("", "Got packet")
                        ("connection", socket->id)
                        ("bytes", size_bytes)
                        ("control", control)
                        ("size", packet_size);
                    connection::in packet(socket, packet_size);
                    if ( control == 0x80 ) {
                        receive_version(packet);
                    } else if ( control == 0x81 ) {
                        tenant_packet(packet);
                    } else if ( control == 0x82 ) {
                        tenant_hash_packet(packet);
                    } else if ( control == 0x83 ) {
                        file_hash_without_priority(packet);
                    } else if ( control == 0x90 ) {
                        file_exists(packet);
                    } else if ( control == 0x91 ) {
                        create_directory(packet);
                    } else if ( control == 0x93 ) {
                        move_out(packet);
                    } else if ( control == 0x9f ) {
                        file_data_block(packet);
                    } else {
                        fostlib::log::warning(c_fost_rask)
                            ("", "Unknown control byte received")
                            ("connection", socket->id)
                            ("control", int(control))
                            ("packet-size", packet_size);
                    }
                    socket->reset_heartbeat(control != 0x80);
                } catch ( fostlib::exceptions::exception &e ) {
                    fostlib::log::error(c_fost_rask)
                        ("", "read_and_process caught an exception")
                        ("connection", socket->id)
                        ("exception", e.as_json());
                    return;
                } catch ( std::exception &e ) {
                    fostlib::log::error(c_fost_rask)
                        ("", "read_and_process caught an exception")
                        ("connection", socket->id)
                        ("exception", e.what());
                    return;
                }
            }
        });
}


/*
    rask::connection
*/


const std::size_t queue_capactiy = 256;


rask::connection::connection(rask::workers &w)
: workers(w), cnx(w.io.get_io_service()),
        sending_strand(w.io.get_io_service()),
        sender(w.io.get_io_service()),
        heartbeat(w.io.get_io_service()),
        identity(0), packets(queue_capactiy) {
    input_buffer.prepare(buffer_size);
}


rask::connection::~connection() {
    fostlib::log::debug(c_fost_rask, "Connection closed", id);
}


bool rask::connection::queue(std::function<out(void)> fn) {
    bool added = false;
    packets.push_back(
        [fn, &added]() {
            ++p_queued;
            added = true;
            return fn;
        },
        [](auto &) {
            ++p_spill;
            return false;
        });
    /// We notify the consumer here and not in the lambda above because
    /// when that lambda executes the function is not yet in the buffer so
    /// there would be race between getting it there and the consumer
    /// pulling it off. Because the queue is protected by a mutex this
    /// can't actually be a problem, until the queue is re-implemented
    /// to be lock free, and then it will be. Doing it at the end is always
    /// safe.
    if ( added ) {
        sender.produced();
    }
    return added;
}


void rask::connection::start_sending() {
    fostlib::log::info(c_fost_rask)
        ("", "Starting sending on connection")
        ("connection", id);
    auto self(shared_from_this());
    queue(send_version);
    boost::asio::spawn(sending_strand,
        [self](boost::asio::yield_context yield) {
            while ( self->cnx.is_open() ) {
                try {
                    auto queued = self->sender.consume(yield);
                    while ( self->cnx.is_open() && queued > 0 ) {
                        auto packet = self->packets.pop_front(
                            fostlib::nullable<std::function<out(void)>>()).value();
                        --queued;
                        packet()(self->cnx, yield);
                        ++p_sends;
                        self->reset_heartbeat(true);
                    }
                } catch ( fostlib::exceptions::exception &e ) {
                    fostlib::log::error(c_fost_rask)
                        ("", "connection::start_sending caught an exception")
                        ("connection", self->id)
                        ("exception", e.as_json());
                    return;
                } catch ( std::exception &e ) {
                    fostlib::log::error(c_fost_rask)
                        ("", "connection::start_sending caught an exception")
                        ("connection", self->id)
                        ("exception", e.what());
                    return;
                }
            }
        });
}


void rask::connection::reset_heartbeat(bool hb) {
    if ( hb ) {
        heartbeat.expires_from_now(boost::posix_time::seconds(5));
        heartbeat.async_wait(
            [self = shared_from_this()](const boost::system::error_code &error) {
                if ( !error ) {
                    self->queue(send_version);
                }
            });
    }
    if ( restart ) {
        reset_watchdog(workers, restart);
    }
}


/*
    rask::connection::reconnect
*/


rask::connection::reconnect::reconnect(rask::workers &w, const fostlib::json &conf)
: configuration(conf), watchdog(w.io.get_io_service()) {
}


/*
    rask::connection::out
*/


rask::connection::out &operator << (rask::connection::out &o, const rask::tick &t) {
    return o << t.time() << t.server();
}


rask::connection::out &operator << (rask::connection::out &o, const fostlib::string &s) {
    // This implementation only works for narrow character string
    return o.size_sequence(s.native_length()) <<
        fostlib::const_memory_block(s.c_str(), s.c_str() + s.native_length());
}


rask::connection::out &operator << (rask::connection::out &o, const fostlib::const_memory_block b) {
    if ( b.first != b.second ) {
        o.bytes(fostlib::array_view<char>(reinterpret_cast<const char *>(b.first),
            reinterpret_cast<const char*>(b.second) - reinterpret_cast<const char*>(b.first)));
    }
    return o;
}


rask::connection::out &operator << (rask::connection::out &o, const std::vector<unsigned char> &v) {
    if ( v.size() ) {
        o.bytes(fostlib::array_view<char>(reinterpret_cast<const char *>(v.data()), v.size()));
    }
    return o;
}


/*
    rask::connection::in
*/


rask::connection::in::in(std::shared_ptr<connection> socket, std::size_t s)
: socket(socket), remaining(s) {
    ++p_received;
}


rask::connection::in::~in() {
     while ( remaining-- ) socket->input_buffer.sbumpc();
     ++p_processed;
}


void rask::connection::in::check(std::size_t b) const {
    if ( remaining < b )
        throw fostlib::exceptions::unexpected_eof(
            "Not enough data in the buffer for this packet");
}


std::size_t rask::connection::in::size_control() {
    auto header(read<uint8_t>());
    if ( header < 0x80 ) {
        return header;
    } else if ( header > 0xf8 ) {
        std::size_t bytes = 0;
        /// We disallow anything too big
        while ( header > 0xf8 && header <= 0xfa ) {
            bytes *= 0x100;
            bytes += read<unsigned char>();
            --header;
        }
        return bytes;
    } else
        throw fostlib::exceptions::not_implemented(
            "size_control recived invalid size byte",
            fostlib::coerce<fostlib::string>(int(header)));
}


std::vector<unsigned char> rask::connection::in::read(std::size_t b) {
    check(b);
    std::vector<unsigned char> data(b, 0);
    socket->input_buffer.sgetn(reinterpret_cast<char *>(data.data()), b);
    remaining -= b;
    return data;
}

