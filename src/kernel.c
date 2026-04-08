#include <stdint.h>
#include <stddef.h>

static volatile uint16_t* terminal_buffer = (uint16_t*)0xB8000;
static const uint8_t VGA_WIDTH = 80;
static const uint8_t VGA_HEIGHT = 25;

#define SHELL_INPUT_MAX 128
#define SHELL_HISTORY_WIDTH SHELL_INPUT_MAX
#define SHELL_HISTORY_MAX 8
#define INPUT_EVENT_LOG_MAX 16
#define GUI_TERM_LOG_LINES 128
#define GUI_TERM_LOG_COLS 120
#define RAMFS_MAX_FILES 32
#define RAMFS_NAME_MAX 32
#define RAMFS_FILE_MAX 65536
#define DISKFS_SECTOR_SIZE 512u
#define MBFS_PATH_MAX SHELL_INPUT_MAX
#define MBFS_NAME_MAX 32
#define MBFS_MAX_INODES 64u
#define MBFS_MAX_DIRENTS 64u
#define MBFS_INODE_EXTENTS 4u
#define MBFS_INODE_TABLE_SECTORS 8u
#define MBFS_DIR_SECTORS 8u
#define MBFS_BITMAP_SECTORS 16u
#define PROCESS_FD_MAX 8
#define PROCESS_USER_PAGE_TRACK_MAX 256
#define RAMFS_OPEN_READ  0x1u
#define RAMFS_OPEN_WRITE 0x2u
#define FD_BACKEND_RAMFS 0u
#define FD_BACKEND_MBFS  1u

/* Forward declare MBFS dir entry for pointer prototypes and string helpers
 * prototypes early so they don't get implicitly declared when used.
 */
struct mbfs_dir_entry;
static int str_equals(const char* a, const char* b);
static void str_copy_bounded(char* dst, const char* src, size_t dst_size);
static int str_starts_with(const char* s, const char* prefix);
static int  diskfs_get_entries(const char* path, struct mbfs_dir_entry* out, uint32_t max_count, uint32_t* out_count);
/* Forward declarations for terminal helpers used by early debug logging */
static void terminal_write(const char* str);
static void terminal_write_dec_u32(uint32_t value);
/* Forward declarations for serial helpers used by debug logging */
static void serial_write(const char* s);
static void serial_write_dec_u32(uint32_t value);
extern uint8_t serial_enabled;

#define ATA_REG_DATA       0u
#define ATA_REG_SECCOUNT0  2u
#define ATA_REG_LBA0       3u
#define ATA_REG_LBA1       4u
#define ATA_REG_LBA2       5u
#define ATA_REG_HDDEVSEL   6u
#define ATA_REG_COMMAND    7u
#define ATA_REG_STATUS     7u

#define ATA_SR_ERR  0x01u
#define ATA_SR_DRQ  0x08u
#define ATA_SR_DF   0x20u
#define ATA_SR_DRDY 0x40u
#define ATA_SR_BSY  0x80u

#define ATA_CMD_IDENTIFY     0xECu
#define ATA_CMD_READ_SECTORS 0x20u
#define ATA_CMD_WRITE_SECTORS 0x30u
#define ATA_CMD_CACHE_FLUSH  0xE7u

#define MBFS_INODE_TYPE_FILE 1u
#define MBFS_INODE_TYPE_DIR  2u
#define MBFS_SUBDIR_SECTORS  8u
#define MBFS_MAGIC 0x3153464Du
#define MBFS_VERSION 1u
#define MBFS_SUPER_LBA 1u
#define MBFS_INODE_TABLE_LBA 2u
#define MBFS_DIR_LBA (MBFS_INODE_TABLE_LBA + MBFS_INODE_TABLE_SECTORS)
#define MBFS_BITMAP_LBA (MBFS_DIR_LBA + MBFS_DIR_SECTORS)
#define MBFS_DATA_LBA (MBFS_BITMAP_LBA + MBFS_BITMAP_SECTORS)

static size_t terminal_row = 0;
static size_t terminal_col = 0;
static uint8_t terminal_color = 0x0F;
static char shell_input_boot[SHELL_INPUT_MAX];
static char* shell_input = shell_input_boot;
static size_t shell_input_len = 0;
static size_t shell_input_cursor = 0;
static size_t shell_input_render_len = 0;
static size_t shell_input_origin_row = 0;
static size_t shell_input_origin_col = 0;
static uint8_t keyboard_extended = 0;
static volatile uint32_t timer_ticks = 0;
static volatile uint32_t syscall_count = 0;
static volatile uint32_t syscall_last_num = 0;
static volatile uint32_t syscall_last_arg0 = 0;
static volatile uint32_t syscall_last_ret = 0;
static volatile uint32_t user_exit_count = 0;
static volatile uint32_t user_last_exit_code = 0;
static volatile uint32_t input_event_count = 0;
static volatile uint32_t input_last_scancode = 0;
static volatile uint32_t input_last_ascii = 0;
static volatile uint32_t input_last_woke = 0;
static uint8_t input_event_bridge_enabled = 1;
static uint32_t input_event_channel = 1;

struct input_event_record {
    uint32_t tick;
    uint8_t scancode;
    uint8_t ascii;
    uint8_t woke;
};

static struct input_event_record input_event_log[INPUT_EVENT_LOG_MAX];
static uint32_t input_event_log_head = 0;
static uint32_t input_event_log_count = 0;
static const uint32_t timer_hz = 100;
static uint32_t boot_multiboot_magic = 0;

static volatile uint32_t mouse_x = 512;
static volatile uint32_t mouse_y = 384;
static volatile uint8_t mouse_buttons = 0;
static volatile uint8_t mouse_enabled = 0;
static volatile uint8_t wm_render_pending = 0;
static volatile uint32_t wm_cursor_prev_x = 512;
static volatile uint32_t wm_cursor_prev_y = 384;
static volatile uint8_t wm_cursor_front_valid = 0;
static uint8_t mouse_cycle = 0;
/* Start menu state: toggled by clicking the "MBos" label on the taskbar */
static uint8_t wm_start_menu_visible = 0;
static const uint32_t wm_start_menu_width = 220u;
static const uint32_t wm_start_menu_height = 300u;
/* Pending GUI command (deferred out of IRQ) */
static volatile uint8_t gui_pending_cmd_ready = 0;
static char gui_pending_cmd[SHELL_INPUT_MAX];
/* Pending GUI action from IRQ (processed in main loop) */
enum gui_action_type {
    GUI_ACTION_NONE = 0,
    GUI_ACTION_OPEN_TERMINAL = 1,
    GUI_ACTION_OPEN_FILEMANAGER = 2
};
static volatile uint8_t gui_pending_action = GUI_ACTION_NONE;
static char gui_pending_action_arg[MBFS_PATH_MAX];
static uint8_t mouse_packet[3] = {0, 0, 0};
static volatile uint32_t last_status_report_tick = 0;
 static uint8_t auto_simulate_click = 0; /* automatic GUI click sequence for headless tests (disabled by default) */
static uint8_t auto_simclick_step = 0;
static uint32_t auto_next_click_tick = 0;
static uint8_t auto_simclick_dumped = 0;
static char gui_term_log[GUI_TERM_LOG_LINES][GUI_TERM_LOG_COLS + 1];
static uint32_t gui_term_head = 0;
static uint32_t gui_term_count = 1;
static uint32_t gui_term_col = 0;

/* Debug event log to trace GUI/input interactions (non-blocking). */
#define DBG_EVENT_LOG_MAX 128
enum dbg_event_type {
    DBG_EV_NONE = 0,
    DBG_EV_MOUSE_CLICK = 1,
    DBG_EV_GUI_PENDING_ACTION = 2,
    DBG_EV_APP_SPAWN = 3,
    DBG_EV_IRQ_KEY_TO_APP = 4,
    DBG_EV_MAINLOOP_PROCESS_ACTION = 5,
    DBG_EV_APP_PROC_STEP = 6,
    DBG_EV_WM_FOCUS = 7
};
struct dbg_event {
    uint32_t tick;
    uint32_t type;
    int32_t arg0;
    int32_t arg1;
};
static struct dbg_event dbg_log[DBG_EVENT_LOG_MAX];
static uint32_t dbg_log_head = 0;
static uint32_t dbg_log_count = 0;
/* When non-zero, emit dbg events immediately to serial. Default 0
 * to avoid flooding the serial console during automated tests. */
static uint8_t dbg_emit_serial = 0;
/* When non-zero at boot, automatically run the allocator stress-test
 * once during startup (development convenience). Disable for normal
 * runs once debugging is complete. */
static uint8_t auto_run_allocstress = 0;
/* When non-zero at boot, automatically run quick disk checks and a
 * small smoke-test (development convenience). Disable for normal runs. */
static uint8_t auto_run_disk_tests = 1; /* enabled temporarily to reproduce disk write crash */
static void dbg_log_event(uint32_t type, int32_t a0, int32_t a1)
{
    uint32_t idx = dbg_log_head;
    dbg_log[idx].tick = timer_ticks;
    dbg_log[idx].type = type;
    dbg_log[idx].arg0 = a0;
    dbg_log[idx].arg1 = a1;
    dbg_log_head = (dbg_log_head + 1u) % DBG_EVENT_LOG_MAX;
    if (dbg_log_count < DBG_EVENT_LOG_MAX) { dbg_log_count++; }
    /* Optionally emit immediately to serial; disabled by default to
     * prevent high-frequency timer/irq debug events from flooding the
     * serial console during automated runs. Enable `dbg_emit_serial`
     * for manual debugging when needed. */
    if (dbg_emit_serial && serial_enabled) {
        serial_write("EV T="); serial_write_dec_u32(dbg_log[idx].tick);
        serial_write(" TY="); serial_write_dec_u32(type);
        serial_write(" A0="); serial_write_dec_u32((uint32_t)a0);
        serial_write(" A1="); serial_write_dec_u32((uint32_t)a1);
        serial_write("\n");
    }
}
static void dbg_dump_log(void)
{
    terminal_write("--- DBG LOG ---\n");
    uint32_t oldest = (dbg_log_head + DBG_EVENT_LOG_MAX - dbg_log_count) % DBG_EVENT_LOG_MAX;
    for (uint32_t i = 0; i < dbg_log_count; i++) {
        uint32_t idx = (oldest + i) % DBG_EVENT_LOG_MAX;
        terminal_write("T="); terminal_write_dec_u32(dbg_log[idx].tick);
        terminal_write(" TYP="); terminal_write_dec_u32(dbg_log[idx].type);
        terminal_write(" A0="); terminal_write_dec_u32((uint32_t)dbg_log[idx].arg0);
        terminal_write(" A1="); terminal_write_dec_u32((uint32_t)dbg_log[idx].arg1);
        terminal_write("\n");
    }
    terminal_write("--- END LOG ---\n");
}

/* Simple in-kernel app/process model used for GUI programs (terminal) */
#define APP_PROC_MAX 8
#define APP_PROC_STDIN_MAX 128
#define APP_PROC_STDOUT_MAX 4096

struct app_proc {
    uint8_t used;
    char name[16];
    void (*step)(int pid);
    void* arg;
    char stdin_buf[APP_PROC_STDIN_MAX];
    uint16_t stdin_head;
    uint16_t stdin_tail;
    char stdout_buf[APP_PROC_STDOUT_MAX];
    uint32_t stdout_head;
    uint32_t stdout_tail;
    uint8_t started;
};

static struct app_proc app_procs[APP_PROC_MAX];

static int app_proc_spawn(const char* name, void (*step)(int pid), void* arg)
{
    for (int i = 0; i < APP_PROC_MAX; i++) {
        if (!app_procs[i].used) {
            app_procs[i].used = 1;
            app_procs[i].step = step;
            app_procs[i].arg = arg;
            app_procs[i].stdin_head = app_procs[i].stdin_tail = 0;
            app_procs[i].stdout_head = app_procs[i].stdout_tail = 0;
            app_procs[i].started = 0;
            str_copy_bounded(app_procs[i].name, name, sizeof(app_procs[i].name));
            dbg_log_event(DBG_EV_APP_SPAWN, i, 0);
            return i;
        }
    }
    return -1;
}

static void app_proc_enqueue_stdin_char(int pid, char c)
{
    if (pid < 0 || pid >= APP_PROC_MAX) { return; }
    struct app_proc* p = &app_procs[pid];
    if (!p->used) { return; }
    uint16_t next = (uint16_t)((p->stdin_tail + 1) % APP_PROC_STDIN_MAX);
    if (next == p->stdin_head) {
        /* full: drop oldest */
        p->stdin_head = (uint16_t)((p->stdin_head + 1) % APP_PROC_STDIN_MAX);
    }
    p->stdin_buf[p->stdin_tail] = c;
    p->stdin_tail = next;
}

static void app_proc_enqueue_stdin_line(int pid, const char* s)
{
    if (pid < 0 || pid >= APP_PROC_MAX) { return; }
    while (*s) { app_proc_enqueue_stdin_char(pid, *s++); }
    app_proc_enqueue_stdin_char(pid, '\n');
}

static int app_proc_readline(int pid, char* out, uint32_t max)
{
    if (pid < 0 || pid >= APP_PROC_MAX) { return 0; }
    struct app_proc* p = &app_procs[pid];
    if (!p->used) { return 0; }
    uint32_t idx = p->stdin_head;
    uint32_t scanned = 0;
    while (idx != p->stdin_tail && scanned + 1 < max) {
        char c = p->stdin_buf[idx];
        if (c == '\n') {
            /* pop up to and including newline */
            uint32_t out_i = 0;
            while (p->stdin_head != (uint16_t)((idx + 1) % APP_PROC_STDIN_MAX) && out_i + 1 < max) {
                out[out_i++] = p->stdin_buf[p->stdin_head];
                p->stdin_head = (uint16_t)((p->stdin_head + 1) % APP_PROC_STDIN_MAX);
            }
            if (out_i > 0 && out[out_i - 1] == '\n') { out_i--; }
            out[out_i] = '\0';
            return (int)out_i;
        }
        idx = (uint32_t)((idx + 1) % APP_PROC_STDIN_MAX);
        scanned++;
    }
    return 0;
}

static void app_proc_write_stdout(int pid, const char* s)
{
    if (pid < 0 || pid >= APP_PROC_MAX) { return; }
    struct app_proc* p = &app_procs[pid];
    if (!p->used) { return; }
    while (*s) {
        p->stdout_buf[p->stdout_tail] = *s++;
        p->stdout_tail = (p->stdout_tail + 1) % APP_PROC_STDOUT_MAX;
        if (p->stdout_tail == p->stdout_head) {
            p->stdout_head = (p->stdout_head + 1) % APP_PROC_STDOUT_MAX;
        }
    }
    wm_render_pending = 1;
}

static uint32_t app_proc_copy_stdout(int pid, char* dest, uint32_t max)
{
    if (pid < 0 || pid >= APP_PROC_MAX) { if (max > 0) dest[0] = '\0'; return 0; }
    struct app_proc* p = &app_procs[pid];
    if (!p->used) { if (max > 0) dest[0] = '\0'; return 0; }
    uint32_t avail = (p->stdout_tail + APP_PROC_STDOUT_MAX - p->stdout_head) % APP_PROC_STDOUT_MAX;
    uint32_t i = 0;
    uint32_t idx = p->stdout_head;
    while (i + 1 < max && idx != p->stdout_tail) {
        dest[i++] = p->stdout_buf[idx];
        idx = (idx + 1) % APP_PROC_STDOUT_MAX;
    }
    dest[i] = '\0';
    return i;
}

static void app_proc_run_all(void)
{
    for (int i = 0; i < APP_PROC_MAX; i++) {
        if (app_procs[i].used && app_procs[i].step) {
            dbg_log_event(DBG_EV_APP_PROC_STEP, i, 0);
            app_procs[i].step(i);
        }
    }
}

/* terminal_app_step moved later so MBFS types are defined before use */

struct ramfs_file {
    uint8_t used;
    char name[RAMFS_NAME_MAX];
    uint32_t size;
    uint8_t* data;
};

static struct ramfs_file ramfs_files[RAMFS_MAX_FILES];
static uint32_t ramfs_file_count = 0;

struct ata_device {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t slave;
    uint8_t present;
    uint8_t atapi;
    uint32_t total_sectors;
};

struct mbfs_superblock {
    uint32_t magic;
    uint32_t version;
    uint32_t total_sectors;
    uint32_t inode_table_lba;
    uint32_t inode_table_sectors;
    uint32_t dir_lba;
    uint32_t dir_sectors;
    uint32_t bitmap_lba;
    uint32_t bitmap_sectors;
    uint32_t data_lba;
    uint32_t data_sector_count;
    uint32_t max_inodes;
    uint32_t max_dir_entries;
    uint32_t clean_shutdown;
    uint32_t reserved[116];
} __attribute__((packed));

struct mbfs_extent {
    uint32_t start_lba;
    uint32_t sector_count;
} __attribute__((packed));

struct mbfs_inode {
    uint8_t used;
    uint8_t type;
    uint16_t flags;
    uint32_t size_bytes;
    uint32_t extent_count;
    struct mbfs_extent extents[MBFS_INODE_EXTENTS];
    uint32_t reserved[5];
} __attribute__((packed));

struct mbfs_dir_entry {
    uint8_t used;
    uint8_t inode_index;
    uint8_t type;
    uint8_t reserved0;
    char name[MBFS_NAME_MAX];
    uint8_t reserved[28];
} __attribute__((packed));

struct mbfs_path_lookup {
    uint8_t found;
    uint8_t parent_is_root;
    uint8_t inode_index;
    uint8_t parent_dir_inode;
    int32_t entry_slot;
};

static struct ata_device ata_devices[4];
static struct ata_device* diskfs_device = 0;
static struct mbfs_superblock mbfs_superblock;
static struct mbfs_inode mbfs_inodes[MBFS_MAX_INODES];
static struct mbfs_dir_entry mbfs_dir[MBFS_MAX_DIRENTS];
static uint8_t mbfs_bitmap[MBFS_BITMAP_SECTORS * DISKFS_SECTOR_SIZE];
/* Scratch buffer used by integrity checks and repair routines. Sized to
 * the on-disk bitmap so it mirrors on-disk layout. Kept static to avoid
 * large stack allocations in kernel context. */
static uint8_t mbfs_expected_bitmap[MBFS_BITMAP_SECTORS * DISKFS_SECTOR_SIZE];
/* Single global sector buffer used for all PIO writes to avoid stack/temporary
 * buffer allocation issues and to make writes easily serializable.
 */
static uint8_t ata_global_sector_buf[DISKFS_SECTOR_SIZE] __attribute__((aligned(4096)));
static uint8_t diskfs_mounted = 0;
static uint8_t mbfs_unclean_last_mount = 0;
static struct mbfs_dir_entry mbfs_subdir_scratch[MBFS_MAX_DIRENTS];

/* Minimal terminal "app" that runs via the app_proc scheduler */
static void terminal_app_step(int pid)
{
    char buf[APP_PROC_STDIN_MAX];
    if (pid < 0 || pid >= APP_PROC_MAX) { return; }
    struct app_proc* p = &app_procs[pid];
    if (!p->started) {
        app_proc_write_stdout(pid, "MBos terminal\n> ");
        p->started = 1;
        return;
    }

    int n = app_proc_readline(pid, buf, sizeof(buf));
    if (n <= 0) { return; }

    if (str_equals(buf, "help")) {
        app_proc_write_stdout(pid, "Commands: help, echo <text>, ls, clear\n");
    } else if (str_starts_with(buf, "echo ")) {
        app_proc_write_stdout(pid, buf + 5);
        app_proc_write_stdout(pid, "\n");
    } else if (str_equals(buf, "clear")) {
        /* Clear by writing many newlines (simple) */
        for (int i = 0; i < 20; i++) { app_proc_write_stdout(pid, "\n"); }
    } else if (str_equals(buf, "ls")) {
        struct mbfs_dir_entry entries[MBFS_MAX_DIRENTS];
        uint32_t entry_count = 0;
        int rc = diskfs_get_entries("/", entries, MBFS_MAX_DIRENTS, &entry_count);
        if (rc < 0) {
            app_proc_write_stdout(pid, "MBFS not available\n");
        } else {
            for (uint32_t i = 0; i < entry_count; i++) {
                app_proc_write_stdout(pid, entries[i].name);
                app_proc_write_stdout(pid, "\n");
            }
        }
    } else {
        app_proc_write_stdout(pid, "Unknown command\n");
    }
    app_proc_write_stdout(pid, "> ");
}


enum exec_format {
    EXEC_FORMAT_UNKNOWN = 0,
    EXEC_FORMAT_RAW,
    EXEC_FORMAT_MBAPP,
    EXEC_FORMAT_ELF32,
    EXEC_FORMAT_PE32
};

/* MBAPP (MBos Application) Format v1
 * Custom executable format for MBos kernel.
 * Layout:
 *   uint32_t magic       - 0x50504142 ('MBAP')
 *   uint32_t version     - format version (1 = v1)
 *   uint32_t flags       - capability flags (reserved, must be 0)
 *   uint32_t entry_off   - entry point offset from code VA start
 *   uint32_t image_size  - payload bytes immediately after header
 *   [image_size bytes]   - code/data payload
 */
struct mbapp_header {
    uint32_t magic;       /* 'M''B''A''P' = 0x50504142 */
    uint32_t version;     /* format version (currently 1) */
    uint32_t flags;       /* capability flags (reserved, must be 0) */
    uint32_t entry_off;   /* entry point offset from code VA start */
    uint32_t image_size;  /* payload bytes after header */
} __attribute__((packed));

struct elf32_ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed));

#define ELF32_PT_LOAD 1u

/* PE32 (Windows i386) Binary Headers */
struct pe32_dos_header {
    uint8_t dos_magic[2];      /* 'MZ' = 0x4D5A */
    uint8_t dos_reserved[58];  /* Skip DOS stub */
    uint32_t pe_offset;        /* Offset to PE signature */
} __attribute__((packed));

struct pe32_coff_header {
    uint16_t machine;          /* 0x014C for i386 */
    uint16_t num_sections;
    uint32_t timeDateStamp;
    uint32_t pointerToSymbolTable;
    uint32_t numSymbols;
    uint16_t sizeOfOptionalHeader;
    uint16_t characteristics;
} __attribute__((packed));

struct pe32_opt_header {
    uint16_t magic;            /* 0x010B for PE32 */
    uint8_t linkerVersion;
    uint8_t linkerMinor;
    uint32_t sizeOfCode;
    uint32_t sizeOfInitializedData;
    uint32_t sizeOfUninitializedData;
    uint32_t addressOfEntryPoint;
    uint32_t baseOfCode;
    uint32_t baseOfData;
    uint32_t imageBase;
    uint32_t sectionAlignment;
    uint32_t fileAlignment;
    uint16_t osVersionMajor;
    uint16_t osVersionMinor;
    uint16_t imageVersionMajor;
    uint16_t imageVersionMinor;
    uint16_t subsysVersionMajor;
    uint16_t subsysVersionMinor;
    uint32_t reservedForWin32;
    uint32_t sizeOfImage;
    uint32_t sizeOfHeaders;
    uint32_t checkSum;
    uint16_t subsystem;
    uint16_t dllCharacteristics;
    uint32_t stackReserveSize;
    uint32_t stackCommitSize;
    uint32_t heapReserveSize;
    uint32_t heapCommitSize;
    uint32_t loaderFlags;
    uint32_t numberOfRVAAndSizes;
} __attribute__((packed));

struct pe32_section_header {
    uint8_t name[8];
    uint32_t virtualSize;
    uint32_t virtualAddress;
    uint32_t sizeOfRawData;
    uint32_t pointerToRawData;
    uint8_t reserved[16];
    uint32_t characteristics;
} __attribute__((packed));

#define PE32_I386_MACHINE 0x014Cu
#define PE32_MAGIC 0x010Bu

#define PROCESS_CONTEXT_MAX 4

struct process_register_snapshot {
    uint32_t vector;
    uint32_t captures;
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
    uint32_t useresp;
    uint32_t ss;
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    uint32_t esi;
    uint32_t edi;
    uint32_t ebp;
    uint32_t esp;
};

enum process_task_state {
    TASK_STATE_NEW = 0,
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_BLOCKED,
    TASK_STATE_EXITED
};

enum process_wait_reason {
    WAIT_REASON_NONE = 0,
    WAIT_REASON_SLEEP,
    WAIT_REASON_MANUAL,
    WAIT_REASON_EVENT
};

struct process_fd {
    uint8_t used;
    uint8_t flags;
    uint8_t backend;
    uint8_t reserved0;
    int32_t file_index;
    uint32_t offset;
};
struct process_syscall_context {
    uint32_t pid;
    uint8_t runnable;
    uint32_t context_switches;
    uint32_t syscall_count;
    uint32_t last_num;
    uint32_t last_arg0;
    uint32_t last_ret;
    uint32_t exit_count;
    uint32_t last_exit_code;
    uint32_t sleep_until_tick;
    uint32_t wait_timeout_tick;
    uint32_t wait_event_channel;
    uint8_t wait_reason;
    uint8_t task_state;
    uint8_t user_ready;
    uint32_t user_eip;
    uint32_t user_esp;
    uint32_t user_code_va;
    uint32_t user_stack_top;
    uint32_t user_heap_base;
    uint32_t user_heap_brk;
    uint32_t user_heap_limit;
    uint32_t user_mapped_pages[PROCESS_USER_PAGE_TRACK_MAX];
    uint32_t user_mapped_count;
    char cwd[MBFS_PATH_MAX];
    uint32_t page_dir_phys;  /* per-process page directory (0 = use global) */
    struct process_fd fds[PROCESS_FD_MAX];
    uint8_t snapshot_valid;
    struct process_register_snapshot snapshot;
};

static struct process_syscall_context process_contexts[PROCESS_CONTEXT_MAX];
static uint32_t current_process_slot = 0;
static uint8_t scheduler_enabled = 0;
static uint32_t scheduler_quantum_ticks = 100;
static uint32_t scheduler_tick_accumulator = 0;
static uint8_t scheduler_frame_restore_enabled = 0;
static uint32_t scheduler_frame_restore_count = 0;
static uint32_t scheduler_frame_restore_reject_count = 0;

#define PAGE_SIZE 4096u
#define MAX_USABLE_REGIONS 32
#define PHYSICAL_RECYCLE_MAX 1024
#define PHYSICAL_OWNER_TRACK_MAX 4096
#define PAGING_IDENTITY_CAP_BYTES (64u * 1024u * 1024u)
#define PAGING_MIN_KEEP_BYTES 0x00400000u
#define KERNEL_VIRT_BASE 0xC0000000u
#define KERNEL_VGA_VIRT 0xC00B8000u
#define KERNEL_HEAP_BASE 0xC1000000u
#define PAGING_TEST_BASE 0xC2000000u
#define PAGING_TABLE_ALIAS_BASE 0xC3000000u
#define PAGING_DIRECTORY_VIRT 0xC3400000u
#define USER_SPACE_BASE 0x00400000u
#define USER_SPACE_LIMIT KERNEL_VIRT_BASE
#define USER_TEST_BASE 0x40000000u
#define USER_TEST_CODE_VA 0x40001000u
#define USER_TEST_STACK_TOP 0x40003000u
#define USER_TASK_BASE 0x50000000u
#define USER_TASK_SLOT_STRIDE 0x00100000u
#define USER_TASK_CODE_OFFSET 0x00001000u
#define USER_TASK_STACK_TOP_OFFSET 0x00004000u  /* 0x2000-0x2FFF reserved as guard page below stack */
#define GUI_FRAMEBUFFER_VIRT_BASE 0xE0000000u
#define KERNEL_HEAP_INITIAL_BYTES (16u * PAGE_SIZE)

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002
#define MULTIBOOT_INFO_MEM_MAP 0x00000040
#define MULTIBOOT_INFO_VBE 0x00000800
#define MULTIBOOT_INFO_FRAMEBUFFER 0x00001000

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint8_t framebuffer_reserved[2];
} __attribute__((packed));

struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));

struct vbe_mode_info {
    uint16_t mode_attributes;
    uint8_t win_a;
    uint8_t win_b;
    uint16_t granularity;
    uint16_t win_size;
    uint16_t segment_a;
    uint16_t segment_b;
    uint32_t win_func_ptr;
    uint16_t pitch;
    uint16_t width;
    uint16_t height;
    uint8_t w_char;
    uint8_t y_char;
    uint8_t planes;
    uint8_t bpp;
    uint8_t banks;
    uint8_t memory_model;
    uint8_t bank_size;
    uint8_t image_pages;
    uint8_t reserved0;
    uint8_t red_mask;
    uint8_t red_position;
    uint8_t green_mask;
    uint8_t green_position;
    uint8_t blue_mask;
    uint8_t blue_position;
    uint8_t reserved_mask;
    uint8_t reserved_position;
    uint8_t direct_color_attributes;
    uint32_t framebuffer;
} __attribute__((packed));

static const struct multiboot_info* boot_multiboot_info = 0;

struct physical_region {
    uint32_t start;
    uint32_t current;
    uint32_t end;
};

enum physical_page_owner {
    PAGE_OWNER_FREE = 0,
    PAGE_OWNER_ALLOCATOR,
    PAGE_OWNER_PAGING_STRUCTURE,
    PAGE_OWNER_KERNEL_HEAP,
    PAGE_OWNER_PAGING_TEST,
    PAGE_OWNER_USER_TASK,
    PAGE_OWNER_USER_MISC
};

struct physical_page_owner_entry {
    uint32_t page;
    uint8_t owner;
};

static struct physical_region physical_regions[MAX_USABLE_REGIONS];
static uint32_t physical_region_count = 0;
static uint32_t physical_region_cursor = 0;
static uint32_t physical_total_pages = 0;
static uint32_t physical_used_pages = 0;
static uint32_t physical_highest_usable_end = 0;
static uint32_t physical_recycled_pages[PHYSICAL_RECYCLE_MAX];
static uint32_t physical_recycled_count = 0;
static uint32_t physical_recycle_drop_count = 0;
static uint32_t physical_recycle_duplicate_count = 0;
static uint32_t physical_recycle_invalid_count = 0;
static uint32_t physical_recycle_protected_count = 0;
static struct physical_page_owner_entry physical_owner_entries[PHYSICAL_OWNER_TRACK_MAX];
static uint32_t physical_owner_entry_count = 0;
static uint32_t physical_owner_overflow_count = 0;

static uint32_t paging_directory_phys = 0;
static uint32_t paging_identity_limit = 0;
static uint32_t paging_table_count = 0;
static uint8_t paging_enabled = 0;
static uint8_t paging_aliases_ready = 0;
static uint32_t paging_test_next_virt = PAGING_TEST_BASE;
static uint32_t paging_last_mapped_virt = 0;
static uint32_t paging_last_mapped_phys = 0;
static uint32_t paging_user_test_next_virt = USER_TEST_BASE;
static uint32_t paging_last_user_virt = 0;
static uint32_t paging_last_user_phys = 0;
static uint8_t gui_framebuffer_detected = 0;
static uint8_t gui_framebuffer_mapped = 0;
static uint32_t gui_framebuffer_phys = 0;
static uint32_t gui_framebuffer_virt = 0;
static uint32_t gui_framebuffer_size = 0;
static uint32_t gui_framebuffer_pitch = 0;
static uint32_t gui_framebuffer_width = 0;
static uint32_t gui_framebuffer_height = 0;
static uint8_t gui_framebuffer_bpp = 0;
static uint8_t* gui_backbuffer = 0;      /* heap-allocated back-buffer */
static uint8_t  gui_backbuffer_ready = 0;
static uint32_t kernel_virtual_base = KERNEL_VIRT_BASE;
static uint32_t kernel_virtual_end = KERNEL_VIRT_BASE;
static uint32_t kernel_vga_virtual = KERNEL_VGA_VIRT;
static uint8_t terminal_using_high_vga = 0;

static uint32_t kernel_heap_start = KERNEL_HEAP_BASE;
static uint32_t kernel_heap_end = KERNEL_HEAP_BASE;
static uint32_t kernel_heap_current = KERNEL_HEAP_BASE;
static uint8_t kernel_heap_ready = 0;
static uint32_t kernel_heap_last_alloc = 0;
static uint32_t kernel_heap_alloc_count = 0;
static uint8_t shell_input_heap_backed = 0;
static uint8_t shell_history_heap_backed = 0;

/* Forward declarations used by early backend helpers. */
static void physical_allocator_free_page(uint32_t physical_page);

extern uint8_t __kernel_start;
extern uint8_t __kernel_end;

static char shell_history_boot[SHELL_HISTORY_MAX][SHELL_HISTORY_WIDTH];
static char (*shell_history)[SHELL_HISTORY_WIDTH] = shell_history_boot;
static size_t shell_history_count = 0;
static size_t shell_history_head = 0;
static int shell_history_browse = -1;

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

struct interrupt_frame {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
};

static struct gdt_entry gdt[6];
static struct gdt_ptr gp;
static struct idt_entry idt[256];
static struct idt_ptr idtp;
static struct tss_entry tss;

extern void gdt_flush(uint32_t);
extern void idt_load(uint32_t);
extern void tss_flush(uint16_t);

#define DECL_ISR(n) extern void isr##n(void)
DECL_ISR(0); DECL_ISR(1); DECL_ISR(2); DECL_ISR(3); DECL_ISR(4); DECL_ISR(5); DECL_ISR(6); DECL_ISR(7);
DECL_ISR(8); DECL_ISR(9); DECL_ISR(10); DECL_ISR(11); DECL_ISR(12); DECL_ISR(13); DECL_ISR(14); DECL_ISR(15);
DECL_ISR(16); DECL_ISR(17); DECL_ISR(18); DECL_ISR(19); DECL_ISR(20); DECL_ISR(21); DECL_ISR(22); DECL_ISR(23);
DECL_ISR(24); DECL_ISR(25); DECL_ISR(26); DECL_ISR(27); DECL_ISR(28); DECL_ISR(29); DECL_ISR(30); DECL_ISR(31);
DECL_ISR(128);

#define DECL_IRQ(n) extern void irq##n(void)
DECL_IRQ(0); DECL_IRQ(1); DECL_IRQ(2); DECL_IRQ(3); DECL_IRQ(4); DECL_IRQ(5); DECL_IRQ(6); DECL_IRQ(7);
DECL_IRQ(8); DECL_IRQ(9); DECL_IRQ(10); DECL_IRQ(11); DECL_IRQ(12); DECL_IRQ(13); DECL_IRQ(14); DECL_IRQ(15);

static inline uint16_t vga_entry(unsigned char ch, uint8_t color)
{
    return (uint16_t)ch | ((uint16_t)color << 8);
}

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ __volatile__("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void)
{
    __asm__ __volatile__("outb %%al, $0x80" : : "a"(0));
}

/* Basic serial (COM1) helpers for emergency debug output over -serial */
#define SERIAL_COM1_BASE 0x3F8
uint8_t serial_enabled = 0;
static void serial_init(void)
{
    /* Disable interrupts */
    outb(SERIAL_COM1_BASE + 1, 0x00);
    /* Enable DLAB */
    outb(SERIAL_COM1_BASE + 3, 0x80);
    /* Set divisor to 3 (38400 baud) */
    outb(SERIAL_COM1_BASE + 0, 0x03);
    outb(SERIAL_COM1_BASE + 1, 0x00);
    /* 8 bits, no parity, one stop bit */
    outb(SERIAL_COM1_BASE + 3, 0x03);
    /* Enable FIFO, clear them, with 14-byte threshold */
    outb(SERIAL_COM1_BASE + 2, 0xC7);
    /* IRQs enabled, RTS/DSR set */
    outb(SERIAL_COM1_BASE + 4, 0x0B);
    serial_enabled = 1;
}

static inline int serial_is_transmit_empty(void)
{
    return (inb(SERIAL_COM1_BASE + 5) & 0x20) != 0;
}

static void serial_putc(char c)
{
    if (!serial_enabled) { return; }
    /* Wait for transmit FIFO empty */
    while (!serial_is_transmit_empty()) { /* spin */ }
    outb(SERIAL_COM1_BASE + 0, (uint8_t)c);
}

static void serial_write(const char* s)
{
    if (!serial_enabled) { return; }
    for (size_t i = 0; s[i] != '\0'; i++) {
        serial_putc(s[i]);
    }
}

static void serial_write_dec_u32(uint32_t v)
{
    if (!serial_enabled) { return; }
    char buf[12];
    size_t pos = 0;
    if (v == 0) { serial_putc('0'); return; }
    while (v > 0 && pos < sizeof(buf)) {
        buf[pos++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (pos > 0) {
        pos--;
        serial_putc(buf[pos]);
    }
}

static void serial_write_hex(uint32_t value)
{
    if (!serial_enabled) { return; }
    const char* hex = "0123456789ABCDEF";
    serial_putc('0'); serial_putc('x');
    for (int i = 7; i >= 0; i--) {
        serial_putc(hex[(value >> (i * 4)) & 0xF]);
    }
}

static inline void load_page_directory(uint32_t physical_address)
{
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(physical_address) : "memory");
}

static inline void irq_disable(void)
{
    __asm__ __volatile__("cli" : : : "memory");
}

static inline void irq_enable(void)
{
    __asm__ __volatile__("sti" : : : "memory");
}

static inline void enable_paging_hw(void)
{
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

static inline uint32_t read_cr2(void)
{
    uint32_t value;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(value));
    return value;
}

static inline void invalidate_page(uint32_t virtual_address)
{
    __asm__ __volatile__("invlpg (%0)" : : "r"((uintptr_t)virtual_address) : "memory");
}

static void terminal_clear(void)
{
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
        }
    }
    terminal_row = 0;
    terminal_col = 0;
}

static void terminal_scroll_one_line(void)
{
    for (size_t y = 1; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_buffer[(y - 1) * VGA_WIDTH + x] = terminal_buffer[y * VGA_WIDTH + x];
        }
    }

    for (size_t x = 0; x < VGA_WIDTH; x++) {
        terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
    }
}

static void gui_term_new_line(void)
{
    gui_term_head = (gui_term_head + 1u) % GUI_TERM_LOG_LINES;
    if (gui_term_count < GUI_TERM_LOG_LINES) {
        gui_term_count++;
    }
    for (uint32_t i = 0; i < GUI_TERM_LOG_COLS + 1u; i++) {
        gui_term_log[gui_term_head][i] = '\0';
    }
    gui_term_col = 0;
}

static void gui_term_push_char(char c)
{
    if (c == '\r') {
        return;
    }

    if (c == '\n') {
        gui_term_new_line();
        return;
    }

    if (c == '\t') {
        for (uint32_t i = 0; i < 4u; i++) {
            gui_term_push_char(' ');
        }
        return;
    }

    if (gui_term_col >= GUI_TERM_LOG_COLS) {
        gui_term_new_line();
    }

    if ((unsigned char)c < 0x20u || (unsigned char)c > 0x7Eu) {
        c = '.';
    }

    gui_term_log[gui_term_head][gui_term_col++] = c;
    gui_term_log[gui_term_head][gui_term_col] = '\0';
}

static const char* gui_term_line_at(uint32_t logical_index)
{
    uint32_t oldest;
    uint32_t idx;

    if (logical_index >= gui_term_count) {
        return "";
    }

    oldest = (gui_term_head + GUI_TERM_LOG_LINES + 1u - gui_term_count) % GUI_TERM_LOG_LINES;
    idx = (oldest + logical_index) % GUI_TERM_LOG_LINES;
    return gui_term_log[idx];
}

static void terminal_put_at(size_t row, size_t col, char c)
{
    if (row >= VGA_HEIGHT || col >= VGA_WIDTH) {
        return;
    }

    terminal_buffer[row * VGA_WIDTH + col] = vga_entry((unsigned char)c, terminal_color);
}

static void terminal_advance_position(size_t start_row, size_t start_col, size_t offset, size_t* out_row, size_t* out_col)
{
    size_t row = start_row;
    size_t col = start_col;

    for (size_t i = 0; i < offset; i++) {
        col++;
        if (col >= VGA_WIDTH) {
            col = 0;
            if (row + 1 < VGA_HEIGHT) {
                row++;
            }
        }
    }

    *out_row = row;
    *out_col = col;
}

static void terminal_putchar(char c)
{
    gui_term_push_char(c);

    if (c == '\n') {
        terminal_col = 0;
        terminal_row++;
        if (terminal_row >= VGA_HEIGHT) {
            terminal_scroll_one_line();
            terminal_row = VGA_HEIGHT - 1;
        }
        return;
    }

    terminal_buffer[terminal_row * VGA_WIDTH + terminal_col] = vga_entry((unsigned char)c, terminal_color);
    terminal_col++;

    if (terminal_col >= VGA_WIDTH) {
        terminal_col = 0;
        terminal_row++;
    }

    if (terminal_row >= VGA_HEIGHT) {
        terminal_scroll_one_line();
        terminal_row = VGA_HEIGHT - 1;
    }
}

static void terminal_write(const char* str)
{
    for (size_t i = 0; str[i] != '\0'; i++) {
        terminal_putchar(str[i]);
    }
    /* Also mirror terminal output to serial (when enabled) so headless
     * QEMU runs receive the same messages over -serial stdio. */
    serial_write(str);
}

static void terminal_write_hex(uint32_t value)
{
    const char* hex = "0123456789ABCDEF";
    terminal_write("0x");
    for (int i = 7; i >= 0; i--) {
        terminal_putchar(hex[(value >> (i * 4)) & 0xF]);
    }
}

static void terminal_write_hex_u64(uint64_t value)
{
    const char* hex = "0123456789ABCDEF";
    terminal_write("0x");
    for (int i = 15; i >= 0; i--) {
        terminal_putchar(hex[(value >> (i * 4)) & 0xF]);
    }
}

static void terminal_write_dec_u32(uint32_t value)
{
    char buf[11];
    size_t pos = 0;

    if (value == 0) {
        terminal_putchar('0');
        return;
    }

    while (value > 0 && pos < sizeof(buf)) {
        buf[pos++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (pos > 0) {
        pos--;
        terminal_putchar(buf[pos]);
    }
}

static void memory_set_u8(void* dst, uint8_t value, size_t count)
{
    uint8_t* bytes = (uint8_t*)dst;
    for (size_t i = 0; i < count; i++) {
        bytes[i] = value;
    }
}

static void memory_copy(void* dst, const void* src, size_t count)
{
    uint8_t* out = (uint8_t*)dst;
    const uint8_t* in = (const uint8_t*)src;
    for (size_t i = 0; i < count; i++) {
        out[i] = in[i];
    }
}

static int str_equals(const char* a, const char* b)
{
    size_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }

    return a[i] == b[i];
}

static size_t str_length(const char* s)
{
    size_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

static void str_copy_bounded(char* dst, const char* src, size_t dst_size)
{
    size_t i = 0;
    if (dst_size == 0) {
        return;
    }

    while (i + 1 < dst_size && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int str_is_empty(const char* s)
{
    return s[0] == '\0';
}

static uint32_t align_up_u32(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static uint32_t align_down_u32(uint32_t value, uint32_t alignment)
{
    return value & ~(alignment - 1u);
}

static int paging_map_page(uint32_t virtual_address, uint32_t physical_address, uint32_t flags);
static int paging_unmap_page(uint32_t virtual_address);
static int paging_map_new_page(uint32_t virtual_address, uint32_t flags, uint8_t owner, uint32_t* physical_out);
static int paging_lookup(uint32_t virtual_address, uint32_t* physical_out, uint32_t* entry_out);
static void physical_allocator_free_page(uint32_t physical_page);
static uint32_t physical_allocator_alloc_page(void);
static void physical_allocator_set_owner(uint32_t physical_page, uint8_t owner);
static int paging_lookup_direct(uint32_t dir_phys, uint32_t virtual_address, uint32_t* physical_out, uint32_t* entry_out);
static int32_t diskfs_find_index(const char* name);
static int32_t mbfs_alloc_dir_slot(void);
static int32_t mbfs_alloc_inode_slot(void);
//static void diskfs_list(void);
//static int diskfs_remove(const char* name);
static void diskfs_list_path(const char* path);
static void diskfs_cat_path(const char* path);
static int  diskfs_remove_path(const char* path);
static int  diskfs_write_path(const char* path, const uint8_t* data, uint32_t size);
static int  diskfs_lookup_path(const char* path, struct mbfs_path_lookup* out, char* leaf_out, uint32_t leaf_max);
static int  diskfs_get_entries(const char* path, struct mbfs_dir_entry* out, uint32_t max_count, uint32_t* out_count);
static uint32_t mbfs_file_size_by_inode_index(uint8_t inode_idx);
static int32_t mbfs_read_by_inode_index(uint8_t inode_idx, uint32_t offset, uint8_t* out, uint32_t count);
static void user_enter_ring3(uint32_t entry_eip, uint32_t user_stack);

static uint32_t paging_alias_virtual_for_directory(uint32_t directory_index)
{
    return PAGING_TABLE_ALIAS_BASE + directory_index * PAGE_SIZE;
}

static struct process_syscall_context* current_process_context(void)
{
    return &process_contexts[current_process_slot];
}

static uint32_t user_task_code_va_for_pid(uint32_t pid)
{
    uint32_t slot = pid - 1u;
    return USER_TASK_BASE + slot * USER_TASK_SLOT_STRIDE + USER_TASK_CODE_OFFSET;
}

static uint32_t user_task_stack_top_for_pid(uint32_t pid)
{
    uint32_t slot = pid - 1u;
    return USER_TASK_BASE + slot * USER_TASK_SLOT_STRIDE + USER_TASK_STACK_TOP_OFFSET;
}

static void process_capture_snapshot(struct process_syscall_context* proc, const struct interrupt_frame* frame)
{
    if (proc == 0 || frame == 0) {
        return;
    }

    proc->snapshot_valid = 1;
    proc->snapshot.captures++;
    proc->snapshot.vector = frame->int_no;
    proc->snapshot.eip = frame->eip;
    proc->snapshot.cs = frame->cs;
    proc->snapshot.eflags = frame->eflags;
    proc->snapshot.useresp = frame->useresp;
    proc->snapshot.ss = frame->ss;
    proc->snapshot.eax = frame->eax;
    proc->snapshot.ebx = frame->ebx;
    proc->snapshot.ecx = frame->ecx;
    proc->snapshot.edx = frame->edx;
    proc->snapshot.esi = frame->esi;
    proc->snapshot.edi = frame->edi;
    proc->snapshot.ebp = frame->ebp;
    proc->snapshot.esp = frame->esp;
}

static void process_print_snapshot(const struct process_syscall_context* proc)
{
    if (proc == 0) {
        return;
    }

    terminal_write("PID ");
    terminal_write_dec_u32(proc->pid);
    terminal_write(" snapshot: ");

    if (!proc->snapshot_valid) {
        terminal_write("<none>\n");
        return;
    }

    terminal_write("vector=");
    terminal_write_dec_u32(proc->snapshot.vector);
    terminal_write(" captures=");
    terminal_write_dec_u32(proc->snapshot.captures);
    terminal_write("\n");
    terminal_write("  EIP=");
    terminal_write_hex(proc->snapshot.eip);
    terminal_write(" CS=");
    terminal_write_hex(proc->snapshot.cs);
    terminal_write(" EFLAGS=");
    terminal_write_hex(proc->snapshot.eflags);
    terminal_write("\n");
    terminal_write("  EAX=");
    terminal_write_hex(proc->snapshot.eax);
    terminal_write(" EBX=");
    terminal_write_hex(proc->snapshot.ebx);
    terminal_write(" ECX=");
    terminal_write_hex(proc->snapshot.ecx);
    terminal_write(" EDX=");
    terminal_write_hex(proc->snapshot.edx);
    terminal_write("\n");
    terminal_write("  ESI=");
    terminal_write_hex(proc->snapshot.esi);
    terminal_write(" EDI=");
    terminal_write_hex(proc->snapshot.edi);
    terminal_write(" EBP=");
    terminal_write_hex(proc->snapshot.ebp);
    terminal_write(" ESP=");
    terminal_write_hex(proc->snapshot.esp);
    terminal_write("\n");
    terminal_write("  USERESP=");
    terminal_write_hex(proc->snapshot.useresp);
    terminal_write(" SS=");
    terminal_write_hex(proc->snapshot.ss);
    terminal_write("\n");
}

static int process_restore_snapshot_to_user_context(struct process_syscall_context* proc)
{
    uint32_t restored_esp;

    if (proc == 0 || !proc->snapshot_valid || !proc->user_ready) {
        return 0;
    }

    if (proc->snapshot.cs != 0x1Bu) {
        return 0;
    }

    if (proc->snapshot.eip < USER_SPACE_BASE || proc->snapshot.eip >= USER_SPACE_LIMIT) {
        return 0;
    }

    restored_esp = proc->snapshot.useresp;
    if (restored_esp == 0) {
        restored_esp = proc->snapshot.esp;
    }

    if (restored_esp < USER_SPACE_BASE || restored_esp >= USER_SPACE_LIMIT) {
        return 0;
    }

    proc->user_eip = proc->snapshot.eip;
    proc->user_esp = restored_esp;
    return 1;
}

static int process_seed_snapshot_from_user_context(struct process_syscall_context* proc)
{
    if (proc == 0 || !proc->user_ready) {
        return 0;
    }

    proc->snapshot_valid = 1;
    proc->snapshot.captures++;
    proc->snapshot.vector = 0xFFFFFFFFu;
    proc->snapshot.eip = proc->user_eip;
    proc->snapshot.cs = 0x1Bu;
    proc->snapshot.eflags = 0x00000202u;
    proc->snapshot.useresp = proc->user_esp;
    proc->snapshot.ss = 0x23u;
    proc->snapshot.eax = 0;
    proc->snapshot.ebx = 0;
    proc->snapshot.ecx = 0;
    proc->snapshot.edx = 0;
    proc->snapshot.esi = 0;
    proc->snapshot.edi = 0;
    proc->snapshot.ebp = proc->user_esp;
    proc->snapshot.esp = proc->user_esp;
    return 1;
}

static int process_snapshot_is_user_compatible(const struct process_syscall_context* proc)
{
    uint32_t restored_esp;
    uint32_t min_eip;
    uint32_t max_eip;

    if (proc == 0 || !proc->snapshot_valid || !proc->runnable) {
        return 0;
    }

    if (proc->snapshot.cs != 0x1Bu || proc->snapshot.ss != 0x23u) {
        return 0;
    }

    min_eip = proc->user_code_va;
    max_eip = proc->user_stack_top;
    if (!proc->user_ready) {
        return 0;
    }

    if (proc->snapshot.eip < min_eip || proc->snapshot.eip >= max_eip) {
        return 0;
    }

    restored_esp = proc->snapshot.useresp;
    if (restored_esp == 0) {
        restored_esp = proc->snapshot.esp;
    }

    if (restored_esp < min_eip || restored_esp > proc->user_stack_top) {
        return 0;
    }

    return 1;
}

static int process_restore_snapshot_to_frame(const struct process_syscall_context* proc, struct interrupt_frame* frame)
{
    if (frame == 0 || !process_snapshot_is_user_compatible(proc)) {
        return 0;
    }

    frame->eax = proc->snapshot.eax;
    frame->ebx = proc->snapshot.ebx;
    frame->ecx = proc->snapshot.ecx;
    frame->edx = proc->snapshot.edx;
    frame->esi = proc->snapshot.esi;
    frame->edi = proc->snapshot.edi;
    frame->ebp = proc->snapshot.ebp;
    frame->esp = proc->snapshot.esp;
    frame->eip = proc->snapshot.eip;
    frame->cs = proc->snapshot.cs;
    frame->eflags = proc->snapshot.eflags | 0x200u;
    frame->useresp = proc->snapshot.useresp;
    frame->ss = proc->snapshot.ss;
    frame->ds = 0x23u;
    frame->es = 0x23u;
    frame->fs = 0x23u;
    frame->gs = 0x23u;
    return 1;
}

static const char* task_state_name(uint8_t state)
{
    switch (state) {
    case TASK_STATE_NEW:
        return "new";
    case TASK_STATE_READY:
        return "ready";
    case TASK_STATE_RUNNING:
        return "running";
    case TASK_STATE_BLOCKED:
        return "blocked";
    case TASK_STATE_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static const char* wait_reason_name(uint8_t reason)
{
    switch (reason) {
    case WAIT_REASON_NONE:
        return "none";
    case WAIT_REASON_SLEEP:
        return "sleep";
    case WAIT_REASON_MANUAL:
        return "manual";
    case WAIT_REASON_EVENT:
        return "event";
    default:
        return "unknown";
    }
}

static void process_set_task_state(struct process_syscall_context* proc, uint8_t state)
{
    if (proc == 0) {
        return;
    }

    proc->task_state = state;
    proc->runnable = (state == TASK_STATE_READY || state == TASK_STATE_RUNNING) ? 1u : 0u;
    if (state != TASK_STATE_BLOCKED) {
        proc->wait_reason = WAIT_REASON_NONE;
    }
}

static void process_block_with_reason(struct process_syscall_context* proc, uint8_t wait_reason, uint32_t sleep_until_tick)
{
    if (proc == 0) {
        return;
    }

    proc->sleep_until_tick = sleep_until_tick;
    proc->wait_timeout_tick = 0;
    proc->wait_event_channel = 0;
    process_set_task_state(proc, TASK_STATE_BLOCKED);
    proc->wait_reason = wait_reason;
}

static uint32_t runnable_process_count(void)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < PROCESS_CONTEXT_MAX; i++) {
        if (process_contexts[i].runnable) {
            count++;
        }
    }
    return count;
}

static void process_contexts_init(void)
{
    for (uint32_t i = 0; i < PROCESS_CONTEXT_MAX; i++) {
        process_contexts[i].runnable = 1;
        process_contexts[i].task_state = TASK_STATE_READY;
        process_contexts[i].context_switches = 0;
        process_contexts[i].syscall_count = 0;
        process_contexts[i].last_num = 0;
        process_contexts[i].last_arg0 = 0;
        process_contexts[i].last_ret = 0;
        process_contexts[i].exit_count = 0;
        process_contexts[i].last_exit_code = 0;
        process_contexts[i].sleep_until_tick = 0;
        process_contexts[i].wait_timeout_tick = 0;
        process_contexts[i].wait_event_channel = 0;
        process_contexts[i].wait_reason = WAIT_REASON_NONE;
        process_contexts[i].user_ready = 0;
        process_contexts[i].user_eip = 0;
        process_contexts[i].user_esp = 0;
        process_contexts[i].user_code_va = user_task_code_va_for_pid(i + 1u);
        process_contexts[i].user_stack_top = user_task_stack_top_for_pid(i + 1u);
        process_contexts[i].user_heap_base = align_up_u32(process_contexts[i].user_stack_top + PAGE_SIZE, PAGE_SIZE);
        process_contexts[i].user_heap_brk = process_contexts[i].user_heap_base;
        process_contexts[i].user_heap_limit = USER_TASK_BASE + i * USER_TASK_SLOT_STRIDE + USER_TASK_SLOT_STRIDE - PAGE_SIZE;
        process_contexts[i].user_mapped_count = 0;
        process_contexts[i].cwd[0] = '\0';
        for (uint32_t mp = 0; mp < PROCESS_USER_PAGE_TRACK_MAX; mp++) {
            process_contexts[i].user_mapped_pages[mp] = 0;
        }
        for (uint32_t fd = 0; fd < PROCESS_FD_MAX; fd++) {
            process_contexts[i].fds[fd].used = 0;
            process_contexts[i].fds[fd].flags = 0;
            process_contexts[i].fds[fd].file_index = -1;
            process_contexts[i].fds[fd].offset = 0;
        }
        process_contexts[i].snapshot_valid = 0;
        process_contexts[i].snapshot.vector = 0;
        process_contexts[i].snapshot.captures = 0;
        process_contexts[i].snapshot.eip = 0;
        process_contexts[i].snapshot.cs = 0;
        process_contexts[i].snapshot.eflags = 0;
        process_contexts[i].snapshot.useresp = 0;
        process_contexts[i].snapshot.ss = 0;
        process_contexts[i].snapshot.eax = 0;
        process_contexts[i].snapshot.ebx = 0;
        process_contexts[i].snapshot.ecx = 0;
        process_contexts[i].snapshot.edx = 0;
        process_contexts[i].snapshot.esi = 0;
        process_contexts[i].snapshot.edi = 0;
        process_contexts[i].snapshot.ebp = 0;
        process_contexts[i].snapshot.esp = 0;
        process_contexts[i].pid = i + 1u;
    }
    current_process_slot = 0;
    process_contexts[current_process_slot].task_state = TASK_STATE_RUNNING;
    /* Enable scheduler by default to run user/test tasks automatically. */
    scheduler_enabled = 1;
    scheduler_quantum_ticks = 100;
    scheduler_tick_accumulator = 0;
    scheduler_frame_restore_enabled = 0;
    scheduler_frame_restore_count = 0;
    scheduler_frame_restore_reject_count = 0;
}

static void scheduler_switch_to_slot(uint32_t slot)
{
    if (slot >= PROCESS_CONTEXT_MAX) {
        return;
    }

    if (slot != current_process_slot) {
        if (process_contexts[current_process_slot].task_state == TASK_STATE_RUNNING) {
            process_set_task_state(&process_contexts[current_process_slot], TASK_STATE_READY);
        }
        process_contexts[slot].context_switches++;
        current_process_slot = slot;
        process_set_task_state(&process_contexts[current_process_slot], TASK_STATE_RUNNING);
    }

    /* Switch to process-private page directory if present, else use global */
    if (paging_enabled) {
        uint32_t target_dir = process_contexts[current_process_slot].page_dir_phys;
        if (target_dir == 0) {
            target_dir = paging_directory_phys;
        }
        load_page_directory(target_dir);
    }
}

static int scheduler_rotate_process(void)
{
    uint32_t previous = current_process_slot;
    uint32_t start = current_process_slot;

    for (uint32_t i = 0; i < PROCESS_CONTEXT_MAX; i++) {
        uint32_t candidate = (start + 1u + i) % PROCESS_CONTEXT_MAX;
        if (process_contexts[candidate].runnable) {
            scheduler_switch_to_slot(candidate);
            return current_process_slot != previous;
        }
    }

    return 0;
}

static int scheduler_rotate_and_maybe_restore(struct interrupt_frame* frame)
{
    if (!scheduler_frame_restore_enabled || frame == 0) {
        (void)scheduler_rotate_process();
        return 0;
    }

    {
        uint32_t start = current_process_slot;
        for (uint32_t i = 0; i < PROCESS_CONTEXT_MAX; i++) {
            uint32_t candidate = (start + 1u + i) % PROCESS_CONTEXT_MAX;
            if (!process_contexts[candidate].runnable) {
                continue;
            }

            if (!process_snapshot_is_user_compatible(&process_contexts[candidate])) {
                continue;
            }

            if (candidate == current_process_slot) {
                return 0;
            }

            scheduler_switch_to_slot(candidate);
            if (process_restore_snapshot_to_frame(current_process_context(), frame)) {
                scheduler_frame_restore_count++;
                return 1;
            }
        }
    }

    if (runnable_process_count() > 1u) {
        scheduler_frame_restore_reject_count++;
    }

    return 0;
}

static void scheduler_wake_sleeping_tasks(void)
{
    uint32_t now = timer_ticks;

    for (uint32_t i = 0; i < PROCESS_CONTEXT_MAX; i++) {
        struct process_syscall_context* proc = &process_contexts[i];
        if (proc->task_state == TASK_STATE_BLOCKED && proc->wait_reason == WAIT_REASON_SLEEP &&
            proc->sleep_until_tick != 0 && now >= proc->sleep_until_tick) {
            proc->sleep_until_tick = 0;
            process_set_task_state(proc, TASK_STATE_READY);
        } else if (proc->task_state == TASK_STATE_BLOCKED && proc->wait_reason == WAIT_REASON_MANUAL &&
                   proc->wait_timeout_tick != 0 && now >= proc->wait_timeout_tick) {
            proc->wait_timeout_tick = 0;
            process_set_task_state(proc, TASK_STATE_READY);
        }
    }
}

static void scheduler_on_timer_tick(struct interrupt_frame* frame)
{
    if (!scheduler_enabled) {
        return;
    }

    scheduler_tick_accumulator++;
    if (scheduler_tick_accumulator >= scheduler_quantum_ticks) {
        scheduler_tick_accumulator = 0;
        (void)scheduler_rotate_and_maybe_restore(frame);
    }
}

static void scheduler_print_status(void)
{
    terminal_write("Scheduler enabled: ");
    terminal_write(scheduler_enabled ? "yes" : "no");
    terminal_write("\nQuantum ticks: ");
    terminal_write_dec_u32(scheduler_quantum_ticks);
    terminal_write("\nTick accumulator: ");
    terminal_write_dec_u32(scheduler_tick_accumulator);
    terminal_write("\nCurrent PID: ");
    terminal_write_dec_u32(current_process_slot + 1u);
    terminal_write("\nRunnable processes: ");
    terminal_write_dec_u32(runnable_process_count());
    terminal_write("\nFrame restore mode: ");
    terminal_write(scheduler_frame_restore_enabled ? "on" : "off");
    terminal_write("\nFrame restores: ");
    terminal_write_dec_u32(scheduler_frame_restore_count);
    terminal_write("\nRestore rejects: ");
    terminal_write_dec_u32(scheduler_frame_restore_reject_count);
    terminal_write("\n");
}

static void process_print_stats(void)
{
    terminal_write("Process statistics:\n");
    for (uint32_t i = 0; i < PROCESS_CONTEXT_MAX; i++) {
        struct process_syscall_context* p = &process_contexts[i];
        terminal_write("PID "); terminal_write_dec_u32(p->pid);
        terminal_write(": "); terminal_write(task_state_name(p->task_state));
        terminal_write(" runnable="); terminal_write(p->runnable ? "yes" : "no");
        terminal_write(" ctxsw="); terminal_write_dec_u32(p->context_switches);
        terminal_write(" syscalls="); terminal_write_dec_u32(p->syscall_count);
        terminal_write(" last_ret="); terminal_write_hex(p->last_ret);
        terminal_write(" exits="); terminal_write_dec_u32(p->exit_count);
        terminal_write(" last_exit="); terminal_write_hex(p->last_exit_code);
        terminal_write("\n");
    }
}

static int paging_is_user_virtual(uint32_t virtual_address)
{
    return virtual_address >= USER_SPACE_BASE && virtual_address < USER_SPACE_LIMIT;
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

static uint32_t parse_u32_decimal(const char* s, uint32_t* value_out)
{
    uint32_t value = 0;
    size_t i = 0;

    if (s[0] == '\0') {
        return 0;
    }

    while (s[i] != '\0') {
        char c = s[i];
        if (c < '0' || c > '9') {
            return 0;
        }
        value = value * 10u + (uint32_t)(c - '0');
        i++;
    }

    *value_out = value;
    return 1;
}

static int str_starts_with(const char* s, const char* prefix)
{
    size_t i = 0;
    while (prefix[i] != '\0') {
        if (s[i] != prefix[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

/* Forward declaration — defined in heap section below */
static void* kmalloc_aligned(uint32_t size, uint32_t alignment);

static int mbfs_component_valid(const char* name)
{
    uint32_t len = 0;

    if (name == 0 || name[0] == '\0') {
        return 0;
    }

    while (name[len] != '\0') {
        char c = name[len];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
            return 0;
        }
        len++;
        if (len >= MBFS_NAME_MAX) {
            return 0;
        }
    }

    return 1;
}

static int mbfs_path_next_component(const char* path, uint32_t* offset, char* component, uint32_t component_max)
{
    uint32_t cursor;
    uint32_t len = 0;

    if (path == 0 || offset == 0 || component == 0 || component_max == 0u) {
        return -1;
    }

    cursor = *offset;
    if (path[cursor] == '\0') {
        return 0;
    }
    if (path[cursor] == '/') {
        return -1;
    }

    while (path[cursor] != '\0' && path[cursor] != '/') {
        char c = path[cursor];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            return -1;
        }
        if (len + 1u >= component_max) {
            return -1;
        }
        component[len++] = c;
        cursor++;
    }

    if (len == 0u) {
        return -1;
    }

    component[len] = '\0';
    if (!mbfs_component_valid(component)) {
        return -1;
    }

    if (path[cursor] == '/') {
        cursor++;
        if (path[cursor] == '\0') {
            return -1;
        }
    }

    *offset = cursor;
    return 1;
}

static int ramfs_name_valid(const char* name)
{
    return mbfs_component_valid(name);
}

static int mbfs_push_component(char* path,
                               uint32_t path_max,
                               uint32_t* path_len,
                               uint32_t* depth,
                               uint32_t* undo_lengths,
                               const char* component)
{
    uint32_t restore_len;
    uint32_t write_len;

    if (path == 0 || path_len == 0 || depth == 0 || undo_lengths == 0 || !mbfs_component_valid(component)) {
        return 0;
    }

    restore_len = *path_len;
    write_len = restore_len;
    if (write_len != 0u) {
        if (write_len + 1u >= path_max) {
            return 0;
        }
        path[write_len++] = '/';
    }

    for (uint32_t i = 0; component[i] != '\0'; i++) {
        if (write_len + 1u >= path_max) {
            return 0;
        }
        path[write_len++] = component[i];
    }

    path[write_len] = '\0';
    undo_lengths[*depth] = restore_len;
    *path_len = write_len;
    (*depth)++;
    return 1;
}

static int mbfs_normalize_path(const char* cwd, const char* input, char* out, uint32_t out_max)
{
    char component[MBFS_NAME_MAX];
    uint32_t undo_lengths[MBFS_PATH_MAX];
    uint32_t depth = 0;
    uint32_t out_len = 0;
    uint32_t offset = 0;
    uint8_t absolute = 0u;

    if (input == 0 || out == 0 || out_max == 0u) {
        return 0;
    }

    out[0] = '\0';
    if (input[0] == '\0') {
        return 0;
    }

    if (cwd != 0 && cwd[0] != '\0') {
        uint32_t cwd_offset = 0;
        while (1) {
            int rc = mbfs_path_next_component(cwd, &cwd_offset, component, sizeof(component));
            if (rc == 0) {
                break;
            }
            if (rc < 0 || !mbfs_push_component(out, out_max, &out_len, &depth, undo_lengths, component)) {
                return 0;
            }
        }
    }

    while (input[offset] == '/') {
        absolute = 1u;
        offset++;
    }

    if (absolute) {
        depth = 0;
        out_len = 0;
        out[0] = '\0';
        if (input[offset] == '\0') {
            return 1;
        }
    }

    while (1) {
        int rc = mbfs_path_next_component(input, &offset, component, sizeof(component));
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            return 0;
        }

        if (str_equals(component, ".")) {
            continue;
        }
        if (str_equals(component, "..")) {
            if (depth > 0u) {
                depth--;
                out_len = undo_lengths[depth];
                out[out_len] = '\0';
            }
            continue;
        }

        if (!mbfs_push_component(out, out_max, &out_len, &depth, undo_lengths, component)) {
            return 0;
        }
    }

    return 1;
}

static void mbfs_terminal_write_path(const char* path)
{
    terminal_putchar('/');
    if (path != 0 && path[0] != '\0') {
        terminal_write(path);
    }
}

static int str_contains_char(const char* s, char needle)
{
    if (s == 0) {
        return 0;
    }

    for (uint32_t i = 0; s[i] != '\0'; i++) {
        if (s[i] == needle) {
            return 1;
        }
    }

    return 0;
}

static int32_t ramfs_find_index(const char* name)
{
    for (uint32_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!ramfs_files[i].used) {
            continue;
        }
        if (str_equals(ramfs_files[i].name, name)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t ramfs_alloc_slot(void)
{
    for (uint32_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!ramfs_files[i].used) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int ramfs_write_text(const char* name, const char* text)
{
    int32_t idx = ramfs_find_index(name);
    uint32_t text_len = (uint32_t)str_length(text);
    struct ramfs_file* file;

    if (!ramfs_name_valid(name)) {
        return -1;
    }
    if (text_len > RAMFS_FILE_MAX) {
        return -2;
    }

    if (idx < 0) {
        idx = ramfs_alloc_slot();
        if (idx < 0) {
            return -3;
        }
        ramfs_files[idx].used = 1;
        ramfs_files[idx].size = 0;
        ramfs_files[idx].data = (uint8_t*)kmalloc_aligned(RAMFS_FILE_MAX + 1u, 8u);
        if (ramfs_files[idx].data == 0) {
            ramfs_files[idx].used = 0;
            return -4;
        }
        str_copy_bounded(ramfs_files[idx].name, name, RAMFS_NAME_MAX);
        ramfs_file_count++;
    }

    file = &ramfs_files[idx];
    for (uint32_t i = 0; i < text_len; i++) {
        file->data[i] = (uint8_t)text[i];
    }
    file->data[text_len] = 0;
    file->size = text_len;
    return 0;
}

static int ramfs_remove(const char* name)
{
    int32_t idx = ramfs_find_index(name);
    if (idx < 0) {
        return 0;
    }

    ramfs_files[idx].used = 0;
    ramfs_files[idx].size = 0;
    ramfs_files[idx].name[0] = '\0';
    ramfs_file_count--;
    return 1;
}

static void ramfs_list(void)
{
    if (ramfs_file_count == 0) {
        terminal_write("ramfs empty\n");
        return;
    }

    for (uint32_t i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!ramfs_files[i].used) {
            continue;
        }
        terminal_write(ramfs_files[i].name);
        terminal_write(" (bytes=");
        terminal_write_dec_u32(ramfs_files[i].size);
        terminal_write(")\n");
    }
}

static void ramfs_cat(const char* name)
{
    int32_t idx = ramfs_find_index(name);
    if (idx < 0) {
        terminal_write("ramfs: file not found\n");
        return;
    }

    terminal_write((const char*)ramfs_files[idx].data);
    terminal_write("\n");
}

static int ramfs_write_bytes(const char* name, const uint8_t* bytes, uint32_t size)
{
    int32_t idx = ramfs_find_index(name);
    struct ramfs_file* file;

    if (!ramfs_name_valid(name)) {
        return -1;
    }
    if (size > RAMFS_FILE_MAX) {
        return -2;
    }

    if (idx < 0) {
        idx = ramfs_alloc_slot();
        if (idx < 0) {
            return -3;
        }
        ramfs_files[idx].used = 1;
        ramfs_files[idx].size = 0;
        ramfs_files[idx].data = (uint8_t*)kmalloc_aligned(RAMFS_FILE_MAX + 1u, 8u);
        if (ramfs_files[idx].data == 0) {
            ramfs_files[idx].used = 0;
            return -4;
        }
        str_copy_bounded(ramfs_files[idx].name, name, RAMFS_NAME_MAX);
        ramfs_file_count++;
    }

    file = &ramfs_files[idx];
    for (uint32_t i = 0; i < size; i++) {
        file->data[i] = bytes[i];
    }
    file->data[size] = 0;
    file->size = size;
    return 0;
}

static void ata_io_delay(uint16_t ctrl_base)
{
    (void)inb(ctrl_base);
    (void)inb(ctrl_base);
    (void)inb(ctrl_base);
    (void)inb(ctrl_base);
}

static int ata_wait_not_busy(const struct ata_device* dev)
{
    for (uint32_t spin = 0; spin < 100000u; spin++) {
        uint8_t status = inb(dev->io_base + ATA_REG_STATUS);
        if ((status & ATA_SR_BSY) == 0u) {
            return 1;
        }
    }
    return 0;
}

static int ata_wait_drq(const struct ata_device* dev)
{
    for (uint32_t spin = 0; spin < 100000u; spin++) {
        uint8_t status = inb(dev->io_base + ATA_REG_STATUS);
        if ((status & ATA_SR_BSY) != 0u) {
            continue;
        }
        if ((status & (ATA_SR_ERR | ATA_SR_DF)) != 0u) {
            return 0;
        }
        if ((status & ATA_SR_DRQ) != 0u) {
            return 1;
        }
    }
    return 0;
}

static void ata_probe_device(struct ata_device* dev, uint16_t io_base, uint16_t ctrl_base, uint8_t slave)
{
    uint16_t identify_words[256];
    uint8_t drive_select = (uint8_t)(0xA0u | (slave ? 0x10u : 0u));
    uint8_t status;
    uint8_t sig_mid;
    uint8_t sig_high;

    memory_set_u8(dev, 0, sizeof(*dev));
    dev->io_base = io_base;
    dev->ctrl_base = ctrl_base;
    dev->slave = slave;

    outb(ctrl_base, 0u);
    outb(io_base + ATA_REG_HDDEVSEL, drive_select);
    ata_io_delay(ctrl_base);

    outb(io_base + ATA_REG_SECCOUNT0, 0u);
    outb(io_base + ATA_REG_LBA0, 0u);
    outb(io_base + ATA_REG_LBA1, 0u);
    outb(io_base + ATA_REG_LBA2, 0u);
    outb(io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    status = inb(io_base + ATA_REG_STATUS);
    if (status == 0u || status == 0xFFu) {
        return;
    }

    for (uint32_t spin = 0; spin < 100000u && (status & ATA_SR_BSY) != 0u; spin++) {
        status = inb(io_base + ATA_REG_STATUS);
    }
    if ((status & ATA_SR_BSY) != 0u) {
        return;
    }

    sig_mid = inb(io_base + ATA_REG_LBA1);
    sig_high = inb(io_base + ATA_REG_LBA2);
    if (sig_mid != 0u || sig_high != 0u) {
        dev->atapi = 1;
        return;
    }

    if (!ata_wait_drq(dev)) {
        return;
    }

    for (uint32_t i = 0; i < 256u; i++) {
        identify_words[i] = inw(io_base + ATA_REG_DATA);
    }

    dev->total_sectors = (uint32_t)identify_words[60] | ((uint32_t)identify_words[61] << 16);
    dev->present = (dev->total_sectors != 0u) ? 1u : 0u;
}

static int ata_pio_read_sector(const struct ata_device* dev, uint32_t lba, uint8_t* out)
{
    if (dev == 0 || !dev->present || out == 0 || lba >= dev->total_sectors || lba > 0x0FFFFFFFu) {
        return 0;
    }

    if (!ata_wait_not_busy(dev)) {
        return 0;
    }

    outb(dev->ctrl_base, 0u);
    outb(dev->io_base + ATA_REG_HDDEVSEL,
         (uint8_t)(0xE0u | (dev->slave ? 0x10u : 0u) | ((lba >> 24) & 0x0Fu)));
    outb(dev->io_base + ATA_REG_SECCOUNT0, 1u);
    outb(dev->io_base + ATA_REG_LBA0, (uint8_t)(lba & 0xFFu));
    outb(dev->io_base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFFu));
    outb(dev->io_base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFFu));
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    if (!ata_wait_drq(dev)) {
        return 0;
    }

    for (uint32_t i = 0; i < DISKFS_SECTOR_SIZE / 2u; i++) {
        uint16_t value = inw(dev->io_base + ATA_REG_DATA);
        out[i * 2u] = (uint8_t)(value & 0xFFu);
        out[i * 2u + 1u] = (uint8_t)(value >> 8);
    }

    ata_io_delay(dev->ctrl_base);
    return 1;
}

static int ata_pio_write_sector(const struct ata_device* dev, uint32_t lba, const uint8_t* in)
{
    if (dev == 0 || !dev->present || in == 0 || lba >= dev->total_sectors || lba > 0x0FFFFFFFu) {
        return 0;
    }

    if (auto_run_disk_tests) {
        serial_write("ata_pio_write_sector: dev="); serial_write_dec_u32((uint32_t)(uintptr_t)dev);
        serial_write(" lba="); serial_write_dec_u32(lba);
        serial_write(" in="); serial_write_dec_u32((uint32_t)(uintptr_t)in);
        serial_write("\n");
    }

    if (!ata_wait_not_busy(dev)) {
        return 0;
    }

    outb(dev->ctrl_base, 0u);
    outb(dev->io_base + ATA_REG_HDDEVSEL,
         (uint8_t)(0xE0u | (dev->slave ? 0x10u : 0u) | ((lba >> 24) & 0x0Fu)));
    outb(dev->io_base + ATA_REG_SECCOUNT0, 1u);
    outb(dev->io_base + ATA_REG_LBA0, (uint8_t)(lba & 0xFFu));
    outb(dev->io_base + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFFu));
    outb(dev->io_base + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFFu));
    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);

    if (!ata_wait_drq(dev)) {
        return 0;
    }

    for (uint32_t i = 0; i < DISKFS_SECTOR_SIZE / 2u; i++) {
        uint16_t value = (uint16_t)in[i * 2u] | ((uint16_t)in[i * 2u + 1u] << 8);
        outw(dev->io_base + ATA_REG_DATA, value);
    }

    outb(dev->io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_not_busy(dev);
}

static void ata_storage_init(void)
{
    ata_probe_device(&ata_devices[0], 0x1F0u, 0x3F6u, 0u);
    ata_probe_device(&ata_devices[1], 0x1F0u, 0x3F6u, 1u);
    ata_probe_device(&ata_devices[2], 0x170u, 0x376u, 0u);
    ata_probe_device(&ata_devices[3], 0x170u, 0x376u, 1u);

    diskfs_device = 0;
    for (uint32_t i = 0; i < 4u; i++) {
        if (ata_devices[i].present) {
            diskfs_device = &ata_devices[i];
            break;
        }
    }
}

static uint32_t diskfs_bytes_to_sectors(uint32_t size)
{
    if (size == 0u) {
        return 0u;
    }
    return align_up_u32(size, DISKFS_SECTOR_SIZE) / DISKFS_SECTOR_SIZE;
}

static int mbfs_bitmap_get(uint32_t data_sector_index)
{
    uint32_t byte_index = data_sector_index >> 3;
    uint8_t bit = (uint8_t)(1u << (data_sector_index & 7u));
    if (byte_index >= sizeof(mbfs_bitmap)) {
        return 1;
    }
    return (mbfs_bitmap[byte_index] & bit) != 0u;
}

static void mbfs_bitmap_set(uint32_t data_sector_index, uint8_t used)
{
    uint32_t byte_index = data_sector_index >> 3;
    uint8_t bit = (uint8_t)(1u << (data_sector_index & 7u));
    if (byte_index >= sizeof(mbfs_bitmap)) {
        return;
    }
    if (used) {
        mbfs_bitmap[byte_index] |= bit;
    } else {
        mbfs_bitmap[byte_index] &= (uint8_t)(~bit);
    }
}

static void mbfs_mark_extent(const struct mbfs_extent* ext, uint8_t used)
{
    if (ext == 0 || ext->sector_count == 0u) {
        return;
    }
    if (ext->start_lba < mbfs_superblock.data_lba) {
        return;
    }

    uint32_t start_index = ext->start_lba - mbfs_superblock.data_lba;
    for (uint32_t i = 0; i < ext->sector_count; i++) {
        mbfs_bitmap_set(start_index + i, used);
    }
}

static int mbfs_alloc_extent(uint32_t sectors_needed, struct mbfs_extent* out)
{
    if (out == 0) {
        return 0;
    }
    out->start_lba = 0;
    out->sector_count = 0;

    if (sectors_needed == 0u) {
        return 1;
    }

    uint32_t run_start = 0u;
    uint32_t run_len = 0u;
    for (uint32_t i = 0; i < mbfs_superblock.data_sector_count; i++) {
        if (!mbfs_bitmap_get(i)) {
            if (run_len == 0u) {
                run_start = i;
            }
            run_len++;
            if (run_len >= sectors_needed) {
                out->start_lba = mbfs_superblock.data_lba + run_start;
                out->sector_count = sectors_needed;
                for (uint32_t j = 0; j < sectors_needed; j++) {
                    mbfs_bitmap_set(run_start + j, 1u);
                }
                return 1;
            }
        } else {
            run_len = 0u;
        }
    }

    return 0;
}

static int diskfs_load_superblock(void)
{
    uint8_t sector[DISKFS_SECTOR_SIZE];

    if (diskfs_device == 0) {
        return 0;
    }
    if (!ata_pio_read_sector(diskfs_device, MBFS_SUPER_LBA, sector)) {
        return 0;
    }

    memory_copy(&mbfs_superblock, sector, sizeof(mbfs_superblock));
    if (mbfs_superblock.magic != MBFS_MAGIC || mbfs_superblock.version != MBFS_VERSION) {
        return 0;
    }
    if (mbfs_superblock.inode_table_lba != MBFS_INODE_TABLE_LBA
            || mbfs_superblock.inode_table_sectors != MBFS_INODE_TABLE_SECTORS
            || mbfs_superblock.dir_lba != MBFS_DIR_LBA
            || mbfs_superblock.dir_sectors != MBFS_DIR_SECTORS
            || mbfs_superblock.bitmap_lba != MBFS_BITMAP_LBA
            || mbfs_superblock.bitmap_sectors != MBFS_BITMAP_SECTORS
            || mbfs_superblock.data_lba != MBFS_DATA_LBA
            || mbfs_superblock.max_inodes != MBFS_MAX_INODES
            || mbfs_superblock.max_dir_entries != MBFS_MAX_DIRENTS) {
        return 0;
    }
    if (mbfs_superblock.total_sectors == 0u || mbfs_superblock.total_sectors > diskfs_device->total_sectors) {
        return 0;
    }
    if (mbfs_superblock.data_sector_count > MBFS_BITMAP_SECTORS * DISKFS_SECTOR_SIZE * 8u) {
        return 0;
    }
    return 1;
}

static int mbfs_load_inodes(void)
{
    uint8_t sector[DISKFS_SECTOR_SIZE];

    for (uint32_t i = 0; i < MBFS_INODE_TABLE_SECTORS; i++) {
        if (!ata_pio_read_sector(diskfs_device, MBFS_INODE_TABLE_LBA + i, sector)) {
            return 0;
        }
        memory_copy(((uint8_t*)mbfs_inodes) + i * DISKFS_SECTOR_SIZE, sector, DISKFS_SECTOR_SIZE);
    }
    return 1;
}

static int mbfs_flush_inodes(void)
{
    /* Use global sector buffer and serialize ATA writes */
    irq_disable();
    for (uint32_t i = 0; i < MBFS_INODE_TABLE_SECTORS; i++) {
        memory_copy(ata_global_sector_buf, ((const uint8_t*)mbfs_inodes) + i * DISKFS_SECTOR_SIZE, DISKFS_SECTOR_SIZE);
        if (!ata_pio_write_sector(diskfs_device, MBFS_INODE_TABLE_LBA + i, ata_global_sector_buf)) {
            irq_enable();
            return 0;
        }
    }
    irq_enable();
    return 1;
}

static int mbfs_load_directory(void)
{
    uint8_t sector[DISKFS_SECTOR_SIZE];

    for (uint32_t i = 0; i < MBFS_DIR_SECTORS; i++) {
        if (!ata_pio_read_sector(diskfs_device, MBFS_DIR_LBA + i, sector)) {
            return 0;
        }
        memory_copy(((uint8_t*)mbfs_dir) + i * DISKFS_SECTOR_SIZE, sector, DISKFS_SECTOR_SIZE);
    }
    return 1;
}

static int mbfs_flush_directory(void)
{
    /* Use global sector buffer and serialize ATA writes */
    irq_disable();
    for (uint32_t i = 0; i < MBFS_DIR_SECTORS; i++) {
        memory_copy(ata_global_sector_buf, ((const uint8_t*)mbfs_dir) + i * DISKFS_SECTOR_SIZE, DISKFS_SECTOR_SIZE);
        if (!ata_pio_write_sector(diskfs_device, MBFS_DIR_LBA + i, ata_global_sector_buf)) {
            irq_enable();
            return 0;
        }
    }
    irq_enable();
    return 1;
}

static int mbfs_load_bitmap(void)
{
    uint8_t sector[DISKFS_SECTOR_SIZE];

    for (uint32_t i = 0; i < MBFS_BITMAP_SECTORS; i++) {
        if (!ata_pio_read_sector(diskfs_device, MBFS_BITMAP_LBA + i, sector)) {
            return 0;
        }
        memory_copy(mbfs_bitmap + i * DISKFS_SECTOR_SIZE, sector, DISKFS_SECTOR_SIZE);
    }
    return 1;
}

static int mbfs_flush_bitmap(void)
{
    /* Use global sector buffer and serialize ATA writes */
    irq_disable();
    for (uint32_t i = 0; i < MBFS_BITMAP_SECTORS; i++) {
        memory_copy(ata_global_sector_buf, mbfs_bitmap + i * DISKFS_SECTOR_SIZE, DISKFS_SECTOR_SIZE);
        if (!ata_pio_write_sector(diskfs_device, MBFS_BITMAP_LBA + i, ata_global_sector_buf)) {
            irq_enable();
            return 0;
        }
    }
    irq_enable();
    return 1;
}

static int mbfs_flush_superblock(void)
{
    /* Write superblock using global buffer and serialize ATA write */
    memory_set_u8(ata_global_sector_buf, 0, DISKFS_SECTOR_SIZE);
    memory_copy(ata_global_sector_buf, &mbfs_superblock, sizeof(mbfs_superblock));
    irq_disable();
    int rc = ata_pio_write_sector(diskfs_device, MBFS_SUPER_LBA, ata_global_sector_buf) ? 1 : 0;
    irq_enable();
    return rc;
}

static int mbfs_set_clean_flag(uint32_t clean)
{
    mbfs_superblock.clean_shutdown = clean ? 1u : 0u;
    return mbfs_flush_superblock();
}

static uint32_t mbfs_file_size_by_inode_index(uint8_t inode_idx)
{
    if (inode_idx >= MBFS_MAX_INODES || !mbfs_inodes[inode_idx].used) {
        return 0u;
    }

    if (mbfs_inodes[inode_idx].type != MBFS_INODE_TYPE_FILE) {
        return 0u;
    }

    return mbfs_inodes[inode_idx].size_bytes;
}

static int32_t mbfs_read_by_inode_index(uint8_t inode_idx, uint32_t offset, uint8_t* out, uint32_t count)
{
    struct mbfs_inode* inode;
    uint32_t copied = 0;
    uint32_t cursor = 0;

    if (out == 0 || count == 0 || inode_idx >= MBFS_MAX_INODES) {
        return -1;
    }

    inode = &mbfs_inodes[inode_idx];
    if (!inode->used || inode->type != 1u) {
        return -1;
    }
    if (offset >= inode->size_bytes) {
        return 0;
    }

    for (uint32_t e = 0; e < inode->extent_count && e < MBFS_INODE_EXTENTS && copied < count; e++) {
        uint32_t extent_bytes = inode->extents[e].sector_count * DISKFS_SECTOR_SIZE;
        if (offset >= cursor + extent_bytes) {
            cursor += extent_bytes;
            continue;
        }

        uint32_t offset_in_extent = (offset > cursor) ? (offset - cursor) : 0u;
        uint32_t sector_index = offset_in_extent / DISKFS_SECTOR_SIZE;
        uint32_t sector_off = offset_in_extent % DISKFS_SECTOR_SIZE;

        for (uint32_t s = sector_index; s < inode->extents[e].sector_count && copied < count; s++) {
            uint8_t sector[DISKFS_SECTOR_SIZE];
            uint32_t avail;
            uint32_t limit_by_size;
            uint32_t chunk;

            if (!ata_pio_read_sector(diskfs_device, inode->extents[e].start_lba + s, sector)) {
                return -1;
            }

            avail = DISKFS_SECTOR_SIZE - sector_off;
            limit_by_size = inode->size_bytes - (cursor + s * DISKFS_SECTOR_SIZE + sector_off);
            chunk = count - copied;
            if (chunk > avail) {
                chunk = avail;
            }
            if (chunk > limit_by_size) {
                chunk = limit_by_size;
            }

            memory_copy(out + copied, sector + sector_off, chunk);
            copied += chunk;
            sector_off = 0u;

            if (chunk == 0u || copied >= count || copied + offset >= inode->size_bytes) {
                break;
            }
        }

        cursor += extent_bytes;
    }

    return (int32_t)copied;
}

static int diskfs_write_bytes(const char* name, const uint8_t* data, uint32_t size)
{
    uint32_t sectors;
    int32_t dir_idx;
    int32_t inode_idx;
    struct mbfs_inode* inode;
    struct mbfs_extent ext;

    if (!diskfs_mounted || data == 0) {
        return -1;
    }

    if (auto_run_disk_tests) {
        terminal_write("diskfs_write_bytes: name="); terminal_write(name); terminal_write(" size="); serial_write_dec_u32(size); terminal_write("\n");
    }

    
    if (!ramfs_name_valid(name)) {
        return -2;
    }

    sectors = diskfs_bytes_to_sectors(size);
    dir_idx = diskfs_find_index(name);
    if (auto_run_disk_tests) {
        terminal_write("diskfs_write_bytes: sectors="); serial_write_dec_u32(sectors); terminal_write(" dir_idx="); serial_write_dec_u32((uint32_t)dir_idx); terminal_write("\n");
    }
    if (dir_idx < 0) {
        dir_idx = mbfs_alloc_dir_slot();
        terminal_write("diskfs_write_bytes: mbfs_alloc_dir_slot -> "); serial_write_dec_u32((uint32_t)dir_idx); terminal_write("\n");
        if (dir_idx < 0) {
            return -3;
        }
        inode_idx = mbfs_alloc_inode_slot();
        terminal_write("diskfs_write_bytes: mbfs_alloc_inode_slot -> "); serial_write_dec_u32((uint32_t)inode_idx); terminal_write("\n");
        if (inode_idx < 0) {
            return -3;
        }
        memory_set_u8(&mbfs_dir[(uint32_t)dir_idx], 0, sizeof(mbfs_dir[(uint32_t)dir_idx]));
        mbfs_dir[(uint32_t)dir_idx].used = 1;
        mbfs_dir[(uint32_t)dir_idx].inode_index = (uint8_t)inode_idx;
        mbfs_dir[(uint32_t)dir_idx].type = 1u;
        str_copy_bounded(mbfs_dir[(uint32_t)dir_idx].name, name, MBFS_NAME_MAX);
        terminal_write("diskfs_write_bytes: dir name set -> "); terminal_write(mbfs_dir[(uint32_t)dir_idx].name); terminal_write("\n");
        memory_set_u8(&mbfs_inodes[(uint32_t)inode_idx], 0, sizeof(mbfs_inodes[(uint32_t)inode_idx]));
        mbfs_inodes[(uint32_t)inode_idx].used = 1;
        mbfs_inodes[(uint32_t)inode_idx].type = 1u;
        terminal_write("diskfs_write_bytes: inode init done idx="); serial_write_dec_u32((uint32_t)inode_idx); terminal_write("\n");
    } else {
        inode_idx = mbfs_dir[(uint32_t)dir_idx].inode_index;
        if (inode_idx < 0 || inode_idx >= (int32_t)MBFS_MAX_INODES) {
            return -6;
        }
    }

    inode = &mbfs_inodes[(uint32_t)inode_idx];

    /* Diagnostic: print ATA device info before writing superblock */
    if (diskfs_device == 0) {
        terminal_write("diskfs_write_bytes: diskfs_device is NULL\n");
    } else {
        terminal_write("diskfs_write_bytes: diskfs_device ptr="); serial_write_dec_u32((uint32_t)(uintptr_t)diskfs_device);
        terminal_write(" present="); serial_write_dec_u32((uint32_t)diskfs_device->present);
        terminal_write(" io_base="); serial_write_dec_u32((uint32_t)diskfs_device->io_base);
        terminal_write(" ctrl_base="); serial_write_dec_u32((uint32_t)diskfs_device->ctrl_base);
        terminal_write(" total_sectors="); serial_write_dec_u32((uint32_t)diskfs_device->total_sectors);
        terminal_write("\n");
    }

    /* Temporarily skip clearing clean flag to isolate crash */
    terminal_write("diskfs_write_bytes: skipping mbfs_set_clean_flag(0) for debug\n");

    for (uint32_t i = 0; i < inode->extent_count && i < MBFS_INODE_EXTENTS; i++) {
        mbfs_mark_extent(&inode->extents[i], 0u);
    }
    memory_set_u8(inode->extents, 0, sizeof(inode->extents));
    inode->extent_count = 0u;

    if (sectors > 0u) {
        if (!mbfs_alloc_extent(sectors, &ext)) {
            return -4;
        }

        if (auto_run_disk_tests) {
            terminal_write("diskfs_write_bytes: allocated extent start="); serial_write_dec_u32(ext.start_lba); terminal_write(" cnt="); serial_write_dec_u32(ext.sector_count); terminal_write("\n");
        }

        inode->extents[0] = ext;
        inode->extent_count = 1u;

        irq_disable();
        for (uint32_t s = 0; s < sectors; s++) {
            uint32_t base = s * DISKFS_SECTOR_SIZE;
            uint32_t chunk = size - base;
            if (chunk > DISKFS_SECTOR_SIZE) {
                chunk = DISKFS_SECTOR_SIZE;
            }
            memory_set_u8(ata_global_sector_buf, 0, DISKFS_SECTOR_SIZE);
            memory_copy(ata_global_sector_buf, data + base, chunk);
            if (auto_run_disk_tests) {
                terminal_write("diskfs_write_bytes: writing sector "); serial_write_dec_u32(s);
                terminal_write(" -> LBA "); serial_write_dec_u32(ext.start_lba + s);
                terminal_write("\n");
            }
            /* Diagnostic: print buffer/source addresses to catch pointer corruption */
            if (auto_run_disk_tests) {
                terminal_write("diskfs_write_bytes: sector_buf_addr="); serial_write_dec_u32((uint32_t)(uintptr_t)ata_global_sector_buf);
                terminal_write(" src_addr="); serial_write_dec_u32((uint32_t)(uintptr_t)(data + base)); terminal_write("\n");
            }
            if (!ata_pio_write_sector(diskfs_device, ext.start_lba + s, ata_global_sector_buf)) {
                if (auto_run_disk_tests) {
                    terminal_write("diskfs_write_bytes: ata_pio_write_sector failed at LBA "); serial_write_dec_u32(ext.start_lba + s); terminal_write("\n");
                }
                irq_enable();
                return -5;
            }
        }
        irq_enable();
    }

    inode->used = 1u;
    inode->type = 1u;
    inode->size_bytes = size;

    if (!mbfs_flush_inodes() || !mbfs_flush_directory() || !mbfs_flush_bitmap()) {
        return -6;
    }
    if (!mbfs_set_clean_flag(1u)) {
        return -6;
    }
    return 0;
}

static int diskfs_try_mount(void)
{
    diskfs_mounted = 0;
    mbfs_unclean_last_mount = 0;
    memory_set_u8(&mbfs_superblock, 0, sizeof(mbfs_superblock));
    memory_set_u8(mbfs_inodes, 0, sizeof(mbfs_inodes));
    memory_set_u8(mbfs_dir, 0, sizeof(mbfs_dir));
    memory_set_u8(mbfs_bitmap, 0, sizeof(mbfs_bitmap));

    if (!diskfs_load_superblock()) {
        return 0;
    }
    if (!mbfs_load_inodes() || !mbfs_load_directory() || !mbfs_load_bitmap()) {
        return 0;
    }

    if (mbfs_superblock.clean_shutdown == 0u) {
        mbfs_unclean_last_mount = 1;
    }

    diskfs_mounted = 1;
    return 1;
}

static int32_t diskfs_find_index(const char* name)
{
    for (uint32_t i = 0; i < MBFS_MAX_DIRENTS; i++) {
        if (!mbfs_dir[i].used) {
            continue;
        }
        if (str_equals(mbfs_dir[i].name, name)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t mbfs_alloc_dir_slot(void)
{
    for (uint32_t i = 0; i < MBFS_MAX_DIRENTS; i++) {
        if (!mbfs_dir[i].used) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t mbfs_alloc_inode_slot(void)
{
    for (uint32_t i = 0; i < MBFS_MAX_INODES; i++) {
        if (!mbfs_inodes[i].used) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int diskfs_format(void)
{
    if (diskfs_device == 0) {
        return -1;
    }
    if (diskfs_device->total_sectors <= MBFS_DATA_LBA) {
        return -2;
    }

    memory_set_u8(&mbfs_superblock, 0, sizeof(mbfs_superblock));
    mbfs_superblock.magic = MBFS_MAGIC;
    mbfs_superblock.version = MBFS_VERSION;
    mbfs_superblock.total_sectors = diskfs_device->total_sectors;
    mbfs_superblock.inode_table_lba = MBFS_INODE_TABLE_LBA;
    mbfs_superblock.inode_table_sectors = MBFS_INODE_TABLE_SECTORS;
    mbfs_superblock.dir_lba = MBFS_DIR_LBA;
    mbfs_superblock.dir_sectors = MBFS_DIR_SECTORS;
    mbfs_superblock.bitmap_lba = MBFS_BITMAP_LBA;
    mbfs_superblock.bitmap_sectors = MBFS_BITMAP_SECTORS;
    mbfs_superblock.data_lba = MBFS_DATA_LBA;
    mbfs_superblock.data_sector_count = diskfs_device->total_sectors - MBFS_DATA_LBA;
    if (mbfs_superblock.data_sector_count > MBFS_BITMAP_SECTORS * DISKFS_SECTOR_SIZE * 8u) {
        mbfs_superblock.data_sector_count = MBFS_BITMAP_SECTORS * DISKFS_SECTOR_SIZE * 8u;
    }
    mbfs_superblock.max_inodes = MBFS_MAX_INODES;
    mbfs_superblock.max_dir_entries = MBFS_MAX_DIRENTS;
    mbfs_superblock.clean_shutdown = 1u;

    /* Serialize all format writes to avoid re-entrancy during PIO */
    irq_disable();
    memory_set_u8(ata_global_sector_buf, 0, DISKFS_SECTOR_SIZE);
    memory_copy(ata_global_sector_buf, &mbfs_superblock, sizeof(mbfs_superblock));
    if (!ata_pio_write_sector(diskfs_device, MBFS_SUPER_LBA, ata_global_sector_buf)) {
        irq_enable();
        return -3;
    }

    memory_set_u8(mbfs_inodes, 0, sizeof(mbfs_inodes));
    memory_set_u8(mbfs_dir, 0, sizeof(mbfs_dir));
    memory_set_u8(mbfs_bitmap, 0, sizeof(mbfs_bitmap));

    memory_set_u8(ata_global_sector_buf, 0, DISKFS_SECTOR_SIZE);
    for (uint32_t i = 0; i < MBFS_INODE_TABLE_SECTORS; i++) {
        if (!ata_pio_write_sector(diskfs_device, MBFS_INODE_TABLE_LBA + i, ata_global_sector_buf)) {
            irq_enable();
            return -4;
        }
    }
    for (uint32_t i = 0; i < MBFS_DIR_SECTORS; i++) {
        if (!ata_pio_write_sector(diskfs_device, MBFS_DIR_LBA + i, ata_global_sector_buf)) {
            irq_enable();
            return -5;
        }
    }
    for (uint32_t i = 0; i < MBFS_BITMAP_SECTORS; i++) {
        if (!ata_pio_write_sector(diskfs_device, MBFS_BITMAP_LBA + i, ata_global_sector_buf)) {
            irq_enable();
            return -6;
        }
    }
    irq_enable();

    diskfs_mounted = 1;
    return 0;
}
/*
static void diskfs_list(void)
{
    if (!diskfs_mounted) {
        terminal_write("diskfs not mounted\n");
        return;
    }
    for (uint32_t i = 0; i < MBFS_MAX_DIRENTS; i++) {
        if (!mbfs_dir[i].used) {
            continue;
        }
        uint8_t inode_idx = mbfs_dir[i].inode_index;
        if (inode_idx >= MBFS_MAX_INODES || !mbfs_inodes[inode_idx].used) {
            continue;
        }
        terminal_write(mbfs_dir[i].name);
        if (mbfs_inodes[inode_idx].type == MBFS_INODE_TYPE_DIR) {
            terminal_write("/ [dir]\n");
        } else {
            terminal_write(" (bytes=");
            terminal_write_dec_u32(mbfs_inodes[inode_idx].size_bytes);
            terminal_write(" extents=");
            terminal_write_dec_u32(mbfs_inodes[inode_idx].extent_count);
            terminal_write(")\n");
        }
    }
}
*/
/*
static void diskfs_cat(const char* name)
{
    uint8_t sector[DISKFS_SECTOR_SIZE + 1u];
    int32_t dir_idx = diskfs_find_index(name);
    uint32_t remaining;

    if (!diskfs_mounted) {
        terminal_write("diskfs not mounted\n");
        return;
    }
    if (dir_idx < 0) {
        terminal_write("diskfs: file not found\n");
        return;
    }

    if (mbfs_dir[(uint32_t)dir_idx].inode_index >= MBFS_MAX_INODES) {
        terminal_write("diskfs: inode invalid\n");
        return;
    }

    struct mbfs_inode* inode = &mbfs_inodes[mbfs_dir[(uint32_t)dir_idx].inode_index];
    if (!inode->used || inode->type != 1u) {
        terminal_write("diskfs: inode not a file\n");
        return;
    }

    remaining = inode->size_bytes;
    for (uint32_t e = 0; e < inode->extent_count && e < MBFS_INODE_EXTENTS; e++) {
        for (uint32_t s = 0; s < inode->extents[e].sector_count; s++) {
            uint32_t chunk = remaining > DISKFS_SECTOR_SIZE ? DISKFS_SECTOR_SIZE : remaining;
            if (!ata_pio_read_sector(diskfs_device, inode->extents[e].start_lba + s, sector)) {
                terminal_write("diskfs: read error\n");
                return;
            }
            sector[chunk] = 0;
            terminal_write((const char*)sector);
            if (remaining <= chunk) {
                terminal_write("\n");
                return;
            }
            remaining -= chunk;
        }
    }
    terminal_write("\n");
}
*/
/*
static int diskfs_remove(const char* name)
{
    int32_t dir_idx;
    uint8_t inode_idx;

    if (!diskfs_mounted) {
        return 0;
    }
    dir_idx = diskfs_find_index(name);
    if (dir_idx < 0) {
        return 0;
    }

    if (!mbfs_set_clean_flag(0u)) {
        return 0;
    }

    inode_idx = mbfs_dir[(uint32_t)dir_idx].inode_index;
    if (inode_idx < MBFS_MAX_INODES && mbfs_inodes[inode_idx].used) {
        for (uint32_t i = 0; i < mbfs_inodes[inode_idx].extent_count && i < MBFS_INODE_EXTENTS; i++) {
            mbfs_mark_extent(&mbfs_inodes[inode_idx].extents[i], 0u);
        }
        memory_set_u8(&mbfs_inodes[inode_idx], 0, sizeof(mbfs_inodes[inode_idx]));
    }

    memory_set_u8(&mbfs_dir[(uint32_t)dir_idx], 0, sizeof(mbfs_dir[(uint32_t)dir_idx]));
    if (!mbfs_flush_inodes() || !mbfs_flush_directory() || !mbfs_flush_bitmap()) {
        return 0;
    }
    return mbfs_set_clean_flag(1u);
}
*/
/* ---- clean unmount ---- */

static void diskfs_umount(void)
{
    if (!diskfs_mounted) {
        return;
    }
    mbfs_set_clean_flag(1u);
    memory_set_u8(&mbfs_superblock, 0, sizeof(mbfs_superblock));
    memory_set_u8(mbfs_inodes, 0, sizeof(mbfs_inodes));
    memory_set_u8(mbfs_dir, 0, sizeof(mbfs_dir));
    memory_set_u8(mbfs_bitmap, 0, sizeof(mbfs_bitmap));
    memory_set_u8(mbfs_subdir_scratch, 0, sizeof(mbfs_subdir_scratch));
    for (uint32_t i = 0; i < PROCESS_CONTEXT_MAX; i++) {
        process_contexts[i].cwd[0] = '\0';
    }
    diskfs_mounted = 0;
    mbfs_unclean_last_mount = 0;
}

/* ---- nested directory helpers ---- */

static int32_t mbfs_find_in_entries(const struct mbfs_dir_entry* entries,
                                     uint32_t count, const char* name)
{
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].used && str_equals(entries[i].name, name)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int mbfs_load_subdir_entries(uint8_t inode_idx,
                                     struct mbfs_dir_entry* out,
                                     uint32_t max_count)
{
    uint8_t sector[DISKFS_SECTOR_SIZE];
    uint32_t total_bytes;
    uint32_t copied;
    struct mbfs_inode* inode;

    if (inode_idx >= MBFS_MAX_INODES || !mbfs_inodes[inode_idx].used) {
        return 0;
    }
    if (mbfs_inodes[inode_idx].type != MBFS_INODE_TYPE_DIR) {
        return 0;
    }
    if (out == 0 || max_count == 0) {
        return 0;
    }

    memory_set_u8(out, 0, max_count * sizeof(struct mbfs_dir_entry));

    inode = &mbfs_inodes[inode_idx];
    if (inode->extent_count == 0) {
        return 1;
    }

    total_bytes = max_count * sizeof(struct mbfs_dir_entry);
    copied = 0;

    for (uint32_t e = 0; e < inode->extent_count && e < MBFS_INODE_EXTENTS && copied < total_bytes; e++) {
        for (uint32_t s = 0; s < inode->extents[e].sector_count && copied < total_bytes; s++) {
            uint32_t chunk;

            if (!ata_pio_read_sector(diskfs_device, inode->extents[e].start_lba + s, sector)) {
                return 0;
            }
            chunk = total_bytes - copied;
            if (chunk > DISKFS_SECTOR_SIZE) {
                chunk = DISKFS_SECTOR_SIZE;
            }
            memory_copy((uint8_t*)out + copied, sector, chunk);
            copied += chunk;
        }
    }
    return 1;
}

static int mbfs_flush_subdir_entries(uint8_t inode_idx,
                                      struct mbfs_dir_entry* entries,
                                      uint32_t max_count)
{
    uint8_t sector[DISKFS_SECTOR_SIZE];
    uint32_t total_bytes;
    uint32_t written;
    struct mbfs_inode* inode;

    if (inode_idx >= MBFS_MAX_INODES || !mbfs_inodes[inode_idx].used) {
        return 0;
    }
    if (mbfs_inodes[inode_idx].type != MBFS_INODE_TYPE_DIR) {
        return 0;
    }
    if (entries == 0 || max_count == 0 || mbfs_inodes[inode_idx].extent_count == 0) {
        return 0;
    }

    inode = &mbfs_inodes[inode_idx];
    total_bytes = max_count * sizeof(struct mbfs_dir_entry);
    written = 0;

    for (uint32_t e = 0; e < inode->extent_count && e < MBFS_INODE_EXTENTS && written < total_bytes; e++) {
        for (uint32_t s = 0; s < inode->extents[e].sector_count && written < total_bytes; s++) {
            uint32_t chunk = total_bytes - written;

            if (chunk > DISKFS_SECTOR_SIZE) {
                chunk = DISKFS_SECTOR_SIZE;
            }
            memory_set_u8(sector, 0, sizeof(sector));
            memory_copy(sector, (uint8_t*)entries + written, chunk);
            if (!ata_pio_write_sector(diskfs_device, inode->extents[e].start_lba + s, sector)) {
                return 0;
            }
            written += chunk;
        }
    }
    return 1;
}

static int diskfs_find_entry_in_parent(uint8_t parent_is_root,
                                       uint8_t parent_dir_inode,
                                       const char* name,
                                       int32_t* slot_out,
                                       uint8_t* inode_out)
{
    int32_t slot;

    if (!mbfs_component_valid(name)) {
        return -1;
    }

    if (parent_is_root) {
        slot = diskfs_find_index(name);
        if (slot < 0) {
            return 0;
        }
        if (slot_out != 0) {
            *slot_out = slot;
        }
        if (inode_out != 0) {
            *inode_out = mbfs_dir[(uint32_t)slot].inode_index;
        }
        return 1;
    }

    if (parent_dir_inode >= MBFS_MAX_INODES || !mbfs_inodes[parent_dir_inode].used ||
        mbfs_inodes[parent_dir_inode].type != MBFS_INODE_TYPE_DIR) {
        return -1;
    }
    if (!mbfs_load_subdir_entries(parent_dir_inode, mbfs_subdir_scratch, MBFS_MAX_DIRENTS)) {
        return -1;
    }

    slot = mbfs_find_in_entries(mbfs_subdir_scratch, MBFS_MAX_DIRENTS, name);
    if (slot < 0) {
        return 0;
    }

    if (slot_out != 0) {
        *slot_out = slot;
    }
    if (inode_out != 0) {
        *inode_out = mbfs_subdir_scratch[(uint32_t)slot].inode_index;
    }
    return 1;
}

static int diskfs_resolve_parent_directory(const char* path,
                                           uint8_t* parent_is_root_out,
                                           uint8_t* parent_dir_inode_out,
                                           char* leaf_out,
                                           uint32_t leaf_max)
{
    char component[MBFS_NAME_MAX];
    uint32_t offset = 0;
    uint8_t parent_is_root = 1u;
    uint8_t parent_dir_inode = 0u;

    if (path == 0 || leaf_out == 0 || leaf_max == 0u || !diskfs_mounted) {
        return -1;
    }

    while (1) {
        int rc = mbfs_path_next_component(path, &offset, component, sizeof(component));
        int find_rc;
        uint8_t inode_idx = 0u;

        if (rc <= 0) {
            return -1;
        }

        if (path[offset] == '\0') {
            str_copy_bounded(leaf_out, component, leaf_max);
            if (parent_is_root_out != 0) {
                *parent_is_root_out = parent_is_root;
            }
            if (parent_dir_inode_out != 0) {
                *parent_dir_inode_out = parent_dir_inode;
            }
            return 1;
        }

        find_rc = diskfs_find_entry_in_parent(parent_is_root, parent_dir_inode, component, 0, &inode_idx);
        if (find_rc <= 0) {
            return find_rc;
        }
        if (inode_idx >= MBFS_MAX_INODES || !mbfs_inodes[inode_idx].used ||
            mbfs_inodes[inode_idx].type != MBFS_INODE_TYPE_DIR) {
            return -1;
        }

        parent_is_root = 0u;
        parent_dir_inode = inode_idx;
    }
}

static int mbfs_directory_is_empty(uint8_t inode_idx)
{
    if (inode_idx >= MBFS_MAX_INODES || !mbfs_inodes[inode_idx].used ||
        mbfs_inodes[inode_idx].type != MBFS_INODE_TYPE_DIR) {
        return -1;
    }
    if (!mbfs_load_subdir_entries(inode_idx, mbfs_subdir_scratch, MBFS_MAX_DIRENTS)) {
        return -1;
    }

    for (uint32_t i = 0; i < MBFS_MAX_DIRENTS; i++) {
        if (mbfs_subdir_scratch[i].used) {
            return 0;
        }
    }

    return 1;
}

/* Resolve path to an inode. Supports arbitrary nested paths. */
static int diskfs_lookup_path(const char* path, struct mbfs_path_lookup* out, char* leaf_out, uint32_t leaf_max)
{
    char leaf_name[MBFS_NAME_MAX];
    uint8_t parent_is_root = 1u;
    uint8_t parent_dir_inode = 0u;
    int find_rc;
    int32_t slot = -1;
    uint8_t inode_idx = 0u;

    if (path == 0 || out == 0 || !diskfs_mounted) {
        return -1;
    }

    memory_set_u8(out, 0, sizeof(*out));
    out->entry_slot = -1;

    find_rc = diskfs_resolve_parent_directory(path, &parent_is_root, &parent_dir_inode,
                                              leaf_name, sizeof(leaf_name));
    if (find_rc <= 0) {
        return find_rc;
    }

    if (leaf_out != 0 && leaf_max > 0u) {
        str_copy_bounded(leaf_out, leaf_name, leaf_max);
    }

    find_rc = diskfs_find_entry_in_parent(parent_is_root, parent_dir_inode, leaf_name, &slot, &inode_idx);
    if (find_rc <= 0) {
        return find_rc;
    }
    if (inode_idx >= MBFS_MAX_INODES || !mbfs_inodes[inode_idx].used) {
        return -1;
    }

    out->found = 1u;
    out->parent_is_root = parent_is_root;
    out->parent_dir_inode = parent_dir_inode;
    out->inode_index = inode_idx;
    out->entry_slot = slot;
    return 1;
}

/* Create a single directory entry at an already-resolved parent path. */
static int diskfs_mkdir_single(const char* path)
{
    struct mbfs_extent ext;
    uint8_t sector[DISKFS_SECTOR_SIZE];
    char leaf_name[MBFS_NAME_MAX];
    uint8_t parent_is_root = 1u;
    uint8_t parent_dir_inode = 0u;
    int32_t dir_idx = -1;
    int32_t inode_idx;
    int resolve_rc;
    int find_rc;

    if (!diskfs_mounted) {
        return -1;
    }

    resolve_rc = diskfs_resolve_parent_directory(path, &parent_is_root, &parent_dir_inode,
                                                 leaf_name, sizeof(leaf_name));
    if (resolve_rc < 0) {
        return -2;
    }
    if (resolve_rc == 0) {
        return -8;
    }

    find_rc = diskfs_find_entry_in_parent(parent_is_root, parent_dir_inode, leaf_name, &dir_idx, 0);
    if (find_rc < 0) {
        return -2;
    }
    if (find_rc > 0) {
        return -7;
    }

    if (parent_is_root) {
        dir_idx = mbfs_alloc_dir_slot();
        if (dir_idx < 0) {
            return -3;
        }
    } else {
        if (!mbfs_load_subdir_entries(parent_dir_inode, mbfs_subdir_scratch, MBFS_MAX_DIRENTS)) {
            return -6;
        }
        for (uint32_t i = 0; i < MBFS_MAX_DIRENTS; i++) {
            if (!mbfs_subdir_scratch[i].used) {
                dir_idx = (int32_t)i;
                break;
            }
        }
        if (dir_idx < 0) {
            return -3;
        }
    }

    inode_idx = mbfs_alloc_inode_slot();
    if (inode_idx < 0) {
        return -3;
    }
    if (!mbfs_alloc_extent(MBFS_SUBDIR_SECTORS, &ext)) {
        return -4;
    }

    if (!mbfs_set_clean_flag(0u)) {
        return -6;
    }

    memory_set_u8(sector, 0, sizeof(sector));
    for (uint32_t s = 0; s < MBFS_SUBDIR_SECTORS; s++) {
        if (!ata_pio_write_sector(diskfs_device, ext.start_lba + s, sector)) {
            return -5;
        }
    }

    memory_set_u8(&mbfs_inodes[(uint32_t)inode_idx], 0, sizeof(mbfs_inodes[(uint32_t)inode_idx]));
    mbfs_inodes[(uint32_t)inode_idx].used = 1u;
    mbfs_inodes[(uint32_t)inode_idx].type = MBFS_INODE_TYPE_DIR;
    mbfs_inodes[(uint32_t)inode_idx].size_bytes =
        MBFS_MAX_DIRENTS * (uint32_t)sizeof(struct mbfs_dir_entry);
    mbfs_inodes[(uint32_t)inode_idx].extent_count = 1u;
    mbfs_inodes[(uint32_t)inode_idx].extents[0] = ext;

    if (parent_is_root) {
        memory_set_u8(&mbfs_dir[(uint32_t)dir_idx], 0, sizeof(mbfs_dir[(uint32_t)dir_idx]));
        mbfs_dir[(uint32_t)dir_idx].used = 1;
        mbfs_dir[(uint32_t)dir_idx].inode_index = (uint8_t)inode_idx;
        mbfs_dir[(uint32_t)dir_idx].type = MBFS_INODE_TYPE_DIR;
        str_copy_bounded(mbfs_dir[(uint32_t)dir_idx].name, leaf_name, MBFS_NAME_MAX);
        if (!mbfs_flush_inodes() || !mbfs_flush_directory() || !mbfs_flush_bitmap()) {
            return -6;
        }
    } else {
        memory_set_u8(&mbfs_subdir_scratch[(uint32_t)dir_idx], 0,
                      sizeof(mbfs_subdir_scratch[(uint32_t)dir_idx]));
        mbfs_subdir_scratch[(uint32_t)dir_idx].used = 1;
        mbfs_subdir_scratch[(uint32_t)dir_idx].inode_index = (uint8_t)inode_idx;
        mbfs_subdir_scratch[(uint32_t)dir_idx].type = MBFS_INODE_TYPE_DIR;
        str_copy_bounded(mbfs_subdir_scratch[(uint32_t)dir_idx].name, leaf_name, MBFS_NAME_MAX);
        if (!mbfs_flush_subdir_entries(parent_dir_inode, mbfs_subdir_scratch, MBFS_MAX_DIRENTS)
                || !mbfs_flush_inodes() || !mbfs_flush_bitmap()) {
            return -6;
        }
    }

    return mbfs_set_clean_flag(1u) ? 0 : -6;
}

/* Create a directory path recursively. Returns 0 on success. */
static int diskfs_mkdir(const char* path)
{
    char component[MBFS_NAME_MAX];
    char partial[MBFS_PATH_MAX];
    uint32_t offset = 0;
    uint32_t partial_len = 0;

    if (path == 0 || path[0] == '\0') {
        return -2;
    }

    partial[0] = '\0';
    while (1) {
        struct mbfs_path_lookup lookup;
        int rc = mbfs_path_next_component(path, &offset, component, sizeof(component));
        int lrc;

        if (rc <= 0) {
            return (rc == 0) ? 0 : -2;
        }

        if (partial_len != 0u) {
            if (partial_len + 1u >= sizeof(partial)) {
                return -2;
            }
            partial[partial_len++] = '/';
        }
        for (uint32_t i = 0; component[i] != '\0'; i++) {
            if (partial_len + 1u >= sizeof(partial)) {
                return -2;
            }
            partial[partial_len++] = component[i];
        }
        partial[partial_len] = '\0';

        lrc = diskfs_lookup_path(partial, &lookup, 0, 0u);
        if (lrc == 1) {
            if (lookup.inode_index >= MBFS_MAX_INODES || !mbfs_inodes[lookup.inode_index].used ||
                mbfs_inodes[lookup.inode_index].type != MBFS_INODE_TYPE_DIR) {
                return -7;
            }
        } else if (lrc == 0) {
            int mkrc = diskfs_mkdir_single(partial);
            if (mkrc != 0) {
                return mkrc;
            }
        } else {
            return -2;
        }
    }
}

static void diskfs_list_entries(const struct mbfs_dir_entry* entries, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        uint8_t inode_idx;

        if (!entries[i].used) {
            continue;
        }

        inode_idx = entries[i].inode_index;
        if (inode_idx >= MBFS_MAX_INODES || !mbfs_inodes[inode_idx].used) {
            continue;
        }

        terminal_write(entries[i].name);
        if (mbfs_inodes[inode_idx].type == MBFS_INODE_TYPE_DIR) {
            terminal_write("/ [dir]\n");
        } else {
            terminal_write(" (bytes=");
            terminal_write_dec_u32(mbfs_inodes[inode_idx].size_bytes);
            terminal_write(" extents=");
            terminal_write_dec_u32(mbfs_inodes[inode_idx].extent_count);
            terminal_write(")\n");
        }
    }
}

/* List path: "" or any directory path. */
static void diskfs_list_path(const char* path)
{
    struct mbfs_path_lookup lookup;
    int lookup_rc;

    if (!diskfs_mounted) {
        terminal_write("diskfs not mounted\n");
        return;
    }
    if (path == 0 || path[0] == '\0') {
        diskfs_list_entries(mbfs_dir, MBFS_MAX_DIRENTS);
        return;
    }

    lookup_rc = diskfs_lookup_path(path, &lookup, 0, 0u);
    if (lookup_rc <= 0 || !lookup.found) {
        terminal_write("diskfs: directory not found\n");
        return;
    }

    if (lookup.inode_index >= MBFS_MAX_INODES || !mbfs_inodes[lookup.inode_index].used ||
        mbfs_inodes[lookup.inode_index].type != MBFS_INODE_TYPE_DIR) {
        terminal_write("diskfs: not a directory\n");
        return;
    }
    if (!mbfs_load_subdir_entries(lookup.inode_index, mbfs_subdir_scratch, MBFS_MAX_DIRENTS)) {
        terminal_write("diskfs: failed to read directory\n");
        return;
    }

    diskfs_list_entries(mbfs_subdir_scratch, MBFS_MAX_DIRENTS);
}

/* Fill caller buffer with directory entries for `path`.
 * out_count will receive the number of entries written into `out`.
 * Returns 0 on success, negative on error.
 */
static int diskfs_get_entries(const char* path, struct mbfs_dir_entry* out, uint32_t max_count, uint32_t* out_count)
{
    if (out == 0 || max_count == 0) {
        return -1;
    }
    if (!diskfs_mounted) {
        return -2;
    }

    if (path == 0 || path[0] == '\0') {
        uint32_t copied = 0;
        for (uint32_t i = 0; i < MBFS_MAX_DIRENTS && copied < max_count; i++) {
            if (mbfs_dir[i].used) {
                out[copied++] = mbfs_dir[i];
            }
        }
        if (out_count) { *out_count = copied; }
        return 0;
    }

    struct mbfs_path_lookup lookup;
    if (diskfs_lookup_path(path, &lookup, 0, 0u) != 1 || !lookup.found) {
        return -3;
    }
    if (lookup.inode_index >= MBFS_MAX_INODES || !mbfs_inodes[lookup.inode_index].used ||
        mbfs_inodes[lookup.inode_index].type != MBFS_INODE_TYPE_DIR) {
        return -4;
    }

    if (!mbfs_load_subdir_entries(lookup.inode_index, mbfs_subdir_scratch, MBFS_MAX_DIRENTS)) {
        return -5;
    }

    uint32_t copied = 0;
    for (uint32_t i = 0; i < MBFS_MAX_DIRENTS && copied < max_count; i++) {
        if (mbfs_subdir_scratch[i].used) {
            out[copied++] = mbfs_subdir_scratch[i];
        }
    }
    if (out_count) { *out_count = copied; }
    return 0;
}

/* Cat file by inode index directly. */
static void diskfs_cat_inode(uint8_t inode_idx)
{
    uint8_t sector[DISKFS_SECTOR_SIZE + 1u];
    struct mbfs_inode* inode;
    uint32_t remaining;

    if (inode_idx >= MBFS_MAX_INODES) {
        terminal_write("diskfs: invalid inode\n");
        return;
    }
    inode = &mbfs_inodes[inode_idx];
    if (!inode->used || inode->type != MBFS_INODE_TYPE_FILE) {
        terminal_write("diskfs: not a file\n");
        return;
    }
    remaining = inode->size_bytes;
    for (uint32_t e = 0; e < inode->extent_count && e < MBFS_INODE_EXTENTS; e++) {
        for (uint32_t s = 0; s < inode->extents[e].sector_count; s++) {
            uint32_t chunk = remaining > DISKFS_SECTOR_SIZE ? DISKFS_SECTOR_SIZE : remaining;

            if (!ata_pio_read_sector(diskfs_device, inode->extents[e].start_lba + s, sector)) {
                terminal_write("diskfs: read error\n");
                return;
            }
            sector[chunk] = 0;
            terminal_write((const char*)sector);
            if (remaining <= chunk) {
                terminal_write("\n");
                return;
            }
            remaining -= chunk;
        }
    }
    terminal_write("\n");
}

/* Cat a file at a path (supports "file" and "dir/file"). */
static void diskfs_cat_path(const char* path)
{
    struct mbfs_path_lookup lookup;
    int rc;

    if (!diskfs_mounted) {
        terminal_write("diskfs not mounted\n");
        return;
    }

    rc = diskfs_lookup_path(path, &lookup, 0, 0u);
    if (rc < 0) {
        terminal_write("diskfs: invalid path\n");
        return;
    }
    if (rc == 0 || !lookup.found) {
        terminal_write("diskfs: file not found\n");
        return;
    }
    diskfs_cat_inode(lookup.inode_index);
}

/* Write data to file at path. */
static int diskfs_write_path(const char* path, const uint8_t* data, uint32_t size)
{
    char leaf_name[MBFS_NAME_MAX];
    uint8_t parent_is_root = 1u;
    uint8_t parent_dir_inode = 0u;
    int resolve_rc;

    if (!diskfs_mounted || data == 0) {
        return -1;
    }

    resolve_rc = diskfs_resolve_parent_directory(path, &parent_is_root, &parent_dir_inode,
                                                 leaf_name, sizeof(leaf_name));
    if (resolve_rc < 0) {
        return -2;
    }
    if (resolve_rc == 0) {
        return -8;
    }

    if (parent_is_root) {
        if (auto_run_disk_tests) {
            terminal_write("diskfs_write_path: root leaf="); terminal_write(leaf_name); terminal_write("\n");
        }
        return diskfs_write_bytes(leaf_name, data, size);
    }

    if (parent_dir_inode >= MBFS_MAX_INODES || !mbfs_inodes[parent_dir_inode].used ||
        mbfs_inodes[parent_dir_inode].type != MBFS_INODE_TYPE_DIR) {
        return -8;
    }
    if (!mbfs_load_subdir_entries(parent_dir_inode, mbfs_subdir_scratch, MBFS_MAX_DIRENTS)) {
        return -6;
    }

    /* Find or create file slot in subdir */
    int32_t file_slot = mbfs_find_in_entries(mbfs_subdir_scratch, MBFS_MAX_DIRENTS, leaf_name);
    int32_t inode_idx;

    if (file_slot < 0) {
        /* Allocate new dir slot and inode */
        for (uint32_t i = 0; i < MBFS_MAX_DIRENTS; i++) {
            if (!mbfs_subdir_scratch[i].used) {
                file_slot = (int32_t)i;
                break;
            }
        }
        if (file_slot < 0) {
            return -3;
        }
        inode_idx = mbfs_alloc_inode_slot();
        if (inode_idx < 0) {
            return -3;
        }
        memory_set_u8(&mbfs_subdir_scratch[(uint32_t)file_slot], 0,
                      sizeof(mbfs_subdir_scratch[(uint32_t)file_slot]));
        mbfs_subdir_scratch[(uint32_t)file_slot].used = 1;
        mbfs_subdir_scratch[(uint32_t)file_slot].inode_index = (uint8_t)inode_idx;
        mbfs_subdir_scratch[(uint32_t)file_slot].type = MBFS_INODE_TYPE_FILE;
        str_copy_bounded(mbfs_subdir_scratch[(uint32_t)file_slot].name, leaf_name, MBFS_NAME_MAX);
        memory_set_u8(&mbfs_inodes[(uint32_t)inode_idx], 0, sizeof(mbfs_inodes[(uint32_t)inode_idx]));
        mbfs_inodes[(uint32_t)inode_idx].used = 1u;
        mbfs_inodes[(uint32_t)inode_idx].type = MBFS_INODE_TYPE_FILE;
    } else {
        inode_idx = mbfs_subdir_scratch[(uint32_t)file_slot].inode_index;
        if (inode_idx < 0 || (uint32_t)inode_idx >= MBFS_MAX_INODES) {
            return -6;
        }
    }

    struct mbfs_inode* inode = &mbfs_inodes[(uint32_t)inode_idx];
    uint32_t sectors = diskfs_bytes_to_sectors(size);
    struct mbfs_extent ext;
    uint8_t sector[DISKFS_SECTOR_SIZE];

    if (!mbfs_set_clean_flag(0u)) {
        return -6;
    }

    /* Free old extents */
    for (uint32_t i = 0; i < inode->extent_count && i < MBFS_INODE_EXTENTS; i++) {
        mbfs_mark_extent(&inode->extents[i], 0u);
    }
    memory_set_u8(inode->extents, 0, sizeof(inode->extents));
    inode->extent_count = 0u;

    if (sectors > 0u) {
        if (!mbfs_alloc_extent(sectors, &ext)) {
            return -4;
        }
        inode->extents[0] = ext;
        inode->extent_count = 1u;

        for (uint32_t s = 0; s < sectors; s++) {
            uint32_t base = s * DISKFS_SECTOR_SIZE;
            uint32_t chunk = size - base;

            if (chunk > DISKFS_SECTOR_SIZE) {
                chunk = DISKFS_SECTOR_SIZE;
            }
            memory_set_u8(sector, 0, sizeof(sector));
            memory_copy(sector, data + base, chunk);
            if (!ata_pio_write_sector(diskfs_device, ext.start_lba + s, sector)) {
                return -5;
            }
        }
    }

    inode->used = 1u;
    inode->type = MBFS_INODE_TYPE_FILE;
    inode->size_bytes = size;

    if (!mbfs_flush_subdir_entries(parent_dir_inode, mbfs_subdir_scratch, MBFS_MAX_DIRENTS)) {
        return -6;
    }
    if (!mbfs_flush_inodes() || !mbfs_flush_bitmap()) {
        return -6;
    }
    return mbfs_set_clean_flag(1u) ? 0 : -6;
}

/* Remove file or empty directory at path. */
static int diskfs_remove_path(const char* path)
{
    struct mbfs_path_lookup lookup;
    int lrc;
    uint8_t inode_idx;

    if (!diskfs_mounted) {
        return 0;
    }

    lrc = diskfs_lookup_path(path, &lookup, 0, 0u);
    if (lrc < 0) {
        return 0;
    }
    if (!lookup.found || lookup.entry_slot < 0) {
        return 0;
    }

    inode_idx = lookup.inode_index;
    if (inode_idx < MBFS_MAX_INODES && mbfs_inodes[inode_idx].used &&
        mbfs_inodes[inode_idx].type == MBFS_INODE_TYPE_DIR) {
        int empty_rc = mbfs_directory_is_empty(inode_idx);
        if (empty_rc < 0) {
            return 0;
        }
        if (empty_rc == 0) {
            return -2;
        }
    }

    if (!mbfs_set_clean_flag(0u)) {
        return 0;
    }

    if (lookup.parent_is_root) {
        if (inode_idx < MBFS_MAX_INODES && mbfs_inodes[inode_idx].used) {
            for (uint32_t i = 0; i < mbfs_inodes[inode_idx].extent_count && i < MBFS_INODE_EXTENTS; i++) {
                mbfs_mark_extent(&mbfs_inodes[inode_idx].extents[i], 0u);
            }
            memory_set_u8(&mbfs_inodes[inode_idx], 0, sizeof(mbfs_inodes[inode_idx]));
        }
        memory_set_u8(&mbfs_dir[(uint32_t)lookup.entry_slot], 0, sizeof(mbfs_dir[(uint32_t)lookup.entry_slot]));
        if (!mbfs_flush_inodes() || !mbfs_flush_directory() || !mbfs_flush_bitmap()) {
            return 0;
        }
    } else {
        uint8_t dir_inode_idx = lookup.parent_dir_inode;
        if (dir_inode_idx >= MBFS_MAX_INODES || !mbfs_inodes[dir_inode_idx].used ||
            mbfs_inodes[dir_inode_idx].type != MBFS_INODE_TYPE_DIR) {
            return 0;
        }
        if (!mbfs_load_subdir_entries(dir_inode_idx, mbfs_subdir_scratch, MBFS_MAX_DIRENTS)) {
            return 0;
        }
        if (inode_idx < MBFS_MAX_INODES && mbfs_inodes[inode_idx].used) {
            for (uint32_t i = 0; i < mbfs_inodes[inode_idx].extent_count && i < MBFS_INODE_EXTENTS; i++) {
                mbfs_mark_extent(&mbfs_inodes[inode_idx].extents[i], 0u);
            }
            memory_set_u8(&mbfs_inodes[inode_idx], 0, sizeof(mbfs_inodes[inode_idx]));
        }
        memory_set_u8(&mbfs_subdir_scratch[(uint32_t)lookup.entry_slot], 0,
                      sizeof(mbfs_subdir_scratch[(uint32_t)lookup.entry_slot]));
        if (!mbfs_flush_subdir_entries(dir_inode_idx, mbfs_subdir_scratch, MBFS_MAX_DIRENTS)) {
            return 0;
        }
        if (!mbfs_flush_inodes() || !mbfs_flush_bitmap()) {
            return 0;
        }
    }

    return mbfs_set_clean_flag(1u);
}

static void diskfs_print_status(void)
{
    terminal_write("ATA devices:\n");
    for (uint32_t i = 0; i < 4u; i++) {
        terminal_write("  slot ");
        terminal_write_dec_u32(i);
        terminal_write(": ");
        if (ata_devices[i].present) {
            terminal_write("ATA sectors=");
            terminal_write_dec_u32(ata_devices[i].total_sectors);
        } else if (ata_devices[i].atapi) {
            terminal_write("ATAPI/unsupported");
        } else {
            terminal_write("empty");
        }
        terminal_write("\n");
    }

    terminal_write("diskfs device: ");
    if (diskfs_device == 0) {
        terminal_write("none\n");
    } else {
        terminal_write("present\n");
    }
    terminal_write("diskfs mounted: ");
    terminal_write(diskfs_mounted ? "yes\n" : "no\n");
    terminal_write("mbfs previous mount state: ");
    terminal_write(mbfs_unclean_last_mount ? "unclean\n" : "clean\n");
    if (diskfs_mounted) {
        terminal_write("mbfs data sectors: ");
        terminal_write_dec_u32(mbfs_superblock.data_sector_count);
        terminal_write("\nmbfs inodes: ");
        terminal_write_dec_u32(mbfs_superblock.max_inodes);
        terminal_write("\nmbfs dir entries: ");
        terminal_write_dec_u32(mbfs_superblock.max_dir_entries);
        terminal_write("\nmbfs clean flag: ");
        terminal_write(mbfs_superblock.clean_shutdown ? "clean\n" : "dirty\n");
    }
}

/* Run a lightweight integrity check on MBFS metadata.
 * Returns: -1=not mounted, 0=ok, >0 = problems found
 */
static int diskfs_check(void)
{
    if (!diskfs_mounted) {
        return -1;
    }

    uint32_t bitmap_bytes = MBFS_BITMAP_SECTORS * DISKFS_SECTOR_SIZE;
    memory_set_u8(mbfs_expected_bitmap, 0, bitmap_bytes);

    uint32_t problems = 0;

    /* Validate inodes and build expected bitmap */
    for (uint32_t i = 0; i < MBFS_MAX_INODES; i++) {
        struct mbfs_inode* inode = &mbfs_inodes[i];
        if (!inode->used) { continue; }

        if (inode->extent_count > MBFS_INODE_EXTENTS) {
            terminal_write("diskcheck: inode "); terminal_write_dec_u32(i);
            terminal_write(" extent_count invalid\n");
            problems++;
            continue;
        }

        for (uint32_t e = 0; e < inode->extent_count && e < MBFS_INODE_EXTENTS; e++) {
            uint32_t start = inode->extents[e].start_lba;
            uint32_t cnt = inode->extents[e].sector_count;
            if (cnt == 0u) { continue; }
            if (start < mbfs_superblock.data_lba || start + cnt > mbfs_superblock.data_lba + mbfs_superblock.data_sector_count) {
                terminal_write("diskcheck: inode "); terminal_write_dec_u32(i);
                terminal_write(" extent out-of-range\n");
                problems++;
                continue;
            }

            uint32_t start_index = start - mbfs_superblock.data_lba;
            for (uint32_t s = 0; s < cnt; s++) {
                uint32_t idx = start_index + s;
                uint32_t byte = idx >> 3;
                uint8_t bit = (uint8_t)(1u << (idx & 7u));
                if (byte < bitmap_bytes) {
                    mbfs_expected_bitmap[byte] |= bit;
                }
            }
        }
    }

    /* Compare expected bitmap with on-disk bitmap */
    for (uint32_t b = 0; b < bitmap_bytes; b++) {
        if (mbfs_expected_bitmap[b] != mbfs_bitmap[b]) {
            problems++;
        }
    }

    /* Validate root directory entries */
    for (uint32_t i = 0; i < MBFS_MAX_DIRENTS; i++) {
        if (!mbfs_dir[i].used) { continue; }
        uint8_t ino = mbfs_dir[i].inode_index;
        if (ino >= MBFS_MAX_INODES || !mbfs_inodes[ino].used) {
            terminal_write("diskcheck: root dir entry "); terminal_write_dec_u32(i);
            terminal_write(" references invalid inode\n");
            problems++;
        }
    }

    /* Validate nested subdirectories */
    for (uint32_t i = 0; i < MBFS_MAX_INODES; i++) {
        if (!mbfs_inodes[i].used || mbfs_inodes[i].type != MBFS_INODE_TYPE_DIR) { continue; }
        if (!mbfs_load_subdir_entries(i, mbfs_subdir_scratch, MBFS_MAX_DIRENTS)) {
            terminal_write("diskcheck: failed to read subdir inode "); terminal_write_dec_u32(i);
            terminal_write("\n");
            problems++;
            continue;
        }
        for (uint32_t j = 0; j < MBFS_MAX_DIRENTS; j++) {
            if (!mbfs_subdir_scratch[j].used) { continue; }
            uint8_t ino = mbfs_subdir_scratch[j].inode_index;
            if (ino >= MBFS_MAX_INODES || !mbfs_inodes[ino].used) {
                terminal_write("diskcheck: subdir inode "); terminal_write_dec_u32(i);
                terminal_write(" entry "); terminal_write_dec_u32(j);
                terminal_write(" references invalid inode\n");
                problems++;
            }
        }
    }

    if (problems == 0) {
        terminal_write("diskcheck: OK\n");
        return 0;
    }

    terminal_write("diskcheck: problems found: "); terminal_write_dec_u32(problems); terminal_write("\n");
    return (int)problems;
}

/* Attempt to repair MBFS metadata by rebuilding the bitmap and clearing
 * references to invalid inodes/extents. This is a best-effort repair and
 * will write corrected metadata back to disk. Returns 0 on success. */
static int diskfs_repair(void)
{
    if (!diskfs_mounted) { return -1; }

    uint32_t bitmap_bytes = MBFS_BITMAP_SECTORS * DISKFS_SECTOR_SIZE;

    /* Start with a clean expected bitmap */
    memory_set_u8(mbfs_expected_bitmap, 0, bitmap_bytes);

    /* Validate inodes; clear any with invalid extents, otherwise mark extents */
    uint8_t modified_inodes = 0;
    for (uint32_t i = 0; i < MBFS_MAX_INODES; i++) {
        struct mbfs_inode* inode = &mbfs_inodes[i];
        if (!inode->used) { continue; }

        int invalid = 0;
        for (uint32_t e = 0; e < inode->extent_count && e < MBFS_INODE_EXTENTS; e++) {
            uint32_t start = inode->extents[e].start_lba;
            uint32_t cnt = inode->extents[e].sector_count;
            if (cnt == 0u) { continue; }
            if (start < mbfs_superblock.data_lba || start + cnt > mbfs_superblock.data_lba + mbfs_superblock.data_sector_count) {
                invalid = 1;
                break;
            }
        }
        if (invalid) {
            memory_set_u8(inode, 0, sizeof(*inode));
            modified_inodes = 1;
            continue;
        }

        for (uint32_t e = 0; e < inode->extent_count && e < MBFS_INODE_EXTENTS; e++) {
            uint32_t start_index = inode->extents[e].start_lba - mbfs_superblock.data_lba;
            for (uint32_t s = 0; s < inode->extents[e].sector_count; s++) {
                uint32_t idx = start_index + s;
                uint32_t byte = idx >> 3;
                uint8_t bit = (uint8_t)(1u << (idx & 7u));
                if (byte < bitmap_bytes) {
                    mbfs_expected_bitmap[byte] |= bit;
                }
            }
        }
    }

    /* Validate root dir entries and clear any that reference missing inodes */
    uint8_t modified_dir = 0;
    for (uint32_t i = 0; i < MBFS_MAX_DIRENTS; i++) {
        if (!mbfs_dir[i].used) { continue; }
        uint8_t ino = mbfs_dir[i].inode_index;
        if (ino >= MBFS_MAX_INODES || !mbfs_inodes[ino].used) {
            memory_set_u8(&mbfs_dir[i], 0, sizeof(mbfs_dir[i]));
            modified_dir = 1;
        }
    }

    /* Validate subdir entries and clear invalid references */
    for (uint32_t i = 0; i < MBFS_MAX_INODES; i++) {
        if (!mbfs_inodes[i].used || mbfs_inodes[i].type != MBFS_INODE_TYPE_DIR) { continue; }
        if (!mbfs_load_subdir_entries(i, mbfs_subdir_scratch, MBFS_MAX_DIRENTS)) { continue; }
        uint8_t changed = 0;
        for (uint32_t j = 0; j < MBFS_MAX_DIRENTS; j++) {
            if (!mbfs_subdir_scratch[j].used) { continue; }
            uint8_t ino = mbfs_subdir_scratch[j].inode_index;
            if (ino >= MBFS_MAX_INODES || !mbfs_inodes[ino].used) {
                memory_set_u8(&mbfs_subdir_scratch[j], 0, sizeof(mbfs_subdir_scratch[j]));
                changed = 1;
            }
        }
        if (changed) {
            (void)mbfs_flush_subdir_entries(i, mbfs_subdir_scratch, MBFS_MAX_DIRENTS);
        }
    }

    /* Commit rebuilt bitmap and any modified metadata */
    if (!mbfs_flush_bitmap()) { return -2; }
    if (modified_inodes) { if (!mbfs_flush_inodes()) { return -3; } }
    if (modified_dir) { if (!mbfs_flush_directory()) { return -4; } }
    if (!mbfs_set_clean_flag(1u)) { return -5; }
    return 0;
}

/* Simple smoke-test that writes a small file to disk and reads it back. */
static void diskfs_smoke_test(void)
{
    const char* path = "MBFSTEST.TXT";
    static const uint8_t payload[] = "MBFS smoke test\n";

    if (!diskfs_mounted) {
        terminal_write("diskfstest: not mounted\n");
        return;
    }

    /* Diagnostic: print payload address and size before write (use serial helpers for numeric output) */
    terminal_write("diskfstest: payload_addr="); serial_write_dec_u32((uint32_t)payload);
    terminal_write(" size="); serial_write_dec_u32((uint32_t)(sizeof(payload) - 1u)); terminal_write("\n");

    int wrc = diskfs_write_path(path, payload, (uint32_t)(sizeof(payload) - 1u));
    terminal_write("diskfstest: write rc="); terminal_write_dec_u32((uint32_t)wrc); terminal_write("\n");
    if (wrc != 0) {
        terminal_write("diskfstest: write failed (rc="); terminal_write_dec_u32((uint32_t)wrc); terminal_write(")\n");
        return;
    }

    struct mbfs_path_lookup lookup;
    if (diskfs_lookup_path(path, &lookup, 0, 0u) != 1 || !lookup.found) {
        terminal_write("diskfstest: lookup failed after write\n");
        return;
    }
    terminal_write("diskfstest: inode="); terminal_write_dec_u32((uint32_t)lookup.inode_index); terminal_write("\n");

    uint8_t buf[64];
    int n = mbfs_read_by_inode_index((uint8_t)lookup.inode_index, 0u, buf, (uint32_t)sizeof(buf));
    if (n <= 0) {
        terminal_write("diskfstest: read failed\n");
        return;
    }

    /* Null-terminate for printing */
    uint32_t rn = (uint32_t)n;
    if (rn >= sizeof(buf)) { rn = sizeof(buf) - 1u; }
    buf[rn] = '\0';
    terminal_write("diskfstest: read back: "); terminal_write((const char*)buf); terminal_write("\n");
}

static int user_range_valid(uint32_t user_va, uint32_t size, uint8_t require_write)
{
    uint32_t end;
    uint32_t page;

    if (size == 0) {
        return 1;
    }

    if (user_va < USER_SPACE_BASE || user_va >= USER_SPACE_LIMIT) {
        return 0;
    }

    end = user_va + size - 1u;
    if (end < user_va || end >= USER_SPACE_LIMIT) {
        return 0;
    }

    page = align_down_u32(user_va, PAGE_SIZE);
    while (page <= end) {
        uint32_t entry = 0;
        struct process_syscall_context* cur = current_process_context();
        if (cur != 0 && cur->page_dir_phys != 0) {
            if (!paging_lookup_direct(cur->page_dir_phys, page, 0, &entry)) {
                return 0;
            }
        } else {
            if (!paging_lookup(page, 0, &entry)) {
                return 0;
            }
        }
        if (require_write && (entry & 0x2u) == 0u) {
            return 0;
        }
        if (page > 0xFFFFF000u - PAGE_SIZE) {
            break;
        }
        page += PAGE_SIZE;
    }

    return 1;
}

static int user_copy_in(char* dst, uint32_t user_va, uint32_t size)
{
    const uint8_t* src = (const uint8_t*)(uintptr_t)user_va;

    if (!user_range_valid(user_va, size, 0u)) {
        return 0;
    }

    for (uint32_t i = 0; i < size; i++) {
        dst[i] = (char)src[i];
    }
    return 1;
}

static int user_copy_out(uint32_t user_va, const uint8_t* src, uint32_t size)
{
    uint8_t* dst = (uint8_t*)(uintptr_t)user_va;

    if (!user_range_valid(user_va, size, 1u)) {
        return 0;
    }

    for (uint32_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
    return 1;
}

static int user_copy_in_cstr(char* dst, uint32_t max_len, uint32_t user_va)
{
    uint8_t* src = (uint8_t*)(uintptr_t)user_va;

    if (max_len == 0 || user_va < USER_SPACE_BASE || user_va >= USER_SPACE_LIMIT) {
        return 0;
    }

    for (uint32_t i = 0; i < max_len; i++) {
        uint32_t addr = user_va + i;
        if (addr < user_va || addr >= USER_SPACE_LIMIT || !user_range_valid(addr, 1u, 0u)) {
            return 0;
        }
        dst[i] = (char)src[i];
        if (dst[i] == '\0') {
            return 1;
        }
    }

    dst[max_len - 1u] = '\0';
    return 0;
}

static void process_close_all_fds(struct process_syscall_context* proc)
{
    if (proc == 0) {
        return;
    }

    for (uint32_t i = 0; i < PROCESS_FD_MAX; i++) {
        proc->fds[i].used = 0;
        proc->fds[i].flags = 0;
        proc->fds[i].backend = FD_BACKEND_RAMFS;
        proc->fds[i].reserved0 = 0;
        proc->fds[i].file_index = -1;
        proc->fds[i].offset = 0;
    }
}

static int process_resolve_disk_path(const struct process_syscall_context* proc,
                                     const char* input,
                                     char* out,
                                     uint32_t out_max)
{
    const char* cwd = "";

    if (proc != 0) {
        cwd = proc->cwd;
    }

    return mbfs_normalize_path(cwd, input, out, out_max);
}

static int process_set_cwd(struct process_syscall_context* proc, const char* path)
{
    char resolved[MBFS_PATH_MAX];

    if (proc == 0 || !diskfs_mounted || path == 0) {
        return 0;
    }
    if (!process_resolve_disk_path(proc, path, resolved, sizeof(resolved))) {
        return 0;
    }
    if (resolved[0] != '\0') {
        struct mbfs_path_lookup lookup;
        if (diskfs_lookup_path(resolved, &lookup, 0, 0u) != 1 ||
            lookup.inode_index >= MBFS_MAX_INODES ||
            !mbfs_inodes[lookup.inode_index].used ||
            mbfs_inodes[lookup.inode_index].type != MBFS_INODE_TYPE_DIR) {
            return 0;
        }
    }

    str_copy_bounded(proc->cwd, resolved, sizeof(proc->cwd));
    return 1;
}

static int32_t process_fd_open(struct process_syscall_context* proc, const char* name, uint8_t flags)
{
    int32_t idx = -1;
    uint8_t backend = FD_BACKEND_RAMFS;
    uint8_t allow_ramfs_fallback = 0u;
    char resolved_name[MBFS_PATH_MAX];

    if (proc == 0 || (flags & (RAMFS_OPEN_READ | RAMFS_OPEN_WRITE)) == 0u) {
        return -1;
    }

    if (name[0] != '/' && !str_contains_char(name, '/') && proc->cwd[0] == '\0') {
        allow_ramfs_fallback = 1u;
    }

    if (diskfs_mounted && process_resolve_disk_path(proc, name, resolved_name, sizeof(resolved_name))
            && resolved_name[0] != '\0') {
        struct mbfs_path_lookup lookup;
        int lrc = diskfs_lookup_path(resolved_name, &lookup, 0, 0u);

        if (lrc == 1 && lookup.inode_index < MBFS_MAX_INODES &&
            mbfs_inodes[lookup.inode_index].used &&
            mbfs_inodes[lookup.inode_index].type == MBFS_INODE_TYPE_FILE) {
            idx = (int32_t)lookup.inode_index;
        } else if (lrc == 0 && (flags & RAMFS_OPEN_WRITE) != 0u) {
            static const uint8_t empty_data[1] = {0};
            if (diskfs_write_path(resolved_name, empty_data, 0u) == 0) {
                if (diskfs_lookup_path(resolved_name, &lookup, 0, 0u) == 1) {
                    idx = (int32_t)lookup.inode_index;
                }
            }
        }

        if (idx >= 0) {
            backend = FD_BACKEND_MBFS;
        }
    }

    if (idx < 0 && !allow_ramfs_fallback) {
        return -2;
    }

    if (idx < 0) {
        idx = ramfs_find_index(name);
        if (idx < 0) {
            if ((flags & RAMFS_OPEN_WRITE) == 0u) {
                return -2;
            }
            if (ramfs_write_text(name, "") != 0) {
                return -3;
            }
            idx = ramfs_find_index(name);
            if (idx < 0) {
                return -3;
            }
        }
        backend = FD_BACKEND_RAMFS;
    }

    for (uint32_t fd = 0; fd < PROCESS_FD_MAX; fd++) {
        if (!proc->fds[fd].used) {
            proc->fds[fd].used = 1;
            proc->fds[fd].flags = flags;
            proc->fds[fd].backend = backend;
            proc->fds[fd].reserved0 = 0;
            proc->fds[fd].file_index = idx;
            proc->fds[fd].offset = 0;
            return (int32_t)fd;
        }
    }

    return -4;
}

static int process_fd_close(struct process_syscall_context* proc, uint32_t fd)
{
    if (proc == 0 || fd >= PROCESS_FD_MAX || !proc->fds[fd].used) {
        return 0;
    }

    proc->fds[fd].used = 0;
    proc->fds[fd].flags = 0;
    proc->fds[fd].backend = FD_BACKEND_RAMFS;
    proc->fds[fd].reserved0 = 0;
    proc->fds[fd].file_index = -1;
    proc->fds[fd].offset = 0;
    return 1;
}

static int32_t process_fd_read(struct process_syscall_context* proc, uint32_t fd, uint8_t* out, uint32_t count)
{
    struct process_fd* desc;

    if (proc == 0 || fd >= PROCESS_FD_MAX || out == 0 || count == 0) {
        return -1;
    }

    desc = &proc->fds[fd];
    if (!desc->used || (desc->flags & RAMFS_OPEN_READ) == 0u || desc->file_index < 0) {
        return -1;
    }

    if (desc->backend == FD_BACKEND_MBFS) {
        int32_t n;
        if ((uint32_t)desc->file_index >= MBFS_MAX_INODES) {
            return -1;
        }
        n = mbfs_read_by_inode_index((uint8_t)desc->file_index, desc->offset, out, count);
        if (n > 0) {
            desc->offset += (uint32_t)n;
        }
        return n;
    }

    {
        struct ramfs_file* file = &ramfs_files[(uint32_t)desc->file_index];
        uint32_t remaining;
        uint32_t n;

        if (!file->used || desc->offset >= file->size) {
            return 0;
        }

        remaining = file->size - desc->offset;
        n = count < remaining ? count : remaining;
        for (uint32_t i = 0; i < n; i++) {
            out[i] = file->data[desc->offset + i];
        }
        desc->offset += n;
        return (int32_t)n;
    }
}

static int32_t process_fd_write(struct process_syscall_context* proc, uint32_t fd, const uint8_t* in, uint32_t count)
{
    struct process_fd* desc;

    if (proc == 0 || fd >= PROCESS_FD_MAX || in == 0 || count == 0) {
        return -1;
    }

    desc = &proc->fds[fd];
    if (!desc->used || (desc->flags & RAMFS_OPEN_WRITE) == 0u || desc->file_index < 0) {
        return -1;
    }

    if (desc->backend == FD_BACKEND_MBFS) {
        uint8_t inode_idx;
        struct mbfs_inode* inode;
        uint32_t old_size;
        uint32_t new_size;
        uint32_t required_sectors;
        uint32_t allocated_sectors;

        if ((uint32_t)desc->file_index >= MBFS_MAX_INODES) {
            return -1;
        }
        inode_idx = (uint8_t)desc->file_index;
        inode = &mbfs_inodes[inode_idx];
        if (!inode->used || inode->type != MBFS_INODE_TYPE_FILE) {
            return -1;
        }

        old_size = inode->size_bytes;
        new_size = desc->offset + count;
        if (new_size < desc->offset) {
            return -1;
        }

        required_sectors = diskfs_bytes_to_sectors(new_size);
        allocated_sectors = (inode->extent_count > 0u) ? inode->extents[0].sector_count : 0u;

        if (!mbfs_set_clean_flag(0u)) {
            return -1;
        }

        if (required_sectors > allocated_sectors) {
            struct mbfs_extent new_ext;
            uint8_t sector[DISKFS_SECTOR_SIZE];

            if (!mbfs_alloc_extent(required_sectors, &new_ext)) {
                return -1;
            }

            for (uint32_t s = 0; s < allocated_sectors; s++) {
                if (!ata_pio_read_sector(diskfs_device, inode->extents[0].start_lba + s, sector)) {
                    return -1;
                }
                if (!ata_pio_write_sector(diskfs_device, new_ext.start_lba + s, sector)) {
                    return -1;
                }
            }

            if (allocated_sectors > 0u) {
                mbfs_mark_extent(&inode->extents[0], 0u);
            }
            inode->extents[0] = new_ext;
            inode->extent_count = (required_sectors > 0u) ? 1u : 0u;
            allocated_sectors = required_sectors;
        }

        {
            uint32_t write_cursor = 0u;
            while (write_cursor < count) {
                uint32_t absolute = desc->offset + write_cursor;
                uint32_t sector_idx = absolute / DISKFS_SECTOR_SIZE;
                uint32_t sector_off = absolute % DISKFS_SECTOR_SIZE;
                uint32_t chunk = count - write_cursor;
                uint8_t sector[DISKFS_SECTOR_SIZE];

                if (sector_idx >= allocated_sectors) {
                    return -1;
                }
                if (chunk > DISKFS_SECTOR_SIZE - sector_off) {
                    chunk = DISKFS_SECTOR_SIZE - sector_off;
                }

                if (sector_off != 0u || chunk != DISKFS_SECTOR_SIZE) {
                    if (!ata_pio_read_sector(diskfs_device, inode->extents[0].start_lba + sector_idx, sector)) {
                        return -1;
                    }
                } else {
                    memory_set_u8(sector, 0, sizeof(sector));
                }

                memory_copy(sector + sector_off, in + write_cursor, chunk);
                if (!ata_pio_write_sector(diskfs_device, inode->extents[0].start_lba + sector_idx, sector)) {
                    return -1;
                }

                write_cursor += chunk;
            }
        }

        if (new_size > old_size) {
            inode->size_bytes = new_size;
        }

        if (!mbfs_flush_inodes() || !mbfs_flush_bitmap()) {
            return -1;
        }
        if (!mbfs_set_clean_flag(1u)) {
            return -1;
        }

        desc->offset += count;
        return (int32_t)count;
    }

    {
        struct ramfs_file* file = &ramfs_files[(uint32_t)desc->file_index];

        if (!file->used || desc->offset > RAMFS_FILE_MAX) {
            return -1;
        }

        if (count > RAMFS_FILE_MAX - desc->offset) {
            count = RAMFS_FILE_MAX - desc->offset;
        }

        for (uint32_t i = 0; i < count; i++) {
            file->data[desc->offset + i] = in[i];
        }
        desc->offset += count;
        if (desc->offset > file->size) {
            file->size = desc->offset;
            file->data[file->size] = 0;
        }

        return (int32_t)count;
    }
}

static int32_t process_fd_seek(struct process_syscall_context* proc, uint32_t fd, uint32_t offset)
{
    struct process_fd* desc;

    if (proc == 0 || fd >= PROCESS_FD_MAX) {
        return -1;
    }

    desc = &proc->fds[fd];
    if (!desc->used || desc->file_index < 0) {
        return -1;
    }

    if (desc->backend == FD_BACKEND_MBFS) {
        uint32_t size;
        if ((uint32_t)desc->file_index >= MBFS_MAX_INODES) {
            return -1;
        }
        size = mbfs_file_size_by_inode_index((uint8_t)desc->file_index);
        if (offset > size) {
            return -1;
        }
        desc->offset = offset;
        return (int32_t)offset;
    }

    {
        struct ramfs_file* file = &ramfs_files[(uint32_t)desc->file_index];
        if (!file->used || offset > file->size) {
            return -1;
        }
        desc->offset = offset;
        return (int32_t)offset;
    }
}

/*
 * paging_map_page_direct / paging_lookup_direct / paging_unmap_page_direct
 *
 * Operate on an arbitrary page directory by physical address rather than
 * through the alias window.  All physical addresses involved must lie within
 * the identity-mapped range (0 … PAGING_IDENTITY_CAP_BYTES) so that they can
 * be dereferenced directly.  Used by the per-process page directory code.
 */
static int paging_map_page_direct(uint32_t dir_phys,
                                  uint32_t virtual_address,
                                  uint32_t physical_address,
                                  uint32_t flags)
{
    uint32_t* dir = (uint32_t*)(uintptr_t)dir_phys;
    uint32_t dir_idx = virtual_address >> 22;
    uint32_t tbl_idx = (virtual_address >> 12) & 0x3FFu;
    uint32_t* table;

    if ((dir[dir_idx] & 0x1u) == 0) {
        uint32_t tbl_phys = physical_allocator_alloc_page();
        if (tbl_phys == 0) {
            return 0;
        }
        physical_allocator_set_owner(tbl_phys, PAGE_OWNER_PAGING_STRUCTURE);
        table = (uint32_t*)(uintptr_t)tbl_phys;
        memory_set_u8(table, 0, PAGE_SIZE);
        dir[dir_idx] = tbl_phys | 0x7u;  /* present | rw | user */
    } else {
        table = (uint32_t*)(uintptr_t)(dir[dir_idx] & 0xFFFFF000u);
    }

    table[tbl_idx] = (physical_address & 0xFFFFF000u) | (flags & 0xFFFu) | 0x1u;
    return 1;
}

static int paging_lookup_direct(uint32_t dir_phys,
                                uint32_t virtual_address,
                                uint32_t* physical_out,
                                uint32_t* entry_out)
{
    const uint32_t* dir = (const uint32_t*)(uintptr_t)dir_phys;
    uint32_t dir_idx = virtual_address >> 22;
    uint32_t tbl_idx = (virtual_address >> 12) & 0x3FFu;
    const uint32_t* table;
    uint32_t entry;

    if ((dir[dir_idx] & 0x1u) == 0) {
        return 0;
    }
    table = (const uint32_t*)(uintptr_t)(dir[dir_idx] & 0xFFFFF000u);
    entry = table[tbl_idx];
    if ((entry & 0x1u) == 0) {
        return 0;
    }
    if (physical_out != 0) {
        *physical_out = (entry & 0xFFFFF000u) | (virtual_address & 0xFFFu);
    }
    if (entry_out != 0) {
        *entry_out = entry;
    }
    return 1;
}

static void paging_unmap_page_direct(uint32_t dir_phys, uint32_t virtual_address)
{
    uint32_t* dir = (uint32_t*)(uintptr_t)dir_phys;
    uint32_t dir_idx = virtual_address >> 22;
    uint32_t tbl_idx = (virtual_address >> 12) & 0x3FFu;
    uint32_t tbl_phys;
    uint32_t* table;
    uint8_t empty;

    if ((dir[dir_idx] & 0x1u) == 0) {
        return;
    }
    tbl_phys = dir[dir_idx] & 0xFFFFF000u;
    table    = (uint32_t*)(uintptr_t)tbl_phys;
    table[tbl_idx] = 0;

    empty = 1;
    for (uint32_t i = 0; i < 1024; i++) {
        if (table[i] & 0x1u) {
            empty = 0;
            break;
        }
    }
    if (empty) {
        dir[dir_idx] = 0;
        physical_allocator_set_owner(tbl_phys, PAGE_OWNER_ALLOCATOR);
        physical_allocator_free_page(tbl_phys);
    }
}

/*
 * process_alloc_per_proc_dir
 *
 * Allocates a new page directory for a process.  The kernel-space entries
 * (directory indices 768-1023, covering 0xC0000000+) are copied from the
 * global kernel page directory so that kernel code/data/stacks are accessible
 * when this directory is loaded into CR3.  The user-space entries (0-767)
 * start zeroed.
 *
 * Returns the physical address of the new directory, or 0 on failure.
 */
static uint32_t process_alloc_per_proc_dir(void)
{
    uint32_t new_phys;
    uint32_t* new_dir;
    const uint32_t* kern_dir;

    if (paging_directory_phys == 0) {
        return 0;
    }

    new_phys = physical_allocator_alloc_page();
    if (new_phys == 0) {
        return 0;
    }

    new_dir  = (uint32_t*)(uintptr_t)new_phys;
    kern_dir = (const uint32_t*)(uintptr_t)paging_directory_phys;

    memory_set_u8(new_dir, 0, PAGE_SIZE);

    /* Share kernel-space page tables (read-only boundary is enforced by CPL) */
    for (uint32_t i = 768; i < 1024; i++) {
        new_dir[i] = kern_dir[i];
    }

    physical_allocator_set_owner(new_phys, PAGE_OWNER_PAGING_STRUCTURE);
    return new_phys;
}

static int process_track_user_page(struct process_syscall_context* proc, uint32_t page_va)
{
    if (proc == 0) {
        return 0;
    }

    page_va = align_down_u32(page_va, PAGE_SIZE);
    if (!paging_is_user_virtual(page_va)) {
        return 0;
    }

    for (uint32_t i = 0; i < proc->user_mapped_count; i++) {
        if (proc->user_mapped_pages[i] == page_va) {
            return 1;
        }
    }

    if (proc->user_mapped_count >= PROCESS_USER_PAGE_TRACK_MAX) {
        return 0;
    }

    proc->user_mapped_pages[proc->user_mapped_count++] = page_va;
    return 1;
}

static int process_map_user_page(struct process_syscall_context* proc,
                                 uint32_t virtual_address,
                                 uint32_t flags,
                                 uint8_t owner,
                                 uint32_t* physical_out)
{
    uint32_t page_va = align_down_u32(virtual_address, PAGE_SIZE);
    uint32_t existing_phys = 0;

    if (proc == 0 || !paging_is_user_virtual(page_va)) {
        return 0;
    }

    /* If this process has a private page directory, map into it directly. */
    if (proc->page_dir_phys != 0) {
        if (paging_lookup_direct(proc->page_dir_phys, page_va, &existing_phys, 0)) {
            if (!process_track_user_page(proc, page_va)) {
                return 0;
            }
            if (physical_out != 0) {
                *physical_out = existing_phys & 0xFFFFF000u;
            }
            return 1;
        }

        existing_phys = physical_allocator_alloc_page();
        if (existing_phys == 0) {
            return 0;
        }

        if (!paging_map_page_direct(proc->page_dir_phys, page_va, existing_phys, flags)) {
            physical_allocator_free_page(existing_phys);
            return 0;
        }

        physical_allocator_set_owner(existing_phys, owner);

        if (!process_track_user_page(proc, page_va)) {
            paging_unmap_page_direct(proc->page_dir_phys, page_va);
            physical_allocator_free_page(existing_phys);
            return 0;
        }

        if (physical_out != 0) {
            *physical_out = existing_phys;
        }
        return 1;
    }

    /* Fallback: global page directory (no per-process dir allocated yet). */
    if (paging_lookup(page_va, &existing_phys, 0)) {
        if (!process_track_user_page(proc, page_va)) {
            return 0;
        }
        if (physical_out != 0) {
            *physical_out = existing_phys & 0xFFFFF000u;
        }
        return 1;
    }

    if (!paging_map_new_page(page_va, flags, owner, physical_out)) {
        return 0;
    }

    if (!process_track_user_page(proc, page_va)) {
        uint32_t phys = 0;
        (void)paging_lookup(page_va, &phys, 0);
        (void)paging_unmap_page(page_va);
        if (phys != 0) {
            physical_allocator_free_page(phys & 0xFFFFF000u);
        }
        return 0;
    }

    return 1;
}

static void process_unmap_user_pages(struct process_syscall_context* proc)
{
    if (proc == 0) {
        return;
    }

    if (proc->page_dir_phys != 0) {
        /*
         * Per-process page directory path.
         * Walk the tracked VA list, look up the PTE through the physical directory,
         * free the physical user page, and let paging_unmap_page_direct clean up
         * empty page tables.
         */
        for (uint32_t i = 0; i < proc->user_mapped_count; i++) {
            uint32_t page_va = proc->user_mapped_pages[i];
            uint32_t phys = 0;
            if (page_va == 0) {
                continue;
            }
            if (paging_lookup_direct(proc->page_dir_phys, page_va, &phys, 0)) {
                paging_unmap_page_direct(proc->page_dir_phys, page_va);
                if (phys != 0) {
                    physical_allocator_free_page(phys & 0xFFFFF000u);
                }
            }
            proc->user_mapped_pages[i] = 0;
        }
        proc->user_mapped_count = 0;

        /*
         * Switch off the per-process directory before freeing it so that we
         * are never executing with a freed CR3.
         */
        if (paging_enabled) {
            load_page_directory(paging_directory_phys);
        }

        physical_allocator_free_page(proc->page_dir_phys);
        proc->page_dir_phys = 0;
        return;
    }

    /* Global page directory path (legacy / no private dir). */
    for (uint32_t i = 0; i < proc->user_mapped_count; i++) {
        uint32_t page_va = proc->user_mapped_pages[i];
        uint32_t phys = 0;
        if (page_va == 0) {
            continue;
        }
        if (paging_lookup(page_va, &phys, 0)) {
            (void)paging_unmap_page(page_va);
            if (phys != 0) {
                physical_allocator_free_page(phys & 0xFFFFF000u);
            }
        }
        proc->user_mapped_pages[i] = 0;
    }

    proc->user_mapped_count = 0;
}

static void process_init_user_runtime_layout(struct process_syscall_context* proc)
{
    uint32_t pid_slot;
    uint32_t slot_base;

    if (proc == 0 || proc->pid == 0) {
        return;
    }

    pid_slot = proc->pid - 1u;
    slot_base = USER_TASK_BASE + pid_slot * USER_TASK_SLOT_STRIDE;
    proc->user_stack_top = user_task_stack_top_for_pid(proc->pid);
    proc->user_heap_base = align_up_u32(proc->user_stack_top + PAGE_SIZE, PAGE_SIZE);
    proc->user_heap_limit = slot_base + USER_TASK_SLOT_STRIDE - PAGE_SIZE;
    proc->user_heap_brk   = proc->user_heap_base;

    /* Allocate a private page directory for this process. */
    if (paging_enabled && proc->page_dir_phys == 0) {
        proc->page_dir_phys = process_alloc_per_proc_dir();
    }
}

static int process_set_user_brk(struct process_syscall_context* proc, uint32_t new_brk)
{
    uint32_t old_brk;
    uint32_t old_page;
    uint32_t new_page;

    if (proc == 0) {
        return 0;
    }

    if (new_brk < proc->user_heap_base || new_brk > proc->user_heap_limit) {
        return 0;
    }

    old_brk = proc->user_heap_brk;
    old_page = align_up_u32(old_brk, PAGE_SIZE);
    new_page = align_up_u32(new_brk, PAGE_SIZE);

    if (new_page > old_page) {
        for (uint32_t va = old_page; va < new_page; va += PAGE_SIZE) {
            if (!process_map_user_page(proc, va, 0x7u, PAGE_OWNER_USER_TASK, 0)) {
                return 0;
            }
        }
    }

    proc->user_heap_brk = new_brk;
    return 1;
}

static int process_build_initial_stack(struct process_syscall_context* proc, const char* arg0)
{
    uint32_t sp;
    uint32_t arg0_ptr;
    uint32_t argv_ptr;
    uint32_t envp_ptr;
    uint32_t arg0_len;
    uint8_t* mem;

    if (proc == 0 || arg0 == 0) {
        return 0;
    }

    if (!process_map_user_page(proc, proc->user_stack_top - PAGE_SIZE, 0x7u, PAGE_OWNER_USER_TASK, 0)) {
        return 0;
    }

    sp = proc->user_stack_top;
    arg0_len = (uint32_t)str_length(arg0) + 1u;
    if (arg0_len > PAGE_SIZE / 2u) {
        arg0_len = PAGE_SIZE / 2u;
    }

    sp -= arg0_len;
    arg0_ptr = sp;
    mem = (uint8_t*)(uintptr_t)arg0_ptr;
    for (uint32_t i = 0; i < arg0_len; i++) {
        mem[i] = (uint8_t)arg0[i];
        if (arg0[i] == '\0') {
            break;
        }
    }
    mem[arg0_len - 1u] = 0;

    sp &= ~0x3u;

    sp -= 4u;
    *((uint32_t*)(uintptr_t)sp) = 0u;
    envp_ptr = sp;

    sp -= 4u;
    *((uint32_t*)(uintptr_t)sp) = 0u;
    sp -= 4u;
    *((uint32_t*)(uintptr_t)sp) = arg0_ptr;
    argv_ptr = sp;

    sp -= 4u;
    *((uint32_t*)(uintptr_t)sp) = envp_ptr;
    sp -= 4u;
    *((uint32_t*)(uintptr_t)sp) = argv_ptr;
    sp -= 4u;
    *((uint32_t*)(uintptr_t)sp) = 1u;

    proc->user_esp = sp;
    return 1;
}

static int user_load_elf32_image_for_current(struct process_syscall_context* proc,
                                             const struct ramfs_file* file)
{
    const struct elf32_ehdr* ehdr;
    uint32_t lowest_vaddr = 0xFFFFFFFFu;
    uint8_t loaded_segment = 0;

    if (proc == 0 || file == 0 || file->size < sizeof(struct elf32_ehdr)) {
        return -7;
    }

    ehdr = (const struct elf32_ehdr*)file->data;
    if (!(ehdr->e_ident[0] == 0x7Fu && ehdr->e_ident[1] == 'E'
            && ehdr->e_ident[2] == 'L' && ehdr->e_ident[3] == 'F')) {
        return -7;
    }
    if (ehdr->e_ident[4] != 1u || ehdr->e_ident[5] != 1u) {
        return -7;
    }
    if (ehdr->e_machine != 3u || ehdr->e_phnum == 0u) {
        return -7;
    }
    if (ehdr->e_phentsize < sizeof(struct elf32_phdr)) {
        return -7;
    }
    if (ehdr->e_phoff > file->size
            || ehdr->e_phoff + (uint32_t)ehdr->e_phnum * ehdr->e_phentsize > file->size) {
        return -7;
    }

    process_init_user_runtime_layout(proc);
    process_unmap_user_pages(proc);

    for (uint32_t i = 0; i < ehdr->e_phnum; i++) {
        uint32_t ph_off = ehdr->e_phoff + i * ehdr->e_phentsize;
        const struct elf32_phdr* ph = (const struct elf32_phdr*)(file->data + ph_off);
        uint32_t seg_start;
        uint32_t seg_end;

        if (ph->p_type != ELF32_PT_LOAD || ph->p_memsz == 0u) {
            continue;
        }

        if (ph->p_memsz < ph->p_filesz) {
            process_unmap_user_pages(proc);
            return -7;
        }
        if (ph->p_offset > file->size || ph->p_offset + ph->p_filesz > file->size) {
            process_unmap_user_pages(proc);
            return -7;
        }
        if (ph->p_vaddr < USER_SPACE_BASE || ph->p_vaddr >= USER_SPACE_LIMIT) {
            process_unmap_user_pages(proc);
            return -7;
        }
        if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr || ph->p_vaddr + ph->p_memsz > USER_SPACE_LIMIT) {
            process_unmap_user_pages(proc);
            return -7;
        }

        seg_start = align_down_u32(ph->p_vaddr, PAGE_SIZE);
        seg_end = align_up_u32(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);

        for (uint32_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            if (!process_map_user_page(proc, va, 0x7u, PAGE_OWNER_USER_TASK, 0)) {
                process_unmap_user_pages(proc);
                return -8;
            }
        }

        {
            uint8_t* dst = (uint8_t*)(uintptr_t)ph->p_vaddr;
            const uint8_t* src = file->data + ph->p_offset;
            for (uint32_t b = 0; b < ph->p_memsz; b++) {
                dst[b] = 0u;
            }
            for (uint32_t b = 0; b < ph->p_filesz; b++) {
                dst[b] = src[b];
            }
        }

        if (seg_start < lowest_vaddr) {
            lowest_vaddr = seg_start;
        }
        loaded_segment = 1u;
    }

    if (!loaded_segment || ehdr->e_entry < USER_SPACE_BASE || ehdr->e_entry >= USER_SPACE_LIMIT) {
        process_unmap_user_pages(proc);
        return -7;
    }

    if (!process_map_user_page(proc, proc->user_stack_top - PAGE_SIZE, 0x7u, PAGE_OWNER_USER_TASK, 0)) {
        process_unmap_user_pages(proc);
        return -8;
    }

    if (!user_range_valid(ehdr->e_entry, 1u, 0u)) {
        process_unmap_user_pages(proc);
        return -7;
    }

    proc->user_code_va = lowest_vaddr;
    proc->user_eip = ehdr->e_entry;
    if (!process_build_initial_stack(proc, "elf32-app")) {
        process_unmap_user_pages(proc);
        return -8;
    }
    proc->user_ready = 1;
    return 0;
}

static int user_load_pe32_image_for_current(struct process_syscall_context* proc,
                                           const struct ramfs_file* file)
{
    const struct pe32_dos_header* dos_hdr;
    const uint8_t* pe_sig;
    const struct pe32_coff_header* coff_hdr;
    const struct pe32_opt_header* opt_hdr;
    uint32_t lowest_vaddr = 0xFFFFFFFFu;
    uint8_t loaded_section = 0;

    if (proc == 0 || file == 0 || file->size < sizeof(struct pe32_dos_header)) {
        return -7;
    }

    dos_hdr = (const struct pe32_dos_header*)file->data;
    if (dos_hdr->dos_magic[0] != 'M' || dos_hdr->dos_magic[1] != 'Z') {
        return -7;
    }

    if (dos_hdr->pe_offset < sizeof(struct pe32_dos_header) || dos_hdr->pe_offset > file->size - 4u) {
        return -7;
    }

    pe_sig = file->data + dos_hdr->pe_offset;
    if (pe_sig[0] != 'P' || pe_sig[1] != 'E' || pe_sig[2] != 0 || pe_sig[3] != 0) {
        return -7;
    }

    if (dos_hdr->pe_offset + 4u + sizeof(struct pe32_coff_header) > file->size) {
        return -7;
    }

    coff_hdr = (const struct pe32_coff_header*)(pe_sig + 4u);
    if (coff_hdr->machine != PE32_I386_MACHINE) {
        return -7;
    }
    if (coff_hdr->numSymbols > 0u || coff_hdr->pointerToSymbolTable != 0u) {
        return -7;
    }
    if (coff_hdr->sizeOfOptionalHeader < sizeof(struct pe32_opt_header)) {
        return -7;
    }

    opt_hdr = (const struct pe32_opt_header*)((const uint8_t*)coff_hdr + sizeof(struct pe32_coff_header));
    if (opt_hdr->magic != PE32_MAGIC) {
        return -7;
    }
    if (opt_hdr->imageBase < USER_SPACE_BASE || opt_hdr->imageBase >= USER_SPACE_LIMIT) {
        return -7;
    }

    process_init_user_runtime_layout(proc);
    process_unmap_user_pages(proc);

    uint32_t section_start = dos_hdr->pe_offset + 4u + coff_hdr->sizeOfOptionalHeader
                           + sizeof(struct pe32_coff_header);
    uint32_t entry_point = opt_hdr->imageBase + opt_hdr->addressOfEntryPoint;

    for (uint32_t i = 0; i < coff_hdr->num_sections; i++) {
        uint32_t sec_off = section_start + i * sizeof(struct pe32_section_header);
        const struct pe32_section_header* sec;
        uint32_t sec_start;
        uint32_t sec_end;
        uint32_t vaddr;

        if (sec_off + sizeof(struct pe32_section_header) > file->size) {
            process_unmap_user_pages(proc);
            return -7;
        }

        sec = (const struct pe32_section_header*)(file->data + sec_off);
        vaddr = opt_hdr->imageBase + sec->virtualAddress;

        if (sec->virtualSize == 0u) {
            continue;
        }

        if (vaddr < USER_SPACE_BASE || vaddr >= USER_SPACE_LIMIT) {
            process_unmap_user_pages(proc);
            return -7;
        }
        if (vaddr + sec->virtualSize < vaddr || vaddr + sec->virtualSize > USER_SPACE_LIMIT) {
            process_unmap_user_pages(proc);
            return -7;
        }

        sec_start = align_down_u32(vaddr, PAGE_SIZE);
        sec_end = align_up_u32(vaddr + sec->virtualSize, PAGE_SIZE);

        for (uint32_t va = sec_start; va < sec_end; va += PAGE_SIZE) {
            if (!process_map_user_page(proc, va, 0x7u, PAGE_OWNER_USER_TASK, 0)) {
                process_unmap_user_pages(proc);
                return -8;
            }
        }

        if (sec->sizeOfRawData > 0u && sec->pointerToRawData > 0u) {
            if (sec->pointerToRawData > file->size
                    || sec->pointerToRawData + sec->sizeOfRawData > file->size) {
                process_unmap_user_pages(proc);
                return -7;
            }
            uint8_t* dst = (uint8_t*)(uintptr_t)vaddr;
            const uint8_t* src = file->data + sec->pointerToRawData;
            for (uint32_t b = 0; b < sec->sizeOfRawData; b++) {
                dst[b] = src[b];
            }
            for (uint32_t b = sec->sizeOfRawData; b < sec->virtualSize; b++) {
                dst[b] = 0u;
            }
        } else {
            uint8_t* dst = (uint8_t*)(uintptr_t)vaddr;
            for (uint32_t b = 0; b < sec->virtualSize; b++) {
                dst[b] = 0u;
            }
        }

        if (sec_start < lowest_vaddr) {
            lowest_vaddr = sec_start;
        }
        loaded_section = 1u;
    }

    if (!loaded_section || entry_point < USER_SPACE_BASE || entry_point >= USER_SPACE_LIMIT) {
        process_unmap_user_pages(proc);
        return -7;
    }

    if (!process_map_user_page(proc, proc->user_stack_top - PAGE_SIZE, 0x7u, PAGE_OWNER_USER_TASK, 0)) {
        process_unmap_user_pages(proc);
        return -8;
    }

    if (!user_range_valid(entry_point, 1u, 0u)) {
        process_unmap_user_pages(proc);
        return -7;
    }

    proc->user_code_va = lowest_vaddr;
    proc->user_eip = entry_point;
    if (!process_build_initial_stack(proc, "pe32-app")) {
        process_unmap_user_pages(proc);
        return -8;
    }
    proc->user_ready = 1;
    return 0;
}

static enum exec_format exec_detect_format(const uint8_t* data, uint32_t size)
{
    if (data == 0 || size == 0) {
        return EXEC_FORMAT_UNKNOWN;
    }

    if (size >= 4u && data[0] == 0x7Fu && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        return EXEC_FORMAT_ELF32;
    }

    if (size >= 2u && data[0] == 'M' && data[1] == 'Z') {
        return EXEC_FORMAT_PE32;
    }

    if (size >= sizeof(struct mbapp_header)) {
        const struct mbapp_header* hdr = (const struct mbapp_header*)data;
        if (hdr->magic == 0x50504142u && hdr->version == 1u && hdr->flags == 0u) {
            return EXEC_FORMAT_MBAPP;
        }
    }

    return EXEC_FORMAT_RAW;
}

static const char* exec_format_name(enum exec_format fmt)
{
    switch (fmt) {
    case EXEC_FORMAT_RAW:
        return "raw";
    case EXEC_FORMAT_MBAPP:
        return "mbapp";
    case EXEC_FORMAT_ELF32:
        return "elf32";
    case EXEC_FORMAT_PE32:
        return "pe32";
    default:
        return "unknown";
    }
}

static int user_load_ramfs_image_for_current(const char* name, enum exec_format* fmt_out)
{
    struct process_syscall_context* proc = current_process_context();
    int32_t idx = ramfs_find_index(name);
    struct ramfs_file* file;
    enum exec_format fmt;
    uint32_t code_va;
    uint32_t entry_off = 0;
    const uint8_t* src;
    uint32_t src_size;

    if (idx < 0) {
        return -1;
    }

    file = &ramfs_files[(uint32_t)idx];
    if (!file->used || file->size == 0) {
        return -2;
    }

    fmt = exec_detect_format(file->data, file->size);
    if (fmt_out != 0) {
        *fmt_out = fmt;
    }

    if (fmt == EXEC_FORMAT_ELF32) {
        return user_load_elf32_image_for_current(proc, file);
    }
    if (fmt == EXEC_FORMAT_PE32) {
        return user_load_pe32_image_for_current(proc, file);
    }

    process_init_user_runtime_layout(proc);
    code_va = user_task_code_va_for_pid(proc->pid);

    if (fmt == EXEC_FORMAT_MBAPP) {
        const struct mbapp_header* hdr = (const struct mbapp_header*)file->data;
        src = file->data + sizeof(struct mbapp_header);
        src_size = hdr->image_size;
        entry_off = hdr->entry_off;
        /* Validate MBAPP v1 structure */
        if (src_size == 0 || src_size > PAGE_SIZE - sizeof(struct mbapp_header)
                || src_size + sizeof(struct mbapp_header) > file->size
                || entry_off >= src_size
                || hdr->version != 1u
                || hdr->flags != 0u) {
            return -7;
        }
    } else {
        src = file->data;
        src_size = file->size;
    }

    process_unmap_user_pages(proc);

    if (!process_map_user_page(proc, code_va, 0x7u, PAGE_OWNER_USER_TASK, 0)) {
        return -3;
    }

    if (!process_map_user_page(proc, proc->user_stack_top - PAGE_SIZE, 0x7u, PAGE_OWNER_USER_TASK, 0)) {
        process_unmap_user_pages(proc);
        return -4;
    }

    {
        uint8_t* code = (uint8_t*)(uintptr_t)code_va;
        for (uint32_t i = 0; i < PAGE_SIZE; i++) {
            code[i] = 0x90u;
        }
        for (uint32_t i = 0; i < src_size; i++) {
            code[i] = src[i];
        }
    }

    proc->user_code_va = code_va;
    proc->user_eip = code_va + entry_off;
    if (!process_build_initial_stack(proc, name)) {
        process_unmap_user_pages(proc);
        return -4;
    }
    proc->user_ready = 1;
    return 0;
}

void process_fd_print_table(const struct process_syscall_context* proc)
{
    if (proc == 0) {
        return;
    }

    terminal_write("PID ");
    terminal_write_dec_u32(proc->pid);
    terminal_write(" fd table:\n");
    for (uint32_t fd = 0; fd < PROCESS_FD_MAX; fd++) {
        const struct process_fd* desc = &proc->fds[fd];
        terminal_write("  fd ");
        terminal_write_dec_u32(fd);
        terminal_write(": ");
        if (!desc->used || desc->file_index < 0) {
            terminal_write("<free>\n");
            continue;
        }
        terminal_write("name=");
        if (desc->backend == FD_BACKEND_MBFS) {
            if ((uint32_t)desc->file_index < MBFS_MAX_DIRENTS && mbfs_dir[(uint32_t)desc->file_index].used) {
                terminal_write(mbfs_dir[(uint32_t)desc->file_index].name);
            } else {
                terminal_write("<mbfs-invalid>");
            }
            terminal_write(" backend=mbfs");
        } else {
            terminal_write(ramfs_files[(uint32_t)desc->file_index].name);
            terminal_write(" backend=ramfs");
        }
        terminal_write(" flags=");
        terminal_write_hex(desc->flags);
        terminal_write(" off=");
        terminal_write_dec_u32(desc->offset);
        terminal_write("\n");
    }
}

void ramfs_backend_smoke_test(void)
{
    struct process_syscall_context* proc = current_process_context();
    int32_t fd;
    uint8_t tmp[64];
    int32_t n;

    fd = process_fd_open(proc, "APPMSG.TXT", RAMFS_OPEN_READ);
    if (fd < 0) {
        terminal_write("fstest: open APPMSG.TXT failed\n");
        return;
    }

    n = process_fd_read(proc, (uint32_t)fd, tmp, 20u);
    if (n < 0) {
        terminal_write("fstest: read failed\n");
        (void)process_fd_close(proc, (uint32_t)fd);
        return;
    }

    (void)process_fd_close(proc, (uint32_t)fd);

    fd = process_fd_open(proc, "SMOKE.TXT", RAMFS_OPEN_WRITE | RAMFS_OPEN_READ);
    if (fd < 0) {
        terminal_write("fstest: open SMOKE.TXT failed\n");
        return;
    }

    if (n > 0) {
        int32_t wn = process_fd_write(proc, (uint32_t)fd, tmp, (uint32_t)n);
        if (wn < 0) {
            terminal_write("fstest: write failed\n");
            (void)process_fd_close(proc, (uint32_t)fd);
            return;
        }
    }

    (void)process_fd_seek(proc, (uint32_t)fd, 0u);
    n = process_fd_read(proc, (uint32_t)fd, tmp, 20u);
    (void)process_fd_close(proc, (uint32_t)fd);

    if (n >= 0) {
        terminal_write("fstest: ok (copied first 20 bytes into SMOKE.TXT)\n");
    } else {
        terminal_write("fstest: verify read failed\n");
    }
}

static int multiboot_has_memory_map(void)
{
    return boot_multiboot_info != 0 && (boot_multiboot_info->flags & MULTIBOOT_INFO_MEM_MAP) != 0;
}

static int multiboot_has_vbe_info(void)
{
    return boot_multiboot_info != 0 && (boot_multiboot_info->flags & MULTIBOOT_INFO_VBE) != 0;
}

static int multiboot_has_framebuffer_info(void)
{
    return boot_multiboot_info != 0 && (boot_multiboot_info->flags & MULTIBOOT_INFO_FRAMEBUFFER) != 0;
}

static void gui_detect_framebuffer(void)
{
    const struct vbe_mode_info* mode;
    uint64_t fb_addr;
    uint64_t size;

    gui_framebuffer_detected = 0;
    gui_framebuffer_mapped = 0;
    gui_framebuffer_phys = 0;
    gui_framebuffer_virt = 0;
    gui_framebuffer_size = 0;
    gui_framebuffer_pitch = 0;
    gui_framebuffer_width = 0;
    gui_framebuffer_height = 0;
    gui_framebuffer_bpp = 0;

    if (multiboot_has_framebuffer_info()) {
        fb_addr = boot_multiboot_info->framebuffer_addr;
        size = (uint64_t)boot_multiboot_info->framebuffer_pitch *
               (uint64_t)boot_multiboot_info->framebuffer_height;

        if ((fb_addr >> 32) == 0 &&
            boot_multiboot_info->framebuffer_pitch != 0 &&
            boot_multiboot_info->framebuffer_width != 0 &&
            boot_multiboot_info->framebuffer_height != 0 &&
            (boot_multiboot_info->framebuffer_bpp == 24 || boot_multiboot_info->framebuffer_bpp == 32) &&
            size != 0 && size <= 0xFFFFFFFFu) {
            gui_framebuffer_detected = 1;
            gui_framebuffer_phys = (uint32_t)fb_addr;
            gui_framebuffer_size = (uint32_t)size;
            gui_framebuffer_pitch = boot_multiboot_info->framebuffer_pitch;
            gui_framebuffer_width = boot_multiboot_info->framebuffer_width;
            gui_framebuffer_height = boot_multiboot_info->framebuffer_height;
            gui_framebuffer_bpp = boot_multiboot_info->framebuffer_bpp;
            return;
        }
    }

    if (!multiboot_has_vbe_info() || boot_multiboot_info->vbe_mode_info == 0) {
        return;
    }

    mode = (const struct vbe_mode_info*)(uintptr_t)boot_multiboot_info->vbe_mode_info;
    if (mode->framebuffer == 0 || mode->pitch == 0 || mode->width == 0 || mode->height == 0) {
        return;
    }

    if (mode->bpp != 24 && mode->bpp != 32) {
        return;
    }

    size = (uint64_t)mode->pitch * (uint64_t)mode->height;
    if (size == 0 || size > 0xFFFFFFFFu) {
        return;
    }

    gui_framebuffer_detected = 1;
    gui_framebuffer_phys = mode->framebuffer;
    gui_framebuffer_size = (uint32_t)size;
    gui_framebuffer_pitch = mode->pitch;
    gui_framebuffer_width = mode->width;
    gui_framebuffer_height = mode->height;
    gui_framebuffer_bpp = mode->bpp;
}

static int gui_map_framebuffer(void)
{
    uint32_t phys_base;
    uint32_t offset;
    uint32_t total_size;

    if (!paging_enabled || !gui_framebuffer_detected || gui_framebuffer_mapped) {
        return gui_framebuffer_mapped ? 1 : 0;
    }

    phys_base = align_down_u32(gui_framebuffer_phys, PAGE_SIZE);
    offset = gui_framebuffer_phys - phys_base;
    total_size = align_up_u32(gui_framebuffer_size + offset, PAGE_SIZE);

    for (uint32_t mapped = 0; mapped < total_size; mapped += PAGE_SIZE) {
        if (!paging_map_page(GUI_FRAMEBUFFER_VIRT_BASE + mapped, phys_base + mapped, 0x3u)) {
            return 0;
        }
    }

    gui_framebuffer_virt = GUI_FRAMEBUFFER_VIRT_BASE + offset;
    gui_framebuffer_mapped = 1;
    return 1;
}

/* Forward declaration — defined in heap section below */
static void* kmalloc_aligned(uint32_t size, uint32_t alignment);

/* Allocate back-buffer from kernel heap (call after heap is ready). */
static void gui_backbuffer_init(void)
{
    if (gui_backbuffer_ready || gui_framebuffer_size == 0) { return; }
    gui_backbuffer = (uint8_t*)kmalloc_aligned(gui_framebuffer_size, 16u);
    if (gui_backbuffer) {
        gui_backbuffer_ready = 1;
    }
}

/* Blit back-buffer to live framebuffer in one pass. */
static void gui_flip(void)
{
    uint32_t i;
    volatile uint8_t* dst;
    const uint8_t* src;

    if (!gui_framebuffer_mapped || !gui_backbuffer_ready) { return; }
    dst = (volatile uint8_t*)(uintptr_t)gui_framebuffer_virt;
    src = gui_backbuffer;
    for (i = 0; i < gui_framebuffer_size; i++) {
        dst[i] = src[i];
    }
}

static void gui_put_pixel_front(uint32_t x, uint32_t y, uint32_t color)
{
    volatile uint8_t* base;
    uint32_t pixel_offset;

    if (!gui_framebuffer_mapped || x >= gui_framebuffer_width || y >= gui_framebuffer_height) {
        return;
    }

    base = (volatile uint8_t*)(uintptr_t)gui_framebuffer_virt;
    pixel_offset = y * gui_framebuffer_pitch + x * (gui_framebuffer_bpp / 8u);

    base[pixel_offset + 0] = (uint8_t)(color & 0xFFu);
    base[pixel_offset + 1] = (uint8_t)((color >> 8) & 0xFFu);
    base[pixel_offset + 2] = (uint8_t)((color >> 16) & 0xFFu);
    if (gui_framebuffer_bpp == 32) {
        base[pixel_offset + 3] = (uint8_t)((color >> 24) & 0xFFu);
    }
}

static void gui_copy_backbuffer_pixel_to_front(uint32_t x, uint32_t y)
{
    volatile uint8_t* dst;
    const uint8_t* src;
    uint32_t off;
    uint32_t bytes_per_px;

    if (!gui_framebuffer_mapped || !gui_backbuffer_ready
            || x >= gui_framebuffer_width || y >= gui_framebuffer_height) {
        return;
    }

    bytes_per_px = gui_framebuffer_bpp / 8u;
    off = y * gui_framebuffer_pitch + x * bytes_per_px;
    dst = (volatile uint8_t*)(uintptr_t)gui_framebuffer_virt;
    src = gui_backbuffer;
    for (uint32_t i = 0; i < bytes_per_px; i++) {
        dst[off + i] = src[off + i];
    }
}

static void gui_restore_cursor_front(uint32_t x, uint32_t y)
{
    gui_copy_backbuffer_pixel_to_front(x, y);
    gui_copy_backbuffer_pixel_to_front(x + 1u, y);
    gui_copy_backbuffer_pixel_to_front(x, y + 1u);
    gui_copy_backbuffer_pixel_to_front(x + 1u, y + 1u);
}

static void gui_draw_cursor_front(uint32_t x, uint32_t y)
{
    const uint32_t color = 0xFFFF00u;

    gui_put_pixel_front(x, y, color);
    gui_put_pixel_front(x + 1u, y, color);
    gui_put_pixel_front(x, y + 1u, color);
    gui_put_pixel_front(x + 1u, y + 1u, color);
}

static uint8_t wm_fast_cursor_move(void)
{
    if (!gui_backbuffer_ready || !gui_framebuffer_mapped) {
        return 0u;
    }

    if (wm_cursor_front_valid) {
        gui_restore_cursor_front(wm_cursor_prev_x, wm_cursor_prev_y);
    }

    gui_draw_cursor_front(mouse_x, mouse_y);
    wm_cursor_prev_x = mouse_x;
    wm_cursor_prev_y = mouse_y;
    wm_cursor_front_valid = 1u;
    return 1u;
}

static void gui_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    uint8_t* base;
    uint32_t pixel_offset;

    if (!gui_framebuffer_mapped || x >= gui_framebuffer_width || y >= gui_framebuffer_height) {
        return;
    }

    /* Write to back-buffer if available, otherwise directly to framebuffer */
    if (gui_backbuffer_ready) {
        base = gui_backbuffer;
    } else {
        base = (uint8_t*)(uintptr_t)gui_framebuffer_virt;
    }
    pixel_offset = y * gui_framebuffer_pitch + x * (gui_framebuffer_bpp / 8u);

    base[pixel_offset + 0] = (uint8_t)(color & 0xFFu);
    base[pixel_offset + 1] = (uint8_t)((color >> 8) & 0xFFu);
    base[pixel_offset + 2] = (uint8_t)((color >> 16) & 0xFFu);
    if (gui_framebuffer_bpp == 32) {
        base[pixel_offset + 3] = (uint8_t)((color >> 24) & 0xFFu);
    }
}

static void gui_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    uint32_t x_end = x + w;
    uint32_t y_end = y + h;

    if (x >= gui_framebuffer_width || y >= gui_framebuffer_height) {
        return;
    }

    if (x_end > gui_framebuffer_width) {
        x_end = gui_framebuffer_width;
    }
    if (y_end > gui_framebuffer_height) {
        y_end = gui_framebuffer_height;
    }

    for (uint32_t py = y; py < y_end; py++) {
        for (uint32_t px = x; px < x_end; px++) {
            gui_put_pixel(px, py, color);
        }
    }
}

/* 8x8 bitmap font, printable ASCII 0x20-0x7E (95 chars).
   Each entry is 8 rows; bit 7 of each byte = leftmost pixel. */
static const uint8_t gui_font_8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x20 space */
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, /* 0x21 ! */
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, /* 0x22 " */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* 0x23 # */
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, /* 0x24 $ */
    {0x00,0x66,0x6C,0x18,0x30,0x66,0x46,0x00}, /* 0x25 % */
    {0x1C,0x36,0x1C,0x38,0x6F,0x66,0x3B,0x00}, /* 0x26 & */
    {0x06,0x06,0x0C,0x00,0x00,0x00,0x00,0x00}, /* 0x27 ' */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* 0x28 ( */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* 0x29 ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 0x2A * */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* 0x2B + */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* 0x2C , */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* 0x2D - */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* 0x2E . */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* 0x2F / */
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, /* 0x30 0 */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 0x31 1 */
    {0x3C,0x66,0x06,0x0C,0x30,0x60,0x7E,0x00}, /* 0x32 2 */
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, /* 0x33 3 */
    {0x06,0x0E,0x1E,0x66,0x7F,0x06,0x06,0x00}, /* 0x34 4 */
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, /* 0x35 5 */
    {0x3C,0x66,0x60,0x7C,0x66,0x66,0x3C,0x00}, /* 0x36 6 */
    {0x7E,0x66,0x0C,0x18,0x18,0x18,0x18,0x00}, /* 0x37 7 */
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, /* 0x38 8 */
    {0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00}, /* 0x39 9 */
    {0x00,0x00,0x18,0x18,0x00,0x18,0x18,0x00}, /* 0x3A : */
    {0x00,0x00,0x18,0x18,0x00,0x18,0x18,0x30}, /* 0x3B ; */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* 0x3C < */
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, /* 0x3D = */
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* 0x3E > */
    {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00}, /* 0x3F ? */
    {0x3E,0x63,0x6F,0x69,0x6F,0x60,0x3E,0x00}, /* 0x40 @ */
    {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}, /* 0x41 A */
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, /* 0x42 B */
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, /* 0x43 C */
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, /* 0x44 D */
    {0x7E,0x60,0x60,0x78,0x60,0x60,0x7E,0x00}, /* 0x45 E */
    {0x7E,0x60,0x60,0x78,0x60,0x60,0x60,0x00}, /* 0x46 F */
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00}, /* 0x47 G */
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, /* 0x48 H */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 0x49 I */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00}, /* 0x4A J */
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, /* 0x4B K */
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, /* 0x4C L */
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, /* 0x4D M */
    {0x66,0x76,0x7E,0x6E,0x66,0x66,0x66,0x00}, /* 0x4E N */
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, /* 0x4F O */
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, /* 0x50 P */
    {0x3C,0x66,0x66,0x66,0x66,0x3C,0x0E,0x00}, /* 0x51 Q */
    {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00}, /* 0x52 R */
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, /* 0x53 S */
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* 0x54 T */
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, /* 0x55 U */
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, /* 0x56 V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* 0x57 W */
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, /* 0x58 X */
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, /* 0x59 Y */
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, /* 0x5A Z */
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* 0x5B [ */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* 0x5C \ */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* 0x5D ] */
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, /* 0x5E ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 0x5F _ */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* 0x60 ` */
    {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00}, /* 0x61 a */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, /* 0x62 b */
    {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00}, /* 0x63 c */
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, /* 0x64 d */
    {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, /* 0x65 e */
    {0x1C,0x30,0x30,0x7C,0x30,0x30,0x30,0x00}, /* 0x66 f */
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x7C}, /* 0x67 g */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, /* 0x68 h */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* 0x69 i */
    {0x06,0x00,0x06,0x06,0x06,0x06,0x66,0x3C}, /* 0x6A j */
    {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00}, /* 0x6B k */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 0x6C l */
    {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00}, /* 0x6D m */
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, /* 0x6E n */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00}, /* 0x6F o */
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, /* 0x70 p */
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06}, /* 0x71 q */
    {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00}, /* 0x72 r */
    {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00}, /* 0x73 s */
    {0x18,0x18,0x7E,0x18,0x18,0x18,0x0E,0x00}, /* 0x74 t */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00}, /* 0x75 u */
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, /* 0x76 v */
    {0x00,0x00,0x63,0x6B,0x7F,0x3E,0x36,0x00}, /* 0x77 w */
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, /* 0x78 x */
    {0x00,0x00,0x66,0x66,0x3E,0x06,0x3C,0x00}, /* 0x79 y */
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, /* 0x7A z */
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, /* 0x7B { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* 0x7C | */
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, /* 0x7D } */
    {0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00}, /* 0x7E ~ */
};

/* Draw one 8x8 character at pixel (x,y). fg/bg are 0x00RRGGBB. */
static void gui_draw_char(uint32_t x, uint32_t y, char c,
                          uint32_t fg, uint32_t bg)
{
    uint32_t idx;
    if ((unsigned char)c < 0x20u || (unsigned char)c > 0x7Eu) {
        c = '?';
    }
    idx = (uint32_t)((unsigned char)c - 0x20u);
    for (uint32_t row = 0; row < 8u; row++) {
        uint8_t bits = gui_font_8x8[idx][row];
        for (uint32_t col = 0; col < 8u; col++) {
            uint32_t color = (bits & (0x80u >> col)) ? fg : bg;
            if (color != 0xFFFFFFFFu) {
                gui_put_pixel(x + col, y + row, color);
            }
        }
    }
}

/* Draw a NUL-terminated string at pixel (x,y), wrapping at the FB edge.
   Pass bg=0xFFFFFFFF to draw fg only (transparent background). */
static void gui_draw_string(uint32_t x, uint32_t y, const char* str,
                            uint32_t fg, uint32_t bg)
{
    uint32_t cx = x;
    while (*str != '\0') {
        if (cx + 8u > gui_framebuffer_width) {
            break;
        }
        gui_draw_char(cx, y, *str, fg, bg);
        cx += 8u;
        str++;
    }
}

/* -----------------------------------------------------------------------
 * Simple Window Manager
 * ----------------------------------------------------------------------- */

/* Forward declaration — defined after the WM block */
static void gui_draw_cursor(void);

#define WM_MAX_WINDOWS   8
#define WM_TITLE_HEIGHT  24
#define WM_TITLE_MAX     32

enum wm_window_type {
    WM_TYPE_GENERIC = 0,
    WM_TYPE_TERMINAL = 1,
    WM_TYPE_FILEMANAGER = 2,
    WM_TYPE_VIEWER = 3
};

struct wm_window {
    uint8_t  used;
    uint8_t  focused;
    uint8_t  type;        /* enum wm_window_type */
    uint8_t  minimized;   /* boolean */
    int32_t  x;
    int32_t  y;
    uint32_t w;
    uint32_t h;
    char     title[WM_TITLE_MAX];
    uint32_t color_body;
    uint32_t color_titlebar;
    uint32_t color_titlebar_focused;
    uint32_t content_id;  /* per-window content index (e.g., term/fs slot) */
};

static struct wm_window wm_windows[WM_MAX_WINDOWS];
static uint32_t wm_window_count = 0;
static int32_t  wm_focused_index = -1;
static uint8_t  wm_drag_active = 0;
static int32_t  wm_drag_index = -1;
static int32_t  wm_drag_off_x = 0;
static int32_t  wm_drag_off_y = 0;
static uint8_t  wm_prev_buttons = 0;
static uint8_t  wm_enabled = 0;

/* Per-window filesystem view state for simple file manager windows */
struct wm_fs_state {
    uint8_t used;
    char    path[MBFS_PATH_MAX];
    uint32_t scroll;
    uint32_t selected;
};
static struct wm_fs_state wm_fs_states[WM_MAX_WINDOWS];

static void wm_str_copy(char* dst, const char* src, uint32_t max)
{
    uint32_t i = 0;
    while (i + 1 < max && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}
/* Per-window terminal view state */
struct wm_term_state {
    uint8_t used;
    uint32_t scroll; /* logical top line index */
    char input[SHELL_INPUT_MAX];
    uint32_t input_len;
    uint32_t input_cursor;
};
static struct wm_term_state wm_term_states[WM_MAX_WINDOWS];

static int32_t wm_open_window(int32_t x, int32_t y, uint32_t w, uint32_t h,
                               const char* title)
{
    if (wm_window_count >= WM_MAX_WINDOWS) {
        return -1;
    }
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!wm_windows[i].used) {
            wm_windows[i].used               = 1;
            wm_windows[i].focused            = 0;
            wm_windows[i].type               = (uint8_t)WM_TYPE_GENERIC;
            wm_windows[i].minimized          = 0;
            wm_windows[i].content_id         = i;
            wm_windows[i].x                  = x;
            wm_windows[i].y                  = y;
            wm_windows[i].w                  = w;
            wm_windows[i].h                  = h;
            wm_windows[i].color_body         = 0x00374257u;
            wm_windows[i].color_titlebar     = 0x004F6078u;
            wm_windows[i].color_titlebar_focused = 0x0066A0D0u;
            wm_str_copy(wm_windows[i].title, title, WM_TITLE_MAX);
            wm_focused_index = (int32_t)i;
            wm_windows[i].focused = 1;
            wm_window_count++;
            return (int32_t)i;
        }
    }
    return -1;
}

static void wm_close_window(uint32_t idx)
{
    if (idx >= WM_MAX_WINDOWS || !wm_windows[idx].used) {
        return;
    }
    wm_windows[idx].used    = 0;
    wm_windows[idx].focused = 0;
    /* If this was a file-manager or terminal window, clear its state */
    if (idx < WM_MAX_WINDOWS) {
        if (wm_fs_states[idx].used) {
            wm_fs_states[idx].used = 0;
            wm_fs_states[idx].path[0] = '\0';
            wm_fs_states[idx].scroll = 0;
            wm_fs_states[idx].selected = 0;
        }
        if (wm_term_states[idx].used) {
            wm_term_states[idx].used = 0;
            wm_term_states[idx].scroll = 0;
            wm_term_states[idx].input_len = 0;
            wm_term_states[idx].input_cursor = 0;
            wm_term_states[idx].input[0] = '\0';
        }
        /* If this window hosted an app_proc (terminal), free the proc */
        if (wm_windows[idx].type == WM_TYPE_TERMINAL) {
            uint32_t pid = wm_windows[idx].content_id;
            if (pid < APP_PROC_MAX && app_procs[pid].used) {
                app_procs[pid].used = 0;
            }
        }
        wm_windows[idx].type = (uint8_t)WM_TYPE_GENERIC;
        wm_windows[idx].content_id = 0;
        wm_windows[idx].minimized = 0;
    }
    wm_window_count--;
    if (wm_focused_index == (int32_t)idx) {
        wm_focused_index = -1;
        for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++) {
            if (wm_windows[i].used) {
                wm_focused_index = (int32_t)i;
                wm_windows[i].focused = 1;
                break;
            }
        }
    }
}

static void wm_draw_window(const struct wm_window* win)
{
    int32_t x = win->x;
    int32_t y = win->y;

    /* Clip to screen */
    if (x >= (int32_t)gui_framebuffer_width || y >= (int32_t)gui_framebuffer_height) {
        return;
    }

    uint32_t ux = (x < 0) ? 0u : (uint32_t)x;
    uint32_t uy = (y < 0) ? 0u : (uint32_t)y;
    uint32_t uw = win->w;
    uint32_t uh = win->h;

    if (ux + uw > gui_framebuffer_width)  { uw = gui_framebuffer_width - ux; }
    if (uy + uh > gui_framebuffer_height) { uh = gui_framebuffer_height - uy; }

    /* Body */
    if (uh > WM_TITLE_HEIGHT) {
        gui_fill_rect(ux, uy + WM_TITLE_HEIGHT, uw, uh - WM_TITLE_HEIGHT, win->color_body);
    }

    /* Title bar */
    uint32_t tb_h = (uh < WM_TITLE_HEIGHT) ? uh : WM_TITLE_HEIGHT;
    uint32_t tb_color = win->focused ? win->color_titlebar_focused : win->color_titlebar;
    gui_fill_rect(ux, uy, uw, tb_h, tb_color);

    /* Title text */
    if (tb_h >= 8u) {
        uint32_t ty = uy + (tb_h - 8u) / 2u;
        gui_draw_string(ux + 8u, ty, win->title, 0xFFFFFFu, 0xFFFFFFFFu);
    }

    /* Close button (red square top-right) */
    if (uw >= 16u && tb_h >= 8u) {
        uint32_t bx = ux + uw - 16u;
        uint32_t by_btn = uy + (tb_h - 8u) / 2u;
        gui_fill_rect(bx, by_btn, 8u, 8u, 0xCC3333u);
    }

    /* Per-window content rendering: terminal or file-manager */
    {
        int win_idx = (int)(win - wm_windows);
        if (win_idx >= 0 && win_idx < (int)WM_MAX_WINDOWS) {
            if (wm_windows[win_idx].minimized) { return; }

            if (wm_windows[win_idx].type == WM_TYPE_TERMINAL && wm_term_states[win_idx].used && uh > WM_TITLE_HEIGHT + 8u) {
                uint32_t content_x = ux + 8u;
                uint32_t content_y = uy + WM_TITLE_HEIGHT + 6u;
                uint32_t content_w = (uw > 16u) ? (uw - 16u) : 0u;
                uint32_t content_h = (uh > WM_TITLE_HEIGHT + 12u) ? (uh - WM_TITLE_HEIGHT - 12u) : 0u;
                uint32_t cols = content_w / 8u;
                uint32_t rows = content_h / 10u;
                uint32_t rows_for_log;
                uint32_t start_log = 0;

                if (cols == 0u || rows == 0u) { return; }

                rows_for_log = (rows > 1u) ? (rows - 1u) : 0u;
                /* If the window has an explicit scroll, honor it, otherwise auto-scroll to bottom */
                if (wm_term_states[win_idx].scroll > 0u) {
                    start_log = wm_term_states[win_idx].scroll;
                } else if (gui_term_count > rows_for_log) {
                    start_log = gui_term_count - rows_for_log;
                }

                /* If this terminal window is backed by an app_proc, render its stdout
                 * history; otherwise fall back to the global gui_term log. */
                uint32_t pid = wm_windows[win_idx].content_id;
                if (pid < APP_PROC_MAX && app_procs[pid].used) {
                    /* Copy recent stdout into a temporary buffer and split into lines */
                    char outbuf[1024];
                    uint32_t outlen = app_proc_copy_stdout((int)pid, outbuf, sizeof(outbuf));
                    char* lines[256];
                    uint32_t line_count = 0;
                    char* start = outbuf;
                    for (uint32_t i = 0; i < outlen && line_count < (uint32_t)(sizeof(lines) / sizeof(lines[0])); i++) {
                        if (outbuf[i] == '\n') {
                            outbuf[i] = '\0';
                            lines[line_count++] = start;
                            start = outbuf + i + 1;
                        }
                    }
                    if (start < outbuf + outlen && line_count < (uint32_t)(sizeof(lines) / sizeof(lines[0]))) {
                        lines[line_count++] = start;
                    }

                    uint32_t first_line = (line_count > rows_for_log) ? (line_count - rows_for_log) : 0u;
                    for (uint32_t r = 0; r < rows_for_log; r++) {
                        char line_buf[GUI_TERM_LOG_COLS + 1u];
                        if (first_line + r < line_count) {
                            const char* src = lines[first_line + r];
                            uint32_t i = 0;
                            while (i < cols && src[i] != '\0' && i < GUI_TERM_LOG_COLS) {
                                line_buf[i] = src[i];
                                i++;
                            }
                            line_buf[i] = '\0';
                        } else {
                            line_buf[0] = '\0';
                        }
                        gui_draw_string(content_x, content_y + r * 10u, line_buf, 0xDDE8F5u, 0xFFFFFFFFu);
                    }
                } else {
                    for (uint32_t r = 0; r < rows_for_log; r++) {
                        const char* src = gui_term_line_at(start_log + r);
                        char line_buf[GUI_TERM_LOG_COLS + 1u];
                        uint32_t i = 0;

                        while (i < cols && src[i] != '\0' && i < GUI_TERM_LOG_COLS) {
                            line_buf[i] = src[i];
                            i++;
                        }
                        line_buf[i] = '\0';
                        gui_draw_string(content_x, content_y + r * 10u, line_buf, 0xDDE8F5u, 0xFFFFFFFFu);
                    }
                }

                {
                    char input_buf[GUI_TERM_LOG_COLS + 1u];
                    uint32_t pos = 0;
                    uint32_t cursor_pos;

                    const char* prompt = "MBos> ";
                    for (uint32_t i = 0; prompt[i] != '\0' && pos < GUI_TERM_LOG_COLS; i++) {
                        input_buf[pos++] = prompt[i];
                    }

                    /* Use per-window terminal input buffer instead of kernel shell_input */
                    uint32_t term_input_len = wm_term_states[win_idx].input_len;
                    uint32_t term_input_cursor = wm_term_states[win_idx].input_cursor;
                    for (uint32_t i = 0; i < term_input_len && pos < GUI_TERM_LOG_COLS; i++) {
                        input_buf[pos++] = (uint8_t)wm_term_states[win_idx].input[i];
                    }

                    cursor_pos = 6u + term_input_cursor;
                    if (((timer_ticks / 25u) & 1u) != 0u && cursor_pos < GUI_TERM_LOG_COLS) {
                        if (cursor_pos >= pos) {
                            input_buf[pos++] = '_';
                        } else {
                            input_buf[cursor_pos] = '_';
                        }
                    }

                    if (pos > cols) {
                        uint32_t shift = pos - cols;
                        uint32_t remain = pos - shift;
                        for (uint32_t i = 0; i < remain; i++) {
                            input_buf[i] = input_buf[i + shift];
                        }
                        pos = remain;
                    }

                    input_buf[pos] = '\0';
                    gui_draw_string(content_x,
                                    content_y + (rows > 0u ? (rows - 1u) * 10u : 0u),
                                    input_buf,
                                    0x99FF99u,
                                    0xFFFFFFFFu);
                }
                return;
            }

            if (wm_windows[win_idx].type == WM_TYPE_FILEMANAGER && wm_fs_states[win_idx].used && uh > WM_TITLE_HEIGHT + 8u) {
                uint32_t content_x = ux + 8u;
                uint32_t content_y = uy + WM_TITLE_HEIGHT + 6u;
                uint32_t content_w = (uw > 16u) ? (uw - 16u) : 0u;
                uint32_t content_h = (uh > WM_TITLE_HEIGHT + 12u) ? (uh - WM_TITLE_HEIGHT - 12u) : 0u;
                uint32_t cols = content_w / 8u;
                uint32_t rows = content_h / 10u;

                if (cols == 0u || rows == 0u) { return; }

                static struct mbfs_dir_entry entries[MBFS_MAX_DIRENTS];
                uint32_t entry_count = 0;
                int rc = diskfs_get_entries(wm_fs_states[win_idx].path, entries, MBFS_MAX_DIRENTS, &entry_count);
                if (rc != 0) {
                    /* Try to mount MBFS on-demand if not mounted */
                    if (rc == -2) {
                        if (diskfs_try_mount()) {
                            rc = diskfs_get_entries(wm_fs_states[win_idx].path, entries, MBFS_MAX_DIRENTS, &entry_count);
                        }
                    }
                }

                uint8_t used_ramfs = 0;
                if (rc != 0) {
                    /* Fallback: show RAMFS files when MBFS isn't available */
                    uint32_t rcopied = 0;
                    for (uint32_t ri = 0; ri < RAMFS_MAX_FILES && rcopied < MBFS_MAX_DIRENTS; ri++) {
                        if (!ramfs_files[ri].used) { continue; }
                        entries[rcopied].used = 1;
                        entries[rcopied].inode_index = 0;
                        entries[rcopied].type = MBFS_INODE_TYPE_FILE;
                        str_copy_bounded(entries[rcopied].name, ramfs_files[ri].name, MBFS_NAME_MAX);
                        rcopied++;
                    }
                    used_ramfs = (rcopied > 0) ? 1 : 0;
                    if (rcopied == 0) {
                        gui_draw_string(content_x, content_y, "MBFS: not mounted or error", 0xFFAAAAu, 0xFFFFFFFFu);
                    } else {
                        uint32_t start = wm_fs_states[win_idx].scroll;
                        /* Draw header indicating RAMFS fallback */
                        gui_draw_string(content_x, content_y - 12u, "RAMFS (fallback):", 0xFFDD88u, 0xFFFFFFFFu);
                        for (uint32_t r = 0; r < rows && r + start < rcopied; r++) {
                            uint32_t eidx = r + start;
                            const char* name = entries[eidx].name;
                            uint32_t color = 0xFFFFFFu;
                            gui_draw_string(content_x, content_y + r * 10u, name, color, 0xFFFFFFFFu);
                        }
                    }
                } else {
                    uint32_t start = wm_fs_states[win_idx].scroll;
                    /* Draw header indicating MBFS */
                    gui_draw_string(content_x, content_y - 12u, "MBFS:", 0x99CCFFu, 0xFFFFFFFFu);
                    for (uint32_t r = 0; r < rows && r + start < entry_count; r++) {
                        uint32_t eidx = r + start;
                        const char* name = entries[eidx].name;
                        uint32_t color = (entries[eidx].type == MBFS_INODE_TYPE_DIR) ? 0x99CCFFu : 0xFFFFFFu;
                        gui_draw_string(content_x, content_y + r * 10u, name, color, 0xFFFFFFFFu);
                    }
                }
                return;
            }
        }
    }
}

/* Draw a simple Start menu anchored to bottom-left (above taskbar). */
static void wm_draw_start_menu(uint32_t tb_y)
{
    if (!wm_start_menu_visible || !gui_framebuffer_mapped) { return; }

    uint32_t menu_w = wm_start_menu_width;
    uint32_t menu_h = wm_start_menu_height;
    uint32_t mx = 8u;
    uint32_t my = (tb_y > menu_h) ? (tb_y - menu_h) : 0u;

    gui_fill_rect(mx, my, menu_w, menu_h, 0x0022332Bu);
    gui_fill_rect(mx + 1u, my + 1u, menu_w - 2u, 22u, 0x00384C63u);
    gui_draw_string(mx + 8u, my + 4u, "Start", 0xFFFFFFu, 0xFFFFFFFFu);

    /* A few example items for now */
    gui_draw_string(mx + 8u, my + 36u, "Terminal", 0xFFFFFFu, 0xFFFFFFFFu);
    gui_draw_string(mx + 8u, my + 56u, "Files", 0xFFFFFFu, 0xFFFFFFFFu);
    gui_draw_string(mx + 8u, my + 76u, "Reboot", 0xFFAAAAu, 0xFFFFFFFFu);
}

static void wm_render(void)
{
    if (!gui_framebuffer_mapped || !wm_enabled) {
        return;
    }

    /* Desktop background */
    gui_fill_rect(0, 0, gui_framebuffer_width, gui_framebuffer_height, 0x001E2A3Au);

    /* Draw windows back-to-front (simple painter's algorithm) */
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (wm_windows[i].used && !wm_windows[i].minimized && wm_focused_index != (int32_t)i) {
            wm_draw_window(&wm_windows[i]);
        }
    }
    /* Draw focused window last so it appears on top */
    if (wm_focused_index >= 0 && wm_focused_index < (int32_t)WM_MAX_WINDOWS
            && wm_windows[wm_focused_index].used && !wm_windows[wm_focused_index].minimized) {
        wm_draw_window(&wm_windows[wm_focused_index]);
    }

    /* Taskbar */
    uint32_t tb_y = gui_framebuffer_height > 40u ? gui_framebuffer_height - 40u : 0u;
    gui_fill_rect(0, tb_y, gui_framebuffer_width, 40u, 0x00283445u);
    uint32_t mbos_color = wm_start_menu_visible ? 0x99CCFFu : 0xFFFFFFu;
    gui_draw_string(8u, tb_y + 13u, "MBos", mbos_color, 0xFFFFFFFFu);

    /* Taskbar: window buttons (icon + title) */
    uint32_t tbx = 72u;
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!wm_windows[i].used) { continue; }

        uint32_t btn_color;
        if (wm_windows[i].minimized) {
            btn_color = (wm_focused_index == (int32_t)i) ? 0x00384C63u : 0x00203038u;
        } else {
            btn_color = (wm_focused_index == (int32_t)i) ? 0x004F7090u : 0x00304050u;
        }

        gui_fill_rect(tbx, tb_y + 6u, 120u, 28u, btn_color);

        /* Small icon to indicate window type */
        uint32_t icon_x = tbx + 6u;
        uint32_t icon_y = tb_y + 10u;
        uint32_t icon_col = 0xAAAAAAu;
        if (wm_windows[i].type == WM_TYPE_TERMINAL) {
            icon_col = 0x88CC88u;
        } else if (wm_windows[i].type == WM_TYPE_FILEMANAGER) {
            icon_col = 0x88BBFFu;
        } else if (wm_windows[i].type == WM_TYPE_VIEWER) {
            icon_col = 0xFFCC88u;
        }
        gui_fill_rect(icon_x, icon_y, 12u, 12u, icon_col);
        if (wm_windows[i].minimized) {
            gui_fill_rect(icon_x + 3u, icon_y + 9u, 6u, 2u, 0x000000u);
        }

        /* Truncate title so it fits inside the button */
        char tbuf[20];
        uint32_t ti = 0;
        while (ti + 1 < sizeof(tbuf) && wm_windows[i].title[ti] != '\0' && ti < 13u) {
            tbuf[ti] = wm_windows[i].title[ti];
            ti++;
        }
        tbuf[ti] = '\0';
        gui_draw_string(tbx + 24u, tb_y + 14u, tbuf, 0xFFFFFFu, 0xFFFFFFFFu);

        tbx += 128u;
    }

    /* Draw Start menu if visible */
    wm_draw_start_menu(tb_y);

    gui_flip();   /* blit back-buffer to live framebuffer */
    gui_draw_cursor_front(mouse_x, mouse_y);
    wm_cursor_prev_x = mouse_x;
    wm_cursor_prev_y = mouse_y;
    wm_cursor_front_valid = 1u;
}

/* Hit-test: returns window index whose title bar was clicked, or -1 */
static int32_t wm_hit_titlebar(int32_t mx, int32_t my)
{
    /* Test focused window first (top-most) */
    for (int32_t pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++) {
            if (!wm_windows[i].used || wm_windows[i].minimized) { continue; }
            int32_t skip = (pass == 0) ? ((int32_t)i != wm_focused_index) :
                                          ((int32_t)i == wm_focused_index);
            if (skip) { continue; }
            int32_t wx = wm_windows[i].x;
            int32_t wy = wm_windows[i].y;
            int32_t ww = (int32_t)wm_windows[i].w;
            if (mx >= wx && mx < wx + ww &&
                my >= wy && my < wy + WM_TITLE_HEIGHT) {
                return (int32_t)i;
            }
        }
    }
    return -1;
}

/* Hit-test: returns window index for any part of window, or -1 */
static int32_t wm_hit_any(int32_t mx, int32_t my)
{
    for (int32_t pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++) {
            if (!wm_windows[i].used || wm_windows[i].minimized) { continue; }
            int32_t skip = (pass == 0) ? ((int32_t)i != wm_focused_index) :
                                          ((int32_t)i == wm_focused_index);
            if (skip) { continue; }
            int32_t wx = wm_windows[i].x;
            int32_t wy = wm_windows[i].y;
            int32_t ww = (int32_t)wm_windows[i].w;
            int32_t wh = (int32_t)wm_windows[i].h;
            if (mx >= wx && mx < wx + ww &&
                my >= wy && my < wy + wh) {
                return (int32_t)i;
            }
        }
    }
    return -1;
}

/* Called from mouse IRQ handler each time mouse state changes */
static uint8_t wm_handle_mouse(void)
{
    uint8_t scene_changed = 0;

    if (!wm_enabled) { return 0u; }
    /* Record mouse entry into WM handler */
    dbg_log_event(DBG_EV_MOUSE_CLICK, (int32_t)mouse_x, (int32_t)mouse_y);

    int32_t mx = (int32_t)mouse_x;
    int32_t my = (int32_t)mouse_y;
    uint8_t btn_left = mouse_buttons & 0x01u;
    uint8_t prev_left = wm_prev_buttons & 0x01u;

    /* Button just pressed */
    if (btn_left && !prev_left) {
        /* If Start menu is visible, intercept clicks into it first. */
        if (wm_start_menu_visible) {
            uint32_t tb_y = gui_framebuffer_height > 40u ? gui_framebuffer_height - 40u : 0u;
            uint32_t menu_w = wm_start_menu_width;
            uint32_t menu_h = wm_start_menu_height;
            uint32_t menu_x = 8u;
            uint32_t menu_y = (tb_y > menu_h) ? (tb_y - menu_h) : 0u;

            if (mx >= (int32_t)menu_x && mx < (int32_t)(menu_x + menu_w) &&
                my >= (int32_t)menu_y && my < (int32_t)(menu_y + menu_h)) {
                int32_t rel_y = my - (int32_t)menu_y;

                /* Terminal (first item) */
                if (rel_y >= 32 && rel_y < 52) {
                    if (!gui_framebuffer_detected) {
                        terminal_write("No framebuffer\n");
                    } else if (!gui_framebuffer_mapped && !gui_map_framebuffer()) {
                        terminal_write("Framebuffer map failed\n");
                    } else {
                            /* Defer actual window creation and process spawn to main loop */
                            gui_pending_action = GUI_ACTION_OPEN_TERMINAL;
                            gui_pending_action_arg[0] = '\0';
                            dbg_log_event(DBG_EV_GUI_PENDING_ACTION, GUI_ACTION_OPEN_TERMINAL, (int32_t)mx);
                    }
                    wm_start_menu_visible = 0;
                    wm_prev_buttons = mouse_buttons;
                    return 1;
                }

                /* Files (second item) */
                if (rel_y >= 52 && rel_y < 72) {
                    if (!gui_framebuffer_detected) {
                        terminal_write("No framebuffer\n");
                    } else if (!gui_framebuffer_mapped && !gui_map_framebuffer()) {
                        terminal_write("Framebuffer map failed\n");
                    } else {
                        /* Defer file-manager open so main loop can resolve paths safely */
                        const char* cwd = current_process_context()->cwd;
                        if (cwd) {
                            str_copy_bounded(gui_pending_action_arg, cwd, sizeof(gui_pending_action_arg));
                        } else {
                            gui_pending_action_arg[0] = '\0';
                        }
                        gui_pending_action = GUI_ACTION_OPEN_FILEMANAGER;
                        dbg_log_event(DBG_EV_GUI_PENDING_ACTION, GUI_ACTION_OPEN_FILEMANAGER, (int32_t)mx);
                    }
                    wm_start_menu_visible = 0;
                    wm_prev_buttons = mouse_buttons;
                    return 1;
                }

                /* Reboot (third item) - not implemented */
                if (rel_y >= 72 && rel_y < 92) {
                    terminal_write("Reboot selected (not implemented)\n");
                    wm_start_menu_visible = 0;
                    wm_prev_buttons = mouse_buttons;
                    return 1;
                }

                /* Click in menu but not on an item: close menu */
                wm_start_menu_visible = 0;
                wm_prev_buttons = mouse_buttons;
                return 1;
            } else {
                /* Click outside menu: close it and consume click */
                wm_start_menu_visible = 0;
                wm_prev_buttons = mouse_buttons;
                return 1;
            }
        }
        if (wm_drag_active == 0) {
            /* Taskbar click handling: focus/restore or minimize focused */
            uint32_t tb_y = gui_framebuffer_height > 40u ? gui_framebuffer_height - 40u : 0u;
            if (my >= (int32_t)tb_y && my < (int32_t)(tb_y + 40u)) {
                /* MBos start button (left side) */
                uint32_t mbos_x = 8u;
                uint32_t mbos_w = 64u;
                if (mx >= (int32_t)mbos_x && mx < (int32_t)(mbos_x + mbos_w)) {
                    wm_start_menu_visible = wm_start_menu_visible ? 0 : 1;
                    scene_changed = 1;
                } else {
                    /* If Start menu visible and click occurred outside it, hide it */
                    if (wm_start_menu_visible) {
                        uint32_t menu_w = wm_start_menu_width;
                        uint32_t menu_h = wm_start_menu_height;
                        uint32_t menu_x = mbos_x;
                        uint32_t menu_y = (tb_y > menu_h) ? (tb_y - menu_h) : 0u;
                        if (!(mx >= (int32_t)menu_x && mx < (int32_t)(menu_x + menu_w) &&
                              my >= (int32_t)menu_y && my < (int32_t)(menu_y + menu_h))) {
                            wm_start_menu_visible = 0;
                            scene_changed = 1;
                        }
                    }

                    /* Taskbar window buttons (positions packed left-to-right for used windows) */
                    uint32_t tbx = 72u;
                    for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++) {
                        if (!wm_windows[i].used) { continue; }
                        if ((int32_t)tbx <= mx && mx < (int32_t)(tbx + 120u)) {
                            if ((int32_t)i == wm_focused_index) {
                                /* Clicking the focused window -> minimize it */
                                wm_windows[i].minimized = 1;
                                wm_windows[i].focused = 0;
                                wm_focused_index = -1;
                                dbg_log_event(DBG_EV_WM_FOCUS, (int32_t)i, -1);
                            } else {
                                /* Clicking a different window -> restore and focus it */
                                if (wm_focused_index >= 0) {
                                    wm_windows[wm_focused_index].focused = 0;
                                }
                                wm_windows[i].minimized = 0;
                                wm_focused_index = (int32_t)i;
                                wm_windows[i].focused = 1;
                                dbg_log_event(DBG_EV_WM_FOCUS, (int32_t)i, wm_focused_index);
                            }
                            scene_changed = 1;
                            break;
                        }
                        tbx += 128u;
                    }
                }
            }

            int32_t hit = wm_hit_titlebar(mx, my);
            if (hit >= 0) {
                /* Check close-button click */
                int32_t wx = wm_windows[hit].x;
                int32_t wy = wm_windows[hit].y;
                int32_t ww = (int32_t)wm_windows[hit].w;
                uint32_t tb_h_win = (wm_windows[hit].h < WM_TITLE_HEIGHT) ? wm_windows[hit].h : WM_TITLE_HEIGHT;
                uint32_t bx = (uint32_t)(wx + ww - 16u);
                uint32_t by_btn = (uint32_t)(wy + (tb_h_win - 8u) / 2u);
                if (mx >= (int32_t)bx && mx < (int32_t)(bx + 8u) && my >= (int32_t)by_btn && my < (int32_t)(by_btn + 8u)) {
                    wm_close_window((uint32_t)hit);
                    scene_changed = 1;
                } else {
                    /* Focus + start drag */
                    if (wm_focused_index >= 0) {
                        wm_windows[wm_focused_index].focused = 0;
                    }
                    wm_focused_index = hit;
                    wm_windows[hit].focused = 1;
                    dbg_log_event(DBG_EV_WM_FOCUS, hit, wm_focused_index);
                    wm_drag_active = 1;
                    wm_drag_index  = hit;
                    wm_drag_off_x  = mx - wm_windows[hit].x;
                    wm_drag_off_y  = my - wm_windows[hit].y;
                    scene_changed = 1;
                }
            } else {
                int32_t any = wm_hit_any(mx, my);
                if (any >= 0 && any != wm_focused_index) {
                    if (wm_focused_index >= 0) {
                        wm_windows[wm_focused_index].focused = 0;
                    }
                    wm_focused_index = any;
                    wm_windows[any].focused = 1;
                    dbg_log_event(DBG_EV_WM_FOCUS, any, wm_focused_index);
                    scene_changed = 1;
                }
            }
        }
    }

    /* Drag in progress */
    if (btn_left && wm_drag_active && wm_drag_index >= 0) {
        int32_t new_x = mx - wm_drag_off_x;
        int32_t new_y = my - wm_drag_off_y;
        if (wm_windows[wm_drag_index].x != new_x || wm_windows[wm_drag_index].y != new_y) {
            wm_windows[wm_drag_index].x = new_x;
            wm_windows[wm_drag_index].y = new_y;
            scene_changed = 1;
        }
    }

    /* Button released */
    if (!btn_left && prev_left) {
        wm_drag_active = 0;
        wm_drag_index  = -1;
        scene_changed = 1;
    }

    wm_prev_buttons = mouse_buttons;
    return scene_changed;
}

/* -----------------------------------------------------------------------
 * End Window Manager
 * ----------------------------------------------------------------------- */

static void gui_draw_cursor(void)
{
    if (!gui_framebuffer_mapped) {
        return;
    }

    uint32_t x = mouse_x;
    uint32_t y = mouse_y;
    uint32_t color = 0xFFFF00;

    if (x < gui_framebuffer_width && y < gui_framebuffer_height) {
        gui_put_pixel(x, y, color);
    }
    if (x + 1 < gui_framebuffer_width && y < gui_framebuffer_height) {
        gui_put_pixel(x + 1, y, color);
    }
    if (x < gui_framebuffer_width && y + 1 < gui_framebuffer_height) {
        gui_put_pixel(x, y + 1, color);
    }
    if (x + 1 < gui_framebuffer_width && y + 1 < gui_framebuffer_height) {
        gui_put_pixel(x + 1, y + 1, color);
    }
}

static void gui_draw_test_scene(void)
{
    if (!gui_framebuffer_mapped) {
        return;
    }

    /* Desktop background */
    gui_fill_rect(0, 0, gui_framebuffer_width, gui_framebuffer_height, 0x001E2A3Au);

    /* Taskbar */
    gui_fill_rect(0, gui_framebuffer_height - 48u, gui_framebuffer_width, 48u, 0x00283445u);
    gui_draw_string(8u, gui_framebuffer_height - 34u, "MBos", 0xFFFFFFu, 0xFFFFFFFFu);
    gui_draw_string(gui_framebuffer_width - 72u, gui_framebuffer_height - 34u,
                    "00:00:00", 0xAABBCCu, 0xFFFFFFFFu);

    /* Window 1: body + title bar + label */
    gui_fill_rect(40, 50, 420, 260, 0x00374257u);
    gui_fill_rect(40, 50, 420, 26, 0x004F6078u);
    gui_draw_string(48u, 57u, "Terminal", 0xFFFFFFu, 0xFFFFFFFFu);

    /* Window 2: body + title bar + label */
    gui_fill_rect(520, 120, 300, 210, 0x00303A4Eu);
    gui_fill_rect(520, 120, 300, 24, 0x00495C78u);
    gui_draw_string(528u, 127u, "Files", 0xFFFFFFu, 0xFFFFFFFFu);

    /* Window 1 body: a few lines of dummy text */
    gui_draw_string(52u, 86u,  "MBos v0.1 kernel shell", 0xCCDDEEu, 0xFFFFFFFFu);
    gui_draw_string(52u, 100u, "> guitest", 0x88FF88u, 0xFFFFFFFFu);
    gui_draw_string(52u, 114u, "Test scene rendered.", 0xCCDDEEu, 0xFFFFFFFFu);
}

static void gui_print_status(void)
{
    terminal_write("GUI framebuffer detected: ");
    terminal_write(gui_framebuffer_detected ? "yes" : "no");
    terminal_write("\nMapped: ");
    terminal_write(gui_framebuffer_mapped ? "yes" : "no");
    terminal_write("\nFB phys: ");
    terminal_write_hex(gui_framebuffer_phys);
    terminal_write("\nFB virt: ");
    terminal_write_hex(gui_framebuffer_virt);
    terminal_write("\nSize bytes: ");
    terminal_write_hex(gui_framebuffer_size);
    terminal_write("\nResolution: ");
    terminal_write_dec_u32(gui_framebuffer_width);
    terminal_write("x");
    terminal_write_dec_u32(gui_framebuffer_height);
    terminal_write(" @ ");
    terminal_write_dec_u32(gui_framebuffer_bpp);
    terminal_write("bpp\n");
}

/* Dump basic WM state to serial for headless verification */
static void wm_dump_state(void)
{
    serial_write("WM STATE: count="); serial_write_dec_u32((uint32_t)wm_window_count); serial_write("\n");
    for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!wm_windows[i].used) { continue; }
        serial_write("WM WIN "); serial_write_dec_u32(i); serial_write(": title=");
        /* terminal_write will also mirror to serial */
        terminal_write(wm_windows[i].title);
        serial_write(", x="); serial_write_dec_u32((uint32_t)wm_windows[i].x);
        serial_write(", y="); serial_write_dec_u32((uint32_t)wm_windows[i].y);
        serial_write(", w="); serial_write_dec_u32(wm_windows[i].w);
        serial_write(", h="); serial_write_dec_u32(wm_windows[i].h);
        serial_write(", focused="); serial_write_dec_u32((uint32_t)wm_windows[i].focused);
        serial_write(", minimized="); serial_write_dec_u32((uint32_t)wm_windows[i].minimized);
        serial_write("\n");
    }
}

static uint64_t multiboot_total_memory_by_type(uint32_t type)
{
    uint64_t total = 0;

    if (!multiboot_has_memory_map()) {
        return 0;
    }

    uint32_t end = boot_multiboot_info->mmap_addr + boot_multiboot_info->mmap_length;
    uint32_t current = boot_multiboot_info->mmap_addr;

    while (current < end) {
        const struct multiboot_mmap_entry* entry = (const struct multiboot_mmap_entry*)(uintptr_t)current;
        if (entry->type == type) {
            total += entry->len;
        }
        current += entry->size + sizeof(entry->size);
    }

    return total;
}

static const char* multiboot_memory_type_name(uint32_t type)
{
    switch (type) {
    case 1:
        return "available";
    case 2:
        return "reserved";
    case 3:
        return "acpi reclaim";
    case 4:
        return "acpi nvs";
    case 5:
        return "bad";
    default:
        return "unknown";
    }
}

static void multiboot_print_memory_summary(void)
{
    if (!multiboot_has_memory_map()) {
        terminal_write("Multiboot memory map unavailable\n");
        return;
    }

    terminal_write("Lower memory KB: ");
    terminal_write_dec_u32(boot_multiboot_info->mem_lower);
    terminal_write("\nUpper memory KB: ");
    terminal_write_dec_u32(boot_multiboot_info->mem_upper);
    terminal_write("\nAvailable RAM bytes: ");
    terminal_write_hex_u64(multiboot_total_memory_by_type(1));
    terminal_write("\nReserved RAM bytes: ");
    terminal_write_hex_u64(multiboot_total_memory_by_type(2));
    terminal_write("\n");
}

static void multiboot_print_memory_map(void)
{
    if (!multiboot_has_memory_map()) {
        terminal_write("Multiboot memory map unavailable\n");
        return;
    }

    uint32_t end = boot_multiboot_info->mmap_addr + boot_multiboot_info->mmap_length;
    uint32_t current = boot_multiboot_info->mmap_addr;
    size_t index = 0;

    while (current < end) {
        const struct multiboot_mmap_entry* entry = (const struct multiboot_mmap_entry*)(uintptr_t)current;

        terminal_write("[");
        terminal_write_dec_u32((uint32_t)index);
        terminal_write("] base=");
        terminal_write_hex_u64(entry->addr);
        terminal_write(" len=");
        terminal_write_hex_u64(entry->len);
        terminal_write(" type=");
        terminal_write(multiboot_memory_type_name(entry->type));
        terminal_write("\n");

        index++;
        current += entry->size + sizeof(entry->size);
    }
}

static int physical_owner_find_index(uint32_t page)
{
    for (uint32_t i = 0; i < physical_owner_entry_count; i++) {
        if (physical_owner_entries[i].page == page) {
            return (int)i;
        }
    }
    return -1;
}

static uint8_t physical_owner_get(uint32_t page)
{
    int index = physical_owner_find_index(page);
    if (index < 0) {
        return PAGE_OWNER_FREE;
    }
    return physical_owner_entries[index].owner;
}

static const char* physical_owner_name(uint8_t owner)
{
    switch (owner) {
    case PAGE_OWNER_FREE:
        return "free";
    case PAGE_OWNER_ALLOCATOR:
        return "allocator";
    case PAGE_OWNER_PAGING_STRUCTURE:
        return "paging-struct";
    case PAGE_OWNER_KERNEL_HEAP:
        return "kernel-heap";
    case PAGE_OWNER_PAGING_TEST:
        return "paging-test";
    case PAGE_OWNER_USER_TASK:
        return "user-task";
    case PAGE_OWNER_USER_MISC:
        return "user-misc";
    default:
        return "unknown";
    }
}

static void physical_owner_set(uint32_t page, uint8_t owner)
{
    int index;

    if (page == 0 || (page & (PAGE_SIZE - 1u)) != 0) {
        return;
    }

    index = physical_owner_find_index(page);
    if (index >= 0) {
        physical_owner_entries[index].owner = owner;
        return;
    }

    if (physical_owner_entry_count < PHYSICAL_OWNER_TRACK_MAX) {
        physical_owner_entries[physical_owner_entry_count].page = page;
        physical_owner_entries[physical_owner_entry_count].owner = owner;
        physical_owner_entry_count++;
    } else {
        physical_owner_overflow_count++;
    }
}

static void physical_allocator_init(void)
{
    uint32_t kernel_end = align_up_u32((uint32_t)(uintptr_t)&__kernel_end, PAGE_SIZE);
    uint32_t minimum_addr = max_u32(0x00100000u, kernel_end);

    physical_region_count = 0;
    physical_region_cursor = 0;
    physical_total_pages = 0;
    physical_used_pages = 0;
    physical_highest_usable_end = 0;
    physical_recycled_count = 0;
    physical_recycle_drop_count = 0;
    physical_recycle_duplicate_count = 0;
    physical_recycle_invalid_count = 0;
    physical_recycle_protected_count = 0;
    physical_owner_entry_count = 0;
    physical_owner_overflow_count = 0;

    if (!multiboot_has_memory_map()) {
        return;
    }

    {
        uint32_t end = boot_multiboot_info->mmap_addr + boot_multiboot_info->mmap_length;
        uint32_t current = boot_multiboot_info->mmap_addr;

        while (current < end && physical_region_count < MAX_USABLE_REGIONS) {
            const struct multiboot_mmap_entry* entry = (const struct multiboot_mmap_entry*)(uintptr_t)current;

            if (entry->type == 1 && (entry->addr >> 32) == 0) {
                uint32_t region_start = (uint32_t)entry->addr;
                uint64_t entry_end_64 = entry->addr + entry->len;
                uint32_t region_end;

                if (entry_end_64 > 0xFFFFFFFFull) {
                    region_end = 0xFFFFFFFFu;
                } else {
                    region_end = (uint32_t)entry_end_64;
                }

                region_start = align_up_u32(max_u32(region_start, minimum_addr), PAGE_SIZE);
                region_end = align_down_u32(region_end, PAGE_SIZE);

                if (region_end > region_start) {
                    struct physical_region* region = &physical_regions[physical_region_count++];
                    region->start = region_start;
                    region->current = region_start;
                    region->end = region_end;
                    physical_total_pages += (region_end - region_start) / PAGE_SIZE;
                    if (region_end > physical_highest_usable_end) {
                        physical_highest_usable_end = region_end;
                    }
                }
            }

            current += entry->size + sizeof(entry->size);
        }
    }
}

static uint32_t physical_allocator_alloc_page(void)
{
    if (physical_recycled_count > 0) {
        uint32_t page;
        physical_used_pages++;
        physical_recycled_count--;
        page = physical_recycled_pages[physical_recycled_count];
        physical_owner_set(page, PAGE_OWNER_ALLOCATOR);
        return page;
    }

    while (physical_region_cursor < physical_region_count) {
        struct physical_region* region = &physical_regions[physical_region_cursor];

        if (region->current + PAGE_SIZE <= region->end) {
            uint32_t page = region->current;
            region->current += PAGE_SIZE;
            physical_used_pages++;
            physical_owner_set(page, PAGE_OWNER_ALLOCATOR);
            return page;
        }

        physical_region_cursor++;
    }

    return 0;
}

static void physical_allocator_free_page(uint32_t physical_page)
{
    uint8_t owner;

    if (physical_page == 0 || (physical_page & (PAGE_SIZE - 1u)) != 0) {
        physical_recycle_invalid_count++;
        return;
    }

    if (physical_page < 0x00100000u || physical_page >= physical_highest_usable_end) {
        physical_recycle_invalid_count++;
        return;
    }

    owner = physical_owner_get(physical_page);
    if (owner == PAGE_OWNER_PAGING_STRUCTURE) {
        physical_recycle_protected_count++;
        return;
    }

    if (owner == PAGE_OWNER_FREE) {
        physical_recycle_duplicate_count++;
        return;
    }

    for (uint32_t i = 0; i < physical_recycled_count; i++) {
        if (physical_recycled_pages[i] == physical_page) {
            physical_recycle_duplicate_count++;
            return;
        }
    }

    if (physical_used_pages > 0) {
        physical_used_pages--;
    }

    if (physical_recycled_count < PHYSICAL_RECYCLE_MAX) {
        physical_recycled_pages[physical_recycled_count++] = physical_page;
        physical_owner_set(physical_page, PAGE_OWNER_FREE);
    } else {
        physical_recycle_drop_count++;
    }
}

static void physical_allocator_set_owner(uint32_t physical_page, uint8_t owner)
{
    if (physical_page == 0 || (physical_page & (PAGE_SIZE - 1u)) != 0) {
        return;
    }
    physical_owner_set(physical_page, owner);
}

static void physical_allocator_print_stats(void)
{
    terminal_write("Allocator regions: ");
    terminal_write_dec_u32(physical_region_count);
    terminal_write("\nTotal pages: ");
    terminal_write_dec_u32(physical_total_pages);
    terminal_write("\nUsed pages: ");
    terminal_write_dec_u32(physical_used_pages);
    terminal_write("\nFree pages: ");
    terminal_write_dec_u32(physical_total_pages - physical_used_pages);
    terminal_write("\nRecycled pool: ");
    terminal_write_dec_u32(physical_recycled_count);
    terminal_write("\nRecycle drops: ");
    terminal_write_dec_u32(physical_recycle_drop_count);
    terminal_write("\nRecycle duplicates: ");
    terminal_write_dec_u32(physical_recycle_duplicate_count);
    terminal_write("\nRecycle invalid: ");
    terminal_write_dec_u32(physical_recycle_invalid_count);
    terminal_write("\nRecycle protected: ");
    terminal_write_dec_u32(physical_recycle_protected_count);
    terminal_write("\nOwner entries: ");
    terminal_write_dec_u32(physical_owner_entry_count);
    terminal_write("\nOwner overflows: ");
    terminal_write_dec_u32(physical_owner_overflow_count);
    terminal_write("\nKernel end: ");
    terminal_write_hex((uint32_t)(uintptr_t)&__kernel_end);

    if (physical_region_cursor < physical_region_count) {
        terminal_write("\nNext page: ");
        terminal_write_hex(physical_regions[physical_region_cursor].current);
    }

    terminal_write("\n");
}

/* Simple allocator self-test: allocate a small set of pages, map them
 * into the paging test area, write/read a pattern, then unmap and free.
 */
static void physical_allocator_selftest(void)
{
    terminal_write("Allocator self-test start\n");
    uint32_t pages[32];
    uint32_t count = 0;

    for (uint32_t i = 0; i < 32; i++) {
        uint32_t p = physical_allocator_alloc_page();
        if (p == 0) break;
        pages[count++] = p;
    }

    terminal_write("Allocated pages: ");
    terminal_write_dec_u32(count);
    terminal_write("\n");

    if (count == 0) {
        terminal_write("Allocator self-test: no pages available\n");
        return;
    }

    uint32_t base_va = paging_test_next_virt;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t va = base_va + i * PAGE_SIZE;
        if (!paging_map_page(va, pages[i], 0x3u)) {
            terminal_write("Allocator self-test: paging map failed\n");
            /* on failure, free remaining pages and abort */
            for (uint32_t j = i; j < count; j++) {
                physical_allocator_free_page(pages[j]);
            }
            return;
        }
        physical_allocator_set_owner(pages[i], PAGE_OWNER_PAGING_TEST);
        uint8_t* ptr = (uint8_t*)(uintptr_t)va;
        for (uint32_t b = 0; b < 16u; b++) {
            ptr[b] = (uint8_t)((i + b) & 0xFFu);
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t va = base_va + i * PAGE_SIZE;
        uint8_t* ptr = (uint8_t*)(uintptr_t)va;
        for (uint32_t b = 0; b < 16u; b++) {
            uint8_t expect = (uint8_t)((i + b) & 0xFFu);
            if (ptr[b] != expect) {
                terminal_write("Allocator self-test: verify failed at idx ");
                terminal_write_dec_u32(i);
                terminal_write("\n");
                goto cleanup;
            }
        }
    }

    terminal_write("Allocator self-test: memory verified\n");

cleanup:
    for (uint32_t i = 0; i < count; i++) {
        uint32_t va = base_va + i * PAGE_SIZE;
        (void)paging_unmap_page(va);
        physical_allocator_free_page(pages[i]);
    }

    paging_test_next_virt = base_va + count * PAGE_SIZE;
    terminal_write("Allocator self-test complete\n");
}

/* Larger allocator/paging stress test triggered from shell. Attempts to
 * allocate a larger batch of pages, map them, write/verify a pattern,
 * then unmap and free them while printing allocator statistics. */
static void physical_allocator_stress_test(void)
{
    terminal_write("Allocator stress-test start\n");
    physical_allocator_print_stats();

    uint32_t pages[512];
    uint32_t count = 0;

    for (uint32_t i = 0; i < 512; i++) {
        uint32_t p = physical_allocator_alloc_page();
        if (p == 0) break;
        pages[count++] = p;
    }

    terminal_write("Allocated pages: ");
    terminal_write_dec_u32(count);
    terminal_write("\n");

    if (count == 0) {
        terminal_write("Allocator stress-test: no pages available\n");
        return;
    }

    uint32_t base_va = paging_test_next_virt;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t va = base_va + i * PAGE_SIZE;
        if (!paging_map_page(va, pages[i], 0x3u)) {
            terminal_write("Allocator stress-test: paging map failed\n");
            /* free remaining pages */
            for (uint32_t j = i; j < count; j++) {
                physical_allocator_free_page(pages[j]);
            }
            return;
        }
        physical_allocator_set_owner(pages[i], PAGE_OWNER_PAGING_TEST);
        uint8_t* ptr = (uint8_t*)(uintptr_t)va;
        for (uint32_t b = 0; b < 64u; b++) {
            ptr[b] = (uint8_t)((i + b) & 0xFFu);
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t va = base_va + i * PAGE_SIZE;
        uint8_t* ptr = (uint8_t*)(uintptr_t)va;
        for (uint32_t b = 0; b < 64u; b++) {
            uint8_t expect = (uint8_t)((i + b) & 0xFFu);
            if (ptr[b] != expect) {
                terminal_write("Allocator stress-test: verify failed at idx ");
                terminal_write_dec_u32(i);
                terminal_write("\n");
                goto cleanup_stress;
            }
        }
    }

    terminal_write("Allocator stress-test: memory verified\n");

cleanup_stress:
    for (uint32_t i = 0; i < count; i++) {
        uint32_t va = base_va + i * PAGE_SIZE;
        (void)paging_unmap_page(va);
        physical_allocator_free_page(pages[i]);
    }

    paging_test_next_virt = base_va + count * PAGE_SIZE;
    terminal_write("Allocator stress-test complete\n");
    physical_allocator_print_stats();
}

static int __attribute__((unused)) paging_map_range(uint32_t virtual_base, uint32_t physical_base, uint32_t size_bytes, uint32_t flags)
{
    uint32_t mapped = 0;
    uint32_t length = align_up_u32(size_bytes, PAGE_SIZE);

    while (mapped < length) {
        if (!paging_map_page(virtual_base + mapped, physical_base + mapped, flags)) {
            return 0;
        }
        mapped += PAGE_SIZE;
    }

    return 1;
}

static void paging_init(void)
{
    uint32_t identity_limit;
    uint32_t* page_directory;
    uint32_t kernel_phys_base;
    uint32_t kernel_size;

    if (physical_region_count == 0) {
        return;
    }

    identity_limit = physical_highest_usable_end;
    if (identity_limit > PAGING_IDENTITY_CAP_BYTES) {
        identity_limit = PAGING_IDENTITY_CAP_BYTES;
    }

    identity_limit = align_up_u32(identity_limit, PAGE_SIZE);
    if (identity_limit < 0x00200000u) {
        identity_limit = 0x00200000u;
    }

    paging_directory_phys = physical_allocator_alloc_page();
    if (paging_directory_phys == 0) {
        return;
    }
    physical_allocator_set_owner(paging_directory_phys, PAGE_OWNER_PAGING_STRUCTURE);

    page_directory = (uint32_t*)(uintptr_t)paging_directory_phys;
    memory_set_u8(page_directory, 0, PAGE_SIZE);

    paging_table_count = 0;
    for (uint32_t base = 0; base < identity_limit; base += 0x400000u) {
        uint32_t page_table_phys = physical_allocator_alloc_page();
        uint32_t* page_table;

        if (page_table_phys == 0) {
            break;
        }

        page_table = (uint32_t*)(uintptr_t)page_table_phys;
        memory_set_u8(page_table, 0, PAGE_SIZE);
        physical_allocator_set_owner(page_table_phys, PAGE_OWNER_PAGING_STRUCTURE);

        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t physical_page = base + i * PAGE_SIZE;
            if (physical_page >= identity_limit) {
                break;
            }
            page_table[i] = physical_page | 0x3u;
        }

        page_directory[base >> 22] = page_table_phys | 0x3u;
        paging_table_count++;
    }

    paging_identity_limit = paging_table_count * 0x400000u;

    kernel_phys_base = align_down_u32((uint32_t)(uintptr_t)&__kernel_start, PAGE_SIZE);
    kernel_size = align_up_u32((uint32_t)(uintptr_t)&__kernel_end - kernel_phys_base, PAGE_SIZE);

    for (uint32_t offset = 0; offset < kernel_size; offset += PAGE_SIZE) {
        uint32_t virt_addr = KERNEL_VIRT_BASE + offset;
        uint32_t dir_index = virt_addr >> 22;
        uint32_t table_index = (virt_addr >> 12) & 0x3FFu;
        uint32_t* page_table;
        
        if ((page_directory[dir_index] & 0x1u) == 0) {
            uint32_t new_table_phys = physical_allocator_alloc_page();
            if (new_table_phys == 0) {
                return;
            }
            page_table = (uint32_t*)(uintptr_t)new_table_phys;
            memory_set_u8(page_table, 0, PAGE_SIZE);
            physical_allocator_set_owner(new_table_phys, PAGE_OWNER_PAGING_STRUCTURE);
            page_directory[dir_index] = new_table_phys | 0x3u;
            paging_table_count++;
        } else {
            page_table = (uint32_t*)(uintptr_t)(page_directory[dir_index] & 0xFFFFF000u);
        }
        
        page_table[table_index] = (kernel_phys_base + offset) | 0x3u;
    }

    uint32_t vga_dir_index = KERNEL_VGA_VIRT >> 22;
    uint32_t vga_table_offset = (KERNEL_VGA_VIRT >> 12) & 0x3FFu;
    uint32_t* vga_page_table;
    
    if ((page_directory[vga_dir_index] & 0x1u) == 0) {
        uint32_t vga_table_phys = physical_allocator_alloc_page();
        if (vga_table_phys == 0) {
            return;
        }
        vga_page_table = (uint32_t*)(uintptr_t)vga_table_phys;
        memory_set_u8(vga_page_table, 0, PAGE_SIZE);
        physical_allocator_set_owner(vga_table_phys, PAGE_OWNER_PAGING_STRUCTURE);
        page_directory[vga_dir_index] = vga_table_phys | 0x3u;
        paging_table_count++;
    } else {
        vga_page_table = (uint32_t*)(uintptr_t)(page_directory[vga_dir_index] & 0xFFFFF000u);
    }
    
    vga_page_table[vga_table_offset] = 0x000B8000u | 0x3u;

    uint32_t alias_dir_index = PAGING_TABLE_ALIAS_BASE >> 22;
    uint32_t* alias_page_table;
    uint32_t alias_table_phys_addr;
    
    if ((page_directory[alias_dir_index] & 0x1u) == 0) {
        alias_table_phys_addr = physical_allocator_alloc_page();
        if (alias_table_phys_addr == 0) {
            return;
        }
        alias_page_table = (uint32_t*)(uintptr_t)alias_table_phys_addr;
        memory_set_u8(alias_page_table, 0, PAGE_SIZE);
        physical_allocator_set_owner(alias_table_phys_addr, PAGE_OWNER_PAGING_STRUCTURE);
        page_directory[alias_dir_index] = alias_table_phys_addr | 0x3u;
        paging_table_count++;
    } else {
        alias_table_phys_addr = page_directory[alias_dir_index] & 0xFFFFF000u;
        alias_page_table = (uint32_t*)(uintptr_t)alias_table_phys_addr;
    }

    for (uint32_t dir_index = 0; dir_index < paging_table_count; dir_index++) {
        uint32_t table_phys = page_directory[dir_index] & 0xFFFFF000u;
        if (table_phys != 0) {
            uint32_t alias_offset = dir_index;
            alias_page_table[alias_offset] = table_phys | 0x3u;
        }
    }

    uint32_t dir_virt_dir_index = PAGING_DIRECTORY_VIRT >> 22;
    uint32_t* dir_virt_page_table;
    
    if (dir_virt_dir_index != alias_dir_index) {
        uint32_t dir_virt_table_phys = physical_allocator_alloc_page();
        if (dir_virt_table_phys == 0) {
            return;
        }
        dir_virt_page_table = (uint32_t*)(uintptr_t)dir_virt_table_phys;
        memory_set_u8(dir_virt_page_table, 0, PAGE_SIZE);
        physical_allocator_set_owner(dir_virt_table_phys, PAGE_OWNER_PAGING_STRUCTURE);
        page_directory[dir_virt_dir_index] = dir_virt_table_phys | 0x3u;
        paging_table_count++;
        
        uint32_t dir_virt_table_offset = (PAGING_DIRECTORY_VIRT >> 12) & 0x3FFu;
        dir_virt_page_table[dir_virt_table_offset] = paging_directory_phys | 0x3u;
    } else {
        uint32_t dir_virt_table_offset = (PAGING_DIRECTORY_VIRT >> 12) & 0x3FFu;
        alias_page_table[dir_virt_table_offset] = paging_directory_phys | 0x3u;
    }

    kernel_virtual_base = KERNEL_VIRT_BASE;
    kernel_virtual_end = KERNEL_VIRT_BASE + kernel_size;

    load_page_directory(paging_directory_phys);
    enable_paging_hw();
    paging_enabled = 1;
    paging_aliases_ready = 1;

    terminal_buffer = (uint16_t*)(uintptr_t)KERNEL_VGA_VIRT;
    terminal_using_high_vga = 1;
}

static uint32_t* paging_get_directory(void)
{
    if (paging_directory_phys == 0) {
        return 0;
    }

    if (paging_enabled && paging_aliases_ready) {
        return (uint32_t*)(uintptr_t)PAGING_DIRECTORY_VIRT;
    }

    return (uint32_t*)(uintptr_t)paging_directory_phys;
}

static uint32_t* paging_get_table(uint32_t virtual_address, uint8_t create)
{
    uint32_t* page_directory = paging_get_directory();
    uint32_t directory_index = virtual_address >> 22;
    uint32_t entry;

    if (page_directory == 0) {
        return 0;
    }

    entry = page_directory[directory_index];
    if ((entry & 0x1u) == 0) {
        uint32_t page_table_phys;
        uint32_t* page_table;

        if (!create) {
            return 0;
        }

        page_table_phys = physical_allocator_alloc_page();
        if (page_table_phys == 0) {
            return 0;
        }

        physical_allocator_set_owner(page_table_phys, PAGE_OWNER_PAGING_STRUCTURE);
        page_directory[directory_index] = page_table_phys | 0x3u;
        paging_table_count++;
        
        if (paging_enabled) {
            load_page_directory(paging_directory_phys);
            
            if (paging_aliases_ready) {
                uint32_t alias_dir_index = PAGING_TABLE_ALIAS_BASE >> 22;
                uint32_t alias_table_phys = page_directory[alias_dir_index] & 0xFFFFF000u;
                uint32_t* alias_table = (uint32_t*)(uintptr_t)alias_table_phys;
                alias_table[directory_index] = page_table_phys | 0x3u;
            }
        }
        
        page_table = (uint32_t*)(uintptr_t)page_table_phys;
        memory_set_u8(page_table, 0, PAGE_SIZE);
        return page_table;
    }

    if (paging_enabled && paging_aliases_ready) {
        return (uint32_t*)(uintptr_t)paging_alias_virtual_for_directory(directory_index);
    }

    return (uint32_t*)(uintptr_t)(entry & 0xFFFFF000u);
}

static int paging_map_page(uint32_t virtual_address, uint32_t physical_address, uint32_t flags)
{
    uint32_t* page_table;
    uint32_t table_index;

    virtual_address = align_down_u32(virtual_address, PAGE_SIZE);
    physical_address = align_down_u32(physical_address, PAGE_SIZE);

    if ((flags & 0x4u) != 0 && !paging_is_user_virtual(virtual_address)) {
        return 0;
    }

    page_table = paging_get_table(virtual_address, 1);
    if (page_table == 0) {
        return 0;
    }

    table_index = (virtual_address >> 12) & 0x3FFu;
    page_table[table_index] = physical_address | (flags & 0xFFFu) | 0x1u;
    invalidate_page(virtual_address);
    return 1;
}

static int paging_unmap_page(uint32_t virtual_address)
{
    uint32_t* page_directory;
    uint32_t* page_table;
    uint32_t directory_index;
    uint32_t table_index;
    uint32_t table_phys;
    uint8_t table_empty = 1;

    virtual_address = align_down_u32(virtual_address, PAGE_SIZE);
    directory_index = virtual_address >> 22;
    page_directory = paging_get_directory();
    if (page_directory == 0) {
        return 0;
    }

    table_phys = page_directory[directory_index] & 0xFFFFF000u;
    page_table = paging_get_table(virtual_address, 0);
    if (page_table == 0) {
        return 0;
    }

    table_index = (virtual_address >> 12) & 0x3FFu;
    if ((page_table[table_index] & 0x1u) == 0) {
        return 0;
    }

    page_table[table_index] = 0;
    invalidate_page(virtual_address);

    for (uint32_t i = 0; i < 1024; i++) {
        if ((page_table[i] & 0x1u) != 0) {
            table_empty = 0;
            break;
        }
    }

    if (table_empty && table_phys != 0 && physical_owner_get(table_phys) == PAGE_OWNER_PAGING_STRUCTURE) {
        page_directory[directory_index] = 0;
        load_page_directory(paging_directory_phys);
        physical_allocator_set_owner(table_phys, PAGE_OWNER_ALLOCATOR);
        physical_allocator_free_page(table_phys);
        if (paging_table_count > 0) {
            paging_table_count--;
        }
    }

    return 1;
}

static int paging_map_new_page(uint32_t virtual_address, uint32_t flags, uint8_t owner, uint32_t* physical_out)
{
    uint32_t physical_page = physical_allocator_alloc_page();

    if (physical_page == 0) {
        return 0;
    }

    if (!paging_map_page(virtual_address, physical_page, flags)) {
        physical_allocator_free_page(physical_page);
        return 0;
    }

    physical_allocator_set_owner(physical_page, owner);

    if (physical_out != 0) {
        *physical_out = physical_page;
    }

    return 1;
}

static int paging_lookup(uint32_t virtual_address, uint32_t* physical_out, uint32_t* entry_out)
{
    uint32_t* page_table = paging_get_table(virtual_address, 0);
    uint32_t entry;

    if (page_table == 0) {
        return 0;
    }

    entry = page_table[(virtual_address >> 12) & 0x3FFu];
    if ((entry & 0x1u) == 0) {
        return 0;
    }

    if (physical_out != 0) {
        *physical_out = (entry & 0xFFFFF000u) | (virtual_address & 0xFFFu);
    }

    if (entry_out != 0) {
        *entry_out = entry;
    }

    return 1;
}

static void paging_print_stats(void)
{
    terminal_write("Paging enabled: ");
    terminal_write(paging_enabled ? "yes" : "no");
    terminal_write("\nPage directory: ");
    terminal_write_hex(paging_directory_phys);
    terminal_write("\nIdentity map limit: ");
    terminal_write_hex(paging_identity_limit);
    terminal_write("\nIdentity min keep: ");
    terminal_write_hex(PAGING_MIN_KEEP_BYTES);
    terminal_write("\nKernel virt base: ");
    terminal_write_hex(kernel_virtual_base);
    terminal_write("\nKernel virt end: ");
    terminal_write_hex(kernel_virtual_end);
    terminal_write("\nPage tables: ");
    terminal_write_dec_u32(paging_table_count);
    terminal_write("\n");
}

static void paging_print_identity_map(void)
{
    terminal_write("Identity mapped bytes: ");
    terminal_write_hex(paging_identity_limit);
    terminal_write("\nTrim floor: ");
    terminal_write_hex(PAGING_MIN_KEEP_BYTES);
    terminal_write("\nIdentity PDEs present:\n");

    {
        const uint32_t* page_directory = paging_get_directory();
        uint32_t max_index = paging_identity_limit >> 22;

        for (uint32_t i = 0; i < max_index; i++) {
            if ((page_directory[i] & 0x1u) != 0) {
                terminal_write("  PDE[");
                terminal_write_dec_u32(i);
                terminal_write("] -> ");
                terminal_write_hex(page_directory[i] & 0xFFFFF000u);
                terminal_write("\n");
            }
        }
    }
}

static void paging_trim_identity(uint32_t keep_bytes)
{
    uint32_t keep_aligned;
    uint32_t* page_directory;

    if (!paging_enabled) {
        terminal_write("Paging not initialized\n");
        return;
    }

    keep_aligned = align_up_u32(max_u32(keep_bytes, PAGING_MIN_KEEP_BYTES), 0x400000u);
    if (keep_aligned >= paging_identity_limit) {
        terminal_write("Identity map already within requested limit\n");
        return;
    }

    page_directory = paging_get_directory();
    for (uint32_t i = keep_aligned >> 22; i < (paging_identity_limit >> 22); i++) {
        page_directory[i] = 0;
    }

    paging_identity_limit = keep_aligned;
    load_page_directory(paging_directory_phys);

    terminal_write("Identity map trimmed to ");
    terminal_write_hex(paging_identity_limit);
    terminal_write(" bytes\n");
}

static void paging_print_directory(void)
{
    if (!paging_enabled || paging_directory_phys == 0) {
        terminal_write("Paging not initialized\n");
        return;
    }

    {
        const uint32_t* page_directory = paging_get_directory();
        for (uint32_t i = 0; i < 16; i++) {
            if ((page_directory[i] & 0x1u) != 0) {
                terminal_write("PDE[");
                terminal_write_dec_u32(i);
                terminal_write("] = ");
                terminal_write_hex(page_directory[i]);
                terminal_write("\n");
            }
        }
    }
}

static void paging_print_user_layout(void)
{
    terminal_write("User virtual range: ");
    terminal_write_hex(USER_SPACE_BASE);
    terminal_write(" - ");
    terminal_write_hex(USER_SPACE_LIMIT);
    terminal_write("\nUser test base: ");
    terminal_write_hex(USER_TEST_BASE);
    terminal_write("\nNext user test VA: ");
    terminal_write_hex(paging_user_test_next_virt);
    terminal_write("\n");
}

static void paging_map_user_test_page(void)
{
    uint32_t physical_page;

    if (!paging_map_new_page(paging_user_test_next_virt, 0x7u, PAGE_OWNER_USER_MISC, &physical_page)) {
        terminal_write("User map test: map failed\n");
        return;
    }

    paging_last_user_virt = paging_user_test_next_virt;
    paging_last_user_phys = physical_page;

    terminal_write("Mapped user page VA ");
    terminal_write_hex(paging_last_user_virt);
    terminal_write(" -> PA ");
    terminal_write_hex(paging_last_user_phys);
    terminal_write("\n");

    paging_user_test_next_virt += PAGE_SIZE;
}

static void user_test_prepare(void)
{
    struct process_syscall_context* proc = current_process_context();
    uint32_t code_page_phys;
    uint32_t stack_page_phys;
    uint8_t* code;

    if (proc->user_ready) {
        terminal_write("User test context already prepared for PID ");
        terminal_write_dec_u32(proc->pid);
        terminal_write("\n");
        return;
    }

    process_init_user_runtime_layout(proc);
    process_unmap_user_pages(proc);

    if (!process_map_user_page(proc, proc->user_code_va, 0x7u, PAGE_OWNER_USER_TASK, &code_page_phys)) {
        terminal_write("User test: failed to map code page\n");
        return;
    }

    if (!process_map_user_page(proc, proc->user_stack_top - PAGE_SIZE, 0x7u, PAGE_OWNER_USER_TASK, &stack_page_phys)) {
        terminal_write("User test: failed to map stack page\n");
        return;
    }

    code = (uint8_t*)(uintptr_t)proc->user_code_va;
    code[0] = 0xB8;  /* mov eax, 2 */
    code[1] = 0x02;
    code[2] = 0x00;
    code[3] = 0x00;
    code[4] = 0x00;
    code[5] = 0xBB;  /* mov ebx, 'P' + (pid-1) */
    code[6] = (uint8_t)('P' + ((proc->pid - 1u) % 10u));
    code[7] = 0x00;
    code[8] = 0x00;
    code[9] = 0x00;
    code[10] = 0xCD; /* int 0x80 */
    code[11] = 0x80;

    code[12] = 0xB8; /* mov eax, 2 */
    code[13] = 0x02;
    code[14] = 0x00;
    code[15] = 0x00;
    code[16] = 0x00;
    code[17] = 0xBB; /* mov ebx, '!' */
    code[18] = 0x21;
    code[19] = 0x00;
    code[20] = 0x00;
    code[21] = 0x00;
    code[22] = 0xCD; /* int 0x80 */
    code[23] = 0x80;

    code[24] = 0xB8; /* mov eax, 3 */
    code[25] = 0x03;
    code[26] = 0x00;
    code[27] = 0x00;
    code[28] = 0x00;
    code[29] = 0xCD; /* int 0x80 */
    code[30] = 0x80;

    code[31] = 0xB8; /* mov eax, 4 */
    code[32] = 0x04;
    code[33] = 0x00;
    code[34] = 0x00;
    code[35] = 0x00;
    code[36] = 0xBB; /* mov ebx, pid */
    code[37] = (uint8_t)(proc->pid & 0xFFu);
    code[38] = 0x00;
    code[39] = 0x00;
    code[40] = 0x00;
    code[41] = 0xCD; /* int 0x80 */
    code[42] = 0x80;

    code[43] = 0xEB; /* jmp $ */
    code[44] = 0xFE;

    proc->user_eip = proc->user_code_va;
    if (!process_build_initial_stack(proc, "usertest")) {
        process_unmap_user_pages(proc);
        terminal_write("User test: failed to build initial stack\n");
        return;
    }
    proc->user_ready = 1;

    terminal_write("User test prepared for PID ");
    terminal_write_dec_u32(proc->pid);
    terminal_write("\nCode VA ");
    terminal_write_hex(proc->user_code_va);
    terminal_write(" (PA ");
    terminal_write_hex(code_page_phys);
    terminal_write(")\nStack VA ");
    terminal_write_hex(proc->user_stack_top - PAGE_SIZE);
    terminal_write(" (PA ");
    terminal_write_hex(stack_page_phys);
    terminal_write(")\n");
}

static void user_test_prepare_pid(uint32_t pid)
{
    uint32_t saved_slot;

    if (pid == 0 || pid > PROCESS_CONTEXT_MAX) {
        terminal_write("Usage: taskspawn <1-");
        terminal_write_dec_u32(PROCESS_CONTEXT_MAX);
        terminal_write(">\n");
        return;
    }

    saved_slot = current_process_slot;
    current_process_slot = pid - 1u;
    user_test_prepare();
    current_process_slot = saved_slot;
}

static void task_teardown_pid(uint32_t pid)
{
    struct process_syscall_context* proc;

    if (pid == 0 || pid > PROCESS_CONTEXT_MAX) {
        terminal_write("Usage: taskteardown <1-");
        terminal_write_dec_u32(PROCESS_CONTEXT_MAX);
        terminal_write(">\n");
        return;
    }

    proc = &process_contexts[pid - 1u];
    process_unmap_user_pages(proc);

    proc->user_ready = 0;
    proc->user_eip = 0;
    proc->user_esp = 0;
    process_close_all_fds(proc);
    proc->snapshot_valid = 0;
    proc->snapshot.captures = 0;
    proc->sleep_until_tick = 0;
    proc->wait_timeout_tick = 0;
    proc->wait_event_channel = 0;
    proc->wait_reason = WAIT_REASON_NONE;
    process_set_task_state(proc, TASK_STATE_EXITED);

    if (current_process_slot == pid - 1u) {
        (void)scheduler_rotate_process();
    }

    terminal_write("Torn down PID ");
    terminal_write_dec_u32(pid);
    terminal_write(" user context (pages unmapped, task stopped)\n");
}

static void task_print_page_ownership(uint32_t pid)
{
    struct process_syscall_context* proc;
    uint32_t code_page;
    uint32_t stack_page;
    uint32_t code_phys = 0;
    uint32_t stack_phys = 0;
    uint8_t code_owner;
    uint8_t stack_owner;

    if (pid == 0 || pid > PROCESS_CONTEXT_MAX) {
        terminal_write("Usage: pageown <1-");
        terminal_write_dec_u32(PROCESS_CONTEXT_MAX);
        terminal_write(">\n");
        return;
    }

    proc = &process_contexts[pid - 1u];
    code_page = proc->user_code_va;
    stack_page = proc->user_stack_top - PAGE_SIZE;
    (void)paging_lookup(code_page, &code_phys, 0);
    (void)paging_lookup(stack_page, &stack_phys, 0);

    code_owner = physical_owner_get(code_phys & 0xFFFFF000u);
    stack_owner = physical_owner_get(stack_phys & 0xFFFFF000u);

    terminal_write("PID ");
    terminal_write_dec_u32(pid);
    terminal_write(" ownership:\n");
    terminal_write("  code VA ");
    terminal_write_hex(code_page);
    terminal_write(" -> PA ");
    terminal_write_hex(code_phys & 0xFFFFF000u);
    terminal_write(" owner=");
    terminal_write(physical_owner_name(code_owner));
    terminal_write("\n");
    terminal_write("  stack VA ");
    terminal_write_hex(stack_page);
    terminal_write(" -> PA ");
    terminal_write_hex(stack_phys & 0xFFFFF000u);
    terminal_write(" owner=");
    terminal_write(physical_owner_name(stack_owner));
    terminal_write("\n");
}

static void user_test_print_context(void)
{
    struct process_syscall_context* proc = current_process_context();
    terminal_write("PID: ");
    terminal_write_dec_u32(proc->pid);
    terminal_write("\n");
    terminal_write("User context ready: ");
    terminal_write(proc->user_ready ? "yes" : "no");
    terminal_write("\nUser CS selector: 0x0000001B\nUser DS selector: 0x00000023\nEIP: ");
    terminal_write_hex(proc->user_eip);
    terminal_write("\nESP: ");
    terminal_write_hex(proc->user_esp);
    terminal_write("\nCode VA: ");
    terminal_write_hex(proc->user_code_va);
    terminal_write("\nStack top: ");
    terminal_write_hex(proc->user_stack_top);
    terminal_write("\nHeap base: ");
    terminal_write_hex(proc->user_heap_base);
    terminal_write("\nHeap brk: ");
    terminal_write_hex(proc->user_heap_brk);
    terminal_write("\nHeap limit: ");
    terminal_write_hex(proc->user_heap_limit);
    terminal_write("\nRun command: userrun");
    terminal_write("\n");
}

static void task_print_state_counts(void)
{
    uint32_t counts[5] = {0, 0, 0, 0, 0};
    uint32_t blocked_sleep = 0;
    uint32_t blocked_manual = 0;
    uint32_t blocked_event = 0;

    for (uint32_t i = 0; i < PROCESS_CONTEXT_MAX; i++) {
        uint8_t state = process_contexts[i].task_state;
        if (state <= TASK_STATE_EXITED) {
            counts[state]++;
        }

        if (state == TASK_STATE_BLOCKED) {
            if (process_contexts[i].wait_reason == WAIT_REASON_SLEEP) {
                blocked_sleep++;
            } else if (process_contexts[i].wait_reason == WAIT_REASON_MANUAL) {
                blocked_manual++;
            } else if (process_contexts[i].wait_reason == WAIT_REASON_EVENT) {
                blocked_event++;
            }
        }
    }

    terminal_write("Task states: new=");
    terminal_write_dec_u32(counts[TASK_STATE_NEW]);
    terminal_write(" ready=");
    terminal_write_dec_u32(counts[TASK_STATE_READY]);
    terminal_write(" running=");
    terminal_write_dec_u32(counts[TASK_STATE_RUNNING]);
    terminal_write(" blocked=");
    terminal_write_dec_u32(counts[TASK_STATE_BLOCKED]);
    terminal_write(" exited=");
    terminal_write_dec_u32(counts[TASK_STATE_EXITED]);
    terminal_write("\nBlocked queues: sleep=");
    terminal_write_dec_u32(blocked_sleep);
    terminal_write(" manual=");
    terminal_write_dec_u32(blocked_manual);
    terminal_write(" event=");
    terminal_write_dec_u32(blocked_event);
    terminal_write("\n");
}

static void task_print_wait_queues(void)
{
    terminal_write("Sleep queue:\n");
    for (uint32_t i = 0; i < PROCESS_CONTEXT_MAX; i++) {
        struct process_syscall_context* proc = &process_contexts[i];
        if (proc->task_state == TASK_STATE_BLOCKED && proc->wait_reason == WAIT_REASON_SLEEP) {
            terminal_write("  PID ");
            terminal_write_dec_u32(proc->pid);
            terminal_write(" until tick ");
            terminal_write_dec_u32(proc->sleep_until_tick);
            terminal_write("\n");
        }
    }

    terminal_write("Manual queue:\n");
    for (uint32_t i = 0; i < PROCESS_CONTEXT_MAX; i++) {
        struct process_syscall_context* proc = &process_contexts[i];
        if (proc->task_state == TASK_STATE_BLOCKED && proc->wait_reason == WAIT_REASON_MANUAL) {
            terminal_write("  PID ");
            terminal_write_dec_u32(proc->pid);
            if (proc->wait_timeout_tick != 0) {
                terminal_write(" timeout ");
                terminal_write_dec_u32(proc->wait_timeout_tick);
            }
            terminal_write("\n");
        }
    }

    terminal_write("Event queue:\n");
    for (uint32_t i = 0; i < PROCESS_CONTEXT_MAX; i++) {
        struct process_syscall_context* proc = &process_contexts[i];
        if (proc->task_state == TASK_STATE_BLOCKED && proc->wait_reason == WAIT_REASON_EVENT) {
            terminal_write("  PID ");
            terminal_write_dec_u32(proc->pid);
            terminal_write(" channel ");
            terminal_write_dec_u32(proc->wait_event_channel);
            terminal_write("\n");
        }
    }
}

static uint32_t scheduler_signal_event_channel(uint32_t channel)
{
    uint32_t woke = 0;

    for (uint32_t i = 0; i < PROCESS_CONTEXT_MAX; i++) {
        struct process_syscall_context* proc = &process_contexts[i];
        if (proc->task_state == TASK_STATE_BLOCKED && proc->wait_reason == WAIT_REASON_EVENT &&
            proc->wait_event_channel == channel) {
            proc->wait_event_channel = 0;
            process_set_task_state(proc, TASK_STATE_READY);
            woke++;
        }
    }

    return woke;
}

static void input_bridge_signal(uint8_t scancode, uint8_t ascii)
{
    uint8_t woke = 0;

    if (!input_event_bridge_enabled || input_event_channel == 0) {
        return;
    }

    input_event_count++;
    input_last_scancode = scancode;
    input_last_ascii = ascii;
    woke = (uint8_t)scheduler_signal_event_channel(input_event_channel);
    input_last_woke = woke;

    input_event_log[input_event_log_head].tick = timer_ticks;
    input_event_log[input_event_log_head].scancode = scancode;
    input_event_log[input_event_log_head].ascii = ascii;
    input_event_log[input_event_log_head].woke = woke;
    input_event_log_head = (input_event_log_head + 1u) % INPUT_EVENT_LOG_MAX;
    if (input_event_log_count < INPUT_EVENT_LOG_MAX) {
        input_event_log_count++;
    }
}

static void input_bridge_print_status(void)
{
    terminal_write("Input bridge: ");
    terminal_write(input_event_bridge_enabled ? "on" : "off");
    terminal_write("\nEvent channel: ");
    terminal_write_dec_u32(input_event_channel);
    terminal_write("\nEvents signaled: ");
    terminal_write_dec_u32(input_event_count);
    terminal_write("\nLast scancode: ");
    terminal_write_hex(input_last_scancode);
    terminal_write("\nLast ascii: ");
    terminal_write_hex(input_last_ascii);
    terminal_write("\nLast wake count: ");
    terminal_write_dec_u32(input_last_woke);
    terminal_write("\n");
}

static void input_bridge_print_log(void)
{
    terminal_write("Input event log (newest first):\n");

    for (uint32_t i = 0; i < input_event_log_count; i++) {
        uint32_t idx = (input_event_log_head + INPUT_EVENT_LOG_MAX - 1u - i) % INPUT_EVENT_LOG_MAX;
        terminal_write("  [");
        terminal_write_dec_u32(i);
        terminal_write("] tick=");
        terminal_write_dec_u32(input_event_log[idx].tick);
        terminal_write(" sc=");
        terminal_write_hex(input_event_log[idx].scancode);
        terminal_write(" asc=");
        terminal_write_hex(input_event_log[idx].ascii);
        terminal_write(" woke=");
        terminal_write_dec_u32(input_event_log[idx].woke);
        terminal_write("\n");
    }
}

static void paging_print_lookup(uint32_t virtual_address)
{
    uint32_t physical_address;
    uint32_t entry;

    terminal_write("Lookup VA ");
    terminal_write_hex(virtual_address);

    if (paging_lookup(virtual_address, &physical_address, &entry)) {
        terminal_write(" -> PA ");
        terminal_write_hex(physical_address);
        terminal_write(" entry=");
        terminal_write_hex(entry);
        terminal_write("\n");
    } else {
        terminal_write(" -> unmapped\n");
    }
}

static void paging_print_virtual_layout(void)
{
    terminal_write("Kernel physical start: ");
    terminal_write_hex((uint32_t)(uintptr_t)&__kernel_start);
    terminal_write("\nKernel physical end: ");
    terminal_write_hex((uint32_t)(uintptr_t)&__kernel_end);
    terminal_write("\nKernel virtual alias: ");
    terminal_write_hex(kernel_virtual_base);
    terminal_write(" - ");
    terminal_write_hex(kernel_virtual_end);
    terminal_write("\nVGA virtual alias: ");
    terminal_write_hex(kernel_vga_virtual);
    terminal_write(" (");
    terminal_write(terminal_using_high_vga ? "active" : "inactive");
    terminal_write(")");
    terminal_write("\nHeap virtual base: ");
    terminal_write_hex(kernel_heap_start);
    terminal_write("\nShell input buffer: ");
    terminal_write_hex((uint32_t)(uintptr_t)shell_input);
    terminal_write(" (");
    terminal_write(shell_input_heap_backed ? "heap" : "boot");
    terminal_write(")");
    terminal_write("\nShell history buffer: ");
    terminal_write_hex((uint32_t)(uintptr_t)shell_history);
    terminal_write(" (");
    terminal_write(shell_history_heap_backed ? "heap" : "boot");
    terminal_write(")");
    terminal_write("\nPaging test base: ");
    terminal_write_hex(PAGING_TEST_BASE);
    terminal_write("\nUser test base: ");
    terminal_write_hex(USER_TEST_BASE);
    terminal_write("\n");
}

static void paging_map_test_page(void)
{
    uint32_t physical_page;

    if (!paging_map_new_page(paging_test_next_virt, 0x3u, PAGE_OWNER_PAGING_TEST, &physical_page)) {
        terminal_write("Paging test: map failed\n");
        return;
    }

    paging_last_mapped_virt = paging_test_next_virt;
    paging_last_mapped_phys = physical_page;

    terminal_write("Mapped test page VA ");
    terminal_write_hex(paging_last_mapped_virt);
    terminal_write(" -> PA ");
    terminal_write_hex(paging_last_mapped_phys);
    terminal_write("\n");

    paging_test_next_virt += PAGE_SIZE;
}

static int kernel_heap_grow(uint32_t bytes)
{
    uint32_t target_end = align_up_u32(kernel_heap_end + bytes, PAGE_SIZE);

    while (kernel_heap_end < target_end) {
        if (!paging_map_new_page(kernel_heap_end, 0x3u, PAGE_OWNER_KERNEL_HEAP, 0)) {
            return 0;
        }
        kernel_heap_end += PAGE_SIZE;
    }

    return 1;
}

static void kernel_heap_init(void)
{
    kernel_heap_start = KERNEL_HEAP_BASE;
    kernel_heap_end = KERNEL_HEAP_BASE;
    kernel_heap_current = KERNEL_HEAP_BASE;
    kernel_heap_ready = 0;
    kernel_heap_last_alloc = 0;
    kernel_heap_alloc_count = 0;

    if (kernel_heap_grow(KERNEL_HEAP_INITIAL_BYTES)) {
        kernel_heap_ready = 1;
    }
}

static void* kmalloc_aligned(uint32_t size, uint32_t alignment)
{
    uint32_t alloc_start;
    uint32_t alloc_end;

    if (!kernel_heap_ready || size == 0) {
        return 0;
    }

    if (alignment == 0) {
        alignment = 1;
    }

    alloc_start = align_up_u32(kernel_heap_current, alignment);
    alloc_end = alloc_start + size;

    if (alloc_end < alloc_start) {
        return 0;
    }

    if (alloc_end > kernel_heap_end) {
        if (!kernel_heap_grow(alloc_end - kernel_heap_end)) {
            return 0;
        }
    }

    kernel_heap_current = alloc_end;
    kernel_heap_last_alloc = alloc_start;
    kernel_heap_alloc_count++;
    return (void*)(uintptr_t)alloc_start;
}

static void kernel_heap_print_stats(void)
{
    terminal_write("Heap ready: ");
    terminal_write(kernel_heap_ready ? "yes" : "no");
    terminal_write("\nHeap start: ");
    terminal_write_hex(kernel_heap_start);
    terminal_write("\nHeap current: ");
    terminal_write_hex(kernel_heap_current);
    terminal_write("\nHeap end: ");
    terminal_write_hex(kernel_heap_end);
    terminal_write("\nHeap used bytes: ");
    terminal_write_hex(kernel_heap_current - kernel_heap_start);
    terminal_write("\nHeap mapped bytes: ");
    terminal_write_hex(kernel_heap_end - kernel_heap_start);
    terminal_write("\nHeap alloc count: ");
    terminal_write_dec_u32(kernel_heap_alloc_count);
    if (kernel_heap_last_alloc != 0) {
        terminal_write("\nLast alloc: ");
        terminal_write_hex(kernel_heap_last_alloc);
    }
    terminal_write("\n");
}

static void shell_runtime_init(void)
{
    void* input_ptr = kmalloc_aligned(SHELL_INPUT_MAX, 8u);
    void* history_ptr = kmalloc_aligned(SHELL_HISTORY_MAX * SHELL_HISTORY_WIDTH, 8u);

    if (input_ptr != 0) {
        shell_input = (char*)input_ptr;
        memory_set_u8(shell_input, 0, SHELL_INPUT_MAX);
        shell_input_heap_backed = 1;
    }

    if (history_ptr != 0) {
        shell_history = (char (*)[SHELL_HISTORY_WIDTH])history_ptr;
        memory_set_u8(shell_history, 0, SHELL_HISTORY_MAX * SHELL_HISTORY_WIDTH);
        shell_history_heap_backed = 1;
    }
}

static void kernel_heap_alloc_test(const char* arg)
{
    uint32_t size;
    void* ptr;

    if (!parse_u32_decimal(arg, &size) || size == 0) {
        terminal_write("Usage: kmalloc <decimal-bytes>\n");
        return;
    }

    ptr = kmalloc_aligned(size, 8u);
    if (ptr == 0) {
        terminal_write("kmalloc failed\n");
        return;
    }

    terminal_write("kmalloc -> ");
    terminal_write_hex((uint32_t)(uintptr_t)ptr);
    terminal_write(" size=");
    terminal_write_dec_u32(size);
    terminal_write("\n");
}

static void shell_print_prompt(void)
{
    terminal_write("MBos> ");
    shell_input_origin_row = terminal_row;
    shell_input_origin_col = terminal_col;
    shell_input_len = 0;
    shell_input_cursor = 0;
    shell_input_render_len = 0;
    shell_input[0] = '\0';
}

static void shell_redraw_input(void)
{
    size_t max_len = shell_input_render_len;
    if (shell_input_len > max_len) {
        max_len = shell_input_len;
    }

    for (size_t i = 0; i < max_len; i++) {
        size_t row;
        size_t col;
        char c = ' ';

        if (i < shell_input_len) {
            c = shell_input[i];
        }

        terminal_advance_position(shell_input_origin_row, shell_input_origin_col, i, &row, &col);
        terminal_put_at(row, col, c);
    }

    shell_input_render_len = shell_input_len;
    terminal_advance_position(shell_input_origin_row, shell_input_origin_col, shell_input_cursor, &terminal_row, &terminal_col);
}

static void shell_set_input(const char* text)
{
    str_copy_bounded(shell_input, text, SHELL_INPUT_MAX);
    shell_input_len = str_length(shell_input);
    shell_input_cursor = shell_input_len;
    shell_redraw_input();
}

static void shell_insert_char(char c)
{
    if (shell_input_len >= SHELL_INPUT_MAX - 1) {
        return;
    }

    for (size_t i = shell_input_len; i > shell_input_cursor; i--) {
        shell_input[i] = shell_input[i - 1];
    }

    shell_input[shell_input_cursor] = c;
    shell_input_len++;
    shell_input_cursor++;
    shell_input[shell_input_len] = '\0';
    shell_redraw_input();
}

static void shell_backspace_char(void)
{
    if (shell_input_cursor == 0 || shell_input_len == 0) {
        return;
    }

    for (size_t i = shell_input_cursor - 1; i < shell_input_len; i++) {
        shell_input[i] = shell_input[i + 1];
    }

    shell_input_cursor--;
    shell_input_len--;
    shell_redraw_input();
}

static void shell_move_cursor_left(void)
{
    if (shell_input_cursor > 0) {
        shell_input_cursor--;
        shell_redraw_input();
    }
}

static void shell_move_cursor_right(void)
{
    if (shell_input_cursor < shell_input_len) {
        shell_input_cursor++;
        shell_redraw_input();
    }
}

static void shell_history_push(const char* cmd)
{
    if (str_is_empty(cmd)) {
        return;
    }

    str_copy_bounded(shell_history[shell_history_head], cmd, SHELL_HISTORY_WIDTH);
    shell_history_head = (shell_history_head + 1) % SHELL_HISTORY_MAX;
    if (shell_history_count < SHELL_HISTORY_MAX) {
        shell_history_count++;
    }
}

static const char* shell_history_get_from_newest(size_t offset)
{
    size_t index = (shell_history_head + SHELL_HISTORY_MAX - 1 - offset) % SHELL_HISTORY_MAX;
    return shell_history[index];
}

static int shell_resolve_disk_path(const char* input, char* out, uint32_t out_max)
{
    return process_resolve_disk_path(current_process_context(), input, out, out_max);
}

static void shell_execute_command(const char* cmd)
{
    const char* kmalloc_prefix = "kmalloc ";
    const char* trimid_prefix = "trimid ";
    const char* procset_prefix = "procset ";
    const char* schedq_prefix = "schedq ";
    const char* taskspawn_prefix = "taskspawn ";
    const char* taskteardown_prefix = "taskteardown ";
    const char* tasksleep_prefix = "tasksleep ";
    const char* taskwait_prefix = "taskwait ";
    const char* taskwake_prefix = "taskwake ";
    const char* eventwait_prefix = "eventwait ";
    const char* eventsig_prefix = "eventsig ";
    const char* inputch_prefix = "inputch ";
    const char* pageown_prefix = "pageown ";
    const char* taskstats_cmd = "taskstats";
    const char* waitq_cmd = "waitq";
    const char* guistatus_cmd = "guistatus";
    const char* guitest_cmd = "guitest";
    const char* guitext_prefix = "guitext ";
    const char* wmopen_prefix  = "wmopen ";
    const char* wmclose_prefix = "wmclose ";
    const char* guiflip_cmd    = "guiflip";
    const char* guibbuf_cmd    = "guibbuf";
    const char* inputstatus_cmd = "inputstatus";
    const char* inputlog_cmd = "inputlog";
    const char* inputbridgeon_cmd = "inputbridgeon";
    const char* inputbridgeoff_cmd = "inputbridgeoff";
    const char* taskrun_prefix = "taskrun ";
    const char* taskstop_prefix = "taskstop ";
    const char* snapinfo_prefix = "snapinfo ";
    const char* snaprestore_prefix = "snaprestore ";
    const char* snapseed_prefix = "snapseed ";
    const char* fscat_prefix = "fscat ";
    const char* fswrite_prefix = "fswrite ";
    const char* fsrm_prefix = "fsrm ";
    const char* diskcat_prefix = "diskcat ";
    const char* diskwrite_prefix = "diskwrite ";
    const char* diskrm_prefix = "diskrm ";
    const char* appload_prefix = "appload ";
    const char* apprun_prefix = "apprun ";
    const char* appfmt_prefix = "appfmt ";
    const char* cd_prefix = "cd ";

    if (cmd[0] == '\0') {
        return;
    }

    if (str_equals(cmd, "help")) {
        terminal_write("Commands: help, clear, about, uptime, ticks, procinfo, procset <id>, tasklist, taskstats, waitq, taskspawn <id>, taskteardown <id>, tasksleep <ticks>, taskwait <ticks>, taskwake <id>, eventwait <ch>, eventsig <ch>, inputstatus, inputlog, inputbridgeon, inputbridgeoff, inputch <ch>, mousestatus, mouseon, mouseoff, pageown <id>, taskrun <id>, taskstop <id>, snapinfo [id], snapnow, snapseed [id], snaprestore [id], ctxswon, ctxswoff, ctxswstat, sched, schedon, schedoff, schedq <ticks>, guistatus, guitest, guicursor, guitext <text>, guiflip, guibbuf, wmon, wmoff, wmrender, wmopen <title>, wmclose <idx>, wmlist, fsls, fscat <name>, fswrite <name> <text>, fsrm <name>, pwd, cd <path>, vls, vcat <path>, vwrite <path> <text>, vrm <path>, diskstat, diskmount, diskumount, diskfmt, diskls [dir], diskmkdir <path>, diskcat <path>, diskwrite <path> <text>, diskrm <path>, fstest, fdstat, appfmt <name>, appload <name>, apprun <name>, syscalls, syscallinfo, syscalldefs, userevents, meminfo, memmap, allocstat, allocpage, idmap, trimid [mb], uspace, mapusertest, lookuserlast, usertestprep, userctx, userrun, pgstat, pgdir, pgmaptest, pglooklast, virtlayout, kheap, kmalloc <bytes>\n");
        terminal_write("Editing: Left/Right arrows, Backspace\n");
        terminal_write("History: Up/Down arrows\n");
        return;
    }

    if (str_starts_with(cmd, appfmt_prefix)) {
        const char* name = cmd + str_length(appfmt_prefix);
        int32_t idx;
        enum exec_format fmt;
        if (!ramfs_name_valid(name)) {
            terminal_write("Usage: appfmt <name>\n");
            return;
        }
        idx = ramfs_find_index(name);
        if (idx < 0) {
            terminal_write("appfmt: file not found\n");
            return;
        }
        fmt = exec_detect_format(ramfs_files[(uint32_t)idx].data, ramfs_files[(uint32_t)idx].size);
        terminal_write("Format: ");
        terminal_write(exec_format_name(fmt));
        terminal_write("\n");
        return;
    }

    if (str_equals(cmd, "fdstat")) {
        process_fd_print_table(current_process_context());
        return;
    }

    if (str_equals(cmd, "fstest")) {
        ramfs_backend_smoke_test();
        return;
    }

    if (str_starts_with(cmd, appload_prefix)) {
        const char* name = cmd + str_length(appload_prefix);
        int rc;
        enum exec_format fmt = EXEC_FORMAT_UNKNOWN;
        if (!ramfs_name_valid(name)) {
            terminal_write("Usage: appload <name>\n");
            return;
        }

        rc = user_load_ramfs_image_for_current(name, &fmt);
        if (rc == 0) {
            terminal_write("App image loaded (format=");
            terminal_write(exec_format_name(fmt));
            terminal_write(") for current PID. Run with 'userrun' or 'apprun <name>'.\n");
        } else if (rc == -1) {
            terminal_write("appload: file not found\n");
        } else if (rc == -2) {
            terminal_write("appload: invalid image size (must be 1..4096 bytes)\n");
        } else if (rc == -6) {
            terminal_write("appload: PE32 detected. Windows ABI loader not implemented yet\n");
        } else if (rc == -7) {
            terminal_write("appload: invalid executable image (bad header/segments)\n");
        } else if (rc == -8) {
            terminal_write("appload: user page mapping failed\n");
        } else {
            terminal_write("appload: failed to map user pages\n");
        }
        return;
    }

    if (str_starts_with(cmd, apprun_prefix)) {
        const char* name = cmd + str_length(apprun_prefix);
        int rc;
        enum exec_format fmt = EXEC_FORMAT_UNKNOWN;
        struct process_syscall_context* proc = current_process_context();
        if (!ramfs_name_valid(name)) {
            terminal_write("Usage: apprun <name>\n");
            return;
        }

        rc = user_load_ramfs_image_for_current(name, &fmt);
        if (rc != 0) {
            terminal_write("apprun: failed to load app image (format=");
            terminal_write(exec_format_name(fmt));
            terminal_write(")\n");
            if (rc == -6) {
                terminal_write("apprun: PE32 path requires Windows ABI runtime layer\n");
            } else if (rc == -7) {
                terminal_write("apprun: malformed executable image\n");
            } else if (rc == -8) {
                terminal_write("apprun: failed to map user pages\n");
            }
            return;
        }

        terminal_write("Entering ring3 app from RAMFS (non-returning)\n");
        user_enter_ring3(proc->user_eip, proc->user_esp);
        return;
    }

    if (str_equals(cmd, "fsls")) {
        ramfs_list();
        return;
    }

    if (str_equals(cmd, "pwd")) {
        mbfs_terminal_write_path(current_process_context()->cwd);
        terminal_write("\n");
        return;
    }

    if (str_equals(cmd, "cd")) {
        current_process_context()->cwd[0] = '\0';
        terminal_write("cwd: /\n");
        return;
    }

    if (str_starts_with(cmd, cd_prefix)) {
        const char* path = cmd + str_length(cd_prefix);
        if (!diskfs_mounted) {
            terminal_write("mbfs: not mounted\n");
            return;
        }
        if (!process_set_cwd(current_process_context(), path)) {
            terminal_write("mbfs: directory not found\n");
            return;
        }
        terminal_write("cwd: ");
        mbfs_terminal_write_path(current_process_context()->cwd);
        terminal_write("\n");
        return;
    }

    if (str_equals(cmd, "vls")) {
        if (diskfs_mounted) {
            terminal_write("disk (mbfs):\n");
            diskfs_list_path(current_process_context()->cwd);
        }
        terminal_write("mem (ramfs):\n");
        ramfs_list();
        return;
    }

    if (str_starts_with(cmd, "vcat ")) {
        const char* path = cmd + 5u;
        char resolved[MBFS_PATH_MAX];
        uint8_t allow_ramfs_fallback = (path[0] != '/' && !str_contains_char(path, '/')
                                        && current_process_context()->cwd[0] == '\0');

        if (!shell_resolve_disk_path(path, resolved, sizeof(resolved)) || resolved[0] == '\0') {
            terminal_write("Usage: vcat <path>\n");
            return;
        }
        if (diskfs_mounted) {
            struct mbfs_path_lookup lookup;
            if (diskfs_lookup_path(resolved, &lookup, 0, 0u) == 1) {
                diskfs_cat_path(resolved);
                return;
            }
            if (!allow_ramfs_fallback) {
                terminal_write("vfs: file not found\n");
                return;
            }
        }
        ramfs_cat(path);
        return;
    }

    if (str_starts_with(cmd, "vwrite ")) {
        const char* args = cmd + 7u;
        char path[MBFS_PATH_MAX];
        char resolved[MBFS_PATH_MAX];
        uint32_t n = 0;
        int rc;

        while (args[n] != '\0' && args[n] != ' ' && n + 1u < MBFS_PATH_MAX) {
            path[n] = args[n];
            n++;
        }
        path[n] = '\0';

        if (args[n] != ' ' || !shell_resolve_disk_path(path, resolved, sizeof(resolved)) || resolved[0] == '\0') {
            terminal_write("Usage: vwrite <path> <text>\n");
            return;
        }
        while (args[n] == ' ') {
            n++;
        }

        if (diskfs_mounted) {
            rc = diskfs_write_path(resolved, (const uint8_t*)(args + n), (uint32_t)str_length(args + n));
            if (rc == 0) {
                terminal_write("vfs: written to disk\n");
            } else if (rc == -8) {
                terminal_write("vfs: parent directory not found\n");
            } else if (rc == -4) {
                terminal_write("vfs: not enough free sectors\n");
            } else {
                terminal_write("vfs: disk write failed\n");
            }
        } else {
            if (path[0] == '/' || str_contains_char(path, '/') || current_process_context()->cwd[0] != '\0') {
                terminal_write("vfs: mem write failed\n");
                return;
            }
            rc = ramfs_write_text(path, args + n);
            if (rc == 0) {
                terminal_write("vfs: written to mem\n");
            } else {
                terminal_write("vfs: mem write failed\n");
            }
        }
        return;
    }

    if (str_starts_with(cmd, "vrm ")) {
        const char* path = cmd + 4u;
        char resolved[MBFS_PATH_MAX];
        uint8_t allow_ramfs_fallback = (path[0] != '/' && !str_contains_char(path, '/')
                                        && current_process_context()->cwd[0] == '\0');

        if (!shell_resolve_disk_path(path, resolved, sizeof(resolved)) || resolved[0] == '\0') {
            terminal_write("Usage: vrm <path>\n");
            return;
        }
        if (diskfs_mounted) {
            int rmrc = diskfs_remove_path(resolved);
            if (rmrc == 1) {
                terminal_write("vfs: removed from disk\n");
                return;
            }
            if (rmrc == -2) {
                terminal_write("vfs: directory not empty\n");
                return;
            }
            if (!allow_ramfs_fallback) {
                terminal_write("vfs: not found\n");
                return;
            }
        }
        if (ramfs_remove(path)) {
            terminal_write("vfs: removed from mem\n");
        } else {
            terminal_write("vfs: not found\n");
        }
        return;
    }

    if (str_equals(cmd, "diskstat")) {
        diskfs_print_status();
        return;
    }

    if (str_equals(cmd, "diskmount")) {
        if (diskfs_try_mount()) {
            terminal_write("mbfs: mounted\n");
            if (mbfs_unclean_last_mount) {
                terminal_write("mbfs warning: previous shutdown was unclean (recovered metadata state)\n");
            }
        } else {
            terminal_write("mbfs: mount failed or no formatted ATA disk found\n");
        }
        return;
    }

    if (str_equals(cmd, "diskumount")) {
        if (!diskfs_mounted) {
            terminal_write("mbfs: not mounted\n");
        } else {
            diskfs_umount();
            terminal_write("mbfs: unmounted cleanly\n");
        }
        return;
    }

    if (str_equals(cmd, "diskfmt")) {
        int rc = diskfs_format();
        if (rc == 0) {
            terminal_write("mbfs: format ok\n");
        } else if (rc == -1) {
            terminal_write("mbfs: no ATA disk available\n");
        } else if (rc == -2) {
            terminal_write("mbfs: disk too small\n");
        } else {
            terminal_write("mbfs: format failed\n");
        }
        return;
    }

    if (str_equals(cmd, "diskcheck")) {
        int rc = diskfs_check();
        if (rc == -1) {
            terminal_write("diskcheck: not mounted\n");
        } else if (rc == 0) {
            terminal_write("diskcheck: OK\n");
        } else {
            terminal_write("diskcheck: problems found: "); terminal_write_dec_u32((uint32_t)rc); terminal_write("\n");
        }
        return;
    }

    if (str_equals(cmd, "diskrepair")) {
        int rc = diskfs_repair();
        if (rc == 0) {
            terminal_write("diskrepair: repaired successfully\n");
        } else if (rc == -1) {
            terminal_write("diskrepair: not mounted\n");
        } else {
            terminal_write("diskrepair: repair failed (rc="); terminal_write_dec_u32((uint32_t)rc); terminal_write(")\n");
        }
        return;
    }

    if (str_equals(cmd, "diskfstest")) {
        diskfs_smoke_test();
        return;
    }

    if (str_equals(cmd, "diskls")) {
        diskfs_list_path(current_process_context()->cwd);
        return;
    }

    if (str_starts_with(cmd, "diskls ")) {
        const char* path = cmd + 7u; /* strlen("diskls ") */
        char resolved[MBFS_PATH_MAX];
        if (!shell_resolve_disk_path(path, resolved, sizeof(resolved))) {
            terminal_write("Usage: diskls [dir]\n");
            return;
        }
        diskfs_list_path(resolved);
        return;
    }

    if (str_starts_with(cmd, "diskmkdir ")) {
        const char* name = cmd + 10u; /* strlen("diskmkdir ") */
        char resolved[MBFS_PATH_MAX];
        if (!shell_resolve_disk_path(name, resolved, sizeof(resolved)) || resolved[0] == '\0') {
            terminal_write("Usage: diskmkdir <path>\n");
            return;
        }
        int mkrc = diskfs_mkdir(resolved);
        if (mkrc == 0) {
            terminal_write("diskfs: directory created\n");
        } else if (mkrc == -1) {
            terminal_write("diskfs: not mounted\n");
        } else if (mkrc == -2) {
            terminal_write("diskfs: invalid name\n");
        } else if (mkrc == -3) {
            terminal_write("diskfs: directory or inode table full\n");
        } else if (mkrc == -4) {
            terminal_write("diskfs: not enough free sectors\n");
        } else if (mkrc == -8) {
            terminal_write("diskfs: parent directory not found\n");
        } else if (mkrc == -7) {
            terminal_write("diskfs: name already exists\n");
        } else {
            terminal_write("diskfs: mkdir failed\n");
        }
        return;
    }

    if (str_starts_with(cmd, diskcat_prefix)) {
        const char* path = cmd + str_length(diskcat_prefix);
        char resolved[MBFS_PATH_MAX];
        if (!shell_resolve_disk_path(path, resolved, sizeof(resolved)) || resolved[0] == '\0') {
            terminal_write("Usage: diskcat <path>\n");
            return;
        }
        diskfs_cat_path(resolved);
        return;
    }

    if (str_starts_with(cmd, diskrm_prefix)) {
        const char* path = cmd + str_length(diskrm_prefix);
        char resolved[MBFS_PATH_MAX];
        if (!shell_resolve_disk_path(path, resolved, sizeof(resolved)) || resolved[0] == '\0') {
            terminal_write("Usage: diskrm <path>\n");
            return;
        }
        int rmrc = diskfs_remove_path(resolved);
        if (rmrc == 1) {
            terminal_write("diskfs: removed\n");
        } else if (rmrc == -2) {
            terminal_write("diskfs: directory not empty\n");
        } else {
            terminal_write("diskfs: not found or remove failed\n");
        }
        return;
    }

    if (str_starts_with(cmd, diskwrite_prefix)) {
        const char* args = cmd + str_length(diskwrite_prefix);
        char path[MBFS_PATH_MAX];
        char resolved[MBFS_PATH_MAX];
        uint32_t n = 0;
        int rc;

        while (args[n] != '\0' && args[n] != ' ' && n + 1u < MBFS_PATH_MAX) {
            path[n] = args[n];
            n++;
        }
        path[n] = '\0';

        if (args[n] != ' ' || !shell_resolve_disk_path(path, resolved, sizeof(resolved)) || resolved[0] == '\0') {
            terminal_write("Usage: diskwrite <path> <text>\n");
            return;
        }

        while (args[n] == ' ') {
            n++;
        }

        rc = diskfs_write_path(resolved, (const uint8_t*)(args + n), (uint32_t)str_length(args + n));
        if (rc == -1) {
            terminal_write("diskfs: not mounted\n");
        } else if (rc == -2) {
            terminal_write("diskfs: invalid path\n");
        } else if (rc == -3) {
            terminal_write("diskfs: directory full\n");
        } else if (rc == -4) {
            terminal_write("diskfs: not enough free sectors\n");
        } else if (rc == -8) {
            terminal_write("diskfs: parent directory not found\n");
        } else if (rc != 0) {
            terminal_write("diskfs: write failed\n");
        } else {
            terminal_write("diskfs: write ok\n");
        }
        return;
    }

    if (str_starts_with(cmd, fscat_prefix)) {
        const char* name = cmd + str_length(fscat_prefix);
        if (!ramfs_name_valid(name)) {
            terminal_write("Usage: fscat <name>\n");
            return;
        }
        ramfs_cat(name);
        return;
    }

    if (str_starts_with(cmd, fsrm_prefix)) {
        const char* name = cmd + str_length(fsrm_prefix);
        if (!ramfs_name_valid(name)) {
            terminal_write("Usage: fsrm <name>\n");
            return;
        }
        if (ramfs_remove(name)) {
            terminal_write("ramfs: removed\n");
        } else {
            terminal_write("ramfs: file not found\n");
        }
        return;
    }

    if (str_starts_with(cmd, fswrite_prefix)) {
        const char* args = cmd + str_length(fswrite_prefix);
        char name[RAMFS_NAME_MAX];
        uint32_t n = 0;
        int rc;

        while (args[n] != '\0' && args[n] != ' ' && n + 1u < RAMFS_NAME_MAX) {
            name[n] = args[n];
            n++;
        }
        name[n] = '\0';

        if (!ramfs_name_valid(name) || args[n] != ' ') {
            terminal_write("Usage: fswrite <name> <text>\n");
            return;
        }

        while (args[n] == ' ') {
            n++;
        }

        rc = ramfs_write_text(name, args + n);
        if (rc == -1) {
            terminal_write("ramfs: invalid file name\n");
        } else if (rc == -2) {
            terminal_write("ramfs: content too large (max 4096 bytes)\n");
        } else if (rc == -3) {
            terminal_write("ramfs: file table full\n");
        } else if (rc == -4) {
            terminal_write("ramfs: out of kernel heap memory\n");
        } else {
            terminal_write("ramfs: write ok\n");
        }
        return;
    }

    if (str_equals(cmd, inputstatus_cmd)) {
        input_bridge_print_status();
        return;
    }

    if (str_equals(cmd, inputlog_cmd)) {
        input_bridge_print_log();
        return;
    }

    /* Debug dump of GUI/input event log */
    if (str_equals(cmd, "debuggui")) {
        dbg_dump_log();
        return;
    }

    if (str_equals(cmd, inputbridgeon_cmd)) {
        input_event_bridge_enabled = 1;
        terminal_write("Input bridge enabled\n");
        return;
    }

    if (str_equals(cmd, inputbridgeoff_cmd)) {
        input_event_bridge_enabled = 0;
        terminal_write("Input bridge disabled\n");
        return;
    }

    if (str_starts_with(cmd, inputch_prefix)) {
        uint32_t ch;
        if (!parse_u32_decimal(cmd + str_length(inputch_prefix), &ch)) {
            terminal_write("Usage: inputch <channel>\n");
            return;
        }
        input_event_channel = ch;
        terminal_write("Input event channel set to ");
        terminal_write_dec_u32(input_event_channel);
        terminal_write("\n");
        return;
    }

    if (str_equals(cmd, guistatus_cmd)) {
        gui_print_status();
        return;
    }

    const char* mousestatus_cmd = "mousestatus";
    if (str_equals(cmd, mousestatus_cmd)) {
        terminal_write("Mouse enabled: ");
        terminal_write(mouse_enabled ? "yes" : "no");
        terminal_write("\nX: ");
        terminal_write_dec_u32(mouse_x);
        terminal_write(" Y: ");
        terminal_write_dec_u32(mouse_y);
        terminal_write(" Buttons: ");
        terminal_write_hex(mouse_buttons);
        terminal_write("\n");
        return;
    }

    const char* mouseon_cmd = "mouseon";
    if (str_equals(cmd, mouseon_cmd)) {
        mouse_enabled = 1;
        terminal_write("Mouse enabled\n");
        return;
    }

    const char* mouseoff_cmd = "mouseoff";
    if (str_equals(cmd, mouseoff_cmd)) {
        mouse_enabled = 0;
        terminal_write("Mouse disabled\n");
        return;
    }

    const char* simclick_prefix = "simclick";
    if (str_starts_with(cmd, simclick_prefix)) {
        const char* p = cmd + str_length(simclick_prefix);
        while (*p == ' ') { p++; }
        if (*p == '\0') {
            terminal_write("Usage: simclick <x> <y>\n");
            return;
        }
        /* Parse X */
        char numbuf[16];
        size_t i = 0;
        while (p[i] != '\0' && p[i] != ' ' && i + 1 < sizeof(numbuf)) {
            numbuf[i] = p[i]; i++;
        }
        numbuf[i] = '\0';
        uint32_t sx = 0;
        if (!parse_u32_decimal(numbuf, &sx)) {
            terminal_write("Invalid X coordinate\n");
            return;
        }
        /* Skip spaces and parse Y */
        const char* q = p + i;
        while (*q == ' ') { q++; }
        if (*q == '\0') { terminal_write("Usage: simclick <x> <y>\n"); return; }
        uint32_t sy = 0;
        if (!parse_u32_decimal(q, &sy)) {
            terminal_write("Invalid Y coordinate\n");
            return;
        }

        /* Simulate click: press then release */
        mouse_x = sx;
        mouse_y = sy;
        mouse_buttons |= 0x01u;
        (void)wm_handle_mouse();
        mouse_buttons &= (uint8_t)~0x01u;
        (void)wm_handle_mouse();
        wm_render_pending = 1;
        terminal_write("Simulated click at "); terminal_write_dec_u32(sx); terminal_write(","); terminal_write_dec_u32(sy); terminal_write("\n");
        return;
    }

    /* Toggle serial debug emission at runtime: dbgserial <on|off> */
    const char* dbgserial_prefix = "dbgserial ";
    if (str_starts_with(cmd, dbgserial_prefix)) {
        const char* arg = cmd + str_length(dbgserial_prefix);
        while (*arg == ' ') { arg++; }
        if (str_equals(arg, "on")) {
            dbg_emit_serial = 1;
            terminal_write("Serial debug enabled\n");
        } else if (str_equals(arg, "off")) {
            dbg_emit_serial = 0;
            terminal_write("Serial debug disabled\n");
        } else {
            terminal_write("Usage: dbgserial <on|off>\n");
        }
        return;
    }

    /* Dump current WM state to serial/terminal */
    if (str_equals(cmd, "wmdump")) {
        wm_dump_state();
        return;
    }

    if (str_equals(cmd, guitest_cmd)) {
        if (!gui_framebuffer_detected) {
            terminal_write("No framebuffer info from bootloader\n");
            return;
        }

        if (!gui_framebuffer_mapped && !gui_map_framebuffer()) {
            terminal_write("Framebuffer map failed\n");
            return;
        }

        gui_draw_test_scene();
        gui_draw_cursor();
        terminal_write("Test scene rendered with cursor at ");
        terminal_write_dec_u32(mouse_x);
        terminal_write(", ");
        terminal_write_dec_u32(mouse_y);
        terminal_write("\n");
        return;
    }

    const char* guicursor_cmd = "guicursor";
    if (str_equals(cmd, guicursor_cmd)) {
        if (!gui_framebuffer_detected) {
            terminal_write("No framebuffer\n");
            return;
        }

        if (!gui_framebuffer_mapped && !gui_map_framebuffer()) {
            terminal_write("Framebuffer map failed\n");
            return;
        }

        gui_draw_cursor();
        terminal_write("Cursor drawn\n");
        return;
    }

    if (str_starts_with(cmd, guitext_prefix)) {
        const char* text = cmd + str_length(guitext_prefix);
        if (!gui_framebuffer_detected) {
            terminal_write("No framebuffer\n");
            return;
        }
        if (!gui_framebuffer_mapped && !gui_map_framebuffer()) {
            terminal_write("Framebuffer map failed\n");
            return;
        }
        gui_draw_string(20u, 20u, text, 0xFFFF00u, 0x00000000u);
        terminal_write("Text drawn: ");
        terminal_write(text);
        terminal_write("\n");
        return;
    }

    if (str_equals(cmd, "wmon")) {
        if (!gui_framebuffer_detected) {
            terminal_write("No framebuffer\n");
            return;
        }
        if (!gui_framebuffer_mapped && !gui_map_framebuffer()) {
            terminal_write("Framebuffer map failed\n");
            return;
        }
        wm_enabled = 1;
        mouse_enabled = 1;
        wm_render();
        terminal_write("Window manager on\n");
        return;
    }

    if (str_equals(cmd, "wmoff")) {
        wm_enabled = 0;
        terminal_write("Window manager off\n");
        return;
    }

    if (str_equals(cmd, "wmrender")) {
        if (!wm_enabled) {
            terminal_write("WM not enabled. Run 'wmon' first\n");
            return;
        }
        wm_render();
        terminal_write("WM rendered\n");
        return;
    }

    /* Open a simple MBFS file-manager window: wmopenfs [path]
     * If path omitted, use current process cwd (or root).
     */
    if (str_starts_with(cmd, "wmopenfs ")) {
        const char* arg = cmd + str_length("wmopenfs ");
        char resolved[MBFS_PATH_MAX];
        if (!process_resolve_disk_path(current_process_context(), arg, resolved, sizeof(resolved))) {
            terminal_write("Usage: wmopenfs <path>\n");
            return;
        }
        if (!gui_framebuffer_detected) {
            terminal_write("No framebuffer\n");
            return;
        }
        if (!gui_framebuffer_mapped && !gui_map_framebuffer()) {
            terminal_write("Framebuffer map failed\n");
            return;
        }
        uint32_t ox = 60u + wm_window_count * 30u;
        uint32_t oy = 60u + wm_window_count * 24u;
        int32_t idx = wm_open_window((int32_t)ox, (int32_t)oy, 480u, 300u, "MBFS");
        if (idx < 0) {
            terminal_write("Max windows reached\n");
            return;
        }
        wm_windows[idx].type = (uint8_t)WM_TYPE_FILEMANAGER;
        wm_windows[idx].content_id = (uint32_t)idx;
        wm_fs_states[idx].used = 1;
        wm_fs_states[idx].scroll = 0;
        wm_fs_states[idx].selected = 0;
        str_copy_bounded(wm_fs_states[idx].path, resolved, sizeof(wm_fs_states[idx].path));
        if (wm_enabled) { wm_render(); }
        terminal_write("Window opened: idx=");
        terminal_write_dec_u32((uint32_t)idx);
        terminal_write("\n");
        return;
    }

    if (str_equals(cmd, "wmopenfs")) {
        char resolved[MBFS_PATH_MAX];
        const char* cwd = current_process_context()->cwd;
        if (cwd == 0) { resolved[0] = '\0'; }
        else if (!process_resolve_disk_path(current_process_context(), cwd, resolved, sizeof(resolved))) {
            resolved[0] = '\0';
        }
        if (!gui_framebuffer_detected) {
            terminal_write("No framebuffer\n");
            return;
        }
        if (!gui_framebuffer_mapped && !gui_map_framebuffer()) {
            terminal_write("Framebuffer map failed\n");
            return;
        }
        uint32_t ox2 = 60u + wm_window_count * 30u;
        uint32_t oy2 = 60u + wm_window_count * 24u;
        int32_t idx2 = wm_open_window((int32_t)ox2, (int32_t)oy2, 480u, 300u, "MBFS");
        if (idx2 < 0) {
            terminal_write("Max windows reached\n");
            return;
        }
        wm_windows[idx2].type = (uint8_t)WM_TYPE_FILEMANAGER;
        wm_windows[idx2].content_id = (uint32_t)idx2;
        wm_fs_states[idx2].used = 1;
        wm_fs_states[idx2].scroll = 0;
        wm_fs_states[idx2].selected = 0;
        str_copy_bounded(wm_fs_states[idx2].path, resolved, sizeof(wm_fs_states[idx2].path));
        if (wm_enabled) { wm_render(); }
        terminal_write("Window opened: idx=");
        terminal_write_dec_u32((uint32_t)idx2);
        terminal_write("\n");
        return;
    }

    /* Open a terminal window: wmopenterm */
    if (str_equals(cmd, "wmopenterm") || str_starts_with(cmd, "wmopenterm ")) {
        if (!gui_framebuffer_detected) {
            terminal_write("No framebuffer\n");
            return;
        }
        if (!gui_framebuffer_mapped && !gui_map_framebuffer()) {
            terminal_write("Framebuffer map failed\n");
            return;
        }
        uint32_t ox = 80u + wm_window_count * 28u;
        uint32_t oy = 80u + wm_window_count * 22u;
        int32_t idx = wm_open_window((int32_t)ox, (int32_t)oy, 600u, 360u, "Terminal");
        if (idx < 0) {
            terminal_write("Max windows reached\n");
            return;
        }
        wm_windows[idx].type = (uint8_t)WM_TYPE_TERMINAL;
        wm_windows[idx].content_id = (uint32_t)idx;
        wm_term_states[idx].used = 1;
        wm_term_states[idx].scroll = 0;
        wm_term_states[idx].input_len = 0;
        wm_term_states[idx].input_cursor = 0;
        wm_term_states[idx].input[0] = '\0';
        if (wm_enabled) { wm_render(); }
        terminal_write("Terminal window opened: idx=");
        terminal_write_dec_u32((uint32_t)idx);
        terminal_write("\n");
        return;
    }

    if (str_starts_with(cmd, wmopen_prefix)) {
        const char* title = cmd + str_length(wmopen_prefix);
        if (!gui_framebuffer_detected) {
            terminal_write("No framebuffer\n");
            return;
        }
        if (!gui_framebuffer_mapped && !gui_map_framebuffer()) {
            terminal_write("Framebuffer map failed\n");
            return;
        }
        uint32_t ox = 60u + wm_window_count * 30u;
        uint32_t oy = 60u + wm_window_count * 24u;
        int32_t idx = wm_open_window((int32_t)ox, (int32_t)oy, 400u, 260u, title);
        if (idx < 0) {
            terminal_write("Max windows reached\n");
            return;
        }
        if (wm_enabled) { wm_render(); }
        terminal_write("Window opened: idx=");
        terminal_write_dec_u32((uint32_t)idx);
        terminal_write("\n");
        return;
    }

    if (str_starts_with(cmd, wmclose_prefix)) {
        uint32_t idx;
        if (!parse_u32_decimal(cmd + str_length(wmclose_prefix), &idx)
                || idx >= WM_MAX_WINDOWS || !wm_windows[idx].used) {
            terminal_write("Usage: wmclose <window-index>\n");
            return;
        }
        wm_close_window(idx);
        if (wm_enabled) { wm_render(); }
        terminal_write("Window closed\n");
        return;
    }

    if (str_equals(cmd, "wmlist")) {
        if (wm_window_count == 0) {
            terminal_write("No open windows\n");
            return;
        }
        for (uint32_t i = 0; i < WM_MAX_WINDOWS; i++) {
            if (!wm_windows[i].used) { continue; }
            terminal_write_dec_u32(i);
            terminal_write(": ");
            terminal_write(wm_windows[i].title);
            terminal_write(" at (");
            terminal_write_dec_u32((uint32_t)wm_windows[i].x);
            terminal_write(",");
            terminal_write_dec_u32((uint32_t)wm_windows[i].y);
            terminal_write(") ");
            terminal_write_dec_u32(wm_windows[i].w);
            terminal_write("x");
            terminal_write_dec_u32(wm_windows[i].h);
            if (wm_focused_index == (int32_t)i) {
                terminal_write(" [focused]" );
            }
            terminal_write("\n");
        }
        return;
    }

    if (str_equals(cmd, guiflip_cmd)) {
        if (!gui_framebuffer_mapped) {
            terminal_write("No framebuffer mapped\n");
            return;
        }
        gui_flip();
        terminal_write("Flip: back-buffer -> framebuffer\n");
        return;
    }

    if (str_equals(cmd, guibbuf_cmd)) {
        terminal_write("Back-buffer ready: ");
        terminal_write(gui_backbuffer_ready ? "yes" : "no");
        terminal_write("\nBack-buffer ptr: ");
        terminal_write_hex((uint32_t)(uintptr_t)gui_backbuffer);
        terminal_write("\nFramebuffer size: ");
        terminal_write_hex(gui_framebuffer_size);
        terminal_write("\n");
        return;
    }

    if (str_equals(cmd, "about")) {
        terminal_write("MBos: 32-bit ASM + C hobby kernel\n");
        return;
    }

    if (str_equals(cmd, "clear")) {
        terminal_clear();
        return;
    }

    if (str_equals(cmd, "uptime")) {
        uint32_t ticks = timer_ticks;
        uint32_t seconds = ticks / timer_hz;
        uint32_t hundredths = (ticks % timer_hz) * 100 / timer_hz;

        terminal_write("Uptime: ");
        terminal_write_dec_u32(seconds);
        terminal_putchar('.');
        if (hundredths < 10) {
            terminal_putchar('0');
        }
        terminal_write_dec_u32(hundredths);
        terminal_write("s\n");
        return;
    }

    if (str_equals(cmd, "ticks")) {
        terminal_write("Ticks: ");
        terminal_write_dec_u32(timer_ticks);
        terminal_write("\n");
        return;
    }

    if (str_equals(cmd, "syscalls")) {
        struct process_syscall_context* proc = current_process_context();
        terminal_write("Syscall traps: ");
        terminal_write_dec_u32(syscall_count);
        terminal_write("\nCurrent process syscalls: ");
        terminal_write_dec_u32(proc->syscall_count);
        terminal_write("\n");
        return;
    }

    if (str_equals(cmd, "syscallinfo")) {
        struct process_syscall_context* proc = current_process_context();
        terminal_write("Current PID: ");
        terminal_write_dec_u32(proc->pid);
        terminal_write("\n");
        terminal_write("Last syscall num: ");
        terminal_write_dec_u32(proc->last_num);
        terminal_write("\nLast arg0 (ebx): ");
        terminal_write_hex(proc->last_arg0);
        terminal_write("\nLast return (eax): ");
        terminal_write_hex(proc->last_ret);
        terminal_write("\n");
        return;
    }

    if (str_equals(cmd, "syscalldefs")) {
        terminal_write("Syscall IDs:\n");
        terminal_write("  1 -> get magic\n");
        terminal_write("  2 -> write char (arg0: ebx low byte)\n");
        terminal_write("  3 -> yield (scheduler handoff)\n");
        terminal_write("  4 -> exit event (arg0: exit code)\n");
        terminal_write("  5 -> sleep ticks (arg0: ebx ticks)\n");
        terminal_write("  6 -> wait event channel (arg0: ebx channel)\n");
        terminal_write("  7 -> signal event channel (arg0: ebx channel)\n");
        terminal_write("  8 -> file open (ebx=name ptr, ecx=flags 1=R 2=W)\n");
        terminal_write("  9 -> file read (ebx=fd, ecx=buf ptr, edx=max bytes)\n");
        terminal_write(" 10 -> file write (ebx=fd, ecx=buf ptr, edx=bytes)\n");
        terminal_write(" 11 -> file close (ebx=fd)\n");
        terminal_write(" 12 -> file seek (ebx=fd, ecx=offset)\n");
        terminal_write(" 13 -> file size (ebx=fd)\n");
        terminal_write(" 14 -> brk (ebx=0 query, else set break)\n");
        terminal_write(" 15 -> get ticks\n");
        return;
    }

    if (str_equals(cmd, "userevents")) {
        struct process_syscall_context* proc = current_process_context();
        terminal_write("User exit events: ");
        terminal_write_dec_u32(user_exit_count);
        terminal_write("\nLast exit code: ");
        terminal_write_dec_u32(user_last_exit_code);
        terminal_write("\nCurrent process exits: ");
        terminal_write_dec_u32(proc->exit_count);
        terminal_write("\nCurrent process last exit code: ");
        terminal_write_dec_u32(proc->last_exit_code);
        terminal_write("\n");
        return;
    }

    if (str_equals(cmd, "procinfo")) {
        struct process_syscall_context* proc = current_process_context();
        terminal_write("Current process slot: ");
        terminal_write_dec_u32(current_process_slot);
        terminal_write("\nCurrent PID: ");
        terminal_write_dec_u32(proc->pid);
        terminal_write("\nTask state: ");
        terminal_write(task_state_name(proc->task_state));
        terminal_write("\nWait reason: ");
        terminal_write(wait_reason_name(proc->wait_reason));
        terminal_write("\nSleep-until tick: ");
        terminal_write_dec_u32(proc->sleep_until_tick);
        terminal_write("\nWait-timeout tick: ");
        terminal_write_dec_u32(proc->wait_timeout_tick);
        terminal_write("\nRunnable: ");
        terminal_write(proc->runnable ? "yes" : "no");
        terminal_write("\nContext switches in: ");
        terminal_write_dec_u32(proc->context_switches);
        terminal_write("\nCWD: ");
        mbfs_terminal_write_path(proc->cwd);
        terminal_write("\n");
        return;
    }

    if (str_starts_with(cmd, procset_prefix)) {
        uint32_t pid;
        if (!parse_u32_decimal(cmd + str_length(procset_prefix), &pid) || pid == 0 || pid > PROCESS_CONTEXT_MAX) {
            terminal_write("Usage: procset <1-");
            terminal_write_dec_u32(PROCESS_CONTEXT_MAX);
            terminal_write(">\n");
            return;
        }

        if (process_contexts[current_process_slot].task_state == TASK_STATE_RUNNING) {
            process_set_task_state(&process_contexts[current_process_slot], TASK_STATE_READY);
        }
        current_process_slot = pid - 1u;
        if (process_contexts[current_process_slot].runnable) {
            process_set_task_state(&process_contexts[current_process_slot], TASK_STATE_RUNNING);
        }
        terminal_write("Switched to PID ");
        terminal_write_dec_u32(pid);
        terminal_write("\n");
        return;
    }

    if (str_equals(cmd, "tasklist")) {
        for (uint32_t i = 0; i < PROCESS_CONTEXT_MAX; i++) {
            terminal_write("PID ");
            terminal_write_dec_u32(process_contexts[i].pid);
            terminal_write(": ");
            terminal_write(task_state_name(process_contexts[i].task_state));
            if (process_contexts[i].task_state == TASK_STATE_BLOCKED) {
                terminal_write("(");
                terminal_write(wait_reason_name(process_contexts[i].wait_reason));
                if (process_contexts[i].wait_timeout_tick != 0) {
                    terminal_write("@t");
                    terminal_write_dec_u32(process_contexts[i].wait_timeout_tick);
                }
                terminal_write(")");
            }
            terminal_write(process_contexts[i].user_ready ? " userctx" : " nouser");
            terminal_write(" cwd=");
            mbfs_terminal_write_path(process_contexts[i].cwd);
            if (i == current_process_slot) {
                terminal_write(" [current]");
            }
            terminal_write("\n");
        }
        return;
    }

    if (str_equals(cmd, taskstats_cmd)) {
        task_print_state_counts();
        return;
    }

    if (str_equals(cmd, waitq_cmd)) {
        task_print_wait_queues();
        return;
    }

    if (str_starts_with(cmd, taskspawn_prefix)) {
        uint32_t pid;
        if (!parse_u32_decimal(cmd + str_length(taskspawn_prefix), &pid) || pid == 0 || pid > PROCESS_CONTEXT_MAX) {
            terminal_write("Usage: taskspawn <1-");
            terminal_write_dec_u32(PROCESS_CONTEXT_MAX);
            terminal_write(">\n");
            return;
        }

        user_test_prepare_pid(pid);
        process_set_task_state(&process_contexts[pid - 1u], TASK_STATE_READY);
        process_contexts[pid - 1u].sleep_until_tick = 0;
        process_contexts[pid - 1u].wait_timeout_tick = 0;
        process_contexts[pid - 1u].wait_event_channel = 0;
        process_contexts[pid - 1u].wait_reason = WAIT_REASON_NONE;
        if (!process_seed_snapshot_from_user_context(&process_contexts[pid - 1u])) {
            terminal_write("taskspawn warning: snapshot seed failed\n");
            return;
        }

        terminal_write("Spawned task PID ");
        terminal_write_dec_u32(pid);
        terminal_write(" with seeded user snapshot\n");
        return;
    }

    if (str_starts_with(cmd, taskteardown_prefix)) {
        uint32_t pid;
        if (!parse_u32_decimal(cmd + str_length(taskteardown_prefix), &pid) || pid == 0 || pid > PROCESS_CONTEXT_MAX) {
            terminal_write("Usage: taskteardown <1-");
            terminal_write_dec_u32(PROCESS_CONTEXT_MAX);
            terminal_write(">\n");
            return;
        }

        task_teardown_pid(pid);
        return;
    }

    if (str_starts_with(cmd, pageown_prefix)) {
        uint32_t pid;
        if (!parse_u32_decimal(cmd + str_length(pageown_prefix), &pid) || pid == 0 || pid > PROCESS_CONTEXT_MAX) {
            terminal_write("Usage: pageown <1-");
            terminal_write_dec_u32(PROCESS_CONTEXT_MAX);
            terminal_write(">\n");
            return;
        }

        task_print_page_ownership(pid);
        return;
    }

    if (str_starts_with(cmd, tasksleep_prefix)) {
        uint32_t ticks_sleep;
        uint32_t eax;
        uint32_t ebx;

        if (!parse_u32_decimal(cmd + str_length(tasksleep_prefix), &ticks_sleep) || ticks_sleep == 0) {
            terminal_write("Usage: tasksleep <decimal-ticks>\n");
            return;
        }

        eax = 5u;
        ebx = ticks_sleep;
        __asm__ __volatile__("int $0x80" : "+a"(eax) : "b"(ebx) : "memory");
        terminal_write("tasksleep syscall ret=");
        terminal_write_hex(eax);
        terminal_write("\n");
        return;
    }

    if (str_starts_with(cmd, taskwait_prefix)) {
        uint32_t ticks_wait;
        struct process_syscall_context* proc = current_process_context();

        if (!parse_u32_decimal(cmd + str_length(taskwait_prefix), &ticks_wait) || ticks_wait == 0) {
            terminal_write("Usage: taskwait <decimal-ticks>\n");
            return;
        }

        if (runnable_process_count() <= 1u) {
            terminal_write("Cannot block last runnable task\n");
            return;
        }

        process_block_with_reason(proc, WAIT_REASON_MANUAL, 0);
        proc->wait_timeout_tick = timer_ticks + ticks_wait;
        (void)scheduler_rotate_process();
        terminal_write("Current PID blocked (manual) until tick ");
        terminal_write_dec_u32(proc->wait_timeout_tick);
        terminal_write("\n");
        return;
    }

    if (str_starts_with(cmd, taskwake_prefix)) {
        uint32_t pid;
        struct process_syscall_context* target;

        if (!parse_u32_decimal(cmd + str_length(taskwake_prefix), &pid) || pid == 0 || pid > PROCESS_CONTEXT_MAX) {
            terminal_write("Usage: taskwake <1-");
            terminal_write_dec_u32(PROCESS_CONTEXT_MAX);
            terminal_write(">\n");
            return;
        }

        target = &process_contexts[pid - 1u];
        if (target->task_state != TASK_STATE_BLOCKED) {
            terminal_write("Task is not blocked\n");
            return;
        }

        terminal_write("Waking blocked task reason=");
        terminal_write(wait_reason_name(target->wait_reason));
        terminal_write("\n");
        target->sleep_until_tick = 0;
        target->wait_timeout_tick = 0;
        target->wait_event_channel = 0;
        process_set_task_state(target, TASK_STATE_READY);
        terminal_write("Task PID ");
        terminal_write_dec_u32(pid);
        terminal_write(" woken to ready\n");
        return;
    }

    if (str_starts_with(cmd, eventwait_prefix)) {
        uint32_t channel;
        uint32_t eax;
        uint32_t ebx;

        if (!parse_u32_decimal(cmd + str_length(eventwait_prefix), &channel)) {
            terminal_write("Usage: eventwait <channel>\n");
            return;
        }

        eax = 6u;
        ebx = channel;
        __asm__ __volatile__("int $0x80" : "+a"(eax) : "b"(ebx) : "memory");
        terminal_write("eventwait syscall ret=");
        terminal_write_hex(eax);
        terminal_write("\n");
        return;
    }

    if (str_starts_with(cmd, eventsig_prefix)) {
        uint32_t channel;
        uint32_t eax;
        uint32_t ebx;

        if (!parse_u32_decimal(cmd + str_length(eventsig_prefix), &channel)) {
            terminal_write("Usage: eventsig <channel>\n");
            return;
        }

        eax = 7u;
        ebx = channel;
        __asm__ __volatile__("int $0x80" : "+a"(eax) : "b"(ebx) : "memory");
        terminal_write("eventsig woke=");
        terminal_write_dec_u32(eax);
        terminal_write("\n");
        return;
    }

    if (str_starts_with(cmd, taskrun_prefix)) {
        uint32_t pid;
        if (!parse_u32_decimal(cmd + str_length(taskrun_prefix), &pid) || pid == 0 || pid > PROCESS_CONTEXT_MAX) {
            terminal_write("Usage: taskrun <1-");
            terminal_write_dec_u32(PROCESS_CONTEXT_MAX);
            terminal_write(">\n");
            return;
        }

        process_set_task_state(&process_contexts[pid - 1u], TASK_STATE_READY);
        process_contexts[pid - 1u].sleep_until_tick = 0;
        process_contexts[pid - 1u].wait_timeout_tick = 0;
        process_contexts[pid - 1u].wait_event_channel = 0;
        process_contexts[pid - 1u].wait_reason = WAIT_REASON_NONE;
        terminal_write("Task PID ");
        terminal_write_dec_u32(pid);
        terminal_write(" marked ready\n");
        return;
    }

    if (str_starts_with(cmd, taskstop_prefix)) {
        uint32_t pid;
        if (!parse_u32_decimal(cmd + str_length(taskstop_prefix), &pid) || pid == 0 || pid > PROCESS_CONTEXT_MAX) {
            terminal_write("Usage: taskstop <1-");
            terminal_write_dec_u32(PROCESS_CONTEXT_MAX);
            terminal_write(">\n");
            return;
        }

        if (runnable_process_count() <= 1u && process_contexts[pid - 1u].runnable) {
            terminal_write("Refusing to stop last runnable task\n");
            return;
        }

        process_block_with_reason(&process_contexts[pid - 1u], WAIT_REASON_MANUAL, 0);
        process_contexts[pid - 1u].wait_timeout_tick = 0;
        process_contexts[pid - 1u].wait_event_channel = 0;
        if (current_process_slot == pid - 1u) {
            scheduler_rotate_process();
        }

        terminal_write("Task PID ");
        terminal_write_dec_u32(pid);
        terminal_write(" marked stopped\n");
        return;
    }

    if (str_equals(cmd, "snapinfo")) {
        process_print_snapshot(current_process_context());
        return;
    }

    if (str_starts_with(cmd, snapinfo_prefix)) {
        uint32_t pid;
        if (!parse_u32_decimal(cmd + str_length(snapinfo_prefix), &pid) || pid == 0 || pid > PROCESS_CONTEXT_MAX) {
            terminal_write("Usage: snapinfo [1-");
            terminal_write_dec_u32(PROCESS_CONTEXT_MAX);
            terminal_write("]\n");
            return;
        }

        process_print_snapshot(&process_contexts[pid - 1u]);
        return;
    }

    if (str_equals(cmd, "snapnow")) {
        uint32_t eax = 1u;
        uint32_t ebx = 0u;
        __asm__ __volatile__("int $0x80" : "+a"(eax) : "b"(ebx) : "memory");
        terminal_write("Snapshot trap captured via int 0x80, ret=");
        terminal_write_hex(eax);
        terminal_write("\n");
        return;
    }

    if (str_equals(cmd, "snapseed")) {
        if (process_seed_snapshot_from_user_context(current_process_context())) {
            terminal_write("Seeded current PID snapshot from prepared user context\n");
        } else {
            terminal_write("Run usertestprep first\n");
        }
        return;
    }

    if (str_starts_with(cmd, snapseed_prefix)) {
        uint32_t pid;
        if (!parse_u32_decimal(cmd + str_length(snapseed_prefix), &pid) || pid == 0 || pid > PROCESS_CONTEXT_MAX) {
            terminal_write("Usage: snapseed [1-");
            terminal_write_dec_u32(PROCESS_CONTEXT_MAX);
            terminal_write("]\n");
            return;
        }

        if (process_seed_snapshot_from_user_context(&process_contexts[pid - 1u])) {
            terminal_write("Seeded PID ");
            terminal_write_dec_u32(pid);
            terminal_write(" snapshot from prepared user context\n");
        } else {
            terminal_write("Run usertestprep first\n");
        }
        return;
    }

    if (str_equals(cmd, "snaprestore")) {
        struct process_syscall_context* proc = current_process_context();
        if (!proc->user_ready) {
            terminal_write("Run usertestprep first\n");
            return;
        }

        if (process_restore_snapshot_to_user_context(proc)) {
            terminal_write("User test context restored from current PID snapshot\n");
            terminal_write("New EIP: ");
            terminal_write_hex(proc->user_eip);
            terminal_write("\nNew ESP: ");
            terminal_write_hex(proc->user_esp);
            terminal_write("\n");
        } else {
            terminal_write("Snapshot restore rejected (need user-mode snapshot with valid EIP/ESP)\n");
        }
        return;
    }

    if (str_equals(cmd, "ctxswon")) {
        scheduler_frame_restore_enabled = 1;
        terminal_write("Frame restore switching enabled\n");
        return;
    }

    if (str_equals(cmd, "ctxswoff")) {
        scheduler_frame_restore_enabled = 0;
        terminal_write("Frame restore switching disabled\n");
        return;
    }

    if (str_equals(cmd, "ctxswstat")) {
        terminal_write("Frame restore mode: ");
        terminal_write(scheduler_frame_restore_enabled ? "on" : "off");
        terminal_write("\nRestores: ");
        terminal_write_dec_u32(scheduler_frame_restore_count);
        terminal_write("\nRejects: ");
        terminal_write_dec_u32(scheduler_frame_restore_reject_count);
        terminal_write("\n");
        return;
    }

    if (str_starts_with(cmd, snaprestore_prefix)) {
        uint32_t pid;
        if (!parse_u32_decimal(cmd + str_length(snaprestore_prefix), &pid) || pid == 0 || pid > PROCESS_CONTEXT_MAX) {
            terminal_write("Usage: snaprestore [1-");
            terminal_write_dec_u32(PROCESS_CONTEXT_MAX);
            terminal_write("]\n");
            return;
        }

        if (!process_contexts[pid - 1u].user_ready) {
            terminal_write("Run usertestprep first\n");
            return;
        }

        if (process_restore_snapshot_to_user_context(&process_contexts[pid - 1u])) {
            terminal_write("User test context restored from PID ");
            terminal_write_dec_u32(pid);
            terminal_write(" snapshot\nNew EIP: ");
            terminal_write_hex(process_contexts[pid - 1u].user_eip);
            terminal_write("\nNew ESP: ");
            terminal_write_hex(process_contexts[pid - 1u].user_esp);
            terminal_write("\n");
        } else {
            terminal_write("Snapshot restore rejected (need user-mode snapshot with valid EIP/ESP)\n");
        }
        return;
    }

    if (str_equals(cmd, "sched")) {
        scheduler_print_status();
        return;
    }

    if (str_equals(cmd, "taskstats")) {
        process_print_stats();
        return;
    }

    if (str_equals(cmd, "schedon")) {
        scheduler_enabled = 1;
        scheduler_tick_accumulator = 0;
        terminal_write("Scheduler enabled\n");
        return;
    }

    if (str_equals(cmd, "schedoff")) {
        scheduler_enabled = 0;
        scheduler_tick_accumulator = 0;
        terminal_write("Scheduler disabled\n");
        return;
    }

    if (str_starts_with(cmd, schedq_prefix)) {
        uint32_t ticks;
        if (!parse_u32_decimal(cmd + str_length(schedq_prefix), &ticks) || ticks == 0) {
            terminal_write("Usage: schedq <decimal-ticks>\n");
            return;
        }
        scheduler_quantum_ticks = ticks;
        scheduler_tick_accumulator = 0;
        terminal_write("Scheduler quantum set to ");
        terminal_write_dec_u32(scheduler_quantum_ticks);
        terminal_write(" ticks\n");
        return;
    }

    if (str_equals(cmd, "meminfo")) {
        multiboot_print_memory_summary();
        return;
    }

    if (str_equals(cmd, "memmap")) {
        multiboot_print_memory_map();
        return;
    }

    if (str_equals(cmd, "allocstat")) {
        physical_allocator_print_stats();
        return;
    }

    if (str_equals(cmd, "allocpage")) {
        uint32_t page = physical_allocator_alloc_page();
        if (page == 0) {
            terminal_write("Allocator: out of pages\n");
        } else {
            terminal_write("Allocated page at ");
            terminal_write_hex(page);
            terminal_write("\n");
        }
        return;
    }

    if (str_equals(cmd, "allocstress")) {
        physical_allocator_stress_test();
        return;
    }

    if (str_equals(cmd, "idmap")) {
        paging_print_identity_map();
        return;
    }

    if (str_equals(cmd, "trimid")) {
        paging_trim_identity(PAGING_MIN_KEEP_BYTES);
        return;
    }

    if (str_starts_with(cmd, trimid_prefix)) {
        uint32_t megabytes;
        if (!parse_u32_decimal(cmd + str_length(trimid_prefix), &megabytes) || megabytes == 0) {
            terminal_write("Usage: trimid <decimal-megabytes>\n");
            return;
        }
        paging_trim_identity(megabytes * 1024u * 1024u);
        return;
    }

    if (str_equals(cmd, "uspace")) {
        paging_print_user_layout();
        return;
    }

    if (str_equals(cmd, "mapusertest")) {
        paging_map_user_test_page();
        return;
    }

    if (str_equals(cmd, "lookuserlast")) {
        if (paging_last_user_virt == 0) {
            terminal_write("No user test page mapped yet\n");
        } else {
            paging_print_lookup(paging_last_user_virt);
        }
        return;
    }

    if (str_equals(cmd, "usertestprep")) {
        user_test_prepare();
        return;
    }

    if (str_equals(cmd, "userctx")) {
        user_test_print_context();
        return;
    }

    if (str_equals(cmd, "userrun")) {
        struct process_syscall_context* proc = current_process_context();
        if (!proc->user_ready) {
            terminal_write("Prepare test first with 'usertestprep'\n");
            return;
        }

        terminal_write("Entering ring3 test context (non-returning)\n");
        user_enter_ring3(proc->user_eip, proc->user_esp);
        return;
    }

    if (str_equals(cmd, "pgstat")) {
        paging_print_stats();
        return;
    }

    if (str_equals(cmd, "pgdir")) {
        paging_print_directory();
        return;
    }

    if (str_equals(cmd, "pgmaptest")) {
        paging_map_test_page();
        return;
    }

    if (str_equals(cmd, "pglooklast")) {
        if (paging_last_mapped_virt == 0) {
            terminal_write("No test page mapped yet\n");
        } else {
            paging_print_lookup(paging_last_mapped_virt);
        }
        return;
    }

    if (str_equals(cmd, "virtlayout")) {
        paging_print_virtual_layout();
        return;
    }

    if (str_equals(cmd, "kheap")) {
        kernel_heap_print_stats();
        return;
    }

    if (str_starts_with(cmd, kmalloc_prefix)) {
        kernel_heap_alloc_test(cmd + str_length(kmalloc_prefix));
        return;
    }

    terminal_write("Unknown command: ");
    terminal_write(cmd);
    terminal_write("\n");
}

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt[num].base_low = (uint16_t)(base & 0xFFFF);
    gdt[num].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt[num].base_high = (uint8_t)((base >> 24) & 0xFF);

    gdt[num].limit_low = (uint16_t)(limit & 0xFFFF);
    gdt[num].granularity = (uint8_t)((limit >> 16) & 0x0F);
    gdt[num].granularity |= (uint8_t)(gran & 0xF0);
    gdt[num].access = access;
}

static void tss_init(uint32_t kernel_stack)
{
    memory_set_u8(&tss, 0, sizeof(tss));
    tss.ss0 = 0x10;
    tss.esp0 = kernel_stack;
    tss.cs = 0x1B;
    tss.ss = 0x23;
    tss.ds = 0x23;
    tss.es = 0x23;
    tss.fs = 0x23;
    tss.gs = 0x23;
    tss.iomap_base = sizeof(tss);

    gdt_set_gate(5, (uint32_t)(uintptr_t)&tss, sizeof(tss) - 1u, 0x89, 0x40);
}

static void user_enter_ring3(uint32_t entry_eip, uint32_t user_stack)
{
    __asm__ __volatile__(
        "cli\n"
        "mov $0x23, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "pushl $0x23\n"
        "pushl %0\n"
        "pushfl\n"
        "pushl $0x1B\n"
        "pushl %1\n"
        "iretl\n"
        :
        : "r"(user_stack), "r"(entry_eip)
        : "memory", "ax");
}

static void gdt_init(void)
{
    uint32_t kernel_stack;

    gp.limit = (uint16_t)(sizeof(struct gdt_entry) * 6 - 1);
    gp.base = (uint32_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    __asm__ __volatile__("mov %%esp, %0" : "=r"(kernel_stack));
    tss_init(kernel_stack);

    gdt_flush((uint32_t)&gp);
    tss_flush(0x2B);
}

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t selector, uint8_t flags)
{
    idt[num].base_low = (uint16_t)(base & 0xFFFF);
    idt[num].base_high = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].selector = selector;
    idt[num].zero = 0;
    idt[num].flags = flags;
}

static void pic_remap(void)
{
    outb(0x20, 0x11);
    io_wait();
    outb(0xA0, 0x11);
    io_wait();

    outb(0x21, 0x20);
    io_wait();
    outb(0xA1, 0x28);
    io_wait();

    outb(0x21, 0x04);
    io_wait();
    outb(0xA1, 0x02);
    io_wait();

    outb(0x21, 0x01);
    io_wait();
    outb(0xA1, 0x01);
    io_wait();

    outb(0x21, 0x0);
    outb(0xA1, 0x0);
}

static uint8_t ps2_wait_write_ready(void)
{
    for (uint32_t i = 0; i < 100000u; i++) {
        if ((inb(0x64) & 0x02u) == 0u) {
            return 1u;
        }
        io_wait();
    }
    return 0u;
}

static uint8_t ps2_wait_read_ready(void)
{
    for (uint32_t i = 0; i < 100000u; i++) {
        if ((inb(0x64) & 0x01u) != 0u) {
            return 1u;
        }
        io_wait();
    }
    return 0u;
}

static void mouse_write_device(uint8_t value)
{
    if (!ps2_wait_write_ready()) { return; }
    outb(0x64, 0xD4);
    if (!ps2_wait_write_ready()) { return; }
    outb(0x60, value);
}

static uint8_t mouse_read_data(void)
{
    if (!ps2_wait_read_ready()) {
        return 0u;
    }
    return inb(0x60);
}

static void mouse_init_hw(void)
{
    uint8_t config;
    if (dbg_emit_serial) { serial_write("PS2: mouse_init_hw start\n"); }

    if (!ps2_wait_write_ready()) { if (dbg_emit_serial) { serial_write("PS2: wait_write_ready failed (A)\n"); } return; }
    outb(0x64, 0xA8); /* Enable PS/2 auxiliary device (mouse). */
    if (dbg_emit_serial) { serial_write("PS2: cmd 0xA8 sent\n"); }

    if (!ps2_wait_write_ready()) { if (dbg_emit_serial) { serial_write("PS2: wait_write_ready failed (B)\n"); } return; }
    outb(0x64, 0x20); /* Read controller config byte. */
    config = mouse_read_data();
    if (dbg_emit_serial) { serial_write("PS2: cfg read="); serial_write_hex((uint32_t)config); serial_write("\n"); }
    config |= 0x02u; /* Enable IRQ12. */
    config &= (uint8_t)~0x20u; /* Ensure auxiliary clock is enabled. */

    if (!ps2_wait_write_ready()) { if (dbg_emit_serial) { serial_write("PS2: wait_write_ready failed (C)\n"); } return; }
    outb(0x64, 0x60); /* Write controller config byte. */
    if (dbg_emit_serial) { serial_write("PS2: cmd 0x60 sent\n"); }
    if (!ps2_wait_write_ready()) { if (dbg_emit_serial) { serial_write("PS2: wait_write_ready failed (D)\n"); } return; }
    outb(0x60, config);
    if (dbg_emit_serial) { serial_write("PS2: cfg wrote="); serial_write_hex((uint32_t)config); serial_write("\n"); }

    mouse_write_device(0xF6); /* Set defaults. */
    if (ps2_wait_read_ready()) {
        uint8_t r = mouse_read_data();
        if (dbg_emit_serial) { serial_write("PS2: mouse resp to 0xF6="); serial_write_hex((uint32_t)r); serial_write("\n"); }
    } else {
        if (dbg_emit_serial) { serial_write("PS2: no resp for 0xF6\n"); }
    }

    mouse_write_device(0xF4); /* Enable data reporting. */
    if (ps2_wait_read_ready()) {
        uint8_t r2 = mouse_read_data();
        if (dbg_emit_serial) { serial_write("PS2: mouse resp to 0xF4="); serial_write_hex((uint32_t)r2); serial_write("\n"); }
    } else {
        if (dbg_emit_serial) { serial_write("PS2: no resp for 0xF4\n"); }
    }

    mouse_cycle = 0;
    mouse_buttons = 0;
    mouse_enabled = 1;
    if (dbg_emit_serial) { serial_write("PS2: mouse_init_hw done, mouse_enabled=1\n"); }
}

static void pit_init(uint32_t frequency_hz)
{
    uint32_t divisor = 1193180 / frequency_hz;

    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

static void idt_init(void)
{
    for (size_t i = 0; i < 256; i++) {
        idt_set_gate((uint8_t)i, 0, 0, 0);
    }

    idt_set_gate(0, (uint32_t)isr0, 0x08, 0x8E);
    idt_set_gate(1, (uint32_t)isr1, 0x08, 0x8E);
    idt_set_gate(2, (uint32_t)isr2, 0x08, 0x8E);
    idt_set_gate(3, (uint32_t)isr3, 0x08, 0x8E);
    idt_set_gate(4, (uint32_t)isr4, 0x08, 0x8E);
    idt_set_gate(5, (uint32_t)isr5, 0x08, 0x8E);
    idt_set_gate(6, (uint32_t)isr6, 0x08, 0x8E);
    idt_set_gate(7, (uint32_t)isr7, 0x08, 0x8E);
    idt_set_gate(8, (uint32_t)isr8, 0x08, 0x8E);
    idt_set_gate(9, (uint32_t)isr9, 0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint32_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint32_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint32_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint32_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint32_t)isr31, 0x08, 0x8E);
    idt_set_gate(128, (uint32_t)isr128, 0x08, 0xEE);

    idt_set_gate(32, (uint32_t)irq0, 0x08, 0x8E);
    idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E);
    idt_set_gate(34, (uint32_t)irq2, 0x08, 0x8E);
    idt_set_gate(35, (uint32_t)irq3, 0x08, 0x8E);
    idt_set_gate(36, (uint32_t)irq4, 0x08, 0x8E);
    idt_set_gate(37, (uint32_t)irq5, 0x08, 0x8E);
    idt_set_gate(38, (uint32_t)irq6, 0x08, 0x8E);
    idt_set_gate(39, (uint32_t)irq7, 0x08, 0x8E);
    idt_set_gate(40, (uint32_t)irq8, 0x08, 0x8E);
    idt_set_gate(41, (uint32_t)irq9, 0x08, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);

    idtp.limit = (uint16_t)(sizeof(struct idt_entry) * 256 - 1);
    idtp.base = (uint32_t)&idt;
    idt_load((uint32_t)&idtp);
}

static void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(0xA0, 0x20);
    }
    outb(0x20, 0x20);
}

static char scancode_to_ascii(uint8_t scancode)
{
    static const char map[128] = {
        0,  27, '1', '2', '3', '4', '5', '6',
        '7', '8', '9', '0', '-', '=', '\b', '\t',
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
        'o', 'p', '[', ']', '\n', 0,  'a', 's',
        'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
        '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v',
        'b', 'n', 'm', ',', '.', '/', 0,  '*',
        0,  ' ', 0,  0,   0,   0,   0,   0,
        0,  0,   0,  0,   0,   0,   0,   0,
        0,  0,   0,  0,   0,   0,   0,   0,
        0,  0,   0,  '-', 0,   0,   0,   '+',
        0,  0,   0,  0,   0,   0,   0,   0,
        0,  0,   0,  0,   0,   0,   0,   0,
        0,  0,   0,  0,   0,   0,   0,   0,
        0,  0,   0,  0,   0,   0,   0,   0,
        0,  0,   0,  0,   0,   0,   0,   0
    };

    if (scancode < 128) {
        return map[scancode];
    }
    return 0;
}

void isr_handler(struct interrupt_frame* frame)
{
    if (frame->int_no == 128) {
        struct process_syscall_context* proc = current_process_context();
        uint32_t ret = 0xFFFFFFFFu;
        uint8_t restored_frame = 0;
        process_capture_snapshot(proc, frame);
        syscall_count++;
        proc->syscall_count++;

        syscall_last_num = frame->eax;
        syscall_last_arg0 = frame->ebx;
        proc->last_num = frame->eax;
        proc->last_arg0 = frame->ebx;

        if (frame->eax == 1u) {
            ret = 0x4D426F73u;
        } else if (frame->eax == 2u) {
            uint8_t c = (uint8_t)(frame->ebx & 0xFFu);
            if ((c >= 32u && c <= 126u) || c == '\n' || c == '\t') {
                terminal_putchar((char)c);
                ret = 1u;
            } else {
                ret = 0xFFFFFFFCu;
            }
        } else if (frame->eax == 3u) {
            restored_frame = (uint8_t)scheduler_rotate_and_maybe_restore(frame);
            ret = 0u;
        } else if (frame->eax == 4u) {
            user_exit_count++;
            user_last_exit_code = frame->ebx;
            proc->exit_count++;
            proc->last_exit_code = frame->ebx;
            process_close_all_fds(proc);
            process_unmap_user_pages(proc);
            proc->user_ready = 0;
            proc->user_eip = 0;
            proc->user_esp = 0;
            proc->snapshot_valid = 0;
            process_set_task_state(proc, TASK_STATE_EXITED);
            proc->sleep_until_tick = 0;
            proc->wait_timeout_tick = 0;
            proc->wait_event_channel = 0;
            proc->wait_reason = WAIT_REASON_NONE;
            restored_frame = (uint8_t)scheduler_rotate_and_maybe_restore(frame);
            ret = 0u;
        } else if (frame->eax == 5u) {
            uint32_t duration = frame->ebx;
            if (duration == 0) {
                duration = 1u;
            }

            if (runnable_process_count() <= 1u) {
                ret = 0xFFFFFFFBu;
            } else {
                process_block_with_reason(proc, WAIT_REASON_SLEEP, timer_ticks + duration);
                proc->wait_timeout_tick = 0;
                proc->wait_event_channel = 0;
                restored_frame = (uint8_t)scheduler_rotate_and_maybe_restore(frame);
                ret = 0u;
            }
        } else if (frame->eax == 6u) {
            if (runnable_process_count() <= 1u) {
                ret = 0xFFFFFFFBu;
            } else {
                process_block_with_reason(proc, WAIT_REASON_EVENT, 0);
                proc->wait_event_channel = frame->ebx;
                restored_frame = (uint8_t)scheduler_rotate_and_maybe_restore(frame);
                ret = 0u;
            }
        } else if (frame->eax == 7u) {
            ret = scheduler_signal_event_channel(frame->ebx);
        } else if (frame->eax == 8u) {
            char name[MBFS_PATH_MAX];
            uint8_t flags = (uint8_t)(frame->ecx & (RAMFS_OPEN_READ | RAMFS_OPEN_WRITE));
            int32_t fd;

            if (!user_copy_in_cstr(name, MBFS_PATH_MAX, frame->ebx) || name[0] == '\0') {
                ret = 0xFFFFFFFCu;
            } else {
                fd = process_fd_open(proc, name, flags);
                if (fd >= 0) {
                    ret = (uint32_t)fd;
                } else {
                    ret = 0xFFFFFFFAu;
                }
            }
        } else if (frame->eax == 9u) {
            uint8_t tmp[256];
            uint32_t fd = frame->ebx;
            uint32_t max_bytes = frame->edx;
            int32_t n;

            if (max_bytes > sizeof(tmp)) {
                max_bytes = sizeof(tmp);
            }

            n = process_fd_read(proc, fd, tmp, max_bytes);
            if (n < 0) {
                ret = 0xFFFFFFFCu;
            } else if ((uint32_t)n > 0u && !user_copy_out(frame->ecx, tmp, (uint32_t)n)) {
                ret = 0xFFFFFFFCu;
            } else {
                ret = (uint32_t)n;
            }
        } else if (frame->eax == 10u) {
            char tmp[256];
            uint32_t fd = frame->ebx;
            uint32_t count = frame->edx;
            int32_t n;

            if (count > sizeof(tmp)) {
                count = sizeof(tmp);
            }

            if (!user_copy_in(tmp, frame->ecx, count)) {
                ret = 0xFFFFFFFCu;
            } else {
                n = process_fd_write(proc, fd, (const uint8_t*)tmp, count);
                ret = (n < 0) ? 0xFFFFFFFCu : (uint32_t)n;
            }
        } else if (frame->eax == 11u) {
            ret = process_fd_close(proc, frame->ebx) ? 0u : 0xFFFFFFFCu;
        } else if (frame->eax == 12u) {
            int32_t off = process_fd_seek(proc, frame->ebx, frame->ecx);
            ret = (off < 0) ? 0xFFFFFFFCu : (uint32_t)off;
        } else if (frame->eax == 13u) {
            uint32_t fd = frame->ebx;
            if (fd >= PROCESS_FD_MAX || !proc->fds[fd].used || proc->fds[fd].file_index < 0) {
                ret = 0xFFFFFFFCu;
            } else if (proc->fds[fd].backend == FD_BACKEND_MBFS) {
                if ((uint32_t)proc->fds[fd].file_index >= MBFS_MAX_INODES) {
                    ret = 0xFFFFFFFCu;
                } else {
                    ret = mbfs_file_size_by_inode_index((uint8_t)proc->fds[fd].file_index);
                }
            } else {
                struct ramfs_file* file = &ramfs_files[(uint32_t)proc->fds[fd].file_index];
                ret = file->used ? file->size : 0xFFFFFFFCu;
            }
        } else if (frame->eax == 14u) {
            if (frame->ebx == 0u) {
                ret = proc->user_heap_brk;
            } else {
                ret = process_set_user_brk(proc, frame->ebx) ? proc->user_heap_brk : 0xFFFFFFFCu;
            }
        } else if (frame->eax == 15u) {
            ret = timer_ticks;
        } else {
            ret = 0xFFFFFFFDu;
        }

        if (!restored_frame) {
            frame->eax = ret;
        }
        syscall_last_ret = ret;
        proc->last_ret = ret;
        return;
    }

    terminal_write("\n[EXCEPTION] vector=");
    terminal_write_hex(frame->int_no);
    terminal_write(" err=");
    terminal_write_hex(frame->err_code);

    /* Mirror exception header to serial for headless debugging */
    serial_write("\n[EXCEPTION] vector="); serial_write_hex(frame->int_no);
    serial_write(" err="); serial_write_hex(frame->err_code);

    if (frame->int_no == 14u) {
        uint32_t cr2 = read_cr2();
        uint32_t dir_idx = cr2 >> 22;
        uint32_t tbl_idx = (cr2 >> 12) & 0x3FFu;
        terminal_write("\nPage fault at CR2=");
        terminal_write_hex(cr2);
        terminal_write(" (dir=");
        terminal_write_dec_u32(dir_idx);
        terminal_write(" tbl=");
        terminal_write_dec_u32(tbl_idx);
        terminal_write(")");
        terminal_write("\nPF flags: present=");
        terminal_write((frame->err_code & 0x1u) ? "yes" : "no");
        terminal_write(" write=");
        terminal_write((frame->err_code & 0x2u) ? "yes" : "no");
        terminal_write(" user=");
        terminal_write((frame->err_code & 0x4u) ? "yes" : "no");
        terminal_write(" rsvd=");
        terminal_write((frame->err_code & 0x8u) ? "yes" : "no");
        terminal_write(" instr=");
        terminal_write((frame->err_code & 0x10u) ? "yes" : "no");

        /* Mirror page-fault details to serial as well */
        serial_write("\nPage fault at CR2="); serial_write_hex(cr2);
        serial_write(" (dir="); serial_write_dec_u32(dir_idx);
        serial_write(" tbl="); serial_write_dec_u32(tbl_idx); serial_write(")\n");
        serial_write("PF flags: present="); serial_write((frame->err_code & 0x1u) ? "yes" : "no");
        serial_write(" write="); serial_write((frame->err_code & 0x2u) ? "yes" : "no");
        serial_write(" user="); serial_write((frame->err_code & 0x4u) ? "yes" : "no");
        serial_write(" rsvd="); serial_write((frame->err_code & 0x8u) ? "yes" : "no");
        serial_write(" instr="); serial_write((frame->err_code & 0x10u) ? "yes" : "no");

        /* User-space page fault: isolate, kill faulting process and reschedule */
        if (frame->cs == 0x1Bu) {
            struct process_syscall_context* proc = current_process_context();
            terminal_write("\nUser-space fault — killing PID ");
            terminal_write_dec_u32(proc->pid);
            terminal_write(" (task isolated, kernel intact)\n");
            process_unmap_user_pages(proc);
            proc->user_ready = 0;
            proc->user_eip = 0;
            proc->user_esp = 0;
            process_close_all_fds(proc);
            proc->snapshot_valid = 0;
            proc->sleep_until_tick = 0;
            proc->wait_timeout_tick = 0;
            proc->wait_event_channel = 0;
            proc->wait_reason = WAIT_REASON_NONE;
            process_set_task_state(proc, TASK_STATE_EXITED);
            user_exit_count++;
            user_last_exit_code = 0xDEAD0000u; /* page fault kill sentinel */
            (void)scheduler_rotate_and_maybe_restore(frame);
            return;
        }
    }

    /* General user-space exception isolation (#GP, #UD, #OF, etc.) */
    if (frame->cs == 0x1Bu) {
        struct process_syscall_context* proc = current_process_context();
        terminal_write("\nUser-space exception #");
        terminal_write_dec_u32(frame->int_no);
        terminal_write(" — killing PID ");
        terminal_write_dec_u32(proc->pid);
        terminal_write(" (task isolated)\n");
        process_unmap_user_pages(proc);
        proc->user_ready = 0;
        proc->user_eip = 0;
        proc->user_esp = 0;
        process_close_all_fds(proc);
        proc->snapshot_valid = 0;
        proc->sleep_until_tick = 0;
        proc->wait_timeout_tick = 0;
        proc->wait_event_channel = 0;
        proc->wait_reason = WAIT_REASON_NONE;
        process_set_task_state(proc, TASK_STATE_EXITED);
        user_exit_count++;
        user_last_exit_code = 0xDEAD0000u | (frame->int_no & 0xFFu);
        (void)scheduler_rotate_and_maybe_restore(frame);
        return;
    }

    terminal_write("\nEIP=");
    terminal_write_hex(frame->eip);
    terminal_write(" CS=");
    terminal_write_hex(frame->cs);
    terminal_write("\nKernel halted.\n");

    /* Mirror EIP/CS to serial as well */
    serial_write("\nEIP="); serial_write_hex(frame->eip);
    serial_write(" CS="); serial_write_hex(frame->cs);
    serial_write("\nKernel halted.\n");

    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

void irq_handler(struct interrupt_frame* frame)
{
    uint8_t irq = (uint8_t)(frame->int_no - 32);

    if (frame->int_no == 32) {
        process_capture_snapshot(current_process_context(), frame);
        timer_ticks++;
        scheduler_wake_sleeping_tasks();
        scheduler_on_timer_tick(frame);
    }

    if (frame->int_no == 33) {
        uint8_t scancode = inb(0x60);

        if (scancode == 0xE0) {
            keyboard_extended = 1;
        } else if (keyboard_extended) {
            if ((scancode & 0x80) == 0) {
                if (scancode == 0x48) {
                    if (shell_history_count > 0) {
                        if (shell_history_browse < (int)(shell_history_count - 1)) {
                            shell_history_browse++;
                        }
                        shell_set_input(shell_history_get_from_newest((size_t)shell_history_browse));
                    }
                } else if (scancode == 0x4B) {
                    shell_move_cursor_left();
                } else if (scancode == 0x4D) {
                    shell_move_cursor_right();
                } else if (scancode == 0x50) {
                    if (shell_history_browse >= 0) {
                        shell_history_browse--;
                        if (shell_history_browse >= 0) {
                            shell_set_input(shell_history_get_from_newest((size_t)shell_history_browse));
                        } else {
                            shell_set_input("");
                        }
                    }
                }
                input_bridge_signal(scancode, 0);
                if (wm_enabled) {
                    wm_render_pending = 1;
                }
            }
            keyboard_extended = 0;
        } else if ((scancode & 0x80) == 0) {
            char c = scancode_to_ascii(scancode);

            /* If a GUI terminal window is focused, route keystrokes to its buffer */
            if (wm_enabled && wm_focused_index >= 0 && wm_focused_index < (int32_t)WM_MAX_WINDOWS
                && wm_windows[wm_focused_index].used
                && wm_windows[wm_focused_index].type == WM_TYPE_TERMINAL
                && wm_term_states[wm_focused_index].used) {
                struct wm_term_state* t = &wm_term_states[wm_focused_index];

                if (c == '\b') {
                    if (t->input_len > 0 && t->input_cursor > 0) {
                        for (uint32_t i = t->input_cursor - 1; i < t->input_len; i++) {
                            t->input[i] = t->input[i + 1];
                        }
                        t->input_cursor--;
                        t->input_len--;
                        t->input[t->input_len] = '\0';
                    }
                } else if (c == '\n') {
                    /* Defer command execution to main loop (avoid running heavy code in IRQ).
                     * If this terminal window is backed by an app_proc, enqueue the
                     * line into the process stdin; otherwise fall back to the old
                     * gui_pending_cmd behavior. */
                    t->input[t->input_len] = '\0';
                    uint32_t cid = wm_windows[wm_focused_index].content_id;
                    if (cid < APP_PROC_MAX && app_procs[cid].used) {
                        app_proc_enqueue_stdin_line((int)cid, t->input);
                        dbg_log_event(DBG_EV_IRQ_KEY_TO_APP, (int32_t)cid, 1);
                    } else {
                        /* Copy into pending buffer (bounded) */
                        uint32_t i = 0;
                        for (; i + 1 < SHELL_INPUT_MAX && i < t->input_len; i++) {
                            gui_pending_cmd[i] = t->input[i];
                        }
                        gui_pending_cmd[i] = '\0';
                        gui_pending_cmd_ready = 1;
                        dbg_log_event(DBG_EV_IRQ_KEY_TO_APP, -1, 1);
                    }
                    shell_history_browse = -1;
                    /* reset window input immediately so typing can continue */
                    t->input_len = 0;
                    t->input_cursor = 0;
                    t->input[0] = '\0';
                    /* Ensure main loop renders and processes pending command */
                    wm_render_pending = 1;
                } else if (c != 0) {
                    if (t->input_len < SHELL_INPUT_MAX - 1) {
                        for (uint32_t i = t->input_len; i > t->input_cursor; i--) {
                            t->input[i] = t->input[i - 1];
                        }
                        t->input[t->input_cursor] = c;
                        t->input_len++;
                        t->input_cursor++;
                        t->input[t->input_len] = '\0';
                    }
                }

                input_bridge_signal(scancode, (uint8_t)c);
                if (wm_enabled) { wm_render_pending = 1; }
            } else {
                /* Default console behavior */
                if (c == '\b') {
                    shell_backspace_char();
                } else if (c == '\n') {
                    terminal_putchar('\n');
                    shell_input[shell_input_len] = '\0';
                    shell_history_push(shell_input);
                    shell_history_browse = -1;
                    shell_execute_command(shell_input);
                    shell_print_prompt();
                } else if (c != 0) {
                    shell_insert_char(c);
                }

                input_bridge_signal(scancode, (uint8_t)c);
                if (wm_enabled) {
                    wm_render_pending = 1;
                }
            }
        }
    }

    if (frame->int_no == 44) {
        if (mouse_enabled) {
            uint8_t status = inb(0x64);
            if ((status & 0x01) != 0) {
                uint8_t data = inb(0x60);

                if (mouse_cycle == 0 && (data & 0x08u) == 0u) {
                    /* Resync packet stream if we lost alignment. */
                    pic_send_eoi(irq);
                    return;
                }

                mouse_packet[mouse_cycle] = data;
                mouse_cycle++;
                
                    if (mouse_cycle >= 3) {
                    mouse_cycle = 0;
                    
                    int8_t dx = (int8_t)mouse_packet[1];
                    int8_t dy = -(int8_t)mouse_packet[2];
                    
                    if ((int32_t)mouse_x + dx > 0) {
                        mouse_x += dx;
                    }
                    if ((int32_t)mouse_y + dy > 0) {
                        mouse_y += dy;
                    }
                    
                    mouse_buttons = mouse_packet[0] & 0x07;

                    /* Log mouse click/motion event (IRQ-level) */
                    dbg_log_event(DBG_EV_MOUSE_CLICK, (int32_t)mouse_x, (int32_t)mouse_y);

                    /* Serial/VGA debug: print packet bytes and key GUI/WM state when enabled */
                    if (dbg_emit_serial) {
                        serial_write("MOUSE IRQ: pkt=");
                        serial_write_hex((uint32_t)mouse_packet[0]); serial_write(" ");
                        serial_write_hex((uint32_t)mouse_packet[1]); serial_write(" ");
                        serial_write_hex((uint32_t)mouse_packet[2]); serial_write(", x=");
                        serial_write_dec_u32((uint32_t)mouse_x); serial_write(", y=");
                        serial_write_dec_u32((uint32_t)mouse_y); serial_write(", btn=");
                        serial_write_dec_u32((uint32_t)mouse_buttons);
                        serial_write(", gf_detected="); serial_write_dec_u32((uint32_t)gui_framebuffer_detected);
                        serial_write(", gf_mapped="); serial_write_dec_u32((uint32_t)gui_framebuffer_mapped);
                        serial_write(", wm_enabled="); serial_write_dec_u32((uint32_t)wm_enabled);
                        serial_write(", wm_render_pending="); serial_write_dec_u32((uint32_t)wm_render_pending);
                        serial_write("\n");
                    }

                    /* Also optionally print a VGA-visible line so digits are visible via VNC/SDL */
                    if (dbg_emit_serial) {
                        terminal_write("MOUSE VGA: pkt=");
                        terminal_write_hex((uint32_t)mouse_packet[0]); terminal_write(" ");
                        terminal_write_hex((uint32_t)mouse_packet[1]); terminal_write(" ");
                        terminal_write_hex((uint32_t)mouse_packet[2]); terminal_write(", x=");
                        terminal_write_dec_u32((uint32_t)mouse_x); terminal_write(", y=");
                        terminal_write_dec_u32((uint32_t)mouse_y); terminal_write(", btn=");
                        terminal_write_hex((uint32_t)mouse_buttons);
                        terminal_write("\n");
                    }

                    /* Clamp to screen */
                    if (gui_framebuffer_width > 0 && mouse_x >= gui_framebuffer_width) {
                        mouse_x = gui_framebuffer_width - 1u;
                    }
                    /* Allow cursor to move into the taskbar area (bottom of screen).
                     * Previously the cursor was clamped just above the taskbar which
                     * prevented interacting with taskbar elements. */
                    if (gui_framebuffer_height > 0 && mouse_y >= gui_framebuffer_height) {
                        mouse_y = gui_framebuffer_height - 1u;
                    }

                    uint8_t scene_changed = wm_handle_mouse();

                    if (wm_enabled) {
                        if (scene_changed) {
                            wm_render_pending = 1;
                        } else {
                            (void)wm_fast_cursor_move();
                        }
                    }
                }
            }
        }
    }

    pic_send_eoi(irq);
}

void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info)
{
    boot_multiboot_magic = multiboot_magic;
    boot_multiboot_info = (const struct multiboot_info*)(uintptr_t)multiboot_info;
    process_contexts_init();
    /* Initialize serial console early for debug output (visible with -serial stdio) */
    serial_init();

    terminal_clear();
    terminal_write("MBos booted from ASM + C kernel!\n");

    if (boot_multiboot_magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        terminal_write("Warning: invalid Multiboot magic ");
        terminal_write_hex(boot_multiboot_magic);
        terminal_write("\n");
    }

    gui_detect_framebuffer();

    gdt_init();
    pic_remap();
    pit_init(timer_hz);
    idt_init();
    mouse_init_hw();
    physical_allocator_init();
    paging_init();
    /* Run a small allocator/paging self-test to catch regressions early.
     * Only run when the physical allocator found usable memory regions. */
    if (physical_region_count > 0) {
        physical_allocator_selftest();
        if (auto_run_allocstress) {
            terminal_write("Auto-run: starting allocator stress-test\n");
            terminal_write("=== AUTO_ALLOCSTRESS START ===\n");
            physical_allocator_stress_test();
            terminal_write("=== AUTO_ALLOCSTRESS END ===\n");
        }
    } else {
        terminal_write("Allocator self-test skipped: no usable memory regions\n");
    }
    if (gui_framebuffer_detected) {
        (void)gui_map_framebuffer();
    }
    kernel_heap_init();
    if (gui_framebuffer_detected) {
        gui_backbuffer_init();
    }
    shell_runtime_init();
    ata_storage_init();
    /* Try to mount MBFS; optionally run development disk checks/repairs
     * and a small smoke-test automatically when `auto_run_disk_tests` is set. */
    int disk_mount_rc = diskfs_try_mount();
    if (disk_mount_rc == 1 && auto_run_disk_tests) {
        terminal_write("Auto-run: MBFS mounted; running quick check\n");
        int chk = diskfs_check();
        if (chk > 0) {
            terminal_write("Auto-run: issues detected; attempting repair\n");
            int reparc = diskfs_repair();
            if (reparc == 0) {
                terminal_write("Auto-run: repair succeeded; remounting\n");
                diskfs_umount();
                (void)diskfs_try_mount();
            } else {
                terminal_write("Auto-run: repair failed (rc="); terminal_write_dec_u32((uint32_t)reparc); terminal_write(")\n");
            }
        } else if (chk == 0) {
            terminal_write("Auto-run: diskcheck OK\n");
        }

        terminal_write("Auto-run: running disk smoke-test\n");
        diskfs_smoke_test();
        terminal_write("Auto-run: disk smoke-test complete\n");
    } else {
        if (auto_run_disk_tests && diskfs_device != 0) {
            terminal_write("Auto-run: MBFS not mounted; attempting format\n");
            int fmt_rc = diskfs_format();
            if (fmt_rc == 0) {
                terminal_write("Auto-run: format OK; mounting\n");
                if (diskfs_try_mount() == 1) {
                    terminal_write("Auto-run: mounted after format; running smoke-test\n");
                    diskfs_smoke_test();
                } else {
                    terminal_write("Auto-run: mount failed after format\n");
                }
            } else {
                terminal_write("Auto-run: format failed (rc="); terminal_write_dec_u32((uint32_t)fmt_rc); terminal_write(")\n");
            }
        } else {
            (void)disk_mount_rc; /* silence unused warning when flag disabled */
        }
    }
    (void)ramfs_write_text("README.TXT",
                           "MBos RAMFS online.\\n"
                           "Use fsls, fscat <name>, fswrite <name> <text>, fsrm <name>.");
    {
        static const uint8_t hello_app[] = {
            0xB8,0x02,0x00,0x00,0x00, 0xBB,0x41,0x00,0x00,0x00, 0xCD,0x80,
            0xB8,0x02,0x00,0x00,0x00, 0xBB,0x50,0x00,0x00,0x00, 0xCD,0x80,
            0xB8,0x02,0x00,0x00,0x00, 0xBB,0x50,0x00,0x00,0x00, 0xCD,0x80,
            0xB8,0x0A,0x00,0x00,0x00, 0xBB,0x00,0x00,0x00,0x00,
            0xB9,0x00,0x20,0x00,0x40, 0xBA,0x0F,0x00,0x00,0x00, 0xCD,0x80,
            0xB8,0x04,0x00,0x00,0x00, 0xBB,0x2A,0x00,0x00,0x00, 0xCD,0x80,
            0xEB,0xFE
        };
        (void)ramfs_write_bytes("HELLO.APP", hello_app, (uint32_t)sizeof(hello_app));
    }
    /* MBAPP v1 test app: prints "MBAP!" then exits */
    {
        /* Payload: syscalls to putchar + exit */
        static const uint8_t mbapp_payload[] = {
            0xB8,0x02,0x00,0x00,0x00, 0xBB,0x4D,0x00,0x00,0x00, 0xCD,0x80,  /* putchar('M') */
            0xB8,0x02,0x00,0x00,0x00, 0xBB,0x42,0x00,0x00,0x00, 0xCD,0x80,  /* putchar('B') */
            0xB8,0x02,0x00,0x00,0x00, 0xBB,0x41,0x00,0x00,0x00, 0xCD,0x80,  /* putchar('A') */
            0xB8,0x02,0x00,0x00,0x00, 0xBB,0x50,0x00,0x00,0x00, 0xCD,0x80,  /* putchar('P') */
            0xB8,0x02,0x00,0x00,0x00, 0xBB,0x21,0x00,0x00,0x00, 0xCD,0x80,  /* putchar('!') */
            0xB8,0x02,0x00,0x00,0x00, 0xBB,0x0A,0x00,0x00,0x00, 0xCD,0x80,  /* putchar('\\n') */
            0xB8,0x04,0x00,0x00,0x00, 0xBB,0x00,0x00,0x00,0x00, 0xCD,0x80,  /* exit(0) */
            0xEB,0xFE                                                           /* inf loop */
        };
        uint8_t mbapp_file[sizeof(struct mbapp_header) + sizeof(mbapp_payload)];
        struct mbapp_header* hdr = (struct mbapp_header*)mbapp_file;
        hdr->magic = 0x50504142u;
        hdr->version = 1u;
        hdr->flags = 0u;
        hdr->entry_off = 0u;
        hdr->image_size = (uint32_t)sizeof(mbapp_payload);
        for (uint32_t i = 0; i < (uint32_t)sizeof(mbapp_payload); i++) {
            mbapp_file[sizeof(struct mbapp_header) + i] = mbapp_payload[i];
        }
        (void)ramfs_write_bytes("TEST.mbapp", mbapp_file, (uint32_t)sizeof(mbapp_file));
    }
    /* PE32 test app: prints "PE32!" then exits */
    {
        /* Minimal PE32 executable for i386 - prints "PE32!" and exits */
        /* Code payload: putchar syscalls + exit */
        static const uint8_t pe32_code[] = {
            0xB8,0x02,0x00,0x00,0x00, 0xBB,0x50,0x00,0x00,0x00, 0xCD,0x80,  /* putchar('P') */
            0xB8,0x02,0x00,0x00,0x00, 0xBB,0x45,0x00,0x00,0x00, 0xCD,0x80,  /* putchar('E') */
            0xB8,0x02,0x00,0x00,0x00, 0xBB,0x33,0x00,0x00,0x00, 0xCD,0x80,  /* putchar('3') */
            0xB8,0x02,0x00,0x00,0x00, 0xBB,0x32,0x00,0x00,0x00, 0xCD,0x80,  /* putchar('2') */
            0xB8,0x02,0x00,0x00,0x00, 0xBB,0x21,0x00,0x00,0x00, 0xCD,0x80,  /* putchar('!') */
            0xB8,0x02,0x00,0x00,0x00, 0xBB,0x0A,0x00,0x00,0x00, 0xCD,0x80,  /* putchar('\\n') */
            0xB8,0x04,0x00,0x00,0x00, 0xBB,0x00,0x00,0x00,0x00, 0xCD,0x80,  /* exit(0) */
            0xEB,0xFE                                                           /* inf loop */
        };
        uint32_t code_size = sizeof(pe32_code);
        uint32_t code_aligned = (code_size + 511u) & ~511u;
        uint32_t total_size = 512u + 72u + code_aligned;
        uint8_t pe32_file[512 + 72 + 256];
        
        if (total_size <= (uint32_t)sizeof(pe32_file)) {
            struct pe32_dos_header* dos = (struct pe32_dos_header*)pe32_file;
            struct pe32_coff_header* coff;
            struct pe32_opt_header* opt;
            struct pe32_section_header* sec;
            uint32_t pe_off = 64u;
            
            /* DOS header */
            dos->dos_magic[0] = 'M';
            dos->dos_magic[1] = 'Z';
            dos->pe_offset = pe_off;
            for (uint32_t i = 0; i < 58u; i++) {
                dos->dos_reserved[i] = 0;
            }
            
            /* PE signature */
            pe32_file[pe_off] = 'P';
            pe32_file[pe_off + 1] = 'E';
            pe32_file[pe_off + 2] = 0;
            pe32_file[pe_off + 3] = 0;
            
            /* COFF header */
            coff = (struct pe32_coff_header*)&pe32_file[pe_off + 4u];
            coff->machine = PE32_I386_MACHINE;
            coff->num_sections = 1u;
            coff->timeDateStamp = 0;
            coff->pointerToSymbolTable = 0;
            coff->numSymbols = 0;
            coff->sizeOfOptionalHeader = (uint16_t)sizeof(struct pe32_opt_header);
            coff->characteristics = 0x0102u;
            
            /* Optional header */
            opt = (struct pe32_opt_header*)((uint8_t*)coff + sizeof(struct pe32_coff_header));
            opt->magic = PE32_MAGIC;
            opt->linkerVersion = 2;
            opt->linkerMinor = 0;
            opt->sizeOfCode = code_aligned;
            opt->sizeOfInitializedData = 0;
            opt->sizeOfUninitializedData = 0;
            opt->addressOfEntryPoint = 0x1000u;
            opt->baseOfCode = 0x1000u;
            opt->baseOfData = 0x2000u;
            opt->imageBase = 0x40000000u;
            opt->sectionAlignment = 0x1000u;
            opt->fileAlignment = 0x200u;
            opt->osVersionMajor = 4;
            opt->osVersionMinor = 0;
            opt->imageVersionMajor = 0;
            opt->imageVersionMinor = 0;
            opt->subsysVersionMajor = 4;
            opt->subsysVersionMinor = 0;
            opt->reservedForWin32 = 0;
            opt->sizeOfImage = 0x3000u;
            opt->sizeOfHeaders = 512u;
            opt->checkSum = 0;
            opt->subsystem = 3u;
            opt->dllCharacteristics = 0;
            opt->stackReserveSize = 0x100000u;
            opt->stackCommitSize = 0x1000u;
            opt->heapReserveSize = 0x100000u;
            opt->heapCommitSize = 0x1000u;
            opt->loaderFlags = 0;
            opt->numberOfRVAAndSizes = 16u;
            
            /* Section header */
            sec = (struct pe32_section_header*)((uint8_t*)opt + sizeof(struct pe32_opt_header));
            sec->name[0] = '.';
            sec->name[1] = 't';
            sec->name[2] = 'e';
            sec->name[3] = 'x';
            sec->name[4] = 't';
            sec->name[5] = 0;
            sec->name[6] = 0;
            sec->name[7] = 0;
            sec->virtualSize = code_size;
            sec->virtualAddress = 0x1000u;
            sec->sizeOfRawData = code_aligned;
            sec->pointerToRawData = 512u;
            sec->characteristics = 0x60000020u;
            for (uint32_t i = 0; i < 16u; i++) {
                sec->reserved[i] = 0;
            }
            
            /* Copy code section */
            for (uint32_t i = 0; i < code_size; i++) {
                pe32_file[512u + i] = pe32_code[i];
            }
            for (uint32_t i = code_size; i < code_aligned; i++) {
                pe32_file[512u + i] = 0;
            }
            
            (void)ramfs_write_bytes("TEST.exe", pe32_file, total_size);
        }
    }
    (void)ramfs_write_text("APPMSG.TXT", "Hello from RAMFS syscall file API\\n");

    terminal_write("GDT loaded, IDT active, PIC remapped.\n");
    terminal_write("PIT timer + keyboard IRQ active. Type 'help'.\n");
    if (multiboot_has_memory_map()) {
        terminal_write("Multiboot memory map detected. Try 'meminfo' or 'memmap'.\n");
        terminal_write("Physical allocator ready. Try 'allocstat' or 'allocpage'.\n");
        terminal_write("Paging ready. Try 'pgstat' or 'pgdir'.\n");
        terminal_write("Kernel heap ready. Try 'kheap' or 'kmalloc 64'.\n");
        terminal_write("User test groundwork ready. Try 'usertestprep' and 'userctx'.\n");
        terminal_write("Scheduler enabled. Use 'sched' to view status.\n");
        if (gui_framebuffer_detected) {
            terminal_write("Framebuffer detected. Try 'guistatus' and 'guitest'.\n");
            /* Auto-initialize GUI with default windows */
            wm_enabled = 1;
            mouse_enabled = 1;
            {
                /* Spawn an in-kernel terminal app and back the window with its pid.
                 * If spawning fails, fall back to a non-app-backed terminal window. */
                int pid = app_proc_spawn("terminal", terminal_app_step, NULL);
                int32_t tidx = wm_open_window(40, 50, 420, 260, "Terminal");
                if (tidx >= 0) {
                    wm_windows[tidx].type = (uint8_t)WM_TYPE_TERMINAL;
                    wm_windows[tidx].content_id = (uint32_t)((pid >= 0) ? pid : (int)tidx);
                    wm_term_states[tidx].used = 1;
                    wm_term_states[tidx].scroll = 0;
                    wm_term_states[tidx].input_len = 0;
                    wm_term_states[tidx].input_cursor = 0;
                    wm_term_states[tidx].input[0] = '\0';
                    if (pid < 0) {
                        terminal_write("Warning: failed to spawn terminal app_proc\n");
                    }
                } else {
                    if (pid >= 0) {
                        /* No window created — free the spawned proc */
                        app_procs[pid].used = 0;
                    }
                }
            }
            {
                int32_t fidx = wm_open_window(520, 120, 300, 210, "Files");
                if (fidx >= 0) {
                    wm_windows[fidx].type = (uint8_t)WM_TYPE_FILEMANAGER;
                    wm_windows[fidx].content_id = (uint32_t)fidx;
                    wm_fs_states[fidx].used = 1;
                    wm_fs_states[fidx].scroll = 0;
                    wm_fs_states[fidx].selected = 0;
                    wm_fs_states[fidx].path[0] = '\0';
                }
            }
            wm_render();
            terminal_write("Window manager active. Click to interact.\n");
        } else {
            terminal_write("No framebuffer reported. Boot GUI entry and run 'guistatus'.\n");
        }
    }
    shell_print_prompt();

    uint32_t wm_last_render_tick = timer_ticks;
    for (;;) {
        /* Handle any pending GUI actions deferred from IRQs (create windows/processes) */
        if (gui_pending_action != GUI_ACTION_NONE) {
            uint8_t act = gui_pending_action;
            char act_arg[MBFS_PATH_MAX];
            str_copy_bounded(act_arg, gui_pending_action_arg, sizeof(act_arg));
            gui_pending_action = GUI_ACTION_NONE;

            if (act == GUI_ACTION_OPEN_TERMINAL) {
                int pid = app_proc_spawn("terminal", terminal_app_step, NULL);
                dbg_log_event(DBG_EV_MAINLOOP_PROCESS_ACTION, act, pid);
                if (pid < 0) {
                    terminal_write("Max app processes reached\n");
                } else {
                    uint32_t ox = 80u + wm_window_count * 28u;
                    uint32_t oy = 80u + wm_window_count * 22u;
                    int32_t idx = wm_open_window((int32_t)ox, (int32_t)oy, 600u, 360u, "Terminal");
                    if (idx >= 0) {
                        wm_windows[idx].type = (uint8_t)WM_TYPE_TERMINAL;
                        wm_windows[idx].content_id = (uint32_t)pid;
                        wm_term_states[idx].used = 1;
                        wm_term_states[idx].scroll = 0;
                        wm_term_states[idx].input_len = 0;
                        wm_term_states[idx].input_cursor = 0;
                        wm_term_states[idx].input[0] = '\0';
                    } else {
                        terminal_write("Max windows reached\n");
                        app_procs[pid].used = 0; /* free proc */
                    }
                }
            } else if (act == GUI_ACTION_OPEN_FILEMANAGER) {
                uint32_t ox2 = 60u + wm_window_count * 30u;
                uint32_t oy2 = 60u + wm_window_count * 24u;
                int32_t idx2 = wm_open_window((int32_t)ox2, (int32_t)oy2, 480u, 300u, "MBFS");
                dbg_log_event(DBG_EV_MAINLOOP_PROCESS_ACTION, act, idx2);
                if (idx2 < 0) {
                    terminal_write("Max windows reached\n");
                } else {
                    wm_windows[idx2].type = (uint8_t)WM_TYPE_FILEMANAGER;
                    wm_windows[idx2].content_id = (uint32_t)idx2;
                    wm_fs_states[idx2].used = 1;
                    wm_fs_states[idx2].scroll = 0;
                    wm_fs_states[idx2].selected = 0;
                    str_copy_bounded(wm_fs_states[idx2].path, act_arg, sizeof(wm_fs_states[idx2].path));
                }
            }
        }

        /* Run any in-kernel app processes (terminal app, etc.) */
        app_proc_run_all();

        /* Process any pending GUI-terminal command enqueued from IRQ context.
         * This ensures command execution (which may call GUI and disk code)
         * runs outside the interrupt handler. */
        if (gui_pending_cmd_ready) {
            /* Move pending command to a local buffer to avoid races with IRQ writes */
            char local_cmd[SHELL_INPUT_MAX];
            uint32_t i = 0;
            while (i + 1 < SHELL_INPUT_MAX && gui_pending_cmd[i] != '\0') {
                local_cmd[i] = gui_pending_cmd[i];
                i++;
            }
            local_cmd[i] = '\0';
            gui_pending_cmd_ready = 0;

            if (!str_is_empty(local_cmd)) {
                shell_history_push(local_cmd);
            }
            /* Print newline (deferred from IRQ) so command output starts on new line */
            terminal_putchar('\n');
            shell_execute_command(local_cmd);
            shell_print_prompt();
            wm_render_pending = 1;
        }
        if (wm_enabled && wm_render_pending) {
            uint32_t now = timer_ticks;
            if ((uint32_t)(now - wm_last_render_tick) >= 2u) {
                wm_render_pending = 0;
                wm_last_render_tick = now;
                wm_render();
            }
        }

        /* Periodic status report (once per second) to aid headless debugging */
        if ((uint32_t)(timer_ticks - last_status_report_tick) >= 100u) {
            last_status_report_tick = timer_ticks;
            serial_write("STATUS: mouse_x="); serial_write_dec_u32((uint32_t)mouse_x);
            serial_write(", y="); serial_write_dec_u32((uint32_t)mouse_y);
            serial_write(", btn="); serial_write_hex((uint32_t)mouse_buttons);
            serial_write(", wm_enabled="); serial_write_dec_u32((uint32_t)wm_enabled);
            serial_write(", wm_render_pending="); serial_write_dec_u32((uint32_t)wm_render_pending);
            serial_write(", gf_mapped="); serial_write_dec_u32((uint32_t)gui_framebuffer_mapped);
            serial_write("\n");
        }

        /* Automatic simulated click sequence for headless testing:
         * Step 0: click Start button
         * Step 1: click Terminal menu item (deferred)
         */
        if (auto_simulate_click) {
            if (auto_simclick_step == 0 && timer_ticks >= 200u) {
                if (gui_framebuffer_mapped && wm_enabled) {
                    uint32_t cx = 12u;
                    uint32_t cy = (gui_framebuffer_height > 30u) ? (gui_framebuffer_height - 30u) : (gui_framebuffer_height / 2u);
                    mouse_x = cx; mouse_y = cy;
                    mouse_buttons |= 0x01u; /* press left */
                    dbg_log_event(DBG_EV_MOUSE_CLICK, (int32_t)mouse_x, (int32_t)mouse_y);
                    (void)wm_handle_mouse();
                    mouse_buttons &= (uint8_t)~0x01u; /* release */
                    (void)wm_handle_mouse();
                    wm_render_pending = 1;
                    serial_write("AUTOCLICK: start \n");
                    terminal_write("Auto-simulated Start click\n");
                    auto_simclick_step = 1;
                    auto_next_click_tick = timer_ticks + 60u; /* wait a bit then click menu */
                }
            } else if (auto_simclick_step == 1 && timer_ticks >= auto_next_click_tick) {
                /* Compute menu position and click the Terminal item (rel_y 32..52) */
                uint32_t tb_y = gui_framebuffer_height > 40u ? gui_framebuffer_height - 40u : 0u;
                uint32_t menu_x = 8u;
                uint32_t menu_h = wm_start_menu_height;
                uint32_t menu_y = (tb_y > menu_h) ? (tb_y - menu_h) : 0u;
                uint32_t click_x = menu_x + 16u;
                uint32_t click_y = menu_y + 40u; /* inside Terminal item */
                mouse_x = click_x; mouse_y = click_y;
                mouse_buttons |= 0x01u;
                dbg_log_event(DBG_EV_MOUSE_CLICK, (int32_t)mouse_x, (int32_t)mouse_y);
                (void)wm_handle_mouse();
                mouse_buttons &= (uint8_t)~0x01u;
                (void)wm_handle_mouse();
                wm_render_pending = 1;
                serial_write("AUTOCLICK: menu->terminal\n");
                terminal_write("Auto-simulated menu click (Terminal)\n");
                auto_simclick_step = 2; /* done */
            }
        }

        /* After auto click sequence completes, dump debug event log once */
        if (auto_simclick_step >= 2 && !auto_simclick_dumped) {
            dbg_dump_log();
            /* Also emit WM state for headless verification */
            wm_dump_state();
            serial_write("AUTO-DUMP: dbg log emitted\n");
            auto_simclick_dumped = 1;
        }
        __asm__ __volatile__("hlt");
    }
}
