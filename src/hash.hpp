/*
    Copyright 2015, Proteus Tech Co Ltd. http://www.kirit.com/Rask
    Distributed under the Boost Software License, Version 1.0.
    See accompanying file LICENSE_1_0.txt or copy at
        http://www.boost.org/LICENSE_1_0.txt
*/


#pragma once


#include <fost/jsondb>


namespace rask {


    class tenant;


    /// The name hash
    using name_hash_type = fostlib::string;


    /// Return the hash for a name
    name_hash_type name_hash(const fostlib::string &);

    /// Re-hash starting at the inode list level
    void rehash_inodes(const fostlib::jsondb::local &);
    /// Re-hash starting at specified database
    void rehash_inodes(const fostlib::json &dbconfig);

    /// Re-hash starting at the tenants level
    void rehash_tenants(const fostlib::jsondb::local &);


}

