/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Bounded Wine ntdll entry gate.
 *
 * This is the integration milestone for the Wine runtime: map the selected
 * i386 PE modules from the runtime namespace, bind their real import graph,
 * locate the versioned Unix-call boundary inside ntdll, enter one exported
 * ntdll function through the IA-32 DBT and stop at that boundary instead of
 * executing it.
 *
 * The boundary is identified structurally, not guessed. Wine's i386 PE
 * syscall stubs end in "call <__wine_syscall>", and __wine_syscall is the
 * single "jmp dword ptr [__wine_syscall_dispatcher]" thunk that references
 * the exported dispatcher slot. Both the slot and the thunk are located from
 * the mapped image, so the stop point is an address the manifest-hashed
 * ntdll really contains.
 *
 * Nothing here calls a host Wine function, and the gate refuses to widen
 * itself: production Pinball startup does not depend on it.
 */
#ifndef PROSPERO_WIN_PW_WINE_GATE_H
#define PROSPERO_WIN_PW_WINE_GATE_H

#include "pw_export.h"
#include "pw_import_bind.h"
#include "pw_loader.h"
#include "pw_sha256.h"
#include "pw_unix_call.h"
#include "pw_x86_engine.h"
#include "pw_wine_unixlib.h"

enum {
    PW_WINE_GATE_MAX_MODULES = 8,
    PW_WINE_GATE_MAX_BOUNDARIES = 8,
    /* Real ntdll initialization runs far more dispatches than a title's
     * startup: this is the budget one gate run may spend before the step
     * limit stops it, not a statement about what the guest needs. Measured on
     * the pinned runtime, the application-root run needs 97 812 dispatches to
     * reach the next service it does not have (NtInitializeNlsFiles), three
     * times what the previous bound allowed, so the bound is set clear of the
     * measurement instead of at it. */
    PW_WINE_GATE_DEFAULT_STEPS = 262144,
    PW_WINE_GATE_MAX_STEPS = 4194304,
    PW_WINE_GATE_STACK_BYTES = 64u * 1024u,
    /* ntdll's loader path translates far more code than a title's startup:
     * the entry count and the code arena are the gate's own budgets, and
     * running out of either is reported as its own classified stop rather
     * than as a guest fault. Measured on the pinned runtime, the Wine-runtime
     * control needs 968 blocks and the application-root run 1 027 to reach its
     * current frontier while using 660 KB of the arena, so the entry count was
     * what bound the run and this bound is set clear of the measured
     * requirement instead of at it. */
    PW_WINE_GATE_CACHE_ENTRIES = 4096,
    PW_WINE_GATE_ARENA_BYTES = 4u * 1024u * 1024u,
    /* Measured: the application-root run needs 67 serviced calls to reach the
     * service it does not have next (NtGetNextThread, i386 syscall 0x00a0), so
     * the default is clear of the requirement rather than at it - the same rule
     * the cache, step and region bounds follow. */
    PW_WINE_GATE_DEFAULT_CALLS = 1024,
    PW_WINE_GATE_MAX_CALL_REGIONS = 16,
    /* Image sections one run may have alive at once. A process maps its
     * modules one at a time and closes each section once its view exists, so
     * this is a budget with room rather than a guess about the graph. */
    PW_WINE_GATE_MAX_SECTIONS = 4,
    PW_WINE_GATE_DEFAULT_ALLOCATION_LIMIT = 4u * 1024u * 1024u,
    /* Where the NT allocator hands out guest memory that ntdll asks for. */
    PW_WINE_GATE_HEAP_BASE = 0x20000000u,
    PW_WINE_GATE_HEAP_LIMIT = 0x30000000u,
    PW_WINE_GATE_MAX_HANDLES = 16,
    PW_WINE_GATE_MAX_PATH = 160,
    PW_WINE_GATE_MAX_READ = 64u * 1024u,
    PW_WINE_GATE_MAX_VALUE = 4096,
};

/*
 * Platform file service below the Unix-call boundary. The gate translates a
 * guest DOS/NT path to a name inside the configured runtime distribution and
 * then asks this service to open, read and close it; the gate itself never
 * touches a host file system call. Everything a handler reads or writes in
 * guest memory still goes through the dispatcher's validated accessor.
 */
