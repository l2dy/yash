/* fuzz_parser.c: parser fuzzing entrypoint */
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

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "option.h"

void exec_mbs(const char *mbcode, const char *name);
void fuzz_init_all();

static int SetPosixlyCorrectFlag(const uint8_t *Data, size_t Size) {
    if (Size < 2) {
        return -1;
    }
    posixly_correct = Data[0] & 0x1;
    return 0;
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    fuzz_init_all();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
    char *code;

    if (SetPosixlyCorrectFlag(Data, Size) == -1) {
        return 0;
    }
    Data++;
    Size--;

    code = malloc(Size + 1);
    memcpy(code, Data, Size);
    memset(code + Size, 0, 1);

    exec_mbs(code, "nn");

    free(code);
    return 0;
}
