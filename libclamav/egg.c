/*
 *  Copyright (C) 2019 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 *
 *  EGG is an archive format created by ESTsoft used by their ALZip
 *  archiving software.
 *
 *  This software is written from scratch based solely from ESTsoft's
 *  file format documentation and from testing with EGG format archives.
 *  ESTsoft's "unEGG" module was not used in the creation of this capability
 *  in order to avoid to licensing restrictions on the ESTsoft "unEGG" module.
 *
 *  EGG structure:
 *
 *     |-----------------------------------------------------|------|
 *     | EGG Header                                          |  1   |
 *     |-----------------------------------------------------|------|
 *     | Extra Field 1:                                      |      |
 *     |   Split Compression                                 |      |
 *     |   Solid Compression                                 | 0~N  |
 *     |   Global Encryption Header                          |      |
 *     |---------------------------------------|------|------|------|
 *     | File Header                           |  1   |      |      |
 *     |---------------------------------------|------|      |      |
 *     | Extra Field 2:                        |      |      |      |
 *     |   Filename Header                     |      | 1~N  |      |
 *     |   Comment Header                      | 0~N  |      |      |
 *     |   Windows File Information            |      |      |      |
 *     |   Posix File Information              |      |      | 0~N  |
 *     |   Encrypt Header                      |      |      |      |
 *     |---------------------------------------|------|------|      |
 *     | Block Header                          |  1   |      |      |
 *     |---------------------------------------|------|      |      |
 *     | Extra Field 3:                        | 0~N  | 0~N  |      |
 *     |---------------------------------------|------|      |      |
 *     | Compressed Data                       |  1   |      |      |
 *     |---------------------------------------|------|------|------|
 *     | Extra Field 4:                                      |      |
 *     |   Archive Comment Header                            | 0~N  |
 *     |-----------------------------------------------------|------|
 *
 *  Authors: Micah Snyder
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#include <stdint.h>
#include <inttypes.h>
#include <wchar.h>
#include <locale.h>
#include <zlib.h>

#if HAVE_BZLIB_H
#include <bzlib.h>
#endif

#ifdef HAVE_ICONV
#include <iconv.h>
#endif

#include "lzma_iface.h"

#include "egg.h"
#include "msdoc.h"

#ifndef WCHAR
typedef uint16_t WCHAR;
#endif

/*
 * All EGG struct variables are little-endian.
 */

#ifndef HAVE_ATTRIB_PACKED
#define __attribute__(x)
#endif

#ifdef HAVE_PRAGMA_PACK
#pragma pack(1)
#endif

#ifdef HAVE_PRAGMA_PACK_HPPA
#pragma pack 1
#endif

/*
 * general defines
 */
#define EOFARC 0x08E28222 /* Signals end of each header, or end of archive. */
//#define EOFAR_ 0x2282E208
/*
 * egg_header
 */
#define EGG_HEADER_MAGIC 0x41474745
#define EGG_HEADER_VERSION 0x0100

typedef uint32_t magic32_t;

typedef struct __attribute__((packed)) {
    magic32_t magic;    /* 0x41474745 */
    uint16_t version;   /* 0x0100 */
    uint32_t header_id; /* Random number of the program (Cannot be 0) */
    uint32_t reserved;  /* 0x00000000 */
} egg_header;

/*
 * file_header
 */
#define FILE_HEADER_MAGIC 0x0A8590E3

typedef struct __attribute__((packed)) {
    magic32_t magic;      /* 0x0A8590E3 */
    uint32_t file_id;     /* Unique value for each header (Includes 0) */
    uint64_t file_length; /* Total size of the file */
} file_header;

/*
 * block_header
 * Note: split block of files exceeding 4G
 */
#define BLOCK_HEADER_MAGIC 0x02B50C13
#define BLOCK_HEADER_COMPRESS_ALGORITHM_STORE 0
#define BLOCK_HEADER_COMPRESS_ALGORITHM_DEFLATE 1
#define BLOCK_HEADER_COMPRESS_ALGORITHM_BZIP2 2
#define BLOCK_HEADER_COMPRESS_ALGORITHM_AZO 3
#define BLOCK_HEADER_COMPRESS_ALGORITHM_LZMA 4

typedef struct __attribute__((packed)) {
    magic32_t magic;            /* 0x02B50C13 */
    uint8_t compress_algorithm; /* compress method algorithm number */
    uint8_t compress_hint;      /* compress method hint */
    uint32_t uncompress_size;   /* size of the block before compressed */
    uint32_t compress_size;     /* size of the block after compressed */
    uint32_t crc32;             /* CRC value of the block */
} block_header;

/*
 * extra_field
 *
 * The extra_field is followed by a uint16_t or uint32_t depending on the bit_flag.
 * This describes the size of the following data.
 * In this way, an unexpected header can still be parsed.
 * Headers that make use of the extra_field:
 *  - windows_file_information header
 *  - posix_file_information header
 *  - encrypt header
 *  - filename header
 *  - comment header
 *  - split_compression header
 *  - solid_compression header
 */
#define EXTRA_FIELD_FLAGS_SIZE_IS_2BYTES 0x00
#define EXTRA_FIELD_FLAGS_SIZE_IS_4BYTES 0x01

typedef struct __attribute__((packed)) {
    magic32_t magic;
    uint8_t bit_flag; /* the size field following bit_flag depends if bit_flag bit 1: */
} extra_field;        /*    0 (uint16_t) */
                      /*    1 (uint32_t) */

/*
 * Extra field: encrypt
 *
 * The encrypt_header is followed by:
 *  1) dummy data (size bytes)
 *
 * Note: Inserted in Extra Field 2 (optional, depending on KeyBase, AES, or LEA)
 */
#define ENCRYPT_HEADER_MAGIC 0x08D1470F
#define ENCRYPT_HEADER_ENCRYPT_METHOD_XOR 0x00
#define ENCRYPT_HEADER_ENCRYPT_METHOD_AES128 0x01
#define ENCRYPT_HEADER_ENCRYPT_METHOD_AES256 0x02
#define ENCRYPT_HEADER_ENCRYPT_METHOD_LEA128 0x10
#define ENCRYPT_HEADER_ENCRYPT_METHOD_LEA256 0x20

typedef struct __attribute__((packed)) {
    uint8_t aes_header[10]; /* AES/LEA Header */
    uint8_t aes_footer[10]; /* AES/LEA Footer */
} aes_lea_128;

typedef struct __attribute__((packed)) {
    uint8_t aes_header[18]; /* AES/LEA header */
    uint8_t aes_footer[10]; /* AES/LEA footer */
} aes_lea_256;

typedef struct __attribute__((packed)) {
    uint8_t verify_data[12]; /* KeyBase encryption verification data */
    uint32_t crc32;          /* KeyBase partial block CRC */
} zip2_xor_keybase;

typedef struct __attribute__((packed)) {
    uint8_t encrypt_method; /* See above encrypt method #defines */
} encrypt_header;

/*
 * Extra field: windows_file_information
 */
#define WINDOWS_INFO_MAGIC 0x2C86950B
#define WINDOWS_INFO_ATTRIBUTE_READONLY 0x01
#define WINDOWS_INFO_ATTRIBUTE_HIDDEN 0x02
#define WINDOWS_INFO_ATTRIBUTE_SYSTEM_FILE 0x04
#define WINDOWS_INFO_ATTRIBUTE_LINK_FILE 0x10 /* junction file */
#define WINDOWS_INFO_ATTRIBUTE_DIRECTORY 0x40

typedef struct __attribute__((packed)) {
    uint64_t last_modified_time; /* "100-Nanosecond Time" since the Windows Epoch (00:00:00 UTC, January 1, 1601) */
    uint8_t attribute;           /* See above attribute #defines */
} windows_file_information;

/*
 * Extra field: posix_file_information
 */
#define POSIX_INFO_MAGIC 0x1EE922E5
#define POSIX_INFO_MODE_FILETYPE_BITMASK 0x0170000  /* bitmask for the file type bitfields */
#define POSIX_INFO_MODE_SOCKET 0x0140000            /* socket */
#define POSIX_INFO_MODE_SYM_LINK 0x0120000          /* symbolic link */
#define POSIX_INFO_MODE_REG_FILE 0x0100000          /* regular file */
#define POSIX_INFO_MODE_BLOCK_DEVICE 0x0060000      /* block device */
#define POSIX_INFO_MODE_DIRECTORY 0x0040000         /* directory */
#define POSIX_INFO_MODE_CHAR_DEVICE 0x0020000       /* character device */
#define POSIX_INFO_MODE_FIFO 0x0010000              /* FIFO */
#define POSIX_INFO_MODE_SET_UID_BIT 0x0004000       /* set UID bit */
#define POSIX_INFO_MODE_SET_GROUPID_BIT 0x0002000   /* set-group-ID bit (see below) */
#define POSIX_INFO_MODE_STICKY_BIT 0x0001000        /* sticky bit (see below) */
#define POSIX_INFO_MODE_PERM_OWNER_MASK 0x00700     /* mask for file owner permissions */
#define POSIX_INFO_MODE_PERM_OWNER_READ 0x00400     /* owner has read permission */
#define POSIX_INFO_MODE_PERM_OWNER_WRITE 0x00200    /* owner has write permission */
#define POSIX_INFO_MODE_PERM_OWNER_EXECUTE 0x00100  /* owner has execute permission */
#define POSIX_INFO_MODE_PERM_GROUP_MASK 0x00070     /* mask for group permissions */
#define POSIX_INFO_MODE_PERM_GROUP_READ 0x00040     /* group has read permission */
#define POSIX_INFO_MODE_PERM_GROUP_WRITE 0x00020    /* group has write permission */
#define POSIX_INFO_MODE_PERM_GROUP_EXECUTE 0x00010  /* group has execute permission */
#define POSIX_INFO_MODE_PERM_OTHERS_MASK 0x00007    /* mask for permissions for others (not in group) */
#define POSIX_INFO_MODE_PERM_OTHERS_READ 0x00004    /* others have read permission */
#define POSIX_INFO_MODE_PERM_OTHERS_WRITE 0x00002   /* others have write permission */
#define POSIX_INFO_MODE_PERM_OTHERS_EXECUTE 0x00001 /* others have execute permission*/

typedef struct __attribute__((packed)) {
    uint32_t mode;               /* see above mode #defines */
    uint32_t uid;                /*  */
    uint32_t gid;                /*  */
    uint64_t last_modified_time; /* "Second Time" since the Unix Epoch (00:00:00 UTC, January 1, 1970) */
} posix_file_information;

/*
 * Extra field: dummy_header
 *
 * The dummy header extra_info is followed by:
 *  1) dummy data (size bytes)
 *
 * Note: No need to consider if the size is too small to fit the dummy header because it can be distinguished by size calculation.
 */
#define DUMMY_HEADER_MAGIC 0x07463307

/*
 * Extra field: filename
 *
 * The filename extra_field is followed by:
 *  1) uint16_t locale IFF bit_flag is NOT unicode (UCS-2 LE)
 *  1) uint32_t parent_path_id IFF bit_flag is relative.
 *     parent_path_id will be the ID of a file possessing the parent path.
 *  2) name buffer (size bytes minus above optional fields)
 */
#define FILENAME_HEADER_MAGIC 0x0A8591AC
#define FILENAME_HEADER_FLAGS_ENCRYPT 0x04
#define FILENAME_HEADER_FLAGS_MULTIBYTE_CODEPAGE_INSTEAD_OF_UTF8 0x08
#define FILENAME_HEADER_FLAGS_RELATIVE_PATH_INSTEAD_OF_ABSOLUTE 0x10
#define FILENAME_HEADER_LOCALE_USE_SYSTEM 0
#define FILENAME_HEADER_LOCALE_JAPANESE 932 /* Shift-JIS */
#define FILENAME_HEADER_LOCALE_KOREAN 949

// typedef struct __attribute__((packed)) {
//     (optional) uint16_t locale
//     (optional) uint32_t parent_path_id
//     uint8_t name_data [extra_field->size - sizeof(locale) - sizeof(parent_path_id)]
// } filename_header;

/*
 * Extra field: comment
 *
 * The comment extra_field is followed by:
 *  1) comment of size "N", exclude NULL character.
 */
#define COMMENT_HEADER_MAGIC 0x04C63672
#define COMMENT_HEADER_FLAGS_ENCRYPT 0x04
#define COMMENT_HEADER_FLAGS_MULTIBYTE_CODEPAGE_INSTEAD_OF_UTF8 0x08

/*
 * Extra field: split compression
 */