typedef enum PwWineFileStatus {
    PW_WINE_FILE_OK = 0,
    PW_WINE_FILE_NOT_FOUND = 1,
    PW_WINE_FILE_DENIED = 2,
    PW_WINE_FILE_ERROR = 3,
} PwWineFileStatus;

typedef struct PwWineFileService {
    void *context;
    /* name is a canonical lower-case file name; the namespace says which root
     * it belongs to, because the same name can exist in both and the loader's
     * own rule keeps them apart: the process's own image and the modules next
     * to it come from the application directory, system modules from the
     * runtime distribution. */
    PwWineFileStatus (*open)(void *context, PwFileNamespace file_namespace,
                             const char *name, uint64_t *size, void **token);
    PwWineFileStatus (*read)(void *context, void *token, uint64_t offset,
                             void *bytes, uint32_t size,
                             uint32_t *read_bytes);
    void (*close)(void *context, void *token);
} PwWineFileService;

/*
 * Platform registry service below the Unix-call boundary. Wine's registry is
 * host state: the gate translates a guest NT key path into a canonical,
 * lower-case path, validates every component and owns the key handles, and
 * this service decides what exists. A key the service does not know is
 * answered with STATUS_OBJECT_NAME_NOT_FOUND, which is what makes ntdll fall
 * back to its own defaults - the gate never invents content, and it never
 * passes the service a path the guest did not name inside the registry
 * namespace.
 */
typedef enum PwWineRegistryStatus {
    PW_WINE_REGISTRY_OK = 0,
    PW_WINE_REGISTRY_NOT_FOUND = 1,
    PW_WINE_REGISTRY_DENIED = 2,
    PW_WINE_REGISTRY_ERROR = 3,
} PwWineRegistryStatus;

typedef struct PwWineRegistryService {
    void *context;
    /* path is a canonical lower-case NT key path: always "\registry\..." and
     * already validated by the gate. */
    PwWineRegistryStatus (*open)(void *context, const char *path,
                                 void **token);
    /* NtCreateKey is create-or-open in NT: a profile without a writable hive
     * answers NOT_FOUND for a key it does not declare, and opens the one it
     * has with *created = 0. */
    PwWineRegistryStatus (*create)(void *context, const char *path,
                                   void **token, uint32_t *created);
    /* value is a canonical lower-case value name; "" is the key's default
     * value. On PW_WINE_REGISTRY_OK the service points *bytes at its own
     * storage, which the gate copies into guest memory through the validated
     * accessor; *size is bounded by PW_WINE_GATE_MAX_VALUE. */
    PwWineRegistryStatus (*query)(void *context, void *token,
                                  const char *value, uint32_t *type,
                                  const void **bytes, uint32_t *size);
    /* NtSetValueKey: store one value on the key the token names, replacing the
     * value of that name if the key already has one. The name is canonical and
     * lower-case ("" is the key's default value) and the bytes are the value's
     * own data, already validated and bounded by the gate; a service that
     * cannot store it answers DENIED or ERROR rather than pretending the value
     * is there. */
    PwWineRegistryStatus (*set_value)(void *context, void *token,
                                      const char *value, uint32_t type,
                                      const void *bytes, uint32_t size);
    void (*close)(void *context, void *token);
} PwWineRegistryService;

/*
 * Platform object-namespace service. Wine's loader opens directories and
 * sections by NT object path (`\KnownDlls`, `\KnownDlls\kernel32.dll`), so
 * this is where the host says what the namespace contains. A name the profile
 * does not declare answers NOT_FOUND, which is what makes the loader fall back
 * to loading the module from the file system - the same thing Wine does on a
 * prefix without known DLLs - and the gate never passes the service a path the
 * guest did not name inside the namespace.
 */
typedef enum PwWineObjectStatus {
    PW_WINE_OBJECT_OK = 0,
    PW_WINE_OBJECT_NOT_FOUND = 1,
    PW_WINE_OBJECT_DENIED = 2,
    PW_WINE_OBJECT_ERROR = 3,
} PwWineObjectStatus;

