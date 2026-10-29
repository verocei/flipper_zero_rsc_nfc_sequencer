/*
 * RSC NFC Sequencer  (v0.6)
 * ------------------------------------------------------------------
 * A reader / access-control test tool for the Flipper Zero. Cycles
 * through saved .nfc tags and emulates them, so an NFC reader can be
 * exercised unattended.
 *
 * Menu: Start / Settings / Generate tags / Delete generated / About.
 *
 * Which tag (order):   SEQ (dir order) / ABC (name) / RND (random)
 * When (timing):
 *   Fixed  - one tag every N seconds, held continuously.
 *   Burst  - a set number of emissions per period, at random moments;
 *            each emission presents the tag briefly, then idles.
 * Total codes:         stop after N emissions (or run forever).
 * Source:              the app's own tags folder, or any chosen folder.
 * LED:                 off / green blink / red blink on each emission.
 * Generate tags:       write GEN-n.nfc files with random UIDs into the app
 *                      folder; Delete removes them. The "Data" option can put
 *                      an NDEF record (name / custom text / URL) into the tag,
 *                      which a phone reads as text -- those are Mifare Classic 1K.
 *
 * Tags:     the .nfc files in <source> (default: the app's tags folder)
 * Settings: /ext/apps_data/rsc_nfc_sequencer/settings.bin
 *
 * Target firmware: stock (OFW) and Momentum. Uses only the shared OFW
 * NFC API, so one build runs on both via ufbt.
 *
 * NOTE ON SCOPE: emulation is only guaranteed faithful for UID-based
 * readers. Full MIFARE Classic content emulation is reader-dependent
 * on the Flipper and outside this app's intended use.
 */

#include <furi.h>
#include <furi_hal_random.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <gui/modules/text_input.h>
#include <input/input.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <toolbox/path.h>
#include <notification/notification_messages.h>

#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <nfc/nfc_listener.h>

#define TAG "RscNfcSeq"
#define APP_NAME "RSC NFC Sequencer"

#define BASE_DIR      EXT_PATH("apps_data/rsc_nfc_sequencer")
#define TAGS_DIR      EXT_PATH("apps_data/rsc_nfc_sequencer/tags")
#define GEN_DIR       EXT_PATH("apps_data/rsc_nfc_sequencer/tags/generated")
#define SETTINGS_PATH EXT_PATH("apps_data/rsc_nfc_sequencer/settings.bin")
#define NFC_START_DIR EXT_PATH("nfc")
#define NFC_EXT       ".nfc"

#define MAX_TAGS 128
#define NAME_LEN 64
#define SRC_LEN  256
#define PATH_LEN (SRC_LEN + NAME_LEN + 8)
#define GEN_TEXT_LEN 64 // custom NDEF text / URL entered on the device

#define INTERVAL_MIN     1
#define INTERVAL_MAX     60
#define INTERVAL_DEFAULT 3

#define PER_PERIOD_MIN 1
#define PER_PERIOD_MAX 60

#define MAX_GEN         100
#define GEN_DEFAULT     10
#define BURST_ON_MS     350 // how long a tag stays presented in burst mode
#define TICK_MS         50 // scheduler resolution

#define SETTINGS_MAGIC   0x52 // 'R'
#define SETTINGS_VERSION 6

typedef enum {
    ModeSequential = 0,
    ModeAlphabetical,
    ModeRandom,
    ModeCount,
} PlayMode;

static const char* const mode_label[ModeCount] = {"SEQ", "ABC", "RND"};

typedef enum {
    TimingFixed = 0,
    TimingBurst,
    TimingCount,
} TimingMode;

static const char* const timing_label[TimingCount] = {"Fixed", "Burst"};

typedef enum {
    LedOff = 0,
    LedRed,
    LedGreen,
    LedBlue,
    LedYellow,
    LedCyan,
    LedMagenta,
    LedWhite,
    LedCount,
} LedMode;

static const char* const led_label[LedCount] =
    {"Off", "Red", "Green", "Blue", "Yellow", "Cyan", "Magenta", "White"};

static const char* const on_off_label[2] = {"Off", "On"};

// What (if anything) to write into a generated tag as NDEF data. Off keeps the
// old UID-only ISO14443-3A tag; the others produce an NTAG215 a phone can read.
typedef enum {
    GenDataOff = 0, // UID only (ISO14443-3A), as before
    GenDataName, // NDEF text record = the tag's own name (GEN-n)
    GenDataText, // NDEF text record = a custom text
    GenDataUrl, // NDEF URI record = a URL
    GenDataCount,
} GenDataMode;

static const char* const gen_data_label[GenDataCount] = {"Off", "Name", "Text", "URL"};

typedef enum {
    SrcApp = 0, // the app's tags folder (your own / sample tags)
    SrcGenerated, // the generated/ subfolder (GEN-*.nfc)
    SrcCustom, // a folder picked with the file browser
    SrcCount,
} SourceMode;

static const uint16_t period_choices[] = {5, 10, 15, 30, 60, 120, 300};
#define PERIOD_COUNT (sizeof(period_choices) / sizeof(period_choices[0]))

static const uint16_t total_choices[] = {0, 10, 20, 50, 100, 200, 500, 1000};
#define TOTAL_COUNT (sizeof(total_choices) / sizeof(total_choices[0]))

// How long each code is actually transmitted (field on), both modes. Long enough
// that a reader completes one read; raise it if a reader doesn't pick codes up.
static const uint16_t hold_choices[] = {200, 300, 500, 1000, 2000, 3000};
static const char* const hold_labels[] = {"0.2s", "0.3s", "0.5s", "1s", "2s", "3s"};
#define HOLD_COUNT (sizeof(hold_choices) / sizeof(hold_choices[0]))

// Minimum spacing between two emissions in burst mode.
static const uint16_t gap_choices[] = {500, 1000, 2000, 3000, 5000};
static const char* const gap_labels[] = {"0.5s", "1s", "2s", "3s", "5s"};
#define GAP_COUNT (sizeof(gap_choices) / sizeof(gap_choices[0]))

typedef enum {
    ViewIdMenu = 0,
    ViewIdSettings,
    ViewIdEmu,
    ViewIdGenerate,
    ViewIdGenText,
    ViewIdConfirmDel,
    ViewIdAbout,
} ViewId;

typedef enum {
    MenuIndexStart = 0,
    MenuIndexSettings,
    MenuIndexGenerate,
    MenuIndexDeleteGen,
    MenuIndexAbout,
} MenuIndex;

// Row order in the settings list.
typedef enum {
    RowMode = 0,
    RowTiming,
    RowDelay,
    RowPerPeriod,
    RowPeriod,
    RowTotal,
    RowSource,
    RowLed,
} SettingsRow;

typedef enum {
    CustomEventTick = 1,
    CustomEventOpenSource = 2,
    CustomEventRebuildSettings = 3,
} CustomEvent;

typedef struct App App;

// Shared model for the custom views: just a back-reference to App.
typedef struct {
    App* app;
} ViewModel;

struct App {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* menu;
    VariableItemList* settings_list;
    View* emu_view;
    View* generate_view;
    View* confirm_view;
    Widget* about;
    TextInput* text_input;
    Storage* storage;
    DialogsApp* dialogs;
    NotificationApp* notif;
    FuriTimer* timer;
    uint32_t tick_freq;

    VariableItem* item_mode;
    VariableItem* item_timing;
    VariableItem* item_delay;
    VariableItem* item_per_period;
    VariableItem* item_period;
    VariableItem* item_total;
    VariableItem* item_source;
    VariableItem* item_led;
    uint8_t source_row; // dynamic row index of the Source item

