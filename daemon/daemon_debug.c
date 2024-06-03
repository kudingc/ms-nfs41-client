/* NFSv4.1 client for Windows
 * Copyright � 2012 The Regents of the University of Michigan
 *
 * Olga Kornievskaia <aglo@umich.edu>
 * Casey Bodley <cbodley@umich.edu>
 * Roland Mainz <roland.mainz@nrubsig.org>
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * without any warranty; without even the implied warranty of merchantability
 * or fitness for a particular purpose.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 */

#include <windows.h>
#include <stdio.h>
#include <sddl.h>
#include <lmcons.h>

#include "daemon_debug.h"
#include "from_kernel.h"
#include "nfs41_driver.h"
#include "nfs41_ops.h"
#include "service.h"
#include "rpc/rpc.h"
#include "rpc/auth_sspi.h"

extern int g_debug_level = DEFAULT_DEBUG_LEVEL;

void set_debug_level(int level) { g_debug_level = level; }

static FILE *dlog_file;
static FILE *elog_file;

#ifndef STANDALONE_NFSD
void open_log_files()
{
    const char dfile[] = "nfsddbg.log";
    const char efile[] = "nfsderr.log";
    const char mode[] = "w";
    if (g_debug_level > 0) {
        dlog_file = fopen(dfile, mode);
        if (dlog_file == NULL) {
            ReportStatusToSCMgr(SERVICE_STOPPED, GetLastError(), 0);
            exit (GetLastError());
        }
    }
    elog_file = fopen(efile, mode);
    if (elog_file == NULL) {
        ReportStatusToSCMgr(SERVICE_STOPPED, GetLastError(), 0);
        exit (GetLastError());
    }
}

void close_log_files()
{
    if (dlog_file) fclose(dlog_file);
    if (elog_file) fclose(elog_file);
}
#else
void open_log_files()
{
    dlog_file = stdout;
    elog_file = stderr;
}
#endif

#define DPRINTF_PRINT_IMPERSONATION_USER 1

void dprintf_out(LPCSTR format, ...)
{
    va_list args;
    va_start(args, format);
#ifdef DPRINTF_PRINT_IMPERSONATION_USER
    char username[UNLEN+1];
    char groupname[GNLEN+1];
    HANDLE tok;
    const char *tok_src;
    bool free_tok = false;
    /* |in_dprintf_out| - protect against recursive calls */
    __declspec(thread) static bool in_dprintf_out = false;

    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, FALSE, &tok)) {
        tok_src = "impersonated_user";
        free_tok = true;
    }
    else {
        int lasterr = GetLastError();
        if (lasterr == ERROR_CANT_OPEN_ANONYMOUS) {
            tok_src = "anon_user";
        }
        else {
            tok_src = "proc_user";
        }

        tok = GetCurrentProcessToken();
    }

    if (in_dprintf_out) {
        (void)strcpy(username, "<unknown-recursive>");
        (void)strcpy(groupname, "<unknown-recursive>");
    }
    else {
        in_dprintf_out = true;

        if (!get_token_user_name(tok, username)) {
            (void)strcpy(username, "<unknown>");
        }
        if (!get_token_primarygroup_name(tok, groupname)) {
            (void)strcpy(groupname, "<unknown>");
        }

        in_dprintf_out = false;
    }

    (void)fprintf(dlog_file, "%04x/%s='%s'/%s' ",
        (int)GetCurrentThreadId(),
        tok_src, username, groupname);

    if (free_tok) {
        (void)CloseHandle(tok);
    }
#else
    (void)fprintf(dlog_file, "%04x: ", (int)GetCurrentThreadId());
#endif /* DPRINTF_PRINT_IMPERSONATION_USER */
    (void)vfprintf(dlog_file, format, args);
    (void)fflush(dlog_file);
    va_end(args);
}

/* log events (mount, umount, auth, ...) */
void logprintf(LPCSTR format, ...)
{
    SYSTEMTIME stime;
    char username[UNLEN+1];
    char groupname[GNLEN+1];
    HANDLE tok;
    const char *tok_src;
    bool free_tok = false;

    GetLocalTime(&stime);

    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, FALSE, &tok)) {
        tok_src = "impersonated_user";
        free_tok = true;
    }
    else {
        int lasterr = GetLastError();
        if (lasterr == ERROR_CANT_OPEN_ANONYMOUS) {
            tok_src = "anon_user";
        }
        else {
            tok_src = "proc_user";
        }

        tok = GetCurrentProcessToken();
    }

    if (!get_token_user_name(tok, username)) {
        (void)strcpy(username, "<unknown>");
    }
    if (!get_token_primarygroup_name(tok, groupname)) {
        (void)strcpy(groupname, "<unknown>");
    }

    va_list args;
    va_start(args, format);
    (void)fprintf(dlog_file,
        "# LOG: ts=%04d-%02d-%02d_%02d:%02d:%02d:%04d"
        " thr=%04x %s='%s'/'%s' msg=",
        (int)stime.wYear, (int)stime.wMonth, (int)stime.wDay,
        (int)stime.wHour, (int)stime.wMinute, (int)stime.wSecond,
        (int)stime.wMilliseconds,
        (int)GetCurrentThreadId(),
        tok_src,
        username, groupname);
    (void)vfprintf(dlog_file, format, args);
    (void)fflush(dlog_file);
    va_end(args);

    if (free_tok) {
        (void)CloseHandle(tok);
    }
}

void eprintf_out(LPCSTR format, ...)
{
    va_list args;
    va_start(args, format);
    (void)vfprintf(elog_file, format, args);
    (void)fflush(elog_file);
    va_end(args);
}

void eprintf(LPCSTR format, ...)
{
    va_list args;
    va_start(args, format);
    (void)fprintf(elog_file, "%04x: ", (int)GetCurrentThreadId());
    (void)vfprintf(elog_file, format, args);
    (void)fflush(elog_file);
    va_end(args);
}

void print_hexbuf(const char *title, const unsigned char *buf, int len)
{
    int j, k;
    fprintf(dlog_file, "%s", title);
    for(j = 0, k = 0; j < len; j++, k++) {
        fprintf(dlog_file, "%02x '%c' ", buf[j], isascii(buf[j])? buf[j]:' ');
        if (((k+1) % 10 == 0 && k > 0)) {
            fprintf(dlog_file, "\n");
        }
    }
    fprintf(dlog_file, "\n");
}

void print_hexbuf_no_asci(const char *title, const unsigned char *buf, int len)
{
    int j, k;
    fprintf(dlog_file, "%s", title);
    for(j = 0, k = 0; j < len; j++, k++) {
        fprintf(dlog_file, "%02x ", buf[j]);
        if (((k+1) % 10 == 0 && k > 0)) {
            fprintf(dlog_file, "\n");
        }
    }
    fprintf(dlog_file, "\n");
}