typedef enum PwWineObjectKind {
    PW_WINE_OBJECT_DIRECTORY = 1,
    PW_WINE_OBJECT_SECTION = 2,
} PwWineObjectKind;

typedef struct PwWineObjectService {
    void *context;
    /* path is a canonical lower-case NT object path starting with '\'. */
    PwWineObjectStatus (*open)(void *context, PwWineObjectKind kind,
                               const char *path, void **token);
    void (*close)(void *context, void *token);
} PwWineObjectService;

typedef enum PwWineStop {
    PW_WINE_STOP_NONE = 0,
    PW_WINE_STOP_UNIX_CALL_BOUNDARY = 1,
    PW_WINE_STOP_UNSUPPORTED_INSTRUCTION = 2,
    PW_WINE_STOP_MEMORY_BOUNDS = 3,
    PW_WINE_STOP_CACHE_LIMIT = 4,
    PW_WINE_STOP_NON_CODE = 5,
    PW_WINE_STOP_DECODE_FAILURE = 6,
    PW_WINE_STOP_X87_TRAP = 7,
    PW_WINE_STOP_STEP_BUDGET = 8,
    PW_WINE_STOP_RETURNED_TO_CALLER = 9,
    PW_WINE_STOP_GATE_ERROR = 10,
    PW_WINE_STOP_UNIX_CALL_UNIMPLEMENTED = 11,
    PW_WINE_STOP_UNIX_CALL_UNKNOWN = 12,
    PW_WINE_STOP_UNIX_CALL_REJECTED = 13,
    PW_WINE_STOP_PROCESS_TERMINATED = 14,
    /* The second dispatcher: reached with the Unix-call bridge disabled, and
     * the outcome of a call the bridge refused at that boundary. The specific
     * reason lives in the tally's last_status, because an unknown handle, an
     * unknown code and an unreadable frame are different facts that must not
     * be collapsed into one. */
    PW_WINE_STOP_UNIXLIB_BOUNDARY = 15,
    PW_WINE_STOP_UNIXLIB_REFUSED = 16,
    PW_WINE_STOP_UNIXLIB_UNIMPLEMENTED = 17,
} PwWineStop;

/*
 * What the second dispatcher did, kept apart from the NT-syscall tallies: a
 * guest reaching one boundary tells you nothing about the other.
 */
typedef struct PwUnixlibTally {
    uint64_t serviced;              /* calls answered with a status */
    uint64_t unknown_handle;        /* the handle was not this run's */
    uint64_t unknown_code;          /* outside the pinned eight-entry table */
    uint64_t malformed;             /* frame, params or span refused */
    uint64_t service_failed;        /* the injected sink refused */
    uint64_t unimplemented;         /* a pinned call this bridge does not serve */
    uint64_t unsupported;           /* answered with a documented failure */
    uint64_t debug_bytes;           /* bytes delivered to the debug sink */
    uint32_t last_code;
    uint32_t last_status;           /* PwWineUnixlibStatus */
    /* Provenance of the last call, read from the frame the guest left: the
     * return PC inside the caller and the args pointer it passed. The evidence
     * names the call site; it never prints guest bytes. */
    uint32_t last_return_pc;
    uint32_t last_args;
} PwUnixlibTally;

typedef struct PwWineModuleRecord {
    char name[PW_MODULE_NAME_MAX + 1];
    char path[PW_PATH_MAX + 1];
    char sha256[PW_SHA256_HEX_BYTES];
    uint32_t size;
    uint32_t machine;
    uint32_t base;                  /* guest load address */
    uint32_t image_bytes;           /* mapped span of the module */
    uint32_t imports;
    uint32_t exports_named;
    uint32_t exports_ordinal;
    uint8_t loaded;
    uint8_t tls_present;
    uint8_t runtime;
    uint8_t origin_application;     /* fingerprint came from PW_FILE_APPLICATION */
} PwWineModuleRecord;

