#include "features.h"
#include "autodrive_notice.h"
#include "fork_text.h"
#include "key_bindings.h"
#include "logging.h"
#include "support.h"

/* "Minha música": plays a folder of the player's own songs through the radio's Xbox Music path,
   which the game still ships. RadioSystem slot 22 opens a URL with Media Foundation
   (MFCreateSourceReaderFromURL also takes a local path), decodes it and feeds FMOD, and shows
   the title and artist like an Xbox Music song. MusicToggle [Home] turns it on (the radio
   enters its Xbox Music mode, which stops the station) or off; MusicNext [End] skips. A
   finished song is followed by the next one, and changing station with the D-pad leaves the
   mode. Azimuth only lets the game open real folders inside Mount (no links, ASCII names), so
   songs go in Mount\LocalMusic: the mod scans it through its host path (beside the game EXE)
   and hands each song to the game as game:\LocalMusic\... FH2_Music.ini beside
   FH2_ExtraOptions.ini can change the folders, the file types and the volume. Keys are read on
   the UI frame and the radio is driven from ForzaSimulation's update on the game thread. */

#define RADIO_SYSTEM_RVA 0x365d410u
#define ENTER_XBOX_MUSIC_RVA 0x1022b60u
#define RADIO_SET_STATION_SLOT 8
#define RADIO_PLAY_SLOT 22
#define RADIO_STOP_SLOT 23
/* RadioSystem fields: +0x85 Xbox Music mode, +0x90 streaming player (+0 enabled when Media
   Foundation started, +8 current SongStream, cleared when a song ends). */
#define RADIO_XBOX_MUSIC_MODE 0x85u
#define RADIO_PLAYER_ENABLED 0x90u
#define RADIO_PLAYER_STREAM 0x98u
/* The player's audio sink (+0x18 of the player) plays the event
   event:/Master/Radio/XboxLiveStreaming; its slot 0 creates that event instance (+0x30) and
   is called once when the radio is built. This build never sets the audio engine flag
   (+0x548) that makes its factory hand out the FMOD sink, so the radio gets a silent
   placeholder; the FMOD sink is built directly from the engine's FMOD Studio system. */
#define RADIO_PLAYER_SINK 0xa8u
#define SINK_FMOD_VTABLE_RVA 0x2e76190u
#define SINK_EVENT_INSTANCE 0x30u
#define AUDIO_ENGINE_RVA 0x365d3d8u
#define AUDIO_ENGINE_STUDIO_SYSTEM 0x7f0u
#define CREATE_FMOD_SINK_RVA 0xfeeec0u
#define SHARED_ASSIGN_RVA 0x11ce8e0u
#define EVENT_SET_VOLUME_RVA 0x1a9b2b0u
/* The sink's programmer-sound callback (FUN_141018fe0) creates a user PCM stream with a
   0x1000-sample decode buffer (about 93 ms); Media Foundation under Wine delivers in bursts,
   so a larger buffer avoids brief underruns (clicks). The immediate is decodebuffersize. */
#define STREAM_BUFFER_RVA 0x101904eu

#define MAX_SONGS 4096
#define MAX_EXCLUDES 8
#define NAME_CAPACITY 256
#define FOLDER_CAPACITY 1024
#define MAX_DEPTH 8

typedef void (*EnterFn)(void *);
typedef void (*SetStationFn)(void *, void *);
typedef void (*PlayFn)(void *, const wchar_t *, const wchar_t *, const wchar_t *);
typedef void (*StopFn)(void *);
typedef char (*SinkInitFn)(void *, void *);
typedef void *(*CreateSinkFn)(void *, void *);
typedef void (*SharedAssignFn)(void *, void *);
typedef void (*ReleaseFn)(void *);
typedef void (*SetVolumeFn)(void *, float);

/* MSVC std::string with its 16-byte small buffer, as RadioSystem::SetStation takes it. */
typedef struct {
    char text[16];
    size_t size, capacity;
} SmallString;

/* Media Foundation under Wine opens MP3; FLAC and M4A do not open there. */
#define MAX_EXTENSIONS 8
static wchar_t extensions[MAX_EXTENSIONS][16];
static unsigned extension_count;

static unsigned char *game_image;
static int enabled, active, scanned;
static volatile LONG pending_toggle, pending_skip;
static wchar_t music_ini[PATH_CAPACITY], default_folder[FOLDER_CAPACITY];
static wchar_t excludes[MAX_EXCLUDES][NAME_CAPACITY];
static unsigned exclude_count;
static wchar_t *songs[MAX_SONGS];
static unsigned song_count, song_next, skipped_names;
static float volume = 2.0f;
static wchar_t game_folder[FOLDER_CAPACITY];
static size_t host_folder_length;