void print_create_attributes(int level, DWORD create_opts) {
    if (level > g_debug_level) return;
    fprintf(dlog_file, "create attributes: ");
    if (create_opts & FILE_DIRECTORY_FILE)
        fprintf(dlog_file, "DIRECTORY_FILE ");
    if (create_opts & FILE_NON_DIRECTORY_FILE)
        fprintf(dlog_file, "NON_DIRECTORY_FILE ");
    if (create_opts & FILE_WRITE_THROUGH)
        fprintf(dlog_file, "WRITE_THROUGH ");
    if (create_opts & FILE_SEQUENTIAL_ONLY)
        fprintf(dlog_file, "SEQUENTIAL_ONLY ");
    if (create_opts & FILE_RANDOM_ACCESS)
        fprintf(dlog_file, "RANDOM_ACCESS ");
    if (create_opts & FILE_NO_INTERMEDIATE_BUFFERING)
        fprintf(dlog_file, "NO_INTERMEDIATE_BUFFERING ");
    if (create_opts & FILE_SYNCHRONOUS_IO_ALERT)
        fprintf(dlog_file, "SYNCHRONOUS_IO_ALERT ");
    if (create_opts & FILE_SYNCHRONOUS_IO_NONALERT)
        fprintf(dlog_file, "SYNCHRONOUS_IO_NONALERT ");
    if (create_opts & FILE_CREATE_TREE_CONNECTION)
        fprintf(dlog_file, "CREATE_TREE_CONNECTION ");
    if (create_opts & FILE_COMPLETE_IF_OPLOCKED)
        fprintf(dlog_file, "COMPLETE_IF_OPLOCKED ");
    if (create_opts & FILE_NO_EA_KNOWLEDGE)
        fprintf(dlog_file, "NO_EA_KNOWLEDGE ");
    if (create_opts & FILE_OPEN_REPARSE_POINT)
        fprintf(dlog_file, "OPEN_REPARSE_POINT ");
    if (create_opts & FILE_DELETE_ON_CLOSE)
        fprintf(dlog_file, "DELETE_ON_CLOSE ");
    if (create_opts & FILE_OPEN_BY_FILE_ID)
        fprintf(dlog_file, "OPEN_BY_FILE_ID ");
    if (create_opts & FILE_OPEN_FOR_BACKUP_INTENT)
        fprintf(dlog_file, "OPEN_FOR_BACKUP_INTENT ");
    if (create_opts & FILE_RESERVE_OPFILTER)
        fprintf(dlog_file, "RESERVE_OPFILTER");
    fprintf(dlog_file, "\n");
}

void print_disposition(int level, DWORD disposition) {
    if (level > g_debug_level) return;
    fprintf(dlog_file, "userland disposition = ");
    if (disposition == FILE_SUPERSEDE)
        fprintf(dlog_file, "FILE_SUPERSEDE\n");
    else if (disposition == FILE_CREATE)
        fprintf(dlog_file, "FILE_CREATE\n");
    else if (disposition == FILE_OPEN)
        fprintf(dlog_file, "FILE_OPEN\n");
    else if (disposition == FILE_OPEN_IF)
        fprintf(dlog_file, "FILE_OPEN_IF\n");
    else if (disposition == FILE_OVERWRITE)
        fprintf(dlog_file, "FILE_OVERWRITE\n");
    else if (disposition == FILE_OVERWRITE_IF)
        fprintf(dlog_file, "FILE_OVERWRITE_IF\n");
}

void print_access_mask(int level, DWORD access_mask) {
    if (level > g_debug_level) return;
    fprintf(dlog_file, "access mask: ");
    if (access_mask & FILE_READ_DATA)
        fprintf(dlog_file, "READ ");
    if (access_mask & STANDARD_RIGHTS_READ)
        fprintf(dlog_file, "READ_ACL ");
    if (access_mask & FILE_READ_ATTRIBUTES)
        fprintf(dlog_file, "READ_ATTR ");
    if (access_mask & FILE_READ_EA)
        fprintf(dlog_file, "READ_EA ");
    if (access_mask & FILE_WRITE_DATA)
        fprintf(dlog_file, "WRITE ");
    if (access_mask & STANDARD_RIGHTS_WRITE)
        fprintf(dlog_file, "WRITE_ACL ");
    if (access_mask & FILE_WRITE_ATTRIBUTES)
        fprintf(dlog_file, "WRITE_ATTR ");
    if (access_mask & FILE_WRITE_EA)
        fprintf(dlog_file, "WRITE_EA ");
    if (access_mask & FILE_APPEND_DATA)
        fprintf(dlog_file, "APPEND ");
    if (access_mask & FILE_EXECUTE)
        fprintf(dlog_file, "EXECUTE ");
    if (access_mask & FILE_LIST_DIRECTORY)
        fprintf(dlog_file, "LIST ");
    if (access_mask & FILE_TRAVERSE)
        fprintf(dlog_file, "TRAVERSE ");
    if (access_mask & SYNCHRONIZE)
        fprintf(dlog_file, "SYNC ");
    if (access_mask & FILE_DELETE_CHILD)
        fprintf(dlog_file, "DELETE_CHILD");
    fprintf(dlog_file, "\n");
}

void print_share_mode(int level, DWORD mode)
{
    if (level > g_debug_level) return;
    fprintf(dlog_file, "share mode: ");
    if (mode & FILE_SHARE_READ)
        fprintf(dlog_file, "READ ");
    if (mode & FILE_SHARE_WRITE)
        fprintf(dlog_file, "WRITE ");
    if (mode & FILE_SHARE_DELETE)
        fprintf(dlog_file, "DELETE");
    fprintf(dlog_file, "\n");
}

void print_file_id_both_dir_info(int level, const FILE_ID_BOTH_DIR_INFO *pboth_dir_info)
{
    /* printf %zd is for |size_t| */

    if (level > g_debug_level)
        return;
    (void)fprintf(dlog_file, "FILE_ID_BOTH_DIR_INFO 0x%p %zd\n",
       pboth_dir_info, sizeof(unsigned char *));
    (void)fprintf(dlog_file, "\tNextEntryOffset=%ld %zd %zd\n",
        pboth_dir_info->NextEntryOffset,
        sizeof(pboth_dir_info->NextEntryOffset), sizeof(DWORD));
    (void)fprintf(dlog_file, "\tFileIndex=%ld %zd\n",
        pboth_dir_info->FileIndex,
        sizeof(pboth_dir_info->FileIndex));
    (void)fprintf(dlog_file, "\tCreationTime=0x%llx %zd\n",
        (long long)pboth_dir_info->CreationTime.QuadPart,
        sizeof(pboth_dir_info->CreationTime));
    (void)fprintf(dlog_file, "\tLastAccessTime=0x%llx %zd\n",
        (long long)pboth_dir_info->LastAccessTime.QuadPart,
        sizeof(pboth_dir_info->LastAccessTime));
    (void)fprintf(dlog_file, "\tLastWriteTime=0x%llx %zd\n",
        (long long)pboth_dir_info->LastWriteTime.QuadPart,
        sizeof(pboth_dir_info->LastWriteTime));
    (void)fprintf(dlog_file, "\tChangeTime=0x%llx %zd\n",
        (long long)pboth_dir_info->ChangeTime.QuadPart,
        sizeof(pboth_dir_info->ChangeTime));
    (void)fprintf(dlog_file, "\tEndOfFile=0x%llx %zd\n",
        (long long)pboth_dir_info->EndOfFile.QuadPart,
        sizeof(pboth_dir_info->EndOfFile));
    (void)fprintf(dlog_file, "\tAllocationSize=0x%llx %zd\n",
        (long long)pboth_dir_info->AllocationSize.QuadPart,
        sizeof(pboth_dir_info->AllocationSize));
    (void)fprintf(dlog_file, "\tFileAttributes=%ld %zd\n",
        pboth_dir_info->FileAttributes,
        sizeof(pboth_dir_info->FileAttributes));
    (void)fprintf(dlog_file, "\tFileNameLength=%ld %zd\n",
        pboth_dir_info->FileNameLength,
        sizeof(pboth_dir_info->FileNameLength));
    (void)fprintf(dlog_file, "\tEaSize=%ld %zd\n",
        pboth_dir_info->EaSize,
        sizeof(pboth_dir_info->EaSize));
    (void)fprintf(dlog_file, "\tShortNameLength=%d %zd\n",
        pboth_dir_info->ShortNameLength,
        sizeof(pboth_dir_info->ShortNameLength));
    (void)fprintf(dlog_file, "\tShortName='%S' %zd\n",
        pboth_dir_info->ShortName,
        sizeof(pboth_dir_info->ShortName));
    (void)fprintf(dlog_file, "\tFileId=0x%llx %zd\n",
        (long long)pboth_dir_info->FileId.QuadPart,
        sizeof(pboth_dir_info->FileId));
    (void)fprintf(dlog_file, "\tFileName='%S' 0x%p\n",
        pboth_dir_info->FileName,
        pboth_dir_info->FileName);
}

