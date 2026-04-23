/*
Plugin Name
Copyright (C) <Year> <Developer> <Email Address>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;

/* Forwards to blogva() with a [PLUGIN_NAME]-prefixed format.
 * blogva itself is declared by <obs-module.h>/<util/base.h> with platform-
 * correct dllimport/visibility linkage — don't redeclare it here, because
 * sources that include both this header and <obs-module.h> would otherwise
 * see two declarations with conflicting linkage (MSVC C2375). */
void obs_log(int log_level, const char *format, ...);

#ifdef __cplusplus
}
#endif
