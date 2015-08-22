//  (C) Copyright Howard Hinnant
//  (C) Copyright 2010-2011 Vicente J. Botet Escriba
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).

//===-------------------------- locale ------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// This code was adapted by Vicente from Howard Hinnant's experimental work
// on chrono i/o to Boost and some functions from libc++/locale to emulate the missing time_get::get()

#ifndef BOOST_CHRONO_IO_TIME_POINT_IO_H
#define BOOST_CHRONO_IO_TIME_POINT_IO_H

#include <time.h>
 
extern time_t internal_timegm(tm const *t);

#endif
