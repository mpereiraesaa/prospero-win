/* SPDX-License-Identifier: LGPL-2.1-or-later
 * A watched directory reports its changes: FindFirstChangeNotification
 * fires when a file is created, and ReadDirectoryChangesW names a renamed
 * file's old and new names (patch 0884: the console has no inotify). */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void path_in(WCHAR *out, const WCHAR *dir, const WCHAR *name)
{
    lstrcpyW(out, dir);
    lstrcatW(out, L"\\");
    lstrcatW(out, name);
}

int main(void)
{
    WCHAR dir[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    DWORD buffer[1024], bytes = 0, created, renamed = 0;
    OVERLAPPED overlapped = { 0 };
    HANDLE change, handle, file;
    int old_name = 0, new_name = 0, ok;

    GetTempPathW(MAX_PATH, dir);
    lstrcatW(dir, L"pw-dirwatch");
    path_in(a, dir, L"a.txt");
    path_in(b, dir, L"b.txt");
    DeleteFileW(a);
    DeleteFileW(b);
    CreateDirectoryW(dir, NULL);

    change = FindFirstChangeNotificationW(dir, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME);
    file = CreateFileW(a, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CloseHandle(file);
    created = change != INVALID_HANDLE_VALUE ? WaitForSingleObject(change, 5000) : WAIT_FAILED;
    if (change != INVALID_HANDLE_VALUE) FindCloseChangeNotification(change);

    handle = CreateFileW(dir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (handle != INVALID_HANDLE_VALUE &&
        ReadDirectoryChangesW(handle, buffer, sizeof(buffer), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, NULL, &overlapped,
                              NULL))
    {
        MoveFileW(a, b);
        renamed = WaitForSingleObject(overlapped.hEvent, 5000);
        if (renamed == WAIT_OBJECT_0 && GetOverlappedResult(handle, &overlapped, &bytes, FALSE) && bytes)
        {
            FILE_NOTIFY_INFORMATION *info = (FILE_NOTIFY_INFORMATION *)buffer;
            for (;;)
            {
                int is_a = info->FileNameLength == 10 && !memcmp(info->FileName, L"a.txt", 10);
                int is_b = info->FileNameLength == 10 && !memcmp(info->FileName, L"b.txt", 10);
                old_name |= is_a && (info->Action == FILE_ACTION_RENAMED_OLD_NAME || info->Action == FILE_ACTION_REMOVED);
                new_name |= is_b && (info->Action == FILE_ACTION_RENAMED_NEW_NAME || info->Action == FILE_ACTION_ADDED);
                if (!info->NextEntryOffset) break;
                info = (FILE_NOTIFY_INFORMATION *)((char *)info + info->NextEntryOffset);
            }
        }
        else CancelIo(handle);
    }
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    DeleteFileW(a);
    DeleteFileW(b);
    RemoveDirectoryW(dir);

    ok = created == WAIT_OBJECT_0 && renamed == WAIT_OBJECT_0 && old_name && new_name;
    printf("directory-changes created=%#lx renamed=%#lx bytes=%lu old=%d new=%d\n", created, renamed, bytes,
           old_name, new_name);
    printf("directory-changes verdict=%s\n", ok ? "pass" : "fail");
    fflush(stdout);
    return ok ? 0 : 1;
}
