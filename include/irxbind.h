/* irxbind.h - minimal local view of the rexx370 IRX control blocks and codes
 * that HTTPREXX touches at runtime.
 *
 * HTTPREXX drives the installed IRX load modules (IRXINIT/IRXEXEC/IRXTERM) via
 * the MVS LINK service and only needs the struct layouts and function codes to
 * build the parameter lists and to read the SAY data inside its I/O routine.
 * Rather than vendoring rexx370's full irx.h (which risks header-chain clashes
 * with the libc370/httpd typedefs), this header reproduces only the fields
 * HTTPREXX reads or writes, faithful to:
 *
 *     ../rexx370/include/irx.h        (envblock, irxexte, instblk, argtable)
 *     ../rexx370/include/irxwkblk.h   (RXF* function codes)
 *     ../rexx370/include/irxio.h      (I/O routine signature)
 *     ../lstring370/include/lstring.h (Lstr - the SAY data)
 *
 * Field offsets are noted; re-sync if those projects change a layout. The
 * compile-time assertion at the end guards the one size that must be exact.
 */
#ifndef IRXBIND_H
#define IRXBIND_H

#include <stddef.h>

/* --- I/O replaceable-routine function codes (irxwkblk.h) ----------- */
#define RXFWRITE    0   /* SAY                       */
#define RXFREAD     1   /* PULL (from terminal)      */
#define RXFREADP    2   /* PULL (from stack)         */
#define RXFTWRITE   3   /* trace output              */
#define RXFWRITERR  4   /* error message             */

/* --- IRXEXEC call-type flags (irx.h) ------------------------------- */
#define IRXEXEC_COMMAND    0x00000000
#define IRXEXEC_SUBROUTINE 0x40000000

/* --- INSTBLK in-storage source (irx.h) ----------------------------- */
#define IRX_INSTBLK_ID     "IRXINSTB"
#define IRX_INSTBLK_HDRLEN 128

/* lstring370 Lstr: the data the I/O routine receives for SAY. pstr is NOT
 * NUL-terminated -- copy exactly len bytes. */
typedef struct {
    unsigned char *pstr;    /* +0x00 data pointer                */
    size_t         len;     /* +0x04 live length in bytes        */
    size_t         maxlen;  /* +0x08 allocated capacity          */
    short          type;    /* +0x0C                             */
} IRX_LSTR;

/* ENVBLOCK, head through envblock_irxexte (the rest is not needed here). */
typedef struct {
    unsigned char  id[8];        /* +0x00 'ENVBLOCK'                 */
    unsigned char  version[4];   /* +0x08                            */
    int            length;       /* +0x0C                            */
    void          *parmblock;    /* +0x10                            */
    void          *userfield;    /* +0x14 HTTPREXX request context   */
    void          *workblok_ext; /* +0x18                            */
    void          *irxexte;      /* +0x1C -> IRX_IRXEXTE             */
} IRX_ENVBLOCK;

/* IRXEXTE entry-point vector, head through irxinout. The active I/O routine
 * lives at io_routine (+0x18); HTTPREXX overwrites it post-init. */
typedef struct {
    int    entry_count;   /* +0x00                                  */
    void  *irxinit;       /* +0x04                                  */
    void  *load_routine;  /* +0x08                                  */
    void  *irxload;       /* +0x0C                                  */
    void  *irxexcom;      /* +0x10                                  */
    void  *irxexec;       /* +0x14                                  */
    void  *io_routine;    /* +0x18 active I/O routine (we patch)     */
    void  *irxinout;      /* +0x1C default I/O routine               */
} IRX_IRXEXTE;

/* One INSTBLK entry: a (statement text, length) pair. */
typedef struct {
    void *stmt;   /* -> REXX statement text (newline not included) */
    int   len;    /* statement length                              */
} IRX_INSTBLK_ENTRY;

/* INSTBLK 128-byte header; the entry array follows via instblk_address. */
typedef struct {
    unsigned char  acronym[8];   /* 'IRXINSTB'                       */
    int            hdrlen;       /* 128                              */
    int            _filler1;
    void          *address;      /* -> first IRX_INSTBLK_ENTRY        */
    int            usedlen;      /* total length of the entry array   */
    unsigned char  member[8];    /* PARSE SOURCE member name          */
    unsigned char  ddname[8];    /* PARSE SOURCE DD name              */
    unsigned char  subcom[8];    /* initial subcommand environment    */
    int            _filler2;
    int            dsnlen;
    unsigned char  dsname[54];
    short          _filler3;
    void          *extname_ptr;
    int            extname_len;
    int            _filler4[2];
} IRX_INSTBLK;

/* One ARGTABLE entry: a (string, length) pair. Terminate the array with an
 * entry of 8 bytes of 0xFF. */
typedef struct {
    void *str;
    int   len;
} IRX_ARGTABLE_ENTRY;

/* The I/O replaceable routine (irxio.h: int irxinout(int, PLstr, envblock*)). */
typedef int (*IRX_IO_ROUTINE)(int function, IRX_LSTR *data, IRX_ENVBLOCK *env);

/* The INSTBLK header must be exactly 128 bytes or rexx370 rejects it. This
 * fails the build (negative array size) if the layout ever drifts. The check
 * applies only with cc370's 4-byte pointers (the build target); a 64-bit host
 * indexer sees a larger struct and is skipped via the pointer-size guard. */
typedef char irxbind_instblk_size_check
    [(sizeof(void *) != 4 || sizeof(IRX_INSTBLK) == 128) ? 1 : -1];

#endif /* IRXBIND_H */
