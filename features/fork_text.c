#include "fork_text.h"
#include "game_addresses.h"
#include "support.h"

/* On-screen notice texts for the time, weather and music keys, in the game's text languages.
   Presets follow media/weather/Presets_Island.xml (14); the mainland list (9) is a subset. */

static const ForkText english = {
    .hour = L"HOUR",
    .weather = L"WEATHER",
    .rain = L"RAIN",
    .clock = L"CLOCK",
    .clock_speed = L"TIME SPEED",
    .now_playing = L"NOW PLAYING",
    .music = L"MY MUSIC",
    .locked = L"Locked by the event",
    .weather_automatic = L"Automatic",
    .weather_frozen = L"Frozen",
    .rain_automatic = L"Automatic",
    .rain_levels = {L"No rain", L"Light", L"Medium", L"Heavy"},
    .clock_states = {L"Running", L"Stopped", L"Real time"},
    .speeds = {L"Normal", L"4×", L"16×", L"0.25×"},
    .music_off = L"Off",
    .music_empty = L"No songs in the folder",
    .music_unavailable = L"Player unavailable",
    .music_failed = L"No song could be opened",
    .presets = {L"Clear sky", L"Cloudy A", L"Cloudy B", L"Cloudy with fog", L"Rain threat A1",
                L"Rain threat A2", L"Rain threat C1", L"Rain threat C2", L"Fog threat",
                L"Storm 1", L"Storm 2", L"Tropical storm 1", L"Tropical storm 2",
                L"Tropical storm with fog"}};

static const ForkText brazilian = {
    .hour = L"HORA",
    .weather = L"CLIMA",
    .rain = L"CHUVA",
    .clock = L"RELÓGIO",
    .clock_speed = L"VELOCIDADE DO TEMPO",
    .now_playing = L"TOCANDO",
    .music = L"MINHA MÚSICA",
    .locked = L"Bloqueada pelo evento",
    .weather_automatic = L"Automático",
    .weather_frozen = L"Congelado",
    .rain_automatic = L"Automática",
    .rain_levels = {L"Sem chuva", L"Fraca", L"Média", L"Forte"},
    .clock_states = {L"Andando", L"Parado", L"Hora real"},
    .speeds = {L"Normal", L"4×", L"16×", L"0,25×"},
    .music_off = L"Desligada",
    .music_empty = L"Nenhuma música na pasta",
    .music_unavailable = L"Player indisponível",
    .music_failed = L"Nenhuma música abriu",
    .presets = {L"Céu limpo", L"Nublado A", L"Nublado B", L"Nublado com neblina",
                L"Ameaça de chuva A1", L"Ameaça de chuva A2", L"Ameaça de chuva C1",
                L"Ameaça de chuva C2", L"Ameaça com neblina", L"Tempestade 1", L"Tempestade 2",
                L"Tempestade tropical 1", L"Tempestade tropical 2",
                L"Tempestade tropical com neblina"}};

static const ForkText german = {
    .hour = L"UHRZEIT",
    .weather = L"WETTER",
    .rain = L"REGEN",
    .clock = L"UHR",
    .clock_speed = L"ZEITGESCHWINDIGKEIT",
    .now_playing = L"WIRD GESPIELT",
    .music = L"MEINE MUSIK",
    .locked = L"Vom Event gesperrt",
    .weather_automatic = L"Automatisch",
    .weather_frozen = L"Eingefroren",
    .rain_automatic = L"Automatisch",
    .rain_levels = {L"Kein Regen", L"Leicht", L"Mittel", L"Stark"},
    .clock_states = {L"Läuft", L"Angehalten", L"Echtzeit"},
    .speeds = {L"Normal", L"4×", L"16×", L"0,25×"},
    .music_off = L"Aus",
    .music_empty = L"Keine Songs im Ordner",
    .music_unavailable = L"Player nicht verfügbar",
    .music_failed = L"Kein Song ließ sich öffnen",
    .presets = {L"Klarer Himmel", L"Bewölkt A", L"Bewölkt B", L"Bewölkt mit Nebel",
                L"Regengefahr A1", L"Regengefahr A2", L"Regengefahr C1", L"Regengefahr C2",
                L"Nebelgefahr", L"Sturm 1", L"Sturm 2", L"Tropensturm 1", L"Tropensturm 2",
                L"Tropensturm mit Nebel"}};

