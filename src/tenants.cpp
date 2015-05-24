/*
    Copyright 2015, Proteus Tech Co Ltd. http://www.kirit.com/Rask
    Distributed under the Boost Software License, Version 1.0.
    See accompanying file LICENSE_1_0.txt or copy at
        http://www.boost.org/LICENSE_1_0.txt
*/


#include <beanbag/beanbag>
#include <rask/tenants.hpp>


void rask::tenants(const fostlib::json &dbconfig) {
    beanbag::jsondb_ptr dbp(beanbag::database(dbconfig));
}