    // nfc
    Nfc* nfc;
    NfcDevice* device;
    NfcListener* listener;

    // tag library
    char names[MAX_TAGS][NAME_LEN];
    uint8_t tag_count;

    // settings
    PlayMode mode;
    TimingMode timing;
    uint8_t interval; // seconds between code starts (fixed)
    uint8_t per_period; // emissions per period (burst)
    uint16_t period_s; // period length seconds (burst)
    uint16_t hold_ms; // how long each code is presented (both modes)
    uint16_t min_gap_ms; // minimum spacing between emissions (burst)
    uint16_t total; // stop after N emissions, 0 = unlimited
    LedMode led;
    bool vibro;
    bool beep;
    SourceMode src_mode;
    char source[SRC_LEN]; // custom folder path (used when src_mode == SrcCustom)

    // runtime
    bool running;
    bool finished;
    int current;
    bool last_load_ok;
    char uid_str[48];
    uint32_t emitted;
    ViewId current_view;

    // scheduler
    uint32_t next_fixed; // tick of next switch (fixed)
    uint32_t period_start; // tick of current period start (burst)
    uint32_t sched[PER_PERIOD_MAX]; // emission tick-offsets within period
    uint8_t sched_count;
    uint8_t sched_idx;
    bool presenting;
    uint32_t present_until;

    // generate screen
    int gen_count;
    uint8_t gen_rep;
    bool gen_result_shown;
    int gen_made;
    GenDataMode gen_data; // what NDEF data to embed in generated tags
    char gen_text[GEN_TEXT_LEN]; // custom text / URL for GenDataText / GenDataUrl
};

/* ---------------- helpers ---------------- */

static void settings_populate(App* app); // defined later; used by the event handler
static void set_source_item_text(App* app); // defined later; used by the generate view

static uint32_t ms_to_ticks(App* app, uint32_t ms) {
    return (uint32_t)(((uint64_t)ms * app->tick_freq) / 1000u);
}

static const char* source_dir(App* app) {
    switch(app->src_mode) {
    case SrcGenerated:
        return GEN_DIR;
    case SrcCustom:
        return app->source[0] ? app->source : TAGS_DIR;
    case SrcApp:
    default:
        return TAGS_DIR;
    }
}

static bool parse_gen_number(const char* name, int* out) {
    if(strncmp(name, "GEN-", 4) != 0) return false;
    const char* p = name + 4;
    if(*p < '0' || *p > '9') return false;
    int n = 0;
    while(*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        p++;
    }
    if(strcasecmp(p, NFC_EXT) != 0) return false;
    *out = n;
    return true;
}

/* ---------------- settings persistence ---------------- */

typedef struct {
    uint8_t magic;
    uint8_t version;
    uint8_t mode;
    uint8_t timing;
    uint8_t interval;
    uint8_t per_period;
    uint8_t led;
    uint8_t src_mode;
    uint8_t vibro;
    uint8_t beep;
    uint16_t period_s;
    uint16_t total;
    uint16_t hold_ms;
    uint16_t min_gap_ms;
    char source[SRC_LEN];
    uint8_t gen_data;
    char gen_text[GEN_TEXT_LEN];
} SettingsBlob;

static void settings_defaults(App* app) {
    app->mode = ModeSequential;
    app->timing = TimingFixed;
    app->interval = INTERVAL_DEFAULT;
    app->per_period = 5;
    app->period_s = 10;
    app->hold_ms = 1000;
    app->min_gap_ms = 2000;
    app->total = 0;
    app->led = LedGreen;
    app->vibro = false;
    app->beep = false;
    app->src_mode = SrcApp;
    app->source[0] = '\0';
    app->gen_data = GenDataName;
    app->gen_text[0] = '\0';
}