static const ForkText french = {
    .hour = L"HEURE",
    .weather = L"MÉTÉO",
    .rain = L"PLUIE",
    .clock = L"HORLOGE",
    .clock_speed = L"VITESSE DU TEMPS",
    .now_playing = L"EN LECTURE",
    .music = L"MA MUSIQUE",
    .locked = L"Verrouillée par l'épreuve",
    .weather_automatic = L"Automatique",
    .weather_frozen = L"Figée",
    .rain_automatic = L"Automatique",
    .rain_levels = {L"Pas de pluie", L"Faible", L"Moyenne", L"Forte"},
    .clock_states = {L"En marche", L"Arrêtée", L"Heure réelle"},
    .speeds = {L"Normale", L"4×", L"16×", L"0,25×"},
    .music_off = L"Désactivée",
    .music_empty = L"Aucun morceau dans le dossier",
    .music_unavailable = L"Lecteur indisponible",
    .music_failed = L"Aucun morceau n'a pu être ouvert",
    .presets = {L"Ciel dégagé", L"Nuageux A", L"Nuageux B", L"Nuageux avec brouillard",
                L"Menace de pluie A1", L"Menace de pluie A2", L"Menace de pluie C1",
                L"Menace de pluie C2", L"Menace de brouillard", L"Orage 1", L"Orage 2",
                L"Tempête tropicale 1", L"Tempête tropicale 2",
                L"Tempête tropicale avec brouillard"}};

static const ForkText spanish = {
    .hour = L"HORA",
    .weather = L"CLIMA",
    .rain = L"LLUVIA",
    .clock = L"RELOJ",
    .clock_speed = L"VELOCIDAD DEL TIEMPO",
    .now_playing = L"REPRODUCIENDO",
    .music = L"MI MÚSICA",
    .locked = L"Bloqueada por la prueba",
    .weather_automatic = L"Automático",
    .weather_frozen = L"Congelado",
    .rain_automatic = L"Automática",
    .rain_levels = {L"Sin lluvia", L"Ligera", L"Moderada", L"Intensa"},
    .clock_states = {L"En marcha", L"Detenido", L"Hora real"},
    .speeds = {L"Normal", L"4×", L"16×", L"0,25×"},
    .music_off = L"Desactivada",
    .music_empty = L"No hay canciones en la carpeta",
    .music_unavailable = L"Reproductor no disponible",
    .music_failed = L"No se pudo abrir ninguna canción",
    .presets = {L"Cielo despejado", L"Nublado A", L"Nublado B", L"Nublado con niebla",
                L"Amenaza de lluvia A1", L"Amenaza de lluvia A2", L"Amenaza de lluvia C1",
                L"Amenaza de lluvia C2", L"Amenaza de niebla", L"Tormenta 1", L"Tormenta 2",
                L"Tormenta tropical 1", L"Tormenta tropical 2", L"Tormenta tropical con niebla"}};

static const ForkText mexican = {
    .hour = L"HORA",
    .weather = L"CLIMA",
    .rain = L"LLUVIA",
    .clock = L"RELOJ",
    .clock_speed = L"VELOCIDAD DEL TIEMPO",
    .now_playing = L"REPRODUCIENDO",
    .music = L"MI MÚSICA",
    .locked = L"Bloqueada por el evento",
    .weather_automatic = L"Automático",
    .weather_frozen = L"Congelado",
    .rain_automatic = L"Automática",
    .rain_levels = {L"Sin lluvia", L"Ligera", L"Moderada", L"Intensa"},
    .clock_states = {L"En marcha", L"Detenido", L"Hora real"},
    .speeds = {L"Normal", L"4×", L"16×", L"0.25×"},
    .music_off = L"Desactivada",
    .music_empty = L"No hay canciones en la carpeta",
    .music_unavailable = L"Reproductor no disponible",
    .music_failed = L"No se pudo abrir ninguna canción",
    .presets = {L"Cielo despejado", L"Nublado A", L"Nublado B", L"Nublado con niebla",
                L"Amenaza de lluvia A1", L"Amenaza de lluvia A2", L"Amenaza de lluvia C1",
                L"Amenaza de lluvia C2", L"Amenaza de niebla", L"Tormenta 1", L"Tormenta 2",
                L"Tormenta tropical 1", L"Tormenta tropical 2", L"Tormenta tropical con niebla"}};