#define SPLIT_COMPRESSION_MAGIC 0x24F5A262

typedef struct __attribute__((packed)) {
    uint32_t prev_file_id; /* ID of previous file, 0 if first */
    uint32_t next_file_id; /* ID of next file, 0 if last */
} split_compression;

/*
 * Extra field: solid compression
 */
#define SOLID_COMPRESSION_MAGIC 0x24E5A060

#ifdef HAVE_PRAGMA_PACK
#pragma pack()
#endif

#ifdef HAVE_PRAGMA_PACK_HPPA
#pragma pack
#endif

typedef struct {
    char* name_utf8;
    uint32_t parent_path_id;
} egg_filename;

typedef struct {
    encrypt_header* header; /* Global Encryption Header */
    union {
        aes_lea_128* al128;
        aes_lea_256* al256;
        zip2_xor_keybase* xor ;
    } encrypt_al;
} egg_encrypt;

typedef struct {
    block_header* blockHeader;
    char* compressedData;
} egg_block;

typedef struct {
    file_header* file;
    egg_filename filename;
    windows_file_information* windowsFileInformation;
    posix_file_information* posixFileInformation;
    egg_encrypt* encrypt;
    uint64_t nBlocks;
    egg_block** blocks;
    uint64_t nComments;
    char** comments;
} egg_file;

typedef struct {
    fmap_t* map;
    size_t offset;
    uint64_t fileExtractionIndex;
    int bSolid; /* Solid == all files compressed together. */
    int bSplit; /* Split == multiple files make up single archive. */
    split_compression* splitInfo;
    egg_encrypt* encrypt;
    uint64_t nFiles;
    egg_file** files;
    uint64_t nBlocks;
    egg_block** blocks;
    uint64_t nComments;
    char** comments;
} egg_handle;

#define EGG_VALIDATE_HANDLE(h) \
    ((!handle || !handle->map || (handle->offset > handle->map->len)) ? EGG_ERR : EGG_OK)

const char* getEncryptName(uint8_t method)
{
    const char* encryptName = NULL;

    switch (method) {
        case ENCRYPT_HEADER_ENCRYPT_METHOD_XOR:
            encryptName = "XOR";
            break;
        case ENCRYPT_HEADER_ENCRYPT_METHOD_AES128:
            encryptName = "AES 128";
            break;
        case ENCRYPT_HEADER_ENCRYPT_METHOD_LEA128:
            encryptName = "LEA 128";
            break;
        case ENCRYPT_HEADER_ENCRYPT_METHOD_AES256:
            encryptName = "AES 256";
            break;
        case ENCRYPT_HEADER_ENCRYPT_METHOD_LEA256:
            encryptName = "LEA 256";
            break;
        default:
            encryptName = "<unknown method>";
    }

    return encryptName;
}

const char* getMagicHeaderName(uint32_t magic)
{
    const char* magicName = NULL;

    switch (magic) {
        case EGG_HEADER_MAGIC:
            magicName = "EGG_HEADER_MAGIC";
            break;
        case FILE_HEADER_MAGIC:
            magicName = "FILE_HEADER_MAGIC";
            break;
        case BLOCK_HEADER_MAGIC:
            magicName = "BLOCK_HEADER_MAGIC";
            break;
        case ENCRYPT_HEADER_MAGIC:
            magicName = "ENCRYPT_HEADER_MAGIC";
            break;
        case WINDOWS_INFO_MAGIC:
            magicName = "WINDOWS_INFO_MAGIC";
            break;
        case POSIX_INFO_MAGIC:
            magicName = "POSIX_INFO_MAGIC";
            break;
        case DUMMY_HEADER_MAGIC:
            magicName = "DUMMY_HEADER_MAGIC";
            break;
        case FILENAME_HEADER_MAGIC:
            magicName = "FILENAME_HEADER_MAGIC";
            break;
        case COMMENT_HEADER_MAGIC:
            magicName = "COMMENT_HEADER_MAGIC";
            break;
        case SPLIT_COMPRESSION_MAGIC:
            magicName = "SPLIT_COMPRESSION_MAGIC";
            break;
        case SOLID_COMPRESSION_MAGIC:
            magicName = "SOLID_COMPRESSION_MAGIC";
            break;
        default:
            magicName = "<unknown header magic>";
    }

    return magicName;
}

/**
 * @brief Convert string to UTF-8, given Windows codepage.
 *
 * @param in                string buffer
 * @param in_size           length of string buffer in bytes
 * @param codepage          Windows code page https://docs.microsoft.com/en-us/windows/desktop/Intl/code-page-identifiers)
 * @param [out] out         pointer to receive malloc'ed utf-8 buffer.
 * @param [out] out_size    pointer to receive size of utf-8 buffer, not including null terminating character.
 * @return cl_egg_error_t   EGG_OK if success. EGG_BREAK if unable to because iconv is unavailable.  Other error code if outright failure.
 */
cl_egg_error_t cli_codepage_to_utf8(char* in, size_t in_size, uint16_t codepage, char** out, size_t* out_size)
{
    cl_egg_error_t status = EGG_BREAK;

    char* out_utf8       = NULL;
    size_t out_utf8_size = 0;

#if defined(HAVE_ICONV)
    iconv_t conv = NULL;
#elif defined(WIN32)
    LPWSTR lpWideCharStr = NULL;
    int cchWideChar      = 0;
#endif

    if (NULL == in || in_size == 0 || NULL == out || NULL == out_size) {
        cli_dbgmsg("egg_filename_to_utf8: Invalid args.\n");
        status = EGG_ERR;
        goto done;
    }

    *out      = NULL;
    *out_size = 0;

    switch (codepage) {
        case 20127:   /* US-ASCII (7-bit) */
        case 65001: { /* Unicode (UTF-8) */
            char* track;
            int byte_count, sigbit_count;

            out_utf8_size = in_size;
            out_utf8      = cli_calloc(1, out_utf8_size + 1);
            if (NULL == out_utf8) {
                cli_errmsg("egg_filename_to_utf8: Failure allocating buffer for utf8 filename.\n");
                status = EGG_EMEM;
                goto done;
            }
            memcpy(out_utf8, in, in_size);

            track = out_utf8 + in_size - 1;
            if ((codepage == 65001) && (*track & 0x80)) {
                /*
                 * UTF-8 with a most significant bit.
                 */

                /* locate the start of the last character */
                for (byte_count = 1; (track != out_utf8); track--, byte_count++) {
                    if (((uint8_t)*track & 0xC0) != 0x80)
                        break;
                }

                /* count number of set (1) significant bits */
                for (sigbit_count = 0; sigbit_count < (int)(sizeof(uint8_t) * 8); sigbit_count++) {
                    if (((uint8_t)*track & (0x80 >> sigbit_count)) == 0)
                        break;
                }

                if (byte_count != sigbit_count) {
                    cli_dbgmsg("egg_filename_to_utf8: cleaning out %d bytes from incomplete "
                               "utf-8 character length %d\n",
                               byte_count, sigbit_count);
                    for (; byte_count > 0; byte_count--, track++) {
                        *track = '\0';
                    }
                }
            }
            break;
        }
        default: {

#if defined(WIN32) && !defined(HAVE_ICONV)

            /*
             * Do conversion using native Win32 APIs.
             */

            if (1200 != codepage) { /* not already UTF16-LE (Windows Unicode) */
                /*
                 * First, Convert from codepage -> UCS-2 LE with MultiByteToWideChar(codepage)
                 */
                cchWideChar = MultiByteToWideChar(
                    codepage,
                    0,
                    in,
                    in_size,
                    NULL,
                    0);
                if (0 == cchWideChar) {
                    cli_dbgmsg("egg_filename_to_utf8: failed to determine string size needed for ansi to widechar conversion.\n");
                    status = EGG_ERR;
                    goto done;
                }

                lpWideCharStr = malloc((cchWideChar + 1) * sizeof(WCHAR));
                if (NULL == lpWideCharStr) {
                    cli_dbgmsg("egg_filename_to_utf8: failed to allocate memory for wide char string.\n");
                    status = EGG_EMEM;
                    goto done;
                }

                cchWideChar = MultiByteToWideChar(
                    codepage,
                    0,
                    in,
                    in_size,
                    lpWideCharStr,
                    cchWideChar + 1);
                if (0 == cchWideChar) {
                    cli_dbgmsg("egg_filename_to_utf8: failed to convert multibyte string to widechars.\n");
                    status = EGG_ERR;
                    goto done;
                }

                in      = (char*)lpWideCharStr;
                in_size = cchWideChar;
            }

            /*
             * Convert from UCS-2 LE -> UTF8 with WideCharToMultiByte(CP_UTF8)
             */
            out_utf8_size = WideCharToMultiByte(
                CP_UTF8,
                0,
                (LPCWCH)in,
                in_size / sizeof(WCHAR),
                NULL,
                0,
                NULL,
                NULL);
            if (0 == out_utf8_size) {
                cli_dbgmsg("egg_filename_to_utf8: failed to determine string size needed for widechar conversion.\n");
                status = EGG_ERR;
                goto done;
            }

            out_utf8 = malloc(out_utf8_size + 1);
            if (NULL == lpWideCharStr) {
                cli_dbgmsg("egg_filename_to_utf8: failed to allocate memory for wide char to utf-8 string.\n");
                status = EGG_EMEM;
                goto done;
            }

            out_utf8_size = WideCharToMultiByte(
                CP_UTF8,
                0,
                (LPCWCH)in,
                in_size / sizeof(WCHAR),
                out_utf8,
                out_utf8_size,
                NULL,
                NULL);
            if (0 == out_utf8_size) {
                cli_dbgmsg("egg_filename_to_utf8: failed to convert widechar string to utf-8.\n");
                status = EGG_ERR;
                goto done;
            }

#elif defined(HAVE_ICONV)

            uint32_t attempt, i;
            size_t inbytesleft, outbytesleft;
            const char* encoding = NULL;

            for (i = 0; i < NUMCODEPAGES; ++i) {
                if (codepage == codepage_entries[i].codepage) {
                    encoding = codepage_entries[i].encoding;
                } else if (codepage < codepage_entries[i].codepage) {
                    break; /* fail-out early, requires sorted array */
                }
            }

            for (attempt = 1; attempt <= 3; attempt++) {
                /* Charset to UTF-8 should never exceed in_size * 6;
                 * We can shrink final buffer after the conversion, if needed. */
                out_utf8_size = (in_size * 2) * attempt;

                inbytesleft  = in_size;
                outbytesleft = out_utf8_size;

                out_utf8 = cli_calloc(1, out_utf8_size + 1);
                if (NULL == out_utf8) {
                    cli_errmsg("egg_filename_to_utf8: Failure allocating buffer for utf8 data.\n");
                    status = EGG_EMEM;
                }

                conv = iconv_open("UTF-8//TRANSLIT", encoding);
                if (conv == (iconv_t)-1) {
                    cli_warnmsg("egg_filename_to_utf8: Failed to open iconv.\n");
                    goto done;
                }

                if ((size_t)-1 == iconv(conv, &in, &inbytesleft, &out_utf8, &outbytesleft)) {
                    switch (errno) {
                        case E2BIG:
                            cli_warnmsg("egg_filename_to_utf8: iconv error: There is not sufficient room at *outbuf.\n");
                            free(out_utf8);
                            out_utf8 = NULL;
                            continue; /* Try again, with a larger buffer. */
                        case EILSEQ:
                            cli_warnmsg("egg_filename_to_utf8: iconv error: An invalid multibyte sequence has been encountered in the input.\n");
                            break;
                        case EINVAL:
                            cli_warnmsg("egg_filename_to_utf8: iconv error: An incomplete multibyte sequence has been encountered in the input.\n");
                            break;
                        default:
                            cli_warnmsg("egg_filename_to_utf8: iconv error: Unexpected error code %d.\n", errno);
                    }
                    status = EGG_ERR;
                    goto done;
                }

                /* iconv succeeded, but probably didn't use the whole buffer. Free up the extra memory. */
                out_utf8 = cli_realloc(out_utf8, out_utf8_size - outbytesleft + 1);
                if (NULL == out_utf8) {
                    cli_errmsg("egg_filename_to_utf8: failure cli_realloc'ing converted filename.\n");
                    status = EGG_EMEM;
                    goto done;
                }
                out_utf8_size = out_utf8_size - outbytesleft;
            }

#else

            /*
             * No way to do the conversion.
             */
            goto done;

#endif
        }
    }

    *out      = out_utf8;
    *out_size = out_utf8_size;

    status = EGG_OK;

done:

#if defined(WIN32) && !defined(HAVE_ICONV)
    if (NULL != lpWideCharStr) {
        free(lpWideCharStr);
    }
#endif

    if (EGG_OK != status) {
        if (NULL != out_utf8) {
            free(out_utf8);
        }
    }

    return status;
}