static void settings_load(App* app) {
    settings_defaults(app);

    File* f = storage_file_alloc(app->storage);
    if(storage_file_open(f, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        SettingsBlob b;
        if(storage_file_read(f, &b, sizeof(b)) == sizeof(b) && b.magic == SETTINGS_MAGIC &&
           b.version == SETTINGS_VERSION) {
            if(b.mode < ModeCount) app->mode = b.mode;
            if(b.timing < TimingCount) app->timing = b.timing;
            if(b.interval >= INTERVAL_MIN && b.interval <= INTERVAL_MAX) app->interval = b.interval;
            if(b.per_period >= PER_PERIOD_MIN && b.per_period <= PER_PERIOD_MAX)
                app->per_period = b.per_period;
            if(b.led < LedCount) app->led = b.led;
            if(b.src_mode < SrcCount) app->src_mode = b.src_mode;
            app->vibro = b.vibro ? true : false;
            app->beep = b.beep ? true : false;
            app->period_s = b.period_s ? b.period_s : 10;
            app->total = b.total;
            if(b.hold_ms) app->hold_ms = b.hold_ms;
            if(b.min_gap_ms) app->min_gap_ms = b.min_gap_ms;
            b.source[SRC_LEN - 1] = '\0';
            strncpy(app->source, b.source, SRC_LEN - 1);
            app->source[SRC_LEN - 1] = '\0';
            if(b.gen_data < GenDataCount) app->gen_data = b.gen_data;
            b.gen_text[GEN_TEXT_LEN - 1] = '\0';
            strncpy(app->gen_text, b.gen_text, GEN_TEXT_LEN - 1);
            app->gen_text[GEN_TEXT_LEN - 1] = '\0';
        }
    }
    storage_file_close(f);
    storage_file_free(f);
}

static void settings_save(App* app) {
    SettingsBlob b;
    memset(&b, 0, sizeof(b));
    b.magic = SETTINGS_MAGIC;
    b.version = SETTINGS_VERSION;
    b.mode = (uint8_t)app->mode;
    b.timing = (uint8_t)app->timing;
    b.interval = app->interval;
    b.per_period = app->per_period;
    b.led = (uint8_t)app->led;
    b.src_mode = (uint8_t)app->src_mode;
    b.vibro = app->vibro ? 1 : 0;
    b.beep = app->beep ? 1 : 0;
    b.period_s = app->period_s;
    b.total = app->total;
    b.hold_ms = app->hold_ms;
    b.min_gap_ms = app->min_gap_ms;
    strncpy(b.source, app->source, SRC_LEN - 1);
    b.gen_data = (uint8_t)app->gen_data;
    strncpy(b.gen_text, app->gen_text, GEN_TEXT_LEN - 1);

    File* f = storage_file_alloc(app->storage);
    if(storage_file_open(f, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, &b, sizeof(b));
    }
    storage_file_close(f);
    storage_file_free(f);
}

/* ---------------- tag library ---------------- */

static void tags_scan(App* app) {
    app->tag_count = 0;
    storage_common_mkdir(app->storage, BASE_DIR);
    storage_common_mkdir(app->storage, TAGS_DIR);

    File* dir = storage_file_alloc(app->storage);
    if(storage_dir_open(dir, source_dir(app))) {
        FileInfo info;
        char name[NAME_LEN];
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;
            size_t len = strlen(name);
            size_t ext = strlen(NFC_EXT);
            if(len <= ext) continue;
            if(strcasecmp(name + (len - ext), NFC_EXT) != 0) continue;
            if(app->tag_count >= MAX_TAGS) break;
            strncpy(app->names[app->tag_count], name, NAME_LEN - 1);
            app->names[app->tag_count][NAME_LEN - 1] = '\0';
            app->tag_count++;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
}

// Insertion sort: qsort is disabled in the firmware API, and MAX_TAGS is small.
static void tags_sort_alpha(App* app) {
    char tmp[NAME_LEN];
    for(int i = 1; i < app->tag_count; i++) {
        memcpy(tmp, app->names[i], NAME_LEN);
        int j = i - 1;
        while(j >= 0 && strncmp(app->names[j], tmp, NAME_LEN) > 0) {
            memcpy(app->names[j + 1], app->names[j], NAME_LEN);
            j--;
        }
        memcpy(app->names[j + 1], tmp, NAME_LEN);
    }
}

static void tag_display_name(App* app, int idx, char* out, size_t out_len) {
    out[0] = '\0';
    if(idx < 0 || idx >= app->tag_count) return;
    strncpy(out, app->names[idx], out_len - 1);
    out[out_len - 1] = '\0';
    size_t len = strlen(out);
    size_t ext = strlen(NFC_EXT);
    if(len > ext && strcasecmp(out + (len - ext), NFC_EXT) == 0) out[len - ext] = '\0';
}

/* ---------------- generated tags ---------------- */

static int generated_count_in(App* app, const char* folder) {
    int c = 0;
    File* dir = storage_file_alloc(app->storage);
    if(storage_dir_open(dir, folder)) {
        FileInfo info;
        char name[NAME_LEN];
        int n;
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;
            if(parse_gen_number(name, &n)) c++;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    return c;
}

// Generated tags now live in GEN_DIR, but earlier builds wrote them into the
// tags root; count both so the user sees everything that "Delete" will clear.
static int generated_count(App* app) {
    return generated_count_in(app, GEN_DIR) + generated_count_in(app, TAGS_DIR);
}

static int generated_max(App* app) {
    int maxn = 0;
    File* dir = storage_file_alloc(app->storage);
    if(storage_dir_open(dir, GEN_DIR)) {
        FileInfo info;
        char name[NAME_LEN];
        int n;
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;
            if(parse_gen_number(name, &n) && n > maxn) maxn = n;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    return maxn;
}

// Write one GEN-n.nfc with a random 4-byte ISO14443-3A UID (UID-only tag).
static bool write_uid_tag(App* app, const char* path) {
    uint8_t uid[4];
    furi_hal_random_fill_buf(uid, sizeof(uid));
    bool ok = false;
    File* f = storage_file_alloc(app->storage);
    if(storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char buf[160];
        int len = snprintf(
            buf,
            sizeof(buf),
            "Filetype: Flipper NFC device\n"
            "Version: 4\n"
            "Device type: ISO14443-3A\n"
            "UID: %02X %02X %02X %02X\n"
            "ATQA: 00 04\n"
            "SAK: 08\n",
            uid[0],
            uid[1],
            uid[2],
            uid[3]);
        ok = storage_file_write(f, buf, len) == (size_t)len;
    }
    storage_file_close(f);
    storage_file_free(f);
    return ok;
}

// Build an NDEF TLV with a single UTF-8 Text record ("en" language). Standard
// and portable: NDEF reader apps (NFC Tools, TagInfo) show it. Note a phone's
// default handler may not pop up for a bare Text record — the URL mode (a URI
// record) is the surest to display on any phone.
static size_t build_ndef_text(const char* text, uint8_t* out, size_t cap) {
    size_t tlen = strlen(text);
    size_t payload = 1 + 2 + tlen; // status byte + "en" + text
    size_t msglen = 4 + payload; // D1 01 <payload> 54 + payload
    size_t total = 2 + msglen + 1; // 03 <msglen> ... FE
    if(payload > 255 || total > cap) return 0;
    size_t i = 0;
    out[i++] = 0x03; // NDEF message TLV
    out[i++] = (uint8_t)msglen;
    out[i++] = 0xD1; // MB+ME+SR, TNF = well-known
    out[i++] = 0x01; // type length
    out[i++] = (uint8_t)payload;
    out[i++] = 0x54; // 'T' (text)
    out[i++] = 0x02; // status: language code length = 2
    out[i++] = 'e';
    out[i++] = 'n';
    memcpy(out + i, text, tlen);
    i += tlen;
    out[i++] = 0xFE; // terminator TLV
    return i;
}

// Build an NDEF TLV with a single URI record (no prefix abbreviation).
static size_t build_ndef_uri(const char* uri, uint8_t* out, size_t cap) {
    size_t ulen = strlen(uri);
    size_t payload = 1 + ulen; // URI identifier code + URI
    size_t msglen = 4 + payload; // D1 01 <payload> 55 + payload
    size_t total = 2 + msglen + 1;
    if(payload > 255 || total > cap) return 0;
    size_t i = 0;
    out[i++] = 0x03;
    out[i++] = (uint8_t)msglen;
    out[i++] = 0xD1;
    out[i++] = 0x01;
    out[i++] = (uint8_t)payload;
    out[i++] = 0x55; // 'U' (URI)
    out[i++] = 0x00; // identifier code 0x00 = no abbreviation (full URI follows)
    memcpy(out + i, uri, ulen);
    i += ulen;
    out[i++] = 0xFE;
    return i;
}

// Write one GEN-n.nfc as a Mifare Classic 1K tag carrying `user` (an NDEF TLV)
// in its NDEF sectors, with a random 4-byte UID. Modelled on a known-good tag:
// the Flipper emulates Mifare Classic NDEF reliably for phones, whereas its
// NTAG/Ultralight emulation often yields only the UID. The .nfc text is written
// directly (block layout), matching the saved-tag format exactly.
static bool write_mfc_tag(App* app, const char* path, const uint8_t* user, size_t user_len) {
    uint8_t img[1024]; // 64 blocks x 16 bytes
    memset(img, 0, sizeof(img));

    uint8_t uid[4];
    furi_hal_random_fill_buf(uid, sizeof(uid));

    // Block 0: UID + BCC + SAK/ATQA + manufacturer bytes.
    img[0] = uid[0];
    img[1] = uid[1];
    img[2] = uid[2];
    img[3] = uid[3];
    img[4] = uid[0] ^ uid[1] ^ uid[2] ^ uid[3]; // BCC
    img[5] = 0x08; // SAK
    img[6] = 0x04; // ATQA
    img[7] = 0x00;
    static const uint8_t mfg[8] = {0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69};
    memcpy(img + 8, mfg, 8);

    // Blocks 1-2: MAD (Mifare Application Directory), every sector -> NDEF (03 E1).
    img[16] = 0x14; // MAD CRC (fixed for this all-NDEF directory)
    img[17] = 0x01; // info byte
    for(int k = 2; k < 16; k += 2) {
        img[16 + k] = 0x03;
        img[16 + k + 1] = 0xE1;
    }
    for(int k = 0; k < 16; k += 2) {
        img[32 + k] = 0x03;
        img[32 + k + 1] = 0xE1;
    }

    // Sector trailers: sector 0 uses the MAD key A0..A5, the rest the public NDEF
    // key D3F7.. ; access bits leave the data blocks readable with key A.
    static const uint8_t mad_tr[16] = {
        0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0x78, 0x77, 0x88, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t ndef_tr[16] = {
        0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7, 0x7F, 0x07, 0x88, 0x40, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(img + 3 * 16, mad_tr, 16);
    for(int s = 1; s < 16; s++) memcpy(img + (s * 4 + 3) * 16, ndef_tr, 16);

    // Lay the NDEF TLV across the data blocks of sectors 1..15 (skip trailers).
    size_t bi = 0;
    for(int blk = 4; blk < 64 && bi < user_len; blk++) {
        if((blk % 4) == 3) continue; // sector trailer
        for(int j = 0; j < 16 && bi < user_len; j++) img[blk * 16 + j] = user[bi++];
    }

    bool ok = false;
    File* f = storage_file_alloc(app->storage);
    if(storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FuriString* s = furi_string_alloc();
        furi_string_printf(
            s,
            "Filetype: Flipper NFC device\n"
            "Version: 4\n"
            "Device type: Mifare Classic\n"
            "UID: %02X %02X %02X %02X\n"
            "ATQA: 00 04\n"
            "SAK: 08\n"
            "Mifare Classic type: 1K\n"
            "Data format version: 2\n",
            uid[0],
            uid[1],
            uid[2],
            uid[3]);
        for(int blk = 0; blk < 64; blk++) {
            furi_string_cat_printf(s, "Block %d:", blk);
            for(int j = 0; j < 16; j++) furi_string_cat_printf(s, " %02X", img[blk * 16 + j]);
            furi_string_cat_printf(s, "\n");
        }
        size_t len = furi_string_size(s);
        ok = storage_file_write(f, furi_string_get_cstr(s), len) == len;
        furi_string_free(s);
    }
    storage_file_close(f);
    storage_file_free(f);
    return ok;
}

// Write `count` GEN-n.nfc files into the app's generated folder, continuing the
// numbering so nothing is overwritten. The tag kind depends on app->gen_data:
// Off = a UID-only ISO14443-3A tag; Name/Text/URL = a Mifare Classic 1K with NDEF.
static int generate_tags(App* app, int count) {
    storage_common_mkdir(app->storage, BASE_DIR);
    storage_common_mkdir(app->storage, TAGS_DIR);
    storage_common_mkdir(app->storage, GEN_DIR);
    int base = generated_max(app);
    int made = 0;
    for(int i = 1; i <= count; i++) {
        char path[PATH_LEN];
        snprintf(path, sizeof(path), "%s/GEN-%d%s", GEN_DIR, base + i, NFC_EXT);

        if(app->gen_data == GenDataOff) {
            if(write_uid_tag(app, path)) made++;
            continue;
        }

        uint8_t user[512];
        size_t user_len = 0;
        if(app->gen_data == GenDataUrl) {
            user_len = build_ndef_uri(app->gen_text, user, sizeof(user));
        } else {
            char text[GEN_TEXT_LEN];
            if(app->gen_data == GenDataName) {
                snprintf(text, sizeof(text), "GEN-%d", base + i);
            } else { // GenDataText
                strncpy(text, app->gen_text, sizeof(text) - 1);
                text[sizeof(text) - 1] = '\0';
            }
            user_len = build_ndef_text(text, user, sizeof(user));
        }
        if(user_len && write_mfc_tag(app, path, user, user_len)) made++;
    }
    return made;
}

// Remove every GEN-*.nfc from a folder; returns how many were deleted.
static int delete_generated_in(App* app, const char* folder) {
    static char victims[MAX_TAGS][NAME_LEN];
    int vc = 0;
    File* dir = storage_file_alloc(app->storage);
    if(storage_dir_open(dir, folder)) {
        FileInfo info;
        char name[NAME_LEN];
        int n;
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;
            if(parse_gen_number(name, &n) && vc < MAX_TAGS) {
                strncpy(victims[vc], name, NAME_LEN - 1);
                victims[vc][NAME_LEN - 1] = '\0';
                vc++;
            }
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);

    int removed = 0;
    for(int i = 0; i < vc; i++) {
        char path[PATH_LEN];
        snprintf(path, sizeof(path), "%s/%.*s", folder, NAME_LEN - 1, victims[i]);
        if(storage_common_remove(app->storage, path) == FSE_OK) removed++;
    }
    return removed;
}

// Clear the generated subfolder, plus any GEN-*.nfc left in the tags root by
// earlier builds.
static int delete_generated(App* app) {
    return delete_generated_in(app, GEN_DIR) + delete_generated_in(app, TAGS_DIR);
}

/* ---------------- emulation core ---------------- */

// Solid-colour LED sequences that stay lit (message_do_not_reset) until led_off().
static const NotificationSequence seq_led_red = {&message_red_255, &message_do_not_reset, NULL};
static const NotificationSequence seq_led_green = {&message_green_255, &message_do_not_reset, NULL};
static const NotificationSequence seq_led_blue = {&message_blue_255, &message_do_not_reset, NULL};
static const NotificationSequence seq_led_yellow =
    {&message_red_255, &message_green_255, &message_do_not_reset, NULL};
static const NotificationSequence seq_led_cyan =
    {&message_green_255, &message_blue_255, &message_do_not_reset, NULL};
static const NotificationSequence seq_led_magenta =
    {&message_red_255, &message_blue_255, &message_do_not_reset, NULL};
static const NotificationSequence seq_led_white =
    {&message_red_255, &message_green_255, &message_blue_255, &message_do_not_reset, NULL};

// Short beeps (sound only, no LED) built from raw messages so they never reset
// the solid colour we set for the emission.
static const NotificationMessage m_beep_ok = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 2000.0f, .volume = 1.0f},
};
static const NotificationMessage m_beep_err = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 500.0f, .volume = 1.0f},
};
static const NotificationSequence seq_beep_ok = {
    &m_beep_ok,
    &message_delay_50,
    &message_sound_off,
    NULL,
};
static const NotificationSequence seq_beep_err = {
    &m_beep_err,
    &message_delay_100,
    &message_sound_off,
    NULL,
};

static const NotificationSequence* led_solid_sequence(LedMode m) {
    switch(m) {
    case LedRed:
        return &seq_led_red;
    case LedGreen:
        return &seq_led_green;
    case LedBlue:
        return &seq_led_blue;
    case LedYellow:
        return &seq_led_yellow;
    case LedCyan:
        return &seq_led_cyan;
    case LedMagenta:
        return &seq_led_magenta;
    case LedWhite:
        return &seq_led_white;
    default:
        return NULL;
    }
}

// Feedback at the start of an emission: optional vibro + beep, then the solid
// LED colour (red if the tag failed to load). The LED stays lit until led_off().
static void emit_feedback(App* app) {
    if(!app->notif) return;
    if(app->vibro) notification_message(app->notif, &sequence_single_vibro);
    if(app->beep)
        notification_message(app->notif, app->last_load_ok ? &seq_beep_ok : &seq_beep_err);
    if(app->led != LedOff) {
        const NotificationSequence* s =
            app->last_load_ok ? led_solid_sequence(app->led) : &seq_led_red;
        if(s) notification_message(app->notif, s);
    }
}

static void led_off(App* app) {
    if(app->notif && app->led != LedOff) notification_message(app->notif, &sequence_reset_rgb);
}

static void emu_stop(App* app) {
    if(app->listener) {
        nfc_listener_stop(app->listener);
        nfc_listener_free(app->listener);
        app->listener = NULL;
    }
    if(app->device) {
        nfc_device_free(app->device);
        app->device = NULL;
    }
    // Keep app->uid_str so the last sent tag stays shown during idle gaps; it is
    // overwritten on the next successful load.
}

static void uid_to_str(const uint8_t* uid, size_t len, char* out, size_t out_len) {
    out[0] = '\0';
    size_t pos = 0;
    for(size_t i = 0; i < len && pos + 3 < out_len; i++) {
        pos += snprintf(out + pos, out_len - pos, (i == 0) ? "%02X" : " %02X", uid[i]);
    }
}

static bool emu_start_index(App* app, int idx) {
    emu_stop(app);
    if(idx < 0 || idx >= app->tag_count) return false;

    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/%.*s", source_dir(app), NAME_LEN - 1, app->names[idx]);

    app->device = nfc_device_alloc();
    if(!nfc_device_load(app->device, path)) {
        FURI_LOG_W(TAG, "load failed: %s", path);
        nfc_device_free(app->device);
        app->device = NULL;
        return false;
    }

    NfcProtocol protocol = nfc_device_get_protocol(app->device);
    if(protocol == NfcProtocolInvalid) {
        nfc_device_free(app->device);
        app->device = NULL;
        return false;
    }

    size_t uid_len = 0;
    const uint8_t* uid = nfc_device_get_uid(app->device, &uid_len);
    if(uid && uid_len) uid_to_str(uid, uid_len, app->uid_str, sizeof(app->uid_str));

    app->listener =
        nfc_listener_alloc(app->nfc, protocol, nfc_device_get_data(app->device, protocol));
    nfc_listener_start(app->listener, NULL, NULL);
    return true;
}

static int next_index(App* app, int from) {
    if(app->tag_count == 0) return -1;
    switch(app->mode) {
    case ModeRandom:
        if(app->tag_count == 1) return 0;
        return (int)(furi_hal_random_get() % app->tag_count);
    case ModeSequential:
    case ModeAlphabetical:
    default: {
        int n = from + 1;
        if(n >= app->tag_count) n = 0;
        return n;
    }
    }
}

// Count one emission; returns true if the total cap was reached.
static bool count_emission(App* app) {
    app->emitted++;
    if(app->total > 0 && app->emitted >= app->total) return true;
    return false;
}

// Build the emission schedule for a new burst period: `per_period` start times
// at random moments, but never closer together than `min_gap_ms`.
static void burst_new_period(App* app, uint32_t now) {
    app->period_start = now;
    app->sched_idx = 0;
    uint32_t period_ms = (uint32_t)app->period_s * 1000u;
    uint32_t gap = app->min_gap_ms ? app->min_gap_ms : 1;

    // How many starts fit while keeping at least `gap` between them.
    uint8_t maxfit = (uint8_t)(period_ms / gap);
    if(maxfit < 1) maxfit = 1;
    uint8_t n = app->per_period;
    if(n > maxfit) n = maxfit;
    if(n > PER_PERIOD_MAX) n = PER_PERIOD_MAX;

    // Random jitter within the slack left after reserving the minimum gaps.
    uint32_t reserved = (n > 0) ? (uint32_t)(n - 1) * gap : 0;
    uint32_t slack = (period_ms > reserved) ? (period_ms - reserved) : 0;
    uint32_t offs[PER_PERIOD_MAX];
    for(uint8_t i = 0; i < n; i++) offs[i] = slack ? (furi_hal_random_get() % slack) : 0;
    for(uint8_t i = 1; i < n; i++) { // sort ascending
        uint32_t v = offs[i];
        int j = i - 1;
        while(j >= 0 && offs[j] > v) {
            offs[j + 1] = offs[j];
            j--;
        }
        offs[j + 1] = v;
    }
    for(uint8_t i = 0; i < n; i++) {
        uint32_t t_ms = offs[i] + (uint32_t)i * gap; // guarantees >= gap spacing
        if(period_ms && t_ms >= period_ms) t_ms = period_ms - 1;
        app->sched[i] = ms_to_ticks(app, t_ms);
    }
    app->sched_count = n;
}

static void run_stop(App* app) {
    furi_timer_stop(app->timer);
    led_off(app);
    emu_stop(app);
    if(app->nfc) {
        nfc_free(app->nfc);
        app->nfc = NULL;
    }
    app->running = false;
    app->presenting = false;
}

// Present the next tag for the Hold window (field on); LED/vibro/beep fire here.
static void start_code(App* app, uint32_t now) {
    app->current = next_index(app, app->current);
    app->last_load_ok = emu_start_index(app, app->current);
    emit_feedback(app);
    app->presenting = true;
    app->present_until = now + ms_to_ticks(app, app->hold_ms);
}

static void run_start(App* app) {
    app->emitted = 0;
    app->finished = false;
    app->presenting = false;
    app->current = -1;
    if(app->tag_count == 0) return;

    // SEQ follows directory order; re-scan so a previous ABC sort doesn't stick.
    tags_scan(app);
    if(app->mode == ModeAlphabetical) tags_sort_alpha(app);

    app->nfc = nfc_alloc();
    uint32_t now = furi_get_tick();
    app->running = true;

    if(app->timing == TimingFixed) {
        start_code(app, now);
        app->next_fixed = now + ms_to_ticks(app, (uint32_t)app->interval * 1000u);
        if(count_emission(app)) {
            run_stop(app);
            app->finished = true;
            return;
        }
    } else {
        burst_new_period(app, now);
    }
    furi_timer_start(app->timer, ms_to_ticks(app, TICK_MS));
}

static void run_tick(App* app) {
    if(!app->running || app->finished) return;
    uint32_t now = furi_get_tick();

    // Both modes: drop the field when the Hold window ends (tap behaviour), so a
    // reader sees each code once rather than continuously.
    if(app->presenting && (int32_t)(now - app->present_until) >= 0) {
        emu_stop(app);
        led_off(app);
        app->presenting = false;
    }

    if(app->timing == TimingFixed) {
        if(!app->presenting && (int32_t)(now - app->next_fixed) >= 0) {
            start_code(app, now);
            app->next_fixed = now + ms_to_ticks(app, (uint32_t)app->interval * 1000u);
            if(count_emission(app)) {
                run_stop(app);
                app->finished = true;
            }
        }
        return;
    }

    // Burst
    if(!app->presenting) {
        if(app->sched_idx < app->sched_count &&
           (int32_t)((now - app->period_start) - app->sched[app->sched_idx]) >= 0) {
            start_code(app, now);
            app->sched_idx++;
            if(count_emission(app)) {
                run_stop(app);
                app->finished = true;
            }
        } else if(
            app->sched_idx >= app->sched_count &&
            (int32_t)((now - app->period_start) - ms_to_ticks(app, (uint32_t)app->period_s * 1000u)) >=
                0) {
            burst_new_period(app, now);
        }
    }
}

/* ---------------- shared drawing ---------------- */

static void draw_centered_trunc(Canvas* canvas, int y, const char* text) {
    char buf[NAME_LEN + 4];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    while(strlen(buf) > 1 && canvas_string_width(canvas, buf) > 126) {
        buf[strlen(buf) - 1] = '\0';
    }
    int w = canvas_string_width(canvas, buf);
    int x = (128 - w) / 2;
    if(x < 1) x = 1;
    canvas_draw_str(canvas, x, y, buf);
}

static void switch_view(App* app, ViewId id) {
    app->current_view = id;
    view_dispatcher_switch_to_view(app->view_dispatcher, id);
}

/* ---------------- emulation view ---------------- */

static void emu_draw_cb(Canvas* canvas, void* model) {
    App* app = ((ViewModel*)model)->app;
    canvas_clear(canvas);

    if(app->finished) {
        canvas_set_font(canvas, FontPrimary);
        draw_centered_trunc(canvas, 28, "Done");
        canvas_set_font(canvas, FontSecondary);
        char line[32];
        snprintf(line, sizeof(line), "%lu codes sent", (unsigned long)app->emitted);
        draw_centered_trunc(canvas, 44, line);
        canvas_draw_str(canvas, 2, 62, "Back = menu");
        return;
    }

    if(app->tag_count == 0) {
        canvas_set_font(canvas, FontPrimary);
        draw_centered_trunc(canvas, 30, "No tags");
        canvas_set_font(canvas, FontSecondary);
        draw_centered_trunc(canvas, 44, "No .nfc in source folder");
        canvas_draw_str(canvas, 2, 62, "Back = menu");
        return;
    }

    // Top: mode / timing / position (+ total), small.
    canvas_set_font(canvas, FontSecondary);
    char top[40];
    if(app->timing == TimingFixed) {
        snprintf(
            top,
            sizeof(top),
            "%s  %ds  %d/%d",
            mode_label[app->mode],
            app->interval,
            app->current + 1,
            app->tag_count);
    } else {
        snprintf(
            top, sizeof(top), "%s  %d/%ds", mode_label[app->mode], app->per_period, app->period_s);
    }
    canvas_draw_str(canvas, 2, 10, top);
    canvas_draw_line(canvas, 0, 13, 128, 13);

    // Current / last sent tag name — large. It stays on screen between
    // emissions so the name (e.g. GEN-1) and UID remain readable.
    canvas_set_font(canvas, FontPrimary);
    if(app->current < 0) {
        draw_centered_trunc(canvas, 32, "Starting");
    } else {
        char name[NAME_LEN];
        tag_display_name(app, app->current, name, sizeof(name));
        draw_centered_trunc(canvas, 32, app->last_load_ok ? name : "!! load failed");
    }

    // UID of the last sent tag — small, kept on screen too.
    canvas_set_font(canvas, FontSecondary);
    if(app->current >= 0 && app->last_load_ok && app->uid_str[0]) {
        char uid[56];
        snprintf(uid, sizeof(uid), "UID: %s", app->uid_str);
        draw_centered_trunc(canvas, 45, uid);
    }

    // Bottom: counter and hint.
    char bottom[32];
    if(app->total > 0) {
        snprintf(bottom, sizeof(bottom), "%lu/%u", (unsigned long)app->emitted, app->total);
    } else {
        snprintf(bottom, sizeof(bottom), "sent %lu", (unsigned long)app->emitted);
    }
    canvas_draw_str(canvas, 2, 62, bottom);
    canvas_draw_str(canvas, 74, 62, "OK/Back=stop");
}

static bool emu_input_cb(InputEvent* event, void* context) {
    App* app = context;
    if(event->type == InputTypeShort &&
       (event->key == InputKeyOk || event->key == InputKeyBack)) {
        run_stop(app);
        switch_view(app, ViewIdMenu);
        return true;
    }
    if(event->type == InputTypeShort || event->type == InputTypeRepeat) return true;
    return false;
}

/* ---------------- generate view ---------------- */

// Run the generation and switch to the confirmation screen.
static void do_generate(App* app) {
    int made = generate_tags(app, app->gen_count);
    app->src_mode = SrcGenerated; // point the source at the new generated set
    settings_save(app);
    tags_scan(app);
    set_source_item_text(app);
    app->gen_made = made;
    app->gen_result_shown = true; // show a confirmation instead of leaving silently
    switch_view(app, ViewIdGenerate);
    with_view_model(app->generate_view, ViewModel * m, { UNUSED(m); }, true);
}

// The text / URL editor accepted: gen_text is already filled; persist and make.
static void gen_text_done_cb(void* context) {
    App* app = context;
    settings_save(app);
    do_generate(app);
}

static void generate_draw_cb(Canvas* canvas, void* model) {
    App* app = ((ViewModel*)model)->app;
    canvas_clear(canvas);

    if(app->gen_result_shown) {
        canvas_set_font(canvas, FontPrimary);
        draw_centered_trunc(canvas, 28, "Generated");
        canvas_set_font(canvas, FontSecondary);
        char line[40];
        snprintf(line, sizeof(line), "%d new, %d total", app->gen_made, app->tag_count);
        draw_centered_trunc(canvas, 44, line);
        canvas_draw_str(canvas, 2, 62, "OK/Back = menu");
        return;
    }

    canvas_set_font(canvas, FontPrimary);
    draw_centered_trunc(canvas, 11, "Generate tags");

    canvas_set_font(canvas, FontBigNumbers);
    char num[8];
    snprintf(num, sizeof(num), "%d", app->gen_count);
    int w = canvas_string_width(canvas, num);
    canvas_draw_str(canvas, (128 - w) / 2, 34, num);

    canvas_set_font(canvas, FontSecondary);
    char dl[40];
    snprintf(dl, sizeof(dl), "Data: %s", gen_data_label[app->gen_data]);
    draw_centered_trunc(canvas, 45, dl);

    bool needs_text = (app->gen_data == GenDataText || app->gen_data == GenDataUrl);
    if(needs_text)
        draw_centered_trunc(canvas, 54, app->gen_text[0] ? app->gen_text : "(empty)");
    else
        draw_centered_trunc(canvas, 54, "<> data   ^v count");

    canvas_draw_str(canvas, 2, 63, "Back");
    canvas_draw_str(canvas, 86, 63, needs_text ? "OK=type" : "OK=make");
}

static bool generate_input_cb(InputEvent* event, void* context) {
    App* app = context;

    // Result screen: any OK/Back returns to the menu.
    if(app->gen_result_shown) {
        if(event->type == InputTypeShort &&
           (event->key == InputKeyOk || event->key == InputKeyBack)) {
            app->gen_result_shown = false;
            switch_view(app, ViewIdMenu);
            return true;
        }
        if(event->type == InputTypeShort || event->type == InputTypeRepeat) return true;
        return false;
    }

    // Up/Down change the count; Left/Right cycle the Data mode.
    bool up = (event->key == InputKeyUp);
    bool down = (event->key == InputKeyDown);
    int delta = 0;

    if(event->type == InputTypeShort && (up || down)) {
        app->gen_rep = 0;
        delta = up ? 1 : -1;
    } else if(event->type == InputTypeRepeat && (up || down)) {
        if(app->gen_rep < 255) app->gen_rep++;
        int step = (app->gen_rep <= 5) ? 5 : 10;
        delta = up ? step : -step;
    } else if(
        event->type == InputTypeShort &&
        (event->key == InputKeyLeft || event->key == InputKeyRight)) {
        int n = (int)app->gen_data + (event->key == InputKeyRight ? 1 : -1);
        if(n < 0) n = GenDataCount - 1;
        if(n >= (int)GenDataCount) n = 0;
        app->gen_data = (GenDataMode)n;
        with_view_model(app->generate_view, ViewModel * m, { UNUSED(m); }, true);
        return true;
    } else if(event->type == InputTypeShort && event->key == InputKeyOk) {
        if(app->gen_data == GenDataText || app->gen_data == GenDataUrl) {
            // Capture the text/URL first, then generate (done in gen_text_done_cb).
            text_input_reset(app->text_input);
            text_input_set_header_text(
                app->text_input, app->gen_data == GenDataUrl ? "Enter URL" : "Enter text");
            text_input_set_result_callback(
                app->text_input, gen_text_done_cb, app, app->gen_text, GEN_TEXT_LEN, false);
            switch_view(app, ViewIdGenText);
        } else {
            do_generate(app);
        }
        return true;
    } else if(event->type == InputTypeShort && event->key == InputKeyBack) {
        switch_view(app, ViewIdMenu);
        return true;
    }

    if(delta) {
        int v = app->gen_count + delta;
        if(v < 1) v = 1;
        if(v > MAX_GEN) v = MAX_GEN;
        app->gen_count = v;
        with_view_model(app->generate_view, ViewModel * m, { UNUSED(m); }, true);
        return true;
    }
    if(event->type == InputTypeShort || event->type == InputTypeRepeat) return true;
    return false;
}

/* ---------------- confirm-delete view ---------------- */

static void confirm_draw_cb(Canvas* canvas, void* model) {
    App* app = ((ViewModel*)model)->app;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    draw_centered_trunc(canvas, 18, "Delete generated?");
    canvas_set_font(canvas, FontSecondary);
    char line[40];
    snprintf(line, sizeof(line), "%d GEN-*.nfc file(s)", generated_count(app));
    draw_centered_trunc(canvas, 36, line);
    canvas_draw_str(canvas, 2, 62, "Back=no");
    canvas_draw_str(canvas, 86, 62, "OK=yes");
}

static bool confirm_input_cb(InputEvent* event, void* context) {
    App* app = context;
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        delete_generated(app);
        tags_scan(app);
        switch_view(app, ViewIdMenu);
        return true;
    }
    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        switch_view(app, ViewIdMenu);
        return true;
    }
    if(event->type == InputTypeShort || event->type == InputTypeRepeat) return true;
    return false;
}

