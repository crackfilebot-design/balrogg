/*  Copyright (C) 2026 Kamila Szewczyk

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#ifndef BLR_OPUSMODE_H
#define BLR_OPUSMODE_H

#include "common.h"

/*  `lev` is the stored PVQ split depth, 0 through PVQ_LEVMAX.  */
int opus_pack(const char * in, const char * out, int lev);
int opus_unpack(const char * in, const char * out);

/*  Return the vendored libopus version for `balrogg -v`.  */
const char * opus_mode_version(void);

#endif