void print_sid(const char *label, PSID sid)
{
    PSTR sidstr = NULL;

    if (ConvertSidToStringSidA(sid, &sidstr)) {
        dprintf_out("%s=SID('%s')\n", label, sidstr);
        LocalFree(sidstr);
    }
    else {
        int status;

        status = GetLastError();
        dprintf_out("%s=<ConvertSidToStringSidA() failed error=%d>\n",
            label, status);
    }
}

const char* opcode2string(nfs41_opcodes opcode)
{
    switch(opcode) {
#define NFSOPCODE_TO_STRLITERAL(e) case e: return #e;
        NFSOPCODE_TO_STRLITERAL(NFS41_INVALID_OPCODE0)
        NFSOPCODE_TO_STRLITERAL(NFS41_SHUTDOWN)
        NFSOPCODE_TO_STRLITERAL(NFS41_MOUNT)
        NFSOPCODE_TO_STRLITERAL(NFS41_UNMOUNT)
        NFSOPCODE_TO_STRLITERAL(NFS41_OPEN)
        NFSOPCODE_TO_STRLITERAL(NFS41_CLOSE)
        NFSOPCODE_TO_STRLITERAL(NFS41_READ)
        NFSOPCODE_TO_STRLITERAL(NFS41_WRITE)
        NFSOPCODE_TO_STRLITERAL(NFS41_LOCK)
        NFSOPCODE_TO_STRLITERAL(NFS41_UNLOCK)
        NFSOPCODE_TO_STRLITERAL(NFS41_DIR_QUERY)
        NFSOPCODE_TO_STRLITERAL(NFS41_FILE_QUERY)
        NFSOPCODE_TO_STRLITERAL(NFS41_FILE_QUERY_TIME_BASED_COHERENCY)
        NFSOPCODE_TO_STRLITERAL(NFS41_FILE_SET)
        NFSOPCODE_TO_STRLITERAL(NFS41_EA_SET)
        NFSOPCODE_TO_STRLITERAL(NFS41_EA_GET)
        NFSOPCODE_TO_STRLITERAL(NFS41_SYMLINK)
        NFSOPCODE_TO_STRLITERAL(NFS41_VOLUME_QUERY)
        NFSOPCODE_TO_STRLITERAL(NFS41_ACL_QUERY)
        NFSOPCODE_TO_STRLITERAL(NFS41_ACL_SET)
    }
    return "<unknown NFS41 opcode>";
}

const char* nfs_opnum_to_string(int opnum)
{
    switch (opnum)
    {
#define NFSOPNUM_TO_STRLITERAL(e) case e: return #e;
        NFSOPNUM_TO_STRLITERAL(OP_ACCESS)
        NFSOPNUM_TO_STRLITERAL(OP_CLOSE)
        NFSOPNUM_TO_STRLITERAL(OP_COMMIT)
        NFSOPNUM_TO_STRLITERAL(OP_CREATE)
        NFSOPNUM_TO_STRLITERAL(OP_DELEGPURGE)
        NFSOPNUM_TO_STRLITERAL(OP_DELEGRETURN)
        NFSOPNUM_TO_STRLITERAL(OP_GETATTR)
        NFSOPNUM_TO_STRLITERAL(OP_GETFH)
        NFSOPNUM_TO_STRLITERAL(OP_LINK)
        NFSOPNUM_TO_STRLITERAL(OP_LOCK)
        NFSOPNUM_TO_STRLITERAL(OP_LOCKT)
        NFSOPNUM_TO_STRLITERAL(OP_LOCKU)
        NFSOPNUM_TO_STRLITERAL(OP_LOOKUP)
        NFSOPNUM_TO_STRLITERAL(OP_LOOKUPP)
        NFSOPNUM_TO_STRLITERAL(OP_NVERIFY)
        NFSOPNUM_TO_STRLITERAL(OP_OPEN)
        NFSOPNUM_TO_STRLITERAL(OP_OPENATTR)
        NFSOPNUM_TO_STRLITERAL(OP_OPEN_CONFIRM)
        NFSOPNUM_TO_STRLITERAL(OP_OPEN_DOWNGRADE)
        NFSOPNUM_TO_STRLITERAL(OP_PUTFH)
        NFSOPNUM_TO_STRLITERAL(OP_PUTPUBFH)
        NFSOPNUM_TO_STRLITERAL(OP_PUTROOTFH)
        NFSOPNUM_TO_STRLITERAL(OP_READ)
        NFSOPNUM_TO_STRLITERAL(OP_READDIR)
        NFSOPNUM_TO_STRLITERAL(OP_READLINK)
        NFSOPNUM_TO_STRLITERAL(OP_REMOVE)
        NFSOPNUM_TO_STRLITERAL(OP_RENAME)
        NFSOPNUM_TO_STRLITERAL(OP_RENEW)
        NFSOPNUM_TO_STRLITERAL(OP_RESTOREFH)
        NFSOPNUM_TO_STRLITERAL(OP_SAVEFH)
        NFSOPNUM_TO_STRLITERAL(OP_SECINFO)
        NFSOPNUM_TO_STRLITERAL(OP_SETATTR)
        NFSOPNUM_TO_STRLITERAL(OP_SETCLIENTID)
        NFSOPNUM_TO_STRLITERAL(OP_SETCLIENTID_CONFIRM)
        NFSOPNUM_TO_STRLITERAL(OP_VERIFY)
        NFSOPNUM_TO_STRLITERAL(OP_WRITE)
        NFSOPNUM_TO_STRLITERAL(OP_RELEASE_LOCKOWNER)
        NFSOPNUM_TO_STRLITERAL(OP_BACKCHANNEL_CTL)
        NFSOPNUM_TO_STRLITERAL(OP_BIND_CONN_TO_SESSION)
        NFSOPNUM_TO_STRLITERAL(OP_EXCHANGE_ID)
        NFSOPNUM_TO_STRLITERAL(OP_CREATE_SESSION)
        NFSOPNUM_TO_STRLITERAL(OP_DESTROY_SESSION)
        NFSOPNUM_TO_STRLITERAL(OP_FREE_STATEID)
        NFSOPNUM_TO_STRLITERAL(OP_GET_DIR_DELEGATION)
        NFSOPNUM_TO_STRLITERAL(OP_GETDEVICEINFO)
        NFSOPNUM_TO_STRLITERAL(OP_GETDEVICELIST)
        NFSOPNUM_TO_STRLITERAL(OP_LAYOUTCOMMIT)
        NFSOPNUM_TO_STRLITERAL(OP_LAYOUTGET)
        NFSOPNUM_TO_STRLITERAL(OP_LAYOUTRETURN)
        NFSOPNUM_TO_STRLITERAL(OP_SECINFO_NO_NAME)
        NFSOPNUM_TO_STRLITERAL(OP_SEQUENCE)
        NFSOPNUM_TO_STRLITERAL(OP_SET_SSV)
        NFSOPNUM_TO_STRLITERAL(OP_TEST_STATEID)
        NFSOPNUM_TO_STRLITERAL(OP_WANT_DELEGATION)
        NFSOPNUM_TO_STRLITERAL(OP_DESTROY_CLIENTID)
        NFSOPNUM_TO_STRLITERAL(OP_RECLAIM_COMPLETE)
        NFSOPNUM_TO_STRLITERAL(OP_ILLEGAL)
    }
    return "<invalid nfs opnum>";
}

