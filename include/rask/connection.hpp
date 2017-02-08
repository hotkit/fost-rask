/*
    Copyright 2015-2017, Proteus Tech Co Ltd. http://www.kirit.com/Rask
    Distributed under the Boost Software License, Version 1.0.
    See accompanying file LICENSE_1_0.txt or copy at
        http://www.boost.org/LICENSE_1_0.txt
*/


#pragma once


#include <rask/configuration.hpp>
#include <rask/clock.hpp>

#include <fost/internet>
#include <fost/jsondb>
#include <fost/log>
#include <fost/rask/protocol>

#include <f5/threading/eventfd.hpp>
#include <f5/threading/ring.hpp>

#include <atomic>


namespace rask {


    class const_file_block_hash_iterator;
    class tenant;
    class tick;
    struct workers;


    /// The name hash
    using name_hash_type = fostlib::string;


    /// A connection between two Rask servers
    class connection : public fostlib::rask_tcp, public std::enable_shared_from_this<connection> {
        friend void read_and_process(std::shared_ptr<connection>);
    public:
        /// Construct a connection
        connection(rask::workers&);
        /// Destructor so we can log failed connections
        ~connection();

        /// Worker pool used for this connection
        rask::workers &workers;

        /// The buffer size to be used
        const static std::size_t buffer_size = 64 * 1024;

        /// The socket used for this connection
        boost::asio::ip::tcp::socket cnx;
        /// Strand used for sending
        boost::asio::io_service::strand sending_strand;
        /// The communication channel for sending data
        f5::eventfd::unlimited sender;
        /// Heartbeat timer
        boost::asio::deadline_timer heartbeat;

        /// The version that these two peers can support for sending
        /// of data
        fostlib::accessors<uint8_t> peer_version;

        /// Structure used to manage
        class reconnect {
        public:
            reconnect(rask::workers &, const fostlib::json &);
            /// The network configuration to be used to connect
            fostlib::json configuration;
            /// The watchdog timer that will be responsible for reconnecting
            boost::asio::deadline_timer watchdog;
            /// Allow the watchdog to cancel the current connection if it can
            std::weak_ptr<connection> socket;
        };

        /// Store the reconnect so the watchdog can be reset
        std::shared_ptr<reconnect> restart;


        /// The identity of the server we're connected with
        std::atomic<uint32_t> identity;


        /// Build an outbound packet
        using out = out_packet;

        /// Queue a packet for outbound sending. Can be called from
        /// multiple threads. Returns true if the packet was queued and
        // false if it was spilled.
        bool queue(std::function<out(void)>);

        /// Allows a network connection to be read from
        class in {
            friend void read_and_process(std::shared_ptr<connection>);
            /// Construct
            in(std::shared_ptr<connection> socket, std::size_t s);
            /// Throw an EOF exception if there isn't enough data
            void check(std::size_t) const;
        public:
            /// The connection we're reading from
            std::shared_ptr<connection> socket;

            /// Destructor clears unprocessed input
            ~in();

            /// Return true if the packet is empty
            bool empty() const {
                return remaining == 0u;
            }
            /// Return the connection ID
            int64_t socket_id() const {
                return socket->id;
            }
            /// Return the number of remaining bytes
            std::size_t remaining_bytes() const {
                return remaining;
            }

            /// Read a size  control sequence
            std::size_t size_control();

            /// Read an integer
            template<typename I,
                typename std::enable_if<std::is_integral<I>::value>::type* = nullptr>
            I read() {
                I i;
                check(sizeof(i));
                if ( sizeof(i) > 1 ) {
                    socket->input_buffer.sgetn(reinterpret_cast<char *>(&i), sizeof(i));
                    boost::endian::big_to_native_inplace(i);
                } else {
                    i = socket->input_buffer.sbumpc();
                }
                remaining -= sizeof(i);
                return i;
            }
            /// Read a clock tick
            template<typename T,
                typename std::enable_if<std::is_same<tick, T>::value>::type* = nullptr>
            T read() {
                auto time(read<int64_t>());
                auto server(read<uint32_t>());
                return tick::overheard(time,server);
            }
            /// Read a string
            template<typename T,
                typename std::enable_if<std::is_same<fostlib::string, T>::value>::type* = nullptr>
            T read() {
                auto data = read(size_control());
                return fostlib::coerce<fostlib::string>(
                    fostlib::coerce<fostlib::utf8_string>(data));
            }
            /// Read a number of bytes
            std::vector<unsigned char> read(std::size_t b);

        private:
            /// The number of bytes remaining to be read
            std::size_t remaining;
        };

    private:
        /// Buffer of outbound packets
        f5::tsring<std::function<rask::connection::out(void)>> packets;
        /// Start the sender side
        void start_sending();
        /// Reset the heartbeat that will send a version packet. If we've
        /// just received a version block we don't want the reset to push
        /// out our own version packet, so receiving side can pass in
        /// `false` here to reset only the watchdog.
        void reset_heartbeat(bool);
    };


    /// Broadcast a packet to all connections -- return how many were queued
    std::size_t broadcast(std::function<rask::connection::out(void)>);

    /// Monitor the connection
    void monitor_connection(std::shared_ptr<connection>);
    /// Read and process a packet
    void read_and_process(std::shared_ptr<connection>);

    /// Send a version packet with heartbeat
    connection::out send_version();
    /// Process a version packet body
    void receive_version(connection::in &);

    /// Build a tenant instruction
    connection::out tenant_packet(const fostlib::string &name,
            const fostlib::json &meta);
    /// Build a packet of a set of hashes in the tenant hash tree
    connection::out tenant_packet(tenant &,
            std::size_t layer, const name_hash_type &, const fostlib::json &);
    /// React to a tenant that has come in
    void tenant_packet(connection::in &packet);
    /// React to a tenant hash that has come in
    void tenant_hash_packet(connection::in &packet);

    /// Send a create directory instruction
    connection::out create_directory_out(
        tenant &, const rask::tick &, const fostlib::string &, const fostlib::json &);
    /// React to a directory create request
    void create_directory(connection::in &);

    /// Send a file exists instruction
    connection::out file_exists_out(
        tenant &, const rask::tick &, const fostlib::string &, const fostlib::json &);
    /// React to a file exists instruction
    void file_exists(connection::in &);

    /// An empty file hash packet without priority
    connection::out send_empty_file_hash(tenant &, const fostlib::json &);
    /// Recieve a file hash packet
    void file_hash_without_priority(connection::in &);
    /// Process a data block
    void file_data_block(connection::in &);

    /// Create a move an inode out packet
    connection::out move_out_packet(
        tenant &, const rask::tick &, const fostlib::string &, const fostlib::json &);
    /// React to a move inode out packet
    void move_out(connection::in &);


    /// The rask protocol definition
    extern const protocol<std::function<void(connection::in&)>> g_proto;


}


/// Insert a clock tick on the buffer
rask::connection::out &operator << (rask::connection::out &, const rask::tick &);
/// Insert a string on the buffer, with its size header
rask::connection::out &operator << (rask::connection::out &, const fostlib::string &);
/// Insert a fixed size memory block. If the size is not fixed then it
/// needs to be prefixed with a size_sequence so the remote end
/// knows how much data has been sent
rask::connection::out &operator << (rask::connection::out &, const fostlib::const_memory_block);
/// Insert a vector of bytes onto the stream. No size prefix is written.
rask::connection::out &operator << (rask::connection::out &, const std::vector<unsigned char> &);