/* ---------------- menu / settings ---------------- */

static void source_value_text(App* app, char* out, size_t n) {
    if(app->src_mode == SrcApp) {
        strncpy(out, "App", n - 1);
        out[n - 1] = '\0';
        return;
    }
    if(app->src_mode == SrcGenerated) {
        strncpy(out, "Generated", n - 1);
        out[n - 1] = '\0';
        return;
    }
    if(app->source[0] == '\0') {
        strncpy(out, "Browse", n - 1); // press OK on this row to pick a folder
        out[n - 1] = '\0';
        return;
    }
    FuriString* p = furi_string_alloc_set(app->source);
    FuriString* name = furi_string_alloc();
    path_extract_filename(p, name, false);
    const char* s = furi_string_get_cstr(name);
    strncpy(out, s[0] ? s : "folder", n - 1);
    out[n - 1] = '\0';
    furi_string_free(name);
    furi_string_free(p);
}

static void set_source_item_text(App* app) {
    if(!app->item_source) return;
    char buf[24];
    source_value_text(app, buf, sizeof(buf));
    variable_item_set_current_value_index(app->item_source, app->src_mode);
    variable_item_set_current_value_text(app->item_source, buf);
}

static void open_source_browser(App* app) {
    DialogsFileBrowserOptions opts;
    dialog_file_browser_set_basic_options(&opts, NFC_EXT, NULL);
    opts.base_path = NFC_START_DIR;

    FuriString* path = furi_string_alloc_set(app->source[0] ? app->source : NFC_START_DIR);
    if(dialog_file_browser_show(app->dialogs, path, path, &opts)) {
        FuriString* dir = furi_string_alloc();
        path_extract_dirname(furi_string_get_cstr(path), dir);
        strncpy(app->source, furi_string_get_cstr(dir), SRC_LEN - 1);
        app->source[SRC_LEN - 1] = '\0';
        furi_string_free(dir);
        app->src_mode = SrcCustom; // picking a folder selects the custom source
        set_source_item_text(app);
    }
    furi_string_free(path);
}