const char* nfs_error_string(int status)
{
    switch (status)
    {
#define NFSERR_TO_STRLITERAL(e) case e: return #e;
    NFSERR_TO_STRLITERAL(NFS4_OK)
    NFSERR_TO_STRLITERAL(NFS4ERR_PERM)
    NFSERR_TO_STRLITERAL(NFS4ERR_NOENT)
    NFSERR_TO_STRLITERAL(NFS4ERR_IO)
    NFSERR_TO_STRLITERAL(NFS4ERR_NXIO)
    NFSERR_TO_STRLITERAL(NFS4ERR_ACCESS)
    NFSERR_TO_STRLITERAL(NFS4ERR_EXIST)
    NFSERR_TO_STRLITERAL(NFS4ERR_XDEV)
    NFSERR_TO_STRLITERAL(NFS4ERR_NOTDIR)
    NFSERR_TO_STRLITERAL(NFS4ERR_ISDIR)
    NFSERR_TO_STRLITERAL(NFS4ERR_INVAL)
    NFSERR_TO_STRLITERAL(NFS4ERR_FBIG)
    NFSERR_TO_STRLITERAL(NFS4ERR_NOSPC)
    NFSERR_TO_STRLITERAL(NFS4ERR_ROFS)
    NFSERR_TO_STRLITERAL(NFS4ERR_MLINK)
    NFSERR_TO_STRLITERAL(NFS4ERR_NAMETOOLONG)
    NFSERR_TO_STRLITERAL(NFS4ERR_NOTEMPTY)
    NFSERR_TO_STRLITERAL(NFS4ERR_DQUOT)
    NFSERR_TO_STRLITERAL(NFS4ERR_STALE)
    NFSERR_TO_STRLITERAL(NFS4ERR_BADHANDLE)
    NFSERR_TO_STRLITERAL(NFS4ERR_BAD_COOKIE)
    NFSERR_TO_STRLITERAL(NFS4ERR_NOTSUPP)
    NFSERR_TO_STRLITERAL(NFS4ERR_TOOSMALL)
    NFSERR_TO_STRLITERAL(NFS4ERR_SERVERFAULT)
    NFSERR_TO_STRLITERAL(NFS4ERR_BADTYPE)
    NFSERR_TO_STRLITERAL(NFS4ERR_DELAY)
    NFSERR_TO_STRLITERAL(NFS4ERR_SAME)
    NFSERR_TO_STRLITERAL(NFS4ERR_DENIED)
    NFSERR_TO_STRLITERAL(NFS4ERR_EXPIRED)
    NFSERR_TO_STRLITERAL(NFS4ERR_LOCKED)
    NFSERR_TO_STRLITERAL(NFS4ERR_GRACE)
    NFSERR_TO_STRLITERAL(NFS4ERR_FHEXPIRED)
    NFSERR_TO_STRLITERAL(NFS4ERR_SHARE_DENIED)
    NFSERR_TO_STRLITERAL(NFS4ERR_WRONGSEC)
    NFSERR_TO_STRLITERAL(NFS4ERR_CLID_INUSE)
    NFSERR_TO_STRLITERAL(NFS4ERR_RESOURCE)
    NFSERR_TO_STRLITERAL(NFS4ERR_MOVED)
    NFSERR_TO_STRLITERAL(NFS4ERR_NOFILEHANDLE)
    NFSERR_TO_STRLITERAL(NFS4ERR_MINOR_VERS_MISMATCH)
    NFSERR_TO_STRLITERAL(NFS4ERR_STALE_CLIENTID)
    NFSERR_TO_STRLITERAL(NFS4ERR_STALE_STATEID)
    NFSERR_TO_STRLITERAL(NFS4ERR_OLD_STATEID)
    NFSERR_TO_STRLITERAL(NFS4ERR_BAD_STATEID)
    NFSERR_TO_STRLITERAL(NFS4ERR_BAD_SEQID)
    NFSERR_TO_STRLITERAL(NFS4ERR_NOT_SAME)
    NFSERR_TO_STRLITERAL(NFS4ERR_LOCK_RANGE)
    NFSERR_TO_STRLITERAL(NFS4ERR_SYMLINK)
    NFSERR_TO_STRLITERAL(NFS4ERR_RESTOREFH)
    NFSERR_TO_STRLITERAL(NFS4ERR_LEASE_MOVED)
    NFSERR_TO_STRLITERAL(NFS4ERR_ATTRNOTSUPP)
    NFSERR_TO_STRLITERAL(NFS4ERR_NO_GRACE)
    NFSERR_TO_STRLITERAL(NFS4ERR_RECLAIM_BAD)
    NFSERR_TO_STRLITERAL(NFS4ERR_RECLAIM_CONFLICT)
    NFSERR_TO_STRLITERAL(NFS4ERR_BADXDR)
    NFSERR_TO_STRLITERAL(NFS4ERR_LOCKS_HELD)
    NFSERR_TO_STRLITERAL(NFS4ERR_OPENMODE)
    NFSERR_TO_STRLITERAL(NFS4ERR_BADOWNER)
    NFSERR_TO_STRLITERAL(NFS4ERR_BADCHAR)
    NFSERR_TO_STRLITERAL(NFS4ERR_BADNAME)
    NFSERR_TO_STRLITERAL(NFS4ERR_BAD_RANGE)
    NFSERR_TO_STRLITERAL(NFS4ERR_LOCK_NOTSUPP)
    NFSERR_TO_STRLITERAL(NFS4ERR_OP_ILLEGAL)
    NFSERR_TO_STRLITERAL(NFS4ERR_DEADLOCK)
    NFSERR_TO_STRLITERAL(NFS4ERR_FILE_OPEN)
    NFSERR_TO_STRLITERAL(NFS4ERR_ADMIN_REVOKED)
    NFSERR_TO_STRLITERAL(NFS4ERR_CB_PATH_DOWN)
    NFSERR_TO_STRLITERAL(NFS4ERR_BADIOMODE)
    NFSERR_TO_STRLITERAL(NFS4ERR_BADLAYOUT)
    NFSERR_TO_STRLITERAL(NFS4ERR_BAD_SESSION_DIGEST)
    NFSERR_TO_STRLITERAL(NFS4ERR_BADSESSION)
    NFSERR_TO_STRLITERAL(NFS4ERR_BADSLOT)
    NFSERR_TO_STRLITERAL(NFS4ERR_COMPLETE_ALREADY)
    NFSERR_TO_STRLITERAL(NFS4ERR_CONN_NOT_BOUND_TO_SESSION)
    NFSERR_TO_STRLITERAL(NFS4ERR_DELEG_ALREADY_WANTED)
    NFSERR_TO_STRLITERAL(NFS4ERR_BACK_CHAN_BUSY)
    NFSERR_TO_STRLITERAL(NFS4ERR_LAYOUTTRYLATER)
    NFSERR_TO_STRLITERAL(NFS4ERR_LAYOUTUNAVAILABLE)
    NFSERR_TO_STRLITERAL(NFS4ERR_NOMATCHING_LAYOUT)
    NFSERR_TO_STRLITERAL(NFS4ERR_RECALLCONFLICT)
    NFSERR_TO_STRLITERAL(NFS4ERR_UNKNOWN_LAYOUTTYPE)
    NFSERR_TO_STRLITERAL(NFS4ERR_SEQ_MISORDERED)
    NFSERR_TO_STRLITERAL(NFS4ERR_SEQUENCE_POS)
    NFSERR_TO_STRLITERAL(NFS4ERR_REQ_TOO_BIG)
    NFSERR_TO_STRLITERAL(NFS4ERR_REP_TOO_BIG)
    NFSERR_TO_STRLITERAL(NFS4ERR_REP_TOO_BIG_TO_CACHE)
    NFSERR_TO_STRLITERAL(NFS4ERR_RETRY_UNCACHED_REP)
    NFSERR_TO_STRLITERAL(NFS4ERR_UNSAFE_COMPOUND)
    NFSERR_TO_STRLITERAL(NFS4ERR_TOO_MANY_OPS)
    NFSERR_TO_STRLITERAL(NFS4ERR_OP_NOT_IN_SESSION)
    NFSERR_TO_STRLITERAL(NFS4ERR_HASH_ALG_UNSUPP)
    NFSERR_TO_STRLITERAL(NFS4ERR_CLIENTID_BUSY)
    NFSERR_TO_STRLITERAL(NFS4ERR_PNFS_IO_HOLE)
    NFSERR_TO_STRLITERAL(NFS4ERR_SEQ_FALSE_RETRY)
    NFSERR_TO_STRLITERAL(NFS4ERR_BAD_HIGH_SLOT)
    NFSERR_TO_STRLITERAL(NFS4ERR_DEADSESSION)
    NFSERR_TO_STRLITERAL(NFS4ERR_ENCR_ALG_UNSUPP)
    NFSERR_TO_STRLITERAL(NFS4ERR_PNFS_NO_LAYOUT)
    NFSERR_TO_STRLITERAL(NFS4ERR_NOT_ONLY_OP)
    NFSERR_TO_STRLITERAL(NFS4ERR_WRONG_CRED)
    NFSERR_TO_STRLITERAL(NFS4ERR_WRONG_TYPE)
    NFSERR_TO_STRLITERAL(NFS4ERR_DIRDELEG_UNAVAIL)
    NFSERR_TO_STRLITERAL(NFS4ERR_REJECT_DELEG)
    NFSERR_TO_STRLITERAL(NFS4ERR_RETURNCONFLICT)
    NFSERR_TO_STRLITERAL(NFS4ERR_DELEG_REVOKED)
    NFSERR_TO_STRLITERAL(NFS4ERR_PARTNER_NOTSUPP)
    NFSERR_TO_STRLITERAL(NFS4ERR_PARTNER_NO_AUTH)
    NFSERR_TO_STRLITERAL(NFS4ERR_UNION_NOTSUPP)
    NFSERR_TO_STRLITERAL(NFS4ERR_OFFLOAD_DENIED)
    NFSERR_TO_STRLITERAL(NFS4ERR_WRONG_LFS)
    NFSERR_TO_STRLITERAL(NFS4ERR_BADLABEL)
    NFSERR_TO_STRLITERAL(NFS4ERR_OFFLOAD_NO_REQS)
    NFSERR_TO_STRLITERAL(NFS4ERR_NOXATTR)
    NFSERR_TO_STRLITERAL(NFS4ERR_XATTR2BIG)
    }
    return "<invalid NFS4ERR_* code>";
}