static void egg_free_encrypt(egg_encrypt* encryptInfo)
{
    free(encryptInfo);
}

static cl_egg_error_t egg_parse_encrypt_header(const uint8_t* index, size_t size, egg_encrypt** encryptInfo)
{
    /*
     * The EGG specification (last updated 2016) for the encrypt header is not accurate.
     * The following describes my findings of the actual format for the encrypt header.
     *
     * The significant discrepancy is that the Size includes the size of the header iself, not just the data following it.
     * No other extra_field header's size field includes the size of itself.
     * This must be accounted for by the caller of this function (see the "Fudge factor" comments where this function is used).
     *
     *     |---------------|---------|------------------------------------------------------------------------------------------------------------|
     *     | Magic(ENCRYP) |    4    |    0x08D1470F                                                                                              |
     *     |---------------|---------|------------------------------------------------------------------------------------------------------------|
     *     | Bit flag      |    1    |    0                                                                                                       |
     *     |---------------|---------|------------------------------------------------------------------------------------------------------------|
     *     | Size          |    2    | sizeof( Magic ) + sizeof( Bit flag ) + sizeof( Size ) + sizeof( Encrypt Method ) + sizeof( Method Header ) |
     *     |---------------|---------|---|--------------------------------------------------------------------------------------------------------|
     *     | Encrypt       |    1    | 0 | KeyBase (XOR)                                                                                          |
     *     | Method        |         |---|--------------------------------------------------------------------------------------------------------|
     *     |               |         | 1 | AES128                                                                                                 |
     *     |               |         |---|--------------------------------------------------------------------------------------------------------|
     *     |               |         | 2 | AES256                                                                                                 |
     *     |               |         |---|--------------------------------------------------------------------------------------------------------|
     *     |               |         | 5 | LEA128                                                                                                 |
     *     |               |         |---|--------------------------------------------------------------------------------------------------------|
     *     |               |         | 6 | LEA256                                                                                                 |
     *     |---------------|---------|---|--------------------------------------------------------------------------------------------------------|
     *
     * Depending on the Method (XOR / AES/LEA128 / AES/LEA256) The above will be be followed one of the following Method Headers:
     *
     *   XOR (KeyBase):
     *     |---------------|---------|------------------------------------------------------------------------------------------------------------|
     *     | verify Data   |   12    |   Encryption Verification Data                                                                             |
     *     |---------------|---------|------------------------------------------------------------------------------------------------------------|
     *     | CRC32         |    4    |   Partial Block CRC                                                                                        |
     *     |---------------|---------|------------------------------------------------------------------------------------------------------------|
     *
     *   AES / LEA 128
     *     |---------------|---------|------------------------------------------------------------------------------------------------------------|
     *     | Magic(ENCRYP) |   10    |   AES/LEA Header                                                                                           |
     *     |---------------|---------|------------------------------------------------------------------------------------------------------------|
     *     | Magic(ENCRYP) |   10    |   AES/LEA Footer                                                                                           |
     *     |---------------|---------|------------------------------------------------------------------------------------------------------------|
     *
     *   AES / LEA 256
     *     |---------------|---------|------------------------------------------------------------------------------------------------------------|
     *     | Magic(ENCRYP) |   18    |   AES/LEA Header                                                                                           |
     *     |---------------|---------|------------------------------------------------------------------------------------------------------------|
     *     | Magic(ENCRYP) |   10    |   AES/LEA Footer                                                                                           |
     *     |---------------|---------|------------------------------------------------------------------------------------------------------------|
     */
    cl_egg_error_t status = EGG_ERR;
    egg_encrypt* encrypt  = NULL;

    if (!index || 0 == size || !encryptInfo) {
        cli_errmsg("egg_parse_encrypt_header: Invalid args.\n");
        goto done;
    }

    *encryptInfo = NULL;

    cli_dbgmsg("egg_parse_encrypt_header: Encrypted archive.\n");
    cli_dbgmsg("egg_parse_encrypt_header: size of encrypt extra_field data: %zu\n", size);

    if (size < sizeof(encrypt_header)) {
        cli_warnmsg("egg_parse_encrypt_header: Encrypt header size too small (%zu < %zu)\n", size, sizeof(encrypt_header));
        goto done;
    }

    encrypt = (egg_encrypt*)cli_calloc(1, sizeof(egg_encrypt));
    if (NULL == encrypt) {
        cli_errmsg("egg_parse_encrypt_header: Failed to allocate memory for egg_encrypt.\n");
        status = EGG_EMEM;
        goto done;
    }

    encrypt->header = (encrypt_header*)index;

    cli_dbgmsg("egg_parse_encrypt_header: encrypt_header->encrypt_method: %02x (%s)\n", encrypt->header->encrypt_method, getEncryptName(encrypt->header->encrypt_method));

    index += sizeof(encrypt_header);
    size -= sizeof(encrypt_header);

    if (ENCRYPT_HEADER_ENCRYPT_METHOD_XOR == encrypt->header->encrypt_method) {
        if (size != sizeof(zip2_xor_keybase)) {
            cli_warnmsg("egg_parse_encrypt_header: Encrypt header size for XOR is different than expected (%zu != %zu)\n", size, sizeof(zip2_xor_keybase));
            goto done;
        }

        encrypt->encrypt_al.xor = (zip2_xor_keybase*)index;

        cli_dbgmsg("egg_parse_encrypt_header: encrypt_header->crc32:          %08x\n", le32_to_host(encrypt->encrypt_al.xor->crc32));
    } else {
        /*
         * For AES/LEA, the additional information is found inside of embedded extra field.
         */
        switch (encrypt->header->encrypt_method) {
            case ENCRYPT_HEADER_ENCRYPT_METHOD_AES128:
            case ENCRYPT_HEADER_ENCRYPT_METHOD_LEA128: {
                if (size < sizeof(aes_lea_128)) {
                    cli_warnmsg("egg_parse_encrypt_header: Encrypt header size for AES/LEA128 is different than expected (%zu != %zu)\n", size, sizeof(aes_lea_128));
                    goto done;
                }

                encrypt->encrypt_al.al128 = (aes_lea_128*)index;

                index += sizeof(aes_lea_128);
                size -= sizeof(aes_lea_128);
                break;
            }
            case ENCRYPT_HEADER_ENCRYPT_METHOD_AES256:
            case ENCRYPT_HEADER_ENCRYPT_METHOD_LEA256: {
                if (size < sizeof(aes_lea_256)) {
                    cli_warnmsg("egg_parse_encrypt_header: Encrypt header size for AES/LEA256 is different than expected (%zu != %zu)\n", size, sizeof(aes_lea_256));
                    goto done;
                }

                encrypt->encrypt_al.al256 = (aes_lea_256*)index;

                index += sizeof(aes_lea_256);
                size -= sizeof(aes_lea_256);
                break;
            }
            default: {
                cli_warnmsg("egg_parse_encrypt_header: Unknown encrypt method: %d\n", encrypt->header->encrypt_method);
                goto done;
            }
        }
    }

    *encryptInfo = encrypt;
    status       = EGG_OK;

done:

    if (EGG_OK != status) {
        egg_free_encrypt(encrypt);
    }

    return status;
}

static cl_egg_error_t egg_parse_comment_header(const uint8_t* index, size_t size, extra_field* extraField, char** commentInfo)
{
    cl_egg_error_t status = EGG_ERR;

    char* comment            = NULL;
    char* comment_utf8       = NULL;
    size_t comment_utf8_size = 0;

    if (!index || 0 == size || !extraField || !commentInfo) {
        cli_errmsg("egg_parse_comment_headers: Invalid args!\n");
        return EGG_ERR;
    }

    *commentInfo = NULL;

    if (extraField->bit_flag & COMMENT_HEADER_FLAGS_ENCRYPT) {
        /*
         * comment is encrypted, nothing to be done.
         */
        *commentInfo = cli_strdup("<encrypted>");
        status       = EGG_ENCRYPTED;
        goto done;
    }

    /*
     * Store comment as UTF-8 string.
     */
    if (extraField->bit_flag & COMMENT_HEADER_FLAGS_MULTIBYTE_CODEPAGE_INSTEAD_OF_UTF8) {
        /*
         * Unlike with filenames, the multibyte string codepage (or "locale") is not present in comment headers.
         * Try conversion with codepage 65001.
         */
        if (EGG_OK != cli_codepage_to_utf8((char*)index, size, 65001, &comment_utf8, &comment_utf8_size)) {
            cli_dbgmsg("egg_parse_comment_header: failed to convert codepage \"0\" to UTF-8\n");
            comment_utf8 = cli_genfname(NULL);
        }
    } else {
        /* Should already be UTF-8. Use as-is.. */
        comment_utf8 = cli_strndup((char*)index, size);
        if (NULL == comment_utf8) {
            cli_dbgmsg("egg_parse_comment_header: failed to allocate comment buffer.\n");
            status = EGG_EMEM;
            goto done;
        }
    }
    comment = comment_utf8;

    cli_dbgmsg("egg_parse_comment_header: comment:          %s\n", comment);

    *commentInfo = comment;
    status       = EGG_OK;

done:
    if (EGG_OK != status) {
        if (comment) {
            free(comment);
        }
    }

    return status;
}

static void egg_free_egg_block(egg_block* block)
{
    free(block);
}

static cl_egg_error_t egg_parse_block_headers(egg_handle* handle, egg_block** block)
{
    cl_egg_error_t status = EGG_ERR;

    egg_block* eggBlock       = NULL;
    block_header* blockHeader = NULL;
    uint32_t magic            = 0;
    const uint8_t* index      = 0;

    if (!handle || !block) {
        cli_errmsg("egg_parse_block_headers: Invalid args!\n");
        return EGG_ERR;
    }

    *block = NULL;

    if (EGG_OK != EGG_VALIDATE_HANDLE(handle)) {
        cli_errmsg("egg_parse_block_headers: Invalid handle values!\n");
        goto done;
    }

    /*
     * 1st:
     *   Block headers must start with the block_header.
     */
    index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(block_header));
    if (!index) {
        cli_dbgmsg("egg_parse_block_headers: File buffer too small to contain block header.\n");
        goto done;
    }

    eggBlock = (egg_block*)cli_calloc(1, sizeof(egg_block));
    if (NULL == eggBlock) {
        cli_errmsg("egg_parse_block_headers: Failed to allocate memory for egg_block.\n");
        status = EGG_EMEM;
        goto done;
    }

    blockHeader           = (block_header*)index;
    eggBlock->blockHeader = blockHeader;

    if (BLOCK_HEADER_MAGIC != le32_to_host(blockHeader->magic)) {
        cli_dbgmsg("egg_parse_block_headers: Invalid block header magic: %08x.\n", le32_to_host(blockHeader->magic));
        goto done;
    }

    cli_dbgmsg("egg_parse_block_headers: block_header->magic:              %08x (%s)\n", le32_to_host(blockHeader->magic), getMagicHeaderName(le32_to_host(blockHeader->magic)));
    cli_dbgmsg("egg_parse_block_headers: block_header->compress_algorithm: %08x\n", blockHeader->compress_algorithm);
    cli_dbgmsg("egg_parse_block_headers: block_header->compress_hint:      %08x\n", blockHeader->compress_hint);
    cli_dbgmsg("egg_parse_block_headers: block_header->uncompress_size:    %08x\n", le32_to_host(blockHeader->uncompress_size));
    cli_dbgmsg("egg_parse_block_headers: block_header->compress_size:      %08x\n", le32_to_host(blockHeader->compress_size));
    cli_dbgmsg("egg_parse_block_headers: block_header->crc32:              %08x\n", le32_to_host(blockHeader->crc32));

    if (0 == le16_to_host(blockHeader->compress_size)) {
        cli_warnmsg("egg_parse_block_headers: Empty block!\n");
    }

    handle->offset += sizeof(block_header);

    /*
     * 2nd:
     *   After the block_header, the following extra field headers may be present:
     *      a) EOFARC
     */

    index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(magic32_t));
    if (!index) {
        cli_dbgmsg("egg_parse_block_headers: File buffer too small to contain end of archive magic bytes.\n");
        goto done;
    }

    magic = le32_to_host(*((uint32_t*)index));
    if (EOFARC != magic) {
        cli_dbgmsg("egg_parse_block_headers: EOFARC missing after block header.  Found these bytes instead: %08x. (%s)\n", magic, getMagicHeaderName(magic));
        goto done;
    }
    cli_dbgmsg("egg_parse_block_headers: End of block header.\n");
    handle->offset += sizeof(magic32_t);

    /*
     * Compressed data should follow the Block Header.
     */
    index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, blockHeader->compress_size);
    if (!index) {
        cli_dbgmsg("egg_parse_block_headers: File buffer too small to contain block compressed data.\n");
        goto done;
    }
    eggBlock->compressedData = (char*)index;
    handle->offset += blockHeader->compress_size;

    *block = eggBlock;
    status = EGG_OK;

