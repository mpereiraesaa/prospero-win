/* SPDX-License-Identifier: LGPL-2.1-or-later
 * A tree whose subdirectories are all open at once can be listed (patch
 * 0871). .NET's FileSystemEnumerator opens each subdirectory relative to
 * its parent's handle (NtCreateFile with RootDirectory) as soon as it finds
 * it, and keeps the handle queued until it lists the directory; this does the
 * same over 1500 directories, each with one below it and a file. On the
 * console every queued handle held a server descriptor in the title's own
 * descriptor table, until the server parked them. */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>

NTSTATUS NTAPI NtQueryDirectoryFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG,
                                    FILE_INFORMATION_CLASS, BOOLEAN, PUNICODE_STRING, BOOLEAN);

#define WIDE 1500
#define QUEUE (2 * WIDE + 2)

typedef struct {  /* FILE_DIRECTORY_INFORMATION */
    ULONG NextEntryOffset, FileIndex;
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, EndOfFile, AllocationSize;
    ULONG FileAttributes, FileNameLength;
    WCHAR FileName[1];
} DIR_INFO;

static HANDLE queue[QUEUE];
static BYTE buffer[64 * 1024];

static HANDLE open_relative(HANDLE root, const WCHAR *name, ULONG len, NTSTATUS *status)
{
    UNICODE_STRING us = { (USHORT)len, (USHORT)len, (WCHAR *)name };
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK io;
    HANDLE handle = NULL;

    memset(&attr, 0, sizeof(attr));
    attr.Length = sizeof(attr);
    attr.RootDirectory = root;
    attr.ObjectName = &us;
    attr.Attributes = OBJ_CASE_INSENSITIVE;
    *status = NtCreateFile(&handle, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attr, &io, NULL, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                           FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT,
                           NULL, 0);
    return *status ? NULL : handle;
}

static void tree(const WCHAR *top, BOOL create)
{
    WCHAR path[MAX_PATH];
    int i;

    for (i = 0; i < WIDE; i++)
    {
        swprintf(path, MAX_PATH, L"%ls\\d%d", top, i);
        if (create) CreateDirectoryW(path, NULL);
        swprintf(path, MAX_PATH, L"%ls\\d%d\\inner", top, i);
        if (create) CreateDirectoryW(path, NULL);
        swprintf(path, MAX_PATH, L"%ls\\d%d\\inner\\f.txt", top, i);
        if (create) CloseHandle(CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
        else
        {
            DeleteFileW(path);
            swprintf(path, MAX_PATH, L"%ls\\d%d\\inner", top, i);
            RemoveDirectoryW(path);
            swprintf(path, MAX_PATH, L"%ls\\d%d", top, i);
            RemoveDirectoryW(path);
        }
    }
    if (!create) RemoveDirectoryW(top);
}

int main(void)
{
    WCHAR top[MAX_PATH];
    int head = 0, tail = 0, listed = 0, files = 0, longest = 0, failed = 0;
    NTSTATUS status = 0;

    GetTempPathW(MAX_PATH, top);
    swprintf(top + lstrlenW(top), MAX_PATH - lstrlenW(top), L"pw-directory-handles-%lu", GetCurrentProcessId());
    CreateDirectoryW(top, NULL);
    tree(top, TRUE);

    queue[tail++] = CreateFileW(top, FILE_LIST_DIRECTORY | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (queue[0] == INVALID_HANDLE_VALUE) { printf("directory-handles verdict=fail open-root\n"); return 1; }

    while (head < tail && !failed)
    {
        HANDLE dir = queue[head++];
        BOOLEAN restart = TRUE;

        for (;;)
        {
            IO_STATUS_BLOCK io;
            DIR_INFO *info = (DIR_INFO *)buffer;

            status = NtQueryDirectoryFile(dir, NULL, NULL, NULL, &io, buffer, sizeof(buffer),
                                          1 /* FileDirectoryInformation */, FALSE, NULL, restart);
            restart = FALSE;
            if (status == 0x80000006 /* STATUS_NO_MORE_FILES */) { status = 0; break; }
            if (status) { failed = 1; break; }
            for (;;)
            {
                ULONG n = info->FileNameLength;
                BOOL dot = (n == 2 && info->FileName[0] == '.') ||
                           (n == 4 && info->FileName[0] == '.' && info->FileName[1] == '.');

                if (!dot && (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    HANDLE sub = open_relative(dir, info->FileName, n, &status);

                    if (!sub || tail == QUEUE) { failed = 1; break; }
                    queue[tail++] = sub;
                    if (tail - head > longest) longest = tail - head;
                }
                else if (!dot) files++;
                if (!info->NextEntryOffset) break;
                info = (DIR_INFO *)((BYTE *)info + info->NextEntryOffset);
            }
            if (failed) break;
        }
        CloseHandle(dir);
        listed++;
    }
    while (head < tail) CloseHandle(queue[head++]);
    tree(top, FALSE);

    if (failed || listed != 2 * WIDE + 1 || files != WIDE)
    {
        printf("directory-handles verdict=fail status=%08lx listed=%d files=%d longest=%d\n",
               (unsigned long)status, listed, files, longest);
        return 1;
    }
    printf("directory-handles verdict=pass listed=%d files=%d longest=%d\n", listed, files, longest);
    return 0;
}