typedef struct PwWineGateConfig {
    const PwFileProvider *provider;
    const PwVmBackend *backend;
    /*
     * The run's own workspace (loader, import-bind workspace, export resolver
     * and translation cache). It used to live in function-local statics inside
     * pw_wine_gate_run, which made two runs in one process share mutable state;
     * the caller owns it now, so a second runner is a second object and the
     * gate has no hidden state. Required: a run with no runner, or with one
     * that was released, is refused.
     */
    struct PwWineRunner *runner;
    const PwWineFileService *files;  /* NULL refuses every open */
    const PwWineRegistryService *registry;  /* NULL answers NOT_SUPPORTED */
    const PwWineObjectService *objects;     /* NULL answers NOT_SUPPORTED */
    /*
     * The version block of the distribution being run, in the exact shape
     * Wine answers SystemWineVersionInformation with: four NUL-terminated
     * strings (version, build id, host system name, host release) packed
     * together. The host derives it from the staged distribution, so the
     * guest is told the version of the modules it is actually executing; a
     * NULL or empty value makes the gate answer STATUS_NOT_SUPPORTED instead
     * of inventing one.
     */
    const char *wine_version_info;
    uint32_t wine_version_info_bytes;
    /*
     * The SID of the token the process runs as, as the host declares it. It is
     * what RtlFormatCurrentUserKeyPath turns into \Registry\User\<SID> before
     * it opens HKCU, so the gate answers NtQueryInformationToken(TokenUser)
     * with it; a NULL or empty value makes that class answer
     * STATUS_NOT_SUPPORTED rather than inventing an identity.
     */
    const uint8_t *token_user_sid;
    uint32_t token_user_sid_bytes;
    /* Optional per-dispatch trace, so a mode difference can be localised to
     * the block that produced it. */
    void (*trace)(void *context, const PwX86State *state);
    void *trace_context;
    const char *root_module;        /* default "kernelbase.dll" */
    const char *entry_module;       /* default "ntdll.dll" */
    const char *entry_symbol;       /* default "NtClose" */
    const char *dispatcher_symbol;  /* default "__wine_syscall_dispatcher" */
    /* Optional application process parameters. NULL retains the bounded
     * defaults used by the ntdll control and generated fixture. */
    const char *process_image_path;
    const char *process_current_directory;
    const char *process_application_directory;
    const char *process_command_line;
    const char *modules[PW_WINE_GATE_MAX_MODULES];
    uint32_t module_count;
    uint32_t step_budget;           /* 0 uses PW_WINE_GATE_DEFAULT_STEPS */
    uint32_t stack_base;            /* 0 lets the gate choose */
    uint8_t chaining;               /* DBT mode toggles, for parity evidence */
    uint8_t residency;
    uint8_t lazy_flags;
    uint8_t modes_set;              /* 0 keeps the engine defaults */
    /*
     * Where the root module comes from. 0 keeps the Wine-runtime control (the
     * root is a system module read from PW_FILE_RUNTIME); 1 says the root is an
     * application image and must come from PW_FILE_APPLICATION. There is no
     * fallback between the two namespaces: a namespace that does not hold the
     * name fails the run rather than quietly trying the other one.
     */
    uint8_t root_application;
    uint8_t bridge_calls;           /* service Unix calls instead of stopping */
    /* Publish and service the second dispatcher:
     * __wine_unix_call_dispatcher / __wine_unixlib_handle. Off by default, so
     * the NT-syscall control run is byte-for-byte what it was. */
    uint8_t unixlib_calls;
    /* The host side of unix_wine_dbg_write; NULL makes that call a service
     * failure rather than a silent success. */
    const PwWineDebugSink *debug_sink;
    uint32_t call_budget;           /* 0 uses PW_WINE_GATE_DEFAULT_CALLS */
    uint32_t allocation_limit;      /* bytes one run may allocate; 0 = default */
} PwWineGateConfig;