done:
    if (EGG_OK != status) {
        if (eggBlock) {
            egg_free_egg_block(eggBlock);
        }
    }

    return status;
}

static void egg_free_egg_file(egg_file* file)
{
    uint32_t i = 0;

    if (NULL != file->filename.name_utf8) {
        free(file->filename.name_utf8);
        file->filename.name_utf8 = NULL;
    }
    if (NULL != file->blocks) {
        for (i = 0; i < file->nBlocks; i++) {
            egg_free_egg_block(file->blocks[i]);
            file->blocks[i] = NULL;
        }
        free(file->blocks);
        file->blocks = NULL;
    }
    if (NULL != file->comments) {
        for (i = 0; i < file->nComments; i++) {
            free(file->comments[i]);
            file->comments[i] = NULL;
        }
        free(file->comments);
        file->comments = NULL;
    }

    free(file);
}

static cl_egg_error_t egg_parse_archive_extra_field(egg_handle* handle)
{
    cl_egg_error_t status = EGG_ERR;

    const uint8_t* index    = NULL;
    extra_field* extraField = NULL;
    uint32_t magic          = 0;
    uint32_t size           = 0;

    if (!handle) {
        cli_errmsg("egg_parse_archive_extra_field: Invalid args!\n");
        return EGG_ERR;
    }

    if (EGG_OK != EGG_VALIDATE_HANDLE(handle)) {
        cli_errmsg("egg_parse_comment_headers: Invalid handle values!\n");
        goto done;
    }

    index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(extra_field));
    if (!index) {
        cli_dbgmsg("egg_parse_archive_extra_field: File buffer too small to contain extra_field header.\n");
        goto done;
    }

    extraField = (extra_field*)index;

    cli_dbgmsg("egg_parse_archive_extra_field: extra_field->magic:    %08x (%s)\n", le32_to_host(extraField->magic), getMagicHeaderName(le32_to_host(extraField->magic)));
    cli_dbgmsg("egg_parse_archive_extra_field: extra_field->bit_flag: %02x\n", extraField->bit_flag);

    handle->offset += sizeof(extra_field);

    if (extraField->bit_flag & EXTRA_FIELD_FLAGS_SIZE_IS_4BYTES) {
        /* size is uint32_t */
        index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(uint32_t));
        if (!index) {
            cli_dbgmsg("egg_parse_archive_extra_field: File buffer too small to contain extra_field header.\n");
            goto done;
        }

        size = le32_to_host(*(uint32_t*)index);

        handle->offset += sizeof(uint32_t);
    } else {
        /* size is uint16_t */
        index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(uint16_t));
        if (!index) {
            cli_dbgmsg("egg_parse_archive_extra_field: File buffer too small to contain extra_field header.\n");
            goto done;
        }

        size = le16_to_host(*(uint16_t*)index);

        handle->offset += sizeof(uint16_t);
    }

    cli_dbgmsg("egg_parse_archive_extra_field: extra_field->size:     %u\n", size);

    magic = le32_to_host(extraField->magic);

    switch (magic) {
        case SOLID_COMPRESSION_MAGIC: {
            /*
             * Solid archive is an archive packed with a special compression method,
             * which treats several or all files within the archive as one continuous data stream.
             */
            cli_dbgmsg("egg_parse_archive_extra_field: Solid archive. Several or all files within the archive treated as one continuous data stream.\n");

            if (0 != handle->bSolid) {
                cli_warnmsg("egg_parse_archive_extra_field: Encountered more than 1 Solid extra_field!\n");
                goto done;
            }
            handle->bSolid = 1;
            break;
        }
        case SPLIT_COMPRESSION_MAGIC: {
            /*
             * Split archives are single archives split into multiple .egg volumes.
             *
             * It is the first file if previous file’s ID is 0, and is the last file
             * if next file’s ID is 0.
             *
             * Header and Extra Field shouldn’t be cut when split compressing.
             * Compressed Block Data can be saved cut.
             * If header is excluded from the split size, insert Dummy Extra Field.
             *
             * If file compression ratio not applied when split compressing, modify
             * Magic of the header into Dummy Header or Skip Header (0xFFFF0000)
             * so it can be skipped.
             */
            split_compression* split = NULL;

            if (0 != handle->bSplit) {
                cli_warnmsg("egg_parse_archive_extra_field: Encountered more than 1 Split extra_field!\n");
                goto done;
            }
            handle->bSplit = 1;
            cli_warnmsg("egg_parse_archive_extra_field: Split archive. Split archives are single archives split into multiple .egg volumes.\n");

            if (sizeof(split_compression) != size) {
                cli_dbgmsg("egg_parse_archive_extra_field: size in extra_field is different than size of split_compression (%zu != %u).\n", sizeof(split_compression), size);
            } else {
                index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(split_compression));
                if (!index) {
                    cli_dbgmsg("egg_parse_archive_extra_field: File buffer too small to contain split compression header.\n");
                    goto done;
                }

                split = (split_compression*)index;

                handle->splitInfo = split;

                cli_dbgmsg("egg_parse_archive_extra_field: split_compression->prev_file_id: %08x\n", le32_to_host(split->prev_file_id));
                cli_dbgmsg("egg_parse_archive_extra_field: split_compression->next_file_id: %08x\n", le32_to_host(split->next_file_id));
            }
            break;
        }
        case ENCRYPT_HEADER_MAGIC: {
            /*
             * EGG files may have a global encryption header.
             * It is unclear if this means each file is encrypted, or that additional
             * data beyond the file contents is encrypted.
             */
            if (NULL != handle->encrypt) {
                cli_warnmsg("egg_parse_archive_extra_field: Encountered more than 1 encrypt_header!\n");
                goto done;
            }

            /*
             * Fudge factor.
             * The documentation is hazy about how the encrypt header works.
             * From testing, it seems that for encrypted files, the size in the extra_field includes the size OF the extra field.
             */
            size -= sizeof(extra_field) + sizeof(uint16_t);

            index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, size);
            if (!index) {
                cli_errmsg("egg_parse_archive_extra_field: File buffer too small to contain encryption headers.\n");
                goto done;
            }

            if (EGG_OK != egg_parse_encrypt_header(index, size, &handle->encrypt)) {
                cli_errmsg("egg_parse_archive_extra_field: Failed to parse encryption headers.\n");
                goto done;
            }
            break;
        }
        default: {
            cli_dbgmsg("egg_parse_archive_extra_field: unexpected header magic:    %08x (%s)\n", magic, getMagicHeaderName(magic));
        }
    }

    handle->offset += size;

    status = EGG_OK;

done:

    return status;
}

