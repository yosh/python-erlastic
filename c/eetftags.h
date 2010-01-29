/* From http://www.erlang.org/doc/apps/erts/erl_ext_dist.html */

#define FORMAT_VERSION 131

#define COMPRESSED              0x50    /* 'P' */
#define DIST_HEADER             0x44    /* 'D' */
#define ATOM_CACHE_REF          0x52    /* 'R' */
#define SMALL_INTEGER_EXT       0x61    /* 'a' */
#define INTEGER_EXT             0x62    /* 'b' */
#define FLOAT_EXT               0x63    /* 'c' */
#define ATOM_EXT                0x64    /* 'd' */
#define REFERENCE_EXT           0x65    /* 'e' */
#define PORT_EXT                0x66    /* 'f' */
#define PID_EXT                 0x67    /* 'g' */
#define SMALL_TUPLE_EXT         0x68    /* 'h' */
#define LARGE_TUPLE_EXT         0x69    /* 'i' */
#define NIL_EXT                 0x6a    /* 'j' */
#define STRING_EXT              0x6b    /* 'k' */
#define LIST_EXT                0x6c    /* 'l' */
#define BINARY_EXT              0x6d    /* 'm' */
#define SMALL_BIG_EXT           0x6e    /* 'n' */
#define LARGE_BIG_EXT           0x6f    /* 'o' */
#define NEW_REFERENCE_EXT       0x72    /* 'r' */
#define SMALL_ATOM_EXT          0x73    /* 's' */
#define FUN_EXT                 0x75    /* 'u' */
#define NEW_FUN_EXT             0x70    /* 'p' */
#define EXPORT_EXT              0x71    /* 'q' */
#define BIT_BINARY_EXT          0x4d    /* 'M' */
#define NEW_FLOAT_EXT           0x46    /* 'F' */