void local_music_set_paths(const wchar_t *directory, const wchar_t *executable_path) {
    if (!path_join(music_ini, directory, L"FH2_Music.ini")) {
        music_ini[0] = 0;
    }
    /* Default folder: LocalMusic beside the game EXE, which Azimuth maps as game:\. */
    size_t length = 0, folder_end = 0;
    for (; executable_path[length]; ++length) {
        if (executable_path[length] == L'\\' || executable_path[length] == L'/') {
            folder_end = length;
        }
    }
    default_folder[0] = 0;
    static const wchar_t name[] = L"\\LocalMusic";
    if (folder_end && folder_end + ARRAY_COUNT(name) < FOLDER_CAPACITY) {
        for (size_t i = 0; i < folder_end; ++i) {
            default_folder[i] = executable_path[i];
        }
        for (size_t i = 0; i < ARRAY_COUNT(name); ++i) {
            default_folder[folder_end + i] = name[i];
        }
    }
}

static wchar_t lower(wchar_t c) {
    return c >= L'A' && c <= L'Z' ? (wchar_t)(c - L'A' + L'a') : c;
}

static int wide_equal_ignore_case(const wchar_t *left, const wchar_t *right) {
    while (*left && lower(*left) == lower(*right)) {
        ++left;
        ++right;
    }
    return !*left && !*right;
}

static size_t wide_length(const wchar_t *text) {
    size_t length = 0;
    while (text[length]) {
        ++length;
    }
    return length;
}

static void wide_copy(wchar_t *out, size_t capacity, const wchar_t *text, size_t length) {
    if (length >= capacity) {
        length = capacity - 1;
    }
    for (size_t i = 0; i < length; ++i) {
        out[i] = text[i];
    }
    out[length] = 0;
}

static int is_song(const wchar_t *name) {
    const wchar_t *dot = 0;
    for (const wchar_t *c = name; *c; ++c) {
        if (*c == L'.') {
            dot = c;
        }
    }
    if (!dot) {
        return 0;
    }
    for (unsigned i = 0; i < extension_count; ++i) {
        if (wide_equal_ignore_case(dot + 1, extensions[i])) {
            return 1;
        }
    }
    return 0;
}

/* Azimuth stops the game on a content path outside the game's ANSI encoding. */
static int is_ascii(const wchar_t *text) {
    for (; *text; ++text) {
        if (*text > 0x7e || *text < 0x20) {
            return 0;
        }
    }
    return 1;
}

static int is_excluded(const wchar_t *name) {
    for (unsigned i = 0; i < exclude_count; ++i) {
        if (wide_equal_ignore_case(name, excludes[i])) {
            return 1;
        }
    }
    return 0;
}

static void scan_folder(const wchar_t *folder, unsigned depth) {
    size_t folder_length = wide_length(folder);
    if (depth > MAX_DEPTH || folder_length + 3 >= FOLDER_CAPACITY) {
        return;
    }
    wchar_t *pattern =
        (wchar_t *)HeapAlloc(GetProcessHeap(), 0, FOLDER_CAPACITY * sizeof(wchar_t));
    if (!pattern) {
        return;
    }
    wide_copy(pattern, FOLDER_CAPACITY, folder, folder_length);
    pattern[folder_length] = L'\\';
    pattern[folder_length + 1] = L'*';
    pattern[folder_length + 2] = 0;
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(pattern, &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const wchar_t *name = entry.cFileName;
            if (name[0] == L'.' && (!name[1] || (name[1] == L'.' && !name[2]))) {
                continue;
            }
            size_t name_length = wide_length(name);
            if (folder_length + 1 + name_length + 1 > FOLDER_CAPACITY) {
                continue;
            }
            wide_copy(pattern + folder_length + 1, FOLDER_CAPACITY - folder_length - 1, name,
                      name_length);
            if (!is_ascii(name)) {
                ++skipped_names;
                continue;
            }
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!is_excluded(name)) {
                    scan_folder(pattern, depth + 1);
                }
            } else if (is_song(name) && song_count < MAX_SONGS) {
                size_t length = folder_length + 1 + name_length;
                wchar_t *path = (wchar_t *)HeapAlloc(GetProcessHeap(), 0,
                                                     (length + 1) * sizeof(wchar_t));
                if (path) {
                    wide_copy(path, length + 1, pattern, length);
                    songs[song_count++] = path;
                }
            }
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
    HeapFree(GetProcessHeap(), 0, pattern);
}