static cl_egg_error_t egg_parse_file_extra_field(egg_handle* handle, egg_file* eggFile)
{
    cl_egg_error_t status = EGG_ERR;

    const uint8_t* index    = NULL;
    extra_field* extraField = NULL;
    uint32_t magic          = 0;
    uint32_t size           = 0;

    if (!handle || !eggFile) {
        cli_errmsg("egg_parse_file_extra_field: Invalid args!\n");
        return EGG_ERR;
    }

    if (EGG_OK != EGG_VALIDATE_HANDLE(handle)) {
        cli_errmsg("egg_parse_file_extra_field: Invalid handle values!\n");
        goto done;
    }

    index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(extra_field));
    if (!index) {
        cli_dbgmsg("egg_parse_file_extra_field: File buffer too small to contain extra_field header.\n");
        goto done;
    }

    extraField = (extra_field*)index;

    cli_dbgmsg("egg_parse_file_extra_field: extra_field->magic:    %08x (%s)\n", le32_to_host(extraField->magic), getMagicHeaderName(le32_to_host(extraField->magic)));
    cli_dbgmsg("egg_parse_file_extra_field: extra_field->bit_flag: %02x\n", extraField->bit_flag);

    handle->offset += sizeof(extra_field);

    if (extraField->bit_flag & EXTRA_FIELD_FLAGS_SIZE_IS_4BYTES) {
        /* size is uint32_t */
        index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(uint32_t));
        if (!index) {
            cli_dbgmsg("egg_parse_file_extra_field: File buffer too small to contain extra_field header.\n");
            goto done;
        }

        size = le32_to_host(*(uint32_t*)index);

        handle->offset += sizeof(uint32_t);
    } else {
        /* size is uint16_t */
        index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(uint16_t));
        if (!index) {
            cli_dbgmsg("egg_parse_file_extra_field: File buffer too small to contain extra_field header.\n");
            goto done;
        }

        size = le16_to_host(*(uint16_t*)index);

        handle->offset += sizeof(uint16_t);
    }

    cli_dbgmsg("egg_parse_file_extra_field: extra_field->size:     %u\n", size);

    magic = le32_to_host(extraField->magic);

    switch (magic) {
        case FILENAME_HEADER_MAGIC: {
            /*
             * File Filename Header
             */
            uint16_t codepage       = 0; /* Windows code page https://docs.microsoft.com/en-us/windows/desktop/Intl/code-page-identifiers) */
            uint32_t name_size      = 0;
            uint32_t remaining_size = size;

            char* name_utf8       = NULL;
            size_t name_utf8_size = 0;

            if (NULL != eggFile->filename.name_utf8) {
                cli_warnmsg("egg_parse_file_extra_field: Encountered more than 1 filename_header!\n");
                goto done;
            }

            index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, size);
            if (!index) {
                cli_dbgmsg("egg_parse_file_extra_field: File buffer too small to contain name fields.\n");
                goto done;
            }

            if (extraField->bit_flag & FILENAME_HEADER_FLAGS_ENCRYPT)
                cli_dbgmsg("egg_parse_file_extra_field: filename_header->bit_flag: encrypted\n");
            else
                cli_dbgmsg("egg_parse_file_extra_field: filename_header->bit_flag: not encrypted\n");

            if (extraField->bit_flag & FILENAME_HEADER_FLAGS_RELATIVE_PATH_INSTEAD_OF_ABSOLUTE)
                cli_dbgmsg("egg_parse_file_extra_field: filename_header->bit_flag: relative-path\n");
            else
                cli_dbgmsg("egg_parse_file_extra_field: filename_header->bit_flag: absolute-path\n");

            if (extraField->bit_flag & FILENAME_HEADER_FLAGS_MULTIBYTE_CODEPAGE_INSTEAD_OF_UTF8)
                cli_dbgmsg("egg_parse_file_extra_field: filename_header->bit_flag: Windows Multibyte + codepage\n");
            else
                cli_dbgmsg("egg_parse_file_extra_field: filename_header->bit_flag: UTF-8\n");

            if (extraField->bit_flag & FILENAME_HEADER_FLAGS_MULTIBYTE_CODEPAGE_INSTEAD_OF_UTF8) {
                /* Utf-8 - header will include locale */
                /* Check that the size is big enough */
                if (remaining_size < sizeof(uint16_t)) {
                    cli_dbgmsg("egg_parse_file_extra_field: size too small for locale information.\n");
                    goto done;
                }
                codepage = *(uint16_t*)index;
                cli_dbgmsg("egg_parse_file_extra_field: filename_header->codepage:       %u\n", codepage);
                index += sizeof(uint16_t);
                handle->offset += sizeof(uint16_t);
                remaining_size -= sizeof(uint16_t);
            }

            if (extraField->bit_flag & FILENAME_HEADER_FLAGS_RELATIVE_PATH_INSTEAD_OF_ABSOLUTE) {
                /* header will include parent_path_id */
                /* Check that the size is big enough */
                if (remaining_size < sizeof(uint32_t)) {
                    cli_dbgmsg("egg_parse_file_extra_field: size too small for parent_path_id.\n");
                    goto done;
                }
                eggFile->filename.parent_path_id = *(uint16_t*)index;
                cli_dbgmsg("egg_parse_file_extra_field: filename_header->parent_path_id: %u\n", eggFile->filename.parent_path_id);
                index += sizeof(uint32_t);
                handle->offset += sizeof(uint32_t);
                remaining_size -= sizeof(uint32_t);
            }

            if (remaining_size == 0) {
                cli_dbgmsg("egg_parse_file_extra_field: size too small for name string.\n");
                goto done;
            }
            name_size = remaining_size;

            /*
             * Store name as UTF-8 string.
             */
            if (extraField->bit_flag & FILENAME_HEADER_FLAGS_MULTIBYTE_CODEPAGE_INSTEAD_OF_UTF8) {
                /* Convert ANSI codepage to UTF-8. EGG format explicitly supports:
                 * - 949 (Korean Unified Code)
                 * - 932 (Japanese Shift-JIS) */
                if (0 == codepage) {
                    if (EGG_OK != cli_codepage_to_utf8((char*)index, name_size, 65001, &name_utf8, &name_utf8_size)) {
                        cli_dbgmsg("egg_parse_file_extra_field: failed to convert codepage \"0\" to UTF-8\n");
                        name_utf8 = cli_genfname(NULL);
                    }
                } else {
                    if (EGG_OK != cli_codepage_to_utf8((char*)index, name_size, codepage, &name_utf8, &name_utf8_size)) {
                        cli_dbgmsg("egg_parse_file_extra_field: failed to convert codepage %u to UTF-8\n", codepage);
                        name_utf8 = cli_genfname(NULL);
                    }
                }
            } else {
                /* Should already be UTF-8. Use as-is.. */
                name_utf8 = cli_strndup((char*)index, name_size);
                if (NULL == name_utf8) {
                    cli_dbgmsg("egg_parse_file_extra_field: failed to allocate name buffer.\n");
                    status = EGG_EMEM;
                    goto done;
                }
            }
            eggFile->filename.name_utf8 = name_utf8;

            cli_dbgmsg("egg_parse_file_extra_field: filename_header->name: %s\n", eggFile->filename.name_utf8);

            break;
        }
        case COMMENT_HEADER_MAGIC: {
            /*
             * File Comment Header
             */
            cl_egg_error_t retval = EGG_ERR;
            char* comment         = NULL;

            index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, size);
            if (!index) {
                cli_dbgmsg("egg_parse_file_extra_field: File buffer too small to contain comment fields.\n");
                goto done;
            }

            if (EGG_OK != (retval = egg_parse_comment_header(index, size, extraField, &comment))) {
                cli_dbgmsg("egg_parse_file_extra_field: Issue parsing comment header. Error code: %u\n", retval);
                break;
            } else {
                /*
                 * Success?
                 */
                if (comment == NULL) {
                    /* Uh... no. */
                    cli_errmsg("egg_parse_file_extra_field: Logic error! Succesfully parsed comment header,"
                               " but did not return egg_comment information!\n");
                    goto done;
                } else {
                    /*
                     * Comment found. Add comment to our list.
                     */
                    eggFile->comments = (char**)cli_realloc(
                        (void*)eggFile->comments,
                        sizeof(char**) * (eggFile->nComments + 1));
                    if (NULL == eggFile->comments) {
                        status = EGG_EMEM;
                        goto done;
                    }
                    eggFile->comments[eggFile->nComments] = comment;
                    eggFile->nComments++;
                }
            }
            break;
        }
        case ENCRYPT_HEADER_MAGIC: {
            /*
             * File Encryption Header.
             */
            if (NULL != eggFile->encrypt) {
                cli_warnmsg("egg_parse_file_extra_field: Encountered more than 1 encrypt_header!\n");
                goto done;
            }

            /*
             * Fudge factor.
             * The documentation is hazy about how the encrypt header works.
             * From testing, it seems that for encrypted files, the size in the extra_field includes the size OF the extra field.
             */
            size -= sizeof(extra_field) + sizeof(uint16_t);

            index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, size);
            if (!index) {
                cli_errmsg("egg_parse_file_extra_field: File buffer too small to contain encryption fields.\n");
                goto done;
            }

            if (EGG_OK != egg_parse_encrypt_header(index, size, &eggFile->encrypt)) {
                cli_errmsg("egg_parse_file_extra_field: Failed to parse encrypt_header.\n");
                goto done;
            }
            break;
        }
        case WINDOWS_INFO_MAGIC: {
            windows_file_information* windowsFileInformation = NULL;

            if (NULL != eggFile->windowsFileInformation) {
                cli_warnmsg("egg_parse_file_extra_field: Encountered more than 1 windows_file_information!\n");
                goto done;
            }

            if (sizeof(windows_file_information) != size) {
                cli_warnmsg("egg_parse_file_extra_field: Invalid size of windows_file_information!\n");
            }

            index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(windows_file_information));
            if (!index) {
                cli_dbgmsg("egg_parse_file_extra_field: File buffer too small to contain windows info.\n");
                goto done;
            }

            windowsFileInformation          = (windows_file_information*)index;
            eggFile->windowsFileInformation = windowsFileInformation;

            cli_dbgmsg("egg_parse_file_extra_field: windows_file_information->last_modified_time:   %016llx\n", le64_to_host(windowsFileInformation->last_modified_time));
            cli_dbgmsg("egg_parse_file_extra_field: windows_file_information->attribute:            %08x\n", windowsFileInformation->attribute);
            break;
        }
        case POSIX_INFO_MAGIC: {
            posix_file_information* posixFileInformation = NULL;

            if (NULL != eggFile->posixFileInformation) {
                cli_warnmsg("egg_parse_file_extra_field: Encountered more than 1 posix_file_information!\n");
                goto done;
            }

            if (sizeof(posix_file_information) != size) {
                cli_warnmsg("egg_parse_file_extra_field: Invalid size of posix_file_information!\n");
            }

            index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(posix_file_information));
            if (!index) {
                cli_dbgmsg("egg_parse_file_extra_field: File buffer too small to contain posix info.\n");
                goto done;
            }

            posixFileInformation          = (posix_file_information*)index;
            eggFile->posixFileInformation = posixFileInformation;

            cli_dbgmsg("egg_parse_file_extra_field: posix_file_information->mode:                 %08x ", le32_to_host(posixFileInformation->mode));
            if (UNLIKELY(cli_debug_flag)) {
                /* File type flags */
                if (posixFileInformation->mode & POSIX_INFO_MODE_REG_FILE) {
                    printf("-");
                } else if (posixFileInformation->mode & POSIX_INFO_MODE_DIRECTORY) {
                    printf("d");
                } else if (posixFileInformation->mode & POSIX_INFO_MODE_CHAR_DEVICE) {
                    printf("c");
                } else if (posixFileInformation->mode & POSIX_INFO_MODE_BLOCK_DEVICE) {
                    printf("s");
                } else if (posixFileInformation->mode & POSIX_INFO_MODE_SOCKET) {
                    printf("s");
                } else if (posixFileInformation->mode & POSIX_INFO_MODE_FIFO) {
                    printf("p");
                } else if (posixFileInformation->mode & POSIX_INFO_MODE_SYM_LINK) {
                    printf("l");
                } else if (posixFileInformation->mode & POSIX_INFO_MODE_SOCKET) {
                    printf("s");
                }
                /* Owner/Group/Other permissions */
                if (posixFileInformation->mode & POSIX_INFO_MODE_PERM_OWNER_READ) {
                    printf("r");
                } else {
                    printf("-");
                }
                if (posixFileInformation->mode & POSIX_INFO_MODE_PERM_OWNER_WRITE) {
                    printf("w");
                } else {
                    printf("-");
                }
                if (posixFileInformation->mode & POSIX_INFO_MODE_SET_UID_BIT) {
                    printf("s");
                } else if (posixFileInformation->mode & POSIX_INFO_MODE_PERM_OWNER_EXECUTE) {
                    printf("x");
                } else {
                    printf("-");
                }
                if (posixFileInformation->mode & POSIX_INFO_MODE_PERM_GROUP_READ) {
                    printf("r");
                } else {
                    printf("-");
                }
                if (posixFileInformation->mode & POSIX_INFO_MODE_PERM_GROUP_WRITE) {
                    printf("w");
                } else {
                    printf("-");
                }
                if (posixFileInformation->mode & POSIX_INFO_MODE_SET_UID_BIT) {
                    printf("s");
                }
                if (posixFileInformation->mode & POSIX_INFO_MODE_SET_GROUPID_BIT) {
                    printf("s");
                }
                if (posixFileInformation->mode & POSIX_INFO_MODE_PERM_GROUP_EXECUTE) {
                    printf("x");
                } else {
                    printf("-");
                }
                if (posixFileInformation->mode & POSIX_INFO_MODE_PERM_OTHERS_READ) {
                    printf("r");
                } else {
                    printf("-");
                }
                if (posixFileInformation->mode & POSIX_INFO_MODE_PERM_OTHERS_WRITE) {
                    printf("w");
                } else {
                    printf("-");
                }
                if (posixFileInformation->mode & POSIX_INFO_MODE_PERM_OTHERS_EXECUTE) {
                    printf("x");
                } else {
                    printf("-");
                }
                /* Sticky Bit */
                if (posixFileInformation->mode & POSIX_INFO_MODE_STICKY_BIT)
                    printf("t");
                printf("\n");
            }

            cli_dbgmsg("egg_parse_file_extra_field: posix_file_information->uid:                  %08x\n", le32_to_host(posixFileInformation->uid));
            cli_dbgmsg("egg_parse_file_extra_field: posix_file_information->gid:                  %08x\n", le32_to_host(posixFileInformation->gid));
            cli_dbgmsg("egg_parse_file_extra_field: posix_file_information->last_modified_time:   %016llx\n", le64_to_host(posixFileInformation->last_modified_time));
            break;
        }
        case FILE_HEADER_MAGIC: {
            if (handle->bSolid) {
                cli_dbgmsg("egg_parse_file_extra_field: Solid archive - on to next file header.\n");
            } else {
                cli_warnmsg("egg_parse_file_extra_field: Missing EOFARC in non-solid/standard archive.\n");
            }
            break;
        }
        default: {
            cli_dbgmsg("egg_parse_file_extra_field: unexpected header magic:    %08x (%s)\n", magic, getMagicHeaderName(magic));
        }
    }

    handle->offset += size;

    status = EGG_OK;

done:

    return status;
}

static cl_egg_error_t egg_parse_file_headers(egg_handle* handle, egg_file** file)
{
    cl_egg_error_t status = EGG_ERR;
    cl_egg_error_t retval;

    egg_file* eggFile       = NULL;
    file_header* fileHeader = NULL;
    uint32_t magic          = 0;
    const uint8_t* index    = 0;

    if (!handle || !file) {
        cli_errmsg("egg_parse_file_headers: Invalid args!\n");
        return EGG_ERR;
    }

    *file = NULL;

    if (EGG_OK != EGG_VALIDATE_HANDLE(handle)) {
        cli_errmsg("egg_parse_file_headers: Invalid handle values!\n");
        goto done;
    }

    /*
     * 1st:
     *   File headers must start with the file_header.
     */
    index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(file_header));
    if (!index) {
        cli_dbgmsg("egg_parse_file_headers: File buffer too small to contain file header.\n");
        goto done;
    }

    eggFile = (egg_file*)cli_calloc(1, sizeof(egg_file));
    if (NULL == eggFile) {
        cli_errmsg("egg_parse_file_headers: Failed to allocate memory for egg_file.\n");
        status = EGG_EMEM;
        goto done;
    }

    fileHeader    = (file_header*)index;
    eggFile->file = fileHeader;

    if (FILE_HEADER_MAGIC != le32_to_host(fileHeader->magic)) {
        cli_dbgmsg("egg_parse_file_headers: Invalid file header magic: %08x (%s).\n", le32_to_host(fileHeader->magic), getMagicHeaderName(le32_to_host(fileHeader->magic)));
        goto done;
    }

    cli_dbgmsg("egg_parse_file_headers: file_header->magic:       %08x (%s)\n", le32_to_host(fileHeader->magic), getMagicHeaderName(le32_to_host(fileHeader->magic)));
    cli_dbgmsg("egg_parse_file_headers: file_header->file_id:     %08x\n", le32_to_host(fileHeader->file_id));
    cli_dbgmsg("egg_parse_file_headers: file_header->file_length: %016llx (%llu)\n",
               le64_to_host(fileHeader->file_length),
               le64_to_host(fileHeader->file_length));

    if (0 == le16_to_host(fileHeader->file_length)) {
        cli_dbgmsg("egg_parse_file_headers: Empty file!\n");
    }

    handle->offset += sizeof(file_header);

    /*
     * 2nd:
     *   After the file_header, the following extra field headers may be present:
     *      a) filename_header
     *      b) comment_header
     *      c) windows_file_information
     *      d) posix_file_information
     *      e) encrypt_header
     *      f) EOFARC
     */

    while (handle->map->len > handle->offset) {

        /* Get the next magic32_t */
        index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(magic32_t));
        if (!index) {
            cli_dbgmsg("egg_parse_file_headers: File buffer too small to contain end of archive magic bytes.\n");
            goto done;
        }

        magic = le32_to_host(*((uint32_t*)index));

        if (EOFARC == magic) {
            /*
             * File headers should conclude with EOFARC magic bytes.
             */
            handle->offset += sizeof(magic32_t);

            cli_dbgmsg("egg_parse_file_headers: End of archive headers.\n");
            break; /* Break out of the loop */
        } else {
            /*
             * Parse extra fields.
             */
            retval = egg_parse_file_extra_field(handle, eggFile);
            if (EGG_OK != retval) {
                cli_dbgmsg("egg_parse_file_headers: Failed to parse archive header, magic: %08x (%s)\n", magic, getMagicHeaderName(magic));
                break; /* Break out of the loop */
            }
        }
    }

    *file  = eggFile;
    status = EGG_OK;