const char* rpc_error_string(int status)
{
    switch (status)
    {
#define RPCERR_TO_STRLITERAL(e) case e: return #e;
        RPCERR_TO_STRLITERAL(RPC_CANTENCODEARGS)
        RPCERR_TO_STRLITERAL(RPC_CANTDECODERES)
        RPCERR_TO_STRLITERAL(RPC_CANTSEND)
        RPCERR_TO_STRLITERAL(RPC_CANTRECV)
        RPCERR_TO_STRLITERAL(RPC_TIMEDOUT)
        RPCERR_TO_STRLITERAL(RPC_INTR)
        RPCERR_TO_STRLITERAL(RPC_UDERROR)
        RPCERR_TO_STRLITERAL(RPC_VERSMISMATCH)
        RPCERR_TO_STRLITERAL(RPC_AUTHERROR)
        RPCERR_TO_STRLITERAL(RPC_PROGUNAVAIL)
        RPCERR_TO_STRLITERAL(RPC_PROGVERSMISMATCH)
        RPCERR_TO_STRLITERAL(RPC_PROCUNAVAIL)
        RPCERR_TO_STRLITERAL(RPC_CANTDECODEARGS)
        RPCERR_TO_STRLITERAL(RPC_SYSTEMERROR)
    }
    return "<invalid RPC_* error code>";
}

const char* gssauth_string(int type) {
    switch(type) {
#define RPCSEC_TO_STRLITERAL(e) case e: return #e;
        RPCSEC_TO_STRLITERAL(RPCSEC_SSPI_SVC_NONE)
        RPCSEC_TO_STRLITERAL(RPCSEC_SSPI_SVC_INTEGRITY)
        RPCSEC_TO_STRLITERAL(RPCSEC_SSPI_SVC_PRIVACY)
    }
    return "<invalid RPCSEC_SSPI_* gss auth type>";
}