static void shuffle_songs(void) {
    ULONGLONG state = GetTickCount64() | 1;
    for (unsigned i = song_count; i > 1; --i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        unsigned j = (unsigned)(state % i);
        wchar_t *swap = songs[i - 1];
        songs[i - 1] = songs[j];
        songs[j] = swap;
    }
}

/* Splits a ';'-separated list into fixed slots. */
static unsigned split_list(const wchar_t *list, wchar_t *slots, unsigned capacity,
                           size_t slot_length) {
    unsigned count = 0;
    const wchar_t *start = list;
    for (const wchar_t *c = list;; ++c) {
        if (*c == L';' || !*c) {
            while (start < c && *start == L' ') {
                ++start;
            }
            if (c > start && count < capacity) {
                wide_copy(slots + count * slot_length, slot_length, start, (size_t)(c - start));
                ++count;
            }
            if (!*c) {
                break;
            }
            start = c + 1;
        }
    }
    return count;
}

static void read_setting(const wchar_t *key, const wchar_t *fallback, wchar_t *out,
                         DWORD capacity) {
    if (music_ini[0]) {
        GetPrivateProfileStringW(L"Music", key, fallback, out, capacity, music_ini);
    } else {
        wide_copy(out, capacity, fallback, wide_length(fallback));
    }
}

static void load_playlist(void) {
    scanned = 1;
    wchar_t folder[FOLDER_CAPACITY], list[NAME_CAPACITY * 2], percent[16];
    read_setting(L"Folder", default_folder, folder, FOLDER_CAPACITY);
    read_setting(L"GameFolder", L"game:\\LocalMusic", game_folder, FOLDER_CAPACITY);
    read_setting(L"Exclude", L"", list, ARRAY_COUNT(list));
    exclude_count = split_list(list, excludes[0], MAX_EXCLUDES, NAME_CAPACITY);
    read_setting(L"Extensions", L"mp3", list, ARRAY_COUNT(list));
    extension_count = split_list(list, extensions[0], MAX_EXTENSIONS, ARRAY_COUNT(extensions[0]));
    /* Volume is a percentage of the event's own level, applied before the radio volume, so
       the game's radio slider still scales it. FMOD amplifies above 100. */
    read_setting(L"Volume", L"200", percent, ARRAY_COUNT(percent));
    unsigned value = 0;
    for (const wchar_t *c = percent; *c >= L'0' && *c <= L'9' && value < 10000; ++c) {
        value = value * 10 + (unsigned)(*c - L'0');
    }
    if (value) {
        volume = (float)(value > 800 ? 800 : value) / 100.0f;
    }
    size_t length = wide_length(folder);
    while (length && (folder[length - 1] == L'\\' || folder[length - 1] == L'/')) {
        folder[--length] = 0;
    }
    host_folder_length = length;
    size_t game_length = wide_length(game_folder);
    while (game_length &&
           (game_folder[game_length - 1] == L'\\' || game_folder[game_length - 1] == L'/')) {
        game_folder[--game_length] = 0;
    }
    if (length && game_length) {
        scan_folder(folder, 0);
    }
    shuffle_songs();
    char line[] = "MUSIC: 0000 songs found.\r\n";
    unsigned count = song_count;
    for (int i = 10; i >= 7; --i) {
        line[i] = (char)('0' + count % 10);
        count /= 10;
    }
    log_append(line);
    if (skipped_names) {
        log_append("MUSIC: names with accents or symbols were skipped; Azimuth cannot open "
                   "them.\r\n");
    }
}

