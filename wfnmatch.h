/* Yash: yet another shell */
/* wfnmatch.h: fnmatch for wide-character strings */
/* (C) 2007-2009 magicant */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#ifndef YASH_WFNMATCH_H
#define YASH_WFNMATCH_H

#include <wchar.h>


/* values for the `flags' argument of `wfnmatch' */
enum wfnmflags {
    WFNM_NOESCAPE = 1 << 0,
    WFNM_PATHNAME = 1 << 1,
    WFNM_PERIOD   = 1 << 2,
    WFNM_CASEFOLD = 1 << 3,
};

/* values for the `type' argument of `wfnmatch' */
enum wfnmtype { WFNM_WHOLE, WFNM_LONGEST, WFNM_SHORTEST, };

/* the return values that are returned on match failure and on error
 * respectively. */
#define WFNM_NOMATCH ((size_t) -1)
#define WFNM_ERROR   ((size_t) -2)

extern size_t wfnmatch(const wchar_t *pat, const wchar_t *s,
	enum wfnmflags flags, enum wfnmtype type)
    __attribute__((nonnull));
extern size_t wfnmatchl(const wchar_t *pat, const wchar_t *s,
	enum wfnmflags flags, enum wfnmtype type, size_t shortest_match_length)
    __attribute__((nonnull));
extern size_t shortest_match_length(const wchar_t *pat, enum wfnmflags flags)
    __attribute__((nonnull));
extern _Bool pattern_has_special_char(const wchar_t *pat, _Bool pathname)
    __attribute__((pure,nonnull));


#endif /* YASH_WFNMATCH_H */


/* vim: set ts=8 sts=4 sw=4 noet: */