static const ForkText italian = {
    .hour = L"ORA",
    .weather = L"METEO",
    .rain = L"PIOGGIA",
    .clock = L"OROLOGIO",
    .clock_speed = L"VELOCITÀ DEL TEMPO",
    .now_playing = L"IN RIPRODUZIONE",
    .music = L"LA MIA MUSICA",
    .locked = L"Bloccata dall'evento",
    .weather_automatic = L"Automatico",
    .weather_frozen = L"Congelato",
    .rain_automatic = L"Automatica",
    .rain_levels = {L"Niente pioggia", L"Leggera", L"Media", L"Forte"},
    .clock_states = {L"In corso", L"Fermo", L"Ora reale"},
    .speeds = {L"Normale", L"4×", L"16×", L"0,25×"},
    .music_off = L"Disattivata",
    .music_empty = L"Nessun brano nella cartella",
    .music_unavailable = L"Lettore non disponibile",
    .music_failed = L"Nessun brano è stato aperto",
    .presets = {L"Cielo sereno", L"Nuvoloso A", L"Nuvoloso B", L"Nuvoloso con nebbia",
                L"Minaccia di pioggia A1", L"Minaccia di pioggia A2", L"Minaccia di pioggia C1",
                L"Minaccia di pioggia C2", L"Minaccia di nebbia", L"Temporale 1",
                L"Temporale 2", L"Tempesta tropicale 1", L"Tempesta tropicale 2",
                L"Tempesta tropicale con nebbia"}};

static const ForkText polish = {
    .hour = L"GODZINA",
    .weather = L"POGODA",
    .rain = L"DESZCZ",
    .clock = L"ZEGAR",
    .clock_speed = L"PRĘDKOŚĆ CZASU",
    .now_playing = L"ODTWARZANE",
    .music = L"MOJA MUZYKA",
    .locked = L"Zablokowana przez wydarzenie",
    .weather_automatic = L"Automatyczna",
    .weather_frozen = L"Zamrożona",
    .rain_automatic = L"Automatyczny",
    .rain_levels = {L"Bez deszczu", L"Lekki", L"Umiarkowany", L"Ulewny"},
    .clock_states = {L"Działa", L"Zatrzymany", L"Czas rzeczywisty"},
    .speeds = {L"Normalna", L"4×", L"16×", L"0,25×"},
    .music_off = L"Wyłączona",
    .music_empty = L"Brak utworów w folderze",
    .music_unavailable = L"Odtwarzacz niedostępny",
    .music_failed = L"Nie udało się otworzyć żadnego utworu",
    .presets = {L"Bezchmurnie", L"Pochmurno A", L"Pochmurno B", L"Pochmurno z mgłą",
                L"Groźba deszczu A1", L"Groźba deszczu A2", L"Groźba deszczu C1",
                L"Groźba deszczu C2", L"Groźba mgły", L"Burza 1", L"Burza 2",
                L"Burza tropikalna 1", L"Burza tropikalna 2", L"Burza tropikalna z mgłą"}};

static const ForkText russian = {
    .hour = L"ВРЕМЯ СУТОК",
    .weather = L"ПОГОДА",
    .rain = L"ДОЖДЬ",
    .clock = L"ЧАСЫ",
    .clock_speed = L"СКОРОСТЬ ВРЕМЕНИ",
    .now_playing = L"СЕЙЧАС ИГРАЕТ",
    .music = L"МОЯ МУЗЫКА",
    .locked = L"Заблокировано событием",
    .weather_automatic = L"Автоматически",
    .weather_frozen = L"Заморожена",
    .rain_automatic = L"Автоматически",
    .rain_levels = {L"Без дождя", L"Слабый", L"Умеренный", L"Сильный"},
    .clock_states = {L"Идут", L"Остановлены", L"Реальное время"},
    .speeds = {L"Обычная", L"4×", L"16×", L"0,25×"},
    .music_off = L"Выключена",
    .music_empty = L"В папке нет песен",
    .music_unavailable = L"Плеер недоступен",
    .music_failed = L"Не удалось открыть ни одной песни",
    .presets = {L"Ясно", L"Облачно A", L"Облачно B", L"Облачно с туманом", L"Угроза дождя A1",
                L"Угроза дождя A2", L"Угроза дождя C1", L"Угроза дождя C2", L"Угроза тумана",
                L"Гроза 1", L"Гроза 2", L"Тропический шторм 1", L"Тропический шторм 2",
                L"Тропический шторм с туманом"}};

