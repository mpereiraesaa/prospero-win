#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run patch 0160's name lookup (lookup_unix_name, find_file_in_dir and the
case-sensitivity probe, as patched for __PROSPERO__) against a real directory
tree through the PS5 working-directory wrappers, and patch 0890's
share_name_changes against a model of the server module. A name created,
deleted or renamed through the wrappers is seen by the next lookup; misses are
answered from a kept listing without the reparse-point retry; other spellings,
8.3 names, reparse points and dispositions behave as Wine's readdir loop did.

The patched functions are whole only in a patched file.c, so the lookups run
when the pinned Wine checkout is present (PROSPERO_WINE_SOURCE or
.deps/wine/source, as for test_wowprospero_contract.py; `make wine-check`
requires it). Without it, the parts whole in the patches still run:
match_dir_entry through pw_cwd_scan_directory, and share_name_changes."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCHES = ROOT / "wine/patches"
LOOKUP = PATCHES / "0160-ntdll-ps5-keep-listings-of-searched-directories.patch"
SHARE = PATCHES / "0890-ntdll-ps5-share-name-changes-with-the-server.patch"
CWD = ROOT / "wine/ps5"
WINE_COMMIT = "490f6d5dcbb2a5047345b8af88d114bbcaad69a8"
FILE_C = "dlls/ntdll/unix/file.c"


def new_side(text, path):
    """The new side of every hunk that changes path, in order."""
    diff = text.split("diff --git a/" + path + " ", 1)[1].split("\ndiff --git ", 1)[0]
    lines = []
    for hunk in diff.split("\n@@")[1:]:
        lines += [x[1:] for x in hunk.splitlines()[1:] if x.startswith((" ", "+")) or x == ""]
    return "\n".join(lines)


def function(source, signature):
    """The definition that starts with signature, through its closing brace."""
    match = re.search(r"^" + re.escape(signature), source, re.M)
    assert match, signature
    end = source.index("\n{", match.start()) + 2
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def wine_source():
    candidates = [Path(os.environ["PROSPERO_WINE_SOURCE"])] if os.environ.get("PROSPERO_WINE_SOURCE") else []
    candidates.append(ROOT / ".deps/wine/source")
    for candidate in candidates:
        if (candidate / FILE_C).is_file():
            return candidate
    return None


def patched_file_c(source):
    """The pinned file.c with every patch of the series that changes it."""
    pristine = subprocess.run(["git", "-C", str(source), "show", f"{WINE_COMMIT}:{FILE_C}"],
                              capture_output=True, text=True)
    with tempfile.TemporaryDirectory() as directory:
        target = Path(directory) / FILE_C
        target.parent.mkdir(parents=True)
        target.write_text(pristine.stdout if pristine.returncode == 0 else (source / FILE_C).read_text())
        for patch in sorted(PATCHES.glob("[0-9][0-9][0-9][0-9]-*.patch")):
            if f"diff --git a/{FILE_C} " in patch.read_text():
                subprocess.run(["git", "apply", f"--include={FILE_C}", str(patch)], cwd=directory, check=True)
        return target.read_text()


file_c = new_side(LOOKUP.read_text(), FILE_C)
start = file_c.index("#ifdef __PROSPERO__\n/* wine/ps5/pw_wine_cwd.h")
MATCH = file_c[start:file_c.index("#endif", file_c.index("static int match_dir_entry")) + len("#endif")]
SOURCE = wine_source()
if SOURCE:
    whole = patched_file_c(SOURCE)
    CODE = "\n\n".join([
        function(whole, "static BOOLEAN get_dir_case_sensitivity_stat("),
        MATCH,
        function(whole, "static NTSTATUS find_file_in_dir("),
        function(whole, "static NTSTATUS lookup_unix_name("),
    ])
    assert MATCH in whole
    assert "pw_cwd_scan_directory( root_fd, unix_name, match_dir_entry, &match, &flags )" in CODE
    assert "&& !no_reparse &&" in CODE
else:
    CODE = MATCH