static void menu_cb(void* context, uint32_t index) {
    App* app = context;
    switch(index) {
    case MenuIndexStart:
        run_start(app);
        switch_view(app, ViewIdEmu);
        break;
    case MenuIndexSettings:
        switch_view(app, ViewIdSettings);
        break;
    case MenuIndexGenerate:
        app->gen_result_shown = false;
        switch_view(app, ViewIdGenerate);
        break;
    case MenuIndexDeleteGen:
        switch_view(app, ViewIdConfirmDel);
        break;
    case MenuIndexAbout:
        switch_view(app, ViewIdAbout);
        break;
    default:
        break;
    }
}

static void mode_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= ModeCount) idx = 0;
    app->mode = (PlayMode)idx;
    variable_item_set_current_value_text(item, mode_label[idx]);
}

static void timing_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= TimingCount) idx = 0;
    app->timing = (TimingMode)idx;
    variable_item_set_current_value_text(item, timing_label[idx]);
    // Delay vs. Codes/period+Period are shown per timing mode; rebuild the list
    // (deferred so we don't mutate it from inside its own change callback).
    view_dispatcher_send_custom_event(app->view_dispatcher, CustomEventRebuildSettings);
}

static void delay_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->interval = idx + INTERVAL_MIN;
    char text[8];
    snprintf(text, sizeof(text), "%d s", app->interval);
    variable_item_set_current_value_text(item, text);
}