const char *FILE_INFORMATION_CLASS2string(int fic)
{
    switch(fic) {
#define FIC_TO_STRLITERAL(e) case e: return #e;
        FIC_TO_STRLITERAL(FileDirectoryInformation)
        FIC_TO_STRLITERAL(FileFullDirectoryInformation)
        FIC_TO_STRLITERAL(FileBothDirectoryInformation)
        FIC_TO_STRLITERAL(FileBasicInformation)
        FIC_TO_STRLITERAL(FileStandardInformation)
        FIC_TO_STRLITERAL(FileInternalInformation)
        FIC_TO_STRLITERAL(FileEaInformation)
        FIC_TO_STRLITERAL(FileAccessInformation)
        FIC_TO_STRLITERAL(FileNameInformation)
        FIC_TO_STRLITERAL(FileRenameInformation)
        FIC_TO_STRLITERAL(FileLinkInformation)
        FIC_TO_STRLITERAL(FileNamesInformation)
        FIC_TO_STRLITERAL(FileDispositionInformation)
        FIC_TO_STRLITERAL(FilePositionInformation)
        FIC_TO_STRLITERAL(FileFullEaInformation)
        FIC_TO_STRLITERAL(FileModeInformation)
        FIC_TO_STRLITERAL(FileAlignmentInformation)
        FIC_TO_STRLITERAL(FileAllInformation)
        FIC_TO_STRLITERAL(FileAllocationInformation)
        FIC_TO_STRLITERAL(FileEndOfFileInformation)
        FIC_TO_STRLITERAL(FileAlternateNameInformation)
        FIC_TO_STRLITERAL(FileStreamInformation)
        FIC_TO_STRLITERAL(FilePipeInformation)
        FIC_TO_STRLITERAL(FilePipeLocalInformation)
        FIC_TO_STRLITERAL(FilePipeRemoteInformation)
        FIC_TO_STRLITERAL(FileMailslotQueryInformation)
        FIC_TO_STRLITERAL(FileMailslotSetInformation)
        FIC_TO_STRLITERAL(FileCompressionInformation)
        FIC_TO_STRLITERAL(FileObjectIdInformation)
        FIC_TO_STRLITERAL(FileCompletionInformation)
        FIC_TO_STRLITERAL(FileMoveClusterInformation)
        FIC_TO_STRLITERAL(FileQuotaInformation)
        FIC_TO_STRLITERAL(FileReparsePointInformation)
        FIC_TO_STRLITERAL(FileNetworkOpenInformation)
        FIC_TO_STRLITERAL(FileAttributeTagInformation)
        FIC_TO_STRLITERAL(FileTrackingInformation)
        FIC_TO_STRLITERAL(FileIdBothDirectoryInformation)
        FIC_TO_STRLITERAL(FileIdFullDirectoryInformation)
        FIC_TO_STRLITERAL(FileValidDataLengthInformation)
        FIC_TO_STRLITERAL(FileShortNameInformation)
        FIC_TO_STRLITERAL(FileIoCompletionNotificationInformation)
        FIC_TO_STRLITERAL(FileIoStatusBlockRangeInformation)
        FIC_TO_STRLITERAL(FileIoPriorityHintInformation)
        FIC_TO_STRLITERAL(FileSfioReserveInformation)
        FIC_TO_STRLITERAL(FileSfioVolumeInformation)
        FIC_TO_STRLITERAL(FileHardLinkInformation)
        FIC_TO_STRLITERAL(FileProcessIdsUsingFileInformation)
        FIC_TO_STRLITERAL(FileNormalizedNameInformation)
        FIC_TO_STRLITERAL(FileNetworkPhysicalNameInformation)
        FIC_TO_STRLITERAL(FileIdGlobalTxDirectoryInformation)
        FIC_TO_STRLITERAL(FileIsRemoteDeviceInformation)
        FIC_TO_STRLITERAL(FileUnusedInformation)
        FIC_TO_STRLITERAL(FileNumaNodeInformation)
        FIC_TO_STRLITERAL(FileStandardLinkInformation)
        FIC_TO_STRLITERAL(FileRemoteProtocolInformation)
        FIC_TO_STRLITERAL(FileRenameInformationBypassAccessCheck)
        FIC_TO_STRLITERAL(FileLinkInformationBypassAccessCheck)
        FIC_TO_STRLITERAL(FileVolumeNameInformation)
        FIC_TO_STRLITERAL(FileIdInformation)
        FIC_TO_STRLITERAL(FileIdExtdDirectoryInformation)
        FIC_TO_STRLITERAL(FileReplaceCompletionInformation)
        FIC_TO_STRLITERAL(FileHardLinkFullIdInformation)
        FIC_TO_STRLITERAL(FileIdExtdBothDirectoryInformation)
        FIC_TO_STRLITERAL(FileDispositionInformationEx)
        FIC_TO_STRLITERAL(FileRenameInformationEx)
        FIC_TO_STRLITERAL(FileRenameInformationExBypassAccessCheck)
        FIC_TO_STRLITERAL(FileDesiredStorageClassInformation)
        FIC_TO_STRLITERAL(FileStatInformation)
        FIC_TO_STRLITERAL(FileMemoryPartitionInformation)
        FIC_TO_STRLITERAL(FileStatLxInformation)
        FIC_TO_STRLITERAL(FileCaseSensitiveInformation)
        FIC_TO_STRLITERAL(FileLinkInformationEx)
        FIC_TO_STRLITERAL(FileLinkInformationExBypassAccessCheck)
        FIC_TO_STRLITERAL(FileStorageReserveIdInformation)
        FIC_TO_STRLITERAL(FileCaseSensitiveInformationForceAccessCheck)
    }
    return "<unknown FILE_INFORMATION_CLASS>";
}

void print_condwait_status(int level, int status)
{
    if (level > g_debug_level) return;
    switch(status) {
        case WAIT_ABANDONED: fprintf(dlog_file, "WAIT_ABANDONED\n"); break;
        case WAIT_OBJECT_0: fprintf(dlog_file, "WAIT_OBJECT_0\n"); break;
        case WAIT_TIMEOUT: fprintf(dlog_file, "WAIT_TIMEOUT\n"); break;
        case WAIT_FAILED: fprintf(dlog_file, "WAIT_FAILED %d\n", GetLastError());
        default: fprintf(dlog_file, "unknown status =%d\n", status);
    }
}