done:
    if (EGG_OK != status) {
        if (eggFile) {
            egg_free_egg_file(eggFile);
        }
    }

    return status;
}

static void egg_free_egg_handle(egg_handle* handle)
{
    uint32_t i = 0;

    if (NULL != handle->encrypt) {
        egg_free_encrypt(handle->encrypt);
        handle->encrypt = NULL;
    }
    if (NULL != handle->files) {
        for (i = 0; i < handle->nFiles; i++) {
            egg_free_egg_file(handle->files[i]);
            handle->files[i] = NULL;
        }
        free(handle->files);
        handle->files = NULL;
    }
    if (NULL != handle->blocks) {
        for (i = 0; i < handle->nBlocks; i++) {
            egg_free_egg_block(handle->blocks[i]);
            handle->blocks[i] = NULL;
        }
        free(handle->blocks);
        handle->blocks = NULL;
    }
    if (NULL != handle->comments) {
        for (i = 0; i < handle->nComments; i++) {
            free(handle->comments[i]);
            handle->comments[i] = NULL;
        }
        free(handle->comments);
        handle->comments = NULL;
    }
}

static cl_egg_error_t egg_parse_archive_headers(egg_handle* handle)
{
    cl_egg_error_t status = EGG_ERR;
    cl_egg_error_t retval;

    egg_header* eggHeader = NULL;
    uint32_t magic        = 0;
    const uint8_t* index  = 0;

    if (!handle) {
        cli_errmsg("egg_parse_archive_headers: Invalid args!\n");
        return EGG_ERR;
    }

    if (EGG_OK != EGG_VALIDATE_HANDLE(handle)) {
        cli_errmsg("egg_parse_archive_headers: Invalid handle values!\n");
        goto done;
    }

    /*
     * 1st:
     *   Archive headers begins with the egg_header.
     */

    index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(egg_header));
    if (!index) {
        cli_dbgmsg("egg_parse_archive_headers: File buffer too small to contain egg_header.\n");
        goto done;
    }

    eggHeader = (egg_header*)index;

    if (EGG_HEADER_MAGIC != le32_to_host(eggHeader->magic)) {
        cli_dbgmsg("egg_parse_archive_headers: Invalid egg header magic: %08x.\n", le32_to_host(eggHeader->magic));
        goto done;
    }

    cli_dbgmsg("egg_parse_archive_headers: egg_header->magic:     %08x (%s)\n", le32_to_host(eggHeader->magic), getMagicHeaderName(le32_to_host(eggHeader->magic)));
    cli_dbgmsg("egg_parse_archive_headers: egg_header->version:   %04x\n", le16_to_host(eggHeader->version));
    cli_dbgmsg("egg_parse_archive_headers: egg_header->header_id: %08x\n", le32_to_host(eggHeader->header_id));
    cli_dbgmsg("egg_parse_archive_headers: egg_header->reserved:  %08x\n", le32_to_host(eggHeader->reserved));

    if (EGG_HEADER_VERSION != le16_to_host(eggHeader->version)) {
        cli_dbgmsg("egg_parse_archive_headers: Unexpected EGG archive version #: %04x.\n",
                   le16_to_host(eggHeader->version));
    }

    handle->offset += sizeof(egg_header);

    /*
     * 2nd:
     *   Egg Header may be followed by:
     *      a) split_compression header  and/or
     *      b) solid_compression
     *      c) global encryption header
     *      d) EOFARC
     */

    while (handle->map->len > handle->offset) {

        /* Get the next magic32_t */
        index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(magic32_t));
        if (!index) {
            cli_dbgmsg("egg_parse_archive_headers: File buffer too small to contain end of archive magic bytes.\n");
            goto done;
        }

        magic = le32_to_host(*((uint32_t*)index));

        if (EOFARC == magic) {
            /*
             * Archive headers should conclude with EOFARC magic bytes.
             */
            handle->offset += sizeof(magic32_t);

            cli_dbgmsg("egg_parse_archive_headers: End of archive headers.\n");
            break; /* Break out of the loop */
        } else {
            /*
             * Parse extra fields.
             */
            retval = egg_parse_archive_extra_field(handle);
            if (EGG_OK != retval) {
                cli_dbgmsg("egg_parse_archive_headers: Failed to parse archive header, magic: %08x (%s)\n", magic, getMagicHeaderName(magic));
                break; /* Break out of the loop */
            }
        }
    }

    status = EGG_OK;

done:
    return status;
}

cl_egg_error_t cli_egg_open(fmap_t* map, size_t sfx_offset, void** hArchive, char*** comments, uint32_t* nComments)
{
    cl_egg_error_t status = EGG_ERR;
    cl_egg_error_t retval = EGG_ERR;
    egg_handle* handle    = NULL;
    uint32_t magic        = 0;
    const uint8_t* index  = 0;

    if (!map || !hArchive) {
        cli_errmsg("cli_egg_open: Invalid args!\n");
        return EGG_ERR;
    }

    handle = (egg_handle*)cli_calloc(1, sizeof(egg_handle));
    if (NULL == handle) {
        cli_errmsg("cli_egg_open: Failed to allocate memory for egg_handle.\n");
        status = EGG_EMEM;
        goto done;
    }
    handle->map    = map;
    handle->offset = sfx_offset;

    /*
     * 1st:
     *   Parse the archive headers.
     */
    if (EGG_OK != (retval = egg_parse_archive_headers(handle))) {
        cli_warnmsg("cli_egg_open: Failed to parse archive headers!\n");
        goto done;
    }

    /*
     * 2nd:
     *   Archive headers may be followed by:
     *      a) 0+ file headers
     *      b) 0+ block headers
     *      c) 0+ archive comment headers
     */
    while (EGG_OK == retval) {

        /* Get the next magic32_t */
        index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(magic32_t));
        if (!index) {
            cli_dbgmsg("cli_egg_open: No more data in archive.\n");
            break;
        }

        magic = le32_to_host(*((uint32_t*)index));

        if (EOFARC == magic) {
            /*
             * Archive headers should conclude with EOFARC magic bytes.
             */
            handle->offset += sizeof(magic32_t);

            if (handle->map->len > handle->offset) {
                cli_warnmsg("Apparent end to EGG archive, but an addition %zu bytes of data exists in the file!\n",
                            handle->map->len - handle->offset);
            } else {
                cli_dbgmsg("cli_egg_open: Successfully indexed EGG archive!\n");
            }

            break; /* Break out of the loop */
        } else if (FILE_HEADER_MAGIC == magic) {
            /*
             * Archive File Header
             */
            egg_file* found_file = NULL;
            if (EGG_OK != (retval = egg_parse_file_headers(handle, &found_file))) {
                cli_dbgmsg("cli_egg_open: Issue parsing file header. Error code: %u\n", retval);
                goto done;
            } else if (found_file == NULL) {
                cli_errmsg("cli_egg_open: Logic error! Succesfully parsed file headers,"
                           " but did not return egg_file information!\n");
                goto done;
            } else {
                /* Add file to list. */
                handle->files = (egg_file**)cli_realloc(
                    (void*)handle->files,
                    sizeof(egg_file*) * (handle->nFiles + 1));
                if (NULL == handle->files) {
                    status = EGG_EMEM;
                    goto done;
                }
                handle->files[handle->nFiles] = found_file;
                handle->nFiles++;
            }
        } else if (BLOCK_HEADER_MAGIC == magic) {
            /*
             * Archive Block Header
             */
            egg_block* found_block = NULL;
            if (EGG_OK != (retval = egg_parse_block_headers(handle, &found_block))) {
                cli_dbgmsg("cli_egg_open: Issue parsing block header. Error code: %u\n", retval);
                goto done;
            } else if (found_block == NULL) {
                cli_errmsg("cli_egg_open: Logic error! Succesfully parsed block headers,"
                           " but did not return egg_block information!\n");
                goto done;
            } else {
                /* Add block to list. */
                if (handle->bSolid) {
                    handle->blocks = (egg_block**)cli_realloc(
                        (void*)handle->blocks,
                        sizeof(egg_block*) * (handle->nBlocks + 1));
                    if (NULL == handle->blocks) {
                        status = EGG_EMEM;
                        goto done;
                    }
                    handle->blocks[handle->nBlocks] = found_block;
                    handle->nBlocks++;
                } else {
                    egg_file* eggFile = NULL;
                    /*
                     * Associate block with most recently added file.
                     */
                    if (handle->nFiles == 0) {
                        cli_dbgmsg("cli_egg_open: No file found for block in non-solid archive.\n");
                        // TODO: create an unamed block.
                    } else {
                        eggFile = handle->files[handle->nFiles - 1];

                        /* Add block to list. */
                        eggFile->blocks = (egg_block**)cli_realloc(
                            (void*)eggFile->blocks,
                            sizeof(egg_block*) * (eggFile->nBlocks + 1));
                        if (NULL == eggFile->blocks) {
                            status = EGG_EMEM;
                            goto done;
                        }
                        eggFile->blocks[eggFile->nBlocks] = found_block;
                        eggFile->nBlocks++;
                    }
                }
            }
        } else if (COMMENT_HEADER_MAGIC == magic) {
            /*
             * Parse extra field for archive comment header.
             */
            extra_field* extraField = NULL;
            char* comment           = NULL;
            uint32_t size           = 0;

            index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(extra_field));
            if (!index) {
                cli_dbgmsg("cli_egg_open: File buffer too small to contain extra_field header.\n");
                goto done;
            }

            extraField = (extra_field*)index;

            cli_dbgmsg("cli_egg_open: archive comment extra_field->magic:    %08x (%s)\n", le32_to_host(extraField->magic), getMagicHeaderName(le32_to_host(extraField->magic)));
            cli_dbgmsg("cli_egg_open: archive comment extra_field->bit_flag: %02x\n", extraField->bit_flag);

            handle->offset += sizeof(extra_field);

            if (extraField->bit_flag & EXTRA_FIELD_FLAGS_SIZE_IS_4BYTES) {
                /* size is uint32_t */
                index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(uint32_t));
                if (!index) {
                    cli_dbgmsg("cli_egg_open: File buffer too small to contain archive comment extra_field header.\n");
                    goto done;
                }

                size = le32_to_host(*(uint32_t*)index);

                handle->offset += sizeof(uint32_t);
            } else {
                /* size is uint16_t */
                index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, sizeof(uint16_t));
                if (!index) {
                    cli_dbgmsg("cli_egg_open: File buffer too small to contain archive comment extra_field header.\n");
                    goto done;
                }

                size = le16_to_host(*(uint16_t*)index);

                handle->offset += sizeof(uint16_t);
            }

            cli_dbgmsg("cli_egg_open: archive comment extra_field->size:     %u\n", size);

            index = (const uint8_t*)fmap_need_off_once(handle->map, handle->offset, size);
            if (!index) {
                cli_dbgmsg("cli_egg_open: File buffer too small to contain extra_field header.\n");
                goto done;
            }

            retval = egg_parse_comment_header(index, size, extraField, &comment);
            if (EGG_OK != retval) {
                cli_dbgmsg("cli_egg_open: Failed to parse archive comment extra_field data.\n");
                goto done;
            }

            handle->comments = (char**)cli_realloc(
                (void*)handle->comments,
                sizeof(char**) * (handle->nComments + 1));
            if (NULL == handle->comments) {
                status = EGG_EMEM;
                goto done;
            }
            handle->comments[handle->nComments] = comment;
            handle->nComments++;
            handle->offset += size;
        } else {
            cli_dbgmsg("cli_egg_open: unexpected header magic:               %08x (%s)\n", magic, getMagicHeaderName(magic));
            status = EGG_ERR;
            goto done;
        }
    }

    if (EGG_OK != retval) {
        if (EGG_BREAK == retval) {
            /* End of archive. */
            if ((handle->bSplit) && (handle->splitInfo->next_file_id != 0))
                cli_warnmsg("cli_egg_open: Abrupt end to EGG volume!\n");
            else
                cli_dbgmsg("cli_egg_open: End of EGG volume in split archive.\n");
        } else {
            /* Something went wrong. */
            cli_warnmsg("cli_egg_open: Failed to parse file headers!\n");
        }
    }

    *hArchive  = handle;
    *comments  = handle->comments;
    *nComments = handle->nComments;

    status = EGG_OK;