static const ForkText japanese = {
    .hour = L"時刻",
    .weather = L"天候",
    .rain = L"雨",
    .clock = L"時計",
    .clock_speed = L"時間の速さ",
    .now_playing = L"再生中",
    .music = L"マイミュージック",
    .locked = L"イベント中はロックされています",
    .weather_automatic = L"自動",
    .weather_frozen = L"固定",
    .rain_automatic = L"自動",
    .rain_levels = {L"雨なし", L"小雨", L"中程度", L"大雨"},
    .clock_states = {L"進行中", L"停止中", L"現実の時刻"},
    .speeds = {L"標準", L"4×", L"16×", L"0.25×"},
    .music_off = L"オフ",
    .music_empty = L"フォルダーに曲がありません",
    .music_unavailable = L"プレーヤーを使用できません",
    .music_failed = L"開ける曲がありません",
    .presets = {L"快晴", L"曇り A", L"曇り B", L"曇り（霧）", L"雨の気配 A1", L"雨の気配 A2",
                L"雨の気配 C1", L"雨の気配 C2", L"霧の気配", L"嵐 1", L"嵐 2", L"熱帯嵐 1",
                L"熱帯嵐 2", L"熱帯嵐（霧）"}};

static const ForkText chinese = {
    .hour = L"時間",
    .weather = L"天氣",
    .rain = L"雨量",
    .clock = L"時鐘",
    .clock_speed = L"時間流速",
    .now_playing = L"正在播放",
    .music = L"我的音樂",
    .locked = L"已被活動鎖定",
    .weather_automatic = L"自動",
    .weather_frozen = L"凍結",
    .rain_automatic = L"自動",
    .rain_levels = {L"無雨", L"小雨", L"中雨", L"大雨"},
    .clock_states = {L"運行中", L"已停止", L"即時時間"},
    .speeds = {L"正常", L"4×", L"16×", L"0.25×"},
    .music_off = L"關閉",
    .music_empty = L"資料夾中沒有歌曲",
    .music_unavailable = L"播放器無法使用",
    .music_failed = L"沒有可開啟的歌曲",
    .presets = {L"晴朗", L"多雲 A", L"多雲 B", L"多雲有霧", L"可能降雨 A1", L"可能降雨 A2",
                L"可能降雨 C1", L"可能降雨 C2", L"可能起霧", L"暴風雨 1", L"暴風雨 2",
                L"熱帶風暴 1", L"熱帶風暴 2", L"熱帶風暴有霧"}};

static const struct {
    const char *code;
    const ForkText *text;
} translations[] = {{"EN", &english},  {"GB", &english},  {"BR", &brazilian}, {"DE", &german},
                    {"FR", &french},   {"ES", &spanish},  {"MX", &mexican},   {"IT", &italian},
                    {"JP", &japanese}, {"CHT", &chinese}, {"PL", &polish},    {"RU", &russian}};

/* Mainland presets (Presets.xml) are these entries of the island list. */
static const unsigned char mainland_order[9] = {0, 1, 2, 4, 5, 6, 7, 9, 10};

const ForkText *fork_text(unsigned char *game_image) {
    /* The game's text tables keep the active language code (BR, EN, ...) as a short string. */
    void *manager;
    unsigned char code[32];
    size_t length, capacity;
    if (!game_image || !memory_read(game_image + GAME_RVA_TEXT_TABLES, &manager, sizeof(manager)) ||
        !manager || !memory_read((unsigned char *)manager + 0x30, code, sizeof(code))) {
        return &english;
    }
    memory_copy(&length, code + 16, sizeof(length));
    memory_copy(&capacity, code + 24, sizeof(capacity));
    if (capacity != 15 || length < 2 || length > 3 || code[length] != 0) {
        return &english;
    }
    for (unsigned index = 0; index < ARRAY_COUNT(translations); ++index) {
        if (text_equal_ignore_case((const char *)code, translations[index].code)) {
            return translations[index].text;
        }
    }
    return &english;
}

const wchar_t *fork_text_preset(const ForkText *text, int index, int count) {
    if (count == 14 && index >= 0 && index < 14) {
        return text->presets[index];
    }
    if (count == 9 && index >= 0 && index < 9) {
        return text->presets[mainland_order[index]];
    }
    return 0;
}