typedef struct PwWineGateReport {
    uint32_t module_count;
    PwWineModuleRecord modules[PW_WINE_GATE_MAX_MODULES];
    uint32_t bound_functions;
    uint32_t bound_data;
    uint32_t bound_modules;
    uint32_t bind_failures;
    uint32_t tls_modules;
    uint32_t boundary_count;
    uint32_t boundary_slot_rva;
    uint32_t boundary_slot_va;
    uint32_t boundary_thunk_rva;
    uint32_t boundary_thunk_va;
    /* The second boundary: where the guest lands when it calls through
     * __wine_unix_call_dispatcher, and the opaque handle published in
     * __wine_unixlib_handle. Zero when the second dispatcher is disabled. */
    uint32_t unixlib_boundary_va;
    uint32_t unixlib_dispatcher_slot_va;
    uint32_t unixlib_handle;
    uint32_t unixlib_handle_slot_va;
    PwUnixlibTally unixlib;
    uint32_t entry_rva;
    uint32_t entry_eip;             /* initial guest EIP */
    uint32_t entry_pe_rva;          /* the module's own entry point */
    /*
     * ntdll's own RtlUserThreadStart, which is what the kernel puts in the
     * first thread's context as its Eip (dlls/ntdll/unix/signal_i386.c:2474)
     * and what NtContinue therefore installs. Zero when the entry module does
     * not export it - a synthetic fixture with an entry of its own - in which
     * case the context carries that zero rather than an invented address.
     */
    uint32_t thread_start_rva;
    uint32_t thread_start_eip;
    /*
     * The transfer address the first thread is started with: the root image's
     * own entry point, which is what the unix side hands signal_start_thread
     * as the thread's start routine (dlls/ntdll/unix/server.c:1780,
     * `main_image_info.TransferAddress`), with the PEB as that thread's
     * argument. Zero when the root image has no entry point.
     */
    uint32_t main_entry_eip;
    uint32_t stub_syscall_id;       /* decoded from the stub's first bytes */
    uint32_t observed_syscall_id;   /* EAX when the boundary was reached */
    uint32_t boundary_return_eip;   /* guest return address at the boundary */
    uint32_t caller_stub_id;        /* id re-read from the stub that called it */
    uint32_t caller_stub_rva;
    uint8_t boundary_return_in_module;
    uint32_t first_eip;
    uint32_t last_eip;
    uint32_t stop_address;
    uint32_t fault_address;         /* memory-bounds identity, when reported */
    uint32_t fault_width;
    uint32_t fault_write;
    /* Which block faulted and how its registers were resident, so a fault the
     * engine caused is told apart from one the guest did. */
    uint32_t fault_block_pc;
    uint32_t fault_block_instructions;
    uint8_t fault_block_resident_mask;
    int8_t fault_block_map[8];
    uint8_t fault_block_code[1024];
    uint32_t fault_block_code_bytes;
    /* The call the run ended on, with its arguments: a stop that names its
     * status without naming the call that asked for it says half of it. */
    uint32_t stop_call_id;
    uint32_t stop_call_args[6];
    /*
     * The call that ended the process and the status it named: NtTerminateThread
     * or NtTerminateProcess, with the value the guest passed as the exit code.
     * For the generated application this is its own entry point's return value,
     * because that is what kernel32's BaseThreadInitThunk hands to
     * RtlExitUserThread.
     */
    uint32_t exit_call_id;
    uint32_t exit_status;
    uint32_t chaining;
    uint32_t residency;
    uint32_t lazy_flags;
    uint32_t stack_base;
    uint32_t stack_bytes;
    uint32_t guest_regions;         /* declared DBT memory regions */
    uint32_t low_exhausted;         /* a low reservation ran out of candidates */
    uint32_t teb_base;              /* minimal guest TEB (FS base) */
    uint32_t teb_bytes;
    uint32_t peb_base;              /* minimal guest PEB (first argument) */
    uint32_t parameters_base;       /* zeroed process-parameters page */
    uint32_t parameters_length;     /* bytes of the populated structure */
    PwUnixCallTally calls;
    uint32_t calls_serviced;
    uint32_t allocations;
    uint32_t releases;              /* NtFreeVirtualMemory releases */
    uint32_t allocated_bytes;
    uint32_t call_regions;          /* guest regions this run mapped for NT */
    /* file_opens counts every NtOpenFile the gate answered with a handle;
     * file_directories counts how many of those were the gate-owned Windows
     * directory, which has no platform token behind it. */
    uint64_t file_opens;
    uint64_t file_reads;
    uint64_t file_bytes;
    uint64_t file_closes;
    uint64_t file_refusals;
    uint32_t file_handles;
    uint64_t file_directories;
    /* NtFsControlFile answers this run gave (FSCTL_GET_OBJECT_ID). */
    uint64_t file_fs_controls;
    /* Image sections this run created, described and refused. */
    uint64_t section_creates;
    uint64_t section_queries;
    uint64_t section_refusals;
    uint64_t section_views;         /* views this run mapped from a section */
    uint64_t section_view_refusals;
    uint64_t section_protects;      /* protections this run installed */
    uint64_t section_protect_refusals;
    /* NtInitializeNlsFiles: the mappings it handed out and the answers it
     * refused. */
    uint64_t nls_maps;
    uint64_t nls_refusals;
    /* NtQueryDefaultUILanguage and NtQueryDefaultLocale answers. */
    uint64_t locale_queries;
    uint32_t files_configured;
    char last_file[PW_WINE_GATE_MAX_PATH + 1];
    uint64_t key_opens;
    uint64_t key_creates;
    uint64_t key_queries;
    uint64_t key_values;            /* queries answered with a value */
    uint64_t key_sets;              /* values NtSetValueKey stored */
    char last_value[PW_WINE_GATE_MAX_PATH + 1];
    uint64_t key_refusals;
    uint32_t registry_configured;
    char last_key[PW_WINE_GATE_MAX_PATH + 1];
    uint64_t token_queries;
    uint64_t process_queries;
    uint32_t process_image_characteristics;
    /* NtQueryVirtualMemory answers this run gave about its own mappings. */
    uint64_t virtual_queries;
    /* NtAreMappedFilesTheSame: how many address pairs the run compared, and
     * how many of those did not describe a file view this run owns. */
    uint64_t address_comparisons;
    uint64_t address_comparison_refusals;
    /* The run's own thread list: how many times the loader asked for the next
     * thread, how many handles that handed out, how many ThreadBasicInformation
     * answers were given, and how many thread questions were refused. */
    uint64_t thread_enumerations;
    uint64_t thread_handles;
    uint64_t thread_queries;
    uint64_t thread_refusals;
    /* NtContinue: how many times the run installed a thread's own context, and
     * how many of those contexts it refused (a segment this run cannot run in,
     * or state it could not read). */
    uint64_t context_restores;
    uint64_t context_restore_refusals;
    uint64_t object_opens;
    uint64_t object_refusals;
    uint32_t objects_configured;
    char last_object[PW_WINE_GATE_MAX_PATH + 1];
    uint64_t dispatches;
    uint64_t retired;
    uint64_t translated_blocks;
    uint64_t translated_bytes;
    uint64_t register_loads;
    uint64_t register_stores;
    uint64_t register_reconciliations;
    uint64_t register_spills;
    uint64_t host_calls;            /* host Wine functions called: must be 0 */
    uint32_t cleanup_modules;
    uint32_t cleanup_mappings;
    uint32_t cleanup_translations;
    /* What cleanup did not manage to give back, and how many teardown actions
     * failed. Unit-level owners remain retryable; this one-shot gate propagates
     * a persistent failure through status as well as through the evidence. */
    uint32_t cleanup_modules_pending;
    uint32_t cleanup_process_pages_pending;
    uint32_t cleanup_call_regions_pending;
    uint32_t cleanup_translations_pending;
    uint32_t cleanup_failures;
    /* The pipeline stage that was executing when the run stopped. */
    const char *gate_stage;
    /*
     * The last dispatched guest PCs, newest last, bounded. A fault has to be
     * explainable without guessing a stack frame: this is the engine's own
     * provenance, and it names the code that reached the failing instruction.
     */
    uint32_t recent_pcs[16];
    uint32_t recent_count;
    PwWineStop stop;
    int status;
} PwWineGateReport;

int pw_wine_gate_run(const PwWineGateConfig *config, PwWineGateReport *report);

const char *pw_wine_stop_name(PwWineStop stop);

/* True when the recorded stop is the accepted acceptance condition. */
int pw_wine_stop_is_acceptance(PwWineStop stop);

#endif