void print_sr_status_flags(int level, int flags)
{
    if (level > g_debug_level) return;
    fprintf(dlog_file, "%04x: sr_status_flags: ", GetCurrentThreadId());
    if (flags & SEQ4_STATUS_CB_PATH_DOWN) 
        fprintf(dlog_file, "SEQ4_STATUS_CB_PATH_DOWN ");
    if (flags & SEQ4_STATUS_CB_GSS_CONTEXTS_EXPIRING) 
        fprintf(dlog_file, "SEQ4_STATUS_CB_GSS_CONTEXTS_EXPIRING ");
    if (flags & SEQ4_STATUS_CB_GSS_CONTEXTS_EXPIRED) 
        fprintf(dlog_file, "SEQ4_STATUS_CB_GSS_CONTEXTS_EXPIRED ");
    if (flags & SEQ4_STATUS_EXPIRED_ALL_STATE_REVOKED) 
        fprintf(dlog_file, "SEQ4_STATUS_EXPIRED_ALL_STATE_REVOKED ");
    if (flags & SEQ4_STATUS_EXPIRED_SOME_STATE_REVOKED) 
        fprintf(dlog_file, "SEQ4_STATUS_EXPIRED_SOME_STATE_REVOKED ");
    if (flags & SEQ4_STATUS_ADMIN_STATE_REVOKED) 
        fprintf(dlog_file, "SEQ4_STATUS_ADMIN_STATE_REVOKED ");
    if (flags & SEQ4_STATUS_RECALLABLE_STATE_REVOKED) 
        fprintf(dlog_file, "SEQ4_STATUS_RECALLABLE_STATE_REVOKED ");
    if (flags & SEQ4_STATUS_LEASE_MOVED) 
        fprintf(dlog_file, "SEQ4_STATUS_LEASE_MOVED ");
    if (flags & SEQ4_STATUS_RESTART_RECLAIM_NEEDED) 
        fprintf(dlog_file, "SEQ4_STATUS_RESTART_RECLAIM_NEEDED ");
    if (flags & SEQ4_STATUS_CB_PATH_DOWN_SESSION) 
        fprintf(dlog_file, "SEQ4_STATUS_CB_PATH_DOWN_SESSION ");
    if (flags & SEQ4_STATUS_BACKCHANNEL_FAULT) 
        fprintf(dlog_file, "SEQ4_STATUS_BACKCHANNEL_FAULT ");
    if (flags & SEQ4_STATUS_DEVID_CHANGED) 
        fprintf(dlog_file, "SEQ4_STATUS_DEVID_CHANGED ");
    if (flags & SEQ4_STATUS_DEVID_DELETED) 
        fprintf(dlog_file, "SEQ4_STATUS_DEVID_DELETED ");
    fprintf(dlog_file, "\n");
}

const char* secflavorop2name(DWORD sec_flavor)
{
    switch(sec_flavor) {
#define RPCSEC_AUTH_TO_STRLITERAL(e) case e: return #e;
        RPCSEC_AUTH_TO_STRLITERAL(RPCSEC_AUTH_SYS)
        RPCSEC_AUTH_TO_STRLITERAL(RPCSEC_AUTHGSS_KRB5)
        RPCSEC_AUTH_TO_STRLITERAL(RPCSEC_AUTHGSS_KRB5I)
        RPCSEC_AUTH_TO_STRLITERAL(RPCSEC_AUTHGSS_KRB5P)
    }

    return "<Unknown RPCSEC_AUTH* flavour>";
}

void print_windows_access_mask(int on, ACCESS_MASK m)
{
    if (!on)
        return;
    if (!DPRINTF_LEVEL_ENABLED(1))
        return;

    dprintf_out("--> print_windows_access_mask: %x\n", m);
    if (m & GENERIC_READ)
        dprintf_out("\tGENERIC_READ\n");
    if (m & GENERIC_WRITE)
        dprintf_out("\tGENERIC_WRITE\n");
    if (m & GENERIC_EXECUTE)
        dprintf_out("\tGENERIC_EXECUTE\n");
    if (m & GENERIC_ALL)
        dprintf_out("\tGENERIC_ALL\n");
    if (m & MAXIMUM_ALLOWED)
        dprintf_out("\tMAXIMUM_ALLOWED\n");
    if (m & ACCESS_SYSTEM_SECURITY)
        dprintf_out("\tACCESS_SYSTEM_SECURITY\n");
    if ((m & SPECIFIC_RIGHTS_ALL) == SPECIFIC_RIGHTS_ALL)
        dprintf_out("\tSPECIFIC_RIGHTS_ALL\n");
    if ((m & STANDARD_RIGHTS_ALL) == STANDARD_RIGHTS_ALL)
        dprintf_out("\tSTANDARD_RIGHTS_ALL\n");
    if ((m & STANDARD_RIGHTS_REQUIRED) == STANDARD_RIGHTS_REQUIRED)
        dprintf_out("\tSTANDARD_RIGHTS_REQUIRED\n");
    if (m & SYNCHRONIZE)
        dprintf_out("\tSYNCHRONIZE\n");
    if (m & WRITE_OWNER)
        dprintf_out("\tWRITE_OWNER\n");
    if (m & WRITE_DAC)
        dprintf_out("\tWRITE_DAC\n");
    if (m & READ_CONTROL)
        dprintf_out("\tREAD_CONTROL\n");
    if (m & DELETE)
        dprintf_out("\tDELETE\n");
    if (m & FILE_READ_DATA)
        dprintf_out("\tFILE_READ_DATA\n");
    if (m & FILE_LIST_DIRECTORY)
        dprintf_out("\tFILE_LIST_DIRECTORY\n");
    if (m & FILE_WRITE_DATA)
        dprintf_out("\tFILE_WRITE_DATA\n");
    if (m & FILE_ADD_FILE)
        dprintf_out("\tFILE_ADD_FILE\n");
    if (m & FILE_APPEND_DATA)
        dprintf_out("\tFILE_APPEND_DATA\n");
    if (m & FILE_ADD_SUBDIRECTORY)
        dprintf_out("\tFILE_ADD_SUBDIRECTORY\n");
    if (m & FILE_CREATE_PIPE_INSTANCE)
        dprintf_out("\tFILE_CREATE_PIPE_INSTANCE\n");
    if (m & FILE_READ_EA)
        dprintf_out("\tFILE_READ_EA\n");
    if (m & FILE_WRITE_EA)
        dprintf_out("\tFILE_WRITE_EA\n");
    if (m & FILE_EXECUTE)
        dprintf_out("\tFILE_EXECUTE\n");
    if (m & FILE_TRAVERSE)
        dprintf_out("\tFILE_TRAVERSE\n");
    if (m & FILE_DELETE_CHILD)
        dprintf_out("\tFILE_DELETE_CHILD\n");
    if (m & FILE_READ_ATTRIBUTES)
        dprintf_out("\tFILE_READ_ATTRIBUTES\n");
    if (m & FILE_WRITE_ATTRIBUTES)
        dprintf_out("\tFILE_WRITE_ATTRIBUTES\n");
    if ((m & FILE_ALL_ACCESS) == FILE_ALL_ACCESS)
        dprintf_out("\tFILE_ALL_ACCESS\n");
    if ((m & FILE_GENERIC_READ) == FILE_GENERIC_READ)
        dprintf_out("\tFILE_GENERIC_READ\n");
    if ((m & FILE_GENERIC_WRITE) == FILE_GENERIC_WRITE)
        dprintf_out("\tFILE_GENERIC_WRITE\n");
    if ((m & FILE_GENERIC_EXECUTE) == FILE_GENERIC_EXECUTE)
        dprintf_out("\tFILE_GENERIC_EXECUTE\n");
}

