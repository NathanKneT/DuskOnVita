/* C library functions VitaSDK's newlib build leaves out but its headers declare.
 * Each is newlib's own definition (newlib/libc/stdio/flockfile.c, funlockfile.c),
 * built from the lock macros the same libc uses internally (sys/stdio.h).
 * fmt's print(FILE*) path takes the stream lock through them. */
#include <stdio.h>
#include <sys/stdio.h>

void flockfile(FILE *fp) { _flockfile(fp); }
void funlockfile(FILE *fp) { _funlockfile(fp); }