server_c = new_side(SHARE.read_text(), "dlls/ntdll/unix/server.c")
SHARE_CODE = function(server_c, "static void share_name_changes(")
assert "share_name_changes( module );  /* before the server starts creating files */" in server_c
assert "pw_cwd_share_changes" in (ROOT / "tools/build_wine_ps5.sh").read_text().split("wineserver_desc.c")[1].split("\n    python3")[0]
# PW_CWD_NO_REPARSE_NAMES is copied into file.c: it must stay the header's.
assert "#define PW_CWD_NO_REPARSE_NAMES 1" in MATCH
assert "PW_CWD_NO_REPARSE_NAMES = 1" in (CWD / "pw_wine_cwd.h").read_text()

HARNESS = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pw_wine_cwd.h"

typedef unsigned short WCHAR;
typedef unsigned char BOOLEAN;
typedef int BOOL;
typedef unsigned int UINT, DWORD, NTSTATUS;
typedef struct { unsigned short Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { UNICODE_STRING *ObjectName; } OBJECT_ATTRIBUTES;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_REPARSE 0x00000104
#define STATUS_NO_SUCH_FILE 0xc000000f
#define STATUS_NO_MEMORY 0xc0000017
#define STATUS_ACCESS_DENIED 0xc0000022
#define STATUS_OBJECT_NAME_INVALID 0xc0000033
#define STATUS_OBJECT_NAME_NOT_FOUND 0xc0000034
#define STATUS_OBJECT_NAME_COLLISION 0xc0000035
#define STATUS_OBJECT_PATH_NOT_FOUND 0xc000003a
#define STATUS_UNSUCCESSFUL 0xc0000001
#define FILE_OPEN 1
#define FILE_CREATE 2
#define FILE_OPEN_IF 3
#define FILE_OVERWRITE 4
#define MAX_DIR_ENTRY_LEN 255
#define INVALID_NT_CHARS '*','?','<','>','|','"'

static NTSTATUS errno_to_status( int err )
{
    switch (err)
    {
    case ENOENT: return STATUS_OBJECT_NAME_NOT_FOUND;
    case ENOTDIR: return STATUS_OBJECT_PATH_NOT_FOUND;
    case EACCES: return STATUS_ACCESS_DENIED;
    default: return STATUS_UNSUCCESSFUL;
    }
}
/* the unix code page, reduced to ASCII */
static int ntdll_wcstoumbs( const WCHAR *src, DWORD srclen, char *dst, DWORD dstlen, BOOL strict )
{
    DWORD i;
    (void)strict;
    if (srclen > dstlen) return -1;
    for (i = 0; i < srclen; i++) { if (src[i] >= 0x80) return -1; dst[i] = (char)src[i]; }
    return (int)srclen;
}
static int ntdll_umbstowcs( const char *src, DWORD srclen, WCHAR *dst, DWORD dstlen )
{
    DWORD i;
    for (i = 0; i < srclen && i < dstlen; i++) dst[i] = (unsigned char)src[i];
    return (int)i;
}
static WCHAR fold( WCHAR c ) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int wcsnicmp_w( const WCHAR *a, const WCHAR *b, int n )
{
    for (; n > 0; n--, a++, b++) if (fold( *a ) != fold( *b )) return fold( *a ) - fold( *b );
    return 0;
}
static WCHAR *wcschr_w( const WCHAR *s, WCHAR c )
{
    for (; *s; s++) if (*s == c) return (WCHAR *)s;
    return NULL;
}
#define wcsnicmp wcsnicmp_w
#define wcschr wcschr_w
/* 8.3 names: at most 8 characters, a dot and 3, no space; the short form of
 * a longer name is its first four letters, '~' and a 3-letter hash */
static BOOLEAN is_legal_8dot3_name( const WCHAR *name, int len )
{
    int i, dot = -1;
    if (len > 12) return FALSE;
    for (i = 0; i < len; i++)
    {
        if (name[i] == ' ') return FALSE;
        if (name[i] == '.') { if (dot != -1) return FALSE; dot = i; }
    }
    return dot == -1 ? len <= 8 : dot <= 8 && len - dot - 1 <= 3;
}
static int hash_short_file_name( const WCHAR *name, int length, WCHAR *buffer )
{
    unsigned hash = 0;
    int i, n = 0;
    for (i = 0; i < length; i++) hash = hash * 31 + fold( name[i] );
    for (i = 0; i < length && n < 4; i++) if (name[i] != '.' && name[i] != ' ') buffer[n++] = fold( name[i] );
    buffer[n++] = '~';
    for (i = 0; i < 3; i++, hash /= 26) buffer[n++] = 'A' + hash % 26;
    return n;
}
#ifdef FULL
static BOOLEAN get_dir_case_sensitivity_stat( int root_fd, const char *dir );
static BOOLEAN get_dir_case_sensitivity( int root_fd, const char *dir ) { return get_dir_case_sensitivity_stat( root_fd, dir ); }
#endif

/* what Wine's own code asks of the kernel (through the wrappers) */
static int wine_stats, wine_opens, reparse_calls;
static int counting_fstatat( int fd, const char *path, struct stat *st, int flags )
{ __atomic_add_fetch( &wine_stats, 1, __ATOMIC_RELAXED ); return fstatat( fd, path, st, flags ); }
static int counting_openat( int fd, const char *path, int flags )
{ __atomic_add_fetch( &wine_opens, 1, __ATOMIC_RELAXED ); return openat( fd, path, flags ); }
#define fstatat counting_fstatat
#define openat counting_openat
static NTSTATUS resolve_reparse_point( int fd, int root_fd, OBJECT_ATTRIBUTES *attr, UNICODE_STRING *nt_name,
        unsigned int nt_pos, unsigned int reparse_len, char **unix_name, int unix_len, int pos, UINT disposition,
        BOOL open_reparse, BOOL is_unix, unsigned int reparse_count )
{
    (void)fd; (void)root_fd; (void)attr; (void)nt_name; (void)nt_pos; (void)reparse_len; (void)unix_name;
    (void)unix_len; (void)pos; (void)disposition; (void)open_reparse; (void)is_unix; (void)reparse_count;
    reparse_calls++;
    return STATUS_REPARSE;
}

/*CODE*/
#undef fstatat
#undef openat

/* patch 0890 against a server module that may or may not share */
static int server_shares, shared_with_server;
static unsigned long *server_counter;
static void server_share( unsigned long *counter ) { server_counter = counter; shared_with_server++; }
static void *test_dlsym( void *module, const char *name )
{
    assert( module == (void *)0x5e5 );
    return server_shares && !strcmp( name, "pw_cwd_share_changes" ) ? (void *)server_share : NULL;
}
#define dlsym test_dlsym
/*SHARE*/
#undef dlsym

static char root[64], drive[512];

#ifdef FULL
/* \??\C:\<path> as nt_to_unix_file_name_no_root hands it on: the unix name
 * of the drive, and the position after "C:\" */
static NTSTATUS lookup( const char *path, UINT disposition, BOOL open_reparse, char *out )
{
    WCHAR wide[512];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr = { &name };
    char *unix_name;
    int i, length = (int)strlen( path ) + 7, unix_len = length * 3 + MAX_DIR_ENTRY_LEN + 3 + (int)strlen( drive );
    NTSTATUS status;

    for (i = 0; i < 7; i++) wide[i] = "\\??\\C:\\"[i];
    for (i = 0; path[i]; i++) wide[7 + i] = (unsigned char)path[i];
    name.Buffer = wide;
    name.Length = name.MaximumLength = (unsigned short)(length * sizeof(WCHAR));
    unix_name = malloc( unix_len );
    strcpy( unix_name, drive );
    status = lookup_unix_name( AT_FDCWD, &attr, &name, 7, &unix_name, unix_len, (int)strlen( drive ),
                               disposition, open_reparse, FALSE, 0 );
    if (out) strcpy( out, status == STATUS_SUCCESS || status == STATUS_NO_SUCH_FILE ? unix_name + strlen( drive ) : "" );
    free( unix_name );
    return status;
}
#endif

static const char *at( const char *name )
{
    static __thread char paths[2][1024];
    static __thread int next;
    char *path = paths[next++ & 1];
    snprintf( path, 1024, "%s/prefix/drive_c/%s", root, name );
    return path;
}
static void touch( const char *name )
{
    int fd = open( at( name ), O_CREAT | O_WRONLY, 0644 );
    assert( fd >= 0 && !close( fd ) );
}

static struct pw_cwd_listing_stats mark;
static void since( unsigned long reads, unsigned long hits, int line )
{
    struct pw_cwd_listing_stats now;
    pw_cwd_listing_stats( &now );
    if (now.reads - mark.reads != reads || now.hits - mark.hits != hits)
    {
        fprintf( stderr, "line %d: reads %lu hits %lu, want %lu %lu\n", line, now.reads - mark.reads,
                 now.hits - mark.hits, reads, hits );
        abort();
    }
    mark = now;
}
#define SINCE(r, h) since( r, h, __LINE__ )

#ifdef FULL
static void test_lookups(void)
{
    char found[1024];
    int stats, opens;

    /* the exact name: one stat of the whole path, no directory read */
    pw_cwd_listing_stats( &mark );
    stats = wine_stats;
    assert( lookup( "Games\\GTAIV\\GTAIV.exe", FILE_OPEN, FALSE, found ) == STATUS_SUCCESS );
    assert( !strcmp( found, "/Games/GTAIV/GTAIV.exe" ) && wine_stats == stats + 1 );
    SINCE( 0, 0 );
    /* another spelling: the walk finds each directory's real name */
    assert( lookup( "games\\gtaiv\\gtaiv.EXE", FILE_OPEN, FALSE, found ) == STATUS_SUCCESS );
    assert( !strcmp( found, "/Games/GTAIV/GTAIV.exe" ) );
    SINCE( 3, 0 );
    assert( lookup( "GAMES\\GtaIv\\GTAIV.EXE", FILE_OPEN, FALSE, found ) == STATUS_SUCCESS );
    assert( !strcmp( found, "/Games/GTAIV/GTAIV.exe" ) );
    SINCE( 0, 3 );

    /* The loader's probe: a miss in update. The first reads the directory
     * once; the reparse retry ("name?") is skipped, so the later ones cost a
     * stat of the path, of each directory on the way, of the name and of the
     * directory, with no open. */
    assert( lookup( "Games\\GTAIV\\update\\socialclub_LOG.txt", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    SINCE( 1, 0 );
    stats = wine_stats;
    opens = wine_opens;
    assert( lookup( "Games\\GTAIV\\update\\socialclub_LOG.txt", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    assert( wine_stats == stats + 5 && wine_opens == opens && !reparse_calls );
    SINCE( 0, 1 );
    assert( lookup( "Games\\GTAIV\\update\\other.dat", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    SINCE( 0, 1 );
    /* a name that would be created */
    assert( lookup( "Games\\GTAIV\\update\\new.txt", FILE_OPEN_IF, FALSE, found ) == STATUS_NO_SUCH_FILE );
    assert( !strcmp( found, "/Games/GTAIV/update/new.txt" ) );
    SINCE( 0, 1 );
    /* a missing directory on the way */
    assert( lookup( "Games\\Nope\\x.txt", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_PATH_NOT_FOUND );

    /* created after the miss (the server's open through its wrappers) */
    touch( "Games/GTAIV/update/SocialClub_Log.txt" );
    assert( lookup( "Games\\GTAIV\\update\\socialclub_LOG.txt", FILE_OPEN, FALSE, found ) == STATUS_SUCCESS );
    assert( !strcmp( found, "/Games/GTAIV/update/SocialClub_Log.txt" ) );
    assert( lookup( "Games\\GTAIV\\update\\SOCIALCLUB_LOG.TXT", FILE_CREATE, FALSE, NULL ) == STATUS_OBJECT_NAME_COLLISION );
    /* deleted after the hit */
    assert( !unlink( at( "Games/GTAIV/update/SocialClub_Log.txt" ) ) );
    assert( lookup( "Games\\GTAIV\\update\\socialclub_LOG.txt", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    assert( lookup( "Games\\GTAIV\\update\\socialclub_LOG.txt", FILE_CREATE, FALSE, NULL ) == STATUS_NO_SUCH_FILE );
    /* renamed */
    assert( !rename( at( "Games/GTAIV/update/Common.rpf" ), at( "Games/GTAIV/update/Moved.RPF" ) ) );
    assert( lookup( "Games\\GTAIV\\update\\common.rpf", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    assert( lookup( "Games\\GTAIV\\update\\moved.rpf", FILE_OPEN, FALSE, found ) == STATUS_SUCCESS );
    assert( !strcmp( found, "/Games/GTAIV/update/Moved.RPF" ) );
    assert( !rename( at( "Games/GTAIV/update/Moved.RPF" ), at( "Games/GTAIV/update/Common.rpf" ) ) );
    /* a directory made after a miss below it */
    assert( lookup( "Games\\GTAIV\\update\\pc\\x.dat", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_PATH_NOT_FOUND );
    assert( !mkdir( at( "Games/GTAIV/update/PC" ), 0777 ) );
    touch( "Games/GTAIV/update/PC/X.dat" );
    assert( lookup( "Games\\GTAIV\\update\\pc\\x.dat", FILE_OPEN, FALSE, found ) == STATUS_SUCCESS );
    assert( !strcmp( found, "/Games/GTAIV/update/PC/X.dat" ) );
    assert( !unlink( at( "Games/GTAIV/update/PC/X.dat" ) ) && !rmdir( at( "Games/GTAIV/update/PC" ) ) );
    assert( lookup( "Games\\GTAIV\\update\\pc\\x.dat", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_PATH_NOT_FOUND );

    /* 8.3 names: a short name matches the long one it stands for */
    {
        WCHAR longW[32], shortW[12];
        char short_name[16];
        const char *long_name = "socialclub_settings.xml";
        int i, n;

        touch( "Games/GTAIV/update/socialclub_settings.xml" );
        for (i = 0; long_name[i]; i++) longW[i] = (unsigned char)long_name[i];
        n = hash_short_file_name( longW, i, shortW );
        for (i = 0; i < n; i++) short_name[i] = (char)shortW[i];
        short_name[n] = 0;
        snprintf( found, sizeof(found), "Games\\GTAIV\\update\\%s", short_name );
        assert( lookup( found, FILE_OPEN, FALSE, found ) == STATUS_SUCCESS );
        assert( !strcmp( found, "/Games/GTAIV/update/socialclub_settings.xml" ) );
        assert( !unlink( at( "Games/GTAIV/update/socialclub_settings.xml" ) ) );
    }

    /* Reparse points: a file "name?" is found for name, and opened itself
     * when the caller asks for the reparse point */
    touch( "Games/GTAIV/update/Junction?" );
    reparse_calls = 0;
    assert( lookup( "Games\\GTAIV\\update\\junction", FILE_OPEN, FALSE, NULL ) == STATUS_REPARSE && reparse_calls == 1 );
    assert( lookup( "Games\\GTAIV\\update\\junction\\below.txt", FILE_OPEN, FALSE, NULL ) == STATUS_REPARSE && reparse_calls == 2 );
    assert( lookup( "Games\\GTAIV\\update\\junction", FILE_OPEN, TRUE, found ) == STATUS_SUCCESS );
    assert( !strcmp( found, "/Games/GTAIV/update/Junction?" ) );
    /* with one in the directory, a miss still tries the retry */
    stats = wine_stats;
    assert( lookup( "Games\\GTAIV\\update\\absent", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    assert( wine_stats == stats + 6 && reparse_calls == 2 );
    assert( !unlink( at( "Games/GTAIV/update/Junction?" ) ) );
    assert( lookup( "Games\\GTAIV\\update\\junction", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    /* a virtual link (the PS5's symlinks) named like one counts too, and
     * links are found by their exact name as before */
    assert( !symlink( "Common.rpf", at( "Games/GTAIV/update/alias?" ) ) );
    assert( lookup( "Games\\GTAIV\\update\\alias", FILE_OPEN, FALSE, NULL ) == STATUS_REPARSE && reparse_calls == 3 );
    assert( !unlink( at( "Games/GTAIV/update/alias?" ) ) );
    assert( !symlink( "/dev/null", at( "Games/GTAIV/update/socialclub_LOG.txt" ) ) );
    assert( lookup( "Games\\GTAIV\\update\\socialclub_LOG.txt", FILE_OPEN, FALSE, NULL ) == STATUS_SUCCESS );
    assert( !unlink( at( "Games/GTAIV/update/socialclub_LOG.txt" ) ) );
    assert( lookup( "Games\\GTAIV\\update\\socialclub_LOG.txt", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
}

/* patch 0890: listings only once the server shares its count */
static void test_sharing(void)
{
    char *env = getenv( "WINE_PS5_DIR_LISTINGS" ) ? strdup( getenv( "WINE_PS5_DIR_LISTINGS" ) ) : NULL;

    server_shares = 0;
    share_name_changes( (void *)0x5e5 );
    pw_cwd_listing_stats( &mark );
    assert( !shared_with_server );
    assert( lookup( "Games\\GTAIV\\update\\absent", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    assert( lookup( "Games\\GTAIV\\update\\absent", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    SINCE( 2, 0 );

    setenv( "WINE_PS5_DIR_LISTINGS", "0", 1 );
    server_shares = 1;
    share_name_changes( (void *)0x5e5 );
    assert( shared_with_server == 1 && server_counter == pw_cwd_changes_counter() );
    assert( lookup( "Games\\GTAIV\\update\\absent", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    assert( lookup( "Games\\GTAIV\\update\\absent", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    SINCE( 2, 0 );

    unsetenv( "WINE_PS5_DIR_LISTINGS" );
    share_name_changes( (void *)0x5e5 );
    assert( shared_with_server == 2 );
    assert( lookup( "Games\\GTAIV\\update\\absent", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    assert( lookup( "Games\\GTAIV\\update\\absent", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    SINCE( 1, 1 );
    /* the server counts a change it made */
    __atomic_add_fetch( server_counter, 1, __ATOMIC_ACQ_REL );
    assert( lookup( "Games\\GTAIV\\update\\absent", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    SINCE( 1, 0 );
    if (env) { setenv( "WINE_PS5_DIR_LISTINGS", env, 1 ); free( env ); }
}

/* Lookups in several threads while the server creates, then deletes: a
 * name created before a lookup starts is found under any spelling, and one
 * deleted before is not. */
enum { FILES = 200, THREADS = 4 };
static int created, deleting, deleted;

static void *looker( void *arg )
{
    unsigned int seed = (unsigned int)(size_t)arg;
    char path[128];
    int done = 0;

    while (!done)
    {
        int c = __atomic_load_n( &created, __ATOMIC_SEQ_CST ), d = __atomic_load_n( &deleted, __ATOMIC_SEQ_CST );
        NTSTATUS newest, older;

        done = d == FILES;
        if (c && !d)
        {
            snprintf( path, sizeof(path), "Games\\GTAIV\\update\\MOD%d.ASI", c - 1 );
            newest = lookup( path, FILE_OPEN, FALSE, NULL );
            snprintf( path, sizeof(path), "Games\\GTAIV\\update\\Mod%d.Asi", rand_r( &seed ) % c );
            older = lookup( path, FILE_OPEN, FALSE, NULL );
            if (!__atomic_load_n( &deleting, __ATOMIC_SEQ_CST ))
                assert( newest == STATUS_SUCCESS && older == STATUS_SUCCESS );
        }
        if (d)
        {
            snprintf( path, sizeof(path), "Games\\GTAIV\\update\\MOD%d.asi", d - 1 );
            assert( lookup( path, FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
        }
        assert( lookup( "Games\\GTAIV\\update\\socialclub_LOG.txt", FILE_OPEN, FALSE, NULL ) == STATUS_OBJECT_NAME_NOT_FOUND );
    }
    return NULL;
}

static void test_threads(void)
{
    pthread_t threads[THREADS];
    char name[64];
    int i;

    for (i = 0; i < THREADS; i++) assert( !pthread_create( &threads[i], NULL, looker, (void *)(size_t)(i + 1) ) );
    for (i = 0; i < FILES; i++)
    {
        snprintf( name, sizeof(name), "Games/GTAIV/update/mod%d.asi", i );
        touch( name );
        __atomic_store_n( &created, i + 1, __ATOMIC_SEQ_CST );
    }
    __atomic_store_n( &deleting, 1, __ATOMIC_SEQ_CST );
    for (i = 0; i < FILES; i++)
    {
        snprintf( name, sizeof(name), "Games/GTAIV/update/mod%d.asi", i );
        assert( !unlink( at( name ) ) );
        __atomic_store_n( &deleted, i + 1, __ATOMIC_SEQ_CST );
    }
    for (i = 0; i < THREADS; i++) assert( !pthread_join( threads[i], NULL ) );
}
#else
/* Without the pinned file.c: match_dir_entry as find_file_in_dir uses it. */
static int find( const char *dir, const char *want, char *found, unsigned int *flags )
{
    WCHAR wide[64];
    struct dir_match match;
    int i, ret;

    for (i = 0; want[i]; i++) wide[i] = (unsigned char)want[i];
    match.name = wide;
    match.length = i;
    match.is_name_8_dot_3 = is_legal_8dot3_name( wide, i ) && i >= 8 && wide[4] == '~';
    ret = pw_cwd_scan_directory( AT_FDCWD, at( dir ), match_dir_entry, &match, flags );
    if (ret > 0) strcpy( found, match.found );
    return ret;
}

static void test_matching(void)
{
    WCHAR longW[32], shortW[12];
    char found[256], short_name[16];
    const char *long_name = "socialclub_settings.xml";
    unsigned int flags;
    int i, n;

    assert( find( "Games/GTAIV", "gtaiv.EXE", found, &flags ) == 1 && !strcmp( found, "GTAIV.exe" ) );
    assert( find( "Games/GTAIV/update", "socialclub_LOG.txt", found, &flags ) == 0 && flags == PW_CWD_NO_REPARSE_NAMES );
    pw_cwd_listing_stats( &mark );
    assert( find( "Games/GTAIV/update", "COMMON.RPF", found, &flags ) == 1 && !strcmp( found, "Common.rpf" ) );
    SINCE( 0, 1 );
    touch( "Games/GTAIV/update/socialclub_settings.xml" );
    for (i = 0; long_name[i]; i++) longW[i] = (unsigned char)long_name[i];
    n = hash_short_file_name( longW, i, shortW );
    for (i = 0; i < n; i++) short_name[i] = (char)shortW[i];
    short_name[n] = 0;
    assert( find( "Games/GTAIV/update", short_name, found, &flags ) == 1 && !strcmp( found, long_name ) );
    assert( !unlink( at( "Games/GTAIV/update/socialclub_settings.xml" ) ) );
    assert( find( "Games/GTAIV/update", short_name, found, &flags ) == 0 );
    touch( "Games/GTAIV/update/point?" );
    assert( find( "Games/GTAIV/update", "absent", found, &flags ) == 0 && !flags );
    assert( find( "Games/GTAIV/update", "POINT?", found, &flags ) == 1 && !strcmp( found, "point?" ) );
    assert( !unlink( at( "Games/GTAIV/update/point?" ) ) );
}

static void test_sharing(void)
{
    char found[256];
    unsigned int flags;
    char *env = getenv( "WINE_PS5_DIR_LISTINGS" ) ? strdup( getenv( "WINE_PS5_DIR_LISTINGS" ) ) : NULL;

    server_shares = 0;
    share_name_changes( (void *)0x5e5 );
    pw_cwd_listing_stats( &mark );
    assert( !shared_with_server );
    assert( find( "Games/GTAIV/update", "absent", found, &flags ) == 0 && find( "Games/GTAIV/update", "absent", found, &flags ) == 0 );
    SINCE( 2, 0 );
    setenv( "WINE_PS5_DIR_LISTINGS", "0", 1 );
    server_shares = 1;
    share_name_changes( (void *)0x5e5 );
    assert( shared_with_server == 1 && server_counter == pw_cwd_changes_counter() );
    assert( find( "Games/GTAIV/update", "absent", found, &flags ) == 0 && find( "Games/GTAIV/update", "absent", found, &flags ) == 0 );
    SINCE( 2, 0 );
    unsetenv( "WINE_PS5_DIR_LISTINGS" );
    share_name_changes( (void *)0x5e5 );
    assert( find( "Games/GTAIV/update", "absent", found, &flags ) == 0 && find( "Games/GTAIV/update", "absent", found, &flags ) == 0 );
    SINCE( 1, 1 );
    __atomic_add_fetch( server_counter, 1, __ATOMIC_ACQ_REL );
    assert( find( "Games/GTAIV/update", "absent", found, &flags ) == 0 );
    SINCE( 1, 0 );
    if (env) { setenv( "WINE_PS5_DIR_LISTINGS", env, 1 ); free( env ); }
}
#endif

int main(void)
{
    char prefix[256];

    snprintf( root, sizeof(root), "/tmp/pw-lookup-XXXXXX" );
    assert( mkdtemp( root ) );
    snprintf( prefix, sizeof(prefix), "%s/prefix", root );
    assert( !mkdir( prefix, 0777 ) && !chdir( prefix ) );
    assert( !mkdir( "dosdevices", 0777 ) && !mkdir( "drive_c", 0777 ) );
    assert( !symlink( "../drive_c", "dosdevices/c:" ) );
    snprintf( drive, sizeof(drive), "%s/dosdevices/c:", prefix );
    assert( !mkdir( at( "Games" ), 0777 ) && !mkdir( at( "Games/GTAIV" ), 0777 ) );
    assert( !mkdir( at( "Games/GTAIV/update" ), 0777 ) );
    touch( "Games/GTAIV/GTAIV.exe" );
    touch( "Games/GTAIV/update/Common.rpf" );
    pw_cwd_listings_enable( 1 );

#ifdef FULL
    test_lookups();
    test_threads();
#else
    test_matching();
#endif
    test_sharing();

    assert( !unlink( at( "Games/GTAIV/update/Common.rpf" ) ) && !rmdir( at( "Games/GTAIV/update" ) ) );
    assert( !unlink( at( "Games/GTAIV/GTAIV.exe" ) ) && !rmdir( at( "Games/GTAIV" ) ) && !rmdir( at( "Games" ) ) );
    assert( !unlink( "dosdevices/c:" ) && !rmdir( "dosdevices" ) && !rmdir( "drive_c" ) );
    assert( !chdir( "/" ) && !rmdir( prefix ) && !rmdir( root ) );
    return 0;
}
'''


def main():
    wraps = sorted(set(re.findall(r"__wrap_([a-z_]+)", (CWD / "pw_wine_cwd_libc.c").read_text())))
    source = HARNESS.replace("/*CODE*/", CODE).replace("/*SHARE*/", SHARE_CODE)
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        (folder / "harness.c").write_text(source)
        exe = folder / "harness"
        command = shlex.split(os.environ.get("CC", "cc")) + shlex.split(os.environ.get("CFLAGS", "-O2"))
        command += ["-std=gnu11", "-pthread", "-D__PROSPERO__"] + (["-DFULL"] if SOURCE else []) + ["-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0",
                    "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-sign-compare", f"-I{CWD}"]
        command += [f"-Wl,--wrap={name}" for name in wraps]
        subprocess.run(command + [str(folder / "harness.c"), str(CWD / "pw_wine_cwd.c"),
                                  str(CWD / "pw_wine_cwd_libc.c"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    if SOURCE:
        print("wine lookup misses passed: exact and other spellings, misses from kept listings without the "
              "reparse retry, created/deleted/renamed/mkdir after, dispositions, 8.3 names, reparse points "
              "and links, server sharing on/off/env, concurrent lookups")
    else:
        print("wine lookup misses passed: entry matching and server sharing; skipped the lookups "
              "(no pinned Wine checkout)")


if __name__ == "__main__":
    main()