/* Name parts from "01 - Artist - Title.ext"; without " - ", the folder is the artist. */
static void describe(const wchar_t *path, wchar_t *title, wchar_t *artist) {
    const wchar_t *name = path, *folder = path, *folder_end = path, *dot = 0;
    for (const wchar_t *c = path; *c; ++c) {
        if (*c == L'\\') {
            folder = name;
            folder_end = c;
            name = c + 1;
        }
    }
    for (const wchar_t *c = name; *c; ++c) {
        if (*c == L'.') {
            dot = c;
        }
    }
    const wchar_t *end = dot ? dot : name + wide_length(name);
    const wchar_t *stem = name;
    while (stem < end && *stem >= L'0' && *stem <= L'9') {
        ++stem;
    }
    if (stem > name && stem < end && (*stem == L' ' || *stem == L'.' || *stem == L'-')) {
        while (stem < end && (*stem == L' ' || *stem == L'.' || *stem == L'-')) {
            ++stem;
        }
    } else {
        stem = name;
    }
    const wchar_t *split = 0;
    for (const wchar_t *c = stem; c + 2 < end; ++c) {
        if (c[0] == L' ' && c[1] == L'-' && c[2] == L' ') {
            split = c;
            break;
        }
    }
    if (split) {
        wide_copy(artist, NAME_CAPACITY, stem, (size_t)(split - stem));
        wide_copy(title, NAME_CAPACITY, split + 3, (size_t)(end - split - 3));
    } else {
        wide_copy(title, NAME_CAPACITY, stem, (size_t)(end - stem));
        wide_copy(artist, NAME_CAPACITY, folder, (size_t)(folder_end - folder));
    }
}

/* ID3v2 text frame (TIT2/TPE1) into wide text: encodings 0 Latin-1, 1 UTF-16 with BOM,
   2 UTF-16BE, 3 UTF-8. Characters outside the BMP are dropped. */
static void id3_text(const unsigned char *data, size_t size, wchar_t *out) {
    size_t length = 0;
    if (!size) {
        out[0] = 0;
        return;
    }
    unsigned char encoding = data[0];
    const unsigned char *text = data + 1, *end = data + size;
    if (encoding == 1 || encoding == 2) {
        int big_endian = encoding == 2;
        if (encoding == 1 && end - text >= 2) {
            big_endian = text[0] == 0xfe && text[1] == 0xff;
            text += 2;
        }
        for (; text + 1 < end && length + 1 < NAME_CAPACITY; text += 2) {
            wchar_t c = big_endian ? (wchar_t)(text[0] << 8 | text[1])
                                   : (wchar_t)(text[1] << 8 | text[0]);
            if (!c) {
                break;
            }
            out[length++] = c;
        }
    } else {
        while (text < end && *text && length + 1 < NAME_CAPACITY) {
            unsigned c = *text++;
            if (encoding == 3 && c >= 0xc0) {
                int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
                c &= 0x3f >> extra;
                while (extra-- && text < end) {
                    c = c << 6 | (*text++ & 0x3f);
                }
                if (c > 0xffff) {
                    continue;
                }
            }
            out[length++] = (wchar_t)c;
        }
    }
    out[length] = 0;
}

/* Reads the title and artist from an ID3v2 tag at the start of the file: TIT2/TPE1 (v2.3,
   v2.4) or TT2/TP1 (v2.2). The whole tag is read, since cover art often comes first. */
static void read_tags(const wchar_t *path, wchar_t *title, wchar_t *artist) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    unsigned char header[10];
    DWORD read = 0;
    if (!ReadFile(file, header, sizeof(header), &read, 0) || read != sizeof(header) ||
        header[0] != 'I' || header[1] != 'D' || header[2] != '3' || header[3] < 2 ||
        header[3] > 4) {
        CloseHandle(file);
        return;
    }
    unsigned version = header[3];
    size_t size = (size_t)(header[6] & 0x7f) << 21 | (size_t)(header[7] & 0x7f) << 14 |
                  (size_t)(header[8] & 0x7f) << 7 | (header[9] & 0x7f);
    if (size > (8u << 20)) {
        size = 8u << 20;
    }
    unsigned char *tag = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
    if (!tag || !ReadFile(file, tag, (DWORD)size, &read, 0)) {
        if (tag) {
            HeapFree(GetProcessHeap(), 0, tag);
        }
        CloseHandle(file);
        return;
    }
    CloseHandle(file);
    size_t header_size = version == 2 ? 6 : 10, id_size = version == 2 ? 3 : 4;
    for (size_t at = 0; at + header_size <= read;) {
        const unsigned char *frame = tag + at;
        if (!frame[0]) {
            break;
        }
        size_t length;
        if (version == 2) {
            length = (size_t)frame[3] << 16 | (size_t)frame[4] << 8 | frame[5];
        } else if (version == 4) {
            length = (size_t)(frame[4] & 0x7f) << 21 | (size_t)(frame[5] & 0x7f) << 14 |
                     (size_t)(frame[6] & 0x7f) << 7 | (frame[7] & 0x7f);
        } else {
            length = (size_t)frame[4] << 24 | (size_t)frame[5] << 16 | (size_t)frame[6] << 8 |
                     frame[7];
        }
        if (at + header_size + length > read) {
            break;
        }
        int is_title = memory_equal(frame, version == 2 ? "TT2" : "TIT2", id_size);
        int is_artist = memory_equal(frame, version == 2 ? "TP1" : "TPE1", id_size);
        if (is_title || is_artist) {
            wchar_t value[NAME_CAPACITY];
            id3_text(frame + header_size, length, value);
            if (value[0]) {
                wide_copy(is_title ? title : artist, NAME_CAPACITY, value, wide_length(value));
            }
        }
        at += header_size + length;
    }
    HeapFree(GetProcessHeap(), 0, tag);
}

