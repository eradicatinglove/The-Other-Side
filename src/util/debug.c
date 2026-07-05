/*
Copyright (c) 2017-2018 Adubbz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "util/debug.h"

#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#ifdef APP_DEBUG_LOG
static const char* kDebugLogPath = "sdmc:/switch/CyberFoil/tinfoil_debug.log";
#endif

void debugLogReset(void)
{
#ifdef APP_DEBUG_LOG
    FILE* f = fopen(kDebugLogPath, "w");
    if (f != NULL) {
        fclose(f);
    }
#endif
}

void debugLogWrite(const char* func, unsigned int line, const char* format, ...)
{
#ifdef APP_DEBUG_LOG
    FILE* f = fopen(kDebugLogPath, "a");
    if (f == NULL)
        return;

    if (func != NULL)
        fprintf(f, "%s:%u: ", func, line);

    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fclose(f);
#else
    (void)func;
    (void)line;
    (void)format;
#endif
}

void printBytes(u8 *bytes, size_t size, bool includeHeader)
{
#ifdef APP_DEBUG_LOG
    int count = 0;
    FILE* f = fopen(kDebugLogPath, "a");
    if (f == NULL)
        return;

    if (includeHeader)
    {
        fprintf(f, "\n\n00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n");
        fprintf(f, "-----------------------------------------------\n");
    }

    for (int i = 0; i < size; i++)
    {
        fprintf(f, "%02x ", bytes[i]);
        count++;
        if ((count % 16) == 0)
            fprintf(f, "\n");
    }

    fprintf(f, "\n");
    fclose(f);
#endif
}