static void per_period_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->per_period = idx + PER_PERIOD_MIN;
    char text[8];
    snprintf(text, sizeof(text), "%d", app->per_period);
    variable_item_set_current_value_text(item, text);
}

static void period_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= PERIOD_COUNT) idx = 0;
    app->period_s = period_choices[idx];
    char text[8];
    snprintf(text, sizeof(text), "%d s", app->period_s);
    variable_item_set_current_value_text(item, text);
}

static void total_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= TOTAL_COUNT) idx = 0;
    app->total = total_choices[idx];
    if(app->total == 0)
        variable_item_set_current_value_text(item, "Off");
    else {
        char text[8];
        snprintf(text, sizeof(text), "%d", app->total);
        variable_item_set_current_value_text(item, text);
    }
}

static void source_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= SrcCount) idx = 0;
    app->src_mode = (SourceMode)idx;
    char buf[24];
    source_value_text(app, buf, sizeof(buf));
    variable_item_set_current_value_text(item, buf);
}

static void led_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= LedCount) idx = 0;
    app->led = (LedMode)idx;
    variable_item_set_current_value_text(item, led_label[idx]);
}

static void vibro_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item) ? 1 : 0;
    app->vibro = idx ? true : false;
    variable_item_set_current_value_text(item, on_off_label[idx]);
}

