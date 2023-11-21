/* Yash: yet another shell */
/* fuzz_parser.cpp: parser fuzzing entrypoint */
/* (C) 2023 l2dy */
/* (C) 2007-2022 magicant */

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


extern "C"
{
void exec_mbs(const char *mbcode, const char *name);
}

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <vector>


extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
    std::string d(reinterpret_cast<const char*>(Data), Size);
    exec_mbs(d.c_str(), "nn");
    return 0;
}