void print_nfs_access_mask(int on, int m)
{
    if (!on) return;
    if (!DPRINTF_LEVEL_ENABLED(1))
        return;

    dprintf_out("--> print_nfs_access_mask: %x\n", m);
    if (m & ACE4_READ_DATA)
        dprintf_out("\tACE4_READ_DATA\n");
    if (m & ACE4_LIST_DIRECTORY)
        dprintf_out("\tACE4_LIST_DIRECTORY\n");
    if (m & ACE4_WRITE_DATA)
        dprintf_out("\tACE4_WRITE_DATA\n");
    if (m & ACE4_ADD_FILE)
        dprintf_out("\tACE4_ADD_FILE\n");
    if (m & ACE4_APPEND_DATA)
        dprintf_out("\tACE4_APPEND_DATA\n");
    if (m & ACE4_ADD_SUBDIRECTORY)
        dprintf_out("\tACE4_ADD_SUBDIRECTORY\n");
    if (m & ACE4_READ_NAMED_ATTRS)
        dprintf_out("\tACE4_READ_NAMED_ATTRS\n");
    if (m & ACE4_WRITE_NAMED_ATTRS)
        dprintf_out("\tACE4_WRITE_NAMED_ATTRS\n");
    if (m & ACE4_EXECUTE)
        dprintf_out("\tACE4_EXECUTE\n");
    if (m & ACE4_DELETE_CHILD)
        dprintf_out("\tACE4_DELETE_CHILD\n");
    if (m & ACE4_READ_ATTRIBUTES)
        dprintf_out("\tACE4_READ_ATTRIBUTES\n");
    if (m & ACE4_WRITE_ATTRIBUTES)
        dprintf_out("\tACE4_WRITE_ATTRIBUTES\n");
    if (m & ACE4_DELETE)
        dprintf_out("\tACE4_DELETE\n");
    if (m & ACE4_READ_ACL)
        dprintf_out("\tACE4_READ_ACL\n");
    if (m & ACE4_WRITE_ACL)
        dprintf_out("\tACE4_WRITE_ACL\n");
    if (m & ACE4_WRITE_OWNER)
        dprintf_out("\tACE4_WRITE_OWNER\n");
    if (m & ACE4_SYNCHRONIZE)
        dprintf_out("\tACE4_SYNCHRONIZE\n");
}


void print_nfs41_file_info(
    const char *label,
    const void *vinfo)
{
    const nfs41_file_info *info = vinfo;
    char buf[512];
    char *p;

    buf[0] = '\0';
    p = buf;

#define PRNFS41FI_FMT(str, arg) \
    p += snprintf(p, (sizeof(buf)-(p-buf)), (str), (arg))

    PRNFS41FI_FMT("attrmask.count=%d, ", (int)info->attrmask.count);

    if (info->attrmask.count >= 1) {
        PRNFS41FI_FMT("{ attrmask.arr[0]=0x%x, ", (int)info->attrmask.arr[0]);
        if (info->attrmask.arr[0] & FATTR4_WORD0_TYPE)
            PRNFS41FI_FMT("type=%d, ", (int)(info->type & NFS_FTYPE_MASK));
        if (info->attrmask.arr[0] & FATTR4_WORD0_CHANGE)
            PRNFS41FI_FMT("change=%lld, ", (long long)info->change);
        if (info->attrmask.arr[0] & FATTR4_WORD0_SIZE)
            PRNFS41FI_FMT("size=%lld, ", (long long)info->size);
        if (info->attrmask.arr[0] & FATTR4_WORD0_HIDDEN)
            PRNFS41FI_FMT("hidden=%d, ", (int)info->hidden);
        if (info->attrmask.arr[0] & FATTR4_WORD0_ARCHIVE)
            PRNFS41FI_FMT("archive=%d, ", (int)info->archive);
        p += snprintf(p, (sizeof(buf)-(p-buf)), "} ");
    }
    if (info->attrmask.count >= 2) {
        PRNFS41FI_FMT("{ attrmask.arr[1]=0x%x, ", (int)info->attrmask.arr[1]);
        if (info->attrmask.arr[1] & FATTR4_WORD1_MODE)
            PRNFS41FI_FMT("mode=0x%lx, ", (long)info->mode);
        if (info->attrmask.arr[1] & FATTR4_WORD1_OWNER) {
            EASSERT(info->owner != NULL);
            PRNFS41FI_FMT("owner='%s', ", info->owner);
        }
        if (info->attrmask.arr[1] & FATTR4_WORD1_OWNER_GROUP) {
            EASSERT(info->owner_group != NULL);
            PRNFS41FI_FMT("owner_group='%s', ", info->owner_group);
        }
        if (info->attrmask.arr[1] & FATTR4_WORD1_NUMLINKS)
            PRNFS41FI_FMT("numlinks=%ld, ", (long)info->numlinks);
        if (info->attrmask.arr[1] & FATTR4_WORD1_TIME_ACCESS) {
            p += snprintf(p, (sizeof(buf)-(p-buf)),
                "time_access=(%lds/%ldns), ",
                (long)info->time_access.seconds,
                (long)info->time_access.nseconds);
        }
        if (info->attrmask.arr[1] & FATTR4_WORD1_TIME_CREATE) {
            p += snprintf(p, (sizeof(buf)-(p-buf)),
                "time_create=(%lds/%ldns), ",
                (long)info->time_create.seconds,
                (long)info->time_create.nseconds);
        }
        if (info->attrmask.arr[1] & FATTR4_WORD1_TIME_MODIFY) {
            p += snprintf(p, (sizeof(buf)-(p-buf)),
                "time_modify=(%lds/%ldns), ",
                (long)info->time_modify.seconds,
                (long)info->time_modify.nseconds);
        }
        if (info->attrmask.arr[1] & FATTR4_WORD1_SYSTEM)
            PRNFS41FI_FMT("system=%d, ", (int)info->system);
        p += snprintf(p, (sizeof(buf)-(p-buf)), "} ");
    }

    dprintf_out("%s={ %s }\n", label, buf);
}

#define NUM_RECENTLY_DELETED 128
static struct
{
    SRWLOCK lock;
    void *deleted_ptrs[NUM_RECENTLY_DELETED];
    size_t deleted_index;
} ptr_recently_deleted_data = {
    .lock = SRWLOCK_INIT,
};

bool debug_ptr_was_recently_deleted(void *in_ptr)
{
    size_t i;
    bool_t res = false;

    AcquireSRWLockShared(&ptr_recently_deleted_data.lock);
    for (i=0 ; i < NUM_RECENTLY_DELETED ; i++) {
        if (ptr_recently_deleted_data.deleted_ptrs[i] == in_ptr) {
            res = true;
            break;
        }
    }
    ReleaseSRWLockShared(&ptr_recently_deleted_data.lock);
    return res;
}

void debug_ptr_add_recently_deleted(void *in_ptr)
{
    size_t i;

    AcquireSRWLockExclusive(&ptr_recently_deleted_data.lock);
    i = ptr_recently_deleted_data.deleted_index++ % NUM_RECENTLY_DELETED;
    ptr_recently_deleted_data.deleted_ptrs[i] = in_ptr;
    ReleaseSRWLockExclusive(&ptr_recently_deleted_data.lock);
}

#define NUM_DELAY_FREE 2048
static struct
{
    SRWLOCK lock;
    void *free_ptrs[NUM_DELAY_FREE];
    size_t free_ptrs_index;
} debug_delay_free_data = {
    .lock = SRWLOCK_INIT,
};

void debug_delayed_free(void *in_ptr)
{
    size_t i;

    AcquireSRWLockExclusive(&debug_delay_free_data.lock);
    i = debug_delay_free_data.free_ptrs_index++ % NUM_DELAY_FREE;

    if (debug_delay_free_data.free_ptrs[i])
        free(debug_delay_free_data.free_ptrs[i]);

    debug_delay_free_data.free_ptrs[i] = in_ptr;
    ReleaseSRWLockExclusive(&debug_delay_free_data.lock);
}