static void beep_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item) ? 1 : 0;
    app->beep = idx ? true : false;
    variable_item_set_current_value_text(item, on_off_label[idx]);
}

static void hold_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= HOLD_COUNT) idx = 0;
    app->hold_ms = hold_choices[idx];
    variable_item_set_current_value_text(item, hold_labels[idx]);
}

static void gap_changed_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= GAP_COUNT) idx = 0;
    app->min_gap_ms = gap_choices[idx];
    variable_item_set_current_value_text(item, gap_labels[idx]);
}

static void settings_enter_cb(void* context, uint32_t index) {
    App* app = context;
    // Defer the blocking file browser out of this input callback (opening it
    // synchronously here deadlocks the view dispatcher).
    if(index == app->source_row)
        view_dispatcher_send_custom_event(app->view_dispatcher, CustomEventOpenSource);
}

static bool navigation_cb(void* context) {
    App* app = context;
    switch(app->current_view) {
    case ViewIdEmu:
        run_stop(app);
        switch_view(app, ViewIdMenu);
        return true;
    case ViewIdSettings:
        settings_save(app);
        switch_view(app, ViewIdMenu);
        return true;
    case ViewIdGenText:
        switch_view(app, ViewIdGenerate); // back from the editor to the generate screen
        return true;
    case ViewIdGenerate:
    case ViewIdConfirmDel:
    case ViewIdAbout:
        switch_view(app, ViewIdMenu);
        return true;
    case ViewIdMenu:
    default:
        return false;
    }
}

static bool custom_event_cb(void* context, uint32_t event) {
    App* app = context;
    if(event == CustomEventTick) {
        run_tick(app);
        with_view_model(app->emu_view, ViewModel * m, { UNUSED(m); }, true);
        return true;
    }
    if(event == CustomEventOpenSource) {
        open_source_browser(app);
        switch_view(app, ViewIdSettings); // restore the list after the dialog closes
        return true;
    }
    if(event == CustomEventRebuildSettings) {
        settings_populate(app);
        variable_item_list_set_selected_item(app->settings_list, RowTiming);
        return true;
    }
    return false;
}

static void timer_cb(void* ctx) {
    App* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, CustomEventTick);
}

/* ---------------- builders ---------------- */

static int index_of_u16(const uint16_t* arr, size_t n, uint16_t v) {
    for(size_t i = 0; i < n; i++)
        if(arr[i] == v) return (int)i;
    return 0;
}