done:
    if (EGG_OK != status) {
        if (handle)
            egg_free_egg_handle(handle);
        *hArchive = NULL;
    }
    return status;
}

cl_egg_error_t cli_egg_peek_file_header(void* hArchive, cl_egg_metadata* file_metadata)
{
    cl_egg_error_t status = EGG_ERR;
    egg_handle* handle    = NULL;
    egg_file* currFile    = NULL;

    if (!hArchive || !file_metadata) {
        cli_errmsg("cli_egg_peek_file_header: Invalid args!\n");
        return EGG_ERR;
    }

    handle = (egg_handle*)hArchive;
    if (EGG_OK != EGG_VALIDATE_HANDLE(handle)) {
        cli_errmsg("cli_egg_peek_file_header: Invalid handle values!\n");
        goto done;
    }

    memset(file_metadata, 0, sizeof(cl_egg_metadata));

    if (handle->fileExtractionIndex >= handle->nFiles) {
        status = EGG_BREAK;
        goto done;
    }

    currFile = handle->files[handle->fileExtractionIndex];
    if (NULL == currFile) {
        cli_errmsg("cli_egg_peek_file_header: invalid egg_file pointer!\n");
        goto done;
    }

    if (NULL == currFile->file) {
        cli_errmsg("cli_egg_peek_file_header: egg_file is missing file header!\n");
        goto done;
    }

    if (handle->bSolid) {
        /*
         * TODO: no idea.
         */
        file_metadata->pack_size   = 0;
        file_metadata->unpack_size = currFile->file->file_length;
    } else {
        uint64_t i = 0;
        if (!currFile->blocks) {
            cli_dbgmsg("cli_egg_peek_file_header: Empty file!\n");
        }
        for (i = 0; i < currFile->nBlocks; i++) {
            egg_block* currBlock = currFile->blocks[i];

            if (!currBlock->blockHeader) {
                cli_errmsg("cli_egg_peek_file_header: egg_block missing block_header!\n");
                goto done;
            }
            file_metadata->pack_size += currBlock->blockHeader->compress_size;
            file_metadata->unpack_size += currBlock->blockHeader->uncompress_size;
        }
        if (file_metadata->unpack_size != currFile->file->file_length) {
            cli_warnmsg("cli_egg_peek_file_header: sum of block uncompress_size's does not match listed file_length!\n");
        }
    }

    file_metadata->filename = strdup(currFile->filename.name_utf8);

    if (NULL != currFile->encrypt)
        file_metadata->encrypted = 1;

    if (currFile->posixFileInformation && currFile->posixFileInformation->mode & POSIX_INFO_MODE_DIRECTORY)
        file_metadata->is_dir = 1;
    else if (currFile->windowsFileInformation && currFile->windowsFileInformation->attribute & WINDOWS_INFO_ATTRIBUTE_DIRECTORY)
        file_metadata->is_dir = 1;

    status = EGG_OK;
done:
    return status;
}

cl_egg_error_t cli_egg_deflate_decompress(char* compressed, size_t compressed_size, char** decompressed, size_t* decompressed_size)
{
    cl_egg_error_t status = EGG_ERR;

    uint8_t* temp    = NULL;
    uint8_t* decoded = NULL;
    uint32_t declen = 0, capacity = 0;

    z_stream stream;
    int zstat;

    if (NULL == compressed || compressed_size == 0 || NULL == decompressed || NULL == decompressed_size) {
        cli_errmsg("cli_egg_deflate_decompress: Invalid args!\n");
        status = EGG_ERR;
        goto done;
    }

    *decompressed      = NULL;
    *decompressed_size = 0;

    if (!(decoded = (uint8_t*)cli_calloc(BUFSIZ, sizeof(uint8_t)))) {
        cli_errmsg("cli_egg_deflate_decompress: cannot allocate memory for decompressed output\n");
        status = EGG_EMEM;
        goto done;
    }

    capacity = BUFSIZ;

    memset(&stream, 0, sizeof(stream));
    stream.next_in   = (Bytef*)compressed;
    stream.avail_in  = compressed_size;
    stream.next_out  = (Bytef*)decoded;
    stream.avail_out = BUFSIZ;

    zstat = inflateInit2(&stream, -15);
    if (zstat != Z_OK) {
        cli_warnmsg("cli_egg_deflate_decompress: inflateInit failed\n");
        status = EGG_EMEM;
        goto done;
    }

    /* initial inflate */
    zstat = inflate(&stream, Z_NO_FLUSH);

    /* check if nothing written whatsoever */
    if ((zstat != Z_OK) && (stream.avail_out == BUFSIZ)) {
        /* Inflation failed */
        cli_errmsg("cli_egg_deflate_decompress: failed to decompress data\n");
        status = EGG_ERR;
        goto done;
    }

    while (zstat == Z_OK && stream.avail_in) {
        /* extend output capacity if needed,*/
        if (stream.avail_out == 0) {
            if (!(temp = cli_realloc(decoded, capacity + BUFSIZ))) {
                cli_errmsg("cli_egg_deflate_decompress: cannot reallocate memory for decompressed output\n");
                status = EGG_EMEM;
                goto done;
            }
            decoded          = temp;
            stream.next_out  = decoded + capacity;
            stream.avail_out = BUFSIZ;
            declen += BUFSIZ;
            capacity += BUFSIZ;
        }

        /* continue inflation */
        zstat = inflate(&stream, Z_NO_FLUSH);
    }

    /* add end fragment to decoded length */
    declen += (BUFSIZ - stream.avail_out);

    /* error handling */
    switch (zstat) {
        case Z_OK:
            cli_dbgmsg("cli_egg_deflate_decompress: Z_OK on stream decompression\n");
            /* intentional fall-through */
        case Z_STREAM_END:
            cli_dbgmsg("cli_egg_deflate_decompress: decompressed %lu bytes from %lu total bytes (%lu bytes remaining)\n",
                       (unsigned long)declen, (unsigned long)(compressed_size), (unsigned long)(stream.avail_in));
            break;

        /* potentially fatal - *mostly* ignored as per older version */
        case Z_STREAM_ERROR:
        case Z_NEED_DICT:
        case Z_DATA_ERROR:
        case Z_MEM_ERROR:
        default:
            if (stream.msg)
                cli_dbgmsg("cli_egg_deflate_decompress: after decompressing %lu bytes, got error \"%s\"\n",
                           (unsigned long)declen, stream.msg);
            else
                cli_dbgmsg("cli_egg_deflate_decompress: after decompressing %lu bytes, got error %d\n",
                           (unsigned long)declen, zstat);

            if (declen == 0) {
                cli_dbgmsg("cli_egg_deflate_decompress: no bytes were decompressed.\n");

                status = EGG_ERR;
            }
            break;
    }

    *decompressed      = (char*)decoded;
    *decompressed_size = declen;

    status = EGG_OK;

done:

    (void)inflateEnd(&stream);

    if (EGG_OK != status) {
        free(decoded);
    }

    return status;
}

#ifdef HAVE_BZLIB_H
cl_egg_error_t cli_egg_bzip2_decompress(char* compressed, size_t compressed_size, char** decompressed, size_t* decompressed_size)
{
    cl_egg_error_t status = EGG_ERR;

    char* temp      = NULL;
    char* decoded   = NULL;
    uint32_t declen = 0, capacity = 0;

    bz_stream stream;
    int bzstat;

    if (NULL == compressed || compressed_size == 0 || NULL == decompressed || NULL == decompressed_size) {
        cli_errmsg("cli_egg_bzip2_decompress: Invalid args!\n");
        status = EGG_ERR;
        goto done;
    }

    *decompressed      = NULL;
    *decompressed_size = 0;

    if (!(decoded = (char*)cli_calloc(BUFSIZ, sizeof(Bytef)))) {
        cli_errmsg("cli_egg_bzip2_decompress: cannot allocate memory for decompressed output\n");
        status = EGG_EMEM;
        goto done;
    }

    capacity = BUFSIZ;

    memset(&stream, 0, sizeof(stream));
    stream.next_in   = compressed;
    stream.avail_in  = compressed_size;
    stream.next_out  = decoded;
    stream.avail_out = BUFSIZ;

    if (BZ_OK != (bzstat = BZ2_bzDecompressInit(&stream, 0, 0))) {
        cli_warnmsg("cli_egg_bzip2_decompress: bzinit failed\n");
        status = EGG_EMEM;
        goto done;
    }

    /* initial inflate */
    bzstat = BZ2_bzDecompress(&stream);

    /* check if nothing written whatsoever */
    if ((bzstat != BZ_OK) && (stream.avail_out == BUFSIZ)) {
        /* Inflation failed */
        cli_errmsg("cli_egg_bzip2_decompress: failed to decompress data\n");
        status = EGG_ERR;
        goto done;
    }

    while (bzstat == BZ_OK && stream.avail_in) {
        /* extend output capacity if needed,*/
        if (stream.avail_out == 0) {
            if (!(temp = cli_realloc(decoded, capacity + BUFSIZ))) {
                cli_errmsg("cli_egg_bzip2_decompress: cannot reallocate memory for decompressed output\n");
                status = EGG_EMEM;
                goto done;
            }
            decoded          = temp;
            stream.next_out  = decoded + capacity;
            stream.avail_out = BUFSIZ;
            declen += BUFSIZ;
            capacity += BUFSIZ;
        }

        /* continue inflation */
        bzstat = BZ2_bzDecompress(&stream);
    }

    /* add end fragment to decoded length */
    declen += (BUFSIZ - stream.avail_out);

    /* error handling */
    switch (bzstat) {
        case BZ_OK:
            cli_dbgmsg("cli_egg_bzip2_decompress: BZ_OK on stream decompression\n");
            /* intentional fall-through */
        case BZ_STREAM_END:
            cli_dbgmsg("cli_egg_bzip2_decompress: decompressed %lu bytes from %lu total bytes (%lu bytes remaining)\n",
                       (unsigned long)declen, (unsigned long)(compressed_size), (unsigned long)(stream.avail_in));
            break;

        /* potentially fatal */
        case BZ_DATA_ERROR:
        case BZ_MEM_ERROR:
        default:
            cli_dbgmsg("cli_egg_bzip2_decompress: after decompressing %lu bytes, got error %d\n",
                       (unsigned long)declen, bzstat);

            if (declen == 0) {
                cli_dbgmsg("cli_egg_bzip2_decompress: no bytes were decompressed.\n");

                status = EGG_ERR;
            }
            break;
    }

    *decompressed      = (char*)decoded;
    *decompressed_size = declen;

    status = EGG_OK;

done:

    (void)BZ2_bzDecompressEnd(&stream);

    if (EGG_OK != status) {
        free(decoded);
    }

    return status;
}
#endif