static void **radio_vtable(unsigned char *radio) {
    return *(void ***)radio;
}

static void leave_mode(unsigned char *radio, const char *reason) {
    active = 0;
    if (radio && radio[RADIO_XBOX_MUSIC_MODE]) {
        ((StopFn)radio_vtable(radio)[RADIO_STOP_SLOT])(radio);
        SmallString station = {"StationOff", 10, 15};
        ((SetStationFn)radio_vtable(radio)[RADIO_SET_STATION_SLOT])(radio, &station);
    }
    log_append(reason);
}

static void play_next(unsigned char *radio) {
    for (unsigned attempt = 0; attempt < song_count; ++attempt) {
        const wchar_t *path = songs[song_next++ % song_count];
        wchar_t title[NAME_CAPACITY], artist[NAME_CAPACITY], url[FOLDER_CAPACITY];
        describe(path, title, artist);
        read_tags(path, title, artist);
        /* Same file under the game path: GameFolder + the part after Folder. */
        size_t game_length = wide_length(game_folder), rest = wide_length(path) - host_folder_length;
        if (game_length + rest + 1 > FOLDER_CAPACITY) {
            continue;
        }
        wide_copy(url, FOLDER_CAPACITY, game_folder, game_length);
        wide_copy(url + game_length, FOLDER_CAPACITY - game_length, path + host_folder_length, rest);
        ((PlayFn)radio_vtable(radio)[RADIO_PLAY_SLOT])(radio, url, title, artist);
        if (*(void **)(radio + RADIO_PLAYER_STREAM)) {
            unsigned char *sink = *(unsigned char **)(radio + RADIO_PLAYER_SINK);
            void *instance = sink ? *(void **)(sink + SINK_EVENT_INSTANCE) : 0;
            if (instance) {
                ((SetVolumeFn)(game_image + EVENT_SET_VOLUME_RVA))(instance, volume);
            }
            wchar_t value[48] = {0};
            size_t length = wide_length(artist);
            wide_copy(value, ARRAY_COUNT(value), artist, length);
            if (length + 3 < ARRAY_COUNT(value)) {
                value[length] = L' ';
                value[length + 1] = 0x2013;
                value[length + 2] = L' ';
                wide_copy(value + length + 3, ARRAY_COUNT(value) - length - 3, title,
                          wide_length(title));
            }
            autodrive_notice_show_text(fork_text(game_image)->now_playing, value);
            log_append("MUSIC: playing the next song.\r\n");
            return;
        }
        log_append("MUSIC: a song could not be opened by Media Foundation; skipped.\r\n");
    }
    leave_mode(radio, "MUSIC: no song could be opened; left the Xbox Music mode.\r\n");
    autodrive_notice_show_text(fork_text(game_image)->music, fork_text(game_image)->music_failed);
}

static void release_shared(void **pair) {
    unsigned char *control = (unsigned char *)pair[1];
    if (control && InterlockedDecrement((volatile LONG *)(control + 8)) == 0) {
        ((ReleaseFn)(*(void ***)control)[0])(control);
        if (InterlockedDecrement((volatile LONG *)(control + 0xc)) == 0) {
            ((ReleaseFn)(*(void ***)control)[1])(control);
        }
    }
}