// (Re)build the settings list. Only the rows relevant to the current timing
// mode are shown: Delay for Fixed, Codes/period + Period for Burst.
static void settings_populate(App* app) {
    VariableItemList* l = app->settings_list;
    variable_item_list_reset(l);
    variable_item_list_set_header(l, "Settings");
    char text[8];
    uint8_t row = 0;

    app->item_mode = variable_item_list_add(l, "Order", ModeCount, mode_changed_cb, app);
    variable_item_set_current_value_index(app->item_mode, app->mode);
    variable_item_set_current_value_text(app->item_mode, mode_label[app->mode]);
    row++;

    app->item_timing = variable_item_list_add(l, "Timing", TimingCount, timing_changed_cb, app);
    variable_item_set_current_value_index(app->item_timing, app->timing);
    variable_item_set_current_value_text(app->item_timing, timing_label[app->timing]);
    row++;

    if(app->timing == TimingFixed) {
        app->item_delay = variable_item_list_add(
            l, "Delay", INTERVAL_MAX - INTERVAL_MIN + 1, delay_changed_cb, app);
        variable_item_set_current_value_index(app->item_delay, app->interval - INTERVAL_MIN);
        snprintf(text, sizeof(text), "%d s", app->interval);
        variable_item_set_current_value_text(app->item_delay, text);
        row++;
        app->item_per_period = NULL;
        app->item_period = NULL;
    } else {
        app->item_per_period = variable_item_list_add(
            l, "Codes/period", PER_PERIOD_MAX - PER_PERIOD_MIN + 1, per_period_changed_cb, app);
        variable_item_set_current_value_index(app->item_per_period, app->per_period - PER_PERIOD_MIN);
        snprintf(text, sizeof(text), "%d", app->per_period);
        variable_item_set_current_value_text(app->item_per_period, text);
        row++;
        app->item_period = variable_item_list_add(l, "Period", PERIOD_COUNT, period_changed_cb, app);
        variable_item_set_current_value_index(
            app->item_period, index_of_u16(period_choices, PERIOD_COUNT, app->period_s));
        snprintf(text, sizeof(text), "%d s", app->period_s);
        variable_item_set_current_value_text(app->item_period, text);
        row++;

        int gi = index_of_u16(gap_choices, GAP_COUNT, app->min_gap_ms);
        VariableItem* it_gap = variable_item_list_add(l, "Min gap", GAP_COUNT, gap_changed_cb, app);
        variable_item_set_current_value_index(it_gap, gi);
        variable_item_set_current_value_text(it_gap, gap_labels[gi]);
        row++;
        app->item_delay = NULL;
    }

    int hi = index_of_u16(hold_choices, HOLD_COUNT, app->hold_ms);
    VariableItem* it_hold = variable_item_list_add(l, "Hold", HOLD_COUNT, hold_changed_cb, app);
    variable_item_set_current_value_index(it_hold, hi);
    variable_item_set_current_value_text(it_hold, hold_labels[hi]);
    row++;

    app->item_total = variable_item_list_add(l, "Total", TOTAL_COUNT, total_changed_cb, app);
    variable_item_set_current_value_index(
        app->item_total, index_of_u16(total_choices, TOTAL_COUNT, app->total));
    if(app->total == 0)
        variable_item_set_current_value_text(app->item_total, "Off");
    else {
        snprintf(text, sizeof(text), "%d", app->total);
        variable_item_set_current_value_text(app->item_total, text);
    }
    row++;

    app->source_row = row;
    app->item_source =
        variable_item_list_add(l, "Source", SrcCount, source_changed_cb, app);
    set_source_item_text(app);
    row++;

    app->item_led = variable_item_list_add(l, "LED", LedCount, led_changed_cb, app);
    variable_item_set_current_value_index(app->item_led, app->led);
    variable_item_set_current_value_text(app->item_led, led_label[app->led]);

    VariableItem* it_vibro = variable_item_list_add(l, "Vibro", 2, vibro_changed_cb, app);
    variable_item_set_current_value_index(it_vibro, app->vibro ? 1 : 0);
    variable_item_set_current_value_text(it_vibro, on_off_label[app->vibro ? 1 : 0]);

    VariableItem* it_beep = variable_item_list_add(l, "Beep", 2, beep_changed_cb, app);
    variable_item_set_current_value_index(it_beep, app->beep ? 1 : 0);
    variable_item_set_current_value_text(it_beep, on_off_label[app->beep ? 1 : 0]);

    variable_item_list_set_enter_callback(l, settings_enter_cb, app);
}

static void build_about(App* app) {
    app->about = widget_alloc();
    widget_add_string_element(app->about, 64, 4, AlignCenter, AlignTop, FontPrimary, APP_NAME);
    widget_add_string_multiline_element(
        app->about,
        2,
        20,
        AlignLeft,
        AlignTop,
        FontSecondary,
        "NFC reader / access test.\n"
        "Cycles saved .nfc tags,\n"
        "fixed or random timing.\n"
        "v0.6  Athos / RSC");
}

static View* make_custom_view(App* app, ViewDrawCallback draw, ViewInputCallback input) {
    View* v = view_alloc();
    view_set_context(v, app);
    view_allocate_model(v, ViewModelTypeLocking, sizeof(ViewModel));
    with_view_model(v, ViewModel * m, { m->app = app; }, false);
    view_set_draw_callback(v, draw);
    view_set_input_callback(v, input);
    return v;
}

static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));

    app->current = -1;
    app->gen_count = GEN_DEFAULT;

    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->notif = furi_record_open(RECORD_NOTIFICATION);
    app->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);
    app->tick_freq = furi_kernel_get_tick_frequency();

    settings_load(app);
    tags_scan(app);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, navigation_cb);

    app->menu = submenu_alloc();
    submenu_set_header(app->menu, APP_NAME);
    submenu_add_item(app->menu, "Start", MenuIndexStart, menu_cb, app);
    submenu_add_item(app->menu, "Settings", MenuIndexSettings, menu_cb, app);
    submenu_add_item(app->menu, "Generate tags", MenuIndexGenerate, menu_cb, app);
    submenu_add_item(app->menu, "Delete generated", MenuIndexDeleteGen, menu_cb, app);
    submenu_add_item(app->menu, "About", MenuIndexAbout, menu_cb, app);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdMenu, submenu_get_view(app->menu));

    app->settings_list = variable_item_list_alloc();
    settings_populate(app);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewIdSettings, variable_item_list_get_view(app->settings_list));

    app->emu_view = make_custom_view(app, emu_draw_cb, emu_input_cb);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdEmu, app->emu_view);

    app->generate_view = make_custom_view(app, generate_draw_cb, generate_input_cb);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdGenerate, app->generate_view);

    app->confirm_view = make_custom_view(app, confirm_draw_cb, confirm_input_cb);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdConfirmDel, app->confirm_view);

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ViewIdGenText, text_input_get_view(app->text_input));

    build_about(app);
    view_dispatcher_add_view(app->view_dispatcher, ViewIdAbout, widget_get_view(app->about));

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    app->current_view = ViewIdMenu;
    return app;
}

static void app_free(App* app) {
    if(app->running) run_stop(app);

    view_dispatcher_remove_view(app->view_dispatcher, ViewIdMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdSettings);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdEmu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdGenerate);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdGenText);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdConfirmDel);
    view_dispatcher_remove_view(app->view_dispatcher, ViewIdAbout);

    submenu_free(app->menu);
    variable_item_list_free(app->settings_list);
    view_free(app->emu_view);
    view_free(app->generate_view);
    view_free(app->confirm_view);
    text_input_free(app->text_input);
    widget_free(app->about);

    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    furi_timer_free(app->timer);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);

    free(app);
}

int32_t rsc_nfc_sequencer_app(void* p) {
    UNUSED(p);
    App* app = app_alloc();
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewIdMenu);
    view_dispatcher_run(app->view_dispatcher);
    app_free(app);
    return 0;
}