cl_egg_error_t cli_egg_lzma_decompress(char* compressed, size_t compressed_size, char** decompressed, size_t* decompressed_size)
{
    cl_egg_error_t status = EGG_ERR;

    uint8_t* temp    = NULL;
    uint8_t* decoded = NULL;
    uint32_t declen = 0, capacity = 0;

    struct CLI_LZMA stream;
    int lzmastat;

    if (NULL == compressed || compressed_size == 0 || NULL == decompressed || NULL == decompressed_size) {
        cli_errmsg("cli_egg_lzma_decompress: Invalid args!\n");
        status = EGG_ERR;
        goto done;
    }

    *decompressed      = NULL;
    *decompressed_size = 0;

    if (!(decoded = (uint8_t*)cli_calloc(BUFSIZ, sizeof(char)))) {
        cli_errmsg("cli_egg_lzma_decompress: cannot allocate memory for decompressed output\n");
        status = EGG_EMEM;
        goto done;
    }

    capacity = BUFSIZ;

    memset(&stream, 0, sizeof(stream));
    stream.next_in   = (Bytef*)compressed;
    stream.avail_in  = compressed_size;
    stream.next_out  = (Bytef*)decoded;
    stream.avail_out = BUFSIZ;

    lzmastat = cli_LzmaInit(&stream, 0);
    if (lzmastat != LZMA_RESULT_OK) {
        cli_warnmsg("cli_egg_lzma_decompress: inflateInit failed\n");
        status = EGG_EMEM;
        goto done;
    }

    /* initial inflate */
    lzmastat = cli_LzmaDecode(&stream);

    /* check if nothing written whatsoever */
    if ((lzmastat != LZMA_RESULT_OK) && (stream.avail_out == BUFSIZ)) {
        /* Inflation failed */
        cli_errmsg("cli_egg_lzma_decompress: failed to decompress data\n");
        status = EGG_ERR;
        goto done;
    }

    while (lzmastat == LZMA_RESULT_OK && stream.avail_in) {
        /* extend output capacity if needed,*/
        if (stream.avail_out == 0) {
            if (!(temp = cli_realloc(decoded, capacity + BUFSIZ))) {
                cli_errmsg("cli_egg_lzma_decompress: cannot reallocate memory for decompressed output\n");
                status = EGG_EMEM;
                goto done;
            }
            decoded          = temp;
            stream.next_out  = decoded + capacity;
            stream.avail_out = BUFSIZ;
            declen += BUFSIZ;
            capacity += BUFSIZ;
        }

        /* continue inflation */
        lzmastat = cli_LzmaDecode(&stream);
    }

    /* add end fragment to decoded length */
    declen += (BUFSIZ - stream.avail_out);

    /* error handling */
    switch (lzmastat) {
        case LZMA_RESULT_OK:
            cli_dbgmsg("cli_egg_lzma_decompress: Z_OK on stream decompression\n");
            /* intentional fall-through */
        case LZMA_STREAM_END:
            cli_dbgmsg("cli_egg_lzma_decompress: decompressed %lu bytes from %lu total bytes (%lu bytes remaining)\n",
                       (unsigned long)declen, (unsigned long)(compressed_size), (unsigned long)(stream.avail_in));
            break;

        /* potentially fatal */
        case LZMA_RESULT_DATA_ERROR:
        default:
            cli_dbgmsg("cli_egg_lzma_decompress: after decompressing %lu bytes, got error %d\n",
                       (unsigned long)declen, lzmastat);

            if (declen == 0) {
                cli_dbgmsg("cli_egg_lzma_decompress: no bytes were decompressed.\n");

                status = EGG_ERR;
            }
            break;
    }

    *decompressed      = (char*)decoded;
    *decompressed_size = declen;

    status = EGG_OK;

done:

    (void)cli_LzmaShutdown(&stream);

    if (EGG_OK != status) {
        free(decoded);
    }

    return status;
}

cl_egg_error_t cli_egg_extract_file(void* hArchive, const char** filename, const char** output_buffer, size_t* output_buffer_length)
{
    cl_egg_error_t status      = EGG_ERR;
    egg_handle* handle         = NULL;
    egg_file* currFile         = NULL;
    char* decompressed         = NULL;
    uint64_t decompressed_size = 0;
    uint64_t i                 = 0;

    if (!hArchive || !filename || !output_buffer || !output_buffer_length) {
        cli_errmsg("cli_egg_extract_file: Invalid args!\n");
        goto done;
    }

    *output_buffer        = NULL;
    *output_buffer_length = 0;

    handle = (egg_handle*)hArchive;
    if (EGG_OK != EGG_VALIDATE_HANDLE(handle)) {
        cli_errmsg("cli_egg_extract_file: Invalid handle values!\n");
        goto done;
    }

    if (handle->fileExtractionIndex >= handle->nFiles) {
        cli_errmsg("cli_egg_extract_file: File index exceeds number of files in archive!\n");
        goto done;
    }

    currFile = handle->files[handle->fileExtractionIndex];
    if (NULL == currFile) {
        cli_errmsg("cli_egg_extract_file: invalid egg_file pointer!\n");
        goto done;
    }

    if (NULL == currFile->file) {
        cli_errmsg("cli_egg_extract_file: egg_file is missing file header!\n");
        goto done;
    }

    if (handle->bSolid) {
        /*
         * TODO: no idea.
         */
    } else {
        if (currFile->nBlocks == 0 || currFile->blocks == NULL) {
            cli_dbgmsg("cli_egg_extract_file: Empty file!\n");
        }

        for (i = 0; i < currFile->nBlocks; i++) {
            egg_block* currBlock  = currFile->blocks[i];
            cl_egg_error_t retval = EGG_ERR;

            if (NULL == currBlock->blockHeader) {
                cli_errmsg("cli_egg_extract_file: current egg_block missing header!\n");
                break;
            }
            switch (currBlock->blockHeader->compress_algorithm) {
                case BLOCK_HEADER_COMPRESS_ALGORITHM_STORE: {
                    /*
                     * No compression. Woohoo!
                     */
                    if (currBlock->blockHeader->compress_size == 0) {
                        cli_warnmsg("cli_egg_extract_file: blockHeader compress_size is 0!\n");
                        break;
                    } else if (currBlock->blockHeader->compress_size != currBlock->blockHeader->uncompress_size) {
                        cli_warnmsg("cli_egg_extract_file: blockHeader compress_size != uncompress_size!\n");
                        break;
                    }
                    decompressed = cli_realloc(decompressed, (size_t)decompressed_size + currBlock->blockHeader->compress_size);
                    if (NULL == decompressed) {
                        cli_errmsg("cli_egg_extract_file: Failed to allocate %llu bytes for decompressed file!\n",
                                   decompressed_size);
                        status = EGG_EMEM;
                        goto done;
                    }

                    memcpy(decompressed + decompressed_size, currBlock->compressedData, currBlock->blockHeader->compress_size);
                    decompressed_size += currBlock->blockHeader->compress_size;

                    retval = EGG_OK;
                    break;
                }
                case BLOCK_HEADER_COMPRESS_ALGORITHM_DEFLATE: {
                    char* decompressed_block       = NULL;
                    size_t decompressed_block_size = 0;

                    if (EGG_OK != cli_egg_deflate_decompress(currBlock->compressedData,
                                                             currBlock->blockHeader->compress_size,
                                                             &decompressed_block,
                                                             &decompressed_block_size)) {
                        /* Failed to decompress block */
                        cli_warnmsg("Failed to decompress RFC 1951 deflate compressed block\n");
                        goto done;
                    }
                    /* Decompressed block. Add it to the file data */
                    decompressed = cli_realloc(decompressed, (size_t)decompressed_size + decompressed_block_size);
                    if (NULL == decompressed) {
                        cli_errmsg("cli_egg_extract_file: Failed to allocate %llu bytes for decompressed file!\n",
                                   decompressed_size);
                        free(decompressed_block);
                        status = EGG_EMEM;
                        goto done;
                    }

                    memcpy(decompressed + decompressed_size, decompressed_block, decompressed_block_size);
                    decompressed_size += decompressed_block_size;

                    free(decompressed_block);

                    retval = EGG_OK;
                    break;
                }
                case BLOCK_HEADER_COMPRESS_ALGORITHM_BZIP2: {
#if HAVE_BZLIB_H
                    char* decompressed_block       = NULL;
                    size_t decompressed_block_size = 0;

                    if (EGG_OK != cli_egg_bzip2_decompress(currBlock->compressedData,
                                                           currBlock->blockHeader->compress_size,
                                                           &decompressed_block,
                                                           &decompressed_block_size)) {
                        /* Failed to decompress block */
                        cli_warnmsg("Failed to decompress BZIP2 compressed block\n");
                        goto done;
                    }
                    /* Decompressed block. Add it to the file data */
                    decompressed = cli_realloc(decompressed, (size_t)decompressed_size + decompressed_block_size);
                    if (NULL == decompressed) {
                        cli_errmsg("cli_egg_extract_file: Failed to allocate %llu bytes for decompressed file!\n",
                                   decompressed_size);
                        free(decompressed_block);
                        status = EGG_EMEM;
                        goto done;
                    }

                    memcpy(decompressed + decompressed_size, decompressed_block, decompressed_block_size);
                    decompressed_size += decompressed_block_size;

                    free(decompressed_block);

                    retval = EGG_OK;
                    break;
#else
                    cli_warnmsg("cli_egg_extract_file: BZIP2 decompression support not available.\n");
                    goto done;
#endif
                }
                case BLOCK_HEADER_COMPRESS_ALGORITHM_AZO: {
                    cli_warnmsg("cli_egg_extract_file: AZO decompression not yet supported.\n");
                    goto done;
                    //break;
                }
                case BLOCK_HEADER_COMPRESS_ALGORITHM_LZMA: {
                    cli_warnmsg("cli_egg_extract_file: LZMA decompression not yet supported.\n");
                    goto done;
                    // char* decompressed_block       = NULL;
                    // size_t decompressed_block_size = 0;

                    // if (EGG_OK != cli_egg_lzma_decompress(currBlock->compressedData,
                    //                                       currBlock->blockHeader->compress_size,
                    //                                       &decompressed_block,
                    //                                       &decompressed_block_size)) {
                    //     /* Failed to decompress block */
                    //     cli_warnmsg("Failed to decompress LZMA compressed block\n");
                    //     goto done;
                    // }
                    // /* Decompressed block. Add it to the file data */
                    // decompressed = cli_realloc(decompressed, (size_t)decompressed_size + decompressed_block_size);
                    // if (NULL == decompressed) {
                    //     cli_errmsg("cli_egg_extract_file: Failed to allocate %llu bytes for decompressed file!\n",
                    //                decompressed_size);
                    //     free(decompressed_block);
                    //     status = EGG_EMEM;
                    //     goto done;
                    // }

                    // memcpy(decompressed + decompressed_size, decompressed_block, decompressed_block_size);
                    // decompressed_size += decompressed_block_size;

                    // free(decompressed_block);

                    // retval = EGG_OK;
                    // break;
                }
                default: {
                    cli_errmsg("cli_egg_extract_file: unknown compression algorithm: %d!\n",
                               currBlock->blockHeader->compress_algorithm);
                    goto done;
                }
            }

            if (EGG_OK != retval) {
                cli_warnmsg("cli_egg_extract_file: Unable to decompress file: %s\n",
                            currFile->filename.name_utf8);
            }

            if ((i == currFile->nBlocks - 1) &&                       // last block ?
                (decompressed_size != currFile->file->file_length)) { // right amount of data ?
                cli_warnmsg("cli_egg_extract_file: alleged filesize (%llu) != actual filesize (%llu)!\n",
                            currFile->file->file_length,
                            decompressed_size);
            }
        }
    }

    cli_dbgmsg("cli_egg_extract_file: File extracted: %s\n", currFile->filename.name_utf8);
    *filename             = strdup(currFile->filename.name_utf8);
    *output_buffer        = decompressed;
    *output_buffer_length = decompressed_size;
    status                = EGG_OK;

done:
    handle->fileExtractionIndex += 1;

    if (EGG_OK != status) {
        /* Free buffer */
        if (NULL != decompressed) {
            free(decompressed);
        }
    }

    return status;
}

cl_egg_error_t cli_egg_skip_file(void* hArchive)
{
    cl_egg_error_t status = EGG_ERR;
    egg_handle* handle    = NULL;

    if (!hArchive) {
        cli_errmsg("cli_egg_skip_file: Invalid args!\n");
        return EGG_ERR;
    }

    handle = (egg_handle*)hArchive;
    if (EGG_OK != EGG_VALIDATE_HANDLE(handle)) {
        cli_errmsg("cli_egg_skip_file: Invalid handle values!\n");
        goto done;
    }

    if (handle->fileExtractionIndex >= handle->nFiles) {
        cli_warnmsg("cli_egg_skip_file: File index exceeds number of files in archive!\n");
        status = EGG_BREAK;
        goto done;
    }

    handle->fileExtractionIndex += 1;
    if (handle->fileExtractionIndex >= handle->nFiles) {
        status = EGG_BREAK;
    }

    cli_dbgmsg("cli_egg_skip_file: File skipped.\n");

    status = EGG_OK;
done:
    return status;
}

void cli_egg_close(void* hArchive)
{
    egg_handle* handle = NULL;

    if (!hArchive) {
        cli_errmsg("cli_egg_close: Invalid args.\n");
        return;
    }

    handle = (egg_handle*)hArchive;
    if (EGG_OK != EGG_VALIDATE_HANDLE(handle)) {
        cli_errmsg("cli_egg_close: Invalid handle values!\n");
        return;
    }

    egg_free_egg_handle(handle);

    return;
}