/* Makes sure the streaming sink has its FMOD event, rebuilding it the way the radio did. */
static void prepare_sink(unsigned char *radio) {
    unsigned char *player = radio + RADIO_PLAYER_ENABLED;
    unsigned char *sink = *(unsigned char **)(radio + RADIO_PLAYER_SINK);
    if (!sink || *(void **)sink != game_image + SINK_FMOD_VTABLE_RVA) {
        unsigned char *engine = *(unsigned char **)(game_image + AUDIO_ENGINE_RVA);
        if (!engine) {
            log_append("MUSIC: no audio engine to build the streaming sink.\r\n");
            return;
        }
        if (!*(void **)(engine + AUDIO_ENGINE_STUDIO_SYSTEM)) {
            log_append("MUSIC: the audio engine has no FMOD Studio system.\r\n");
            return;
        }
        void *created[2] = {0, 0};
        ((CreateSinkFn)(game_image + CREATE_FMOD_SINK_RVA))(created,
                                                            engine + AUDIO_ENGINE_STUDIO_SYSTEM);
        ((SharedAssignFn)(game_image + SHARED_ASSIGN_RVA))(radio + RADIO_PLAYER_SINK, created);
        release_shared(created);
        sink = *(unsigned char **)(radio + RADIO_PLAYER_SINK);
        if (!sink || *(void **)sink != game_image + SINK_FMOD_VTABLE_RVA) {
            log_append("MUSIC: the FMOD streaming sink could not be built.\r\n");
            return;
        }
        log_append("MUSIC: replaced the radio's silent placeholder with an FMOD sink.\r\n");
    }
    if (!*(void **)(sink + SINK_EVENT_INSTANCE)) {
        char ok = ((SinkInitFn)(*(void ***)sink)[0])(sink, player);
        log_append(ok ? "MUSIC: created the XboxLiveStreaming event instance.\r\n"
                      : "MUSIC: FMOD could not create the XboxLiveStreaming event.\r\n");
    }
}

static void toggle(unsigned char *radio) {
    if (active) {
        leave_mode(radio, "MUSIC: turned off.\r\n");
        autodrive_notice_show_text(fork_text(game_image)->music, fork_text(game_image)->music_off);
        return;
    }
    if (!scanned) {
        load_playlist();
    }
    if (!song_count) {
        autodrive_notice_show_text(fork_text(game_image)->music, fork_text(game_image)->music_empty);
        return;
    }
    if (!radio[RADIO_PLAYER_ENABLED]) {
        log_append("MUSIC: the radio's Media Foundation player did not start.\r\n");
        autodrive_notice_show_text(fork_text(game_image)->music,
                                   fork_text(game_image)->music_unavailable);
        return;
    }
    prepare_sink(radio);
    if (!radio[RADIO_XBOX_MUSIC_MODE]) {
        ((EnterFn)(game_image + ENTER_XBOX_MUSIC_RVA))(radio);
    }
    active = 1;
    log_append("MUSIC: turned on.\r\n");
    play_next(radio);
}

void local_music_update(void) {
    if (!enabled) {
        return;
    }
    unsigned char *radio = *(unsigned char **)(game_image + RADIO_SYSTEM_RVA);
    if (!radio) {
        return;
    }
    if (InterlockedExchange(&pending_toggle, 0)) {
        toggle(radio);
    }
    if (!active) {
        InterlockedExchange(&pending_skip, 0);
        return;
    }
    if (!radio[RADIO_XBOX_MUSIC_MODE]) {
        /* The player picked a station with the D-pad. */
        active = 0;
        log_append("MUSIC: left because the radio changed station.\r\n");
        return;
    }
    if (InterlockedExchange(&pending_skip, 0)) {
        ((StopFn)radio_vtable(radio)[RADIO_STOP_SLOT])(radio);
        play_next(radio);
    } else if (!*(void **)(radio + RADIO_PLAYER_STREAM)) {
        play_next(radio);
    }
}

void local_music_frame(void) {
    if (!enabled) {
        return;
    }
    if (key_bindings_pressed(KEY_MUSIC_TOGGLE)) {
        InterlockedIncrement(&pending_toggle);
    }
    if (key_bindings_pressed(KEY_MUSIC_NEXT)) {
        InterlockedIncrement(&pending_skip);
    }
}

static const unsigned char buffer_original[8] = {0xc7, 0x44, 0x24, 0x48, 0x00, 0x10, 0x00, 0x00};
static const unsigned char buffer_larger[8] = {0xc7, 0x44, 0x24, 0x48, 0x00, 0x40, 0x00, 0x00};
static const unsigned char *const buffer_accepted[] = {buffer_original, buffer_larger};
PatchDefinition local_music_buffer_patch = {STREAM_BUFFER_RVA,
                                            0,
                                            8,
                                            GROUP_LOCAL_MUSIC,
                                            "local_music_stream_buffer",
                                            buffer_original,
                                            buffer_larger,
                                            buffer_larger,
                                            buffer_accepted,
                                            2};

void local_music_prepare(unsigned char *image, const ModOptions *options) {
    game_image = image;
    enabled = options->local_music;
}
