/*
 * Arcade Station v0.1 - emulador arcade (nucleo FBNeo) para PS3, solo LOCAL en esta version (basado en Katari v0.1.4 / NesNet v1.9.9)
 *   PSL1GHT + nucleo libretro Stella 2014
 *
 * Netplay: lockstep con retraso fijo (delay) sobre TCP, 2 jugadores.
 *   - El host crea la sala y recibe un CODIGO NUMERICO (10 digitos) que codifica
 *     su IP publica (el puerto es el de config.txt, 7779 por defecto, igual en los dos).
 *     El cliente escribe ese codigo con el mando.
 *   - Sin servidor: conexion directa host <-> cliente.
 *
 * Se compila de 2 formas:
 *   -DNESNET_PS3   -> aplicacion real para PS3
 *   -DNESNET_HOST  -> version de pruebas para PC (sin video, mando simulado)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include "libretro.h"

#ifdef NESNET_PS3
#include <ppu-types.h>
#include <malloc.h>
#include <sys/process.h>
#include <sys/systime.h>
#include <io/pad.h>
#include <rsx/rsx.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>
#include <audio/audio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/net.h>
#include <net/netdb.h>
#include <net/netctl.h>
#include <sysmodule/sysmodule.h>
#include <netinet/in.h>
SYS_PROCESS_PARAM(1001, 0x100000);
int closesocket(int s);      /* definida en libnet, pero sin prototipo en las cabeceras */
#else
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#define closesocket close
#endif

#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

/* ------------------------------------------------------------------ */
/* Constantes                                                          */
/* ------------------------------------------------------------------ */
/* v1.9.1: el mensaje M_RESET (reinicio sincronizado desde el menu R2) cambia el protocolo: una v1.9.1 y una v1.9
 * se rechazan entre si con el aviso "VERSIONES DISTINTAS DE ARCADE STATION" en vez de desincronizarse. */
/* v1.9.2: el mensaje M_GAME (el host cambia de juego desde R2) vuelve a cambiar el protocolo: 1vs1 = 5, modo de 4 = 6. */
/* v1.9.4: el mensaje M_STATE (el host carga un estado guardado y se lo manda al amigo) cambia el protocolo otra vez: 1vs1 = 7, modo de 4 = 8.
 * Una v1.9.3 y una v1.9.4 se rechazan en el saludo en vez de desincronizarse. */
/* v1.9.6: captura de pantalla con L2 (local, no toca la red): el protocolo NO cambia, 1vs1 = 7 y modo de 4 = 8 como en la v1.9.5. */
/* Katari v0.1: protocolo propio, distinto del de NesNet (7/8): un Katari y un NesNet se rechazan en el saludo. */
#define PROTO_VER      41
#define DEFAULT_PORT   7780        /* NesNet 7777, SilverSky 7778, Katari 7779, Arcade Station 7780 */
#define SCR_W          320        /* Arcade Station: lienzo 320x240 (x3 = 960x720, 4:3 a 720p) */
#define SCR_H          240
#define RING           64
#define MSG_SIZE       16
#define NO_FRAME       0xFFFFFFFFu

/* Modos de juego (el menu y config.txt devuelven siempre estos valores) */
#define MODE_SOLO      0
#define MODE_HOST      1
#define MODE_JOIN      2
#define MODE_ROM       3      /* v1.7: opcion "ELEGIR ROM" del menu (no es un modo de juego) */
#define MODE_FOUR      4      /* v1.8: opcion "MULTIJUGADOR DE 4" (submenu con host y unirse) */
#define MODE_ONLINE    6      /* Arcade Station: JUGAR ONLINE (1vs1 y multijugador) */
#define MODE_OPT       5      /* v1.8.2: opcion "OPCIONES" del menu (pantalla y mandos; no es un modo de juego) */

/* v1.8: el modo de 4 jugadores es una COPIA aparte del 1vs1 y habla otro protocolo, asi un host de 1vs1
 * nunca acepta a alguien del modo de 4 (ni al reves). Desde la v1.9.2: 1vs1 = 5, modo de 4 = 6. */
#define PROTO_VER4     42

/* FCEUmm define su "GAMEPAD" como subclase 1 del mando (RETRO_DEVICE_GAMEPAD en su libretro.c) */
#define NES_DEV_GAMEPAD RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1)

/* v1.7: selector de ROMs. Las ROMs van en USRDIR/roms/ y se reconocen por su cabecera, no por el nombre */
#define ROM_MAX        256
#define ROM_NAMELEN    96

/* Botones NES (mismo orden que el byte del mando) */
#define NB_A      0x01
#define NB_B      0x02
#define NB_SEL    0x04
#define NB_START  0x08
#define NB_UP     0x10
#define NB_DOWN   0x20
#define NB_LEFT   0x40
#define NB_RIGHT  0x80
#define NB_Y      0x100   /* Arcade Station: botones extra (cuadrado, triangulo, L1, R1) para juegos de 3 y 6 botones */
#define NB_X      0x200
#define NB_L      0x400
#define NB_R      0x800
#define NB_L2     0x1000   /* Arcade Station: L2 solo en los mandos 2 a 4 (en el 1 es la captura) */
#define NB_R2     0x2000

/* Botones de la interfaz (menus) */
#define UI_UP     0x01
#define UI_DOWN   0x02
#define UI_LEFT   0x04
#define UI_RIGHT  0x08
#define UI_OK     0x10
#define UI_CANCEL 0x20
#define UI_START  0x40

/* ------------------------------------------------------------------ */
/* Estado global                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    char     rom[420];        /* v1.7: ruta de la ROM elegida en el selector (ya no sale de config.txt) */
    char     mode[16];        /* "", "host", "client", "local" */
    char     code[32];
    char     public_ip[32];
    int      delay;           /* 0 = automatico */
    int      port;
    int      res;             /* 0 = la de la TV, 720 = forzar 720p */
    int      render;          /* 0 = GPU (escalado), 1 = CPU directo (sin transferencia GPU) */
    int      zoom;            /* 0 = automatico */
    int      audio;           /* 1 = con sonido (por defecto), 0 = sin sonido */
    int      upnp;            /* v1.6: 1 = el host abre el puerto por UPnP (por defecto), 0 = no */
} Config;

static Config   g_cfg;
static char     g_dir[256] = "/dev_hdd0/game/ARCA00001/USRDIR";
static FILE    *g_log;
static int      g_running = 1;
static const char *g_err = "";

static uint8_t *g_rom;                    /* NULL = ninguna ROM elegida (stand-by) */
static size_t   g_romsz;
static uint32_t g_romcrc;
static char     g_rom_name[ROM_NAMELEN];  /* v1.7: nombre del archivo de la ROM elegida */
static int      g_menu_sel;               /* v1.7: opcion marcada del menu principal */

static uint32_t g_fb[SCR_W * SCR_H];      /* lienzo ARGB */
static uint16_t g_pad[4];                 /* entrada NES que ve el nucleo (v1.8: 4 puertos; el 1vs1 solo usa 0 y 1) */
static int      g_core_ports;             /* Arcade Station: puertos que el nucleo declara (SET_INPUT_DESCRIPTORS) */
static unsigned g_rot;                    /* Arcade Station: rotacion pedida por el nucleo (0..3, antihoraria x90) */
static int      g_four;                   /* v1.8: 1 = partida de 4 jugadores (Four Score), 0 = 1vs1 como siempre */
static unsigned g_pixfmt = RETRO_PIXEL_FORMAT_RGB565;
static uint32_t g_frame_crc;
static int      g_vid_fresh;              /* v1.9.6: 1 = el nucleo entrego imagen nueva en este cuadro (g_fb esta limpia, sin textos encima) */
static int      g_shot_req;               /* v1.9.6: 1 = hay una captura pedida (L2); se toma justo despues de emular el cuadro */
static int      g_core_loaded;

static int      g_net = -1;               /* socket de la partida */
static uint8_t  g_rx[512];
static int      g_rxlen;

/* ------------------------------------------------------------------ */
/* Utilidades                                                          */
/* ------------------------------------------------------------------ */
static uint32_t now_ms(void)
{
#ifdef NESNET_PS3
    u64 s, n;
    sysGetCurrentTime(&s, &n);
    return (uint32_t)(s * 1000ULL + n / 1000000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
#endif
}

static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

/* Arcade Station: reloj fino (microsegundos) y acumuladores para saber donde se va el tiempo de cada cuadro.
 * Cada 120 cuadros el latido los escribe en arcade.log (por cuadro, en microsegundos). */
static uint64_t now_us(void)
{
#ifdef NESNET_PS3
    u64 s, n;
    sysGetCurrentTime(&s, &n);
    return (uint64_t)s * 1000000ULL + (uint64_t)(n / 1000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
#endif
}
static uint64_t g_tm_run, g_tm_scale, g_tm_draw, g_tm_pres;   /* retro_run entero, video_cb, dibujo a pantalla, present entero */

static void logf_(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log) { fprintf(g_log, "[%u] %s\n", (unsigned)now_ms(), buf); fflush(g_log); }
#ifdef NESNET_HOST
    fprintf(stderr, "%s\n", buf);
#endif
}

static uint32_t crc32_buf(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= d[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint32_t fnv_u32(uint32_t h, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++) { h ^= (v >> (i * 8)) & 0xFF; h *= 16777619u; }
    return h;
}

static void pump(void);   /* atiende al sistema (boton PS, etc.) */

/* Latido: cada ~2 s escribe en nesnet.log en que fase esta el programa. Si la consola se
 * cuelga, la ultima linea del log dice cuando y en que fase paso. */
static const char *g_stage = "inicio";
static uint32_t    g_hb;
/* Arcade Station: salto de dibujo automatico (solo SOLO). Si el juego tarda mas de 1/60 s por cuadro (Moonwalker), se acumula una "deuda"
 * de tiempo; al pasar de 6 ms se emula el cuadro SIN dibujar (el nucleo salta el dibujo y no se escala ni se presenta) y se espera lo justo
 * para que la deuda quede en 0. Asi la velocidad del juego y el audio quedan a 60 por segundo y solo se pierden algunas imagenes.
 * Un juego que ya va a 60 nunca acumula deuda: nunca salta nada. */
#if defined(NESNET_PS3) || defined(ARCADE_TEST_SKIP)
extern void fba_skip_draw(int);
static void core_skip_draw(int s) { fba_skip_draw(s); }
#else
static void core_skip_draw(int s) { (void)s; }
#endif
static int64_t  g_debt;
static uint64_t g_fs_prev;
static int      g_skip_run;
static uint32_t g_skipped;
static int      g_noskip;       /* 1 si existe USRDIR/noskip.txt: apaga el salto de dibujo (para comparar) */
#define SKIP_ON_US   6000
#define SKIP_MAXRUN  3

static void heartbeat(void)
{
    if (++g_hb % 120 == 0) {
        if (g_skipped) { logf_("saltos de dibujo (automatico): %u en total", (unsigned)g_skipped); }
        logf_("latido %u fase=%s", (unsigned)g_hb, g_stage);
        if (g_tm_run || g_tm_pres)
            logf_("tiempos por cuadro (us): nucleo=%u escalado=%u dibujo=%u present_total=%u",
                  (unsigned)((g_tm_run - g_tm_scale) / 120u), (unsigned)(g_tm_scale / 120u),
                  (unsigned)(g_tm_draw / 120u), (unsigned)(g_tm_pres / 120u));
        g_tm_run = g_tm_scale = g_tm_draw = g_tm_pres = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Configuracion (config.txt: clave=valor)                             */
/* ------------------------------------------------------------------ */
static void cfg_defaults(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.port = DEFAULT_PORT;
    g_cfg.res = 720;                      /* v1.3: por defecto 720p (probado en la PS3) */
    g_cfg.audio = 1;                      /* v1.5: sonido activado por defecto (audio=off o R2 al abrir lo apaga) */
    g_cfg.render = 1;                     /* v1.3: por defecto dibujo por CPU, sin la GPU (la ruta GPU se colgo 3 veces) */
    g_cfg.upnp = 1;                       /* v1.6: abrir el puerto por UPnP (upnp=off en config.txt lo apaga) */
}

static void cfg_load(void)
{
    char path[300], line[300];
    FILE *f;
    cfg_defaults();
    snprintf(path, sizeof path, "%s/config.txt", g_dir);
    f = fopen(path, "r");
    if (!f) { logf_("sin config.txt (%s), uso valores por defecto", path); return; }
    while (fgets(line, sizeof line, f)) {
        char *k = line, *v, *e;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == '#' || *k == ';' || *k == '\n' || *k == '\r' || !*k) continue;
        v = strchr(k, '=');
        if (!v) continue;
        *v++ = 0;
        e = k + strlen(k);
        while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        while (*v == ' ' || *v == '\t') v++;
        e = v + strlen(v);
        while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (!strcmp(k, "rom")) { /* v1.7: la ROM ya no se lee de aqui, se elige en el menu (carpeta roms) */ }
        else if (!strcmp(k, "mode"))      snprintf(g_cfg.mode, sizeof g_cfg.mode, "%s", v);
        else if (!strcmp(k, "code"))      snprintf(g_cfg.code, sizeof g_cfg.code, "%s", v);
        else if (!strcmp(k, "public_ip")) snprintf(g_cfg.public_ip, sizeof g_cfg.public_ip, "%s", v);
        else if (!strcmp(k, "delay"))     g_cfg.delay = atoi(v);
        else if (!strcmp(k, "port"))      g_cfg.port = atoi(v);
        else if (!strcmp(k, "res"))       { if (!strcmp(v, "auto")) g_cfg.res = 0; else if (!strcmp(v, "720")) g_cfg.res = 720; }
        else if (!strcmp(k, "render"))    { if (!strcmp(v, "gpu")) g_cfg.render = 0; else if (!strcmp(v, "cpu")) g_cfg.render = 1; }
        else if (!strcmp(k, "zoom"))      g_cfg.zoom = atoi(v);
        else if (!strcmp(k, "audio"))     { if (!strcmp(v, "off")) g_cfg.audio = 0; else if (!strcmp(v, "on")) g_cfg.audio = 1; }
        else if (!strcmp(k, "upnp"))      { if (!strcmp(v, "off")) g_cfg.upnp = 0; else if (!strcmp(v, "on")) g_cfg.upnp = 1; }
    }
    fclose(f);
    if (g_cfg.port <= 0 || g_cfg.port > 65535) g_cfg.port = DEFAULT_PORT;
}

/* Katari: el nucleo Stella ya trae libretro-common completo (file_stream, file_path...), no hacen falta shims. */

/* ------------------------------------------------------------------ */
/* Fuente 5x7 (ASCII 32..90) y dibujo de texto en el lienzo            */
/* ------------------------------------------------------------------ */
static const uint8_t font5x7[59][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E},
    {0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},
    {0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
};


/* ------------------------------------------------------------------ */
/* v1.9.7: menus en 1280x720 con fuente DejaVu Sans Bold y fondo mosaico.
 * Solo es DIBUJO de menus: no toca la red ni el juego. Los menus siguen llamando a ui_text / ui_center / ui_clear con las
 * coordenadas del lienzo viejo (256x240); mientras g_hr_on = 1 esas llamadas se traducen a este lienzo de 1280x720
 * (x5, y3). El modo GPU (render = 0), el AJUSTE DE IMAGEN y lo que se dibuja en pleno juego siguen con el dibujo de siempre. */
/* ------------------------------------------------------------------ */
#define HR_W 1280
#define HR_H 720
#define HC_RED   0xFFFF2D95u
#define HC_WHITE 0xFFF8F4FFu
#define HC_DIM   0xFFB9A8F0u
#define HC_GRID  0xFF2BE3FFu
#define HC_GREEN 0xFF6EDC96u
#define HC_AMBER 0xFFFFBE50u
#define HC_YEL   0xFFFFE246u
#define HC_MUTE  0xFF6A5C9Au
#define HC_SHAD  0xFF02040Eu
/* ---- Arcade Station v0.3: fuente neon (Montserrat Bold Italic, licencia OFL) con brillo horneado en el alfa: 1..10 = halo, 12..15 = tubo; 5 tamanos. Antes: DejaVu Sans Bold (licencia libre Bitstream Vera / DejaVu), 5 tamanos, alfa de 4 bits, generada fuera de la PS3; Katari: el tamano grande suma A, I, K y R para el logo ---- */
typedef struct { uint16_t cp; uint8_t adv; int8_t bx, by; uint8_t w, h; uint32_t off; } FGlyph;
enum { FS_S, FS_M, FS_L, FS_H, FS_X, FS_N };        /* 22, 28, 34, 46 y 88 px (alto) */
static const int fg_asc[FS_N] = { 21, 26, 32, 43, 82 };
static const int fg_start[FS_N + 1] = { 0, 113, 226, 339, 399, 419 };
#define FG_NDATA 211359
static const FGlyph fg_tab[419] = {
 {32,6,0,0,0,0,0u},{33,6,-3,-19,14,23,0u},{34,10,-1,-19,15,13,161u},{35,16,-3,-19,23,22,259u},{36,14,-4,-22,22,28,512u},
 {37,19,-2,-19,24,23,820u},{38,16,-3,-19,22,23,1096u},{39,5,-1,-19,10,13,1349u},{40,8,-2,-20,15,28,1414u},{41,8,-5,-20,16,28,1624u},
 {42,10,-2,-20,16,16,1848u},{43,13,-2,-16,18,17,1976u},{44,6,-4,-8,12,15,2129u},{45,8,-3,-11,14,10,2219u},{46,6,-4,-8,12,12,2289u},
 {47,9,-6,-22,22,28,2361u},{48,15,-2,-19,20,23,2669u},{49,9,-2,-19,14,22,2899u},{50,13,-4,-19,20,22,3053u},{51,13,-4,-19,21,23,3273u},
 {52,15,-3,-19,21,22,3515u},{53,13,-4,-19,21,23,3746u},{54,14,-2,-19,20,23,3988u},{55,14,-2,-19,20,22,4218u},{56,15,-3,-19,21,23,4438u},
 {57,14,-3,-19,20,23,4680u},{58,6,-4,-16,14,20,4910u},{59,6,-4,-16,14,23,5050u},{60,13,-2,-17,19,18,5211u},{61,13,-3,-15,19,15,5382u},
 {62,13,-3,-17,19,18,5525u},{63,13,-2,-19,18,23,5696u},{64,23,-3,-19,29,27,5903u},{65,17,-5,-19,24,22,6295u},{66,17,-3,-19,23,22,6559u},
 {67,16,-2,-19,22,23,6812u},{68,18,-3,-19,24,22,7065u},{69,15,-3,-19,22,22,7329u},{70,14,-3,-19,22,22,7571u},{71,17,-2,-19,23,23,7813u},
 {72,18,-3,-19,24,22,8078u},{73,7,-3,-19,14,22,8342u},{74,12,-5,-19,21,23,8496u},{75,16,-3,-19,25,22,8738u},{76,13,-3,-19,19,22,9013u},
 {77,21,-3,-19,28,22,9222u},{78,18,-3,-19,24,22,9530u},{79,19,-2,-19,24,23,9794u},{80,16,-3,-19,22,22,10070u},{81,19,-2,-19,24,26,10312u},
 {82,16,-3,-19,22,22,10624u},{83,14,-4,-19,22,23,10866u},{84,14,-2,-19,21,22,11119u},{85,17,-2,-19,23,23,11350u},{86,16,-2,-19,24,22,11615u},
 {87,25,-1,-19,31,22,11879u},{88,16,-5,-19,26,22,12220u},{89,15,-2,-19,23,22,12506u},{90,15,-4,-19,23,22,12759u},{91,8,-4,-20,17,28,13012u},
 {92,9,-2,-22,13,28,13250u},{93,8,-5,-20,17,28,13432u},{94,13,-3,-16,18,16,13670u},{95,11,-5,-3,18,8,13814u},{96,13,1,-20,12,10,13886u},
 {97,15,-3,-15,21,19,13946u},{98,15,-3,-20,21,24,14146u},{99,13,-3,-15,20,19,14398u},{100,15,-3,-20,22,24,14588u},{101,14,-3,-15,20,19,14852u},
 {102,9,-3,-20,18,23,15042u},{103,15,-4,-15,22,23,15249u},{104,15,-3,-20,20,23,15502u},{105,7,-3,-21,14,24,15732u},{106,7,-7,-21,18,29,15900u},
 {107,14,-3,-20,22,23,16161u},{108,7,-3,-20,14,23,16414u},{109,23,-3,-15,28,18,16575u},{110,15,-3,-15,20,18,16827u},{111,14,-3,-15,20,19,17007u},
 {112,15,-4,-15,22,23,17197u},{113,15,-3,-15,21,23,17450u},{114,9,-3,-15,17,18,17692u},{115,12,-4,-15,19,19,17845u},{116,10,-3,-18,16,22,18026u},
 {117,15,-2,-15,20,19,18202u},{118,13,-2,-15,20,18,18392u},{119,21,-2,-15,27,18,18572u},{120,13,-5,-15,23,18,18815u},{121,13,-5,-15,23,23,19022u},
 {122,12,-4,-15,20,18,19287u},{123,9,-3,-20,17,28,19467u},{124,7,-4,-20,15,28,19705u},{125,9,-5,-20,16,28,19915u},{126,13,-2,-13,18,11,20139u},
 {161,6,-4,-15,14,22,20238u},{176,9,-2,-19,15,14,20392u},{183,7,-2,-12,11,11,20497u},{191,13,-3,-15,18,22,20558u},{193,17,-5,-23,24,26,20756u},
 {201,15,-3,-23,22,26,21068u},{205,7,-3,-23,18,26,21354u},{209,18,-3,-23,24,26,21588u},{211,19,-2,-23,24,27,21900u},{218,17,-2,-23,23,27,22224u},
 {220,17,-2,-23,23,27,22535u},{225,15,-3,-20,21,24,22846u},{233,14,-3,-20,20,24,23098u},{237,7,-3,-20,17,23,23338u},{241,15,-3,-20,20,23,23534u},
 {243,14,-3,-20,21,24,23764u},{250,15,-2,-20,20,24,24016u},{252,15,-2,-20,20,24,24256u},{32,8,0,0,0,0,24496u},{33,8,-3,-22,15,26,24496u},
 {34,12,-1,-22,18,14,24691u},{35,19,-3,-22,27,25,24817u},{36,17,-4,-26,25,33,25155u},{37,24,-1,-23,27,27,25568u},{38,20,-3,-23,25,27,25933u},
 {39,6,-1,-22,12,14,26271u},{40,10,-2,-24,17,33,26355u},{41,10,-5,-24,17,33,26636u},{42,12,-2,-24,19,18,26917u},{43,16,-2,-19,21,19,27088u},
 {44,7,-5,-9,14,17,27288u},{45,10,-2,-13,15,11,27407u},{46,7,-4,-9,13,13,27490u},{47,11,-6,-26,24,32,27575u},{48,18,-2,-23,23,27,27959u},
 {49,11,-2,-22,16,25,28270u},{50,16,-4,-23,23,26,28470u},{51,16,-4,-22,24,26,28769u},{52,19,-3,-22,25,25,29081u},{53,16,-4,-22,24,26,29394u},
 {54,17,-2,-23,23,27,29706u},{55,17,-2,-22,24,25,30017u},{56,18,-3,-23,24,27,30317u},{57,17,-3,-23,23,27,30641u},{58,7,-4,-19,15,23,30952u},
 {59,7,-5,-19,16,27,31125u},{60,16,-2,-20,22,21,31341u},{61,16,-2,-18,21,17,31572u},{62,16,-3,-20,22,21,31751u},{63,16,-2,-23,21,27,31982u},
 {64,28,-3,-23,34,32,32266u},{65,21,-5,-22,28,25,32810u},{66,21,-3,-22,27,25,33160u},{67,20,-2,-23,26,27,33498u},{68,22,-3,-22,28,25,33849u},
 {69,18,-3,-22,25,25,34199u},{70,17,-3,-22,25,25,34512u},{71,21,-2,-23,27,27,34825u},{72,22,-3,-22,28,25,35190u},{73,9,-3,-22,15,25,35540u},
 {74,14,-5,-22,23,26,35728u},{75,20,-3,-22,29,25,36027u},{76,16,-3,-22,22,25,36390u},{77,26,-3,-22,32,25,36665u},{78,22,-3,-22,28,25,37065u},
 {79,23,-2,-23,28,27,37415u},{80,20,-3,-22,26,25,37793u},{81,23,-2,-23,28,31,38118u},{82,20,-3,-22,26,25,38552u},{83,17,-4,-23,25,27,38877u},
 {84,17,-2,-22,24,25,39215u},{85,21,-2,-22,27,26,39515u},{86,20,-1,-22,27,25,39866u},{87,31,-1,-22,37,25,40204u},{88,19,-5,-22,30,25,40667u},
 {89,19,-1,-22,26,25,41042u},{90,18,-4,-22,27,25,41367u},{91,10,-4,-24,20,33,41705u},{92,11,-2,-26,15,32,42035u},{93,10,-6,-24,20,33,42275u},
 {94,16,-3,-19,20,19,42605u},{95,14,-5,-3,21,9,42795u},{96,16,2,-24,14,11,42890u},{97,19,-3,-18,25,22,42967u},{98,19,-3,-24,24,28,43242u},
 {99,16,-3,-18,23,22,43578u},{100,19,-3,-24,26,28,43831u},{101,17,-3,-18,23,22,44195u},{102,10,-3,-24,20,27,44448u},{103,19,-4,-18,26,27,44718u},
 {104,19,-3,-24,24,27,45069u},{105,8,-3,-25,16,28,45393u},{106,8,-8,-25,21,34,45617u},{107,18,-3,-24,25,27,45974u},{108,8,-3,-24,15,27,46312u},
 {109,28,-3,-18,33,21,46515u},{110,19,-3,-18,24,21,46862u},{111,18,-3,-18,23,22,47114u},{112,19,-4,-18,25,27,47367u},{113,19,-3,-18,25,27,47705u},
 {114,12,-3,-18,19,21,48043u},{115,14,-4,-18,22,22,48243u},{116,12,-2,-21,17,25,48485u},{117,19,-2,-18,23,22,48698u},{118,16,-2,-18,23,21,48951u},
 {119,25,-2,-18,32,21,49193u},{120,16,-5,-18,26,21,49529u},{121,16,-6,-18,27,27,49802u},{122,15,-4,-18,23,21,50167u},{123,11,-2,-24,18,33,50409u},
 {124,8,-4,-24,16,33,50706u},{125,11,-6,-24,19,33,50970u},{126,16,-2,-16,21,13,51284u},{161,8,-4,-18,15,26,51421u},{176,11,-1,-23,16,17,51616u},
 {183,8,-2,-14,12,12,51752u},{191,16,-3,-18,21,26,51824u},{193,21,-5,-28,28,31,52097u},{201,18,-3,-28,25,31,52531u},{205,9,-3,-28,20,31,52919u},
 {209,22,-3,-28,28,31,53229u},{211,23,-2,-28,28,32,53663u},{218,21,-2,-28,27,32,54111u},{220,21,-2,-28,27,32,54543u},{225,19,-3,-24,25,28,54975u},
 {233,17,-3,-24,23,28,55325u},{237,8,-3,-24,19,27,55647u},{241,19,-3,-24,24,27,55904u},{243,18,-3,-24,24,28,56228u},{250,19,-2,-24,23,28,56564u},
 {252,19,-2,-24,23,28,56886u},{32,10,0,0,0,0,57208u},{33,11,-4,-28,19,33,57208u},{34,15,-2,-28,22,19,57522u},{35,25,-4,-28,33,32,57731u},
 {36,22,-5,-32,31,40,58259u},{37,30,-2,-28,34,33,58879u},{38,25,-4,-28,32,33,59440u},{39,9,-2,-28,15,19,59968u},{40,13,-3,-29,22,40,60111u},
 {41,13,-6,-29,21,40,60551u},{42,15,-3,-29,24,22,60971u},{43,21,-2,-24,25,25,61235u},{44,10,-6,-11,17,21,61548u},{45,14,-3,-16,19,13,61727u},
 {46,10,-5,-11,16,16,61851u},{47,14,-8,-32,31,40,61979u},{48,23,-3,-28,29,33,62599u},{49,14,-2,-28,19,32,63078u},{50,20,-5,-28,29,32,63382u},
 {51,21,-6,-28,31,33,63846u},{52,24,-4,-28,31,32,64358u},{53,21,-5,-28,30,33,64854u},{54,22,-3,-28,29,33,65349u},{55,21,-2,-28,29,32,65828u},
 {56,23,-4,-28,30,33,66292u},{57,22,-4,-28,29,33,66787u},{58,10,-5,-23,18,28,67266u},{59,10,-6,-23,19,33,67518u},{60,21,-2,-24,26,25,67832u},
 {61,21,-3,-22,27,21,68157u},{62,21,-3,-24,26,25,68441u},{63,20,-2,-28,26,33,68766u},{64,35,-3,-28,41,39,69195u},{65,26,-7,-28,35,32,69995u},
 {66,26,-3,-28,32,32,70555u},{67,25,-3,-28,33,33,71067u},{68,28,-3,-28,34,32,71612u},{69,23,-3,-28,30,32,72156u},{70,22,-3,-28,30,32,72636u},
 {71,26,-3,-28,33,33,73116u},{72,28,-3,-28,34,32,73661u},{73,12,-3,-28,18,32,74205u},{74,19,-6,-28,29,33,74493u},{75,25,-3,-28,35,32,74972u},
 {76,21,-3,-28,26,32,75532u},{77,33,-3,-28,39,32,75948u},{78,28,-3,-28,34,32,76572u},{79,29,-3,-28,35,33,77116u},{80,25,-3,-28,31,32,77694u},
 {81,29,-3,-28,35,38,78190u},{82,25,-3,-28,31,32,78855u},{83,22,-5,-28,31,33,79351u},{84,21,-2,-28,30,32,79863u},{85,26,-2,-28,32,33,80343u},
 {86,26,-2,-28,34,32,80871u},{87,39,-1,-28,46,32,81415u},{88,25,-6,-28,36,32,82151u},{89,24,-2,-28,32,32,82727u},{90,23,-5,-28,33,32,83239u},
 {91,13,-5,-29,24,40,83767u},{92,14,-2,-32,18,40,84247u},{93,13,-7,-29,24,40,84607u},{94,21,-3,-23,25,23,85087u},{95,17,-7,-4,26,11,85375u},
 {96,21,2,-29,18,13,85518u},{97,24,-3,-22,30,27,85635u},{98,24,-4,-29,30,34,86040u},{99,21,-4,-22,28,27,86550u},{100,24,-3,-29,31,34,86928u},
 {101,22,-4,-22,28,27,87455u},{102,14,-3,-29,24,33,87833u},{103,24,-5,-22,32,33,88229u},{104,24,-4,-29,29,33,88757u},{105,11,-4,-30,20,34,89236u},
 {106,11,-10,-30,26,41,89576u},{107,22,-4,-29,32,33,90109u},{108,11,-4,-29,19,33,90637u},{109,36,-4,-22,41,26,90951u},{110,24,-4,-22,29,26,91484u},
 {111,23,-4,-22,29,27,91861u},{112,24,-5,-22,31,33,92253u},{113,24,-3,-22,30,33,92765u},{114,15,-4,-22,24,26,93260u},{115,19,-5,-22,27,27,93572u},
 {116,15,-3,-26,22,31,93937u},{117,24,-3,-22,30,27,94278u},{118,21,-3,-22,29,26,94683u},{119,32,-3,-22,40,26,95060u},{120,21,-6,-22,32,26,95580u},
 {121,21,-7,-22,33,33,95996u},{122,19,-5,-22,28,26,96541u},{123,14,-3,-29,23,40,96905u},{124,11,-5,-29,20,40,97365u},{125,14,-7,-29,23,40,97765u},
 {126,21,-3,-19,27,15,98225u},{161,11,-6,-22,20,32,98428u},{176,15,-2,-28,21,20,98748u},{183,11,-3,-17,16,15,98958u},{191,20,-4,-22,26,32,99078u},
 {193,26,-7,-34,35,38,99494u},{201,23,-3,-34,30,38,100159u},{205,12,-3,-34,23,38,100729u},{209,28,-3,-34,34,38,101166u},{211,29,-3,-34,35,39,101812u},
 {218,26,-2,-34,32,39,102495u},{220,26,-2,-34,32,39,103119u},{225,24,-3,-29,30,34,103743u},{233,22,-4,-29,28,34,104253u},{237,11,-4,-29,23,33,104729u},
 {241,24,-4,-29,29,33,105109u},{243,23,-4,-29,29,34,105588u},{250,24,-3,-29,30,34,106081u},{252,24,-3,-29,30,34,106591u},{32,14,0,0,0,0,107101u},
 {33,14,-5,-37,25,43,107101u},{37,41,-2,-37,45,43,107639u},{40,17,-3,-39,28,53,108607u},{41,17,-8,-39,28,53,109349u},{43,28,-3,-32,34,32,110091u},
 {44,13,-7,-14,21,26,110635u},{45,18,-4,-21,25,17,110908u},{46,13,-6,-14,20,20,111121u},{47,19,-10,-43,40,53,111321u},{48,32,-3,-38,38,44,112381u},
 {49,19,-2,-37,25,42,113217u},{50,28,-6,-38,38,43,113742u},{51,28,-7,-37,40,43,114559u},{52,32,-5,-37,41,42,115419u},{53,28,-6,-37,39,43,116280u},
 {54,30,-3,-38,38,44,117119u},{55,29,-3,-37,39,42,117955u},{56,31,-5,-38,40,44,118774u},{57,30,-5,-38,39,44,119654u},{58,13,-6,-30,23,36,120512u},
 {60,28,-3,-32,36,33,120926u},{62,28,-4,-32,35,33,121520u},{63,27,-3,-38,35,44,122098u},{65,35,-8,-37,46,42,122868u},{66,35,-4,-37,43,42,123834u},
 {67,34,-3,-38,43,44,124737u},{68,38,-4,-37,46,42,125683u},{69,31,-4,-37,41,42,126649u},{70,30,-4,-37,41,42,127510u},{71,36,-3,-38,44,44,128371u},
 {72,37,-4,-37,46,42,129339u},{73,16,-4,-37,24,42,130305u},{74,25,-8,-37,38,43,130809u},{75,34,-4,-37,47,42,131626u},{76,28,-4,-37,35,42,132613u},
 {77,44,-4,-37,53,42,133348u},{78,37,-4,-37,46,42,134461u},{79,39,-3,-38,46,44,135427u},{80,34,-4,-37,42,42,136439u},{81,39,-3,-38,46,51,137321u},
 {82,34,-4,-37,42,42,138494u},{83,30,-6,-38,41,44,139376u},{84,29,-3,-37,40,42,140278u},{85,36,-2,-37,43,43,141118u},{86,35,-2,-37,45,42,142043u},
 {87,53,-1,-37,61,42,142988u},{88,33,-8,-37,49,42,144269u},{89,32,-2,-37,43,42,145298u},{90,31,-6,-37,44,42,146201u},{161,14,-7,-30,25,42,147125u},
 {183,15,-3,-22,20,19,147650u},{191,27,-5,-30,34,43,147840u},{193,35,-8,-46,46,51,148571u},{201,31,-4,-46,41,51,149744u},{205,16,-4,-46,31,51,150790u},
 {209,37,-4,-47,46,52,151581u},{211,39,-3,-46,46,52,152777u},{218,36,-2,-46,43,52,153973u},{220,36,-2,-46,43,52,155091u},{32,25,0,0,0,0,156209u},
 {48,59,-6,-71,73,82,156209u},{49,35,-4,-70,47,79,159202u},{50,52,-11,-71,71,80,161059u},{51,52,-12,-70,74,81,163899u},{52,60,-9,-70,77,79,166896u},
 {53,52,-11,-70,73,81,169938u},{54,56,-6,-71,72,82,172895u},{55,54,-4,-70,71,79,175847u},{56,58,-8,-71,73,82,178652u},{57,56,-9,-71,72,82,181645u},
 {63,52,-4,-71,64,81,184597u},{65,67,-15,-70,87,79,187189u},{69,59,-7,-70,76,79,190626u},{73,29,-7,-70,45,79,193628u},{75,65,-7,-70,87,79,195406u},
 {78,70,-7,-70,86,79,198843u},{82,64,-7,-70,79,79,202240u},{83,56,-10,-71,75,82,205361u},{84,54,-4,-70,74,79,208436u},
};
static const char fg_b64[] =
"AAARERERAAAREiIiERAAESNEQyIRARI03d1CEQESRv//UxERI1f//VMhESNc//hTIREjbf/4UyERJG3/50IRESR+/9ZCEREkf/+FMhERJH//dTIREiRv/mQh"
"ERI0budTIRASNWd2QyEQEjXe1kIREBJN//ZCEQASTv/9MhEAEjX/5DIRABEjRUMhEAAREjMiERAAAREREREAAAABEREAAAAAERERERERABESIiIiIRERIzRE"
"REMhESNN1lXdQyESRv/nf/5CESR//Yn/1CETXf+Z3/dCEjXv+I7/ZCETXv537+UyESTu1m7tQhESNEREREMhERIiIiMiIRABEREREREQAAAAAAEREREREREA"
"AAAAEREiIiIiIREAAAAREjNEMzQzIREAABESNF3VVW1TIRAAERI0bv53f/1TIRABEjVo/9ma/4ZCEQESNXmv+qrf+nUyEBE03////////9MhESNf////////"
"9jIREjXd7/7d//3dUyERI1it/6qv/qhkMREjRor/2qr/qXUyERI1eq/6qt/6l0MhESTf////////1DEQEk7////////9QhEBJN3v/t3//d1TIRASNY3/qq7+"
"l1QhEAETRu/pmf/XUyIRABEjX/1m3/ZTIREAAREjRVRFVUMhEQAAAREjMzMzMiERAAAAARESIiIhEREAAAAAAAAAABEREQAAAAAAAAAREREREQAAAAAAEREi"
"MyIREQAAAAERIjRVQyIRAAAAERIzVt/VQiEQAAERI0Z579dUIRAAERI1eM3/7XUyEQARI1ff/////kIRABE1fv/////+UxEBEjbf/+//3v1CEQESR//+rv2p"
"dUIRARJH//2v+qhkMhEBEkf//+/6qFQyEQESRt////6pZDIRARI1ff////6FMhEBEjRprf///+ZCEQESNWiq/9//90IRESNGear/rf/3QhERJG/arP6u/+ZC"
"EREk7//v////1TIREST///////1kIRARI13v///+hlMhEAESRXmv+pdkMhEAARIjVs/oZTMhEQAAERI0btVDIhEQAAARESM0QyIREQAAAAAREiIiEREAAAAA"
"AAEREREREAAAAAABEREREREREREQAAAREiIiIhEREiERAAESI0REMyIiMzIREBEjRW3WVDM0RdwhEBEkbv//1lRW3/QhEBI13+rf+GZn/9QhEBJG/6qv+YiP"
"/WQhEBJH/6qv+qr/51MhERJH/6qv+q7+hlMhERJG/6rv6u/ql1QyERI17///rv+qqHVCERI0be7a3/rf//dTIREjVomt/63/3v9kIREiNXmv/a/9qv90IRES"
"NXn/2q/6qu90IRESNX/+ma/6qv90IQESRu/od5/6rf9kIQETTv92VX/+7/dTIQES7/ZURFfv/2UyEQESNEQzIzRWZUMhEQARIiIiIiMzMzIREAABEREREREi"
"IhERAAAAEREREREREREQAAAAAREREiEREQAAAAAREiMzMyIREAAAABEjRFVVQyERAAABEjRn3d1lMhEAABETRt////dDEQAAESRt//7//mMhEAERNH//qq//"
"ZCEQARI1j/+qr/90IRABEjaf/6rv/WQyEREjR67////ZZDIRESNYrv//6ql1QhESNXn////qrexTIRJG3//e//6v/1MhEkf//arv///9YyETXf/6qq7///lT"
"IRNd//qqqv//2FMhE13//93f///3UxESRu////////ZCERI0bf///tqfdTIRESNGeIiHdmVDIRAREjNFVVREQzIREAEREiMzMyIiEREAABEREREREREREAAA"
"EREREAERIiERESIzMhERJN1DIRI1/+QhEkb/1CETXf9kIRNe/2QhE07+UyESTu1CERIjQyIRERIiERABERERAAAAARERERAAAAERIiIREAABESMzMhEQAREj"
"VWUyEQERI1z/9CEQESNX//1CEREiRu/+ZCEREjWP/4UyERE0fv/nUyERI1j/+WQhERJG3/+FMhEBNH7/10MhECNY//lkIRECNp//hTIRACRt//hTIRACRt/+"
"dTIRACR+/+dDIRACR+/9dDIRACR+/9dCERACRu/+dCERACNt/+dCEQACNc//dCEQABJH//ZCEQABI17/wyEQABEkVVQyEAABESMzIhEAAAERIiERAAAAARER"
"EQAAAAAAABERERAAAAABESIhERAAABESMzMhEQAAESNWVDIRAAERNP/2QhEAARI17/1TIQABEjbf/mQhAAESNs//dCEAARI2n/91MQABEjaf/4UyAAESNp//"
"hTIAARJGn/+FMgABEkbP/4UyABEiR9/+dDEAESNX7/50IQARI1j//WQhABEkaf/4UyEBEjR9//dTIQESNY//1kIRARJG3/+FMhERE1fv/WQhEREjXf/3UyEQ"
"ESNv/8ZCERARI+/+ZDIRABEjRVVDIREAARIzMyIREAABERIiEREAAAABERERAAAAAAAREREREAAAARESIhERAAEREjMzMhEQARIzRWVDIhERI0V8/nVCEREk"
"bq39nlMhEjX/////1CESNo7//+xkIRJGnv//6GMhEk3////+UyESNenf2uZCEREkV++HZDIRESI1XVQzIRABEiMzMyIREAAREiIhEREAAAERERERAAAAAAER"
"ERERAAAAARESIiIREAAAEREjREMiEQAAESI13uUyERABEiNW7/dTIRARI0V5//lkMhERJGea/+qGQxESNf//////UyESNf//////UyESNe7v/+7uQxERJGiv"
"/6hlMhERI0aP/oZDIRABEjRv/WQyERABESNd1UMhEQAAERIzQyIREAAAAREiIhERAAAAABEREREAAAAAAREREAABERERERABEiMyIRERI0VUMhERJG/9QxES"
"Nf//UyESNv//YyESNt/+UyESRt/3QxESNf/VMhESPf5UIRERJNRCIRARIjMiERABEREREAAAERERAAAAEREREREQAREiMzIhEQESNFVUMhEBI13d3cMhESTv"
"///TIREk////UyEBI0VmZUIRARIzREMiEQERIiIiERAAEREREREAAAERERAAABEREREAARIiMiEQESJEVDIRESRf/UIREST//1MREST//1MRESTf/UIRESNF"
"VDIRARIjMiEQABEREREAAAERERAAAAAAAAAAEREREQAAAAAAAAERIiIREAAAAAABESI0QyEQAAAAAAESJF3dMhEAAAAAERJF7/0yEQAAAAERI1f/9jIRAAAA"
"ARI0bv/VMhEAAAAREkaP/3QxEQAAABEjV+/8YyEQAAABESRs/+dTIRAAAAESNX//hkIREAAAESJG3/50MhEAAAARI1f/+FMhEQAAARI0bf/nQyEQAAABEjWP"
"/4UyEQAAABEjR+/9ZDIRAAABESNY//dTIRAAAAESNH7/1kIhEAAAERI1j/91MhEAAAARI0fv/GQhEQAAABEjXP/nUyEQAAAAESRv/4UyERAAAAARJN/+ZCIR"
"AAAAABEk//ZDIREAAAAAESNFVDIREAAAAAAREjMyIREAAAAAAAEREiEREAAAAAAAABERERAAAAAAAAAAABERIiIhEREAAAERIjMzMyIREAARIjRVZlVDIREB"
"EiNWjd3XZDIRERJFfv///9ZDIREjWP/////9ZCESNX7//t3//3UxEkbf/9qq3//VMiNY//6oia//1jIjbf/9l3iv/9ZCJG7/+oZor//WMiR+/+p2aK//xjIk"
"f//pdnnf/4UyJH//6oeK//50ISR//+qZnf/9ZCEkbv/6qq//51MhI1j//+7//4UyERJG7/////hkIhESNG3///12QyEQESNFd4h2VDIREBESI0RVRDIhEQAB"
"ERIiMyIhERAAABERERERERAAAAARESIhERABEiMzMiERESNFVVQyERE03d3dUyESNf///9QhEjb///+FMRI13f//hTESNXrv/4UxEjRp7/50MRIkaf/9dCER"
"JGn/+WQhEiR9//hTIRI0fv/4UyESNX7/50MhEjWP/9ZCERI1j/+GMhESNd//dTIREjXv/2QhEBEk7/5TIRARI0VUMhEAARIjMiERAAEREREREAAAAAEREiIi"
"EREAAAAREiMzMyIREAABEiNFZmVDIhEAASNFfd7ddUMhABEkbv////1kIQARJP//////dTIAEST//czv/+YyABEkXoiarv/2QgARI0Vmea7/5jIAESI0Vnmv"
"/9UyABEiNFeK//51MQERIjVo3//4ZCEBEiNXnf/+lkMhARI1ee//6XVCIREjV5///al1MhERJG3//6qpdUIREjXv//////1CERI1///////9QhESPP//////"
"9TIRESRWeIiIdlQhEBEiNEVVVUQyIRABESIjMzMiIREAAAARESIiIiIREQAAERIjM0QzMiEQAAESNFZmZmVDIRAAEjXd3d3d3UMRABEk3//////1MhABEk7/"
"/////2MhABEk3d3d///FMhABEjRoqt//p0MhABESRXrf/6hkIRABESNY7/+qhTIRABESNc///qhTIRABESNd////lkIRAREjRX3e//50IRARIzRWiq7/90IR"
"ESNFZnia7/90IRESTeiJmq//5jIREjX//+7///dTIQEj3//////4ZCEQESRu////12QiEQESNFd4h3ZTIhEAARIzRFVUQyIRAAARESIjMyIhEQAAABERERER"
"EREAAAAAAAAAAREREREAAAAAABERIiIhEQAAAAAREiNEQyEQAAAAERI0Vt3TIRAAABESNGf//jIRAAAREjRo//5kIRAAERI0aP/+dTIRAAESNGjv/5dTIREB"
"EjRo7/+pdUMhERIkaO//qqh1MhERI1jv/6rP/2QxESNX7//Krf/oUyESRt//2qr//qdCESTP////////9TESTf////////5TESTe7u7u///u1CESNFd4mq7/"
"6nUyERIzRFZ5//11MhEBESIzRW//xTIhEAEREiI0VmUyIRAAARERIjMzMhEQAAAAARERIiEREAAAAAEREiIiIREQAAABESIzMzMyIRAAAREjRWZmVUMhEAAR"
"I13d3d3dQhEAESNX//////UxEAESRt//////UyEBESR+//3d3dQxEBEjWP/6qphlMhEBEjbP/6qodkMhEBEkbv/93ahkMhEBEkf/////6FMhEBEkb//////m"
"QhEREkZ5qq3//3QhERI0V4iarf/4UxERI1Z4iarf/4QhERJG7Jmqrv/3QhERNe//7u///VMhERNP//////1kIRERJG3////oZDIRARI0VniIdlQyEQABEiNE"
"VVRDIhEQAAERIiMzIiEREAAAAREREREREQAAAAAAERESIiIREQAAAREiIzMzMhEQABESM0VmZUMiEQESI0Z43e3VQhEREkV9/////0MRESNX7/////5TERI1"
"fv//2q3mQhESRt//6qmHZTIRI1j//qqph1QyESRt//qt3ahkMhEkfv/+///+hTIRJH7//////+ZCESWP//6qrv/3UxEkf//qqqr//FMRJH7/6qqq//hTESNt"
"//qqrf/3QhEjV//+3e//5kIREkbP/////nQyERI0aO///9dTIRARI0VniHdkMhEQARIjRFVEMyERAAAREiIzIiEREAAAAREREREREQAAABERIiIiIiEREAES"
"IzNEREMzIRERI0VWZmZmVTIRESRt3d3d3d1TIRI17///////5CESRv///////9QhEkf//d3d//90IRJH/+qqrf/8ZCESPf/omq//51MhEjRmd4rv/5ZCEREj"
"RWed//11MhEREjRXr//oUyEQERI0aO//lkMhEAESNH3//XUyEQABEjWP//hTIREAARJG7/+WQyEQAAEjV//+dTIRAAABI17/91MhEQAAARPf/9VCERAAAAES"
"NVVDIhEAAAABESMzMhEQAAAAABERIhERAAAAAAAAERESIiIRERAAABESIzMzMyIREAAREjRVZmVUMhEQARI0Z93t12QyEQESNX7////+ZDEQESRu//////5j"
"IREjWP/+qq7/90IREjbf/6qqr/90IREkbf/6qq3/90IRIkev/+qt//50IRI1eu/////+lkIRJGnv/////6lTIRJH7//arf//hTISNY//2qqs//hTISNd//qq"
"qq//1TIRNd//yqqt//hTERJH///d3v/+ZCERI13//////nUyEREkaO///+11MhEBEjRWeIh3ZDIREAESI0RVVUMyERAAAREiIzMiIhEQAAABEREREREREAAA"
"AAARESIiIREQAAARESIzMzIhERABESM0VmZUMiERARI0Z83tx1QyEREjRt/////WQyERJG3//////WQhETV//+qq7/91MhI13/+qqq//1jISNt//qqqv/9ZC"
"Ejbf/6qqz//nQhI1j//93v//50IRNH7//////9ZCESNX3//+3/+WMhEjRomqqu//hTIRI1Z4mqr//XQhESRniarf//hTIRI17+7v//+GQhESPf/////oZDIR"
"Ejzv///tdUMhEBEkVniHdlMiERAREjRFVEMyEREAAREiIzIiIREAAAAREREREREQAAAAAAERERAAAAARERERAAABEiIyIRAAESJEVDIRABEkX/1CEQARJP//"
"UxEAETX//1MhARE03/1CEQERI0VUMhEBEiNEQyERERIzRDIhEBEjRVUyIRARJG/9QyEQEST//1MhEBEk//9TIQARJN/9QhEAESNFVDIRAAESIzIhEAAAERER"
"EQAAAAERERAAAAAAAREREAAAABEREREAAAESIjIhEAARIkRUMhEAESRf/UIRABEk//9TEQARNf//UyEBETTf/UIRAREjRVQyEQESI0RDIREREjREMiERESNF"
"VUMhEBEkb/1TIRASNf//YyEQEjb//2MhEBI23/5TIRASRt/3QxEAEjX/1TIRABI9/lQhEQARJNRCIRAAESIzIhEQAAEREREQAAAAERERAAAAAAAAAAERERAA"
"AAAAARERERERAAAAERESIiIiEQABERIiM0RDIhABESIzRVZt4yERESNFZ33v/0IRESNWfO///+QhESNX7///7XUyERI2///tqHVCIREkb//aqXZDIRASTf//"
"/ZhkMhEBI1ff///nUyEQESNWfe///TIRAREjRWeO/1MhAAERIjRFZtQhEAABERIiMzMiEQAAAREREiIhEQAAAAABEREREQAAAAERIiIiIiERAAESIzRERDMi"
"EQARNFZndmZUMhARJN7u7u7u4yEBEk7//////kIQETTv/////9QhESNGiZmZmHUyERI1Z4mZmHZDIREkV4mqqZhkMRESTv/////+QhERJP//////1CEREjzM"
"zMzMxDIRARI0VVVVVDIRABESIzMzMyIREAARERIiIhERAAAAAAEREREAAAAAABEREREREAAAABESIiIhEREAAAESNERDMiERAAASNO12VDMhEQARI9//2HZT"
"IhEBEj3///7HVCEQESNXzv///VMhARI0aJrf//xCEREjVomq7/9kIREjRonv///lMhESRt////2GQhERJP///odlQyEREk/+h2ZUMiEQESPVVUQzIhEQABEj"
"MzMiIREQAAERIiIREREQAAABERERERAAAAAAARESIiIRERAAERIjMzMyIREBEiNFZmVUMhEBEkV83d12QyERJF3////9ZCERJO//////1TIRJP/9qt//5jIR"
"JF54iq7/5jIRI0Vmea//1TIBEjNGee//dDEBESNGjv/4UyEBESNX//6GQhEBESNe/+dUMhEBESRe7nVDIRABESRnh1QyERABEjXe1lMhEQABEjX//UMhEAAB"
"Ej3//kIREAAAEjXv9TIRAAAAESNFQyERAAAAERIjIhEQAAAAAREREREAAAAAAAEREQAAAAAAAAABERESIiIREREAAAAAABEREiMzMzMiIREAAAAAEREjNFVW"
"ZVRDIhEQAAAREiNFZ93d3XZUMhEAABESNFff/////+dkMhEAERI0bf//7d3u//1kIREBEjRt/+qqqaqq7/xTIRERJG3/2qqqqqqq7/dDEREjXP/aqu7+2u7q"
"/9UyERNH/+qt////7/6t/mQhEjXf+qz//u7//9qv90IRJG7+qv/9qq3/+qr/dCESR//ar/6pmq//qq/3QhE1f/qt/9qYqv/6rf5kIRNY/6rf/amaz/6q79Yy"
"ETWP+q3/6qqv/9qv91MRE1f/qq//7d7//q7+ZCERJH/+qt////////50MhESNu/6qt///a3//nUyEQEjV//qqqqqqZmHZTIREBEkXP/8qqqph2VUMhEQAREk"
"bf//7u7/1UMiERAAARI0V+/////9QyEREAAAERIjVnfd3XVDIREQAAAAERIjNFVVVDMhEQAAAAAAEREiIzMzIhERAAAAAAAAAREREiIRERAAAAAAAAAAAAAA"
"EREREREAAAAAAAABESIzIhEQAAAAAAAREjRVQyERAAAAAAERJFfd1TIRAAAAABESNX//91MhEAAAABEjV+///WQhEAAAARI0af///nUyEQAAERJGj//v/4Uy"
"EQAAESNX3/6v/5ZCEQABEjRp//qu/9dCEQAREkaO/9qt/+hTIQARI1ff/6qq//lTIQERJGn//aqq//lkIQESNY7///////10IQESRt////////50IREjWP//"
"7u7u//+FMREkbv/6mIiar/+FMREk3/+XZVVnn//VMREk//5lQzNFbv/kIREjRVVDMiIjRVVDIQESIzMiIREiIzMiEQEREiIRERERESEREAAAEREiIiIiEREQ"
"AAAREjMzMzMzIhEQABEiNFZmZmVUMhEQERJF3d3d3YdlMhEBEjXf//////51MhARJG7///////5kIREjV+/+qqrf//dCERI1j//aqqrf/4UxESRp//qqqq3/"
"90MREkff/6qqrf/9dCESNX7///////2WQhEjWO///////8lkIRI2n//93d3//5ZCESRp//qqqqr//WMhEkbf/6qpqq//1jIRJH7/+qqqrf/9UyESR+//7u7v"
"//90IREkb////////nUyERI1//////7sdTIRARJFd4iIh3ZUMhEQESI0RVVVREMiERAAERIiMzMyIiEREAAAABEREiIiIRERAAABERIjMzMzIhEQABESI0VW"
"ZlVDIRABESNFeN3t12QyERESNWjv////5kIRESNX3///////UxESNH7//+3d7/9TERJG3//qh3d95UIRI1f//6hlVVVEMhEjbP/9hkQzMzIhECRu//p1QzIi"
"IREQJH7/6WVDMiIREQAkfv/pdUMzIiERACR+//p2RERDMhEAI23/+pdmZmVDIQAjWP//2piI/1MhEBJG3///7v//9CEQEjV+//////9jIRARI1aO////1kMh"
"ABESNFZ4iHZUMhEAAREiNEVVRDIhEAAAEREiIzMiIREQAAABEREREREREQAAAAARESIiIiERERAAAAERIzMzMzMiEREAABEiNFZmZlVDMhEQAREkXd3d3Ih2"
"QyERARI13//////tdTIRARJG7///////51MhESNH7/7d3e///mQhESNY//2piJr//4UyESNp//qHZnmv/9YyESRt//p2VWev/+ZCEjR+//llRFef/+ZCEjWO"
"/+hlRFev/9ZCEjWP/9hlRWiv/9YyEjaf/6hlVnnv/4UyEkbf/6l3eJ3//mQhEkfv/6qpmu//91MhEkfv////////hkIREkb////////nVCIREjX//////thl"
"QiEQESRXeIiId2VDIhEAESI0RVVVRDMiEREAAREiIzMzIiIREQAAAAARESIiIiIREQAAAREjMzMzMzIhEAARIjRWZmZVRDIRAREkXd3d3d3dQhEBEjXf////"
"//9TEQESRu///////1MRESNY7/7d3d3dQhERI1n//aqZh2QyEREkaf/6qph2UyERESR9//qqqYdTIRASNY7//////1MhEBI1jv/////+UyEQEjaf//3d3d1D"
"IRASRp//qqmYdUIREBJG3/+qmYdlMhEAEkfv/qqqmHVCEQASR+///////kMRABJG///////+QhEAEjX///////5CEQARJFd4iId3ZDIRABEiNEVVVEQzIRAA"
"AREiIzMzIiERAAAAABERIiIiIhERAAABESMzMzMzMiEQABEiNFZmZlVEMhEBESRd3d3d3d1CEQESNd///////0MRARJG7///////QxERI1fv/t3d3d1CEREj"
"WP/9qZh2VDIRESRp//qph2VDIRERJH3/+qmHZUMhEBI1fv/93d3dQyEQEjWO//////5DEQASNZ///////kIRABI2n//t3d3VMhEAEkbf/6mHZUMhEAASR+//"
"l1RDMhEQABJG7/51QyIREQAAEkb//WQyIREAAAASNf/2QyEREAAAABEjVVQyERAAAAAAERIzMiERAAAAAAABERERERAAAAAAAAAAERESIiIiEREAAAARESIz"
"NDMyIREAABESI0VWZlVDIREAERI0V43e3YZTIRAREjVo7/////1TIQESNX3///////5CERI0fv//7d3v/9QhESRt//6ph3iO1TIRI1f//6hmVWZlQyESNs//"
"2GVERVVDIREkbv/6dURFZmVCERJH7/6XVEVt/+QyESR+/+l1RFfv/VMhEkfv/6hlVo7/1TIRI23//Jh3iv/4UxESNY///ama3/90IRASRt///+7///ZCEQEj"
"R9///////VMhEBEjRo7////XVCERAREjRWeIiHZUIhEAAREiNEVVVDMiEQAAARESIjMzIiERAAAAAREREREREREAAAAAABERERERERERERAAAREiIiERESIi"
"IREAERI0QzIhEjNEMhEAESNd3UMiI0XdUyEBEjXf/2QzNG//5CEBEkbv/3VDRY//1TERI0fv/oZUV5//hTIRI1j//ZdmeN//hTIRJGn/+qmImu/+hTIRJH3/"
"/aqqqv/+dCESNX7////////9dCESNY7////////5ZCESNZ///u7u7//5YyESNp//qpmarv/oUyESRt//qHd4r//nQyESRu//llVXn//WQhESRu/+dURWn/+F"
"MhESRv/9ZDNF3/90MRESNf/2QyI07/5TIRARI1VUMiIjRVUyERAREjMyIRESIzMhEQABEREREREREREREAAAABEREREAAAERIiIREAAREjRDMhEAESNd3UIR"
"ARI13/9TIQESRu//YyEREkfv/mQhESNY//1kIREjaf/5YyERJG3/+FMhEjR+//hTIRI1jv/nQyESNY//1kIREjaf/5YyERJG3/+FMhESRu//dTIREkbv/mQh"
"EBI2//1TIRASNf/2QhEQESNVVDIRABESMzIhEAABEREREQAAAAAAEREiIiIREAAAABESMzMzMiEQAAABIkVWZlVDIRAAARJF3d3d3UIRAAARNf/////1MhAA"
"ARNf/////mMhAAARJN3d3//WQhAAARI1aJr/+WMhAAARIjRo3/+FMhAAAREjRo3/+FMhAAAREjRo7/50MhAAERIjRp//10IRABESIzR5//lkIRAREjNEV9//"
"hTIRARI0Vmee//dTIRASNd6Imv/9ZCEQESPf/+7//3UyEQESPf/////WQhEQASNd7//+hkMhEAARI1Z3h2VDIRAAAREjRFVEMiERAAABESIjMiIREQAAAAER"
"ERERERAAAAAAABEREREREREREQAAABESIiIhEiIiIREAABESNEQzIjNEQyIRAAESNd3VQ0RW3dwhEAESNd//dVVo//9CEQARJG7/+HeJ//5UIRARI1fv/qmd"
"//51MhABEjWP/9qt//51MhEAESRp//qu//2FQiEQARJH3/+u//2GQyEQABI1jv/+//2nUyIRAAEjWO/////adTIREAASNp//////p1MhEQABI2n//+r//oZC"
"ERAAEkbf/+qt//10MhEAASR+//qZrv/4UyEQABJH7/6XeJ//5kIRAAEkb//XVWff/9QhEAASNf/2VERX7/9CEQABEjVVQzIzRVVDIRAAERIzMyIiIjMyIRAA"
"ABERIhERERIiEREAAAAAEREREQAAAAAAERIiIREAAAAAERI0QzIRAAAAARI13dQhEAAAARI13/9TIRAAABEkbv/2MhEAABESR+/+ZCEQAAESNY//1kIRAAAR"
"I2n/+WMhEAABEkbf/4UyEQAAEjR+//hTIREAASNY7/6FMiEQABI1j//YUzIREAEjaf/6hkMyERASRt//qXZUMhERJH7/+qmHZTIREkfv//////0xESRv////"
"//9TIRI1///////0MRESRXeIiHdlMhERIjRFVVREMhEAERIiMzMiIhEQAAAREREREAABEREREQAAAREiIiERERESIiIREAAREjREMhERESNEQyIRAREjXdZT"
"IhIiNW3dQhEBEjXf/3UyIjNX7/9TIQESRu//1kMzRX3//mMhESNY7//oZERXr//9YyERI1n///p1VXn///ljIREkaf///Zdnnv//+VMhESR9////qInv///4"
"UyESNX7/7v/arf/+/+hTIRI1jv/d/+qv/67/10IREjWf/6r/+v/9r/+mQhESNp//qt///9qv/5YyERJG3/+qr//+qt//hTIREkbv/pie//qa7/50IRASRu/9"
"hn3/mInv/WQhEBI2//hlVsdmZ//3UyEQEjX/9kQ0RERG//VCEQARI0VUMiIiIzRVVCIRABESMzIiERESIjMyIRAAARERERERERERERERAAAAABERERERERER"
"ERAAAREiIiIRESIiIREAESI0RDMiIjNEMhEBESRd11QzM0XdUyEBEjXf/4ZENG7/5CEBEkbv/+hlVo//1TERI1jv//2GZ5//hTIRI1n///+oeN//hTERJGn/"
"///amu/+dTIRJH3//v/6qu/+dCESNH7/6v/+qv/9dCESNY7/6u//yv/5ZCESNY//2q///f/5YyESNp//qq3////oUyESRt//qIrv///nQyESRu/+l3iv///X"
"QhESRu/+dlaO//+WMhESNv/9ZEVo//91MRESNf/2QzNG3/5TIRARI1VUMiI0VmVCERAREjMyIiIiMzMiEQABERERERERIiIREAAAABEREiIiIhERAAAAARES"
"IzNDMyIREQAAERIzRWZmZUMiERABESNFeN3t2HVDIRAREjVo7////+hkMhERI1ff///////WQhESNH7//+3d7//4UyESRt//6piIrf/+ZCEjV///qGZmiu//"
"dCEjbP/9hlRFed//hTEkbv/6dURFaN//hTEkfv/pZURFaN//dCEkfv/pdURFee/+dCEkfv/6dlVWiv/9ZCEjbf/6l3Z4r//4UyEjWP//ypmd///WQhESRu//"
"/+7///1kMhESNX7//////9ZTIRARI1aO///+12QyERAREjRWeIh3ZUMhEQABESI0RVVUMyIREAAAEREiIzMyIhERAAAAAREREREREREAAAAAABERIiIiIRER"
"AAABEiMzMzMzIhEQABEiNFZmZlRDIREBESRd3d3ch2UyEQESNd//////51MhARJG7///////dDERI1fv/93e///WMhEjWP/9qqqv//ZCESRp//qpia7/90IR"
"JH3/+piJrv/nQhI1fv/6qZqv/9YyEjWO/+qqrf//hTISNZ////////1kIRI2n///////hkMhEkbf/+7u7YdUMhESR+//qYh2VDIhERJH7/6XVUQyIREQEkb/"
"/XVDMiEREQASNf/2UyIhEREAABEkVVQyIRERAAAAERIzMiEREAAAAAABERIRERAAAAAAAAAAERESIiIiEREAAAABERIjM0MzIhERAAAREjNFZmZlQyIREAER"
"I0V43e3YdUMhEBESNWjv////6GQyEREjV9///////9ZCERI0fv//7d3v//hTIRJG3//qmIit//5kISNX//+oZmaK7/90ISNs//2GVEV53/+FMSRu//p1REVo"
"3/+FMSR+/+l1REVp3/90ISR+/+l1REV57/50ISR+//qGVWeK//1kISNt//qYd3mv//hTISNY///Kqq3//9ZCERJG7///7v///YUyERI1fv//////2XQyEREj"
"V57////aqGQyERESNGiv//6q33QxEAERI0V57////+QhEAAREiNGjv///9QhEAABESI0V4zddkMhEAAAEREiNFVVVDIRAAAAABERIjMzMiEQAAAAAAERERIi"
"EREQAAAAEREiIiIhEREAAAESIzMzMzMiERAAESI0VmZmVUMhEQERJF3d3d2HZTIRARI13//////nUyEBEkbv//////91MhEjV+//3d3//+YyESNY//2qqq//"
"90IRJGn/+qmJrv/3QhEkff/6qImu/+dCEjV+//qqqq//5kISNY7/6qqt//+FMhI1n////////WQhEjaf//////6nUyESRt//7u//+oZCIRJH7/+qrf/6hTIR"
"Ekfv/piK7/51MhESRv/9dmef/9UyERI1//dURW7/9CEQESRVVUMzRWVDIRAREjMzIiIzMyIRAAEREiERERIiERAAAAABEREiIiIREQAAABESIzMzMyIREAAB"
"ESNFVmZVQyEQABESNWfN3d1lMhEAESNX3/////5CEQARNH7//////kIRARI23//qqt78QhEBEkb//qqYh2UyEQESR//9qph2VCIRARJG///qqYZTIREBEjbf"
"///+qGQyEQESNX3////+dTIRARI0Z53v///mMhEBEjRWearf//dCEREjRWd4mq3/90IRESNf2Zmqrv/mQhERJO//7t7//9UyEREk///////9ZCEQESNd7///"
"/oZDIRABEjVneIh2VDIRAAERIzRVVUQyIREAABESIjMzIiEREAAAARERERERERAAAAAREiIiIiIhERAAESIzNEREMzIhEBEjRVZmZmZVQyEREk3d3d3d3d1C"
"ERI1////////9TERI9////////9TERI13d3v/93d1CEREkV5r//qh1QyEREjRYr//YZTMhEBESNXr/+nUyIRAAESNX3/+WQyERAAESNX7/+FMhEQAAESNY//"
"51MhEAAAESNY//1kIhEAAAESNp//ljIRAAAAESNt//hTIRAAAAESNu//dCERAAAAESNf/+YyEQAAAAARNP/9QyEQAAAAARI0VUMhEAAAAAAREjMyIREAAAAA"
"ABERERERAAAAAAABEREREREREREQAAERIiIREREiIhEQARIjRDIhEiNEMyERESRd3UIiI13dQhERI17/5TIjXP/1MhESR//9VDNG3/9kIRI1j//WQzV+/+ZC"
"ESNt//hkNFj//WQhEkff/4ZERp//ljISNH7/6FRFfP/4UyEjWP/9hURX3/+FMhI1j//YZVaO/+dDISNs//qGVXn//WQhEjbf/6hmaK//hTIRI13/+ph4rv/3"
"UyESNY//6qqt//1kIRASRu///u///nQyEQEjV//////+dTIRABEjV9///+11MhEQAREjVniId2QyIRAAAREjRFVVQzIREAAAAREiIzMyIhEQAAAAARERERER"
"ERAAAAAAEREREREREREREQABEiIiERERESIiERARIzRDIhERIzRDIRARJN3VQyIiNF3dMhESNf/9VDIjRu//QhESNu/+dTM0bf/2QhESRu//hkRGj//mQhES"
"Nt//l1Vn7/91MhESNY//2GZ5//1kIRASNY//6Yif/+dTIRASNH7/+prf/4ZCERARJH3/+qr//XQyEQARJGn//a7/6FMhEQARI1n//d//lkMhEAARI1jv///9"
"dTIRAAABEkff///4ZCERAAABEjaf///HQyEQAAABEjV///51MhEAAAAAESRu//dTIREAAAAAESJFZmUyIRAAAAAAARIjNDMhEQAAAAAAABESIiIREAAAAAAB"
"ERERERERERERERERERABESIiIRESIiIiEREiIiEQESNEMyIiNERDMiIjRDMhESNd1UMzNW3dVDM0Vt1CERNO/+ZDRX7/91Q0V//1MhI13/91RXn//9dUV+/+"
"UyEjXf/5Zmjv//2GZ5//dTISNZ//qHiv///pd47/50IRI1j/+piu///+qJr/+GMhEjWP/9qq//3/+qrv/nQyESNY//2q7/2v/6rP/5ZCEQI1jv/qz/+q//qv"
"/+dTIRAjR+/+rv/ar/+t//lkIRABJH3//f/6qu/9//11MhEAEkff///9ma7///+WQhEAASNp///+mHnf///XUyEQABI1j///qGaN///4YyEQAAETR///6GVW"
"n//9ZDIRAAARI1//92Q0V///dTIRAAABESRWZlQzM0ZmZDIREAAAARIjNDMiIiM0QzIREAAAAAERIiIhEREiIiEREAAAAAAAABEREREREREREAAAAAESIiIi"
"IiIiIhEAAAARIzREMyMzRDMhEAAAESTd1lREVt3dIRAAABI1//11Vn7//TEQAAASNe//mHjf/9QhEAAAETWP/9qt//1kIRAAABEkff/63//XUyEQAAARJGn/"
"///+hTIRAAABESNo3///6WQyEQAAARIkWK///qdTIREAAAESNGmv//2nQyEQAAAREjV6///+p0MhEAAAESNGn////9dTIRAAABEkaO//rv/4UyEQAAESNX7/"
"+qr//WQhEAABEkbv/9mJ7/90IRAAARNO//12Z5//1CEQAAET7//WVEVu//QhEAABEjRVVDMzRVVDIRAAABEjMzIiIiMzIhEAAAARESIhERESIhERAAAAERER"
"EREREREREAAREiIiERESIiIRAAESNEMyIiIzQzIRARI93VQzM0Vt3TIQESTv/mVERX//0yEBE03/+GVWjv/lMhARJH//2HeO//dDEQESRu//qZ3/+FMhEBEj"
"af/6qv/9ZCERARI1jv/q//11MhEAESNH3////oUyERAAESRp////lkMhEAABEjWO//+nUyEQAAARI1ff/+hUIREAAAESNH3/+nQyEQAAAAESR+//hTIRAAAA"
"ABEkfv/nQyEQAAAAARJG//1jIRAAAAAAESNf/2QyEQAAAAAAESNVVDIRAAAAAAABESMzIhEQAAAAAAABERERERAAAAAAAAABERIiIiIiEREAAAESIzNEREMz"
"IREAARI0VWZmZmVTIRAAESTd3d3d3d1TIQABI1////////5CEAASPf///////9QhAAEjXd3d3f//5kIQABEkVomq3//nUyEAARI0V4rf/+hkIRABESNFed//"
"6GQyEQARIjVp3//oZDIRABESNGjf/+llMiEQARI0aN//6XVDIREBESRo7//qhkMyEQARI1jv/+qYZUMhEAESR+//6qqYdkMhABI17////////TIQASNf////"
"////0yEAEjX////////1MhABEkVniIiId2VCEQARIjRFVVVURDIREAAREiIzMzMiIhEQAAAAARERERERAAABESIzMiERAAERI0RVQyEQABEjVnd2QyEAESNH"
"///+QhABEjbf///UIQARJH7/7dUyEBEjV//9hkMRARI1j//HUyEQESNp//lkIhEBEkbf/4UyEQESNH7/51MhEBEjWP/9dCIRARI1j//GQhEAESNp//ljIRAB"
"Ekbf/4UyEQASNH7/51MhEAEjWP/9dDIRABI1j//XQyEQASNp//p0MhEAEkbf/6dTIRABJG7//dUyEAASRv///1MhAAEjX///9TEQABEkV3d1QhEAARIzRVQy"
"ERAAARIiMiIREAAAAREREREQAAAAEREREAAAESIiERAAESMzMhEQASNN1TIRABI1/+UyEQEkb/5kIRASR//3QhERJG7/dTIREkbv+FMhESRt/9ZCERI2j/1k"
"IREjWP/nQhESNY//dTIRI0fv+FMhESR+/4UyERJG3/1jIREjbf/WQhESNY/+dCERI1j/50IRETR//3QxARJG7/dTEBEjbf91MQESNd/9QxABEkX/1CEAESNF"
"VDIQABEiMyIRAAARERERAAAAERERAAAAARERERERAAABESIzMiEQAAARI0VUQyEQABEjVnh2QyEAARNO///2MhAAETX///9kIQABI13e/+dCEAASNHnv/XQh"
"AAESRp//1kIQABIkaf/5YyEAESJH3/+FMhABEjR+/+dTIQARI1jv/XQiEAESNY//1kIRABEjaf/5YyEQERJG3/+FMhEBEjR+/+dTIRARI1jv/XQhEBESRo//"
"1kIRARI0ef/5YyEQESRo3/+FMhEBI13e/+dCEQASNv///WQhEAEj3///1TIRABI0Z3dlMhEAARI0RUQyERAAARIiMiIREAAAAREREREQAAAAAAAREREREAAA"
"AAERIiIhEQAAABESM0QyERAAAREjRmZTIRAAARI1f//VMhEAESNG7//mQhEBESRo///3QhEBEjV+/q/3UxEBEkbf+q/8UyEBE1f/2Z79YyERI17/h47+YyER"
"I9/3Zm3/UyERI+/lREX/QxEBEjRDMzREMhEBESIiIiIiIRAAEREREREREQABERIiIiIhERABEiM0REMzIRARI0VWZmZUMhERJP//////4xERLf//////0xER"
"I0VWZmZUMhEBEiM0REMzIRABERIiIiIhERAAEREREAABESIhEQABEjMyIRARI93UMhERI+/9QhEBI07/QhEBEjTe0hEBESMzIhEAEREiERAAABEREQAAARES"
"IiIiIiERAAERIjREREQzIhEBESNFZ3d2ZlQyEREjV97/7Z3u1CERI1fv///+//1TESNH7//+////hTISRt//2qrf//hTISR//+qZqv//hTITWP/6mImv/+dD"
"EjWP/6mImv/9dCETWP/6qZqv/5ZCETR//+qqr//4UyESRu///u///3UyESNX///////mQhERI1fv//3v/VMhEREjVnd3d3ZTIRABESM0RUREMyERAAERIiIz"
"IiIhEQAAAREREREREREAAAAAEREREQAAAAAAABESIiEREAAAAAAREjRDMhEQAAAAARI1ZlQyERAAAAESNd//VDIREQAAESRu/+dUMiERABEjV//9h1RDIREB"
"EjWP/6qYdlMhEREkaf/67/7XUyEREkff//////dTIRI1fv///v//5kIRI1j//9qq7/+FMhI2n//qmar//VMhJGn/+qiJr//WMhJH3/+piJr//VMhJH7/+qma"
"3/+FMhNY///aqq//50IRNY///+7v//hTIRJH//7////4ZCERJN/+rf//52QyERI0Vmd3d3ZUIhEBEiNEREVEQyIREAERIiIjMiIhERAAAREREREREREAAAAB"
"ERIiIiIREQAAERIjNEQzIhEAAREjRWZ2ZUMhEBESNWju/+1kIRARI1fv////5TIQEjR+///v//5CERI23//amd/lMhESR//+qHd2VDIRE1j/+pdlVUMhEBNY"
"//qHZVRDIRATWP/6mHdmQyEQEkf//qmZ7lQhEBJG7//+7//0IRASNH7/////YyEQESNX3v//1kMhABESNFZ3dlQyEQABESM0REQyIRAAABERIiIiIREQAAAB"
"EREREREQAAAAAAAAAAABERERAAAAAAAAARESIiERAAAAAAEREjNDMhEAAAABEREjRWZDIQAAERESIjRv/9QhAAEREiMzRo//dCEAERIjRFVnn/+FMQERI0Vn"
"iIrf/3UxERI1fe/+2u/+dCERI1fv///+//10IRI0fv//7///+mQhEjbf/9qq3//5YyESR//+qZqv//hTIRNY//qoia//6FMhE1j/+piJr//XQhETWP/6qZqv"
"/5ZCERJH//6qqv//lTIREkbv//7v//+FMhESNX///////mQhEBEjV+///e/9UyEQERI0Z3d3d2UyEQABESM0RVREMyERAAAREiIjMiIiERAAAAERERERERER"
"AAAAARESIiIiEREAABESI0REQyIREAERI0Vnd2VDIREREjVo7v/tZDIRESNX7////+ZCERI0fv/+3v/9UyESRt//qqrf/3QhEkf//aqqr/90IRNY////////"
"dCEjWP///////2QhE1j/+qqqqpdTIRJH//2qqql1MhESRu//7t7/ZDIREjV+/////9QhEREjV97//+ZDIRAREjRnd3dkMhEAAREjNEREMyERAAARESIiIiER"
"EAAAAREREREREAAAAAABEREREREAAAAREiIyIhEQAAERI0RVQyEQABESNWfdxTIQAREjV+///0IRARI1fv///UIRESNGn//a1jIRESRp7/6qdTIREjXd///d"
"dDIREjb/////dCEQEkb/////ZCEQEkad//qXUyEQEjWO/+l1MhEAEjWO/9hTIhEAEjWP/5dDIRAAEjaf/5YyERAAEjbf/4UyEQAAEjbv/mQhEQAAEjXv/VMh"
"EAAAETT/9kIREAAAESNFVDIRAAAAERIjMiEQAAAAAREREREAAAAAAAARESIiIiIiEREAAREiNERERDMyEQAREjRWd3ZmZUMhAREjV97/7Znu5CEBEjV+////"
"7f/lMhEjR+///v///9YyESNt//2qrf//ljIRJH//6pmq//+VMhE1j/+qiJr//4UyETWP/6qZqv/+hTIRNY//2qqt//10IREkfv/+3e//+mQhESRp///////5"
"YyERI1ev///u//hTIREjV5qtyq7/10IRESRXmqqq3/+GMhERJG/92t7//nQyEREk///////nUyEQESNe/////XUyERABEkV43ch2QyERAAESI0VVVUQyERAA"
"ABESIzMzIiERAAAAARERIiERERAAAAAAEREREQAAAAAAAREiIhERAAAAABESNEMyERAAAAARI1ZlQyEREAABEjXf/1QyEREAARJG7/51QyIREBEjV//9h1VD"
"IRERI1j/+qmHZTIRESRp//re/+dTIREkff//////dTISNX7//////+YyEjWP//2qr//3QhI2n//qqq7/90ISNp//qYiu/+dCEkbf/6d3nv/WQhJH7/6HZ5//"
"1jISR//9dlef/4UyEkf/+WVW3/90MRJG//dURu/+ZCESPf/1QzXv/UMhESRVVDIzRVQyEREiMzIiIjMyIREBEREREREREREQAAABEREQAAAAERIhERAAARIj"
"MyERABEjRVQyEQARJN//UyEBETTv/1MhARI17/9TIQESNX3WUyEREkV4dkMRESNH7uZCEREjXf/2QhERJG7/5kIREjV//9ZCERI1j/+WMhESNp//hTIREkbf"
"/3UyERJH7/50IRASR//9ZCEQEkf/+FMhEBJG//dDERASPf/1MhEAESRVQyERABEiMyIREAABEREREQAAAAAAAAERERAAAAAAABESIREQAAAAARIjMyERAAAA"
"ESNFVDIRAAAAESTf/1MhAAABETTv/1MhAAABEjXv/1MhAAABEjVt1lMhAAAREjV4dkMhAAARI0bu5kIRAAARI13/9kIRAAARJG3/50IRAAERNH7/5kIRAAES"
"NY//1kIRAAESNY//ljIRAAESRt//hTIRABEjR9/+dTIRABEjV+/+dCEQAREjWP/9ZCEQARI0af/5YyEQASI1fP/4UyEQAiNWnv/nQhEAAjXY3//WMhEAAjX/"
"//50MhEAAj7//+dTIRAAAjRt12UyERAAASNERDMhEQAAARIiIiIREAAAARERERERAAAAAAARERERAAAAAAAAAREiIhEQAAAAAAAREjNDIhEQAAAAABEjVmVC"
"IREQAAABEjXf/1MiEREAAAESRu/+ZDIiEREAERNH//11RDMyEQARI1j/+oZmZUMhEBEkaf/6mI3u7iEQESR9//qq7//kMRASNY7/6q7//WQhEBI1j//a///X"
"UyEQEjaf/+//+oZCERASRq/////adTIRABJH3/////p1MhEAEkfv//7//YUyEQASV///qu//ljIRABJH//2qr//mQhEAEkf/+YeN//1CEQASPf/3VWf//0IR"
"ABIkVVVERVZUMhEAESIzMyIzMzIhEAABERIiESIiIREAAAAAEREREQAAAREiIhEQABESM0MhEQARI1ZlMhEBEjXf/1MRARJG7/5TIRESR//9YyERI1j/+FMh"
"ESNp//hTIREkbf/4UyESNH7/50MhEjWP/9dCERI1j/+WQhESNp//hTIREkbf/4UyERJH7/50IRESR//9ZCEQEkf/+FMhEBJG//dDERASPf/1MhEAESRVQyER"
"ABEiMyIREAABEREREQAAABERIiIiIiIiIiIhERAAESI0RERERERERDMhEQESNFZ3d3d2Zmd3ZDIRERJG7urf/9mZ3v/nUyERI13//////t////90MREkfv//"
"/////////9YyEjWP//2q3//+qq//5kISNY//2qqv/+qqrv/nQhI2n/+pia//2Yiu/+ZCEkbf/6h4r/+oeK//1kISR+/+l3jf/5dnr/+WMhJH//2GaN/+hmev"
"/4UyEkf/+XVn7/12V9//dCESRv/3VVf//WVW7/5jIRI9//ZERf/2VEX//UMhESRVVDM0VVQzNEVUMhERIjMyIiIzMiIiMzIhEQERERERERIhEREREREQABER"
"IiIiIiIREAARIjREREQzIREBEjRWZ3d3ZTIRERJG7ure/+dTIREjXf//////dTIRJH7//////+YyEjWP//2qr//3QhI1j//qma7/90ISNp//qYiu/+dCEkbf"
"/5d3nv/WQhJH7/6GZ5//1jISR//9dlef/4UyEkf/+WVW3/90MRJG//dURu/+ZCESPf/1QzXv/UMhESRVVDIzRVQyEREiMzIiIjMyIREBEREREREREREQAAER"
"EiIiIhERAAAREiM0REMyERABESNFZ3dmQyERERI1aO7/7XVCEREjV+/////3UyESNH7//+///3QhEjbf/9qq3//VMhJH//6pia//5jITWP/6mHiu/+ZCE1j/"
"+pd4rv/WMhNY//qoia//xTISR//+qqrv/3QhEkbv//7v//hTIRI1fv/////GQhERI1fe///XZDIRERI0Z3d3ZUMhEAERIzRERDMiERAAEREiIiIiEREAAAER"
"EREREREAAAABERIiIiIiEREAAAESI0REREMyERAAESNFZnd3ZkMhEQERJG7urv/tdTIRARI13//////3UyEBEkfv///v//5kIREjWP//2qrv/4UyESNZ//2p"
"mq//1TIRJGn/+piJr//WMhEkff/6mImv/9UyEjWO//qpmt//hTISNY///aqq//50IRI2n///7u//+FMhEkaf/+////+GQhESRt//rf//52QyERJH7/6piIdl"
"QiEQEkf//YZlVEMiERASRv/4ZUMyIhERABI1//ZTIiEREQAAETRWVDIhEREAAAARIjMzIRERAAAAAAERIiEREQAAAAAAABEREREAAAAAAAAAARESIiIiIiER"
"AAERIjREREQzIhEBESNFZ3d2ZlQyEREjV97/7Z3u1CERI1fv///+7/1TESNH7//+////hTISRt//2qrf//hTISR//+qZqv//hTITWP/6mImv/+dDEjWP/6mI"
"mv/9dCETWP/6qZqv/6ZCETR//+qqr//5YyESRu///u///4UyESNX///////nUyERI1fv//3//XQhEREjVniar/+WMhEBESNFV4r/+FMhEAERIjRX3/90IREA"
"ARESI17/5TIRAAABERIkVmVCERAAAAAREiNDMhEQAAAAABERIiIREAAAAAAAARERERAAAAARESIiIhEQAAESIzQzMiEQARI0VmZlQyERESRu7p3vQhERI13/"
"///kIRESR+////5CERI1j//+l1MhESNY//6XVCEQEjaf/6dUIhEBJG3/+WQyEQASR+/+dTIREAEkf//WQyEQABJH//hTIREAASRv/3QyEQAAEj3/9TIRAAAB"
"EkVUMhEQAAARIjMiERAAAAAREREREAAAAAABERIiIiIREAABESIzREQyIRAAESNFZ3dlQyEQESNGju//7UIRARJG7/////UxEBI13//t7v5TIREjbv/aqqhk"
"MhESNu/+2qqGQyERI23////ZZDIREjV9////91MhESNXmq3v/+UyERJGiaqq7/5TIREk3+3d3//FMREST//////mQhERJM7///7WQyEQEjRWd3dlQyEQARIj"
"REREMiERAAERIiIiIhERAAABEREREREQAAAAAREREREQAAAREiIiIREAAREjREQyERABEjRt3VMhEREjRt//dTIRESRo7/6XQxESNd7//+7TIRI2/////2Mh"
"Ejbv///uUyESNp3/+oZDERI1jv/pdTIREjWP/9hUIhESNY//p1MhERI1n/+nUyEQEjXf/6hkIRARNc//7eQxEBEkb///9TEQESNX7//kIRABEjRmZkMhEAER"
"IzRDMhEAABERIiIREQAAAREREREAAAARERERERERERABEiIyIiIiMiIRESNFVDMjNFRCEREkbu5TNF7u0yESNd/+ZEV//9QhEkbv/nVWj/90ISNX//12Z9//"
"dDEjWP/6dmjf/nQhI2z/+od57/50ISRt//qHiv/9ZCEkbv/qma3/+WMhJG7/+qqv//hTISNd///v///nQhETR///////5kIREjV+//6u/9UyEREjVniHd3ZT"
"IRAREjRFVERDMhEQAREiIjMiIiERAAAREREREREREAABEREREREREREAESIzIiIiIiIhEBI0VUMyM0REMhESTu7VQ0Vu7kIRE17/5lVX7/5CESNd//hmef/2"
"QhEjWP/6iJ//1TIRE1j/+qrf/nQxERNH7/2q//hjIRASR9/+rv/XQyEQEkbP/9//hTIREBI1n////GQyEQASNX///+dTIRAAESRu//+GQhEQABEjXf/9ZDIR"
"AAABEjVmZUMhEAAAAREjNDMiERAAAAARESIiEREAAAABEREREREiIRERERERARIiIiIiMzMiIiIiIRESNERDM0VWVDMzREMhESTu5lRGfu5lRF3u0yETXv/W"
"Zo7/+GZn//UyETXf/oeK//+oeN/+UyEjWP/qmu///amv/3UxETWP/6rf///qrv/WQhETWP/6r//f/qr/+FMhESR+/63/6v/67/1kIRESR9/9//qu/9/+hTIR"
"ASRt///9qu///5ZCERASNY///6md///nUyEQARJH///Yd5//+GQhEQARI17/52Vn//5kMhEAAREkVmZURFZmVDIRAAABEiMzMzIzNDMiERAAAAERIiIiEiIi"
"IREQAAAAAAEREiIiIhEREAAAARIjMzIjMyIRAAABEjRVVERVVDIRAAARJO7nZmfu7SEQAAETTv/pme/+QxEAABE1f//a7/5kIRAAERNH3//v/oUyEQABEjRq"
"///+lkMREAARI0et//+oUyEQAAESNXrv/+p1MhEAAREkaO///6dTIRAAESNX7////oUyEQABEkbv/6r/+FMhEAARNO//mZ7/5TIQAAES7/93Z4//0xEAABEj"
"RVVERVVTIRAAABEjMzMzMzMhEQAAARESIiIiIhERAAAAABEREREREREREAAAESIzIiIiIiIhEAARI0VUMyM0REMhEAESTu7VQ0Vu7kIRABE17/51VX7/5CEQ"
"ESNd//hmef/2QhEBEjWP/6iJ7/1TIRARE1j//arf/nQxEQERNH7/2q//hjIRABESR9/+rv/XQyEQAREkav/9//hTIRAAERJGn///+WQyEQABEiRp7//+hTIR"
"AAARI0aN//+WQhEQABEiNWnf/9dTIRAAARI1aK7/6FMhEQAAEjXtzf/4ZCIRAAABI9////1kMhEAAAASPv///GQyERAAAAETRs12VDIREAAAABEjRFRDIhEQ"
"AAAAABEiIiIhERAAAAAAABERERERAAAAAAAAAAEREiIiIiIREAABEjNEREQzIRAAESNGZ3d3ZTIRABEk7u7u7u5DEQASNf//////UxEBEjXu7u///VMRARI1"
"aKrf/9dCEQESNGit//2FMhEBEjVp3//YZDIRESNGjf/9l1MhEREjV9//2oZDIRARJH3//aqHUyEQEjXf//7u7tMhEBI1///////TIRASNf//////UyEAESRW"
"eIh3ZUIRABEiNEVVREMiEQABESIjMyIiERAAAAAAERERERAAAAARESIiERAAABESM0QyIQAAERI0ZmVCEQABEjV+//QxEAESNH///1MRABEkbf/9dDEQESNH"
"7/6GQhEBEjWP/9dDIRERJGn/+WQhEREjV9//hTIRARJGjv/nUyEQEjV9//1kIhABI27//oYyEQASNv//2FMhEAEjbO/+dTIRABI1jv/nQyEQASNY7/10MhEA"
"EjWP/9dCEQABI1j/+WQhEAASNd//p0IRAAEjXf/+dCEQABEkf//9QhEAARI13//UIRAAERI1ZlQyEQAAERIzQzIRAAAAERIiIREAAAAAEREREQAAAAAAABER"
"ERAAAAEREiIREAAAESIzMhEAABEjRVUyEQAREkb/9CEQARI13/5TIQARJG7/1TIQERJH7/hTIQESNY//hTIQESNY//dDIQESRt/+dCERESR+/9ZCEREjV+/4"
"YyEREjWP/4UyEREjaP/3UyEREkbf/nQhEREkfv/WQhEBI0fv+GMhEBI1j/+FMhEBI1j/90MhEBI23/5kIRABI27/1jIRABI17/dTIRABI1//VCEQABEjVVQy"
"EQABESMzIhEAAAERIiERAAAAAREREQAAAAAAAREREREAAAAREiIhERAAAREjRDMhEQABEjVmZDIRAAETTv/2QyEAASNf//5jIQARI13//3QhABEjWN//dCEA"
"ESNX3/50MQARI1jv/nQyABEjWP/9dDIAESNY//p1MgARI1j//oUyABEkae//1TIBESR5///VMQESNX7/7GQhARI1j/+nUyEREkbP/5ZCEREjR9//hTIRESNY"
"7/50MhERNGn//WQhEBI13v/3UyEQEjb//+ZCERASPf/9ZDIRABI0VmVDIRAAESM0MyIREAABESIiEREAAAARERERAAAAABERIiIREREQAREjMzMiIiERESI1"
"VVRERDIRESRd7tdmbtMhEjXv//6Y79MhEj3/7v///1MhEj79eN//9kIREjRVVWbdZDIRESIzM0REQyEQAREiIiIiIhEQABEREREREREAAAARERERAAABEiIi"
"IRAAESNERDIRABEkX/1CEQERNf//UxEBEjX//1MRARI13+ZDERESNXd1QhERIkbddTIRESNX/9UyEREkbf/VMhESNH7/hTIREjWP/4UyERI23/91MhESRu/+"
"dCEQEkf//mQhEBJH//1jIRASTf/9QyEQEjTd1TIRABEjREMhEQAREiIiERAAAREREREAAAABERERERAAAREiIiIREAESI0REMhERESRW3WQyEREjXv//1DIR"
"E07+iP9kIRI1/oiI/UIRI1/oiI/UIRE174iO/EIREkb/7/5TIREjRu/tVCEQESNFVUMiEQERIjMyIREAARERERERAAAREREQABERIREQESIzMhEREkVVMhES"
"Nf/1MhEk7//TIRJO//UyESNe/UMRESNEQyERESIiIRABEREREAAAAAARERERAAAAAAERIiIhEAAAAAESNEQyEQAAABEjXv5DEQAAAREk3//TIRAAARI07/9T"
"IRAAERI1fv1TIRABESNFeHZDERABEjRX3dVCERARI0V9//UyERARJFfv/3UyERASNY7/+GQyERATR//+l2VDIRAjXv/6l2ZUMhEjb//qqIjlQhEjbv/9qt//"
"QhEjXf/////+QhESRu/////VMhESNGfd3YdUIRARI0VWZlQyIRAREiMzMzIhEQABEREiIiEREAAAAAAAAAABEREREAAAAAAAAAAREiIhEQAAAAAAABESIzMy"
"EQAAAAAAABEjRd3dIRAAAAAAARI0Xv/jIRAAAAAAERJF7/1TIRAAAAABESNd7pdTIRAAAAABEjRp3ddTIRAAAAAREkaP//hkIRAAAAARI1fv//10IhEAAAES"
"NHn///6FMhEAABESRo//7/+WMhEAABEjV9/+r/+WQhEAARI0af/6rv/XQiEAERJGjv/arf/oUyEAESNX3/+qqv/5UyEBESRp//2qqv/5ZCEBEjWO///////9"
"dCEBEkbf///////+dCERI1j//+7u7v//hTERJG7/+piImq//hTERJN//l2VVZ5//1TERJP/+ZUMzRW7/5CERI0VVQzIiI0VVQyEBEiMzIiERIiMzIhEBERIi"
"EREREREhERAAAAAAABEREREQAAAAAAARERIiIhEAAAAAARESM0QyIRAAAAEREiNF3d0hEAAAERIjNW7/5DIRAAESIzRX7/1lMhEAESJFZ43uqXVCEQERJG3d"
"3d3d3UMRARI13///////UyEBEkbv//////9TEREjWO/+3d3d3UIRESNZ//2qqYdlMhERJGn/+qqYdlMhEREkff/6qqmHUyEQEjWO//////9TIRASNY7/////"
"/lMhEBI2n//93d3dQyEQEkaf/6qpmHVCERASRt//qpmHZTIRABJH7/6qqph1QhEAEkfv//////5DEQASRv///////kIRABI1///////+QhEAESRXeIiHd2Qy"
"EQARIjRFVVREMyEQAAERIiMzMyIhEQAAAAAAARERERAAAAAAERIiIREAAAAREiMzMhEAAAARI0Xd3SEAAAESJF7/4yEQABESNe/9UyEQABEjXe51QhEAAREk"
"bd11MhEAARI13/90MhEAARJG7/90MhEAESNH7/50IREAESNY//1kIRAAESNp//ljIRAAESRt//hTIRAAEjR+//hTIRAAEjWO/+dDIRAAEjWP/9ZCEQAAEjaf"
"/5YyEQAAEkbf/4UyEQAAEkbv/3UyEQAAEkbv/mQhEAAAEjb//VMhEAAAEjX/9kIREAAAESNVVDIRAAAAERIzMiEQAAAAAREREREAAAAAAAAAAAEREREREQAA"
"AAAAAREiIiIiIRAAAAAAERI0REQzMhEAAAABESNG3WZtQyEQAAAREjTf/+zf0yERAAESI0X/3v//ZDIRABEjRWfceI7YdkMhAREkbdmHdneI3VQhARI13/+Y"
"ZmeO/+UxARJH7//pdmef/9UyESNY7//9l3iv/4UyESNZ////qInf/4UyESRp////2prv/oUyESR9//7/+qrv/nQhEjR+/+r//qr//XQhEjWO/+rv/8r/+WQh"
"EjWP/9qv//3/+WMhEjaf/6qt////6FMhEkbf/6iK7///50MhEkbv/pd4r///10IREkbv/nZWjv//ljIREjb//WRFaP//dTEREjX/9kMzRt/+UyEQESNVVDIi"
"NFZlQhEQERIzMiIiIjMzIhEAARERERERESIiERAAAAAAAAAREREREAAAAAAAAAEREiIhEQAAAAAAARESM0QyIRAAAAAAERIjRd3dIRAAAAARESI1bv/kMhEA"
"AAERIjRW7/1kMhEAABESNFZ97ql1MiEQAREjRnjd7dqGUyERERI1aO/////pZDIRESNX3///////1kIREjR+///t3e//+FMhEkbf/+qYia3//mQhI1f//6hm"
"Zorv/3QhI2z//YZUVXnf/4UxJG7/+nVERWjf/4UxJH7/6WVERWjf/3QhJH7/6XVERXnv/nQhJH7/+nZVVor//WQhI23/+pd2eK//+FMhI1j//8qZnf//1kIR"
"Ekbv///u///9ZDIREjV+///////WUyEQESNWjv///tdkMhEQERI0VniId2VDIREAAREiNEVVVDMiERAAABERIiMzMiIREQAAAAERERERERERAAAAAAAAAAAB"
"EREREAAAAAAAABERIiIREAAAAAABEREjMzIhEAAAABEREiNd3dIREAABERESI17/5DIRAAERIiIjTv/WQyIRARIjREM97ndlQyERESRd3URFVn3dQyERI17/"
"5URFbP/1MhESR//9ZDRX3/9kIRI1j//WQ0V+/+ZCESNt//hkRGj//WQhEkff/4ZERp//ljISNH7/6GRFfP/4UyEjWP/9hURX3/+FMhI1j//YZVaO/+dDISNs"
"//qGVXn//WQhEjbf/6hmaK//hTIRI13/+ph4rv/3UyESNY//6qqt//1kIRASRu///u///nQyEQEjV//////+dTIRABEjV9///+11MhEQAREjVniId2QyIRAA"
"AREjRFVVQzIREAAAAREiIzMyIhEQAAAAARERERERERAAAAAAAAAAEREREREQAAAAAAAREiIiIhEQAAAAAREiMzMzMhEQAAABESI01VXUMhEQAAERIjX/1u/0"
"MhEAARIjNF/9fv9UIhEBEjRFVd1mfXZTIRERJF3dZVVWjd1TIREjXv/mVEVs//YyERJH//1kRFff/2QhEjWP/9ZERX7/5kIRI23/+WREaP/9ZCESR9//hkRG"
"n/+WMhI0fv/oZEV8//hTISNY//2FRFff/4UyEjWP/9hlVo7/50MhI2z/+oZVef/9ZCESNt//qGZor/+FMhEjXf/6mHiu//dTIRI1j//qqq3//WQhEBJG7//+"
"7//+dDIRASNX//////51MhEAESNX3///7XUyERABESNWeIh3ZDIhEAABESNEVVVDMhEQAAABESIjMzIiERAAAAABEREREREREAAAAAAAAAAAEREREQAAAAAA"
"ARESIiERAAAAAAERIjNDIhEAAAARESNF3d0hEAAAEREjRe/+QhEQABESI0Xv/WQyEQAREjRW3uiGVCEQESI0Z4mqmYZTIRESNX3v/tre7UIREjV+////7//V"
"MhI0fv//7///+FMhJG3//aqt//+FMhJH//6pmq//+FMhNY//qYia//50MSNY//qYia//10IRNY//qpmq//lkIRNH//6qqv//hTIRJG7//+7///dTIRI1f///"
"///+ZCEREjV+///e/9UyERESNWd3d3dlMhEAERIzRFREQzIREAAREiIjMiIiERAAABEREREREREQAAAAAAABEREREAAAAAABERIiIREAAAAAERIjMzIRAAAA"
"AREjRd3dIRAAABESNF7/5CEQAAERI0Xv/VQhEAAREjRd7odUIREBESNWeaqXZDIRERI1eO7/7XUyEREjV+/////nQyESNX7//t7//WMhEkbf/6qq3/90IRJH"
"//2qqq//dCETWP///////3QhI1j///////9kIRNY//qqqqqXUyESR//9qqqpdTIREkbv/+7e/2QyERI1fv/////UIRERI1fe///mQyEQERI0Z3d3ZDIRAAER"
"IzRERDMhEQAAEREiIiIhERAAAAERERERERAAAAAAABEREREAAAAAERIiIREAAAAREjMzIRAAABESNN3dIQAAERI17/4yEQABEjTv/UMhEAERJN7mUyEQABEj"
"V3dkMhEAESNG7uZCERABEjXf/2QhEQARJG7/5kIRAAEjV//9ZCEQABI1j/+WMhEAASNp//hTIRAAEkbf/3UyEQABJH7/50IRAAASR//9ZCEQAAEkf/+FMhEA"
"ABJG//dDERAAASPf/1MhEAAAESRVQyERAAABEiMyIREAAAABEREREQAAAAAAAAABEREREREAAAABESIiIiIREAAAERI0RERDIhEAAREjRt12bUIRAAESNN//"
"7N/TIQAREkX/3v//UyEBESNW3Jqu11MhARI1eJqqqqhUIREjR+7q3v/pZDIRI23//////4UyESR+///////mQhI1j//9qq//90ISNY//6pqu//dCEjaf/6mI"
"rv/nQhJG3/+nd57/1kISR+/+hmef/9YyEkf//XZXn/+FMhJH//llVt//dDESRv/3VEbv/mQhEj3/9UM17/1DIREkVVQyM0VUMhERIjMyIiIzMiERARERERER"
"EREREAAAAAAAEREREQAAAAAAARESIiERAAAAAAERIjMzIRAAAAABESNF3d0hEAAAEREjRe/+MhEAABESI0Xv/VMhEAAREjNG3uhlMhEAERI0V4mph1MhERES"
"NWju/+2GQyEREjV+/////4ZCERI0fv//7///dDERI23//aqt//1TIRJH//6pma//5jIRNY//qYeK7/5kIRNY//qXeK7/1jIRNY//qoia//xTIRJH//6qqu//"
"dCERJG7//+7//4UyERI1fv/////GQhEREjV97//9dkMhEBESNGd3d2VDIRAAERIzRERDMiERAAARESIiIiIREQAAABERERERERAAAAAAAAABEREREAAAAAAA"
"ERIiIREAAAAAEREjMzIREAAAAREiNN3dIRAAARESI17/5CERABESIjTv/VQyEQESIzNN7nZUMhERI0VVVVZnZkMhESRu7mVVfu7UIRI13/51Vo//1DESRu/+"
"dVaf/3UxI1f//XZn3/91MSNY//p2aN/+dCEjbP/6h3nv/nQhJG3/+oeK//1kISRu/+qZrf/5YyEkbv/6qq//+FMhI13//+///+dCERNH///////mQhESNX7/"
"/q7/1TIRESNWeId3dlMhEBESNEVUREMyERABESIiMyIiIREAABEREREREREQAAAAABEREREREAAAAAERIiIiIREAAAAREjMzMzIRAAABESNNVV1DIRAAERI0"
"/9bv9DERAREiRf/X7/UyERESM0bdZn1lQhERI0VmZVZndlMhESRu7mVVfu7UIRI13/51Vo//1DESRu/+dlef/4UxI1f//XZn3/+FMSNY//p2aN/+dCEjbP/6"
"h3nv/nQhJG3/+oeK//1kISRu/+qZrf/5YyEkbv/6qq//+FMhI13//+///+dCERNH///////mQhESNX7//q7/1TIRESNWeId3dlMhEBESNEVUREMyERABESIi"
"MyIiIREAABEREREREREQAAABERIiIREAABEiMzMiEQARI0VmVDIQERJG///UIQESNd///EIQESRt//90IQESR+//50IREjWO//10IREjWP//lkIREjWf//hT"
"IREjaf/+dTIREkbf/9dCEREkbf/5YyEREkbv/3UyEREkbv/mQhEREkbd11MhEBI0Z4dkMhEBI1fdhkIREBJG//9kIRABJN///UIRABJN//9kIRABI17/1TIR"
"ABEkVlQyEQABEiMzMhEQAAERIiEREAAAARERERAAAAAREiIiIiEREAESIzMzMzMhEBEjRVVVVVQyEREk3/923/9DERI13/+I3/9TIRJG7/6Z7/5jIRJH//2a"
"//1TIRJH//qa//hTIRJH//mJ//dCERJN/+h9/+YyERI97tZt7tQxEREjRUREVUMhEBESIzIjMyIRAAEREREREREQAAAAAAERESIREREREQAAAAABESIzMyIz"
"MiERAAAAAREjRVVERFVDIREAAAERI0b/9lV//1MhEAABESNGj/6Hef/mQhEQARIjRo7/2prv/XUyEQARJFea//qqr/+pZCEQESNd3e//3d///d1TIQESRv//"
"////////9TIQESTf//////////9TIRETRszf/8zN//zMZDEREjRorf/aqt/9qGUyEREjRor/+qqv/6l1QiERI0Z6r/+qqv/6l1MhEBJGiq7/6qrv/qp1MhEB"
"NN///////////lMhEBNe///////////lMRABJO7v//7u///u5kIRABI1iu/6qq7/qYZDIRABI0af/6ma//l1QyEQABEjXf/oeN/+dTIhEAABEjTv92Vu/8Uy"
"IREAAAESNFVURFVUMiERAAAAERIjMyIzMzIREQAAAAARERIhEiIhERAAAAAAAAAAAAAAEREREAAAAAAAAAABERIiEREAAAAAAAARESIzMiERAAAAAAAREiNF"
"VUMhEQAAAAEREjNG7/ZDIREAAAERIjRXnv6GQyEQAAERI0V4qv/qhlMhEAARI0aN7////tZDEQARI0af///////1MhABEkaf////////UyEAETV////v/97/"
"1TEQESNd//+q/+qp1kIRARI23//qr/2odUMhEBEjbf//rf+qdkMhEQESNt/////6qGQyERARI1j/////7KdUIREBESRp//////6XQyEQERI1ed7/////ljIR"
"AREjRoqt/////nQhEBEjRWea7/rv//dCEQESRWeJrv6q//90IRESNe6aqv/q3//3QhERJH//7d/+7//9YyEREk7////////+dTIRESTv///////+hTIRARI1"
"fe/////pdUIREAESNWiq/+qYZTIhEAAREjRXn/2GVDIREAAAERI0Xf92QyIREAAAABESJFZlQyEREAAAAAAREiMzMiERAAAAAAAAEREiIhERAAAAAAAAAAER"
"EREQAAAAAAAAARERERERERERERAAAAEREiIiEREREREREQABESM0RDIhERIiIiERARIjVmZlQyIiM0RDIRARJFfv/9ZTMzRd/+MRASNc////5lRFbf/1MRAT"
"R//qrv+GVXj/9kIRAjXf+qqv/Yd5//hTIRAkbv+pqv/Zif/9ZDIRAkf/6prf/Krv/XVCIRAkb/6qrv+q7/6XVDIREjbv+qr/6u/+qXZTIhEjXP////rf/qqp"
"hlMhESRt///q3/+q7//nUyESNGeaqq//rP///+ZCERI0V4qv/6r/6q7/dTERIjRor//a3/qqr/1TIREjRo7/2q7/qZr/1TIREjV+/+ma/+qZr/1TIRI0bf/o"
"d4/+qq7/dCERI13/6GVn7/ys/+ZCERJM//dkRGj////3QyERI//2VDM0Z///5lMhEBI0RUMiIjRWd2VDIREBEiMyIhESI0REMyERAAEREREREREiIiIREQAA"
"ABEREREREREREREAAAAAAAEREREREREAAAAAAAEREiIiIiERAAAAAAERIjREREMhEQAAAAERI0VniHZTIREAAAARI1fO///nUyEQAAARI1fv////91MRAAAB"
"Ekbv//7v/+YyEQABEjWP/+qq7/9kIRAAESNp//2qrv/3QhEAARJG3//aqv/9ZCEQARIkav//rv/+hkIhEBEjV67////+qGQyEREiRorv///9qodUIRESNXrv"
"///+qqnXUyESNX3//////ar//UIRJG3//6rf//3f/3QhEkf//6qq7////+dCETXP/+qqqu////pkISNd//6qqqr///6WMhE1z//+qqrv///5YyESR///////"
"////5TIRI1j///////7/90MRESRo7////tqd91MhERIkVniZmIdmZVMhEQESI0RVVVVEQzMhEQABESIzMzMyIiIREQAAARERIiIiEREREAAAABEREREQARIj"
"MiEQESNFVDIRESTf/0MREjXf/1MhEkbv/lMhEkf//VMhEkf/91MhEkf/90IREk3/5TIREj3u1CERESNEQyEQERIiIhEAARERERAAAAAAARERERAAAAARESIi"
"ERAAABESIzMyERAAERI0VmUyEQABEjRu//0xEAESNG3//lMhAREkaP//dTEQESNX7//WQhEBEkaf//hTIRESNY7//XQyEREkaf//lkIRASNH7//XUyEQEjWP"
"//lkIhEBJGn//4UyEQASR9//11MhEAI1jv/6dCIRACNY//+WQhEAAjaf//lTIRAAI2n//oUyEQACRs//6FMhEAAkbf/+hTIRAAJG3//XUyEQACRt//51MhEA"
"Ajaf/+dTIRAAI1n//oUyEQACNY//+FMhEAASR+//hTEQAAEjbf/8UxEAABI0b//kIRAAARI0ZmQyEQAAERIzQzIRAAAAERIiIhEQAAAAEREREQAAAAAAAAAB"
"EREREAAAAAERIiEREAAAARIjMzIREAAAESRWVDIRAAARI9//1TIQAAESRv/+ZCEAABEkfv/4UyAAARJH7/+WMgAAESR9//1kIAABEkff/9dCAAARJGr//nQg"
"AAESRq//50MAABEkev/+dDAAASJH3//nQgABEjR9//10IAARI1ff/9dCAAESNY7/+WQgABEjaf//ljIAARJGn//4UyABEjR9//50MgARI1jv/8ZCEBESRp//"
"+FMhARI1ff/+dDIQESRp//+WQhERI0ff/+dTIRESNY//+WQhEBEkbv/9dDIRARJM//91MhEAEST//3VCERABEjVmZTIhEAABEjRDMiEQAAAREiIiERAAAAAR"
"EREREAAAAAAAABEREREAAAAAARERIhEREAAAAREiMzMiERAAARIjNFVUMhEQARI0Rn/9ZDIRARE0Z4r/qHUyEREjXv2v+t/kIRESTf//////UxERNH3////9"
"dTERI1iu///9p0IREjXf/////mQhESNf/+/9//UyERI07a3/quZDEQESNWjv6HZDIRABEjRd7VQzIRAAERIjREMyIREAABERIiIiEREAAAARERERERAAAAAA"
"AAERERERAAAAAAARESIyIREAAAAAERIjRUMiEQAAABESNF7uVCERAAAREiRY//dTIREAERI0V9//hkMhEAESNGed//qGQyEREjV4qv/+qoZCERE07///////"
"9TERE1////////9TERE1////////5TEREkaKrv/6qXVCEREjRorv/qhlQhEAESNGj//XVDIhEAERI1f/+GQyERAAAREkXu5kMhEQAAABEiRVVDIREAAAAAES"
"IzMiERAAAAAAAREREREQAAAAAAAREREQAAABERIRERAAESIzMiERARI0VVQyEQESRu/2QxERI17//2MhESRv//9kIREkb///ZCERJGn//WMhESRs/+dDEREk"
"bv+FMhERJG/9ZDIRESPv9kMhEBEjRVQyERABEiMyIREAABEREREQAAABEREQAAABERESEREQABEiMzMiIRARI0RVVDMhESNGeId1QhESTP////UyESTf////"
"UyESTv////UxESNGd3dlMhERIzRERDIREBEiIiIiERAAEREREREQAAABERERAAABERIRERABEiMzIhEBEjRVVDIRESRu/2QhERNe//9TIRI1///2MhETX///"
"UyERJG//ZCEREjRWVDIRARIjMzIREBERIiEREAABERERAAAAAAAAAAABERERERAAAAAAAAAREiMyIREAAAAAAAERI0VUMhEAAAAAABESNG7u0yEAAAAAABEj"
"Ru//UyEAAAAAAREkaP/+UyEAAAAAARI1fv/3UyEAAAAAESNGz//WQhEAAAAAESNY7/+FMhEAAAABEjR9//1kMhEAAAABEjWP//hTIRAAAAARI0ff/8ZDIRAA"
"AAERJGj//oUyEQAAAAESNX7/+WQiEQAAABESRp//51MhEAAAABEjV+//lkIREAAAARIkaf/+dTIRAAAAARI1jv/4ZCERAAAAESNGz//XQyEQAAAAESNY7/+F"
"MhEAAAABEjR9//10MhEAAAAREjWP//hTIRAAAAARI0ff/8ZDIRAAAAARI1j//oUyEQAAAAARJH7/+WQiEQAAAAASNY//51MhEAAAAAASNe//hTIREAAAAAAS"
"Nf/+ZDIRAAAAAAASPe7mQyEQAAAAAAARI0VEMhEQAAAAAAAREiMiIREAAAAAAAABERERERAAAAAAAAAAAAARERERERERAAAAARESIjMzIiERAAABESIzRVVU"
"MyERAAERI0VniIh2QyERAREjRn3v//7XUyEQESNGjv/////nUyERI0af///////nQhESNo///+3f//+FMhI1fv//yqqv//1kISRp///aiJrv/+dCE0fv//qG"
"Z53//3QyNY///ZZVed//+FMkaf//qGVWnf/+dDJH3//6dVV57//nQiR+//6XVVev//10Ikfv/+l2Vor//5YyJH7//qdmee//+FMiR9//6piJr//9ZCEjbf//"
"qqqv//+FMhI1j///3d///5ZCERJG3///////10MhESNX7//////XUyEQESNX3v///YZTIREBESNGeIiHZUMhEQABESM0VVVEMiERAAABESIjMzIiEREAAAAB"
"ERESIhEREAAAAAERIiIiIiEQARIzRERDMhERI0Z3d3ZDIREk3////9QhETXv////1TESNf////+FMRE17u///4UxESRorf/+hTIRI1eu//10IREjR6///XQh"
"ESNHr//5ZCERI0ff//lTIREjV9//6FMhESNY7//XQyERI1n//9dCEREkaf//lkIRESRt//+FMhERJH3//oUyEREkfv/9dCEQESR///1jIRARJH//91MhEBEj"
"3//2QhEAARJFZlQyEQABEiMzMiEQAAAREiIhEQAAAAAAERERIhEREQAAAAAREiIzMyIhERAAABEiM0VVVEMiEQAAESI1Z4iIdlQyEQABEkV97///6GQyEAES"
"Nd//////+GQhABEk7///////+FMgARJO///d3v//5jIAESNd/Zmar//+ZCAAESRWZnea7//nQgABEiNFVnmu//1kIAAREiNEZ53//4UyAAERIjRXnP//10IQ"
"ABESNFed///oUyEAERI0V57//+hkIhAREjRXrv//2WQyEQESNGjP//+pdUMhERI0aN///qqHVCIRESRo7//+qql2QyERI1j////d3d3VMhESNv////////9j"
"IREkb/////////YyEBI9/////////lMRARNFeIiZmIh2UyEQESI0VVVVVVRDIREAERIjMzMzMyIhEQAAABESIjMzMzIiERAAAREjREVVVVRDIhEAARI1Z3iI"
"iId1QhEAARNe////////UyEAASNf////////ZCEAASNv////////YyEAASNe7u7u///4UyEAARJFeKqt//+nQxEAARI0V5rf//qFMhEAAREjV53//6lkMhEA"
"ABEjV9//+qhkMhEAABEjWP///qlkMhEAAREjXf////11MhEAEREjXf////+WMhEAERIjVoqt///WQhEBEiM0Vniq7//mQhERI0VVZniq7//mQhERJG6IiImq"
"///WMhESNe/+3a3v//91MRASTf////////hkIRASPf///////oZDIRASNG3v///+12QyEQARI0V4iJiHZUMhEQAREjNFVVVUQyIREAABERIjMzMyIhERAAAA"
"ERERIiIhEREAAAAAAAAAABERIiIREAAAAAAAABESIzMyIRAAAAAAABESNFZlQyEAAAAAABESNG3//zIRAAAAABEiNX3//9QhEAAAABESNXn//9UyEQAAABES"
"NXn//9dTIRAAABESNXn//+hkMhEAABESNXn//+l1QyEQABESNXn//+qXZTIREAESNHn//+qqmGUyEQESNGnv//qq//9kIRERJGjv//qqz//oUyERI1jv//qq"
"rf/9l0MREkbv//7d3f///dUyESR///////////9kIRJd///////////2QhEk7//////////+UyESNWeJmaqu//2oZCEREjRFVmeK//+oZDIRERIiM0RWn//4"
"ZDIRABEREiIzVv//ZDIREAABERESI0ZmVDIREAAAAAERESM0QyIREAAAAAAAERESIiEREAAAAAABESIiMzMiIhEQAAAREjNEVVVEQyIRAAARI0Z3iIh3ZUIR"
"AAESNG///////9MhAAESNt///////2MhABEjR+///////1MhABEjWP//7u7u7VMRABEkav/+qqmHZTIRABE0ff/9qph2VDIRABI1j//6qph1QyIRARI1n//+"
"7tqXUyIRARI23//////5ZDIRARI27///////hTIRARI17u7u7///5kIRARI0Z4mqqv//90IRERI0VneIqt//+EIRESNFZ3d4mt//90IRESRu2ZmZqu//50IR"
"ETXf/92t3///hjIREjX////////9dCEQETXv///////XUyEQESRs7////9hlMhEQESNFZ4iYh2VDIREAARIjRFVVVEMiERAAABESIzMzMiIREQAAAAERESIi"
"IRERAAAAAAAAEREREiERERAAAAARESIjMzIiERAAARESI0RVVUQyERABESI0VniIh2UyEQARI0V43v///9UyEBEjRo3//////9MhESJGjv//////9TIREjWO"
"///93N78UyESNX7//9qpmHZTIREkaf//2qmIdlQyESNX7//qqqmHZDIhEjWf//2t7u2XVDIRJGz//+/////XUyESR9/////////WQhEkfv///t3v//91MRJH"
"7//9qqrf//hTISR+//+qmar//9UyEkbf/+qpmq//+VMhI2n//6qaqv//hTERNX///qqs///nQhESRo////////hTIREjRt//////+GQhEREjRo7////YZDIR"
"AREjRWeIiHZUMhEAAREiNEVVVEMiERAAARESIzMzIiEREAAAABERESIREREAAAAAERIiMzMzMzIiERABEiNEVVVVVVRDIRERI0V3iIiIiIdlMhERJG//////"
"////UxESNX//////////YyESNt/////////+YyESRu///u7u///4UyESRu//qqqq///XQhESNv/+mJqu//+WMhESNe7td4rf//x0MhERI1ZmZ4r//+hTIRAR"
"EjRFV57//5ZDIRABESM0aM///XUyEQAAESI1ef//6GQhEQAAESJFjv//lkMhEAAAESNHn//9dTIRAAAAESNY7//5ZCIRAAABETR9//+nQyEQAAABEjWP//6F"
"MhEAAAABEkbv//hkIhEAAAABEk3//9ZDIRAAAAABEk///mQyERAAAAABEjRWZUMhEQAAAAAAESMzMyIREAAAAAAAEREiIhERAAAAAAAAAAAREREiIREREAAA"
"AAERIiMzMyIhEQAAABESM0VVVUMyERAAARIjVniIiHZDIREAESJFfe///+11MhEAESRo///////nUyEBEjWP///////+dCEBEkbv//2qrv//hTEBEkf//9qq"
"qv//1TIRI1j//6qqqv//xTIRI1j//9qqqv//hTIRI1jf//7c3//+dDERJGmv///////pZCESNXrv//////6oYyESRp/////////YUyESR+//6qqq7//5YyEj"
"WP//qqmqr//9YyEjXf//qpiar//9YyEjWP//qqmqr//4UyETV///6qqt///3QxESRu////////+GMhESNX7///////hkMhERI1eO////7YZDIRAREjRXiJmI"
"dlQyEQABESM0VVVVRDIhEQAAEREiMzMzIiEREAAAAREREiIiEREQAAAAAAEREREREREQAAAAAREiIzMyIhERAAABESM0VVVEMiERAAESI1Z4iIdlQyERARIk"
"V97///2GQyEQESRY7//////WQyERI1f///////+GQhESRt///aqu//91MhE1f//9qqqv//ljIRNY//+qmarv/+dCEjWP//qpmq7//nQhE1j//9qqqv//50MR"
"JH///+3e///+dDESRt/////////nQhEjV9/////v//10IRIkV53v/tr//5YyESI0Z4qqqu//6FMhEjRWd4mq3//9ZCERI1Z4iarf//6FMhETRv3Kre///5ZC"
"ERI17///////lkMhESPf//////6GQyEQEjXv////6XVDIREBEjVniIh3ZTIhEQAREjRVVVVDMiERAAAREiMzMzIiEREAAAARERIiIREREAAAAAAAAREREQAA"
"AAEREhEREAABEiMzIhEAARI0VVQyEQARJG7/ZCEQARNe//9TIQASNf//9jIQASNf//9TIQERNG//ZDEQERI1ZmQyEQERIzREMhEQERIzMzIhEBESM0QzIREB"
"EjRWVTIREBEkbv9kMhEBE17//1MhEBI1///2MhEBE1///1MhEBEkb/9kIRABEjRWVDIRAAESIzMyERAAEREiIREQAAABERERAAAAAAAAEREREAAAAAEREhER"
"EAAAESIzMiEQAAESNFVUMhEAARJG7/ZCEQABE17//1MhAAEjX///YyEAASNf//9TIQARE0b/9kMRABESNWZkMhEAERIzREMhEQAREjMzMiEQAREjNEMyERAB"
"EjRWZTIREBESRu/3UyEQESNe//9jIRARJG///2QhEBEkb///ZCEQESRp//1jIRARJGz/50MREBEkbv+FMhEAESRv/WQyEQARI+/2QyEQABEjRVQyERAAARIj"
"MiERAAAAERERERAAAAABEREQAAAAAAAAAAABEREREAAAAAAAERERERERAAAAABERESIiIiEQAAARESIjM0REMhEAEREiM0RWZ95CEQERIjRFZ43v/1MRARI0"
"Vnie///+UxERI0aJ7////+ZCEREkbf////7YZTIREjXv///tqXZDIRASNv//2qqXZTIhEBI2///+qpdkMiEQEjXf///+2XVCERARJFfe////50MhEBEiRWie"
"////UyEAARIjRWeN//9TEQAAERIjRFZ37kIRAAABERIjNEVEMhEAAAAREREiIzIhEAAAAAAREREREREAAAAAAAABEREREAAAAAERIiIiIiIREQABEiMzNEQz"
"MyERARI0VWZmZmVDIRARJN3d3d3d3dMhESNf///////+QhESRv///////9QhESTe7u7u7u7mQhESRXiZqqmYhlMhESRWeImZiHZUMhEjRniZmZmYdUIREkbu"
"7u7u7u7UIREk3////////EIREk7///////9jIREj3d3d3d3d1DIQESNVZmZmZlQyEQESIzNEREQzIhEAARESIiIiIiEREAAAAAEREREQAAAAAAAAEREREREQ"
"AAAAAAESIiIiEREQAAAAASNERDMyIREQAAARJO53ZUMyIREAABE17/7YdlQyIRAAETX////ZhlMiEQARNM7////sdUIRAREkV53////9UyEBESNGearv//9j"
"IQESI0Z5qq3//2QhARI0Z4re///+YyERI0aM7////+dDEREkbv////7YZTIRESTv///th2VDIRARJO/+2HZVQyIREBEk/XdlVDMiEREAESNEREMyIhERAAAB"
"EiIiIiERERAAAAAREREREREAAAAAAAEREREQAAAAAAAAABERESIhEREQAAAREiIzMzIhERAAERIzRVVVQyIREBEiNGeIiIdUMhEBEkV97///6GQyERI13///"
"///+ZCERJN////////xTIRJO//7c3v//5jIRI179mZqu//5kIBEkVmZ4mu//5jIBEiNEVnmv//hTIAESI0Vor//+dCEAERI0V9///nUyEAARI0bf//11MhEA"
"ARI13//5dTIREAARI1//+HUyIRAAERJF3d1lMiEQAAESNGeIdUIhEQAAESNX3YZDIREAAAESRv//ZDIRAAAAESTf//1CERAAAAESTf//ZCEQAAAAESNe/9Uy"
"EQAAAAARJFZUMhEAAAAAARIjMzIREAAAAAABESIhERAAAAAAAAEREREQAAAAAAAAAAAAEREREREREREQAAAAAAAAABEREiIjMzIiIREQAAAAAAARESIzREVV"
"VEMyIREAAAAAAREiNEVnd4h3ZlQyERAAAAAREjNWeO7////tdlMiEQAAAREjRn3////////9dUIREAAREjRo///+3d3e///nUyEQABEjRt///aqpmZqt//51"
"MhEBEjRt//2qqqmZmaqu/+ZCERESRo//yqqq3aqqqqr/+FMhESNX//2qrf///q7/+t/9YyERJG3/6qrv////7//qr/50IRI1j/+qrv//7v///9qu/3QhEjbf"
"/qrf/+qqrf//qq7/dCESR+/9qu//qoiq//+qrv90IRJH//qq//2od5r//6qu/3QhE1j/+qr//ah3mv/+qq/+ZCETWP/6qv/9qHiq//2qr/1jIRNY//qq//6p"
"ma//+qrv91MhE1j/+qr//9qq7//6qv/mQhESR//6qt/////////v/3UyERJG7/6qrv////3////4UyEQEjWP/6qq3//+qq///nVCERARJH7/6qqqqqqZmZh2"
"VCIRABEjWP//qqqqqZiHZVQyIRAAERJGj//+2qqq3tZDMiERAAABEjRo7///////5TIhERAAAAARI0V97//////kMhERAAAAABESI0Z3jd3ddkMhEQAAAAAA"
"ARESM0VWZmVEMhEQAAAAAAAAAREiIzMzMyIhEQAAAAAAAAAAERERIiIiEREQAAAAAAAAAAAAAAEREiIiERAAAAAAAAAAERIzREMiEQAAAAAAAAERI0ZnZUMh"
"EAAAAAAAARI1fv/+ZCEQAAAAAAARI0ff//+FMhEAAAAAARIkaf///9ZCEQAAAAAREjWO////50MhEAAAABEjR6/////4UyEQAAABEiRp//+u//lkIRAAAAES"
"NY7//a7//XQhEAAAESNHr//6rf/+hTIRAAESJGnv/8qq//+FMhEAARI1jf/+qqr//5ZCEQARI0ev//qqqu//10IRAREkae///MzM///oUyEBEjV9////////"
"//hTIRESRp//////////+WMhESNY7//////////9ZCERJG3//6qZmZqu//10IRE0f//9l2ZmZ53//nQhETXv/+hlRERFaf//ZCERPf//dkMyIjNW///DIREj"
"VmZUMiIiIjRWZUIRERIzMzIhERERIjMzIhEBERIiERERERERIiIREAAAERIiMzMzMyIhERAAABESNEVVVVVEQyIREAABEjVniIiIh3ZUMhEAARI13//////u"
"2GUyEQARJG7////////+dDEQESNY//////////1jIQESNp///d3d3v//90IQESRq//+qqqqu//90IQEjR9//6qmZqt//90IREjWO//6qqaqv//1kIREjWf//"
"2qqq3v//ljIREkaf/////////qdTIREkev////////+qdTIRI0ff/////////8dTIRI1jv/+qqqqrv//hTIRI1n//9qpmaqv//hTIRI2n//6qYiJqv//hTER"
"JGr//6qpmarv//dCERJH3//6qqqt7//+ZCERJH7//////////nUyERJG//////////51MhEBI1////////7YdTIREBEkV4iIiIiHdlQyIRABEiNEVVVVVURD"
"IhEQAAERIiMzMzMzIiEREAAAAAAAAREREiIhEREQAAAAARESIjMzMiIREQAAABESIzRVVVRDIhEQAAESI0VniIiHZUMhEAARIjVo3v///+xlMhEBEiRX3///"
"////5kIRERJFjv////////9TEREjV+////3d7///UxESNH3//+qYiInv5kIREjaf//6oZlVWbVQyERJH7//6hlQzNEQzIRAjWP//2WQzIiIiIREQI2n//6dU"
"MiIhERERACRt//+nUyIhEREREAAkbf/+p0MiIhERERAAJG3//6dUMiIiIhEQACRt//+oZUMzMzMhEAAjWf//2oZVVWZUMhEAE1f///2ph4if9kIRABJG3///"
"/d3e//9TIQASNH3/////////UyEAESNX3///////5kIRABESNWjf////7WUyEQABESNFZ4mZiHVDIRAAABESI0VVZVVDIhEQAAABERIjMzMzIhERAAAAAAER"
"ESIiIREREAAAAAAREiIzMzMyIiEREAAAAREjRFVVVURDMiERAAABEjVniIiId2VUMhEQABEjXf/////u2XZDIREAESRu////////6XUyEQESNY//////////"
"11MhARI2n//+7u7v///9ZCEBEkav//qpmZrf//+FMgESR9//6odmeK3//9YyESNY7//pZURWiv//50IRI1j//9hUM0V57//nQhEjaf//p1MzNGnv/+dCESRq"
"//+WQzM0ae//50ISNH3//pZDM0V6///WQhI1jv/+lkQ0Ror//5YyEjWP//2WVEVnrv//hTISNp//+pdmZ4rv//1kIRJGr//6qZiZrv//51MhEkff//3d3e//"
"//6FQhESR+//////////6GQiERJG/////////+11QyEQEjX///////7ZdlMiEQARJFeIiIiIh2ZUMiERABEiNFVVVVVUQzIhEQAAAREiIzMzMzIiIREQAAAA"
"ABESIjMzMzMiIREAABESNEVVVVVEQyIRAAESNWeIiIiHdlQyEAESNd/////////kIQARJG7////////9QhARI1j/////////xCEBEjaf//7u7u7u5TIQESRq"
"//+qqZiHZUIRASNH3//qqYh2ZUMhERI1jv/+qqmHdUMhEREjWf//6qqqmGUyEQESRp////////9TIRARJHr////////lMhEBI0ff///////9UxEAEjWO//6q"
"qqmHVCEQASNZ///aqYh2VDIRABI2n//6qYd2ZUMhEAEkav//qqmYh2UyEQASR9///d3d3d3UIRABJH7/////////UxEAEkb/////////5TEQASNf////////"
"/UIRABEkV4iIiIiHdkMhEAESI0RVVVVVRDMhEAABESIjMzMzMiIREQAAAAAREiIzMzMzIiERAAAREjRFVVVVREMiEQABEjVniIiIh3ZUMhABEjXf////////"
"5CEAESRu/////////UIQESNY/////////8QhARI2n//+7u7u7uUyEBEkav//qqmId2VCEQEjR9//6ph2ZVQyIRESNY7//qmHZlQzIRERI1n//9qph3ZTIhEB"
"Ekaf//7d3d3cQhEQESR6////////5DEQASNH3////////UIRABI1jv///////9QhEAEjWP//2qqYh2QyEQASNp//+ph2VUQyEQABI2n//6dUQzIiERAAEkbf"
"/+hkMiIRERAAASRu//51MiEREQAAABI2///WQiERAAAAAAEjX//2QyERAAAAAAARI1ZlQyERAAAAAAABESMzMiERAAAAAAAAAREiIhERAAAAAAAAAAAAAAER"
"ERIiIREREAAAAAARESIjMzMyIhEQAAAAERIjNFVVVUMyIRAAABESNFZ4iIiHZTIhEAARIjVozv///+11QhEAESJFff///////9UyERESNY7/////////1CER"
"EjV+////7d7///1CERI0ff//+piIiZ/9UyERI2n//+qHZVZndlQyERJH7//6hlRERFVUMhECNY///ZZUMzRFVEMhECNp//+nVDMzRWZUMhECRt//+nUzM0Xe"
"7lQhECRt//6nVDM0bv/2QhECRt//+nVDNFj//3QhECRt//+oZURXn//nQhECNZ///ahmZ4r//nQhEBNX///9qYia7//WQhEBJGz////d3e//+FMhEBI0ff//"
"//////90MREBEjV9////////hTIRABESNWjf////7HVCERAAERI0VniZmIdlMiEQAAAREiNFVWVVQzIREAAAABERIjMzMzIiERAAAAAAARERIiIiEREQAAAA"
"AAARESIhERERERIiEREAAREjMzMhERESMzMyEQABEjRWVDIRESNFZUMhABEjXf/+QyIiNd//5CEAESRu//5jIiNG7//VMgESNX///WQzM1j//9UyARI1n//5"
"dEM0af//hTIBEkav//p1REWM//+FMgEjR9//6odmZ53//oUyESNY7//qqYiZrv/9dDIRI1n//+3d3d3f//10IREkaf//////////+mQhESR6///////////5"
"YyESNH3//////////+hTIRI1jv/+qqqqqv//2FMhEjWP//2piIiq///XQhESNp//+oZlZ4r//5ZCERI2n//5dURFjP//lTIREkbf/+hkM0V9//6FMhESRu//"
"51MzNH7//XQhEBI2///WQiI0b//9UyEQEjX//2QyIiNf//ZCERARI1ZlQyERIkVmVDIRABESMzMiERESIzMyIRAAAREiIhERERESIiEREAAAABERIiEREAAR"
"EjMzMhEAARI0VlQyEAESNN//5CEAESRu//5TIBEjV///1TIBEjWP//lTIBEkaf//hTIBEkff/+hTIRI1jv/+dTIRI1j//9dCERI2n//5ZCERJGr//5YyESNH"
"3//oUyESNY7//nUyESNY///XQhESNp//+WQhESNp//+FMhESRt//6FMhESRu//50IRASNv//1TIRASNf//ZDERARI1ZlQyEQAREjMzIhEQABESIiEREAAAAA"
"ABESIjMzMiIREAAAESI0RVVVRDIRAAABEkVniIiHZTIQAAESNf//////5CEAABEk3//////9UyAAARJN///////WMgAAESPe7u7v//lTIAAAETRXiar//4Uy"
"AAABEjRWet//6FMgAAABEiNWnv/9dDIAAAAREjRp///XQhAAAAERI0av//lkIQAAARESNHr//5UyEAABERIjWN//6FMhABERIiNGjv/9dCIQERIjM0V5///W"
"QhEBEjRVVniv//lTIRESNW+Iia7//nUyEREkb//a3v//1kIRARJP///////nUyEQESTf//////hTIRABEjV+////11QiEQABEjVniIh2UyIRAAAREjRFVVRD"
"IREAAAAREiMzMyIhEQAAAAAREREiERERAAAAAAARESIhEREREiIhERAAABESMzMyIiIiMzMyEQAAARI0VmUzIjNFZlQyEQABEjXf/+VDNFff//4hEAARJG7/"
"/WVFaN///kIRABEjV///12Z47//9ZCEQARI1n//6iInv//1kMhEAESRq//+qqu//+XUyEQABI0ff/+qq///5dTIREAESNY7//ar//+l1MhEQABEjWf//3f//"
"6nVCIRAAARJGr//+///6lkMhEQAAESR6//////6oZDIRAAABI0ff//////2WQyEQAAASNY7///////p1MhEAAAEjWP///6r//+hTIRAAABI2n///qq3//8dD"
"IRAAASNp//+qma///4UyEQAAEkbf/+p2eK///nQhEAABJG7//YZVaO//+FMhAAASNv//1kREaP//9TIQAAEjX//2UzM0Z///0yEAABEjVmVDIiI0VmZUIRAA"
"AREjMzMhERIjNDMiEQAAAREiIhEREREiIiERAAAAAAARESIhERAAAAAAAREjMzMhEAAAAAABEjRWVDIRAAAAABEjTf/+QhEAAAAAESRu//5TIQAAAAESNX//"
"/VMhEAAAARI1j//5UyEQAAABEkaf//hTIRAAAAESR9//6FMhEAAAESNY7//nUyEAAAARI1j//9dCIQAAABEjaf//pkIREAAAESRq//+WQiEQAAASNH3//pZC"
"IREAABI1jv/+hkMiERAAEjWP//2WRDIhERASNp//+pdlRDMhEBJGr//6qYd2VDIREkff//3d3d3dQxESR+////////9TIRJG/////////1MhEjX////////+"
"QhERJFeIiIiId2UyEREiNFVVVVVEMyEQAREiIzMzMzIiERAAABERIiEREAAAERESIhEQAAERIzMzIREAAREiMzMyEQABEjRmZDIRERESNFZlQyEAESNd//dT"
"IRERI0Z//+QhABEkbv/+dDIREjRo///VMQESNY///5ZCIiNGjv//xTIBEjaf///XUzI0aN///5UyARJGr///+WQzRYz///+FMgETR9////p1RFev///+hTIR"
"I1jv///+l1Z67////XQyESNZ/////6h4nv////x0IREjaf//3//amt//7//6ZCERJGr//q//+qr//q//+WMhETR9//2u//qv//qv/+hTIRI1jv/8qv/+7/+q"
"3//XQyESNY//+qrv///9qu//x0IREjWf//qa3///6qr//5ZCERI2n//piK///qma//+FMhESRt//2GaN//mHiv/+dTIREkbv/8dVZ//HZn3//WQhEBI2//+F"
"REVmVUVu//xTIRASNf//ZDIzMzM0X//2QhEQESNWZUMiIiIiI0VmVDIRABESMzMiERERERIjMzIhEAABERIiERERERERESIhEQAAAAARESIiERERERIiEREA"
"AREjMzMiERESMzMyEQABEjVmZUMiEiNFZUMhABEjXf/9ZDIiNd//5CEAESRu//+GQzNG7//VMgESNY///9hUNFj//9UyARI2n///+nVFaf//hTIBEkav///+"
"l2Z6//+FMgETR9/////Zd43//oUyESNY7/////qYrv/9dDIRI1n//+///aqv//10IREjaf//rv//qq//+WQhESRq//+q///qr//5YyESNH3//qrf//zf/+hT"
"IRI1jv/9qq///+//2FMhEjWP//2YnP/////XQhESNp//+oeK7////6ZCERI2n//5dmiv////ljIREkbf/+hlVo3///6FMhESRu//11RFaf///XQhEBI2///W"
"QzNX3//9YyEQEjX//2QyIjV+//ZDERARI1ZlQyIiI0ZmZDIRABESMzMiERESM0QzIRAAAREiIhERERESIiEREAAAAAABERESIiERERAAAAAAARESIjMzMyIh"
"ERAAAAAREiNEVVVVQzIhEQAAARIjRWeIiIh2VCIREAARIjVo3v///+12QyEQARIkV9///////+hkMhEREkWO/////////4ZCEREjV+///+3d7///+FMhEjR9"
"///qmIma7//9ZCESNp///qh2Zniv//91MRJH7//6hlREV53//4UyI1j//9lkMzNGiv//lTIjaf//p1QyM0WK//+VMiRt//+nUzIjRYr//4UyJH3//qdTMjNG"
"jf//hTIkff//p1QzNFee//50ISRt//+oZURFaK///WQhI2n//9qGZWeK///oUyETWP///amIic///5ZCERJG3///7d3e///9dTIREjV9/////////9dTIRAR"
"I1ff///////pdTIREBESNXjv////7XZDIREAAREjRXiJmYh2VDIREAAAERIjRVVlVUMyIREAAAABERIjMzMzIiEREAAAAAAREREiIiIRERAAAAAAABESIjMz"
"MyIiEREAAAERI0RVVVVEMyIREAABEjVniIiId2VDIREAESNd/////+7YZDIRABEkbv///////9ZDIQESNY/////////9ZCEBEjaf///u7v///4UyARJGr//6"
"qqqt///WMgEjR9//6piIqv//1jIRI1jv/+qHd5rv/9ZCESNZ///ah3ea///WMhEkaf//qpiJrf//hTIRJHr//6qqqt///nQhEjR9///u7v////hTIRI1jv//"
"//////+WQhESNY/////////oZDIREjaf///u7u3YdUMhEBJGn//6qYh3ZUMiERASRt//+oZlRDMiEREAEkbv/+hkQzIiEREAABI2///WQyIhEREQAAASNf//"
"dTIhEREAAAAAESNWZUMhERAAAAAAABESMzMyERAAAAAAAAABESIiEREAAAAAAAAAAAAAAREREiIhEREQAAAAAAEREiIzMzMiIREQAAAAERIjRFVVVUMyIREA"
"AAESI0VniIiIdlQiERAAESI1aN7////tdkMhEAESJFff///////oZDIRERJFjv////////+GQhERI1fv///t3e////hTIRI0ff//6piJmu///WQhEjaf//6o"
"dmZ4r///dTESR+//+oZURFed//+FMiNY///ZZDMzRor//5UyI2n//6dUMjNFiv//lTIkbf//p1MyI0WK//+FMiR9//6nUzIzRo3//4UyJH3//6dUMzRXnv/+"
"dCEkbf//qGVERWiv//1kISNp///ahmZnmv//6FMhE1j///2pmZrP//+WQhESRt///+3d3v///XUyERI1ff/////////YZDIRESNX3///////6qdTIREREjV5"
"7////+2ql1MhEAERI0Z53///2qreZCEQABESNFaN///////UIRAAAREiNGjf/////+QhEAAAEREjRXnv///9UyEAAAAAERIjRniJiHVCEQAAAAABESIzRVVV"
"QyERAAAAAAABESIjMzMiERAAAAAAAAAREREiIRERAAAAABESIjMzMzIiEREAAAERI0RVVVVEMyIREAABEjVniIiId2VDIREAESNd/////+7YZDIRABEkbv//"
"/////9dDIQESNY/////////9ZCEBEjaf///u7v///4UyARJGr//6qqqt///WMgEjR9//+piIqv//1kIRI1jv/+qHd5rv/9ZCESNZ///ah3ia///WMhEkaf//"
"qpiJrP//hTIRJHr//6qqqt///nQxEjR9///u7v////lkIRI1jv////////+XQyESNY/////////qdTIREjaf///u7//9qGQyERJGn//6qq3//6hTIRASRt//"
"+pia///YUyEQEkbv/+l2eN//+FMhEBI2///XVFaO//5TIRASNf//dUM0Z///QxEAESNWZlMyI0VmZTIRABESMzMyIiIjMzMhEAABESIiERERESIiERAAAAAA"
"AREREiIhEREAAAAAEREiIzMzIiERAAAAERIjNFVVVEMiEQAAERI0VniIiHZUMhEAARI0Z97////tZCEQARI0aP///////1MhABEkaP////////UyEAETV///"
"/tqt7/1TEQESNd//+qqYiI1UIRARI23//qqYdmVUMhEBEjbf//qph2VEMhEQESNd///9qphlQyERARI1f/////7IZTIREBESRo//////6WQyEQERI1aN7///"
"//hTIRAREjRWia3v///mQhEBEjNFZniqrv//dCEQESRWZmeJqq//90IRESNe6IiJmq3//nQhERJG//7aqs7//9YyEREk7////////+dTIRESTv///////+dT"
"IRARI1fe/////odTIREAESNGeImZh3VDIREAAREjNFVWVVQyIREAAAERIiMzMzMiEREAAAABEREiIiIREREAAAABESIiMzMzMzIiERABEjNEVVVVVVRDMhER"
"I0Z3iIiIiIdmQyERJN//////////5CERNe//////////1CERNf//////////1CERNO7u7v//7u7uUyERI1aJrf//qodlQhEBEjRorf/+p2VDIREBEiNXrv/9"
"lkMiERAAESNGn//8dTIhEQAAESNHr//6ZDIREAAAESNH3//5ZCERAAAAESNX3//oUyEQAAAAESNY7//XQyEQAAAAESNZ///WQhEAAAAAESNp//+WQhEAAAAA"
"ESRt//+FMhEAAAAAESR9//51MhEAAAAAESR+//1kIRAAAAAAESRv//1TIRAAAAAAESNf//ZCERAAAAAAARJFZlQyEQAAAAAAAREjMzIhEAAAAAAAABESIiER"
"EAAAAAAAARESIhERERERIhERAAERIzMyIRERIjMzIREBESRWZUMhESNFZlMhEBEjXv/9MiEiRv//UyERI0f//3QyIjWP//YyERI2j//3UyI0bf/+ZCERJG3/"
"/3UyI1fv/9ZCESNH7//nQyM1j//9ZCESNY///XQzNGn//5YyESNp//+WQzNGr//4UyESRq//+WQzNX3//oUyESR9//6GQzRY7//XQyEjV+//6FQzRp///WQh"
"EjWP//2FREV6//+WMhEjWP//qGREaM//+FMhEjWP//qWVVee//50MhAjWP//2odnmv//lkIRASR///+qmarv/+hTIRASRt///93e///5ZCEQASNY////////"
"/XQyEQARJGj///////11MhEAARI0aN/////YZTIREAABEjRWeJmYdlQyERAAAAESI0RVZVRDIhEQAAAAAREiMzMzMiEREAAAAAABEREiIiEREQAAAAARESIh"
"ERAAARESIhERARIzMzIRERERIjMzIRESNFZlMhERESNFZUMhESTv/+UyERIjVu//0xETXf//dDIiI1fv/+UxEjXP//hTIiNGn//3UxEjWP//xkMzRo///WQh"
"EjWP//11Q0V9//+FMhEjR+//6GVFef//lkIRESR9//+nZXnv/+dTIRASRp//+od43//5ZCIRASNZ///Zia///XQyEQASNY7//qqu//6FMhEQASNH3//6rf//"
"lkMhEAARJGr//6r//9dTIRAAARI2n//97//oZCERAAARI1jv////+nQyEQAAABI0ff////6FMhEAAAABEkav////lkIhEAAAABEjWf///8dDIRAAAAABETR/"
"///oUyEQAAAAAAESNe//91QhEQAAAAAAERJFZ3ZUIhEAAAAAAAARIjREQyERAAAAAAAAABESIiIhEQAAAAAAAAEREiERERERIiIhERERESIREQESIzMyIRES"
"IzQzIhERIjMzIhERJFZlQyIiNFZmVDIiI0VVVCERI1//9kMiNG3//lQyI0Xf/+MhEkb//3UzNGj///dTM0WP//YyESR///lkRFff//+WQ0V+//5jIRJH///X"
"VFef///6dUV5//+FMhE0fv/9hlaN////yGVo7//XQhESR+//6XaK/////Zdor//4YyERJH3//qiJ7//v/9mInv/9dDIREkfP//qZr//6//6pmv//hjIRASRq"
"//+qrv/9r//qqu//11MhEBI2n//6qv//qv//qt//+GQhEAEjWf//qu//yq7/+q///XQyEQASNY7//a//6qrv/63//4YyEQABI1jv/97/+pmt//3//9dDIRAA"
"EjV9/////ph53////+hjIRAAARJH3////6hmiv////p0MhEAABEkbP///+llV6////6FMhEAAAESNp////l1RGn///+mQiEQAAARE1f///11Q0V////nUyEQ"
"AAAAESRv//91QyNG7//3UyERAAAAARI0V3dlMyIjRWd2VCERAAAAAAESI0RDMiERIjREQyERAAAAAAABESIiIhERERIiIiERAAAAAAAAAAAREiIhERERIiIR"
"EQAAAAESIzMyIiIiMzMyEQAAAAEjRWZUMyM0VmVDIRAAABEk7//mVDRWf//9MRAAABEk3//5dVV4///kMRAAABEkbv/+l3if//5kIRAAABEjbP//qprv//hT"
"IRAAABEjWO//6q7//4ZCEQAAAAEiR6//+u//+WQyEQAAAAESRp7/////p1MhEQAAAAESNYr////6hUIhEAAAAAESNXrv///ZZDIRAAAAABEiNYrv//+oZCIR"
"AAAAABEjRp3///+oUyIRAAAAARIkWN/////ZZCIRAAAAERI1fP/////6dDIRAAAAESNXn//+r//+hTIRAAABEjRp///qrf//lkIRAAABEkaP//+pmu//50IR"
"AAABE0f///mHeK//+FMhAAABI17//5dVVo7//lMhAAABI+///GVDNGf//9MRAAABEkVmZUMyI0VmZTIRAAABEiM0MyIiIiM0MyERAAAAERIiIhERERIiIhEQ"
"AAABERIhEREREREiEREAESIzMyIRERIjMzIhEBEkVmVDIiIjRVVUIRASPf/+ZDIjNG3//jIQEkb//4ZDNFbf//UyEBJG7//XVEV5//91MhASNt//+XZnn//9"
"ZCEQEjWP//yHie//51MhEBI0ff/+qq3//oZCERARJGn//6rf//lkMhEAESNY7//a///HUyEQABEjR9/////9hTIREAABEkaf////6GQiEQAAARI1jf///5ZD"
"IRAAAAESNXr///p1MhEQAAABEjR6///pZCIRAAAAABEkev//p1MhEAAAAAARJHz//5ZCERAAAAAAESR9//6FMhEAAAAAABEkfv/9dCIRAAAAAAARJG///VMh"
"EAAAAAAAESNf//ZCERAAAAAAAAESNWZUMhEAAAAAAAABESMzMiEQAAAAAAAAABESIiEREAAAAAAAAAABESIjMzMzMyIhEQAAARIzRFVVVVVUQyIRAAESNFZ4"
"iIiIiHdUIRAAESTf/////////8MhAAESXv/////////2QhAAETX//////////2QhAAESTu7u7u7v///FMhAAESNGeJqqrv//l0IRAAERI0Vnmq///6dTIRAA"
"ARIjRXmv//+nUyERAAARIjRor///p1QiEQAAERI0V6///6dUMhEAAAESJFev//+nVDIREAABEiNXr///qGQyERAAAREjV6///6hkMiERAAARI1ev//+pdUMy"
"ERAAESNXr///qpdlQzIRAAESRp///6qqmHZUIRAAEjWP///t3d3d3dMhEAEjb//////////9QhEAEkb//////////9QhEAEjz//////////1MhEAESRXiImZ"
"iIiHZUIRAAESI0VVVVVVVUQyIRAAAREiMzMzMzMyIhEQAAAAAAAREREREREAAAABESIiIiIREAAAERI0RERDIRAAABEjVnd3ZTIRAAESNH////9CEQABEkbf"
"///+UxEAARJH7////UIRABEjWP//2oZCEQARI2n//6hkMhAAESRq//+WQyEQABI0ff/+hTIREAESNY7//XUyEQABEjWP//pkIhEAARI2n//5YyEQAAESRq//"
"+FMhEAABI0ff/+hTIRAAESNY7//XQiEAABEjWP//lkIRAAARI2n//5YyEQAAESRq//+FMhEAABI0ff/+hTIRAAASNY7//XQyEQAAEjWP//p0MhEAABI2n//6"
"dDIRAAASRq//+nUyEQAAEkff/+qGQhEAABJH7////UIRAAASRv////1CEQAAEjX////2QhEAABE0Z4iHZDIQAAARI0RVVEMhEAAAARIjMzMiEQAAAAARESIh"
"ERAAAAAAEREREQAAABEiIiIRAAARI0VEMhEAARJO7tQhEQASNf/+UyEQASNu//dCEQASNt//hTIRASNt//hTIRASNp//lkIRASNY//10IRASNY//50MhASNX"
"7/51MhESNH7/+FMhERJG3/+FMhERJGn/+WQhERI2n//WQhERI1j//XQhERI1jv/nQyEBI0fv/4UyEBEkff/4UyEBEkbf/5YyEBEjaf/9ZCEBEjWP/9dCEBEj"
"WP/+dCEBETR+/+dCEAESRt//dCEAESNd//dCEAESNH//ZCEAARI17uUyEAAREjRUMhEAABESIiIREAAAERERERAAAAABEREREREAAAAAERIiIiIREAAAARIj"
"RERDIhEAAAETRXeHdUIRAAARI1////9TIQAAESR/////ZCEAABEk3////nQhAAARJGmu//10IQAAESNXrv/9ZCEAABEjR6//+WQhAAARI0ev//lTIQAAESNH"
"3//oUyEAABEjWO//10MhAAARI1jv/9dCEQAAESNp//+WQhEAABEkaf//hTIRAAARJH3//oUyEQABEjV+//10MhAAARI1jv/9dCEQAAESNp//+WQhEAAREkaf"
"//hTIRAAESNH3//oUyEQAREjWO//10MhAAESNGnv/9ZCEQAREkV6//+WQhEAESNXmv//hTIRABEk3////nQyEQARNe////1kIRAAEST////9UyEQABEjV4iI"
"dTIRAAAREjRVVUMhEQAAAREiMzMiERAAAAARERIhEREAAAAAAAAAERERERAAAAAAAREiIiERAAAAABEiM0MyERAAAAESJFZmUyEQAAAREkbf/9UyEQABESNY"
"///mQhEAARI1fv//91MhABESRp//7/hTIQARI1jv/d/9ZCEBEjRt/+qv/nQhARI1j/+qr/50IQESR+/9mJ//hTERI13/+HeO/4UxESNf/4ZVbf/VMQEj7/5l"
"RFb/5CEBE0VVQzM0VUMhARIjMyIiIzMyEQAREiIREREiEREAAREREREREREAAREiIiIiIiIREAESIzREREREMiEQESRWZnd3ZmVDIQEj3////////zIREj7/"
"///////jIREj3d3d3d3d1DIQESNFVVVVVVQyEQERIjMzMzMzIhEQARERIiIiIhEREAAAEREREQAAAREiIhERAAESMzMiEQARI0VlQyEQESP//2QhEBEjX//l"
"MhABI0b//TIRARI0Xd0yEAERIzRDIRAAEREiIhEQAAERERERAAAAEREiIiIiIiIREAAAERIjNEREMzMyIRAAERI0Vmd2ZmZlQyEQERI1Z83u2Znd3UMRERI1"
"ff////2v//UyERI1fv////////5kIREkbf///u/////WQhEjWP//6qqt///6ZCESR+//+piJrv//ljIRNX///ah3it//+VMhE1j//6l2aK7//oUyEjWP//qX"
"Z4r//9dDIRNY//+qh4mv//pkIREkf//+qpqu//+WMhESRu///tre///4UyERI1j////////+dCEQESRo////////1kIRARI0aO///q7/90MhEAESNFZ4iId3"
"dlMhEAAREiNEVVVURDMhEQAAEREiIzMzMiIhEQAAAAERERIiIhEREQAAAAABEREREAAAAAAAAAARESIhERAAAAAAAAERIzMzIREAAAAAABESNWZUMhEQAAAA"
"ABEjXf/9QyERAAAAABEkbv/9VCIREQAAARI1j//5ZTMiEREAARI2n//6hlRDIhEQARJH3//qmIdlQyERASNH3//a3e3YZDIRESNY7//v////xkMhESNp////"
"/////WQhESRq////7v///4UyESR9///qqq3//9YyEjV9//+qmJrv/+dCEjWO//2od4rf/+dCEjaf//qXZnrf/+dCEkav//qXZ4rv/+dCEkff//qoeJr//9Yy"
"Ekff//6qmq7//4UyEkfv///tre///WQhEkf/////////51MhEkf//t/////+dTIREj3//a3v//6HUyEREjRWd3d4iHdUMhEQESI0RERVVVQzIREAAREiIiMz"
"MzIhERAAABERERIiIREREQAAAAARERIiIiIREQAAABESIzNEQzIhEQAAERIzRWZ2ZUMhEQAREjRnje7thlMhEAESNWjv////51MhERI1ff//////9kIRESRt"
"///+7///5CERI1j//+qpnf/mMhESRt//+pd3fXZDIREkf//9l2VVVUMhEBNY//+oZUREMyIRATWP//qGVVREMhEQE1j//6l2ZmZUIhEBJH///qmIjedTIRAS"
"Rt///tre//5CEQEjV////////9MhEBEkWO//////1TIQARIkV97///11QhEAARIjVniIh2UyERAAAREjNFVVQzIREAAAAREiIzMyIhEQAAAAARERERERERAA"
"AAAAAAAAAAAAEREREQAAAAAAAAAAAREiIhEQAAAAAAAAAREiMzMhEQAAAAAAARESNFZlQhEAAAAAERERI0b//1MhAAAAERERIjRY//9kIQAAEREiIjNFff/+"
"ZCEAAREiM0RFVo7//WQhABESNFZnd3iu//xkIQERI0Z53u2qr//5YyEBEjV9/////q//+VMhESNX3////////+hTIREkbf///u/////XQyESNY///qqq3///"
"10IREkbf//qYia7//6ZCERJH7//ah3it//+WMhETWP//qXZorf/+hTIRE1j//6l2eK///XQyEBNY//+qh4mv//10IRASR///6qmq7//5ZCEQEkbf//7a3v//"
"+FMhEBI1j////////+dDEQARJGj////////WQhEAESNGjf//6t//1TIRAAESNFZ4iId3dlMhEAABESI0RVVVREMyERAAABERIiMzMzIiIREAAAAAERERIiIh"
"EREQAAAAABEREiIiIhERAAAAERIjM0RDMiERAAAREjRFZnZlQyERABESNGeN7t2GUyERERI1ef/////XUyEREjV9///////nQyERJG3//+3e///WMhEjWP//"
"2qqq//90IRJH7//qqqqu//hSETWP//7d3d3//4UxI1j/////////90ISNY//////////ZCETWP//qqqqqphkMhEkf//9qqqqmGUyERJG3//9qq3vhkIhESNY"
"///////+UyEQESRo7//////lMhEBEiRX3v///nVCEQABEiNWeIiHZUIREAABESM0VVVEMhEQAAABESIjMzIiERAAAAABEREREREREAAAAAAAARERERERAAAA"
"ARESIiIiERAAABESI0REQyERAAERI0Vnd2VCEQABEjRo7///UyEAESNGj////1MhARIkaP////5TIQESNX3//6qGQxERI1ee//qpdTIRESRpr//6qnUyERI1"
"3//////VMhESNu//////dDEQEjb//////2QhEBI1iv//ypdTIRASNXr//6hkMhEAEjR6//+nQyEQABI1ff/+hTIhEAASNY7//XUyEQAAEjWP//pkIhEAABI1"
"j//5YyEQAAASNp//+FMhEAAAEjbf/+dCEQAAABI17//WMhEAAAARNP//ZDIRAAAAESNFZUMhEAAAAAESMzMiERAAAAABERIiEREAAAAAAAABERIiIiIiIiER"
"EAAAERIjNEREMzMyIREAAREjRVZnZlVVVDIRABESNWed7tmIzd1TIQERI1ff////6u//ZCEBEjV+//////7//3QhARJG3///7v////50IREjWP//6qqs///9"
"dCERJG3//qmImt///WQhESR///2od4rP//lkIRElj//6mHeK3//5YyERNY//+qiImv//6FMhESR///2qmq3//9dTIREkfv//6qrv///XQhERI2j/////////"
"lkIRESNX3////////5UyEREjRorv///d//6FMhARI0Z5qqqqrv/9dCEQESNXiaqqqs//+FMhEBEkbv2qqq3//9dCEQARJN////////6FMhEAEST////////o"
"VCERABEjXe/////tdUIhEAABEkVnjN2YdlMiEQAAARIjRVZmVUMyERAAAAAREiMzMzMiIREAAAAAARERIiIiEREQAAAAAAABEREREAAAAAAAAAARESIhERAA"
"AAAAAAERIzMzIREAAAAAABESNWZUMhEQAAAAABEjXf/9QyERAAAAABEkbv/9VCIREQAAARI1j//5ZTMiEREAARI2n//6hlRDIhEQARJH3//qmIdlQyEQASNH"
"3//are7YZDIRESNY7//v////1kIRESNp/////////WMhESRq/////////nQhESR9///6qq7//4UhEjV9//+qmar//4UxEjWO//2oeJr//4UyEjWf//qHZ5z/"
"/oUyEkaf//p2Vo3//nQhEkbf/+llVp7//XQhEkff/9hUVp//+WMhEkfv/9dURp//+FMhEkf//4ZDRs//50MREkf//3UzRd//5kIREj3//lQyNO//1DIRETRW"
"ZUMiI0VlQyEQESIzMyIRIjMzMhEQAREiIhERERIiEREAAAABEREREQAAABERIiEREAABESMzMyEQAAESNFZUMhEAARNM//1CEQARI1///1MRABEjb///UyEA"
"ESNu//1TIQERJFeNhkIRARI0Z4h1MhEBEjXN3XUyEQESRu//1TIRESNY//+FMhERI2n//4UyEREkbf/+dDEQEjR9//10IRASNY7//WQhEBI1j//5ZCEQEkaf"
"//hTIRASRt//6FMhEBJH3//XQhEAEkfv/9ZCEQASR///hTIRABJH//90IREAEj3//lMhEAARNFZlMhEQABEiMzMhEQAAAREiIREQAAAAAAAAABEREREQAAAA"
"AAARESIhERAAAAAAERIzMzIRAAAAAAESNFZUMhEAAAAAETTP/9QhEAAAABEjX///UxEAAAABEjb///UyEAAAABEjbv/9UyEAAAAREjV42GQhEAAAARI0Z4h1"
"MhEAAAARI1fd3VMhEAAAARJG7//VMhEAAAESNH7//FMhEAAAESNY//+FMhEAAAESNp//+FMhEAAAESR9//51MhAAAAEjR+//10IRAAABEjWO//xkIRAAABEj"
"af//lkIRAAABEkaf//hTIRAAARI0ff/+hTIRAAERI1jv/9dDIQAAESI1jv/8ZCEQAAESNGn//5YyEQAAEjRXr//4UyEQAAI0V53//nQxEAAANGea///WQhEA"
"AARe////91MhEAAARv////hkIRAAAATf///oZDIRAAAANWfdd1QyEQAAAAI0VVVDIhEQAAAAIiMzMiEREAAAAAEREREREQAAAAAAAAEREREQAAAAAAAAAAER"
"EiIREQAAAAAAAAERIzMzIREAAAAAAAERI1ZlQyERAAAAAAARI13//UIhERAAAAABEkbv/9UyEREREAABEjWP//hUMiIhERAAESNp//+WQzMzIiEQARJG3//p"
"ZVVVVDIRABI0ff/9l2d93d0yEBEjWO//2oie///jIRESNp//+qqu///VMhERJGr//6qv//+GQhEBEkff/+rf//+XQyEQEjWN//7v//6nUyERASNY7/////6o"
"ZDIRABI2n//////qhUIhEAEkav//////2GQiEQASR9///+///5ZDIRABJH3//9qv//11MhEAEkfv/+qq7//5YyEQASR///qYiv//5kIRABJH//+XZo3//9Qh"
"EAEj3//mVEV+//9CEQASNFZlQzNFZmZDIRABEiMzMyIiM0QzIRAAAREiIhEREiIiEREAAAAAARERERAAAAEREiIREAABESMzMiEQAREjVmVDIQARI13//UIQ"
"ARJG7//UMQESNY//+FMQESNp//+FMQESRt//51MQEjR9//10IREjWO//1kIREjWP//lkIREkaf//hTIREkff/+hTIRI0ff/9dDIRI1jv/9dCERI1j//5ZCER"
"JGn//4UyERJG3//oUyERJH3//XQyEBJH7//WQhEBJH//+FMhEBJH//90MREBI9//5TIRABE0VmUyERABEiMzMhEQAAERIiEREAAAAAEREiIiIiIiIiIiIiER"
"EAABESIzNERERDMzM0RDMiEQABEjRWZmZ3dlVVVmZmUyIRARI0zd2Zze7Yh4jd7tdkMhARJG7//f////qt////+GQhESNY/////////v/////3UyESNp////"
"///////////9YyESRt///qqt////qqr//+dCEjR9//+qmq7//6qarv/+dCEjWO//2oeK7//qh4rf/+dCEjWf//qHaK7//Zdnrv/9dCEkaf//p2Z6//+oZnr/"
"/5ZCEkbf/+llZ6//+XVnr//5YyEkff/9hlV9//6GVXr//4UyEkfv/9dUV+//11RX3//nQxEkf//5ZEV+//xlRX7//WQhEkf//3VEV///dUNG//+FMhEj3//l"
"QzRf//ZDNF//9kIRETRWZUMiNFZlQyI0VmVDIRESIzMyIiIjMzIiIiMzMiEQAREiIhERESIiERERIiIREAAAARESIiIiIiIREQAAERIjM0RERDMiERAAESNF"
"ZmZndmVDIRABEjTN3Zmt7thkMhEBEkbv/9/////WQhERI1j////////9YyERI2n////////+dCERJG3///qqrv//hSESNH3//6qZqv//hTESNY7//ah4mv//"
"hTISNZ//+oZnnP/+hTISRp//+nZWjf/+dCESRt//6WVWnv/9dCESR9//2FRWn//5YyESR+//11RGn//4UyESR///hkNGz//nQxESR///dTNF3//mQhESPf/+"
"VDI07//UMhERNFZlQyIjRWVDIRARIjMzIhEiMzMyERABESIiEREREiIREQAAABEREiIiIhEREAAAERIjM0RDMiEREAAREjNFZnZlQyIREBESNGeN7u2HVDIR"
"ARI1aO/////oZDIREjV9///////4ZCERJG3///7v///3UyEjWP//6qqt///WMhJG3//6mImu//50ISR///2Yd4rf/+dCE1j//6l2Z53//nQhNY//+pdmit//"
"50ITWP//qYeJr//9YyEkf//+qpmu//+FMhJG3//+2t7//9ZCESNX////////11MhESRo7//////XUyEREiRX3v///YZTIREBEiNWeIiHZUMhEQABEiM0VVVE"
"MiERAAABESIjMzIiEREAAAABERESIREREQAAAAARESIiIiIiEREQAAAREiMzREREMyIREAABEjRWZmZ3ZlQyERABEjTN3Zne7ddkMhEAESRu//3////8ZDIQ"
"ESNY/////////WQhARI2n////u////hTIBEkff//6qqt///WMgEjR9//+piJrv/+dCESNY7//ah3it//50IRI2n//6l2Z63//nQhEkav//qXZ4rv/+dCESR9"
"//+qh4mv//1jISNY3//+qpqu//+FMhI1jv///tre///WQhEjaf/////////nUyESRp///f/////nUyERJG3//qrv//6HUyEREkff/9qZmIh1QyERASR+//2H"
"ZlVUMyERABJH//+XVDMzIhERAAEkf//4VDIiERERAAASTf/+ZDIhEREQAAABI0ZmVDIREQAAAAAAESM0QyIREAAAAAAAABEiIiEREAAAAAAAAAARERERAAAA"
"AAAAAAAAABERIiIiIiIiERAAABESIzREQzMzMiEQABESNFZndmVVVUMhEBESNWfN7tmI3d1DERESNX3////9r//1MhESNX7//////v/+ZCERJG3///7v////"
"1kIRI1j//+qqrf//+WQhEkfv//qYia7//5YyETV///2od4rf//hTIRNY//+pdmiu//6FMhI1j//6l2eK///XQyETWP//qoiKr//6dCERJH///qqqrv//lkIR"
"Ekbv//7a3v//+VMhESNY/////////oUyEREkaP///////9dDIQESNGjv//6v//pkIRABEjRWiZqq//+WMhEAERIjRWZ5r//4UyEQABERIjRGjf/+dCEQAAAB"
"ERIjRu//1kIRAAAAARESI17/91MhEAAAAAAREiRWdlMhEAAAAAAAARIjREMhEQAAAAAAAAERIiIhEQAAAAAAAAABEREREQAAAAABERIiIiIREQABESIzMzMy"
"IRAAESNFZmVVQyEQESNM3dmI3kMRARJG7//f//UyERI1j/////9jIREjaf/////lMhESRt///9l2QhESNH3//6l1QyERI1jv/9l1QyEQEjWf//qFMiEQASRp"
"//+WQyERABJG3//oUyERAAEkff/9dDIRAAASR+//1kIREAABJH//+FMhEAAAEkf//3QxEQAAASPf/+UyEQAAABE0VmUyERAAAAESIzMyERAAAAABESIhERAA"
"AAAAAAARERIiIiIREQAAAREiMzREMyIREAAREjRFZnZlQyEQABEjRnje7t1lMhABEjRt//////5CEQESRt///////UIRARNX///t3e/2QhEBI13//aqqmHUy"
"EREjbf/+qqqYZDIRESNd///+7ahkMhERI1fv/////oUyEREjRo3v////5kIRESNGiaqt7//3QhERI1eJqqqv//dCEREkb+yqqt//9kIRETXv///////VMhER"
"Nf///////WQhEBEkbe////6GQyEQESNFZ4iIdlQyEQABEiNEVVVEMiERAAAREiIzMyIhERAAAAERERIhEREQAAAAARESIhERAAABESMzMyERAAESI0VlQyER"
"AREjXe7tUyERESNX7//XQyERI1ev//qWQyESR97//+3cQhI13//////UISNu//////dCEjXu///+7lMhI1iv//qoZDESNXr//6hkMhEjR6//+nUyERI0ff/+"
"lkMhECNY7//YZDIRAjWP//qGQyEQE1j//6l1MhEBJH///qhkIRASR+////5TEQEjXP////UxEBEkbO///UIRARI0Vnd2QyEQARIjRERDIRAAAREiIiIhEQAA"
"ARERERERAAAAEREREREREREREQAREiIyIhESIjIiEQESNEVDIiIzRUQyERI03d1TMjRd3dQhESRv//ZDNF3//lMhI1f//3VDRt//5jISNp//+FRFjv/9YyEk"
"ff/+hUVp//+VMiNH7//YZVev//hTIjWP//qGVo3//oUyI1n//6hmed//50MiNp//+od4rv/9dCEjbf//qYiq//+WQhI23//6qqr///lTISNY///93v///oUy"
"ETV/////////50IREjbf///////9YyERI0aO///q7/9kMRERI0V4iIh3d2QyEQARIjRVVVREQzIRAAEREiMzMzMiIREQAAARERIiIhEREQAAARERERERERER"
"EREBESIyIhEREiIiIRARI0VEMiIiNERDIREjzd1kMiNFbd1CERJN//1kM0V///UyETV//+dURX7//VMhE1j//5dWef//dCERNX7/+od57/+GQhESR+//2Ynf"
"/+dTIREkbf/+qq//+GQhEBI2n//6rv/9dDIRASNZ//+q//6FMhEAEjWO//z//5ZDIRABEkff////2FMhEAARJGr////5ZCERAAESNY////x0MhEAABESR+//"
"/nUyEQAAABEjXv//dUIREAAAAREkVndlMiEQAAAAAREjREQyERAAAAAAAREiIiIREAAAAAABEREREREREREREREREREQERIiIiERIjMyIhESIiIhEREjREMy"
"IjRVVDMiI0REMhESNd3VQzNG3d1UMzRt3dMhEk3//WRFaP//dURW3/9TIRJH//11Vp7//5dVaP/+UyETV//+h3jf///Yd47/+FMhE0f//6iK////2Yiv/+dC"
"ERJH7/+qrf///+qq7/+GMhESR9//qq//7v/6rf/9dDIREkbP/9rv/67/+q//+GMhEBI2n//a//6t//rf/9dDIRASNZ//7v/6qv/9//+GMhEAEjWO////2qr/"
"///8dDIRABEkfv///6ma////6FMhEAARJG3///2Hee///5ZCIRAAESNY///4ZWff//51MhEAAAESRv//xlRFf//3UyERAAABEjRndlQzNFZ3ZTIREAAAABEi"
"NEQzIiI0RDMhEQAAAAARESIiIRERIiIiERAAAAAAAAEREREREREREREAAAAREiMyIiIiMyIREAAAESNFVEMzRFVDIRAAARI93ddlVWfd3TEQAAESTf//h3if"
"//wyEAABEkb//9qa///VMRAAARI2z//6r//9ZCEQAAESNY7//v//11MhEAABEjV6/////YUyEQAAARI0eu///9lkMhEAABESNXrP//6oUyERAAARIkaK///+"
"p1MhEAAAESNXr////6hTIRAAARI0af/////pYyEQAAESNY///q///WQhEAABE0fv/+qt//90IRAAASNe//6Yie//1DEQAAEj7//nZWaP//QhEAABEkVmZURE"
"VmZDIRAAARIjMzMyIzMzMhEAAAAREiIiIiIiIhERAAAAAAERERERERERERERAAABESIyIhEREiIiIRAAABEjRUQyIiI0REMhEAARI83dZDIjRW3dQhEAARJN"
"//5kM0V///UyEAARNX//+FRFff/9UyEAARNY//+XVnn//3QhEAARJH7/+od57/+GMhEAARJH3//Zid//50MhEAARJGz//qqv//hjIRAAARI2n//6rv/9dDIR"
"AAARI1j//6r//oUyEQAAARI1jv/97/+WQiEQAAARI0ff////2FMhEAAAARI0ev////lkIREAAAERI0ev///6dDIRAAAAESI0au///oUyEQAAABESNFeu//+W"
"QyEQAAAREjRniu//11MhEAAAARI12Jrf/+hTIREAAAARJM/////4ZCIRAAAAARNf////+GQyEQAAAAARJO///+hkMhEQAAAAARI1Z9yGVDIREAAAAAAREjRV"
"VUMiERAAAAAAABESIzMyIREQAAAAAAAAERESIRERAAAAAAAAAAAAEREiIiIiIhEQAAAREjNERERDMiEQAAESNVZ3d3ZlQyEQARI13d3d3d3dQhEAESTf////"
"///1MRABE07///////5TEQARJO7u7u///3QhEAESRXmqrv//hjIRABEjRoqu//+WQyEQERI0aK7//6dTIRABEiRXrv//p1QiEQARI1ee//6oZDIREBEjR57/"
"/ql1QyEQARJGnv/+qpdlMhEAEjWP//+qqphkIRABI2////////9DEQASRv///////+UhEAEj3////////UIRABEkV4iIiId2QyEQARIjRVVVVUQzIRAAAREi"
"IzMzMiIREQAAAAAAAREREREAAAABERIiIhEQAAAREiMzMyIRAAARI0VmZUMhAAESNGfv/9MhAAESRt///9QhABEjWP///2QhABEkbf//11MhARI1fv/9hkIR"
"ARI1n//6dTIRERJGn//5ZCIRESNX3//oUyEQESNY7//XQyEQEjR6///GQhEAEkad//+FMhEAE1z///11MhEAI13//9pkIRAAI27//+lkIRAAI1jf//ljIRAA"
"I1ev//lTIRAAI1ev//hTIRAAI0ff/+hTIRAAI1jv/9hTIRAAI1jv/8hTIRAAE1j//6hTIRAAElj//+ljIRAAEkf///9kIRAAEjbf//9jIRAAESRt//5TIQAA"
"ESNFZ2VCEQAAARIjREMhEQAAARESIiIREAAAAAERERERAAAAAAAAAREREQAAAAEREiIREAAAARIjMyIRAAARI0VVQyEAAREkb//UIQABEjXf/3QhAAESRu//"
"dDEAESNX//90MQARI1j//nQhABEkaf/9dCEAESR9//lkIQESNH7/+WMhARI1j//4UyEBEjaf/+dTIQESRp//10IhARJH3/+WQhERI0fv/5YyEREjWP//hTIR"
"ESNp//51MhERJGn//XQiEBEkff/5ZCEQEjR+//ljIRASNY//+FMhEBI1j//nQyEQEjaf/9dCEQASRt//ljIRABJG7/+FMhEAEjb//3QhEQASNf/+UyEQABEk"
"VmVCERAAESI0QyIRAAABESIiERAAAAARERERAAAAAAAAAREREREAAAAAAREiIhERAAAAARIjMzMhEQAAABE0VmZDIREAABEjb//nUyEQAAESR///90MRAAAR"
"NN///9UyEAABEkff//5kIQAAESRp7//nQhAAARJGnv/9dCEAABEjaf//pkIQAAESRp//+WQhAAARJGr//5ZCEAABEkff/+lkIQAAEiR9//6nQhAAESNHz///"
"1DEAARI1eu//90IQABEjWO///3QhAAESRt///IUyEAESNH7//ZZDEQARI1j//6dTIRAREkaf//lkIhEBEjR6//+FMhEBESNY3//nUyEQESNGnv/9dCEQARJG"
"jf//hTIRABE03///50MREAETXv//+FMhEAARJP//51QhEQABEjVndlQiEQAAERI0RDMiEQAAABESIiIhEQAAAAAREREREQAAAAAAAREREREREREAAAERIiIi"
"IiIRERABESM0RDMzMzIREREjVndmVERUMhERI1fv/4d2f/0yERNH////2Z3/1CESNf////////YyESNv/pnf///9UyESPf93d4///FMhERI0VURWd3ZUIRER"
"EiMzMzREQyIRABEREiIiIiIhEQAAAREREREREREAAAAAEREREREAABESIiIhEAABEjREQyEQARI0buZDIQARJG//9jIQARNO///UIQERNd//9kIQESNG7/1T"
"IQESNGeHZDIQESNWiHUyERESRu/9UyEREjV//9UyEREjaf/9YyEREkff/5YyERI1jv/4UyERI1n//4UyERJGz//oUyERJH7//nQyEBNY///XQhEBNY///WQh"
"EBNd//+FMhEBJO//9kIRABI1Z3ZDIRABEjREMyEQABESIiIhEQAAERERERAAAAABERERERAAABERIiIREQABESM0QzIREBEiNFZmVCIRESRX7//VQhESNd//"
"//5TIRJG/9md/3QhE13+mInv5TITXf2Yid/lMhNd/piJ7+UyEkb/2Z3/dCESNd////5TIREkV+//1UIRESI0VmZUIhEBESM0QzIREAARESIiEREAAAERERER"
"EAAAEREREQABERIiERAREjMzIhERI0VlQyESNd/+VCESRv//5CESTf//5TESRv//1CESNd/9UyERI0VUMhEREjMzIREBEREhERAAAAABERERERAAAAAAAREi"
"IiIREAAAAAARI0REMhEAAAAAESNG7mQyEQAAAAESRv//ZCEQAAAAESTf//1CEQAAABEjTf//xCEQAAAREjRu/9UyEQAAARIjVoiGQyEQAAERI0Z4h1QhEQAB"
"ESNGj//1MhEAAREjRo7//lMhEAARI1ae//50MhEAASNHn//+hkMhEQASRt///ZZUMhEQAjWP//2XZUQyERAjbv/+qXZmVDIRAkf//+qpiI9lMhAkbv//2qrf"
"/9QhEjbf///////+QhETR////////VMhESNX7////+hkIRARI1aJ3ch2VDIRAREjRVZmVUMiEQABESIzMzMyIREQAAERESIiIRERAAAAAAAAAAAAABEREREQ"
"AAAAAAAAAAABERIiIREAAAAAAAAAABESMzMyEQAAAAAAAAABEiNVZUMhEAAAAAAAABEiRX//4yEQAAAAAAABESNd//5TIRAAAAAAAAESNd//1kMhAAAAAAAA"
"ESNG3dl1MhEAAAAAAAESJFiaqXUyEQAAAAAAERI1fv/+dTIRAAAAAAARI1ff//+WQhEAAAAAARIkaf///9dCIRAAAAAREjWO////6FMhEAAAABEjR6/////4"
"UyEQAAABEiRp//+u//lkIRAAAAESNY7//a7//XQhEAAAESNHr//6rf/+hTIRAAESJGnv/8qq//+FMhEAARI1jf/+qqr//5ZCEQARI0ev//qqqu//10IRAREk"
"ae///MzM///oUyEBEjV9//////////hTIRESRp//////////+WMhESNY7//////////9ZCERJG3//6qZmZqu//10IRE0f//9l2ZmZ53//nQhETXv/+hlRERF"
"af//ZCERPf//dkMyIjNW///DIREjVmZUMiIiIjRWZUIRERIzMzIhERERIjMzIhEBERIiERERERERIiIREAAAAAAAAAERERERAAAAAAAAABERIiIhEQAAAAAA"
"ABESIzQzIRAAAAAAARESNFZlQyEQAAAAARESNGf//kIRAAAAEREiNG3//mQiEQAAERIjNG3//XZDIRAAERI0VWfd2phlMhEAARI1Z4mqqqqYZTIQARI13///"
"/////+QhABEkbv////////1CEBEjWP/////////EIQESNp///u7u7u7lMhARJGr//6qqmYdlQhEBI0ff/+qpiHZlQyEREjWO//6qqYh1QyERESNZ///qqqqY"
"ZTIRARJGn////////1MhEBEkev///////+UyEQEjR9////////1TEQASNY7//qqqqYdUIRABI1n//9qpiHZUMhEAEjaf//qph3ZlQyEQASRq//+qqZiHZTIR"
"ABJH3//93d3d3dQhEAEkfv////////9TEQASRv/////////lMRABI1/////////9QhEAESRXiIiIiId2QyEQARIjRFVVVVVEMyEQAAERIiMzMzMyIhERAAAA"
"AAAAERERERAAAAAAARESIiERAAAAABESMzMyEQAAAAESI1VlQyEQAAAREkV//+MhEAAAESNd//5TIRAAARI03//WQxEAAAESNd3YZTIRAAARI0aJmGQiEQAA"
"ESNd//5kIREAABEkfv/+ZCEQAAESNY///WQhEAABEjaf//ljIRAAARJGn//4UyEQAAESR9//6FMhEAARI1jv/+dTIQAAESNY///XQhEAABEjaf//lkIRAAAR"
"JGr//5YyEQAAEjR9//6FMhEAABI1jv/+dTIRAAASNY///XQhEAAAEjaf//lkIRAAABI2n//4UyEQAAASRt//6FMhEAAAEkbv/+dCEQAAABI2///VMhEAAAAS"
"Nf//ZDERAAAAESNWZUMhEAAAABESMzMiERAAAAABESIiEREAAAAAAAAAAAABERERERERAAAAAAAAARESIiIiIREQAAAAAAAREjNERDMzIRAAAAAAAREjRs1m"
"VVQyEQAAAAAREjRv//iI/0MhEAAAAREiNe///+/+UyERAAAREiNG/5nv//dUIhEAAREjRWbXd33th2UyEQABEjVnh3ZlZniIdlMhABEjXf/9hlRFZ9//5DEA"
"ESRu//+XVEVo7//VMgESNY///9hlRWj//9UyARI2n///+oZVef//lTIBEkav///+l2aK//+FMgETR9/////Zd53//oUyESNY7/////qYrv/9dDIRI1n//+//"
"/aqv//10IREjaf//rv//qq//+WQhESRq//+q///qr//5YyESNH3//qrf//zf/+hTIRI1jv/9qq///+//2FMhEjWP//2YnP/////XQhESNp//+oeK7////6ZC"
"ERI2n//5dmiv////ljIREkbf/+hlVo3///6FMhESRu//11RFaf///XQhEBI2///WQzNX3//9YyEQEjX//2QyIjV+//ZDERARI1ZlQyIiI0ZmZDIRABESMzMi"
"ERESM0QzIRAAAREiIhERERESIiEREAAAAAAAAAAAERERERAAAAAAAAAAABERIiIhEQAAAAAAAAABESIzQzIRAAAAAAAAABESNFZlQyEQAAAAAAARESNGf//k"
"IRAAAAAAEREiNG3//lQhEAAAAAEREiNG3//XUyERAAAAERIjRWjd2pdUMhEAAAESI0VomqqqqGQyERAAESI1aN7////thkMhEAESJFff///////pZDIRERJF"
"jv////////+WQhERI1fv///t3e////hTIRI0ff//6pmJqu///WQhEjaf//6odmZ5r///dTESR+//+oZURFed//+FMiNY///ZZDMzRor//5UyI2n//6dUMjNF"
"iv//lTIkbf//p1MyI0WK//+FMiR9//6nUzIzRo3//4UyJH3//6dUMzRXnv/+dCEkbf//qGVERWiv//1kISNp///ahmVniv//6FMhE1j///2piInP//+WQhES"
"Rt///+3d3v///XUyERI1ff/////////XUyEQESNX3///////6XUyERAREjV47////+12QyERAAERI0V4iZmIdlQyERAAABESI0VVZVVDMiERAAAAARESIzMz"
"MyIhERAAAAAAERERIiIiEREQAAAAAAAAAAAAAREREREAAAAAAAAAAAEREiIhEQAAAAAAAAABESMzMyERAAAAAAAAEREjRWVDIRAAAAAAERERI1b//kIREAAA"
"ARERESNN//5UIhEQAAERIiIjTf/9ZUMiEQABESMzMzTd12ZVQyIRAREkVmVERFVVZnZUIRARI17//UMzRFf//1MhESNH//91MzNGj//2QhESNo//+FMzNH3/"
"/mQhESRt//+FMzNX7//WQhEjR+//51MzRY///WQhEjWP//10MzRp//+WMhEjaf//lkMzRq//+FMhEkav//lkMzV9//6FMhEkff/+hkM0WO//10MhI1fv/+hU"
"M0af//1kIRI1j//9hURFev//ljIRI1j//6hkRGjP//hTIRI1j//6llVXnv/+dDIQI1j//9qHZ5r//5ZCEQEkf///qpmq7//oUyEQEkbf///d3v//+WQhEAEj"
"WP////////10MhEAESRo///////9dTIRAAESNGjf////2GUyERAAARI0VniZmHZUMhEQAAABEiNEVWVUQyIREAAAAAERIjMzMzIhERAAAAAAARERIiIhEREA"
"AAAAAAAAAAARERERERAAAAAAAAABERIiIiIREAAAAAAAAREiMzMzMiEQAAAAAAERI0VURVVDIRAAAAABESNO/2Zv/UIREAAAARESNf/9fv/1MhEQAAERIjRv"
"/3fv/VQiEQABEiNEVW1mVtd2VDIRARIkVmZlVURWd3dUIRARI17//VQzRGf//1MhESNX//91QzNGj//2QhESNp//+FMzNX3//nQhESR9//+FMzNY7//WQhEj"
"R+//51MzRY///WQhEjWP//10MzRp//+WMhEjaf//lkMzRq//+FMhEkav//lkMzV9//6FMhEkff/+hkM0WO//10MhI1fv/+hUM0af//1kIRI1j//9hURFev//"
"ljIRI1j//6hkRGjP//hTIRI1j//6llVXnv/+dDIQI1j//9qHZ5r//5ZCEQEkf///qpmq7//oUyEQEkbf///d3v//+WQhEAEjWP////////10MhEAESRo////"
"///9dTIRAAESNGjf////2GUyERAAARI0VniZmHZUMhEQAAABEiNEVWVUQyIREAAAAAERIjMzMzIhERAAAAAAARERIiIhEREAAAAAAAAAAAAAERERERAAAAAA"
"AAAAEREiIhEQAAAAAAABERIzQzIRAAAAAAABESI1ZlQyEQAAAAABESNFf//kIRAAAAARESJF3//lQhEQAAAREiM13//XVDIRAAAREiNFbd2YdUMhEQAREjRW"
"eaqpiHZTIRAREjVoze7aqt3dUyEREjV9/////a//9jIREjV+/////////mQhESRt///+7////9ZCESNY///qqq3///pkIRJH7//6mImu//+WMhE1f//9qHeK"
"3//5UyETWP//qXZorv/+hTISNY//+pdniv//10MhE1j//6qHia//+mQhESR///6qmq7//5YyERJG7//+2t7///hTIREjWP////////50IRARJGj////////W"
"QhEBEjRo7//+rv/3QyEQARI0VniIh3d2UyEQABESI0RVVVREMyERAAARESIjMzMyIiERAAAAAREREiIiERERAAAAAAAAAAERERERAAAAAAAAARESIiERAAAA"
"AAABESMzMyEQAAAAABESI1VlQyEQAAAAERIkV//+MhEAAAAREiRd//5TIRAAABESI13//WQyEQAAERIjVt3ZdkMhEAAREjRXiaqYZDIREBESNGid7t2YZDIR"
"ERI1ef/////YVCEREjV9///////nUyERJH3//+3e///WMhEjWP//2qqq//90IRJH7//qqqqu//hSETWP//7d3d3//4UxI1j/////////90ISNY//////////"
"ZCETWP//qqqqqphkMhEkf//9qqqqmGUyERJG3//9qq3vhkIhESNY///////+UyEQESRo7//////lMhEBEiRX3v///nVCEQABEiNWeIiHZUIREAABESM0VVVE"
"MhEQAAABESIjMzIiERAAAAABEREREREREAAAAAAAAREREREAAAAAARESIiERAAAAAREjMzMhEAAAAREjRWVDIRAAAREjVv/+MhEAABEjXf/+UyEAABEiTf/9"
"ZCEQAAESNd3XZDIRAAERJGeIdTIREAARI1zd11MhEAABEkbv/9UyEQABEjWP//hTIRAAESNp//+FMhEAARJG3//nUyEAABI0ff/9dCEQAAEjWO//1kIRAAAS"
"NY//+WQhEAABJGn//4UyEQAAEkbf/+hTIRAAASR9//10IRAAABJH7//WQhEAAAEkf//4UyEQAAASR///dCERAAABI9//5TIRAAAAETRWZTIREAAAARIjMzIR"
"EAAAAAERIiEREAAAAAAAAAAAAREREREREQAAAAABERIiIiIhERAAAAAREjNERDMzIRAAAAARI0bNdlVUMhEAAAESNG//+Yj/QhEAABESRu///+/+QxEAAREj"
"Rv+q7//2QxEAERI0Z9mare2GQhEAESNWeJmqqql1QhEBEjXN3aqt7tqGQyEBEkbv/9/////XUyERI1j////////9ZCERI2n////////+dCERJG3///qqrv//"
"hTESNH3//6qZqv//hTESNY7//ah4mv//hTISNZ//+oZnnP/+hTISRp//+nZWjf/+dCESRt//6WVWnv/9dCESR9//2FRWn//5YyESR+//11RGn//4UyESR///"
"hkNGz//nQxESR///dTNF3//mQhESPf/+VDI07//UMhERNFZlQyIjRWVDIRARIjMzIhEiMzMyERABESIiEREREiIREQAAAAAAAAAREREREAAAAAAAAAEREiIh"
"EQAAAAAAARESMzMyEQAAAAAAERIjVWVDIRAAAAABESNFf//jIRAAAAAREiRd//5TIRAAABERIzXf/9ZDIRAAAREiNFbd2HVDIRAAERI0VniqqXVDIRABESNG"
"ed7u2pdTIREBEjVo7////+l0MhERI1ff//////+WQhERJG3///7v///4UyESNY///qqq3//9YyESRt//+piJrv/+dCESR///2od4rf/+dCETWP//qXZnnf/+"
"dCETWP//qXZorf/+dCETWP//qYeJr//9YyESR///6qma7//4UyESRt///tre///WQhESNX////////11MhERJGjv/////9dTIRARIkV97///2GUyERABEiNW"
"eIiHZUMhEQAAESIzRVVUQyIREAAAAREiIzMyIhERAAAAABERESIREREQAAAAAAAAAAERERERAAAAAAAAARESIiERAAAAAAABESMzMyEQAAAAAAERI0VlQyEQ"
"AAAAEREjVv/+MhEAAAERESNd//5TIREAERESI03//WUyIRAREiMzNN3XZlQyEQESNFVVRVZmZlQyERI03d1lRFZ93dQhESRv//dURW3//lMhI1j//4VEV9//"
"5jISNp//+GRGjv/9YyEkff/+hkVp//+VMiNH7//YZVev//hTIjWP//qGVo3//oUyI1n//6hmed//50MiNp//+od4rv/9dCEjbf//qYiq//+WQhI23//6qqr/"
"//lTISNY///93v///oUyETV/////////50IREjbf///////9YyERI0aO///q7/9kMRERI0V4iIh3d2QyEQARIjRVVVREQzIRAAEREiMzMzMiIREQAAARERIi"
"IhEREQAAAAAAABEREREREAAAAAAAERIiIiIREAAAAAERIjMzMzIhEAAAABEjRVRFVUMhEAAAESNO/2Zv/UIRAAAREjX//X7/9TIRABESNF//d+/9UyEQERI0"
"Rm1mZtd1QiEBEjRWZmVVZ3dlMhESNd3dZURGfd3UMREkb//3VEVt//5TISNY//+FRFff/+YyEjaf//hkRo7//WMhJH3//oZFaf//ljIjR+//2GVXr//4UyI1"
"j//6hlaN//6FMiNZ//+oZnnf/+dDIjaf//qHeK7//XQhI23//6mIqv//lkISNt//+qqq///5UyEjWP///d7///6FMhE1f////////+dCERI23////////WMh"
"ESNGjv//6u//ZDERESNFeIiId3dkMhEAESI0VVVUREMyEQABERIjMzMzIiEREAAAERESIiIREREAAAAAAREREREREAAAARERIiIhERAAARESIzMyIhEQABES"
"M0REQyIRABESNFZmZUMhEAESI13///1DIQERI0bv//9kMhAREjV+///3UyEBEiRY///+dTIRESNGj///10MhERI0af//+WQyEREjV5///4ZDIRESNX3//+dU"
"IhERI1ff//11MiERIjV+//+WQyEREiRY7//oZDIRESJFj//9dTIhERIkV///hkMhEREiRX//91QyERESNFz//mUyIRERI0Vnh2VDIREBEjRniHZTIhEQEiNW"
"3tdlMiERASNG7//2UyIRABI03///1TIREAEjTf///lMhEQASNN///2QyERABEjXf/9UyIRAAESNFVlVDIREAAREiNEQyIREAABERIiIiIREQAAAREREREREQ"
"AAAAAREREREAAAAAAAAREREREREREQAAERESIiIiIhEREAEREiIzMzMzIiERAREjNERERERDIhEREjRWZmVWZlQyERESNe//537//kMhESNG7//Yjv/9QyER"
"I1f//5mf//dTIREjV///qq//91MhESRo//6qr//nUyERJG3//Znf/9ZDIREkbf/5md//dUIhESNe//iI7/9lMhERI0//53f//lQyEREiRVZlVWZlQyERERIz"
"REREREQyEREBESIzMzMzMiEREAARESIiIiIhEREAABEREREREREREAAAAAAAAAEREREREREREQAAAAAAAAAREREiIhESEREREAAAAAABEREiIzIiIiIiIREQ"
"AAAAARESIzREQzM0RDMhERAAAAEREiNFVmVURVZVQyERAAABERIjRW//1mZu//UyIREAABESI0V9//h3eP/+ZDIREAAREiNFeP//qYnP/9dUIhEAERI0Vnmv"
"/9qqrv/6hkMhEAESI1aJrf/6qqr//ql1QiEAESNN7u7//+7u///+7uQyEBESNe/////////////9UyEBEiRf/////////////9QyEREjRe7u///u7v///u7m"
"QyEREjRXmq//6qqt//qphlMhEREjRXmt//qqqv//qYZUMhERIjRnmu//qqqv/9qXVDIhERI0V4qv/+qqrf/6qGVDIRESNFeKrf/9qqrv/6qXVDIRESNM////"
"/////////+VCIRASNd/////////////+VCERASNe/////////////9UyERASNO7u///u7u///u7mQyEQASNFea//6qqt//qpdkMhEQARI0aK//2qqu//qHVD"
"IhEAARI0V+//qYmv/+hlQyIREAAREjRv/+h3ff/9ZUMiERAAAREiRf/9Zlbv/2VDIhERAAABESNFVVVERWZUQyEREQAAABERIzREMzNEQzIhEREAAAAAEREi"
"IiIiIiIiIREQAAAAAAAREREREREREREREAAAAAAAAAAAAAAAEREREREAAAAAAAAAAAABEREREREREAAAAAAAAAARERIiIiIRERAAAAAAAAARERIjNEMyIREQ"
"AAAAAAERESIzRVVUMiEREAAAAAEREiI0Vn//VDIhERAAAAEREiM0Vn3/52QyIREAAAEREjNFZ4rv/ZdkMiERAAAREjRWeJqv/+qYZTIhEAAREjRWie//////"
"7WQyEQAREiRXjv////////9TIRABESNWj//////////1MhEAESJGj//////////9UyEQARI0ff///q//2q3/ZDIRAREjV////ar/+qqYZUMhEBESNY///6qv"
"/6qHZUMhEQERI1j///2t/+qoZUMyERAREjWP///9//2qhlQyIREBESNX7//////aqXZDIhEQERI0aP///////al1QyERAREjRXnv///////YZDIREBESI1aK"
"3v//////51MhEQERIjRXiqr//////4ZCIRAREjNFZ5qv/+////1kMhEREiNFZ4mq//qv///nQyERESNFZ4iqr/+q7//9dDIRESJF39mqrf/qr///1kMhERI0"
"b//9qu/+3///+FQhEREjXf///////////XUyERESNf///////////XVCIRARI0bv/////////XZDIREBESNWje/////+2HZDIhEQERI0Vnmq//6ql2VDIhEQ"
"ABESM0V4r//ZhlRDIhEQAAAREiNFaP/4dkQyIhEQAAABERIjRW//ZUMyIREQAAAAARESIzRVVEMiIREQAAAAAAERESIzMzIiEREQAAAAAAABEREiIiIhEREA"
"AAAAAAAAABEREREREREAAAAAAAAAEREREREREREREREREQAAAAERESIiIhEREREREREREAABERIiMzMzIiERESIiIhERAAERIjNFVVRDIiIiIzMzIhEAERIj"
"RWd3dlQzIiM0VVQyERARIjRn7//+dlQzNFXf/+MhEBEjRt/////3ZURFbf/+QyEQEiRX//7d//12VWeP/+VDIRASNG7/6qrf/4dmeP//dUIhEBI1f/+qqq//"
"mIif//hkMhEQEkXf/6mar/+pme//h1QyERASRt/+qqrf/6qu//2GUzIRERJG3/6qqu/+qu//2XZTMiEREjXf/6qq//qt//6pdlQyIRESNX//7d//6t//6qmH"
"ZUMiERI0bf////6q//6qqtmGVDIRESNW3///6q//+q7///1lMhERI0Vniaqq//+q7////+ZDIREiNFZ4mq7/+q7//a7/91MhERIjRWea7//ar/+qqv/9UyER"
"EiNEZ53//arv/qqq7/1kIRERI0Vo3//qqv/9qqrv/WQhAREjRXz//pma//qpmv/9VCEBESNGj//ph4n//Kqq//dTIQESJFf//4dmeP/+qq7/5kMhARI0X//4"
"dlVn3//t7/91QiEBEjTv/3ZUREZ//////WQyEQESPv/9VUMzRGfv//52QyIRAREjRFRDMiIzRWZ3ZlQyIREAESIzMzIiIiIzRFVEMyIREAARESIiIREREiIz"
"MzMiEREAAAERERERERERESIiIhEREAAAAAEREREREREREREREREAAAAAAAABERERIhERERAAAAAAAAABEREiIiIiIhEREAAAAAAAEREiIzREQzMiERAAAAAA"
"ABESI0RWZmVUMiERAAAAAAERIzRmeIyHdkMiERAAAAAREiRWff///+hlMhEQAAAAERI1aP//////1kMhEQAAAREjRo////////dTIREAAAERI1fv//2q3//9"
"VCERAAAREjRo///aqq7//WQyEQAAERI0bf//qqqv//1kMhEQABEiNX3//9qq7//4ZDIREAERIjV6///6rv//12QyERABESNFiv///////ZdUMiEQERIjRorP"
"/////6qXZUMhERESNFea3////9qqmHZDIRERI0V53//////aqq7XVDIRESNWjv//////+qrf/+UyERI0aO///qr///+q///VMhESNH3//+qqr////v//dTIR"
"EjV///+qqqr//////XUyERJGj//+qqqq3////+l1MhESRt///6qqqq3///+pZDIREkWP///qqqqu////2GQyERI1f////+7v//////1kIhESNG3/////////"
"///+UyEREiNX3////////q//5kMhEREjRWjv/////amZ/WUyIRAREjRWeInMmYh3dmVEMhEQEREiNFVmZmZlVUREMiERAAEREiMzRERERDMzMiIREAAAERES"
"IiMzMzIiIiEREQAAAAEREREiIiIiERERERAAAAAAERERERAAABEREREREAEREiIiIREQERIjMzMiERESI0VVQyERESNO//5CIREjRe//1DIREjRv//ZTIREj"
"V///dTIREkV//+ZDIREkXf/9ZDIREkXf/3UyEREjXv/2QyEREjT//lQiEREiNFVUMhERESIzMzIREQERIiIiEREAARERERERAAABEREREAAAAAAAABERERER"
"EQAAAAARERIiIiEREAAAARESIjMzIhEQAAARESI0RVQzIREAABESI0XN3dMhEQABESI0Z///5DIRABESI0aP//9kMhEAERI0V+///WQyEQERIjV9///nVCIR"
"AREjRo///5ZTIREREiRX3//+hkMhERESNGj///l1MiEQESI1ff//6GQyERARI0aO///XVDIREBEjRp///5ZTIhEAEiNX3//+hkMhEQASNGjv//p1QyERABI0"
"aP//+XUyIRAAEjV5///oZDIREAASNX3//9hkMhEQACJFjv//p1QiEQAAIkaO//+XUyIRAAAjRo///5dTIREAACNGj///lkMhEQAAI0aP//+WQyERAAAjRo//"
"/5ZDIREAACNGj//+hkMhEQAAI0aP//+GQyERAAAiRo///4ZDIREAACJFjv//lkMhEQAAEjV+//+WQyERAAASNG3//9ZDIREAABI0aP//1kMhEAAAESNX///m"
"QyEQAAARI0Xv//UyERAAABESNF3d1DIREAAAERIjRERDIhEAAAABESIjMyIhEQAAAAARESIiIREQAAAAAAEREREREQAAAAAAAAABEREREREQAAAAABERIiIR"
"ERAAAAAREiMzMiEREAAAERIjRVQzIREAAAERI13d1UMhEAABESNN//9lMiEAABESNW///mQyEAABESNX///3UyEAABESNX7//4ZCIAABESNX3//4ZDIAABEi"
"NX3//9dTIAABEiNXz//9dTIAABEiNXn//9dTIAABEiNXn//+dTIAABEiNXn//+dTIAABEiNXr//+dTIAABEiRX3//9dTIAAREjRY3//9dTIAAREjRo7//9dT"
"IAAREjRo7//5dDIAAREjRp///4ZDIAARIjV5///oZDIAERI0V9///nVCIAERI0aO///XUyEAESI1ef//+WQyEBESNFff//6FQyEBESNGjv//11MiEREiNXn/"
"//hkMhEREjRo7//9dUIhEREjVp///4ZDIRERIkV+///XVDIRERI0aP//+GQyERARI1b///hlMiERARI13//+dUMhEQARI0///nVDIREQARI03d1lQyIREAAR"
"EjRFRDMiERAAABEiIzMyIREQAAABERIiIhEREAAAAAERERERERAAAAAAAAAREREREREQAAAAAAERERIiIREREAAAARERIiMzMiEREQAAEREiI0REQzIhERAA"
"ERIjRFbdxUMyERABESM0Vn3/12UzIREBEiRWeJ7/2XZTIhEREjRv/a7/qu/UMhEREjXv/+//7//1MhERI0bf///////VMhERI0aK7////9l1MhERI1aa7///"
"/qhkMhERI1fv//////1kMhERI1///v/+//5UIRERI03+yv/q3/ZDIRERIkXYnf/Zh8VCIRAREjRWfv/XZUQyERABESM0Xd1lRDIhEQABERIjRFREMiIREQAA"
"EREiIzMyIhEREAAAARERIiIiERERAAAAAAEREREREREAAAAAAAAAAREREREAAAAAAAAAERERIREREAAAAAABEREiIyIhERAAAAABERIjNEQzIhEQAAABERIj"
"RWZlQyIREAABERIjRW//9UMhERABERIjRX3//mUyIREAERIzRWjv/9dkMiERERI0Vniv//qXVDIhERI0Vnmq//+qh1QyEREjXd3d///93d3EIREjRv//////"
"///9QyESNN//////////ZDIRI07/////////9kIREiTN3d3//93d3VMhERI0V5qv//qodlQyEREiNFed//6YZUMyERERIjRo7//YZUMiERABESI1b//8ZUMi"
"EREAEREjRf//ZUMiEREAABESI0VmVUMiEREAAAAREiM0RDMhEREAAAAAEREiIiIhEREAAAAAABERERERERAAAAAAAAABEREREQAAAAAAAAAREREREQAAABER"
"ERERERAAEREiIiIhEQAREiM0RDIhERESI0VmVDMhEREjRt//ZTIRESI13///ZDIREjRv///2QyERI0b///91MhESNH7///ZDIREjV5///WQyERI1ef/+dTIR"
"ESNW7/+GQyEREjVv/9ZTIhERI03/5lQyERESNP/2VDIRERESNERDMhERABEiMzMiEREAARESIiEREQAAARERERERAAAAABERERAAAAAAERERERERERAAEREi"
"IiIiIREQAREjM0RDMyIREREjRFVVVUMiEREjRWd4d2VDIRESNf/////9MhERJF//////UyEREjX/////9TIRESPe7u7u7UMhERIkRWZmZUMiERESM0REREMi"
"ERAREiIzMzIiERABEREiIiIRERAAAAEREREREAAAERERERERAAEREiIiIhEQEREjNEMyIREREjRVVUMiEREjRd//ZDIRESNd///1QhERI1////ZCIREjX///"
"9kIhESNe///1QhERI0Xv/mQyERESNFZlQyIRERIjNEMyIREBESIiIiIREAAREREREREAAAEREREREAAAAAAAAAAAAAERERERERAAAAAAAAAAABEREiIiIREQ"
"AAAAAAAAABERIjMzMiERAAAAAAAAAAERIjRVVEMhEQAAAAAAAAERIjRe7u4yERAAAAAAAAERIjRn///UIhEAAAAAAAAREjRX7//2QiEQAAAAAAAREiNX3//9"
"VCIRAAAAAAARESNGj//+dTIREAAAAAABESNFff//hkMhEQAAAAABESI0aP//51QiERAAAAAAERI0V9//+GQyERAAAAAAERIjVo///nVDIREAAAAAAREjRo7/"
"/4ZTIhEQAAAAAREiNXn//9dUMhEQAAAAABESNGjv//hkMiERAAAAABESI1ef//11QyERAAAAABERI0aO//+GQyIREAAAAAERI0V9///HVDIREAAAAAERIjRo"
"///oZDIREQAAAAAREjRX3//5dTIhEQAAAAAREiNWj//+hkMhEQAAAAABESNFff//l1MiERAAAAABESI1aP//6GQyERAAAAAAERI0aO//+GUyIREAAAAAERIj"
"V5///XVDIREAAAAAEREjRo7//4ZDIhEQAAAAAREiNXn//9dUMhEQAAAAABESNGjv//hkMiERAAAAAAERI1ff//x1MiERAAAAAAARIkV///6GQyERAAAAAAAB"
"EjRt//+GUyIREAAAAAAAESNG///nVDIREAAAAAAAARI03//3ZDIREQAAAAAAABEjT//+ZDIhEQAAAAAAAAESI1VmVDIhEQAAAAAAAAAREjNERDIhERAAAAAA"
"AAABERIiMyIhERAAAAAAAAAAARERIiERERAAAAAAAAAAAAEREREREQAAAAAAAAAAAAAAAAERERIiIiEREREAAAAAERESIiMzMyIhEREAAAARESIjNERERDMi"
"EREAABERIjNFZmZmVUMiEREAEREiNFZ4jd2IdlQyERABESI0Z47////+hlQyEQERIjVo3///////hlMiEREiNGjv////////hkMhERI0aO/////////+dUIh"
"EiRX3////Kqv///4ZDIRI0aP///qqqqv///XUyEiRX7///qoiJrv//51MhI0aP///ah3eK3//+hTISNX3///qXZnit///4VCIkWO///qh2Zonf//6FQiNGj/"
"//2XZmaK7//+hTIjRp///6l2Vmiu///nUyI0ef//+odlZ4r///11MiNX3///qHZmea///5ZDIjV9///6l2Z4rv//6GQyI0ff//+ph3ia///9dTISNGn///qp"
"iJrv//+GQyEiRo///9qqqu///9dTIhEjV+///+qq7///6GQyERI0aP/////////4ZTIhESJFfv////////h1QiERESNGfv//////6GVDIREBESNFeO////6X"
"ZUMhERAREiNFZ4jMiHZUMyEREAAREiM0VmZmZUQyIREQAAARESIzREREMyIhERAAAAARESIiMzMiIhEREAAAAAARERESIiIREREAAAAAABERERERERERABER"
"IiIiIiIRERESIzNERDMiERESI0VWZlVEMhERI0V3iIh2VDIRIkX//////1MhEjRv/////+ZCISNd//////1kMhI07/////+WQyEjRnmt///5ZDISJFaK3///"
"hkMhEjRorv//6GQiESNFeu///YVCIRI0V6///6dTIhEjRXr///l1MhESNFjf//+WQyERI0aO///oZDIREjRo7//9hUIhESNGn///p1MiERI1ef//+XUyERIj"
"V9///4ZDIREiNX7//+hkMhESJFjv//11MiERIkaP//+XUyEREiRo///4ZDIRERJG3///dTIhEBEjXv//5kMhEQESNO///VMiERARIjRWZVQyERABESI0REMi"
"EREAAREiIjIiEREAAAERESIREREAAAAAABERERIiIiIREREAAAAAERESIiMzMyIhEREAAAARESIjNERERDMiEREAAAERIjRFZmZmVUMyEREAAREiNFZ4zd2I"
"dlQyERAAESI1Z9/////+hlQyEQABEjRt////////11MiEAERI17/////////hkMhABESPv/////////+dTIQAREjXv/+qqrP///4VCEAARI0bf2Imaqv//+G"
"QhAAESI0Vmd3iar///hkIQABESI0VVZnmq///4VCEAAREiM0RVZ5rv//51MhAAEREiM0Vnit///5ZDIQABERIjRFeK3///51QiEAEREiM0V4rv///oZDIRAB"
"ESIzRXiu///+h1QyEQEREjNGea////2XVDIhEBESM0Z53////JdkMyERERIjRnnv///qqHVDIhERESNGif///+qphlQyIRERI0aN////2qqodlMiEQESNX7/"
"///czMzMxUMhEBI0bv//////////1DIRASNG///////////2QyEQEjTf//////////9kIhEBIk3///////////UyERARI0V3iJmZmYiHZUMhEAESI0VWZmZm"
"ZmVUQyERABESIzNERERERDMyIREAABERIiIzMzMzIiIhERAAAAAAAREREiIiIiIhEREQAAAAARESIiMzMzMyIiEREAAAAREiMzREREREQzIhEQAAABEiNFVm"
"ZmZmZlRDIREAABESNFZ4iZmZmIdlQyEQAAESJF//////////9TIRAAARI03//////////lMhEAABEjTf/////////+UyEQAAESNO//////////dTIRAAARIk"
"V4mqqq7///lkMhEAABESNFeJqq3///p1QyEQAAERIjRXiq3///qGUyIRAAABESNFaK7///qXVDIREAAAERI0V57//+qpdUMhEQAAAREiRX7///2ql1QyERAA"
"ABESNFj/////2XVDIREAABERI0V//////+hkMhEQABERIjRd//////+XUyERAAEREiNFaJqu///+dTIREAERIiM0Vniarv//91MhEQERIjNEVWd5qt///4Uy"
"ERARIjRWZmd4mq3///dTIREREjRv2IiImqr///50MhEBESRe//2qqq3///+GQyEQESNG///////////XUyERARI0///////////nVDIREBESRe/////////X"
"ZDIREAERI0Z+/////+2HVDIhEQABEiNFZ4idmId2VDIhEQAAERIjRFZmZmZVQzIhEQAAABESIjNERERDMyIREQAAAAARESIiMzMyIiEREQAAAAAAERERIiIi"
"IREREQAAAAAAAAAAAAAAERERERERAAAAAAAAAAABERESIiEREQAAAAAAAAABERIiIzIiERAAAAAAAAABERIjNERDMhEQAAAAAAABERIzRWZlVDIRAAAAAAAB"
"ESIzVn///+MhEQAAAAABESI0Vo7//+UyERAAAAABESI0Vo7///ZTIREAAAABESI0Vo7///hkMhEQAAABESI0V47///l1QyERAAABESI0V47///qGVDIREAAB"
"ERI0Vo3///yXZUMiERABERI0Vo3///2phlQyIREAERIzVo3///2qmYZUMhEQERIjVo3///2qrd3WUzIREREjRor///6qrf//1kMhEREjRXr///6qqu//+XVD"
"IRESNXn///6qqq///6hlMhESJGj///+qqq3///qnVCIRI0f//////////////UMhEjV//////////////2QyESNd//////////////ZDIRI07///////////"
"//5kIRESNWeJmqqqrf//+qhlMhERIzRWZ3eJqt///ahlQyERESIzRFVWeK7//9hlQyERAREiIjM0RWj///hlMyERAAERESIiM0Vv//9lQyEREAABERESIiNF"
"ZnZlQyEREAAAABERERIjNEREMyEREAAAAAABERERIiMzIiEREAAAAAAAAAEREREiIREREAAAAAAAARERESIiIiEREREAAAAAEREiIiMzMiIiEREAAAABERIj"
"NEREREMzIhEQAAABESM0VWZmZmVUMyERAAAREjRWeIiIiId2UyIRAAERIjVv////////9DIRAAERI0bf////////5TIRAAESI1fv////////5TIRABESNGj/"
"////////1DIRABESNXn//+qqqpmGVDIRABEiRY3//6qqmIdlQyERABEjRo7//6qpiHZUMiERAREjRp///qqqmHZUMiERAREjV9///tyqqYZUMiERAREjV+//"
"/////phlMiERAREjV/////////12QyERAREjV//////////XUyERAREjXf/////////3VCERERIjRniaqqrP///4ZCIRERIjRWd4iaqq///9ZDIRERI0RWZ3"
"iJqq///9ZDIRESI0Vnd3iJqq///4ZCIRESNF3omZmaqu///4VCERESNW//7aqq3v///nUyERESNe//////////+GQyEQESPf//////////hlMhEQESNd////"
"/////oZTIhEQESNFfe/////+h2VDIREAERI0VniJzJiHZUMyEREAAREjNFVmZmZVRDIhERAAARESIzRERERDMiIREQAAABEREiIzMzIiIhEREAAAAAERERIi"
"IiIREREQAAAAAAAAARERESIiIiERERAAAAABEREiIjMzMiIhERAAABEREiIzREREQzIhEQAAEREiI0RVZmZmVDMhEQABESI0VWeInd2HZUMhEAERIjRWie//"
"////1TIRAREiNGee////////0yERESI0Z9/////////1QhEREjRn3/////////dTIRESNFff///+qqqZ3mQyEREjVp////qqqYh2ZDIhESNGjv//+qqpiHZV"
"QyEREjRp///9qqqph2VDMhERI1ff//+qrd2qh2UzIREjRo7//+3////9l1QyERI0af//////////hlMiESNXn///////////hkMhEjV9/////t3v///9ZDIR"
"I1ff///6qqqv///3UyESNX3///2qqqrf//+FMhEjR8///6qpmqr///hTIRI0af//+qqZqq///3UyESJGj///qqqqrv//51MhESNX3///qqqt///9ZDIREjRo"
"////7u////51QiEREjVo/////////4ZDIRERIkV4///////+hlMiERERI0Vo3////+12UyIREBERI0RmeIyYh2VDIhEQABERIjRFZmZlVEMiEREAABERIiM0"
"RERDMiIREQAAABEREiIjMyIiIRERAAAAABERERIiIhERERAAAAAAERERIiIiIiIiIREREAARESIiMzMzMzMyIiEREBERIjNERERERERDMyERAREjRFZmZmZ2"
"ZmZVQyERESNFZ4iZmZmZmYdlQyEREjXf///////////1MhESNG3///////////9kIREjV+///////////+ZCERI1f////////////WQhESNY///qqqqt///+"
"hTIREkXf//qqqqrv//+XUyERI13//6mZqs///9hkMhESNO//+Iiar///+XUyEREiNWd3d3mu///6hUMhEREjNFVmaJr///6GQyIRAREjNEVXiu///5dUMhEQ"
"ERIjNEZ53///2GQyERAAERIjNWiv///pZTIhEQABESI0V57///l1QyERAAAREiNGjf///YZDIhEQAAERIjV5///+llMiERAAABESNGjv//+XVDIREAAAARIj"
"V5///+hkMiERAAAAESNGj///+XUyIREAAAABEjRt///9dUMhEQAAAAARI1f///52QyIREAAAAAESNe///4ZDIhEQAAAAABEj3///1lMiERAAAAAAARIjRWZl"
"QyIREQAAAAAAERIjREQzIhERAAAAAAAAERIiMzIiEREAAAAAAAABERESIhEREQAAAAAAAAAAAAERERIiIiIREREQAAAAARERIiIzMzIiIRERAAAAERESIzRE"
"RERDMiEREAAAERIjRFZmZmZVQyIREQABESI0VniN3ZiHZUMhEQAREiNGeO/////9hlQiEQAREjRo7////////WUyEQERI0aO/////////8ZDIQERI1fv///9"
"3e////dTIQESJGj///6qqq3///hUIRESNGn///qqqqr///xkIRESNH3///qqqqr///hkIhESNXn///qqqqr///hkIhEiNXn///6qqt///+dTIREjRXrf////"
"/////pdTIREjRoqv////////6oZDIRIjV53/////////qoZDIRI0aN//////////6oVDIRI1ff///qqqrf///oZDIRI1fv//6qqqqt///4ZDIRJFj///qqqa"
"qq///4ZCIRJGj///qqmZqq///4ZCERJFj///qqqaqu///3UyERI1f///+qqqrf///nQyERI0bv///+7e////+GQiERIkV+//////////hlMhEREjRn7/////"
"///oZUIhEREiNGeO/////+x2VDIREBESI0VniJnJiHZUMyEREAERIjNFVmZmZVRDIhERAAARESIzRERERDMiIREQAAABEREiIjMzIiIhEREAAAAAEREREiIi"
"IhEREQAAAAAAARERESIiIhERERAAAAABEREiIjMzMiIRERAAAAEREiIzREREQzIhERAAARESI0RWZmZlVDMiERAAERIjRWeJ3diHZUMiERAREiNWfe/////o"
"dUMhEQESI1aO///////9dUMhEREjRo/////////9dTIhERI1fv///+3v///4ZDIREjRs///9qqqu///nUyERI1fv//6qqqqv//+GQyESNX///9qpmqr///x0"
"MhEjV////KqZqq///9dTIRI1f///2qqqrP///XUyESNX7///2qqq////11MhEjRt////7u7////9dTIRIkV+///////////HUyERI1aO//////////lkMhES"
"NFeN7////d///4ZDIREjNFeJqsqqr///2FQiERIzRWeJqqqt///5dTIRESNEVniJqqr///6GQyERIjRmeImqqv///5dTIRESNF7pmqqu////11QyEREjVv//"
"///////XZDIRERI17////////9dkMiERESPf///////+h2QyIREBEjTe/////+mGVDIhEQARIjVneNiIh2VEMiERAAERIjRVZmZlVEMiIREQAAERIjNERERD"
"MiIREQAAAAEREiIjMyIiIRERAAAAAAERERIiIhEREREAAAAAAAABERERERAAAAAREREREREAAAEREiIiIhEQABERIzRDMiERABESNFVVQyIRABEjRd//ZDIR"
"AREjXf//9UIRAREjX///9kIhAREkX///9kMhAREjXv//9UIRAREjRu/+ZDIRAREjRWZmVDIRAREiNEVUMyEREREiM0RDMiERERIiM0QzIhERERIjRERDIhER"
"ESI0VmZUMhERESNF3/9lMiEQESNd///1QyEQESNf///2QyEQESNf///2QyEQESNe///1QhEQESNF7/5kMhEQERI0VmVDIhEAERIjNEMyIREAAREiIiIiERAA"
"ABEREREREQAAAAEREREREAAAAAAAEREREREAAAAAERERERERAAAAEREiIiIhEQAAEREjNEMyIREAAREjRVVUMiEQABEjRd//ZDIRABESNd///1QhEAERI1//"
"//ZCIQAREkX///9kMhABESNe///1QhEAERI0bv/mQyEQAREjRWZmVDIRABESI0RVQzIREBERIjNEQzIhEQERIjNEQzIhERAREjNEVEMyEREREiNFZmVDIhER"
"ESNG3/9lQiERESJF3///ZDIRERI0b///9lMhEREjRv///3UyEQESNH7///ZDIRARI1ef//1kMhEBEjV5//51MhEQESNW7/+GQyERARI1b//WUyIREBEjTf/m"
"VDIREAESNP/2VDIREQAREjREQzIREQAAESIzMyIREQAAARESIiEREQAAAAEREREREQAAAAAAEREREAAAAAAAAAAAABEREREREREAAAAAABEREREREREREAAA"
"ARERERIiIiIiEREAABERESIiMzNDMyIRABERESIjNERVVV0yEQEREiIzRFVmd97+MhERESIzRVZ3je///UMhERIjRWd4nv////VCIREjNWeJ7//////lMhER"
"I1aN//////7XZDIREjRu/////+2YdkMiERI1f////9qph2VDIRESNd///qqqmHZUMiEREjXv///tqph2VDIhERI17/////6phlQyERESNFff/////tdlMhEQ"
"ESNFeN7/////5UIREBEiNFZ4nf////VCERAREiM0VmeN7//1QhEQARESIzRVZ3fv9DIRAAARESIjNEVVVUMiEQAAARERIiIzM0MyIREAAAARERESIiIiIhEQ"
"AAAAABERERERERERAAAAAAAAAREREREREAAAAAERESIiIiIiIiEREAAAERIiMzMzMzMyIhEQABESM0RFVVVVREMiERAREjRWZ3d3d3ZlQyIRARIkXu7u7u7u"
"7u5DIRARI03/////////9TIRERI17/////////9UIRERI17/////////5TIRERI13d3d3d3d3dZDIRESI0Z4iZmZmZh2VDIRESM1Z3iJmZmId1QyIRESNFZ4"
"mZmZmYh1QyERESNd3d3d3d3d3dQyERESRf/////////+UyEREiRv/////////9QyERESTf/////////2QyEQESNN3d3d3d3d3VMhEQESI0Vmd3d3dmVDMhEA"
"ERIjRERVVVREQyIREAAREiIzMzMzMzIiERAAAREREiIiIiIiEREQAAAAABEREREREREAAAAAAAEREREREREREQAAAAABESIiIiIhERERAAAAERIzREMzIiIR"
"ERAAABEjTVVVVEMyIhERAAARI1/+h3ZVQzIhERABESRf///Yd2VDMiERARIk3////+mHZUMiEQESJF7/////7YdUMhEBEiNWje/////9dUIhERIjRWia3///"
"//1DIRESI0Vniqre///9UyEREiNFZ4qqrf//91MhERI0Vniazv////ZDIREjRWeJ7//////lQyERI1Z+//////7XZDIREiRf/////+2YdUMiERI03////9mH"
"ZVQyIRESNN///Yh2ZUQzIhEQEiTv53dmVEMzIhERABEjxVVVRDMyIhEREAARIjMzMzIiIhEREQAAEREiIiIiEREREQAAAAEREREREREREQAAAAAAERERERER"
"EAAAAAAAAAAREREiIiIhEREQAAARERIiIzMzIiIREQAAERIiM0REREMyIREQAREiNEVmZmZlRDIhERESI0VniN3Zh2VDIRERIjRn3/////7XVDIRESNG3///"
"/////nVCIREjXv/////////WQyERI+/////v////91MhESNe//6qqq7///hTIREjRt+IiJqq///4ZCEREjRWZ3eJqv//+FMhERIjRFVmia3//+dTIQERIjNF"
"Z4rP//+GQyEBERIjRWed///9dTIhABESI0V47///l1QyEQAREiNGj///6XVDIREAERIjVu///odUMiERABESI13//+h1QyIREAAREiRe//+GVDIhEQAAERI0"
"VoiHZUMiEREAABESNGeIdlQyIREQAAARIkV97XZUMiERAAAAESNG///2UyIREAAAABEjXf///VMiERAAAAARI17///1TIREAAAAAESNN///2QyERAAAAABEi"
"Re//1TIREAAAAAAREjRVZUQyERAAAAAAAREjNEQyIREAAAAAAAEREiIiIhERAAAAAAAAEREREREREAAAAAAAAAAREREREAAAAAAAAAAAAAAREREREiIiIhER"
"EREAAAAAAAAAARERESIiIjMyIiIhEREAAAAAAAABEREiIzNEREREMzIiEREQAAAAABEREiIzRFVmZmZVVEMiIREQAAAAERESI0RWZ3iN3Yh3ZUQyIREQAAAR"
"ESIzRWeN7//////tdlQyIREAABERIjRWff//////////12QyIREAAREiNFeP/////u7v////52QyIREBESI0Z9///+qqqqqqrv//92QyEREREjRn7//+qqqZ"
"mZmqqu//51QiERESNFfv//qqqqqpmZqqqu//1kMhERIjV9//6qqqqtyqqqqqqu//dUIRESNGj//6qqrv///9rv/+qv/8ZDIRIkV+//qqrf/////97//qrv/m"
"QyESNGj//qqt//////////2q3/51MhEjV+//qqr///7d3////6qs//dTISJGj//qqu///aqqrv//+qqv/3UyEjRt//2qr//9qpmqr//+qqr/91MhI1ff/6qt"
"//+qmImq///qqt//dTISNX7/+qrv/+qYd4qv//2qrf/nUyEjV//+qq7//qmHiar//6qq7/1kMhI1f//qqu//6qiImu//+qqv/4ZDISNX//6qrv//qqmqr//+"
"qq3/91MhEjV+//qq3//+qqqu///qqv/9ZDIRI1fv/6qq////7e/////d//51QiESNG3/+qqt/////////////3ZDIREjRo//6qqu/////+r/////hkMhEREj"
"V+//qqqt7//+2qrv//12UyIREBI0bP//qqqqqqqqqZmZh3VDIhEQASJFfv//qqqqqqqZiHd2VUMiEREAESNGfv//6qqqqqmYdmVEMyIREQABEiNGfv///u3d"
"3e/+ZUMyIRERAAAREiNGff/////////1QyIhERAAAAAREiNFeO////////1DIhEREAAAAAEREiNFZ3je7u7ddlQyEREQAAAAAAEREiM0VWZnd2ZlQzIREQAA"
"AAAAAAERESIzNERVVEQzIhERAAAAAAAAAAARERIiIzMzMyIhEREAAAAAAAAAAAARERESIiIiIREREAAAAAAAAAAAAAAAAAAAARERERERERAAAAAAAAAAAAAB"
"EREiIiIRERAAAAAAAAAAAAEREiMzMzIhERAAAAAAAAAAARESM0VVVDMhERAAAAAAAAABERI0Vnd3ZUMhEQAAAAAAAAAREjRX3///dUIhEQAAAAAAABESI1eP"
"///9ZDIREAAAAAAAERIjRo7////nVCIRAAAAAAARESNGjf////+GQyERAAAAAAERI0V5//////l1MhEQAAAAAREiNWjv//7//9hUIhEAAAAAERI0aN///q//"
"/oZDIREAAAAREjRXn///qv//+XQyERAAABESI1aO///arv//p1MiEQAAAREjRor//+qq3//9hUIhEAABESNFef//+qqq///oZDIRAAERIjRo7//+qqqv//+X"
"QyEQABESNGiv//+qqqrv//p1MiEAERIjV5///9qqqq3//9hUIhABESNGjf///////////oZDIQERIkV5////////////+WQyEBESNGjv////////////l1Mh"
"ARIkV9/////////////9dTIRESNGj///2qqqqqqt///nUyEREjV+///qmHd3eJqv//91MhERI13///mHZVVVZ4n///dTIRESNf///XZUREREVo7//9UyEREj"
"7///dUQzMzM0Vu///kMhERI0VmZlQzIiIiIzRWZlQyEQERIzREQzIiIREiIjNERDIhEAERIiMyIiERERERIiIzIiERABERESIRERERERERESIhEREAAAARER"
"EiIiIiIiEREREAAAABERIiIjMzMzMiIhEREAAAERIiM0REREREQzMiEREAAREiNEVmZmZmZmVUMyEREAERI0VniImZmYiHdlQyERAREjRd/////////th1Qy"
"EQERI1fv//////////51QiEBEiRo////////////5kMhERI0af////////////dTIRESNXr///qqqqqu///9ZCIRIkWN///qqqqqqv///WQiESNGjv//2qqZ"
"qqr///hkIhEjRp///8qqqqqt///4ZCIRI1ev//+qqqqqz///11MiEiNXr///7u7u7////pdTIRI0aN///////////9qGQyESNGnv//////////+qhkMhEjRp"
"/////////////IZDIRI1ev///aqqqq3///+GQyEiNXr///qqqqqqr///lkMhIkWN///qqpmZqq///9ZDISJGjv//2qmYmaqv///WQyEjRp///8qqmZqq3///"
"hkIhI0af//+qqqqqrv///3UyESNGn/////////////hkMhEjRt////////////+GUyIRIkbv///////////oZUMhERI1/////////+7YdlQyERASNFZ4iIiI"
"iIh3ZlQzIREQESM0VWZmZmZlVUQzIhERABESIzRERERERDMzIhEREAARESIiIzMzMyIiIhEREQAAAAAAABERESIiIiIhEREQAAAAAAERESIiMzMzMiIhERAA"
"AAABERIiM0RERERDMiEREAAAARESIzRVZmd2ZVQzIREQAAERIjNFZ4id3diHZUMhEQABESI0Vnje/////+11QyEQAREiNFeN/////////nUyEREREjRXj///"
"////////ZCERERI0V5////////////5CIRESI1ef////2qqq3//+VCERESNGj////qmId3iO/mQyEREiNX3///2odmVVZm1UMiEQEjRo///+qHVURERERDIh"
"EQEjV9///6h1RDMzMzMyIREAIkWO///qdkQzIiIiIhEREAI0aP///ZdUMyIiIiERERAAI0af//+oZUMyIiIhERERAAI0af//+oZUMyIiIhEREQAAI0af//+o"
"dUMzIiIiIREQAAI0af//+pdlQzMzMzIhERAAIkaP///ah1VEREREMiERAAEjWO///6qHZmZmbVQyIRAAEjV9////qpiIiI7/ZDIRAAEjRo/////tqq3v//5T"
"IREAESNWj////////////UIREAESNFfP//////////5TIREAERI0V47////////9ZDIRAAERIjRWie/////+12QyERAAAREiM0VniJzJiHZUMiERAAABERIj"
"RFZmZmZVRDIhEQAAAAEREiIzRERERDMiIREQAAAAARERIiIzMzMiIhEREAAAAAABERERIiIiIhEREQAAAAAAAAERERIiIiIiIRERERAAAAAAEREiIjMzMzMi"
"IiEREQAAAAERIjM0REREREMzIiEREAAAERIjRVZmZmZmZVRDIiERAAAREjRWeImZmYiHdlRDIhEQAREjRd////////7YdlQyIREBESNX7//////////odkMi"
"EQESJFj////////////XZDIRERI0af////////////11QiEREjV5///6qqrN7////4ZDIREiRX3//+qpiImq7///51MhESNGjv//2od2d4mu///4ZDIRI0af"
"///ZdlVWaJr///10MhEjV5///6hlREVXiv///XUyEiNXr///p2VERFaK7//+dTISJFjf//6XVEREVorv//11MhI0aO///ZdUQ0RWiv///XUyEjRp///9hlRE"
"RFaK///8dDISNXn///qGVERFZ53///hkMiI1ev//+oZVRFV4r///6FQiIkWN///qh2VVZ4rv///HUyEjRo7//9qYd3eIrf///oZDISNGj///2qmZmarv///4"
"ZTIRI0af///d3d3d7////8dUIhEjRp/////////////9dUMhESNG3////////////odUMiERIkbv//////////7YZUMiERASNf////////7tiHZUMiERABI0"
"VniIiYiIiHdlVDIiERAAESNEVWZmZmZlVUQzIhERAAAREiM0REREREQzMiIRERAAABERIiIjMzMzIiIiERERAAAAAAABERESIiIiIiIREREAAAARESIiMzMz"
"MyIiIREQAAERIjM0REREREQzMiERABESI0VWZmZmZmVVQyIRABESNFZ4iZmZiIh3ZDIRAREjRd///////////0MhAREjV+///////////kMhARIkaP//////"
"/////UMhERI0af//////////9kMhERI1ev//+qqqqpmHZTIRESJFjf//6qqpmIdmVDIRESNGjv//2qqYh3ZVQyIRESNGn///yqqZh3ZUMiERESNXr///qqqq"
"mIdUMiEREiRYr///7u7u7u7UMhEQEjRo3//////////VMhEQEjRp7/////////9lMhEQEjR5//////////9kMhEQEjV6///9zMzMzMxUIhEQIjV6///6qqmY"
"h2VDIREAIkWN///qqpiHdlVDIREAI0aO///aqZiHdlVDIREAI0af///KqqmYh2VDIhEAI0af///aqqqqmYZUIhEAI0af///////////UMhEAI0bf////////"
"///EMhEAIkbv//////////9kIhEAEjX///////////9TIREAEjRWeIiZmIiId2VDIRAAESNEVWZmZmZlVUMyERAAERIjNEREREREMzIhEQAAEREiIiMzMzMi"
"IiEREQAAAAABERESIiIiIiIREREAAAARESIiMzMzMyIiIREQAAERIjM0REREREQzMiERABESI0VWZmZmZmVVQyIRABESNFZ4iZmYiIh3ZDIRAREjRd//////"
"/////0MhAREjV+///////////kMhARIkaP///////////UMhERI0af//////////9kMhERI1ev//+qqqqZiHZTIRESJFjf//6qqYh3dlQzIRESNGjv//2qmH"
"dmVUMyERESNGn///2ph3ZlVDMiERESNXr///qqmIdmVDIhEREiRYr///qqqpiHZDIhEQEjRo3///7u7u7u5UMhEQEjRp7/////////9kMhEAEjRp////////"
"//9UIhEAEjV5//////////9TIREAIjV6///+3d3d3d1DIRAAIkWN///qqZiHdlQyERAAIkaO///al3ZVVEMiERAAI0aP///ZdlRDMyIhEQAAI0aP//+oZUMy"
"IiEREAAAI0aP//+WVDIiERERAAAAIkbf//51QyIhEREAAAAAEkXv//5lMiEREQAAAAAAEjT///1UMhEREAAAAAAAEiNFZlVDIREQAAAAAAAAESI0REMiEREA"
"AAAAAAAAERIiMyIhERAAAAAAAAAAARERIhEREQAAAAAAAAAAAAAAABERESIiIiIhERERAAAAAAERESIiMzMzMiIhEREAAAABERIiM0RERURDMiIREQAAARES"
"IzRVZmd2ZlRDIhEQAAERIjNFZ4id3dmHZUMiERABESI0Vnje//////12UyERAREiNFaN/////////9ZDIREREjRXj///////////5TIRERI0V5//////////"
"///TIRESI1ef////7aqq3v//1TIRESNGj////qmId4iJ/9ZDIREiNX3///6odmZmZndmUyIREjRo///+qHZVRFVVVVQyEREjV9///6l2VERERVVUMyERIkWO"
"///qhlRDNEVVVUMyERI0aP///ZdUQzNEVndlQyERI0af//+odUQzRFb//+UyERI0af//+oZUQ0RWj//9UyERI0af//+pdURERX3//8VCERI0af//+pdlREVo"
"7//4VCERIkaP///ah2VVZ5///4VCEREjWO///6qHd3ia///nUyEREjV9////qpmJmq///XUyEREjRo/////tqqrf///GQyERESNWj///////////+FQiEQES"
"JFef//////////9lMhEQERI0V47////////9dUMhEQEREjRWee/////+2GVDIREAAREiM0VniJzJiHZlQyEREAABERIjRFZmZmZlRDIhERAAAAEREiIzRERE"
"RDMiIREQAAAAARERIiIzMzMiIhEREAAAAAABERERIiIiIhERERAAAAAAAAEREREREREAERERERERAAAAERESIhERERERERIiEREQAAEREiIzIiERERESIzMi"
"IREAEREjNERDMiEREiM0REMiEQAREjRWZmVDIiIiNFZmVDIhAREiRd///lMiIiNF7//+QyEBESNG7//9ZDIiNFf///1TIQESJFf///1lMzM0aP//91MhERI0"
"aP//+XVDM0V5///4VCEREjV5///5dlREVo3//+hUIREiRY3//+qGVVVnnv//51MhESNGjv//2pdmZnmv///XUyERI0af///aqYiImq///5dTIREjV5///6qq"
"qqqqr///l0MhEiNXr/////////////6WQyESNGjf/////////////oZDIRI0aO/////////////9hUIhEjRp//////////////p1MiESNXn///7d3d3d3v//"
"+XUyESI1ev//+qqpmqqt///pZDIRIkWN///qmHd4iq7//+hkMhEiRo7//9mGZmZ5r///11QiESNGj///2HVUVWiv//+XUyERI0aP//+XZERFaK///5ZDIREj"
"Ro///4ZUMzRX3//+hUIhESJG3//+dUMzNFfv//51MhEQEkXv//5kMyIjRv///WQyERASNP///VQyIiNF///2QyERABIjRWZVQyIRIjRWZlQyIREAESI0REMi"
"IRESIzREQyIREAAREiIjIiERERESIjIiIREAAAERESIRERERERERIhEREAAAAAABEREREREAAAARERIiEREQAAEREiIzIiERABERIzREQyIRABESNFZmVDIh"
"AREiRd///kMhAREjRu///VMhARIkV////WQhERI0aP//+GQiERI1ef//+GQiESI1ff//6FQiESNGjv//11MiESNGj///11MhESNXn///l1MhEiNXr///lkMh"
"EiRY3//+hkMhEjRo7//9hkMhEjRp///9dTIhEjV5///5dTIhIjV5///5ZDIRIjV9///oZDIRIkWO///YVCIRIkaP///XUyIRI0aP//+WQyERI0aP//+GQyER"
"Ikbf//51MhEQEkXv//5kMhEQEjT///1TIREQEiNFZlQzIREAESI0REMiERAAERIiIyIhERAAARERIhEREQAAAAAAABERESIiIiERERAAAAAAEREiIiMzIiIh"
"ERAAAAAREiMzREREQzMiERAAAAESI0VWZmZmVUMhEQAAAREjRneIiZiHZUMhEAAAESNF////////9TIRAAABEjTf///////+ZDIQAAARI17////////mQyEA"
"AAESNP////////10MhAAABEiRWiaqq///5dDIQAAAREjRWeJqv//+WQyEAAAARIjRVeK3//+hkMhAAAAERIjRGee///oZDIQAAABERIjRXnv//11QiEAAAAR"
"ESI0V6///6dTIRAAAAEREiNGiv//+XQyEQAAARESIjRo3//+hkMhEAABEREiM1aN///oZDIRAAEREiIzRXnv//11MiEQAREiIzNEaK///5dTIREBESI0REVX"
"iv//+GQyERERI0RWZmeK3//+hUIhEBEiNW/oiJqv///XUyERARI0b//qqq7///hkMhEQESNf/////////XUyIRABEjX////////+dkMhEQARI0b///////6G"
"QyIRAAESJFbe////7XZDIhEQABESNFZ4jIiHZUMiERAAABESM0VWZmZUQyIREQAAARESIzREREMyIhERAAAAARERIiIzMiIhEREAAAAAARERESIiIREREAAA"
"AAAAAAEREREREREREREREREQAAAAARERIiIRERERESIiIREQAAABERIiMzIiIiIiIzMyIhEQAAEREjNEREMiIiMzRERDIhEAABESNFZmZUMzM0RWZmVDIREA"
"ERIkXf//5URERWfv///SIRABESNG7//9ZVVWeP///9QiEQARIkV////HZmeJ////dTIREBESNGn///qHd4r///92QyEQAREjV5///6mZnf///oZTIhEAESJF"
"jf//6qqt///+hlQyERABEjRo7//9qq7///6HVDIREAARI0af///Krv///YdUMiERAAEiNXr///qu///9l1QyIREAABIkWK///6////yoZTMhERAAASNGjf//"
"/////Kl1QyIREAAAEjRp7///////6pdUMiERAAABI0ef////////qXVDIhEQAAASNXr/////////qGQyIREAAAIjV6/////d///9l1MiERAAACJFjf///9qu"
"///6dUMhEQAAAiRo7///qqqv///pZTIREAAAI0aP///aqZrf///XVCIRAAACNGj///qYd4rv//+GQyERAAAjRo///5hmZ4r///51MhEQAAIkbf//6GVVVo3/"
"//1UIREAABJF7//9dUREVo7///VCERAAASNP///FQzM0Vn///+MhEQAAEiNFZmVDMiIzRWZmVDIRAAABEiNERDMiIiIjNERDMhEQAAAREiIyIiIRERIiIzIi"
"ERAAAAARERIhERERERERIiEREQAAAAAAARERERERAAAAAAAAABEREiIRERAAAAAAAAEREiIzIiERAAAAAAARESM0REMiERAAAAAAERI0VmZUMiEQAAAAAREi"
"Rd///kMhEQAAAAERI0bv//1TIREAAAABEiRX///9ZCERAAAAERI0aP//+GQiEQAAABESNXn///hkIhEAAAARIjV9///oVCIRAAAAESNGjv//11MiEQAAABEj"
"Ro///9dTIhEAAAARI1ef//+XUyIRAAAAEiNXr///lkMhEQAAABIkWN///pZDIhEQAAASNGjv//2GQyIREAAAEjRp///9hkMiEREAABI1ef//+oZDMiEREAAi"
"NXr///qGVDMiEREAIkWN///qhlRDMiIRECNGjv//6pdmVUQyIREjRo///9qpiHZlQyIRI0af///czMzMzMQyESNGn//////////9QiEjRt///////////UMh"
"Ikbv//////////ZCERI1///////////1MhESNFZ4iImIiId2VDIRESNEVWZmZmZVVDMhERESIzREREREQzMiERARESIiIzMzMiIiEREQAAABEREREREQAAAA"
"AREREREREAAAARERIiIRERAAAAERERIiIREQAAEREiMzMiERERERERIiMzIiERABERIzREQzIhEREREiM0REQyIRABESNFZmZUMiERERIjRWZmVDIRAREjRd"
"//91QyIRESI0V9///UMhAREjV+///XVDIiIiNFef//9lMhARIkaP///4ZTMiIjRXn///91MhERI0af///9dkMzM0V57///51MhERI1ev///+l1QzNFed////"
"51MhESJFjf////qGVERXjf////11MhESNGjv////2XZVV4r/////p1MhESNGn/////+odmeK//////l1MhESNXr//////Zh3iu/////+lkMhEiNYr//+///q"
"mJrf//7//+hkMhEiRo3//97//6qqr//+3//9hkIhEjRo7//6r//9qq///q7//6dTIhEjRp///6rv//qu//+q///5dTIREjV5///qrf//3v//2q///pZDIREj"
"V6///qqv/////9qq///oZDIRIjV9///aqt/////qqt///YVCIRIkWO//+qmq////+qqu//+XUyERIkaP//+piK7///qpmv//+WQyERIkaP//6Yd4z//9mImv"
"//+GQyERIkaP//6HZnj//od3iv//51MhEQIkbf//12VVZ912ZWff//1kMhEQEjXv//dlRERVVURFbv//dUIhEQEjT///ZUMzMzMzM0X///ZDIREAEiNFZlQz"
"IiIiIiIzRWZlQyIREAESI0RDMiIRERESIiM0RDMiERAAERIiIiIhERERERESIiIiIREQAAARERERERERERERERERIREREAAAAAABEREREREREREREREREQAA"
"ABEREiIiERERERESIhEREAABERIjMzIiERERIiIzIiERABERI0RVRDIiISIjNERDIhEAERI0VndmVDIiIjRWZlQyIQERI0Xf//xlQzIzRd///kMhAREjV+//"
"/4ZUMzRX7//9UyEBEjRo////2GVERWj///dTIRESNGn////6hlRVef//+FQhERI1ev////6XZVaN///oVCERIkWN/////6l2Z53//+dTIREjRo7/////+ph4"
"rv//11MhESNGn//////9qYmv//+XUyERI1ev//////+qqq///5ZDIRIjV6///6///+qq3//+lkMhEiRo3//+rv//+qrf//6GQyESNGjv//2q///+qu///YVC"
"IRI0af//+qrf///a///6dTIhEjV5///6qq7///r///l1MhEiNXr///qqqv//////6WQyESI1ff//6pia7//////oZDIRIkWO///Zh4mv/////9hUIhEiRo//"
"/6h2aJ3/////p1MhESNGj///l2VWiv////+WQyERI0aP//+GVEVor////oZDIREiRt///nZERFeO///+dTIREBJF7//9ZUMzRXj///1kMhEQEjT///ZUMiM0"
"Vt//9lMhEQASI0VmVUMiIiNFZnZlQyERABEiNERDMiESIjNEREMyERAAERIiIiIhERERIiMzIiEREAABERESEREREREREiIREREAAAAAAAAREREiIiIiIRER"
"EQAAAAAAARERIiIzMzMyIiEREQAAAAABERIiM0REVURDMyIREQAAAAEREiM0VWZ3dmZVQzIhEQAAAREiM0VniJ3d2Yd2VDIhEQABESI0Vnjf/////+2GVDIh"
"EQERIjRXjf////////+HVDIRERESNFef///////////XVDIRERI0V8////////////+WUyEREiNXn////9qqqu////+GQyERI0aP///+qYiImq////11MhEi"
"NX3///2odmZniq///+dTIRI0aP///qh2VVVniu///4ZCISNX3///qHVUREVnmv//+GQyIkWO///qdlQzNEV5r///lkMiNGj///2XVEMzNFaK///5ZDIjRp//"
"/6hlQzMzRXmv//+GQyI0af//+oZUMzNEV53///hkIiNGn///qHVEMzRWiu///nUyEjRp///6l2VERFZ5r///x1MhIkaP///ah2VVVniv///4ZDIRI1jv//+p"
"h2Znea7///11MhESNX3///+qmIiZrv///oZDIREjRo/////sqq3v////hlMiEREjVp////////////+HVCIRERI0V9///////////odUMhEQERI0V47/////"
"///9hlQyEREBESI0Vo3v/////th2VDIREQABESI0VWeInMmId2VDIhERAAABERIjRFZmZmZlVDMiEREAAAABERIjM0REREQzMiIREQAAAAABEREiIjMzMyIi"
"ERERAAAAAAABERERIiIiIhERERAAAAAAAAABERESIiIiIhERERAAAAABERIiIzMzMyIiIRERAAABESIzNEREREQzMiIREQABESI0VWZmZmZlVEMiEREAERI0"
"VniJmZmIh2VDMhEQERI0Xf///////uh2VDIRAREjV+/////////+hlMiEBEiRY///////////4ZDIRESNGn///////////+FQiERI1ef//+qqq3v///9ZDIR"
"IkWN///qqqqqr///51MhEjRo7//9qYiJqt///3UyESNGn///2ph3ia3///hTIRI1ef//+qh3eJrf//51MhIjV6///6qYiJqu///nUyEjRo3//+qqmZqt///9"
"ZDISNGnv//2qqqqt////hUIhI0af///+7u7v////xlMhEjV6////////////11QiEiNXr///////////11QyESJFjf/////////9h1QyERIkaO///+7u3d2I"
"dlQyIREjRo///9qpmId2VUMyERECNGn///qYdmVVRDMiEREAI0aP//+YZVRDMyIhEREAAiRt///oZUMzIiIREREAABJF7//+dUMiIhERERAAAAEjT///1UMi"
"IREREQAAAAASI1ZmZUMiERERAAAAAAABEiNERDMiEREQAAAAAAAAERIiMyIhEREAAAAAAAAAABEREiERERAAAAAAAAAAAAAAAAAREREiIiIiIREREQAAAAAA"
"ARERIiIzMzMyIiEREQAAAAABERIiM0REVURDMyIREQAAAAEREiM0VWZ3dmZVQzIhEQAAAREiM0VniJ3d2Yd2VDIhEQABESI0Vnjf/////+2GVDIhEQERIjRX"
"jf////////+HVDIRERESNFef///////////XVDIRERI0V8////////////+WUyEREiNXn////9qqqu////+GQyERI0aP///+qYiImq////11MhEiNX3///2o"
"dmZniq///+dTIRI0aP///qh2VVVniu///4ZCISNX3///qHVUREVnmv//+GQyIkWO///qdlQzNEV5r///lkMiNGj///2XVEMzNFaK///5ZDIjRp///6hlQzMz"
"RXmv//+GQyI0af//+oZUMzNEV53///hkIiNGn///qXVEM0RWiu///nUyEjRp///6l2VERFZ5r///x1MhIkaP///ah2VVVnmv///4ZDIRI1jv//+qh3d3ia7/"
"//11MiESNX3///+qmZmarv///oZDIREjRo/////sqq3v////l1QyEREjV5////////////+oZDIhERI0V9///////////ql1QyERERI0V57////////9qoZT"
"IhERESI0V43v/////tqqmGVCIREBESI0Vnmt////yqqt91QiEQABESIzRWie////7u//5UIhEAABERIjRWee////////xCERAAABERIjNFeN///////lMhEQ"
"AAABERIiNFZ53////tZDIRAAAAABEREiM0VniIiHZUMhEQAAAAAAEREiI0RVZmZUQyIRAAAAAAAAERESIjM0REMyIhEQAAAAAAAAARERIiIiIiIhERAAAAAA"
"AAAAABEREREhEREREAAAAAABERESIiIiIhERERAAAAABERIiIzMzMyIiIRERAAABESIzNEREREQzMiIREQABESI0VWZmZmZlVEMiEREAERI0VniJmZmIh2VD"
"MhEQERI0Xf///////+x2VDIRAREjV+//////////hlQiEBEiRY///////////8ZDIRESNGn///////////+GQiERI1ef//+qqq3v///9ZDIRIkWN///qqqqq"
"r///51MhEjRo7//+qZiJqt///4UyESNGn///2ph3ia3///hTIRI1ef//+qmHeJrf//+FMhIjV6///6qYiJqu///nUyEjRo3//+qqmaqs///9dDISNGnv//2q"
"qqqt////hkIhI0af///+7u7v////l1MhEjV6////////////2GQyEiNXr///////////qGUyISJFjf/////////9qXVDIRIkaO///93d///+qoZDIhEjRo//"
"/9qqqv//+pdUMhESNGn///qpma7//+l1MiERI0aP//+od4mv///XUyIRAiRt///pdmZ43///dTIREBJF7//+dlRVeP///VMhEQEjT///1kQ0RW3///QyEQAS"
"I1ZmZUMzM0RWZlQyIRABEiNERDMiIiM0REQyIRAAERIiMyIiIiIiIjMiIREAABEREiIRERERESIhEREAAAAAAAAREREiIiIiERERAAAAAAERESIiMzMzIiIR"
"EQAAAAEREiIzRERERDMiIREAAAEREiNEVWZmZmVEMiEQAAAREjNFZ4id3ch2VDIREAAREjRWeO//////7VQyEQAREiNWjv////////9DIRABESNGj///////"
"///1MhEAESJFj/////7v///9UyEQARI0bf///qqqqZ3/ZDIRAREjV////aqpiIh3ZTIhEBESNX///6qph3dmVEMhEQERI1j///2qqYdmVEMiERAREjV////9"
"qqmHZUMyIREBESNX7////+2qh2VDIhEQERI0aP///////ZhlMiERAREiRXn////////XZDIREBESI0Z53v//////51MhEQERIjRWeJre/////4ZCIRAREjNF"
"Vniaqt////1kMhEREiNFVmZ4iaqv///mQyERESNFZnd3iJqq7//9ZDIRERJF39iIiZmqr///1kIhERI0b//9qqqq3///91QhEREjTf///////////WQyERES"
"NP///////////XVCIRARI0bv/////////XVDIREBESNWfe/////+2GVDIhEQERIjRWeInMmIdmVDIhEQABESI0RVZmZmZUQzIhEQAAAREiIzRERERDMiIREQ"
"AAABEREiIjMzMyIiEREQAAAAARERESIiIiIREREQAAAAABERESIiIiIiIiERERAAARESIiMzMzMzMyIiEREAERIjM0REREREREQzIhEQESI0VWZmZ3dmZmVU"
"MiERESNFZ4iZmZmZmId2UyEREiRf////////////9DIREjRv////////////5TIREjTf////////////1TIREjTv////////////xDIREiRWiaqu///qqph2"
"UyERESNFZ5qu///amHZUMyERESI0Vnmv///ZhlVDIhEQERIjRXmv//+oZUMyIREAAREjNWjP//+nVDMiEREAAREiNGjf//6XVDIhERAAAREiNWnv//2GQyIR"
"EQAAAREiNXn///2FQyEREAAAABEiNXn///l1MiERAAAAABEiNXz///lkMhEQAAAAABEiRY3//+hkMhEQAAAAABEjRo7//9hUIhEAAAAAABEjRo///9dTIhEA"
"AAAAAREjRp///5ZDIREAAAAAAREjRs///4ZDIREAAAAAABEjRt///nVCIRAAAAAAABEjRu///WUyERAAAAAAABEjRv///VQyERAAAAAAABESNf//9kMhEQAA"
"AAAAABESNFZmVDIhEQAAAAAAAAERIzREQyIREAAAAAAAAAEREiIiIiERAAAAAAAAAAAREREhEREQAAAAAAAAAAARERERERERERERERERAAABEREiIRERERER"
"EiIRERAAERIiMyIiEREREiIzIiERAREiNEREMiEREiM0REMiERESI0VmZUMiESI0VmZVMiEREjRv//9UMiIjRd///0MhESJFf///ZDIiI0bf//5TIREjRt//"
"/3UzIjRX7//9ZCIRI1fv//51QyM0aP//+GQiEjRo7//9dUMzNXn///hkIhI0aP//+XVDM0V9///4ZCISNXn///l1QzNGjf//6FQiIjV9///5ZUM0Vo7//9dT"
"IiJFjf//6GVDNFef//+XUyEjRo7//9hkQzRXn///lkMhI0af//+oZEREaN///oZDISNXn///qGVERWjf//6GQyEjV9///6dlREV57//9dTIhI1ff//6oZURW"
"iv//+XUyESNX7//+qGVVZ5r///hkMhEjV+///ql2Zniu///nVCIRI1fv//+qh3eJr///l1MhESNG3///2qmZqv///oZDIREiRo////7Kqt////llMhEQEjV+"
"///////////XVCIREBI0aO/////////+dkMhEQARI0aO////////12QyIREAESI0Z57////+2HVDIhEQABESI0VniJ2Zh3ZUMiERAAARESI0RWZmZmVUMyIR"
"EQAAARESIjNERERDMyIRERAAAAARERIiIzMzIiIRERAAAAAAABERESIiIiEREREAAAAAABEREREREQAAABEREREREQABEREiIRERERERERESIhEREBESIjMy"
"IhERERERIiMyIhEQESI0REQyIRERESIzREQzIRERI0VmZVMyERESI0VmZVMiERIkX///ZTIiIiI0V////TIREjRv///WQyIiI0V+///lMhESNG7//+dUMiM0"
"V9///3UyERI1fv//6GQzM0Vo///9ZDIREjR9///5dUQ0Vo7//+dUIhESNGn///p2VEVnn///llMhERI0aP///YZVVnn///11QyEREiRo7//+l2Znnf//+GUy"
"IRASJFff//+od3iv//+XVDIREBIjV5///6mIiu///oZDIhEAESNGn///2pmt///5dTIhEQARI0aO///qqq///9hUMhEQABEjRo3///qq7//+hkMiERAAESI1"
"ev//+q3///l1QyERAAAREjV5///8r///2GQyEREAABESNGnv//3v//6XUyIREAAAARI0aN//////+oVDIREAAAABEiNX3//////oZDIhEQAAAAERI1ef////"
"/5dUMhEQAAAAAREjRo/////9hkMhEQAAAAAAESI1fv///+hkMiERAAAAAAAREjRt////h1MiERAAAAAAAAESI1b///1lQyERAAAAAAAAAREjRWd3dlQyEREA"
"AAAAAAAAERIzRVVEMyEREAAAAAAAAAARESIzMzIiEREAAAAAAAAAAAERESIiIhEREAAAAAAAAAARERERERERERERERERERERERERERAAARERIiERERERESIi"
"IRERERERIiEREQAREiIzIiIRERIiMzMyIhEREiIjIiIREBEiNERDMiIiIjRFVUMyIiIiM0REMiEQEiNFZmVDIiIzRWd3ZUMiIjNFZmVDIRESNO///lQzMzRn"
"///3VDMjNF3///QiERI13//+ZUMzRn3///1lQzNGf///1DIRIkbf//+GVERXn////XZERFff//5kMhEiRo///4dVVXnv///+hlRVeP///GQyESNGj///l2VW"
"iv////6XZVaN///nVCIRI0aP//+odmie/////6hmaJ///5ZTIREjRo///9l3ea//////qHd53//+hkMhESNGjv//2Yia7/////+piJr///l1MiEQIkWN///q"
"mar//+3//6qZrv//6GQyERAiRY3//+qqrv//rf//2qqv//+XUyIRECI1fP//+qrP//6q///aqu///oZDIREAEjV6///6qu//+qr//+qq///5dTIhEQASNXn/"
"//qt///aqv//6q7//9hkMhEQABI0ef//+q///6qq///6r///l1MiERAAEjRp7//93//9qqrv//rv//2GQyERAAASNGjv//3///qZmu///f//+XUyIREAABIk"
"aN//////2YiK3//////YZDIREAAAEiNY3/////+od3nf/////5dTIhEQAAARI1ev/////ZdmeK/////9hkMhEQAAABEjRp/////pdlVnn/////llMiERAAAA"
"ESNGj////6hlRFaP////11QyERAAAAAREjV////+hlQ0RX////6GQyERAAAAABESNG7///dlQzM0bv//92QyIREAAAAAAREjRWd3ZUMyIjNFZ3dlQyIREAAA"
"AAABESI0RVVEMiIiIjRFVUQyIREAAAAAAAAREiIzMzIiERESIjMzMiIREQAAAAAAAAERESIiIhERERESIiIiEREQAAAAAAAAAAABEREREREREREREREREAAA"
"AAARERIiIhERERESIiEREQAAAAAREiIzMiIiIiIjMzIiERAAAAERIzRERDMiIjM0REQyIRAAAAESI1ZmZVQzM0RWZmVDIREAAAESNP///WVERFZ+///zIREA"
"AAESNe///4dlVnjv///EIhEAAAESNX///9h3d43///1TIREAAAESNX7///qYid///9ZDIREAAAESNGj///2qrf///XVDIRAAAAESJFjf//+qr///6GQyERAA"
"AAESI1ef///a///+hlMiERAAAAERI0aN///////pdUMhEQAAAAERI0aK//////6nZDIhEQAAAAERI0V63/////qGUyIREAAAABESI0V6r////6p1QyIREAAA"
"ABESI0aKr////al1QyERAAAAABESNFea7////ql1MyERAAAAAREiNGiu/////9l1QyERAAAAEREjRXnv//////p1QyERAAAAERIjV47///3///2GQyERAAAB"
"ESI0aO///9rf//+XUyERAAABESNGff///qqv///YVCIRAAABEiRX3///6pmt///4ZDIRAAABEjRt///+mIiK///+dTIRAAAREjXP///5hmZ43///dTIRAAAR"
"EjX///+HZVVWj///5TIRAAAREj////dlRDRFbf//9DIRAAAREjRWZmVEMzM0RWZlQyIRAAABEiM0REQzIiIjM0REMiERAAABERIiMzIiIhIiIiMyIhEQAAAA"
"ERESIiERERERESIhEREQAAAAARERERERERERERERERAAABEREiERERERERESIRERAAERIiMyIiERERIiIzIiERAREiNERDMiIiIiM0REMiERESI0VmZUMyIi"
"M0VmZUMiEREjT///dUMzM0Rn///9MhERI17//+dUQzRWfv//5DIRESNX///4dURFaO///mQyEREjV+///YZVZ43///dUIhERI0ff//+Ydnif//+GUyERESNG"
"j///qYia///9dUMhEREiRY7//+qqrv//6GQyERAREjV5///6qu///oZTIhEQERI0ae///a3///l1QyERABESNFjf//+v//+nZDIREQABEiNXn//////9hkMi"
"ERAAAREjRp7/////6XVCIREAAAERI0aK/////pdUMhERAAAAESJFev////qGQyIREAAAABEiNXnv///ZdTMhEQAAAAAREjRp3///qGQyIRAAAAAAERI0ae//"
"/pdTIhEQAAAAABESNGn///2GQyERAAAAAAAREjRp///5dTIhEQAAAAAAARI0aP//+GQyERAAAAAAAAESJG3//+dUIhEQAAAAAAABESRe///mQyERAAAAAAAA"
"AREjT///1TIhEQAAAAAAAAARIjRWZVQyERAAAAAAAAAAERIjREQzIREAAAAAAAAAAAERIiIyIhERAAAAAAAAAAAAERESIREREAAAAAAAAAAAABERERIiIiIi"
"IiIRERAAAAARERIiIzMzMzMzIiIREAAAAREiM0REREREREQzIhEQAAERIzRVZmZmd2ZmZUMyEQAAESJFZ4iJmZmZmYh2UyEQAAESNO////////////5DIQAA"
"ESNf////////////1TIQAAESRf////////////dTIQAAESNf///////////+ZDIQAAESNGeJqqqqrv///oZCIQAAESI0Vniaqq3///6GUyEQAAERIzRWeJqt"
"///+l1QyEQAAERIjRFaJrf///pdUMhEQAAERIjNFeK3///6XZDIhEQABERIjRWie///+l2QyIREAABESIzRnnv///pdkMiERAAARESI0Z57///6YZUMiERAA"
"AREiNGee///+mGVDIhEQAAERIjRXnv///qhlQzIhEQAAERI0V57///6pdlQyIREQABESNFee///+qYdlQzIhEQABEiNXnv///qqYdmVDMhEQABEjRo7///6q"
"qpiHZlMyEQABEjV+////3MzMzMzMQyERABI0bv////////////wyERABI0b/////////////VCERABI0b/////////////UyERABEk3////////////+QyEQ"
"ABEjRXeImZmZmIiHdlMiEQABEiNFVmZmZmZmZVVDMhEQABESIzNERERERERDMyIREAAAEREiIjMzMzMzIiIhEREAAAAAAAERESIiIREREAAAABERIiMzIiIR"
"EAAAAREiM0RERDMhEQAAERIjRWZmZUQyEQAAERI0Xd3d3dUyEQABESJF3/////1DIQABESNG3/////xDIQABEiNX7/////ZDIQAREjRo///+3dVCEQAREjRp"
"///6l2QyEQARIjV9///qhkMiEQARIkWN///ZZUMhEQERI0aO///YZDIhEAERI0af//+XVDIREAERI1ef//+XUyIREAESI1ff//6GQyERAAESJFjf//2GQyER"
"ABESNGjv//11MiEQABESNGn///l1MiEQABESNXn///lkMhEQABEiNX3//+hkMhEQABEiRY3//9hUMhEAABEjRo7//9dTIhEAABEjRp///5dTIhEAABEjV5//"
"/5ZTIREAABIjV8///pZDIREAABIkWN///YZDIREAABI0aO///YZDIREAABI0af//+oZDIREAABI0af//+oZTIhEAABI1fP//6pdUIhEAABI1ff///u5kMhEA"
"ABI1fv////9kMhEAABI0b/////9kIhEAABI0b/////5TIREAABEjXd3d3dVDIRAAABEiNFZmZlQyERAAABESIzRERDMiERAAAAERIiIzMiIREQAAAAARERIi"
"IREREAAAAAEREREREQAAABERIiIhEREAABESIzMyIREAABEjRFVDIhEQABIk3u7lMiERABI03//2QyERABI1f//3UyERABI1f//4ZDIREBI1f//9ZDIREBI1"
"fv/9dTIREBI1fv/+dTIhEBI1ff//hkMhERI1fP//hkMhERI0af//lkMhERI0aP//x1MhERI0aP//11MiERIkWO//51QiERIjV9//6GQiERIjV9//+GQyEREj"
"V5//+WQyEREjRo//+XUyEREjRo///XUyIREiRo7//XUyIREiRX7//oVCIREiNX3//4ZDIRESNXn//4ZDIRESNGn//5ZDIRESNGj//9dTIRESJFjv/9dTIQES"
"I1fv/+dTIQERI1ff/+dTIQERI0bf//dTIQERIkV///dTIQAREjVv//ZDIQAREjRf//1DIQABESNFZlQyEQABESIzREMiEQAAEREiIiIhEQAAARERIhEREAAA"
"ABERERERAAAAAAERESIiIREREAAAABERIiMzMiIREQAAABEiM0RERDMhEQAAAREjRWZmZVQyEQAAARI0Xd3d3dVCIQAAARI13/////1DIQAAERJF7/////dT"
"IQAAERJF//////dTIQAAERI13d7//+dTIQAAERI1aK3//9dTIQAAERI0aK7//9dTIQAAERI0V6///5dTIQAAERI0V6///5ZDIQAAERI0WN///oZDIQAAERI0"
"aN///YVCIQAAERI0aO///XUyIQAAERI0af//+XUyEQAAESI1ef//+WQyEQAAESI1ff//6GQyEQAAESNFjf//2FQiEQABESNGjv//11MiEQABESNGn///l1Mh"
"EQABEiNXn///lkMhEQAREiRX3//+hkMhEQAREjRo3//9hUIhEAARIjRo7//9dTIhEAERIjV5///5dTIREAERI0V6///5ZDIREBESI1aN///oZDIREBEiNGed"
"///YVCIRABEjRXmu///XUyERABEjXe7v//+WQyERABEkX/////+GQiERABEkX/////51MhEQABEkX/////1kMhEQABEjTd3d3dZDIREAABEiNFZmZlQyIREA"
"ABESIzRERDMiERAAAAERIiIjMiIhEQAAAAAREREiIREREAAAAAAAAAAREREREREQAAAAAAARESIiIiEREAAAAAARESIzMzIhERAAAAAREiNEVVQzIREAAAAR"
"ESNFbd3VQiERAAARESNFfv//ZTIREAABESNFff//+GQyERABESI1aP///9dTIREAERI0aO////51MhEQERIjV8//7f/4ZCIRAREjRo//+s//hkMhEREiRX7/"
"/ar//XUyERESNGj//qqv/+dTIRERI1fv/5mZ7/91MhERI0bf/9h3jf/3UyEREjRv//h2Z4//1TIRESNe//h2VWf//lMhERI9//5lREVu/+QyEREjTMxVQzNE"
"XMwyIRERIzREMyIjNEQyIREBESIiIiIiIiIiIREAERERIREREREREREAAAEREREREREREREAAAARERIiIiIiIiIRERABERIjMzMzMzMyIhEQARIjNEVVVVVU"
"RDIhERESNFZmd3d3ZmVDIhEREk///////////TIRESNP//////////0yEREiT//////////0MhEREjRWZmd3d2ZlQyERARIjNEVVVVVURDIhEQERIiMzMzMz"
"MzIiERAAERESIiIiIiIiEREAABEREREREQAAABESIiIhERAAAREiMzMiEREAARIjREQzIREAARLe7u5UMhEQARI9///VMhEQARI03//1QyEQARIkXf//QyEQ"
"AREjRd//4yEQABESM0REMhEQABERIjMzIhEQAAERESIiEREAAAARERERERAAAAARERIiIiIiIiIiEREAABEREiIzMzMzMzMyIhEQARESIjRFVVVVVVREMyER"
"AREiNFVneId3d3dmVDIRERIjRWje7/7Zme7u5TIRESI0Z9//////2v//9kIhESNGfv///////v//5kMhEjRX7///////////1kMhEjVs////7Kzv////l0Mh"
"IkWP///6qqqt////lkMhI0af//+qmYmq////lkMhI1fv//6ph3iq7//+hkMhI1fv//yod3ia///9hUIhI1j///qod3ia///6dTIhI1j///qod3iq///5dTIR"
"I1j///2piImt///5ZDIRI1fv//6qqqqv///oZDIRI0bf///qqq3////XUyIRIkWP//////////+XUyEREjRo//////////+GQyEREiNWj//////v//91MhEQ"
"ESNFaN///+qv//5UMhEQERI0VneIiIiId2VDIREAEREiNEVmZmVVVUQyIREAARESIjNEREREMzIiERAAABEREiIiMyIiIiIREQAAAAERERESIiIhEREREAAA"
"AAAAERERERERAAAAAAAAAAABERIiIiEREAAAAAAAAAAREiMzMyIREQAAAAAAAAERIjRVVEMiEREAAAAAABERI0bd3VQyEREQAAAAABESNG7//9VDIhERAAAA"
"ABESNX///4ZDMiEREQAAAREjRo///4dUMyIhERAAAREjV5///pdlRDMiEREQARIjV9///qmHZlQzIhEQARI0aO///aqpiHZUMiERERI0af//+q3v/th2QyIR"
"ERI1ef///f/////nZDIRESI1ev/////////+dkMhESJFjf//////////11MhESNGjv///+2q3///+GQiESNGn///+qqqqv///WQyESNXn///2qmZqu///nUy"
"EiNXr///qpiImt///nUyEiRY3//+qYd3mt///nUyEjRo7//9qYd3mt///nUyEjRp///9qYd4mu///XQyEjR5///9qpiJqv//+GQyEjV5////qqmqr///51Mh"
"EjV9////+qqt////hkMhEjV+///////////9dUIhEjV////v///////nZDIREjRv//6v//////12QyIREiRf//2a3////YdUMiERESNFZ3d3eIiId2VDIhEQ"
"ESI0RVVVVmZmVUMyIREQEREiMzMzREREMzIhEREAARESIiIiIjIiIiEREQAAABERERESIiIhEREREAAAAAABEREiIiIiIhEREAAAABERIiMzMzMzIhERAAAB"
"ERIjNEVVVUQzIhEQABESIzRWd3d3ZlQyIRABESI0Vnje//7sdUMhERESI0V47//////3VDIRERI0V9/////////VMhERIkV9//////////UyEREjVo////7K"
"zv///TIREiRY////qpmZ3/1kMhESNGn///qod3d9dlQiERI1fv//6pdmZlVUQyEREjV+///KhmVVVEMyIRESNY///6mGVVVEQyIRERI1j///qodmVVVDMhER"
"EjWP///amHd3ZlQyIRESNX7//+qpiIj+ZUMhEBI0bf///qqq3//9QyEQEiRX7/////////9DIRARI0aP////////9kMhEBEiNWjv//////5lMhEQERIjVnzv"
"///+hlQyERABESI0VniIiHZlQyERAAEREiM0VWZlVEMiEREAABERIiMzREQzMiEREAAAARERIiIiIiIhEREAAAAAARERESIhEREREAAAAAAAAAAAAAAAERER"
"EREQAAAAAAAAAAABEREiIiEREAAAAAAAAAARERIjMzMiERAAAAAAAAARERIjRVVDIhEAAAAAABERESIzVt3dUyEQAAAAERERESIjRu///EMhAAABERERIiIz"
"Rn///2UyEAABEREiIjMzRXn///dTIQABERIiMzREVWjf//51MhABESIjRFVmZnid///XUyEBESI0RWeIiIma7//8dTIRESI0Vo3u/+2qr///l0MhESI0Z9//"
"////2v//+WQyERI0Z9///////+///oZDIRIkV9///////////9hkMhEjRo////7Krv////yFQiEiRY7///qqqq3///+nUyISNGn///qpmZqu///5dTIRI1ff"
"//6ph3iq7//+lkMhEjV+///al3eJrv//2GQyESNY///6qHd4mv///XVCIRI1j///qph3iq///5dTIREjV////amIiaz///lkMhESNX7///qqqqr///6GQyEQ"
"I0bf///qqq3////XVCIRAiRX///////////9dTIREBI0aP//////////hkMhEQEiNWj//////v//91MiEQARI0Vo3///6q///mQyERABESM0VniIiIiHdlQy"
"IRAAEREiNEVWZmVVVUQyIREAABERIiM0REREMzMiIREAAAARERIiIjMiIiIiEREAAAAAERERESIiIhEREREAAAAAAAERERIiIiIiEREQAAAAEREiIzMzMzIi"
"EREQAAERIiNEVVVVRDMiERAAERIjRVZ3iId2VDIhEQERIjRWiO7//th1QyIRERIjRnnv/////+dkMhEREjRn3////////nZDIREjRX3/////////51MhESNX"
"n///6qqt///4ZCISJGj///2qqqrf//1kMhI0bP//6qqqqq///nUyEjV+///93d3d3///dTISNY7///////////5kMhJFj////////////mQyEkWP///d3d3d"
"3d3WUyESNY///6qqqqqqmHVDIRI1fv//6qqqqqmHVDIhEjRt///9qqqt/4ZTIhESJFfv////7///1kMhEREjRo/////////1QyERESI1aO///////9UyERAR"
"EiNWeO////7XVDIREAERIjRWeIiId2VDIREAARESI0RVZmZVQzIREQAAEREiIzNERDMyIREQAAABERESIiIiIiEREQAAAAABEREREiEREREQAAAAAAABERES"
"IiEREQAAAAERESIiMyIhERAAABERIjNEREMyIRAAARESI0VWZmVDIhEAEREjNWeN7u1UMhEAERIzVo7////9MhEBESI1aP/////1QhEREiNGjv/////VMhER"
"EjRXz///2t5lMhERI0V57//9qpdUMhESI1aK///6qoZTIhESNGzN///8zMZTIRESNX////////ZDIRESRd////////ZDIRESRu///////+ZDIRASRd3v///t"
"3XUyERASNXqv//+qh1QyEQAiNXnf//6odUMhEQASNXnv//2XVDIhEAASNXnv//qGQyIREAASNXn///l1QyERAAAiNXn///lkMiERAAAiNX3//+hkMhEQAAAS"
"NX7//9dUIhEQAAASRY7//5dTIREAAAASRY///4ZDIREAAAASNX///3UyERAAAAASNd///mQyERAAAAASNO///VMiERAAAAARI0VmVTMhEQAAAAARIjNEQyIR"
"EAAAAAAREiIiIiEREAAAAAABERERERERAAAAAAAAAAARERIiIiIiIiIiEREQAAARERIiMzMzMzMzMyIREQABERIiNEVVVVVUREQzIREAAREiNFVneId3ZmZm"
"VDIRABESI0Vo3u/+2Zne7uVCIQERIjRn3//////a7//2QyEBESNGfv///////e//91MhERI0V+///////////+dTIRESNW3////are/////XUyERIkWP///q"
"qqqq////11MhESNGz///qpmZqu///5dTIREjV+///qmIiKrf//+WQyERI1fv//2ph3iq7//+lkMhESNX///6qYiJqv///YZDIREjV+///aqZmqr///2FQiER"
"I1fv//+qqqqv///6dTIhESNG3////aqt////+XUyEREjRo///////////+lkMhERIkV5///////////YZDIRESI0aK//////7v//11MiEREiNFeK3v//7a//"
"/5dDIRERIjRniaqqqqqv//6GQyERESI1Z4mqqqqq7//9dTIhEBEjRd/aqqqqrv//+GQyERARI0b//+7d3v///8dTIhEAESNO//////////11QyERABEjTv//"
"///////XZDIREAAREkV+///////odUMiERAAARI0Vnjd3t2YdlQyIREAAAERIzRWZ3d3ZlQzIhEQAAABERIjNERVVEQzIhERAAAAABERIiIzMzMyIhEREAAA"
"AAABERESIiIiIhEREQAAAAAAAAAREREREREAAAAAAAAAABERIiIiEREAAAAAAAAAERIjMzMiEREAAAAAAAAREiNFVUQyIREQAAAAABERI0bd3VQyEREQAAAA"
"AREjRu///VQyIRERAAAAERI1f///hkMyIRERAAAREjRo///5dUMyIhERAAERI1ef//6XZVQzIiERABEiNX3//+qYdmVEMiERARI0aO///aqpmHZUMiERESNG"
"n///qt7//tdkMhEREjV5///9/////+dkMhESI1ev/////////+dTIREiRY3//////////9ZDIRI0aO////7d3v///nUyESNGn////aqqrv//91MhEjV5///9"
"qpmq3///hTISI1ev//+qiImt///oUyEiRX3//+qHd4rf//51MhI0aO///ZdmeK7//9dTISNGj///qHZmiv//+XUyEjRp///6hlVor///lkMhI0af//6XVVaK"
"///oZDISNX3//+hlRWjf//51MiEjV+///XVERn7//9dTIRI0b///hlREV///+GQyESNG///3VDNFb///dTIRESNf//5UMzNF///lQyEREjRWZlQzIjNFZmVD"
"IREREjNEQzIiIiM0RDMiEREREiIiIiEREiIiIiEREAEREREREREREREREREQAAAAABEREREREQAAAAEREiIiIREQAAAREiMzMzIREQAAERI0VVVDIREAAREj"
"Re/+VDIRAAERJF7//+UyEQAREjRv///1MhEAERI0b///9UIRABESNG7//9UyEQAREjRn3udkMhEBESI1Z4h2VDIRAREjRWiIdlMiEQERI0bu7uZTIhEREiRX"
"///2UyERERI0aP//51MhERESNX3//+dTIRERIkV+///XUyERESNGj///lkMhEREjRp///4ZDIRERI1ef//6GQyEQEiNX3//+hUIhEBIkWO///XUyIRASNGj/"
"//l1MhEQEjRp///4ZDIREBI0af//6GQyERASNX3//+dTIhEAEjV+///XUyERABI0b///hkMhEQASNG///3UyERAAESNf//5UMhEQABEjRVZlQyERAAAREjNE"
"QzIhEQAAEREiIiIhERAAAAEREREREREAAAAAAAAAAAAREREREREAAAAAAAABERIiIiEREAAAAAAAERIjMzMyEREAAAAAABESNFVVQyERAAAAAAERI0Xv/lQy"
"EQAAAAABESNe///lMhEAAAAAARI0b///9TIRAAAAABESNG////VCEQAAAAAREjRu///VMhEAAAAAERI0Z97nZDIRAAAAAREiNGeIh1QyEQAAAAERI0VniHZU"
"MhEAAAABESNG3u7mUyIRAAAAARIjV///91MiEQAAABESNGj///dUIhEAAAAREjRp///nUyERAAAAESI1ff//51MhEQAAABEiRY7//9dTIREAAAERI0aP//+X"
"QyERAAABESNGn///lkMhEQAAARIjV5///oZDIREAAAESJFff//6FQyEQAAAREjRo7//9dTIhEAAAERI0aP//+XUyERAAAREiNXn///lkMhEQAAERIkV6///o"
"ZDIREAAREiNGjf//6FQiEQAAERIjRo7//9dTIhEAABEiNFef//+XUyERAAASI0Vor///hkMhEQAAIjRWec///oVCIRAAACNFZ5rv//11MhEQAAAzVu3e///4"
"ZDIREAAANF7/////11MhEQAAADRv/////nVDIREAAAA07////9dUMhEQAAAAM0bd7ddlQyIREAAAACM0VmZlRDIhEQAAAAAiIzREQzIhERAAAAAAESIiMiIi"
"EREAAAAAABERESIhEREQAAAAAAAAABEREREREAAAAAAAAAAAAAEREiIiIREQAAAAAAAAAAAREiMzMyIREQAAAAAAAAABESI0RVQzIREQAAAAAAAAEREjRd3d"
"VDIRERAAAAAAABESNF7//9QyIREREAAAAAAREjV///91QyIRERERAAABESNGj///hkMyIiIRERAAAREjRp///oZUMzMyIhERAAESI1ff//6GVERERDMhEQAB"
"EjRo7//9l2ZmZmVUMhEQERI0af//+ph3eO7u7tIREBESNXn///qpiZ////0yERARIjV6///qqqz////VMhEQESJGjf//6qrf///3ZDIRABEjRo7//9qu///+"
"hlMiEQARI0ef//+q7///6XVDIREAESNXr///r////ahkMiERABIjV6///////9qXVDIREAASJGjf//////+qhlMiERAAEjRo7///////6oZDIhEQABI0af//"
"//////2WUyIREAASNHn////9////l1QyERAAEjV5////qs///9hkMhEQABI1ff//+qqu///5ZTIREAASNX7//9qZqv///nUyERAAEjV///+pd4nf///VQhEQ"
"ABI0b///h2Znj///9UIREAASJF///mVUVWf///4yERAAESNFZmVUQzRVZmZUMhEAABESM0REMzIzNEREMyERAAARESIjMiIiIiIzMyIREAAAAREREiERERES"
"IiIRERAAAAAAABEREREREAAAABERIiIhERAAABESIzMzIhEQABESI0RVQzIRABERI0Xd3VMiEAERI0Xv//1DIQAREjV///91MhAREjRo///3UyEBESNGn//+"
"dTIQESI1ff//51MhARIkWO///XUyEREjRo///5dDIRESNGn///lkMhESI1ef//6GQyERIkV9///oZDIREjRo7//9dTIhESNGj///l1MiERI1af//+WQyERIj"
"V5///oZDIREiRX3//+hkMhESNGjv//11MiERI0aP//+XUyEREjRp///5ZDIRESNGn//+hkMhERI1ff//51MiEQEjV+///XUyERASNG///4ZDIREBI0b///dT"
"IREAESNf//5UMhEQARI0VWZUMhEQABESM0RDMiERAAEREiIiIhERAAABERERERERAAAAAAARERIiIiIiIiIiIiIiIiIRERAAABERIjMzMzREQzMzMzMzMzIh"
"ERAAERIjNEVVVVVVVUREVVVVVDMiERABESNFZ3d3eIiHdmZnd4h3ZUMiEQERI0Xu7uqt7/7pmIid7//sdUMhEBEiNX///t/////9qt/////+dUMhERI0aP//"
"///////97//////+dTIRESNX3//////////////////4ZDIRIkWO////7d7//////d3////XQyESNGj///+qqq3////qqqr///51MhEjRp///9qpqq///+qp"
"mq3//+dTIRI1ef//+qiJqv///amImt///nUyEiRX3//+qHeJr///qYd4rv//11MhI0aO///Zh3ea///6h2eK7//5dTISNGj///qHZnnf//6YZniv//+WQyEj"
"Rp///6hmZ57//9l2Z4r///hkMhI0af//6XZWef//+oZVaN///oVCISNX3//+hlVnn///l2VWjv//11MhEjV+///XZVV5///oZVVo7//4ZDIRI0b///hlRFbf"
"//52RFZ///91QiESNG///3VERG7//9ZURFf///ZTIRESNf//5kMzRf//9lQzNN///lQiEREjRWZlQzIzRVZlVDMjNFZlVDIREREjNEQzIiIjNERDMiIjNERD"
"MhEREREiIiIiIRIiIiIiIRIiIiIiEREAERERERERERERIREREREREhEREQAAABEREiIiIiIiIiEREAAAEREiMzMzMzMzMiEREAAREiNERVVVVVVUMyIREAER"
"I0Vnd3d4iHdlQyIRAREjRe7u6q3v/+12QyIQESI1f//+3/////52QyEREjRo//////////91QhERI1ff//////////1kMhEiRY7////t3e///+dTIRI0aP//"
"/9qqqu///3UyESNGn///2qmarf//+FMhEjV5///6mIia3//+hTISJFff//6od3it///nUyEjRo7//9l2Zorv//11MhI0aP//+odmaK///5dTISNGn///l2VW"
"iv//+WQyEjRp///pdVVor//+hkMhI1ff//6GVFaN///nUyISNX7//9dURGfv//11MhEjRv//+GVERX///4ZDIRI0b///dUM0Vv//91MhERI1///lQzM0X//+"
"VDIRESNFZmVDMiM0VmZUMhERESM0RDMiIiIzREMyIRERESIiIiIRESIiIiIREQARERERERERERERERERAAAAARERIiIiIiIhEREAAAABERIiMzMzMzIiEREA"
"AAEREiM0VVVVVEMiEREAAREiM0VneIh3ZUMyEREBESI0Vnje//7odlQyERERIjRXjv//////h1QyERESNFff////////11MiERIkV9//////////hkMhESNW"
"j////arO////dTIRIkWP///6qqqt///4ZDISNGn///qpiJqv///WQyEjV+///qmHeJrv//11MhI1fv//yod3ea3//9dTISNY///6qHZ3mu///XQyEjWP//+q"
"h3eJr///lkMhI1j///2piImt///4ZCISNX7//+qpmqr///11MhEjRt///+qqrf///4ZDIRIkV+//////////hlMiERI0aP/////////XVDIRESI1aO//////"
"/odUMhERESI1Z87////sdlQyEREBESI0VniIiIdlQzIhEQARESI0RVZmZVRDIhERAAARESIjM0REMzIiEREAAAARERIiIiIiIiEREQAAAAABERERIiIRERER"
"AAAAAAABEREiIiIiIiIhEREAAAABERIjMzMzMzMzIhEREAABESIzRVVVVVVUQzIhEQAAERI0Vnd3d4iHdlQyIREAERI0Xu7uqt7/7YdkMiEQARIjV///7v//"
"///nZDIRAREjRo//////////52QyEBESNX3//////////9dTIQESJFjv///+2q3///+GQiERI0aP///6qqqq///9ZDIREjV5///9qpiarv//51MhEiNXr///"
"qoh4mt///nUyESJFjf//6ph3eK3//+dTIRI0aO///amHd5rf//51MhEjRp///9qYd4mu///XQyESNXn///2qmImq///4ZDISI1ev///6qqqq///+dTIRIkWN"
"////+qqt////hkMhEjRo7///////////11QiESNGn///////////52QyERI0ef//+u//////12QyIREjV5///qrf///9h1QyIRESNX3//+qpmZiHdlQyIREB"
"I1fv//2Yd3ZmVUMyIREQEjV///+oZVVEQzMiEREQASNX///4dUQzMyIhEREAABI1f//+dUMyIiEREREAAAEjTf//5kMyIhERERAAAAASI13d1UMiIREREAAA"
"AAABEiNFVEMiERERAAAAAAAAERIjMzMiEREQAAAAAAAAABERIiIhEREAAAAAAAAAAAARERERERAAAAAAAAAAAAAAABEREiIiIiIiIiIREQAAERESIjMzMzMz"
"MzIiERABERIiNEVVVVVEREQyIREBESI0VWd4h3dmZmZUIhEREiNFaN7v/tmZ3u7lMhERIjRn3//////a///1QiERI0Z+///////9///mQyESNFfv////////"
"///WQyESNWz////srO////+WQyEiRY////qqqq3///+WQyEjRp///6qYiarv//+WQyEjV+///qmHeKrv//6GQyEjV+///Kh3eJr///2FQiEjWP//+qh3eKr/"
"//p1MiEjWP//+qiHiar///l1MhEjWP///amYmq3///lkMhEjV+///qqqqq///+hkMhEjRt///+qqrf///9hkIhEiRY///////////6dTIhESNGj/////////"
"/5dTIRESI1aP/////////5ZDIRERI0Vo3///6t///oZDIREREjRWeJmqqt///YVCIRARESI0Vmd4mu//+XUyERABERIjNEVniv//+WQyERAAERESIzRFef//"
"+FQiEQAAARERIiM1bf//5lMhEQAAAAERESI0Xf//1UMhEQAAAAABERIjRd3dVDIREAAAAAAAEREiNEVUQyIREAAAAAAAARESIjMzIhERAAAAAAAAAAEREiIi"
"IREQAAAAAAAAAAAREREREREAAAAAABEREiIiIiEREQAAARESIzMzMzIiERAAERIjNEVUREQyIRAAESI0Vmd2ZmVDIREBESNF7u7pne70MhEBEiNX///t///1"
"MhEREjRo///////lMhEREjV9///////VMhERIkWO/////u5kMhERI0aP///+qXZDIRERI0af///qh2QyIRARI1ef//+pdUMiERASJFff//6XVDIhEQASNGjv"
"//2GUzIhEQASNGj///p1QyIREAASNGn///l1MiERAAASNGn//+hkMiERAAASNX3//+dUMhEQAAASNX7//9dTIREAAAASNG///4ZDIREAAAASNG///3UyERAA"
"AAARI1///lQyERAAAAARI0VmZUMhEQAAAAAREjNEQzIhEQAAAAARESIiIiEREAAAAAABERERERERAAAAAAAAAAERESIiIiIiEREQAAABERIiMzMzMyIhEQAA"
"AREiM0RVVVVEMiERAAERIjRVZ3d3dmVDIRAAERI0Vo3u///tZTIREBESNFff///////kMhEBESRX7////////kMhEBEjRt////////9kMhEREjV////aqqre"
"5kMhEREjWP//6qqqqYZUMhEREkWP//+qqqmHZTIhEREjV/////7aqYZTIhEREjV+///////pdUMhEREjRo3///////hkMhERIjRnmt7/////5kMhERIjVnia"
"qq3v//91MhERI0Vomaqqqv//91MhERI0b+2qqqqv//50MhERI13//+7u7///1kMhERI1/////////+dTIRERI1/////////nVDIRERI0V+7////th1QyERAR"
"EjRWd4iIh3ZUMiERABESM0VWZmVVQzIhEQABERIjM0REQzMiEREAAAERESIiIiIiIRERAAAAARERERIiEREREQAAAAAAEREREREREQAAAAEREiIiIiEREAAA"
"ERIjNEQzIhERAAERIjRVZlQzIREQERIjRX7u7VQyERAREjRX7//9ZTIhEREjRXn///qGQyIREiNXmv//+pdkMhESNG7v///+7uUyERI1f///////9UIREkXf"
"///////1QhESRe///////+UyERI1eq///6qYZDIREjV5r//+qYZTIhESNWnf//6oZUMhERI0ae///ZdUMiEQEjRp7//6hkMyERASNXn///qGQyIREBI1ef//"
"6oZDIhEQEjV9///qhkMiERASNX3//9qHVDIREBI0fv//6phkMhEQEjRt////7+UyERARJFf/////9UIRABEjRt/////1MhEAESI1be///9QyEQAREjNWd3d2"
"VCIRAAERIjRFVVQzIREAARESIzMzMyIREAAAERESIiIiEREAAAABEREREREREAAAAAEREREREREREREREREAABERIiIiIhERIiIiIREQARESMzQzMiIiIzRD"
"MiERAREjRVVUQyIjNFVVQyERERI0Xu7uVDMzRu7u5DIRESI1f///ZDM0bP//5TIRESNG3//+dURFff//1kIhESNX3//9dURWjv//1kMhEiRY7//5dlRXj///"
"hkMhEjRo///5dlVnn///hkIhEjR5///5dlVoz//+hkIhEjV9///pdmZ53//9dTIhIjWN///ZdmaK7//9dTIhIkaO//+ph3ea///5dTIRIkaP//+qh3iq///5"
"ZDIRIkaP//+qmZqu///oZDIRIkaP///aqqrf///YVCIREjWP///9qs7////XUyEREjV+//////////+WQyEREjRo//////////+GQiEQESNW3//////f//51"
"MhEQESNFaO///9qv//1UMhEQERI0VniIiIiId2VDIREAEREjNFVmZmVVVUQyEREAARESIzNERERDMzIhERAAABEREiIiMyIiIiIREQAAAAEREREiIiIhERER"
"EAAAABERERERERERERERERAAERIiIiIREREiIiIhERAREiM0MzIiIiIzMzMiERESNEVVVDMiIzRVVUMhEREjXu7tVDMzRG3u7tMhESNG///nVENFZ///9DIR"
"EjRv//+GVEVn7//8UyERI1fv//l2VWfP//5kMhESNH3//9h2eJ///4ZDIREjRp///ph4nf//11MhERI0af//+pma///oZDIRESNGjv//qqrv//l1MiEREiRo"
"7//8qt///oZDIREBIjV9///ar///l1MiERARI1ef//6u///XVDIREAESNGn///r//+hkMiERABEjRo7/////+XVDIREAARIjWN/////9hkMhEQAAERI1ef//"
"//+XUyIREAABESNGj////5dUMhEQAAABESNX7///6GQyEREAAAAREjRe///3ZDIhEQAAAAAREjRWd3ZUMiERAAAAAAERIjRFVUQyIREAAAAAAAERIiMzMyIh"
"ERAAAAAAAAERESIiIRERAAAAAAAAERERERERERESIRERERERERERAAERIiIiIhEiIiIiIiERIiIiIhEQERIjMzMyIiIzREQzIiIjMzMyIRERIjRVVEMzM0Vm"
"ZlQzMzRFVUMiEREjTu7mVDNFZ+7u1UQ0RW7u7TIREiRf//1lRFZ+///nVEVX7//1MhESNG///nZVaJ///+hlVnj//+UyERI0bv//l2eJ7///+Ydnjv//dTIR"
"EjR9//+oiJr////6mIiv//1kMhESNGz//6mZrv////qpmu//+GQyERI0af//2qrf/////aqt///XUyEREjRo///aqv//7//+qq///oZDIRESNGjv/+qt//+u"
"//6q3//5dTIhERIkaO//+q///a3//6r//+hkMhEQEiNX3//67//6rP//rf//l1MiERARI1ev//3//9qq///f//6GQyERABEjR5//////qqr/////+XUyIREA"
"ESNGj/////ypmu/////YZDIREAARIkWO////6oiJ7////5dTIhEQABESNX3///+odnjf///9dUMhEQAAERI0bf///odlZ4////hkMhERAAABESNW///3ZURV"
"f///xkMiERAAAAERIkVnd2VUM0RWd3ZUMiERAAAAABESI0RVRDMyM0RVVEMiERAAAAAAEREiMzMyIiIiIzMzIiEREAAAAAABEREiIiIREREiIiIREREAAAAA"
"AAAAEREREREREREREREQAAAAAREiIiIiIiIiIiIREQAAAAESIzREMzMzNEQzIhEAAAAREjRWZVRERFVVVDIREAAAESNO7u52VVZ97u7TIRAAABEjX///2Hd4"
"7///QyEQAAERI1z///qZnv//9kMhEAABESNX7//9qt///3VCIRAAAREjV5///63///hkMhEQAAERI0aO//////+XVCIRAAABESNGiv/////6hkMhEQAAAREj"
"RYrf////qXUzIREAABESI0aKr///+qhkMiERAAAREiNGit////qnVDIREAAAERI0V53////9p1QyERAAAREiNWjf/////6dUMhEQAAERI0Z9///////YZDIR"
"EAABEiNXz///yv//+WQyERAAARI0aP///arv//51MhEQABESNX///9mar///dTIRAAAREjX///2Hd47//+UyEQAAERI////XZlZn///0MhEAAAESNFZmZVRE"
"VWZlQyIRAAABESM0REQzMzRERDIhEQAAARESIjMiIiIiMzIiERAAAAAREREiIhEiIiIhEREAAAAAAAAREREREREREREREREQAAAAERIiIiIRERESIiIhERAA"
"ABESIzQzMiIiIjMzMyIREAAREjRFVVQzIiM0VVVDIREAAREjXu7tVDMzRF3u7tMhEAARI0b///dUQ0Vn///0MhEAARI0b///hlRFZ+///FMhEAARI0fv//x2"
"VWef//5kMhEAERI0ff//2XZnnv//hkIhEAERI0af//6oeJ3//9dTIREAERI0aP//+pma///oZDIREAARI0aO//+qqu//+XUyIREAARIkWN///arP//2GQyER"
"AAARIjV8///qr///llMiERAAAREjV5///63//8dUMhEQAAAREjV5///6///oZDIhEQAAAREjRp7/////+XVCIREAAAARIjRo3/////2GQyERAAAAERIjRor/"
"///+l1MiERAAAAERIjRor////6hkMhEQAAABESIzRorv///oZDIhEAAAAREiM0Vorf//+XVDIREAAAAREjRVaJr///2GQyERAAAAARI0Vnia3//+hkMiERAA"
"AAARI1b/3e///5dTMhEQAAAAARJF//////+XVDIREAAAAAARJN//////11QyEREAAAAAARI9/////odUMiERAAAAAAARI0V97dh2VDIhEQAAAAAAARIjRVZm"
"VUMyIREAAAAAAAAREiM0REQzIhERAAAAAAAAABERIiIyIiIREQAAAAAAAAAAERERIiEREREAAAAAAAAAAAAAARESIiIiIiIiEREQAAAREiIzM0REMzMiERAA"
"AREiNEVVVVVVVDMhEQABEiNWZ3iIiId2VDIRAAESNO7u7u7u7u7lMhEAERJF//////////VCEQARIkb/////////9kIRABEjTf/////////mQhEAESJFaJqq"
"rf///nUyEQARIjRniqrf///oZDIRAREiNFaKrf///YZTIhEBESI0V4rf///ZdUMhEQERIzVonf///ZdkMiERERIjRXnf///ZhlMyERAREjRXnf///ah1QyIR"
"EBEiNWjf///amGVDIhEQESNGjf///aqYdkMiERARI1ff///aqqmHVDIRABIkbf/////////kMhEAEjRv/////////+UyEQASNG//////////1DIRABIk3///"
"//////9UIhEAESNFZ4iJmIiHZUMhEQARIjRVZmZmZlVEMhEQABESIzNEREREQzIhERAAARESIiIzMzIiIhERAAAAAAAAEREREREREAAAAAARESIiIiEREAAA"
"ABERIjMzMyIREAAAERIjRFVVQyIRAAAREiNFZ33dQyEQABERI0Z+///1QhEAAREjRX////9kIRAAESI1fv////VCEQAREjRo////7VMhEAESI1ff//6oZDIR"
"AREjRo7//9h1MiEQERI0aP//+nZDIREREiNXn///l1MiERERI0WN///oZDIhEBEiNGjf//2FQyERARI0V57//5dTIhEQESNWiv//+WQyERABI0Z67//+hkMh"
"EQASNX/////XVCIRAAEjV////9l1MiEQABJFz///6oZDIREAASRd////6GQyERAAEjV63//+hkMhEQABI1ed///oVDIRAAASNWnf//2FQiEQAAEjRp7//6dU"
"IhEAABI0ef//+XUyIRAAASNXn///l1MiEQAAEjV9///pdTIhEAABI1ff//6XUyIRAAASNX7//+p1QiEQAAEjRu////5kIhEAABIkbf////ZCIRAAARI1bv//"
"/lQhEQAAESNFfv//1TIREAABESNFZ33VQyEQAAARESM0VVRDIREAAAARESIzMzIhEQAAAAARERIiIiEREAAAAAAREREREREQAAAAAAAAARERERERAAAAABER"
"EiIhERAAAAEREiMzMiERAAABESM0REMiEQAAERI0Xd3UMhEAARESNW///EMhAAERI0bf//ZDIQABESNX7//3UyEAERIkWP//51MhABESNGj//9dTIQAREjV5"
"///GQyEAESI1ff//hkMhABEiRY7//4ZDIQERI0aP//6FQiEBESNGn//9dTIhARIjV5///HUyIQESI1ff//lkMhEBEjRo7//4ZDIRERI0aP//6GQyERESNGn/"
"/9dTIhERIjV5///HUyIRESI1ff//lkMhEREjRo7//4ZDIRERI0aP//6GQyERESNGn//9dTIhEBEjV5///HUyIRASI1ff//lkMhEQEiRY7//4ZDIREBI0aP//"
"6FQyERASNGj//9dTIhEAEjRo///GQyERABI0bf//hkMhEQASNG7//3UyIRAAEjRv//5kMhEQABEjX//9VCIREAARI03d1UMhEQAAERI0REQyEREAABESIjMy"
"IREQAAABERIiIRERAAAAABEREREREAAAAAAAAAEREREREREAAAAAARESIiIiEREAAAAAERIjMzMiEREAAAAREjRFVUMyEREAAAESNF3ddlQyERAAAREjXf//"
"51QyEQAAESJF7///91MhEAABEjRv////1kIhAAARIkbe///+ZDIQAAESJFiu///nUyEAABEiRXnv//11MhAAARIjV57//8dTIQAAESI1ef//+XUyEAABEiNX"
"n///lkMhAAARIkV9///pZDIQAAESJFjf//2WQyEAABEjRo7//9l1MhAAERI0aO///adTIQABESNGjv///9VCEAAREjRorv//+FQhAAESI1ed////dTIQAREj"
"RX3////2QyEAERI0aP///ZdUMhABEiNWn///qGQyEQERIjV9///pdUMhEBESNGjf//2GQyIRARIjRo7//8dUMhERESI1ef//+XUyIRAREjRor///hkMhEQES"
"JFed///oVDIREBEjRor///11MhEQARI13v///4ZDIREAEiRv////51MhEQABIkb////nVDIREAARJM///9dUMhEQAAESNN13dlQyIREAABEiNFVVQzIhEQAA"
"AREiMzMzIhERAAAAARESIiIhEREAAAAAAREREREREQAAAAAAAREREiIiIREREREQAAEREiIjMyIiIiIhERABERIjREREMzMzMiERABESM0VmZlVEREQzIRER"
"EjRW3u7XZmZt3UIhEREjVu///+iIiP/0MhERI0bv////+pn//kMhERI13/////////9kMhERI1//6Zn/////5UIhERI0//d3iO///+ZDIRERIk3cZVZnje12"
"QyIRAREjNERERFVmVUMiERABESIzMzMzREQzIhERABEREiIiIiIiIiEREQAAERERERESIiEREREAAAAAABEREREREQAAAAEREiIiIREQAAARESMzMzIREQAA"
"ERI0VVVDIREAAREjRd/+VDIRAAERI17//+UyEQABEiRf///1QhEAERI0b///9UIRABESNG7//9UyEQAREjRX7+1kMhEAERI0V4h2VDIRAREiNGeIdlMiEQER"
"I0Vu7uZDIhEBESNG3//mQyERARIjV+//5lMhERESNGj//9dTIREREjRp///WUyERESI1ff//lkMhEREjRo7//5ZDIRERI0aP//+GQyERESNXn///hkMhEBEj"
"V9///oVCIRARJFjv//51MhEQESRY///9dTIREBEkXf///GQyERARI17///dTIhEAESNP///2QyERABEiNWZmVDIhEQAREiNEREMiERAAAREiMzMiIREAAAAR"
"ESIiEREQAAAAARERERERAAAAAAAREREREREQAAAAERESIiIhEREAABERIjMzMiIREQAREiM0RVRDIhEQERIjRWbGZUMyERERI0Xf///mUyIRESNF3////+ZD"
"IRESNc//2Z7/5UIhESRu/5mInv9kMhEjRv/piInf/FMhEjRv/piIif/VMhEjRv/piInf9kMhESRe/9mZn/9kMhESNG//7u//1TIRESJFb////9VDIRERI0Vt"
"7+1lQyERAREjNFVmVUMiERARESIzREMzIhEQABERIiIiIiEREAAAERERERERERAAAAEREREREAAAERESIhEREAERIiMyIhEQERIjREQzIREREjRW1lQiEREj"
"Rv//1TIRESNe///1QhERJF////ZCEREkX///9UIRESNe///VMhERI0Xe7VQyERESNEVUQyEREREiMzMiEREBERIiIiEREAAREREREREAAAAAAAERERERERAA"
"AAAAAAARESIiIhERAAAAAAABERIjMzMhERAAAAAAAREjRVVUMhEQAAAAABESNF3/5UMhEAAAAAAREjXf//5TIREAAAABESJG////VCERAAAAAREjRv///1Qh"
"EQAAABERI0bv//5TIREAAAAREiNGfe7WQyERAAABESI0VniIdUMhEQAAERIjNFeIh2UyIREAARESM0V97u5kMhEQABERIzRnj///ZDIREAAREjNWef///VQy"
"ERAAESI1aN///+dUMhERABEjVo7///2GUzIREQASNGjv//+odUMyIREAEjV+///6l2VEMyIRECJGz///qodmVUQyERAjRu///qqYd2ZlQyEQI0fv//6qmYiO"
"9lMhESNH7///2qqt///UMhEjRu//////////9DIREkV//////////2QyERI0aP///////+ZTIRESI1aO/////+hlQyEQESM0Z4jN2Yd2VDIREBESI0VWZmZl"
"VDIhERARESIzRERERDMiEREAARERIiMzMzIiEREQAAARERESIiIiERERAAAAAAAAAAAAAAAAERERERERAAAAAAAAAAAAAAARESIiIhERAAAAAAAAAAAAABER"
"IjMzMiERAAAAAAAAAAABERIjRFVEMhEQAAAAAAAAAAAREiNFbu7uMhEAAAAAAAAAABESI0V///5DIRAAAAAAAAAAERIjRd///VQyEQAAAAAAAAARESNF3//3"
"ZDIhEAAAAAAAAAERIzXf/+hlQyERAAAAAAAAAREiNGeJmXZTIhEQAAAAAAABESI0Z5qqmGQyIREAAAAAAAAREjRX3///hlMiERAAAAAAABESNFef///9dUMh"
"EQAAAAAAERIjVo7////oZDIREAAAAAARESNGjf////+WQyERAAAAAAERI0V5//////p1MiEQAAAAAREiNWnv//7//9hUIhEAAAAAERI0aN///q///oZDIREA"
"AAAREjRXn///qv//+XQyERAAABESI1aO///arv//p1MiEQAAAREjRor//+qq3//9hUIhEAABESNFef//+qqq///oZDIRAAERIjRo7//+qqqv//+XQyEQABES"
"NGiv//+qqqrv//p1MiEAERIjV5///9qqqq3//9hUIhABESNGjf///////////oZDIQERIkV5////////////+WQyEBESNGjv////////////l1MhARIkV9//"
"///////////9dTIRESNGj///2qqqqqqt///nUyEREjV+///qmHd3eJqv//91MhERI13///mHZVVVZ4n///dTIRESNf///XZUREREVo7//9UyEREj7///dUQz"
"MzM0Vu///kMhERI0VmZlQzIiIiIzRWZlQyEQERIzREQzIiIREiIjNERDIhEAERIiMyIiERERERIiIzIiERABERESIRERERERERESIhEREAAAAAAAAAABERER"
"EREQAAAAAAAAAAERESIiIhERAAAAAAAAARERIjMzMyIREAAAAAAAERESI0RVVDIhEAAAAAARERIjNFbu7uMhEQAAABERESI0Vn///kMhEQAAARERIiNFbf//"
"1kMiEQAAEREiIzRW3//4dkMiEQABESIzRFVt//6YdlQyEQAREiNFVneJmqqph1QyIQAREjRXiJmqqqqqmHVDIQERI0Xf//////////9TIQERI1fv////////"
"//5TIQESJGj///////////1TIRESNGn///////////ZDIRESNXr///qqqqqph2UyIREiRY3//+qqqZiHdlQyEREjRo7//9qqmId2VUMiEREjRp///8qqmYd2"
"VDIhEREjV6///6qqqpiHVDIhERIkWK///+7u7u7u1DIREBI0aN//////////1TIREBI0ae//////////ZTIREBI0ef//////////ZDIREBI1ev///czMzMzM"
"VCIRECI1ev//+qqpmIdlQyERACJFjf//6qqYh3ZVQyERACNGjv//2qmYh3ZVQyERACNGn///yqqpmIdlQyIRACNGn///2qqqqpmGVCIRACNGn///////////"
"1DIRACNG3///////////xDIRACJG7///////////ZCIRABI1////////////UyERABI0VniImZiIiHdlQyEQABEjRFVmZmZmZVVDMhEQABESIzRERERERDMy"
"IREAABERIiIjMzMzIiIhEREAAAAAAAAREREREREAAAAAABEREiIiEREAAAAAEREiMzMyIREAAAAREiNEVUQyERAAABESI0Vu7u4yEQAAAREjRW///kMhEAAB"
"ESNF3//9VCIRAAARIjTf//dkMhEQABESNN//52VDIREAAREjRniIdkMiERABESJFeJmHZDIREAAREjRt///mQyERAAESI1fv//1kMhEQAREjRo///9ZDIREA"
"ERI0af//+GQyERABESNXn///hkMhEQARIjV9///oZDIRAAESNGjv//2FQiEQABEjRo///9dTIhEAARI1ef//+XUyERAAEiNXr///lkMhEQABIkWN///oZDIR"
"EAASNGjv//2GQyEQAAEjRp///9dTIhEAABI1ef//+XUyIRAAAiNXn///lkMhEQAAIjV9///oZDIREAACJFjv//2FQiEQAAAiRo///9dTIhEAAAI0aP//+WQy"
"ERAAACNGj///hkMhEQAAAiRt///nUyERAAAAEkXv//5kMhEQAAABI0///9UyEREAAAASI0VmVDMhEQAAAAESI0REMiERAAAAABESIiMiIREQAAAAABEREiER"
"ERAAAAAAAAAAAAABERERIREREREQAAAAAAAAABERIiIiIiIiEREAAAAAAAABERIjREQzMzIiERAAAAAAABERI0VVZVVERDIhEQAAAAABERI0XO/tdmbuQyER"
"AAAAABERIjXf///ojf9TIhEQAAABERIjRv///////lQyEREAABESIjRd/9mf///3VDIhEQABESIzRW3ud4jv/odlQyIRABESI0Vmd3dmZ3eIh3ZUMhEAERI0"
"V4iIdmVVZneIh2VDIQERI0Xf//yHZVVWZ9///kMhAREjV+///5hlVVV47//9UyEBEjRo////2XZVVnn///hUIRESNGn////6h2VWiv//+GQhERI1ev////6o"
"dmeN///oVCERIkWN/////6mHd53//+dTIREjRp7/////+ph4rv//11MhESNGn//////9qZmv//+XUyERI1ev//////+qqq///5ZDIRIjV6///6///+qq3//+"
"lkMhEiRo3//+rv//+qrf//6GQyESNGjv//2q///+qu///YVCIRI0af//+qrf///a///6dTIhEjV5///6qq7///r///l1MhEiNXr///qqqv//////6WQyESI1"
"ff//6pia7//////oZDIRIkWO///Zh4mv/////9hUIhEiRo///6h2aJ3/////p1MhESNGj///l2VWiv////+WQyERI0aP//+GVEVor////oZDIREiRt///nZE"
"RFeO///+dTIREBJF7//9ZUMzRXj///1kMhEQEjT///ZUMiM0Vt//9lMhEQASI0VmVUMiIiNFZnZlQyERABEiNERDMiESIjNEREMyERAAERIiIiIhERERIiMz"
"IiEREAABERESEREREREREiIREREAAAAAAAAAAAAAAREREREREAAAAAAAAAAAAAARERIiIiEREAAAAAAAAAAAABERIjMzMyIREAAAAAAAAAABEREiNEVVQyIR"
"AAAAAAAAABEREiM0Vu7u4yERAAAAAAAAERESI0Vn///kMhEQAAAAAAERESIjRW3//9ZDIREAAAAAARERIjNFbf//h1QyIREAAAABERIjNEVt//6YdUMiERAA"
"AAEREiNEVniaqqqHZUMiERAAAREiM0Vnia3d2qqHZDMhEQABESI0Vnjf/////+2XZDIhEQERIjRXjf////////+XZDIRERESNFef///////////XVDIRERI0"
"V8////////////+XUyEREiNXn////9qqqu////+GQyERI0aP///+qZiImq////11MhEiNX3///2od2Zniq///+dTIRI0aP///qh2VVVniu///4ZCISNX3///"
"qHVUREVnmv//+GQyIkWO///qdlQzNEV5r///lkMiNGj///2XVEMzNFaK///5ZDIjRp///6hlQzMzRXmv//+GQyI0af//+oZUMzNEV53///hkIiNGn///qHVE"
"MzRWiu///nUyEjRp///6l2VERFZ5r///x1MhIkaP///ah2VVVniv///4ZDIRI1jv//+ph2Znea7///11MhESNX3///+qmIiZrv///oZDIREjRo/////sqq3v"
"////hlMiEREjVp////////////+HVCIRERI0V9///////////odUMhEQERI0V47////////9hlQyEREBESI0Vo3v/////th2VDIREQABESI0VWeInMmId2VD"
"IhERAAABERIjRFZmZmZlVDMiEREAAAABERIjM0REREQzMiIREQAAAAABEREiIjMzMyIiERERAAAAAAABERERIiIiIhERERAAAAAAAAAAAAAAAAERERERERAA"
"AAAAAAAAAAARERIiIhERAAAAAAAAAAARERIjMzMiERAAAAAAAAABERIjNFVEMiERAAAAAAABEREiNFbu7uMhEQAAAAARERERI0Vv//5DIREQAAAREREREiNd"
"///VQyIREQABEREiIiI03//3ZUMyIREAERIiMzMzPf/+dmVUMyIRAREiNEREQzRVZmZmZVQyERESI0VmZVRERFVWZ3dlQyEREjRv//9kQzREVt///1MhESJF"
"f///dUMzNFff//5UIREjRt///3VDMzRo7//9ZCIRI1fv//51QzNFaP//+GQyEjRo7//9dUMzRXn///hkIhI0aP//+XVDM0V9///4ZCISNXn///l1QzNGjf//"
"6FQiIjV9///5ZUM0Vo7//9dTIiJFjf//6GVDNFef//+XUyEjRo7//9hkQzRXn///lkMhI0af//+oZEREaN///oZDISNXn///qGVERWjf//6GQyEjV9///6dl"
"REV57//9dTIhI1ff//6oZURWiv//+XUyESNX7//+qGVVZ5r///hkMhEjV+///ql2Zniu///nVCIRI1fv//+qh3eJr///l1MhESNG3///2qmZqv///oZDIREi"
"Ro////7Kqt////llMhEQEjV+///////////XVCIREBI0aO/////////+dkMhEQARI0aO////////12QyIREAESI0Z57////+2HVDIhEQABESI0VniJ2Zh3ZU"
"MiERAAARESI0RWZmZmVUMyIREQAAARESIjNERERDMyIRERAAAAARERIiIzMzIiIRERAAAAAAABERESIiIiEREREAAAAAAAAAAAABERERERERERAAAAAAAAAA"
"EREiIiIiIRERAAAAAAAAEREiMzMzMzIhERAAAAAAARESM0RERERDMhERAAAAABERIjTe1VVe5UMhEQAAAAERESNO//xm//9TIhEQAAARERIjX//9d///VDIR"
"EQABERIiNF7/92f//mQyIREAERIjM0RW7mZmzudlQyIRAREiNEVVVVVVVWd3dlQyIRESI0VndmVUREVWeIdlQyEREjRv//9lRENEVt///1MhESJFf///dUMz"
"NFff//5UIhEjRt///3VDMzRo7//9ZCIRI1fv//51QzNFaP//+GQyEjRo7//9dUMzRXn///hkMhI0aP//+XVDM0V9///4ZCISNXn///l1QzNGjf//6FQiIjV9"
"///5ZUM0Vo7//9dTIiJFjf//6GVDNFef//+XUyEjRo7//9hkQzRXn///lkMhI0af//+oZEREaN///oZDISNXn///qGVERWjf//6GQyEjV9///6dlREV57//9"
"dTIhI1ff//6oZURWiv//+XUyESNX7//+qGVVZ5r///hkMhEjV+///ql2Zniu///nVCIRI1fv//+qh3eJr///l1MhESNG3///2qmZqv///oZDIREiRo////7K"
"qt////llMhEQEjV+///////////XVCIREBI0aO/////////+dkMhEQARI0aO////////12QyIREAESI0Z57////+2HVDIhEQABESI0VniJ2Zh3ZUMiERAAAR"
"ESI0RWZmZmVUMyIREQAAARESIjNERERDMyIRERAAAAARERIiIzMzIiIRERAAAAAAABERESIiIiEREREAAAAAAAAAAAAAABEREREREQAAAAAAAAAAERESIiIh"
"ERAAAAAAAAABERIiMzMyIREAAAAAAAARESI0RVRDIhEAAAAAABEREiNFbu7uMhEQAAAAERERIzRn///kMhEQAAABEREiM0bf//1kMhEQAAARESIjNF3//4dU"
"MiERABEREiM0Vd//6IdkQyERARESIzRVZ4iZmIdlQyIRAREiNFZ3iZqqqZiHVDIRERIjRXje7/7aqu7u5UIhESI0Z9//////2v//9kMhESNGfv///////v//"
"5kMhEjRX7///////////11MhEjVs////7Kzv////l1MhIkWP///6qqqt////lkMhI0af//+qmZmq////lkMhI1fv//6ph4iq7//+hkMhI1fv//yod3ia///9"
"hUIhI1j///qod3ia///6dTIhI1j///qod3iq///5dTIRI1j///2piImt///5ZDIRI1fv//6qqqqv///oZDIRI0bf///qqq3////XUyIRIkWP//////////+X"
"UyEREjRo//////////+GQyEREiNWj//////v//91MhEQESNFaN///+qv//5UMhEQERI0VneIiIiId2VDIREAEREiNEVmZmVVVUQyIREAARESIjNEREREMzIi"
"ERAAABEREiIiMyIiIiIREQAAAAERERESIiIhEREREAAAAAAAAAAAAREREREREAAAAAAAAAERESIiIhERAAAAAAAAERESIzMzIhEQAAAAAAEREiNEVUQyERAA"
"AAAAEREiNFbu7uMhEAAAAAEREiNGf//+QyEQAAAAEREiNF3//9VDIREAAAEREiNF3//4ZUMhEQAAERIiNF3//odlMyERAAERIjNFZ5mZh2VDIREAERIjRWeJ"
"qqqYdUMiEQERIjRXie7//tmGVDIRERIjRnnv/////+hlMiEREjRo3////////oZDIREjRX3/////////51MhESNXn///6qqt///4ZCISJGj///2qqqrf//1k"
"MhI0bP//6qqqqq///nUyEjV+///93d3d3///dTISNY7///////////5kMhJFj////////////mQyEkWP///d3d3d3d3WUyESNY///6qqqqqqmHVDIRI1fv//"
"6qqqqqmHVDIhEjRt///9qqqt/4ZTIhESJFfv////7///1kMhEREjRo/////////1QyERESI1aO///////9UyERAREiNWeO////7XVDIREAERIjRWeIiId2VD"
"IREAARESI0RVZmZVQzIREQAAEREiIzNERDMyIREQAAABERESIiIiIiEREQAAAAABEREREiEREREQAAAAAAAAERERERERAAAAAAARERIiIhERAAAAABERIjMz"
"MiERAAAAERIjRFVEMhEQAAAREiNFbu7uIhEAAAERI0Vv//5DIRAAAREjRd///VMiEQABERI03//3VDIREAAREjTf/+dlMyEQAAERI0Z4h2VDIREAAREjRWiI"
"dlMiERAAERI0bu7uZTIhEQAREiRX///2UyIREAERI0aP//51MiEQABESNX3//+dTIREAARIkV+///XUyERAAESNGj///lkMhEQABEjRp///4ZDIREAARI1ef"
"//6GQyEQAAEiNX3//+hUIhEAABIkWO///XUyIRAAASNGj///l1MhEQAAEjRp///4ZDIREAABI0af//6GQyERAAASNX3//+dTIhEAAAEjV+///XUyERAAABI0"
"b///hkMhEQAAASNG///3UyERAAAAESNf//5UMhEQAAABEjRVZlQyERAAAAAREjNEQzIhEQAAAAEREiIiIhERAAAAAAEREREREREAAAAAAAAAAAABERERIhER"
"EREQAAAAAAEREiIiIiIiIREQAAAAARESM0REMzMyIhEAAAABERI0VWZlVURDIREAAAEREjRc7+13Zu5DIRAAABESNF3///6Y3/QyEQAAERIjRv///////lMh"
"EAARESNF3/2q////ZTIRAAERIzVt7pqq7/6GUyIQAREjNWeJmaqqqphlMiEBESI1Z4mqqqqqqYZUMhAREjRu7u6q3v/+2XVDIQESJFf//+3/////6XUyIREj"
"Ro//////////+GQyERI1ff//////////1kMhEiRY7////t3e///+dTIRI0aP///9qqqu///4UyESNGn///2qmarf//+FMhEjV5///6qIia3//+hTISJFff//"
"6od3it///nUyEjRo7//9l2Zorv//11MhI0aP//+odmaK///5dTISNGn///l2VWiv//+WQyEjRp///pdVVor//+hkMhI1ff//6GVFaN///nUyISNX7//9dURG"
"fv//11MhEjRv//+GVERX///4ZDIRI0b///dUM0Vv//91MhERI1///lQzM0X//+VDIRESNFZmVDMiM0VmZUMhERESM0RDMiIiIzREMyIRERESIiIiIRESIiIi"
"IREQARERERERERERERERERAAAAAAAAAAABEREREREQAAAAAAAAABEREiIiIREQAAAAAAAAERESIzMzIhEQAAAAAAARESI0RVRDIREAAAAAABERIjRW7u7jIR"
"AAAAABEREiNGf//+QyEQAAAAEREiI0bf//1UMhEAAAARESIzRd//92VDIREAABERIjNF3//odkMiERAAERIjNFZ4mZmHZDMhEQAREiNFZ4maqql2VDIhEBES"
"I0Vone//7ql2UyIRERIjRXjv//////l2QyERESNFff////////11QiERIkV9//////////hkMhESNWj////arO////dUIRIkWP///6qqqt///4ZDISNGn///"
"qpiJqv///WQyEjV+///qmHeJrv//11MhI1fv//yod3ea3//9dTISNY///6qHZ3mu///XQyEjWP//+qh3eJr///lkMhI1j///2piImt///4ZCISNX7//+qpmq"
"r///11MhEjRt///+qqrf///4ZDIRIkV+//////////hlMiERI0aP/////////XVDIRESI1aO///////odUMhERESI1Z87////sdlQyEREBESI0VniIiIdlQz"
"IhEQARESI0RVZmZVRDIhERAAARESIjM0REMzIiEREAAAARERIiIiIiIiEREQAAAAABERERIiIRERERAAAAAAAAAAAAERERERERAAAAAAAAAAABEREiIiEREA"
"AAAAAAAAARESIzMzIhEQAAAAAAABERIjNFVEMiEQAAAAAAARESI0Vu7u4yERAAAAABEREiNFb//+QyERAAAAERERIjRd///VQyIREAABEREiIzTf//dlQyIR"
"EAARESIjM03//nZlQzIhEQERIjNEREVWZmZlVEMhEQERI0VWZVVVVmZ3ZlQyERESNF7u7mVEVWfu7uQyEREiNX///2VEVXz//+VCEREjRt///nVUVn3//9ZD"
"IREjV9///XZUVo7//9ZDIRIkWO//+XZVV5///4ZDIRI0aP//+XZVZ5///4ZCIRI0ef//+XZWaM///oZCIRI1ff//6XZmed///XUyISI1jf//2XZmiu///XUy"
"ISJGjv//qYd3mv//+XUyESJGj///qod4qv//+WQyESJGj///qpmarv//6GQyESJGj///2qqq3///2FQiERI1j////arO////11MhERI1fv//////////lkMh"
"ERI0aP//////////hkIhEBEjVt//////3//+dTIREBEjRWjv///ar//9VDIREBESNFZ4iIiIiHdlQyERABERIzRVZmZlVVVEMhERAAEREiMzREREQzMyIREQ"
"AAARERIiIjMiIiIiEREAAAABERERIiIiIRERERAAAAAAAAABERERERERERAAAAAAAAARESIiIiIhEREAAAAAAAERIjMzMzMyIREAAAAAABESI0RERERDMhEQ"
"AAAAAREiNN7VVV7lQyERAAAAEREjTv/8Zv//UyEREAABERIjX//9d///VCIREAARESI0Xv/3d//+VDIREQAREiM0Vu52Zs7nVDIhEQERIjRVZmZlVmd3ZUMh"
"ERESI0Vnd2ZVVWd3dlQyERESNF7u7mVURWfu7uUyEREiRX///3VUVXz//+VCIREjRt///nZUVn3//9ZDIREjV9///XZVVo7//9ZDIRIkWO//+nZVZ5///4ZD"
"IRI0aP//+XZVaJ///4ZDIRI0ef//+XZWaM///oZCIRI1ff//6XZmed///XVCISI1jf//2XZmiu///XUyISJGjv//qYd3mv//+XUyESJGj///qod4qv//+WQy"
"ESJGj///qpmarv//6GQyESJGj///2qqq3///2FQiERI1j////arO////11MhERI1fv//////////lkMhERI0aP//////////hkIhEBEjVt//////3//+dTIR"
"EBEjRWjv///ar//9VDIREBESNFZ4iIiIiHdlQyERABERIzRVZmZlVVVEMhERAAEREiMzREREQzMyIREQAAARERIiIjMiIiIiEREAAAABERERIiIiIRERERAA"
"AAAAAAEREREREREREAAAABERESIiIiIRERAAAAEREiIzMzMiIREQAAEREiM0RERDMiERAAEREiNFVmZlRDIhEAAREiNF3d3d3VQyEQARESM1b/////VDIRAB"
"ERI0V/////9lMiEAERIjVo/////mUyIQEREjRXn////9dUIhAREiNGff////l1MiEBESI0aN////+GUyIQERIjVo7////oZDIhAREjRXnv///9hkMiERESNF"
"ef////l1QyIRERI0V5////+XVDIRERIjRXn////YZTIhEREiNGjf///8hkMiERESI0aN////l1QyIRERIjRo7///6HVDIREREiNWjv///YZDIhERESI1aP//"
"/5dUMiERERIjVo////hlQyEREREiNGj///52QyIREBESI0Z////XVDIhEQERIzRt///3ZDMhERARIjNGeIiHZUMiEREBEiNFZ4iHdlQyIREAESI0VniIdlQz"
"IREQARI0Vn3u2HVDIhERABIjRX////dkMiEREAEiNW/////mUyIREAASI1b/////ZTIhEQABIjXf////9kMiERAAEiNc/////mQyEREAARI0b////2VDIREA"
"ABEjRW7//2VDIhEQAAESI0VWZmVDMhERAAAREiM0RVRDMhERAAABERIiMzMzIhEREAAAARESIiIiIhEREAAAAAEREREhEREREAAAAAABEREREREREAAAAAAA"
"ABERERESIhERERERERERERERERAAAAAREREiIiIiIiEREREREREREREREAAAERESIjMzMzMiIhEREREiIiIiIREQABEREiM0RVVUQzMiIiIiIjMzMzIhEQAR"
"ESIzRVZmZmVUMyIiIiM0RERDMhERARESM0Vn3u7tdlVDMiIzNFXd3d0iERAREiNFbf/////XZUMzM0RW3///QyERARIjRX3//////+dlRDRFZs///1QyERAR"
"I0Vt////////h2VERWeP///VQyERASI0Z///6qrf//6GVVVnj///1lMiERASNFfv//qqqu//+XZmd4///+dUMiERASNGf//9qqqt//+Yd3ie///nZUMhERAS"
"NGj//6qZqt//+piInv//6HVDIhERAiNW3//6qZqt//+qma3///h2VDIhERAiNX7//6qZqt//+qqt///5hlQzIhERAiNX7//6qqqu//6qrP//+pdlQzIhERAS"
"NW3//6qqqv//2qr///2odlRDIiERESNG3//9qqrv//qq///9qodmVDMiERESNFf///2q7//9qv///qqph3ZUMyIRESI1bv///////qrv//6qqqmIdlQzIRES"
"I0V+//////6q7//+qq3u/+12VDIhERIzVn3////9qt///6qu/////3ZUMhERIjRWeJ3dqqrf//+qr///////dkMiEREiNFZniZqqr///qq///+7///51QyER"
"EiM0VWeJqq///9qu///Kqu//92QyERESIzRVZ4qv///aqv//2qqq///WQyERESIjNFZ5rv//6qrv//qqqq///WUyEBERIjNFaJ7//+qqr//+qpmq///WUyIB"
"ERIjRWeO///6qqr//9qpmq///WUyEBERIjRWjf//+pmZr//6qpmq///GQyEBERI0Vn3///mIeJr//6qpqt//92QyEBESI0V4///8h3Z4n//9qqqu//51QyEB"
"ESI1Z////YdmVnj//+qqrf//xlMiEBESNFb///12ZVVWfv//7d7//+dUMhEBESNF7//+dlRERFaP///////3ZDIhEBESNO///nZUQzNEVo//////52VDIREB"
"ESPv//9lVDMyM0RWfv///9dlQyIREBESI0RVVUMzIiIjNFZnfHd2VEMiEREBERIjNEQzMiIiIiM0RVZmVVQzIhERAAEREiIzMiIhERESIjM0RERDMiIRERAA"
"ARESIiIhEREREREiIjMzMyIhEREQAAAREREREREREREREREiIiIiEREREAAAAAEREREREREREREREREREREREQAAAAAAAAAAEREREREREREAAAAAABERERIi"
"IiEREQAAAAABERESIjMyIiEREAAAABEREiIzREQzIhERAAABEREiM0VVVUQyIREAAAEREiNEVt3d3UMhEQAAEREiNFZ+////QyERAAEREiNFZ+////1DIhEA"
"ARESI0Z9////5kMiEQARESI0V4////92QyERABESI0Vo7////XVDIREBERIjRnn////4ZUMhEQERIjRXjv///5dkMiEREREiNGfP///+h1QyEREREiNFeO//"
"//qGUyIRERESI0Z5////6XVDIhEQERI0Vo7////IZUMhERARIjRnn////5dkMiEREBEiNWjf///9h1QyIREAESNFef////qGQzIREQASI0Z5////6XVDIhEQ"
"ABIjRo3////YZUMiERAAEjRWjv///6hkMiEREAASNFef////l2QyIREAACI0Z5////6XVDIhEQAAIjRo3////YZTMhERAAAiNWjv///6hkMiERAAACNFee//"
"//l2QyIREAAAI0V5////+XVDIhEQAAAjRXn////pdUMiERAAACNFef///9l1QyEREAAAI0Z5////2HVDIREQAAAjRnr////YZUMhEQAAACNGjP///9hlMiER"
"AAAAI0aN////2GUyIREAAAAjRo3////YZTIhEQAAACNGfP///9hlMiERAAAAI0V5////2GUyIREAAAAjRXn////YZTIhEQAAACNFef///+hlMiERAAAAIjVo"
"////6GUyIREAAAAiNGjv///4dTIhEQAAABI0Z9////h1MiERAAAAEjRXz///+HUyIREAAAASI0aP///9ZTIhEQAAABEjRX7///5kMiERAAAAESI0bf///1Qy"
"EREAAAAREjRW7u7uQzIREAAAABESI0RVZlRDIhEQAAAAEREiM0REQzIhEQAAAAABERIiMzMyIhERAAAAAAARERIiIiIRERAAAAAAAAERERERERERAAAAAAAA"
"AAAAAAEREREREREQAAAAAAABERESIiIREREAAAAAAAEREiIjIiIRERAAAAAAEREiM0RDMyIREQAAAAAREiNFVVVDMiERAAAAAREiNN3d3VQzIREAAAABESNF"
"////1kMiEQAAABERI0Xv///3VDIRAAAAEREjRt////hlMiEAAAAREiNGj////XVDIQAAABESI1aP///+hkMiAAAAERIjVo7///+GUyIAAAAREiNGjf///5dU"
"MgAAABESI0aN////l1QyAAAAERIjRoz////HVDIAAAAREiNGiv///9hkMgAAABESI0aK////2GQyAAAAERIjRor////YZDIAAAAREiNGiv///9hkMgAAABES"
"I0aK////2GQyAAAAERIjRor////YZDIAAAAREiNWiv///9hkMgAAABESI1aN////2GQyAAABERI0Vo3///+nZDIAAAEREjRXnv///5dUMgAAAREiNFee////"
"l1QyAAABESI0Z5////+XVDIAAAERIjRor////oZTIgAAEREjNWjf///9hkMiAAAREiNFed////l2QyIAABESI0V5////+XVDIQABERIjVor////oZUMhAAER"
"IjRXjf///9hkMiEAAREiNFee////l2QyIQARESM1aK////6HVDIRABESI0V43////YZDIhEBERIjRnn////5dUMiEQERIjRWjf///+hlMyERAREiNFef////"
"l2QyIRERESM1aN////6GVDIRERERI0V5////+XZDIhEQERIjRo3////oZUMhERAREjRXj////5dUMiEREBESNFfv///9dkMyEREAERI0bP///+hlQyIREAAR"
"EjRv////hlQyIREQABESNO////dlQyIREQAAERI07u7u1lQzIhERAAAREiNFVmZVQzIRERAAABERIjREREMyIRERAAAAARESIzMzMiIRERAAAAABEREiIiIi"
"ERERAAAAAAARERERERERERAAAAAAAAAAAAAAEREREREREAAAAAAAAAAAABEREREREREREAAAAAAAAAABERESIiIiIRERAAAAAAAAABEREiIzMzMiIREQAAAA"
"AAAREREiM0RVRDMiEREAAAAAARERIiNFVmZlQzIhERAAAAARERIiNFbv//9UMiEREQAAARERIiNFZ////mVDIhERAAARERIiNEZ4///+dkMyIREQABERIjNE"
"V4z///2GVDMiERABERIjNFZ4nf//+pdlQzIhEQERIjRVZ4mu///6qHZUMyERERIjRWeJqq////qqh2VDIhEREjRd3d3d7////d3d3VQyERESNG//////////"
"////VDIRESJFz/////////////9kMhERIkXf/////////////2QyEREiRe/////////////+VDIRESI1//////////////1TIhEREjRWeaqq///9qqmHZUMh"
"ERESI0Vniq3///qph2ZUMiEREREjNFZ5rv//+ph2VEMiEREBESIzRWif///5hlRDMiEREAEREiM0V4///+h1QzIiEREAABERIjRWf///12QzIiEREQAAERES"
"I0Xf//92VDIiEREQAAABEREjNN7u7mVDIhEREQAAAAARESIzRVZVRDIhEREQAAAAAAEREiM0REQzIhEREAAAAAAAABERIiMzMiIhEREAAAAAAAAAARERIiIi"
"IREREAAAAAAAAAAAERERERERERAAAAAAAAAAERERERERERAAABERESIiEREREAARERIiIiIiEREQEREiIzNDMyIRERERIjNFVVRDMhERERIzRWfGZUMiERES"
"I0Z///9lQyERESNFf////2UzIREiNW7////9VDIREjRX/////+ZDIREjRX/////9ZDIREjRX/////4ZDIREjRX3////nVDIREjRXnv//+HVDIREjRXnv//12"
"QyIREjRXn///hlQyEREjRX7//8dUMiEREjRX///nZTMhEREjRe//92VDIhEREjRf//1lQyIREREiPf/+VUMiERERESM0VVRDIhEREBESIzMzMyIRERABERIi"
"IiIiEREQAAERERIiEREREAAAAREREREREQAAAAABERESIiIiEREREAABEREiIiIiIiIhERABERIiMzMzMzMiIREQERIjNEVVVVRDMiERERIjRFZmd2ZlRDIh"
"EREjRd3d3d3d3VMyERESNF////////1DIRESI13////////EMhERIjXf///////2QyEREiNe////////VDIRERI0////////5UMhEREjRVZ3iId3ZUMiERES"
"I0RVZmZlVEMiERAREiM0RERERDMiEREBERIiMzMzMzIiEREAARERIiIiIiIhEREQAAEREREREREREREQAAABEREREREREQAAEREREiIREREQARERIiIiIiER"
"EREREiMzMzMiERERESM0RVVEMiERERIjRWbGZUMiEREiNFb///ZUMhERI0Vv////ZDIhESNG7////9VDIREjRv/////lQyERI0b/////1UMhESNG/////2Uy"
"IREjRd////5kMiERIjRd///mVDIRERIzRWZmVUMiERESIzRFVEMyIRERESIjMzMzIhERARERIiIiIhEREQARERESIRERERAAARERERERERAAAAAAAAAAAAAA"
"AAAREREREREREQAAAAAAAAAAAAAAAREREiIiIhEREAAAAAAAAAAAAAARERIiMzMyIhERAAAAAAAAAAAAARERIjNEREMyIREAAAAAAAAAAAARERIjRVZmVDMh"
"EQAAAAAAAAAAARERIjRW7///0yIRAAAAAAAAAAABERIjRW3///9TIhEAAAAAAAAAABERIjRWj////VQiEQAAAAAAAAAAEREiNGff///2VCIRAAAAAAAAAAER"
"EiNFeP///8ZTIhEAAAAAAAAAAREiNFaO///+dkMhEQAAAAAAAAARESI0Z5////l1QyERAAAAAAAAARESI0Vo7///6GUyIREAAAAAAAABERIzRnn///+XVDIh"
"EQAAAAAAABERIjRXjv///oZTMhERAAAAAAAAERIjNWjf///5dUMiERAAAAAAAAEREiNFef///9hlQyEREAAAAAAAAREiNFaN////l1QyIREQAAAAAAARESI0"
"V5////2GVDIhEQAAAAAAABESI0Vo3///+XVDIhERAAAAAAABERIjRnn////IZTMiERAAAAAAAAERIjRWjv///odUMiEREAAAAAAAEREiNGef///5dkMyEREA"
"AAAAAAEREiNFeO///+h1QyIREAAAAAAAARESM1aJ////l2QyIREQAAAAAAARESI0V47///6GVDIhEQAAAAAAABESIzVo3///+XZDIhERAAAAAAABERIjRXn/"
"///YZUMiERAAAAAAAAERIjRWjf///5dUMiEREAAAAAAAEREiNFef///9hlQyIREAAAAAAAAREiNFaN////l1QyIREQAAAAAAARESI0Z5////2GUzIhEQAAAA"
"AAARESI0Vo7///6HVDIhERAAAAAAABERIjRnn///+XZDMhERAAAAAAABERIjRXjv///odUMiERAAAAAAAAEREjNGif///5dkMiEREAAAAAAAEREiNFeO///+"
"hlQyIREAAAAAAAARESI0aN////l2QyIREQAAAAAAABESI0V4////2GVDIhEQAAAAAAAAERIjRn3///+XVDIhERAAAAAAAAAREjRXj////YZTMiERAAAAAAAA"
"ABEiNGff///4dUMiEREAAAAAAAAAESI1aP///9dkMiEREAAAAAAAAAARI0Vu///+hlQyIREQAAAAAAAAABEjRX////h1QyIREQAAAAAAAAAAESNF7///52Qy"
"IREQAAAAAAAAAAARIjX///92VDIhERAAAAAAAAAAABEiPN3d3VRDIhERAAAAAAAAAAAAERIzRVVVQzIhERAAAAAAAAAAAAAREiIzREMzIhEREAAAAAAAAAAA"
"ABEREiIzIiIhEREAAAAAAAAAAAAAARERIiIiEREREAAAAAAAAAAAAAAAERERERERERAAAAAAAAAAAAAAAAAAAAABEREREREiIREREREQAAAAAAAAEREREiIi"
"IiIiIhEREQAAAAAAAREREiIjMzMzMyIiEREQAAAAAREREiIzRERFREQzMiEREQAAABEREiIzRFVmZmZlVEMiIREQAAERESIzRVZ3iIiIh2ZUMyIREQABERIj"
"NFZ4ne///+2HZUMyIREAEREiNEV4nv///////YdkMyERARESI0Vnjv/////////4dkMiEQERIjRWee///////////4dUMiERESM0Z5/////////////oZTIh"
"ERIjRXjv/////////////XVDIRESNFaO/////+qq3v////+GUyIRIjRnn/////qqqqrv////l1QyESNFef////+qqZmqr////9dkMhIjRo3////9qYiImq7/"
"///oZDISNFef////6ph3d4mt////6GQyIjRnn////8qHZmZ5rf///+hlMiI1aO////+pdmVWeK3////oZTIjRXn////9qHZVVnit////6GUyI0V5////+phl"
"VVZ4rv///+hlMiNGjf////qXZVVWeK7////YZDIjRo3////qh2VVVnmv////2GQyI1aO////6odlVVZ5r////5dkMiNWjv///9qHZVVnit////+XVDIjVp//"
"///ah2VVZ5rv///+hlMiI1af////2odmZnia/////YZDIiNWj////9qYdmeJrv////l1QyEjRo7////aqHd4mq/////oZTIhI0aO////+qmYiarv////l1Qy"
"ISNFff////2qqqqu/////oZTMhEjRXn/////2qqq7/////l1QyIRIjRo7//////u7/////+XZDIhERI0V5/////////////9hlQyIRESI0Z9////////////"
"2GVDIhERESNFaO///////////IZUMiEREBEiNFaN/////////odlQyIREQAREiNFZ43//////ph2VDIhEREAEREiNFZ3id3d2Yh2VEMiEREQABEREiNEVmd3"
"d3dmVUMyIRERAAABEREiM0RFVVVVVEMyIhEREAAAABEREiIzM0REQzMyIhEREQAAAAABERESIiIzMzIiIhERERAAAAAAABEREREiIiIiIRERERAAAAAAABER"
"ESIiIiIiERERABERIiIzMzMyIiERERERIjM0REREMzIiERERIjNFVVZlVUQzIRERIjRWZ3eHd3ZUMyEREjRd3d3d3d3dxDIhEiNG/////////VQyESJFf///"
"/////9VDIRI0Xf////////hkMhEjRe////////+GQyESNF7////////oZDIRIjVszMz////9hkMhEiNFeJqv////2GQyERI0Vnmq////+nZDIREiNFaKr///"
"/5dUMhESI0Vorf///+l1QyERIjNWiu////2WVDIREiM0aK/////YZTIhERIzVor////6hkMiEREjRWiv////l1QyIREiNFeN////6XVDIRESI0V57////YZU"
"MhERIjRXn////9hlMiEREiNGiv////qGQyIRESI0aK////+XVDIhERIjVo3////pdUMhEREjRXnv///9hlMyERESNFef////2GQyIRERI0V5////+XZDIhER"
"EjRnn////5dUMhERESNGjf///+hlMyERARI0aO////2GQyIREBEjRo/////XVDIhEQESNGj////4ZUMhERARI0V/////dkMiERABEjRd////5lQyEREAESI0"
"7////VQyIREAAREjRFZnZlQyIREQABESIzRFVUQzIREQAAEREiIzMzMyIRERAAABEREiIiIiIRERAAAAARERESIhERERAAAAAAAAAAEREREREiIiERERERAA"
"AAAAAAAREREiIiIiIiIiEREREAAAAAABEREiIjMzMzMzIiIREREAAAAAERESIjNERFVUREMyIhEREAAAARESIjNFVmZmZmVUQzIhERAAABERIjNFVneIiIiH"
"dlRDIhERAAAREiM0VnjN7///7Zh2VDIhEQAAERIzVnjv///////+h2VDIhEAAREiNGfv//////////2GUzIRAAERI0Vv////////////52QyIQABESNF////"
"//////////x1QyEAAREjT//////////////+dkMhAAERI0X////9qqqu/////4ZDIQABESNFbv/9mZmqqu/////GUyIAAREiNFbf2IiIiaqv////11MiAAAR"
"EiNFVmZmd3iqr////5dTIgAAEREjNEVVVWZ4mq////+GUyIAABERIiM0RFVWeKrf////hkMhAAABERIiMzRFVnmq/////XVDIQAAARESIjM0RWeJrv////h1"
"QyEAAAEREiIzNFZ4mu/////XZDIhAAARERIiM0Vnia7////+hlQyEQAAEREiIzRWeJr/////6XVDIhEAARERIjNFZ4nf/////pdlMyERABEREiM0Vniu////"
"/9l2VDIhEQARESIzRWea//////2YZUMyIREBERIjNFaJ3/////6ph1RDIhEREREiM0V4nf/////qqHZUMyIRERERIzRniu/////9qph2VDMiERAREiNFeK//"
"////qqqYdlQzIhEQESI0V43/////6qqqqYdlQyIREBEjRWjv/////aqqqqqYdkMiERARI0Z+//////////////9UMhEQEiNW///////////////+VDIREBIk"
"V////////////////VQyERASNF3///////////////1UMhEQEiNe///////////////2UyIREBEjTv//////////////9UMhEQARI0VneImZmZmZmZmIdlQy"
"IREAESI0RWZnd3d3d3d2ZlVDMhERABESIzREVVVVVVVVVURDMiIREAARESIjMzREREREREMzMiIRERAAARERIiIiMzMzMzMiIiIREREAAAAAAAAREREiIiIi"
"IiIiIiIREREAAAAAARERIiIzMzMzMzMzMiIhERAAAAARESIjNEREREVERERDMyIREQAAABESIzRFVWZmZmZmZVVEMiERAAABERI0RWd3iIiIiIiHdmVDIhEA"
"AAERIjRt3d3d3d3d3d3d3VQyEQAAAREjRe//////////////VDIRAAABESNG//////////////9kMhEAAAERI1b//////////////mUyEQAAAREjXf//////"
"///////+ZDIRAAABESNO//////////////dkMhEAAAERI0XMzMzMzMz/////l1QyEQAAAREiNFZ4mqqqrv////mGQyIRAAABESIzRWeJqqrv////qHVDIhEA"
"AAAREiNEVniqrv///+qXZDMhEQAAABERIjRWeKrv///+qoZUMiERAAAAEREiNFZ5rv///+qpdkQyIREAAAABESIzVon////+qqh1QzIhEQAAAAEREjRWj///"
"//qqqXVDMiERAAAAARESNFff//////2pdlMyEREAAAARERI0V+///////+qGVDIhEQAAABERIjRX7////////pdUMiERAAABEREiNFb/////////2GQyIREA"
"ABEREiI0Vt3d3v/////4ZTIhEQABEREiIzRVeJqqrv////x1QiERABEREiIzNEVniaqq/////XVDIREBERIiMzREVWZ4mqrv///9dUMhEQERIjNEVVVWZnia"
"qu////11QiEREREjRFZmZmd3iJqq/////HUyIREREiNG79iIiIiZqq7////4ZTIhERESNFf//9qqqqqq7////+dkMhERESI1b/////7t7v//////h1QyERAR"
"IkXv//////////////12QyIREBEiRf//////////////2GUzIREQESI13/////////////yGVDIhEQAREjRX7//////////+h2VDIhERABESM0Z43v//////"
"7Yh2VDIhERAAEREjNFZ3ic3d3ZiHdlQzIhEREAABESIzRVZnd3d3d2ZUQzIhEREAAAEREiIzRFVVVVVVREMyIhEREAAAABEREiIzNEREREMzMiIREREAAAAA"
"AREREiIiMzMzMiIiERERAAAAAAAAERERESIiIiIiIRERERAAAAAAAAAAAAAAAAAAEREREREREREQAAAAAAAAAAAAAAEREREiIiIiEREQAAAAAAAAAAAAARER"
"EiIzMzMiIREQAAAAAAAAAAABEREiIzREREMyIREAAAAAAAAAAAERESIzRFVmVUMyEREAAAAAAAAAARERIjNFZ93d3dMiERAAAAAAAAABEREiNEZ3/////kMh"
"EQAAAAAAAAERESI0Vnj////+VDIREAAAAAAAARERIjRWeO////9lQyERAAAAAAABEREiNFZ57////3ZTIhEQAAAAAAERESI0Vnnv////h1QyIREAAAAAARER"
"IjRWee////+YZUMiERAAAAABEREiNFZ57////9h2QzIhEQAAAAERESI0Vnnf////2YZUMyIREQAAABERIjRWed/////al2VDMiEREAAAEREiNFZ53////+qo"
"dlVDIiERAAARESI0Rnnf////6qmYdlRDIhERABERIjNGea/////qqqmYdlQzIREQARESM0V4r////+qqrd3d1lQyIRERERIjRXiv////+qqq////1lMyERER"
"EiNFaK/////6qqrf///8dUMiERERI0Von/////qqqq7///+oZUMiEREiNGee////+qqqqu////qXZDIhERI0V47////8qqqqr////qqHVDIRESNGjv/////d"
"3d3e/////d3dQyIRIjVu///////////////////VQiESI1f///////////////////dUMhEiRX///////////////////3VDIRIjVv//////////////////"
"91MiERI03//////////////////+ZDIRESNF3d3d3d3d3d3////+3d1lQyEREiNFZneIiJmqqq////+qmGVDIhEREiNEVVZmZ3iJqv////qYZUMyEREREiMz"
"RERVVmeJz////phlQzIhEQEREiIjMzNERWeN////2GVDMiEREAERERIiIjMzRWfv///8dUMyIREQAAERERESIiIzRW////91QzIhERAAAAEREREREiIzRVZ3"
"d2VDMiEREAAAAAABERERESIjRFVVVEMyIRERAAAAAAAAARERERIjM0RDMyIhEREAAAAAAAAAABERERIiIiIiIhEREQAAAAAAAAAAAAARERESIiIREREQAAAA"
"AAAAAAERERIiIiIiIiIiIREREAAAAAABEREiIjMzMzMzMyIiEREQAAAAARESIjNERERERERDMyIhERAAAAEREiM0RVZmZmZmVVRDMiERAAAAEREjNFZ3eIiI"
"iHd2ZUQyERAAABERIjRd3d3d3d3d3d3dQyIRAAABESI0V/////////////VDIRAAARESNFaP////////////ZDIRAAAREiNGff////////////ZDIRAAEREj"
"RXjv///////////+VDIRAAERIjRXn////////////9VDIRAAERIjRo3////czMzMzMzFQyIRAAERI0V57///+qqqqZiHZUQyERABERI0V5////6qqpiHdmVE"
"MiERABESI0aK////qqqZh3ZlRDMhERABESI1aN////qqqph3ZVQzIhERABESNFef///+qqqqmHdlQzIhERABESNFef////7t3aqpdlRDIhERABESNGff////"
"/////smGVDIhERABESNGjv//////////6XZUMhERARESNGf////////////5dUMiERARESNFf////////////+hlMiERARESNF7/////////////11QyERAR"
"ESNFZ4mqqqqt7/////6GQyERAREiM0VniJmaqqqv////+GQyEREREiM0Vmd3iImqqt////+GQyIREREiNEVWZ3d4iaqq////+GUyIRERIjRFZmZ3d3iaqq//"
"//+GQyIRERIzRWd3d3eIiaqt////+GQyERESI0Vv6JmYiZmqqv////51QyERESNFbv/+qqqqqq3/////l1QiERESNF3////+7d7v/////+hkMiERESNG////"
"///////////4ZUMhERESNO//////////////+HVDIhEQESNF7////////////+h2QyIREQESI1bf///////////YdkMyIREAERI0Vnjv///////sh2VDMiER"
"EAERIjRWZ4id3d2ZiHZlQzIhERAAEREiM0VWZ3d3d3ZlVDMiIREQAAARESIzNEVVVVVVVEMzIhEREAAAARERIiIzNEREREMzIiIRERAAAAABERESIiIzMzMz"
"IiIhEREQAAAAAAERERESIiIiIiIhEREREAAAAAAAAAAAABEREREREiIhERERERAAAAAAAAEREREiIiIiIiIiERERAAAAAAEREREiIjMzMzMzIiIRERAAAAAR"
"EREiIzNEREVERDMyIREQAAABEREiIzRFVmZmZmVUQyIREQAAERESIzRVZneIiIh3ZlQyIREAARESIjRFZ4jN7////tdlQyERABERIjNFZ4nv/////////VMi"
"EQEREiM0Vonv///////////UMhEBERIzRXjf////////////VDIREREiNFeN////////////91QyERESI0V43////////////+ZTIhEREjRWjf/////9qqqq"
"ne9lQyERESI0Z5/////+qqqZmIh2VDMhEREjRXnv////2qqpiHd2ZUQyIRESI0aN/////aqqmYh3ZlRDIhEREjRXn////+qqqqmYd2VUMyIRESI0aM////+q"
"qqqqqYdmVDMiEREiNWjv///+qq3e7tqZdlRDIhERI0V5/////a7//////ZhlQyIRESNGiv////7////////5dlQyIREjRo3//////////////5dkMiERI1aO"
"///////////////4ZUMhESNWnv///////u7//////XVDIREjV57/////6qqqrv////+GQyIRI1ef/////qqqqqrf////hlMiESNWn/////qqqqqqr////5dT"
"IhEjVo7////qqpmZqq7////XUyIRI0aO////2qqYiaqu////l1MiESNGjf///+qqmZmqr////4ZTIhEjRXn////6qqmaqt////+GQyIRIjVo7////qqqqqr/"
"///+dkMhERI0V9/////qqqrf////+GVDIRESI1aO/////////////9dkMiEREiNFeP////////////6GVDIREREiNFeP///////////oZUMiERAREjNGeO//"
"///////9hlQyIREQERIjNFZ43//////th2VDIhERABERIjNFZniM3d2Yh2VUMiEREQABERIiNEVmd3d3dmVUMyIRERAAABEREiM0RFVVVVVEMyIhEREAAAAB"
"EREiIjM0RERDMyIiEREQAAAAABERESIiIzMzIiIiERERAAAAAAABEREREiIiIiIhERERAAAAAAABERESIiIiIiIiIiIiIiERERAAARERIiIzMzMzMzMzMzMy"
"IiEREAEREiIzRERERVVVVURERDMyIRERERIjNEVWZmZmZmZmZmVVQzIhERERIzRWd3iIiIiIiIiId2VUMiERESI0Xd3d3d3d3d3d3d3d3VMyERESNFf/////"
"///////////1QyEREiNWz////////////////2QyEREjRX3////////////////mUyEREjRn7////////////////WUyEREjRo////////////////+GQyER"
"EjRo////7d3d3d3/////12QyERIjVs////qqqqqqr////+l1QyEREjVt////qqmqqq7////6hkMiEREjRu///+mYiaqt/////pdUMhEREjRf///9iHiJqv//"
"//+oZDIhEREiNN3d3Yd3eJrv////2HVDIhERESNFVmd2Znear////+l2QyIRERESIzRVVVZnia/////6hlQyIREBERIjNERVVnit/////ZdUMiEREAEREiIz"
"RFVnmv////6XZDMiERAAERESIjNEV4rv////qGVDIhERAAARERIjNFZ53////+l1QyIREQAAARERIjNFeK/////5hlMyIREAAAABERIjRWee/////YZUMiER"
"EAAAABERIjRXiv////6XZDIhERAAAAABESIzRon/////mGVDIhEQAAAAABESI0V43////9h1QyIREQAAAAARESM1aJ/////pdkMiEREAAAAAARESNFee////"
"+oZUMiEREAAAAAAREiNGjf////2HVDIhERAAAAAAAREjRXj/////l2QzIhEQAAAAAAAREjRn7////8hlQyIREQAAAAAAAREjRt/////odUMiEREAAAAAAAAR"
"EjRv////+HVDIhERAAAAAAAAAREjTv////12QzIhERAAAAAAAAAREjT////+dUQyIREQAAAAAAAAAREjNFZ3dmVDMiEREAAAAAAAAAARESM0VVVUQzIhERAA"
"AAAAAAAAABESIjM0QzMiIRERAAAAAAAAAAABEREiIiIiIhEREQAAAAAAAAAAAAEREREiIhERERAAAAAAAAAAAAAAAAAAEREREREiIiEREREREAAAAAAAAAER"
"ERIiIiIiIiIhERERAAAAAAABERESIiMzMzMzMyIiEREQAAAAABEREiIzNERFVUREMyIhEREAAAABEREiM0RVZmZmZmVUMyIhERAAAAEREiM0VmeIiIiId2VU"
"MyIREQAAEREiNFZ4je7///7Zh2VDMiERAAEREiNFZ47////////9h2QzIREAAREiNFaN///////////4dkMiEQABESM1aO/////////////HVDIRABESI0Z9"
"//////////////hkMiEAERIjVo//////3c3v/////XVDIQEREjRX3////9qqqqr/////dkMhARESNGj////+qqqqqt////+GQyEBESI1aP////qqqqqqr///"
"/4ZDIQERI0V5////+qqqqqqv////hkMhEREjRXn////6qqqqqt////6GQyEREiNFee////6qqqqq/////XVDIRESI0V53////+qqqt/////5dUMhERIjRnmu"
"/////////////6hlMiERIjNWiqr////////////8l2QyIREiNFeKrv///////////qqXVDIhESM0aJ3////////////+qpdUMiERI0V47//////////////q"
"h1QyIRIjRo3/////7Kqq7/////2XVDIREiNXj////+qqqqqq/////5dUMhESNFff////qqqqqqqv////l1QyERI0aO////2qqZmaqq/////XVDIREjRo////"
"/KqpiJmqr////9dUMhESNGj////8qqmImaqv////11QiERI0aP////2qqZmaqt////+GUyIREjRX7////6qqqqqq/////4ZDIRESJFff/////aqqqt/////9"
"dUMhERIjRo///////+7//////+hlMiERESNFeP//////////////h1QyERERIjRnj////////////+h1QyIREBESM0Z47//////////9h1QzIREQERIjRFeJ"
"3///////7YdlQzIREQARESIzRWd4md3d3JiHZVQzIhERAAEREiM0RWZ3d3d3dmVUMyIhERAAABERIiM0RFVVVVVVRDMiIRERAAAAARERIiMzNEREREMzIiIR"
"ERAAAAAAERERIiIjMzMzIiIiERERAAAAAAABEREREiIiIiIiERERERAAAAAAAAAAERERERESIhERERERAAAAAAAAABERERIiIiIiIiIREREQAAAAAAARERIi"
"IzMzMzMyIiERERAAAAAAERESIjNERERERDMyIhEREAAAABERIiM0RVZmZmZVRDMiEREQAAARESIzRVZ3iIiIh2ZUQyIRERAAEREiM0Vnid7///7Yh2VDIhER"
"AAERIjNFeN////////6HZUMiEREBERIjRnjv/////////9hlQyIREBESI0V4////////////6GVDIRERESNFeP/////////////YZDIhEREiNGjv/////t3e"
"/////5dUMhEREjRXn////+qqqqrv///+hlMiEREjRn7////qqqqqqv////l1QyEREjRo////+qqpmaqt////2GQyERIjVo////+qqZiaqt////6GQyIRIjVp"
"////+qqZmaqt////+WUyIRIjVo////+qqpmaqu////+XUyIRIjVo/////aqqqqqv////+XVCIREjRo/////6qqqqr/////+XVCIREjRX7/////7Kre//////"
"+XVCIREjRXj///////////////+XUyIREiNGff//////////////6GUyIREiNFaN//////////////2GQyIRESI0V4nv//////6v////qGQyERESI0Vnid7/"
"///srf////l1QyERESIzRWeJmqqqqqrv///+hlMiERESIzRVZ4iZqqqqr////6hkMiERESIzRVZniJmqqq/////pdUMhERESM0VWZ3iJmqqu////+oZDIhER"
"EiNFVneIiZqqrv////6HVDIREREjRW3YmZqqqt//////l2QyIREREjRX//7d3d7//////5hlMyERERIjVu////////////+YZUMiERARIkXf////////////"
"mGVDIhERARIkX////////////pdlQyIREQARIjX//////////+mHZUMiEREQAREjRd7///////6YdlRDIhEREAAREjNFZ4zd3dmYh2VUMyIRERAAAREiM0VW"
"d3d3d2ZVQzIiEREQAAABESIzNEVVVVVVRDMiIREREAAAABEREiMzNEREQzMyIiERERAAAAAAERESIiIzMzMiIiIREREAAAAAAAARERESIiIiIiIRERERAAAA"
"AAAAAAAREREREREREAAAABERERIiEREREAAAERESIiIiIhEREAARERIjMzMzIhERAAEREjNEVVRDIhEQARESI0VmxmVDIhEAERIjRW///2VDIRABESNFb///"
"/2QyIQAREjRu/////VQyEBERI0b/////5UMhARESNG/////9VDIQEREjRv////9lQiEBERI0Xf///+ZDIhARESNFbf//5lQyEQEREiNFZndmVDIhEBERIjNF"
"VVVUMyERARESIzNEREQzIhEREREiMzREQzMiERERESIjM0RDMyIhEREREiM0RERDMiIRERESI0RVVVRDIiEREREjNVZ8dlVDIhERESI0V///9lQyIREBEjRW"
"////9lMyERARI0bv////1UMhEQESNG/////+VDIREBEjRv/////VQyERARI0b/////ZTIhEQESNF3////mQyIREBEiNF3//+ZUMhEQAREjNFZmZVQyIREAER"
"IjNEVUQzIhERABERIiMzMzMiEREAABEREiIiIiERERAAABERERIhEREREAAAABEREREREREAAAAAAAAAAAAAAREREREREREREAAAAAAAAAARERERERERERER"
"EQAAAAAAABEREREREiIiIiIRERAAAAAAERERERIiIiMzMzIiERAAAAARERERIiIjMzREREMyIREAABERERIiIjM0RFVWZV0zIREAAREREiIjM0RVVmd33v9D"
"IREAERESIjM0RVZneI3v//5DIhEBEREiM0RVZneI3v////1TIhEBERIjNFVneJnv//////ZTIhERESI0VmeJnv////////VDIREREiNFZ4nf/////////WVD"
"IREREjRWje/////////YdlQyIRERIjVu/////////amHZUMyERERI0V////////qqYh2VDMiERARI0aP/////qqqmHZlQzIhERARI0bf///9qqqqmHZUQzIh"
"ERARI0bv////7aqqmHZUQyIhERARI0b///////2qmHdlQyIREQARI0Xe///////+2ZdlQzIREQARIjRXjv///////+l2VDIREQAREjNFeJ3v///////tVDIh"
"EQAREiM0VniZ7///////5DIhEQARESIzRFZ3id//////1TIhEAABERIiM0RWZ4ie////ZDIREAAAERESIjNEVWZ4jf//VDIREAAAARERIiIzREVWZ33vQyIR"
"EAAAABERESIiMzREVVVUMyEREAAAAAERERESIiMzNEQzMiERAAAAAAABEREREiIiIjMiIhEQAAAAAAAAARERERESIiIiEREQAAAAAAAAAAEREREREREREREA"
"AAAAAAAAAAAAEREREREREQAAAAAAAAARERERERERERAAAAAAAAAAABEREREREREREREAAAAAAAAAERESIiIiIhEREREQAAAAAAABESIjMzMyIiIhERERAAAA"
"AAERIjNEREQzMyIiEREREAAAABESNE1mZlVEQzMiIREREAAAERIjTf/od2ZVRDMyIhEREAABESNF7//+2IdmVUQzIiEREAAREjRf/////ZiHZVRDIiEREAER"
"I0X//////+2YdmVDMiERABESNF7////////Zh2VDMhEQAREjRWff///////+mHVDIhEBERIjRWed7////////XVDIRARESI0Vmiarf///////8QyEQEREiM0"
"Vniaqs7/////91MiEBERIjNFZniaqqrf////dUIhERESI0RWeJmqqq7////3UyIRERIjNFZ3iaqu//////5lMiEREiM0VniZrv///////9ZDIRERIzRWeJ3/"
"///////+1lQyEREiNFZ9/////////9h2VDIhERIjXP/////////ZiHZUMiERESNF7///////6Zh3ZUQyIREREjRf/////+yYd2ZUQzIhERARI0X////tiHdm"
"VUQzIiEREQESNF//7Yd3ZlVEMzIiERERABEiPf1mZmVVRDMyIiEREREAAREjNEVVREQzMiIiEREREAAAERIjMzMzMzIiIhEREREQAAABERIiIiIiIiIRERER"
"EQAAAAABERESIiIREREREREAAAAAAAABEREREREREREREAAAAAAAAAAAERERERERERAAAAAAAAAAAAAAABERERERIiIhERERERAAAAAAEREREiIiIiIiIiER"
"ERAAAAARERIiIzMzMzMzIiIRERAAABEREiIzRERVVERDMiIRERAAEREiIzRFVmZmZmVUMyIhERARESIzRVZ3iIiIiHZlQyIREQEREjNFZ4ne///+7IdlQyIR"
"EBESM0V43////////9h1QyIRERIjRX3///////////h1QyERESNFbv////////////hlMiEREjRf/////////////+dUMhERI07//////////////4ZDIRES"
"NM////7aqqrf////+GQyEREjRW7//ZmZqqrf////1lMhEREjRW34iIiImqr////4ZTIQERIjRVZmZneJqq////+GQyEBERIjRFVVZneKrf///+hkMhABESIj"
"NERVZ4mq/////XVDIQARERIjM0RWeJr////+hkMiEAARERIjNFV4nP////6GVDIRAAERESIzRWed/////odUMiEQAAEREiM0Z47////9h2QzIREAABERIjRW"
"jv////mHVDMiERAAARESI0V+////6YdUMyIREQAAEREiNG3////phlQzIhERAAABERIjRv////h2VDMiEREAAAARESM0b////XZUMyIREQAAAAERIjRWeImI"
"dlQzIhEREAAAARESI0VniIh2VDMiEREQAAAAEREjRWeIiHdlQyIhERAAAAABESI0Vo3u2HVDMiEREAAAAAAREiNW3///51QyIRERAAAAAAERI0V/////1kMi"
"EREAAAAAABESNF7////+ZDIhEQAAAAAAAREjRe/////mQyEREAAAAAAAERI0Xv////1UMhERAAAAAAABESI1b////mQyIREAAAAAAAARESNFz//+ZUMhERAA"
"AAAAAAAREiNFZmZVQyIREAAAAAAAAAEREiNEVURDIhERAAAAAAAAAAEREiMzMzMiEREAAAAAAAAAABEREiIiIiIRERAAAAAAAAAAABEREREhEREREAAAAAAA"
"AAAAABEREREREREAAAAAAAAAAAAAAAAAAAAAARERERIiIREREQAAAAAAAAAAAAAAAAARERIiIiIiIhEREAAAAAAAAAAAAAAAARERIiMzMzMyIRERAAAAAAAA"
"AAAAAAARESIjNEVVVEMyIREAAAAAAAAAAAAAARESIzRWZ3dmVDMhERAAAAAAAAAAAAARERIzRWfd3d3VQyIREQAAAAAAAAAAABERIjRXj/////dUMiERAAAA"
"AAAAAAABERIjRXjv////+GUzIREQAAAAAAAAABERIjRWjf/////9dkMiERAAAAAAAAAAERIjNGef//////6GUyIREAAAAAAAAAEREiNFee///////5dUMhER"
"AAAAAAAAEREiNFaM////////qGQyIREAAAAAAAARESM0aJ/////////YZTIhEQAAAAAAARESI0V57///+v///+l1QyERAAAAAAARESI0Vor////q7///+XVD"
"IREQAAAAABESIzRon////6rf///6hkMiERAAAAABERIjRXnf///9qq////2GUyIREAAAABERIjRWiv///+qqr////pdUMhERAAAAERIjNGee////qqqu////"
"l1QyIREAAAEREiNFed////2qqq7///+oZDIhEQAAEREiNFaK////+qqqrf///9hlMiERAAAREiM0Z57////aqqqq////6XVDIREAARESI0V53////qqqqqr/"
"///5dUMiEQABESI0Vor////6qqqqqu////qGQyIRABERIjRnnv///+qqqqqq7////YZTIhEAERIjRXjf///////////////+l1QyEQEREjNWif//////////"
"//////+XVDIRAREiNFee/////////////////6hkMiERESI1aK//////////////////2GQyIRERI0V5///////////////////oZTIhERIjVo7////6qqqq"
"qqqqrf////l1MiEREjRXn////6qpmYiImZqq////+XVCIRESNGf////+qYd3dmZ3iJrv///9dUIhESI1bf////qHZlVVVVVnid////11QiERIjVv////2HZU"
"RERERFVoz////mUyIREiNe////6HVUMzMzMzRFaP////ZDIhESI9////92VEMyIiIiM0RW/////EMhEREjNFZndlVDMiIiIiIiM0Vmd2ZUMiERESIzRVVVRD"
"MiIRERESIjNEVVVEMyEREREiMzNDMzIiERERERESIjMzMzMiEREBERIiIiIiIhERERERERESIiIiIiEREAARERESIiERERERAAEREREREiIREREQAAAAARER"
"EiIiIiIiIiIiIhERERAAAAAAARESIiMzMzMzMzMzMiIiERERAAAAARESIzNERERERERERDMzIiEREQAAARESI0RVVmZmZmZmZVVEQzIhERAAARESI0Vmd4iI"
"iIiIiHd2ZUMyIREQABESI0Vt3d3d3d3d3d2Zh3ZUMiERABERIjVt/////////////+h2VDIhEAEREjRX7//////////////+hlQyEQAREiNWj///////////"
"/////nZDIhARESNFef////////////////11QyEBESI0aN/////////////////4ZDIQERIjVo3////qqqqqqq7/////hkMhAREjRXnv///9qqqqqqqu////"
"+GUyERESNFef////qqqZmaqqr////5ZTIhESI0aK////+qqZmZmqqv////hlMiERIjVo3////qqqmZmqqt////+GQyIREjRXnf///+qqqqqqqq/////YZDIR"
"ESNFee////2qqqqqqt/////pdUMhESI0Z6/////t3d3d7v/////6hlQyERIjRor////////////////ql2QyIREiNWjf//////////////+qqXVDIhESNFed"
"///////////////+qpdUMiERI0V57////////////////8l1QyIRIjRnr/////7u7u7u//////+XZDIhEiNGiv////qqqqqqqq3////9hkMiESI1aN////6q"
"qqmaqqqt////6GQyIRI0V53////qqpmIiZqqr////4ZDIhEjRXnv///9qpmIiImqqv////hkMhESNFef////qqmYiImaqt////+GQyERI0Z6////+qqpmZma"
"qq/////nVDIREjRo3////qqqqqqqqq/////9dUIhESNGjf///+3d3d3d3v//////hkMiERI0aO//////////////////h1QyEREjRo//////////////////"
"l2QyIRESNGf////////////////+h2QzIRERI0Xf///////////////sh2RDIhEQEiNN////////////7t2IdlQzIhERARI0VneImZmZmZmYiId2ZVQzIhER"
"ABEiNEVmZ3d3d3d3dmZlVEMyIhERAAERIjNERVVVVVVVVVREQzMiIRERAAARESIjMzNEREREQzMzMyIiERERAAAAERESIiIjMzMzMzIiIiIhERERAAAAAAAA"
"AAABERERESIiIiIREREREQAAAAAAAAARERESIiIiIiIiIiEREREAAAAAAAEREREiIjMzMzMzMzIiERERAAAAAAERERIiMzRERVVVREQzIiEREQAAAAERESIj"
"NEVVZmZmZmVUQzIhERAAAAERESIzRFZneIiIiIh3ZUQyIREQAAERESIzRWd4nd7///7tiHZUMiERAAERESI0VWeN7/////////12VDIhEQARESI0Vnjf////"
"////////dlMyERARESI0Vnnv/////////////9VDIRERESI0Von////////////////UMhERESI0Vo3/////////////////QyERERIzVnn//////+2qqs7/"
"///+ZDIRERIjRXn//////qqZiImZ3//+ZUMhEREjRWjv////+qmHd3d3eI/9ZUMiEREiNGfP////+ph2ZVVVVmZtZUMyERASNFeO////+ph2VURERERVVEMy"
"EREBI0V5/////ah2VEMzMzMzMzMiEREQIjRo7////6l2VDMzIiIiIiIiEREQAjNXn////9qGVDMyIiIiIiIhEREQACNFef////qXZUMyIiIhIREREREQAAI0"
"Z9////+pdUMyIiIRERERERERAAAjRo3////qh1QzIiIhERERERERAAACNGjv///+qHVDMiIiEREREREREAAAI0aO////6odUMzIiIiIhEREREQAAAjRo7///"
"/ql2RDMyIiIiIiERERAAACNGjf////qXZUQzMzMzMzIiEREAAAI0V9////+qh2VEQzNEREQzIhERAAAjRXn////+qYdlVURVVVVUQyIREAACI1aP/////amH"
"dmZmZ3f+ZUMhERAAEjRn3/////2qmIh4iIz//mUyIREAASNFeP/////+yqqZms7///1UMhEQABIjRn3///////7u7v/////9QyERAAESNFaO////////////"
"////5DIREAARIjRXjv//////////////5UMhEQABESI0V43/////////////1lQyERAAEREjNFaJ7//////////+hlQyIREAARESIzRWeJ3v//////7YdlQy"
"IREAAAEREiI0VWeImd3d3JiHZlQyIREQAAABEREiM0VWZ3d3d3dmVUMyIRERAAAAARERIiM0RVVWZlVVRDMyIRERAAAAAAERERIiMzNEREREMzIiIRERAAAA"
"AAABERERIiIjMzMzMiIiERERAAAAAAAAABEREREiIiIiIiIRERERAAAAAAAAAAERERIiIiIiIiIiIiEREREQAAAAAAAAEREiIjMzMzMzMzMyIiIREREQAAAA"
"AAEREiMzREREREREREMzMiIREREAAAAAEREiNEVVZmZmZmZlVURDMiIRERAAAAEREiNFZneIiIiIiHd3ZlRDMiEREQAAAREiNFbd3d3d3d3d2ZiHdlRDIiER"
"EAARESI1bf///////////+2YdlQyIREQABERI0V+//////////////6XZUMiEREAERIjVo////////////////2GVDIhEQEREjRXn////////////////+hl"
"QyIRAREiNGjf/////////////////YZTIhEBESI1aN////7d3d3d7///////l1QyIQERI0V57////aqqmZmqrv/////4ZTIhEREjRXn////6qZiHeImq3///"
"//11QyEREiNGiv////qodmZmd4mt/////oZDIhESI0aN////+pdlVVVWeJrv////hlMiERI0Vo3////qhlVERFVnmt////+XVCIREjRXnv///9l2VERERFaK"
"r////5dUMhEiNFef////qXVEMzNEVnmv////l1QyESI0aK////+oZUMzMzRWea////+XVDIRIjVo3////6hlQzMzNFZ5r////5dUMhEjRWnf///+l2VDMzM0"
"Vnmv////l1QyESNFee////2XZEMzMzRWit////+XUyISI0Z5////+pdUQzMzRFeK7////oZTIhIjRor////6l1VDMzRFZ5r////9hkMiEiNWjf////qHZURE"
"RFZ4rv////l1QyESM1ad////6pdlVEVVZ4qv////6GUyIRI0V57////amHZlVmZ4qv////+XVDIhEjRXn////6qph3d3eJqv/////oZTMhESNGev////qqqZ"
"iImarf/////5dUMiERI0aN////+qqqqqqt7//////5dkMiEREjRo3//////////////////8hlQyIRESNGjv/////////////////5hlQyIRERI0aP//////"
"///////////pdlQyIREQEjRn/////////////////YdlQyIREQASNF3//////////////+yHZVQyIRERABIjTf///////////u2Yh2ZUMyIRERAAESNFZ3iJ"
"mZmZmZmIiHdlVEMiIRERAAARIjRFZmd3d3d3d2ZmVURDMiIRERAAABESIzREVVVVVVVVVUREMzIiERERAAAAEREiIzMzREREREMzMzMiIhEREQAAAAABEREi"
"IiIzMzMzMiIiIiEREREQAAAAAAAAAAERERIiIiIiIiIiIiIhEREQAAAAARESIiMzMzMzMzMzMyIiEREQAAABERIjM0REREREREREQzMiIREAAAEREiNEVVZm"
"ZmZmZmZVVEMyIREAARESI0Vmd4iIiIiIh3d2ZUMyERAAERIjRW3d3d3d3d3d3d3d3UMiEQARESI1bf//////////////9UMhEAEREjRX7///////////////"
"VDIRABESI1aP///////////////lQyEQEREjRXn///////////////1UMhEBESI0aN///////////////9UyIRAREiNWjf///+zMzMzMzMzMxUMiEQERI0V5"
"7////aqqqpmZiHdlQzIRERESNFef////qqqZiIh3ZlVDMiERERIjRor////6qqmIh3dmVUMyIRERESI1aN////6qqpiId3ZVRDIhERAREjRXnf///+qqqpmI"
"h3ZUMyIREQERI0V57////aqqqqqZh2VDMiEREBEiNGiv/////u7u7u7u7uVDIREQARIjRor//////////////lQyEREAESI1aN//////////////1UMhERAB"
"EjRXnf/////////////3VDIREAARI0V57/////////////9lMiERAAEiNGiv/////u7u7u7u7uVDIhEQABIjRor////6qqqqqpmHdlQyEREAASI1aN////6q"
"qqmYiHdmVDIhERAAEjRXnf///+qqmYh3dmVUQyIREQABI0V57////aqpiHd2ZlVDMiEREAASNFef////qqqYiHd2ZVRDIhERAAEjRnr////6qqqZmIh3ZlQz"
"IREQABI0aN////6qqqqqqZmIdlQyIREAASNGjf////7u7u7u7u7u7kMiERAAEjRo7///////////////1TIhEQABI0aP///////////////9UyIREAASNGf/"
"//////////////9lMiERAAEjRd////////////////ZDIREAABIjTf///////////////lQyERAAARI0VneImZmZmZmYiId2VDIhEQAAESI0RWZnd3d3d3d2"
"ZmVUMiEREAABESIzREVVVVVVVVVUREMyIREQAAARESIjMzNERERERDMzMyIhERAAAAARERIiIiMzMzMzMiIiIhEREQAAAAAAAAERERIiIiIiIiIiIiIhEREQ"
"AAAAARESIiMzMzMzMzMzMyIiEREQAAABERIjM0REREREREREQzMiIREAAAEREiNEVVZmZmZmZmZVVEMyIREAARESI0Vmd4iIiIiIh3d2ZUMyERAAERIjRW3d"
"3d3d3d3d3d3d3UMiEQARESI1bf//////////////9UMhEAEREjRX7///////////////VDIRABESI1aP///////////////lQyEQEREjRXn/////////////"
"//1UMhEBESI0aN///////////////9UyIRAREiNWjf///+zMzMzMzMzMxUMhEQERI0V57////aqqmZmIh3ZlQyIRERESNFef////qqmYh3d2ZVRDMhERERIj"
"Ror////6qoh3ZmZVREMyIRERESI1aN////+qmHdmZVVEMzIhERAREjRXnf///+qpiHdmZVRDMiEREQERI0V57////aqpiId2ZVQzIhERABEiNGev////qqqq"
"mYh3ZUQyIREQARIjRor////9qqqqqpmHZUMiEREAESI1aN//////////////1DIhEQABEjRXnf/////////////2QyIREAARI0V57/////////////9kMhER"
"AAEiNGev//////////////ZDIREAABIjRor//////////////lQyERAAASI1aN/////d3d3d3d3dVDIhEQAAEiNWjf///+qqqZiId2ZUMyERAAABI0V57///"
"/aqYd3ZmVUQzIhEQAAASNFef////qpdmVVREQzIiERAAAAEjRXn////6l2VURDMyIiEREQAAABI0Z9////+oZUQzMiIiERERAAAAASNGjf///+l2RDMiIhER"
"EREAAAAAEjRo7////YZUMiIhEREREAAAAAABI0Z/////l2QzIiEREREAAAAAAAASNFf////4ZUMiEREREAAAAAAAAAEiNd////91QyIRERAAAAAAAAAAABIj"
"Tf///+VDIhEREAAAAAAAAAAAARIzRWZ2ZUMyIREQAAAAAAAAAAAAESIzRFVUQzIhERAAAAAAAAAAAAABESIjMzMzIiEREAAAAAAAAAAAAAARERIiIiIiEREQ"
"AAAAAAAAAAAAAAARERESIhERERAAAAAAAAAAAAAAAAAAAAAAAREREREiIiIiIREREREAAAAAAAAAARERESIiIiIiIiIiIREREQAAAAAAABERESIiMzMzMzMz"
"MiIhEREQAAAAABERESIjM0REVVVVREMzIiEREQAAAAERESIjNEVVZmZmZmZVRDMiEREAAAARERIjNEVmd4iIiIiHdmVDMiEREAABEREiM0VmeJze////7th2"
"ZUMiERAAERESI0VWeN7/////////6HZUMhERABERIjRWeN/////////////nZTIhEQEREiNFZ57//////////////8VDIRERESI0Von////////////////9"
"QyERERIjRWjf/////////////////0MhERESM1Z5///////tqqqt7////+ZDIRERIjRXn//////qqZmIiZrf//5lQyERESNFaO/////8qYh3d3d4iP/XVDIh"
"ERIjRnz/////qod2ZVVWZnd3ZUQyERESNFeO////+qh2VVRERVVWZVRDIhEREjRXn////9qHZUREREREVVVEMyIRESI0aO////+pdlRDMzM0RFVURDIhEREj"
"NXn////9qHVEMzMzNEVVVUQyIhEQI0V5////+pdlQzMzM0RVZmZUQyIRECNGff////qXZEMzMzNFZ3d3ZUMiERAjRo3////qh1RDMzM0Vn////1DIhEQI0aO"
"////6odUQzMzNFff///2VDIRECNGjv///+qHVEMzM0Vn7///91QyERAjRo7////ql2VEMzRFeO///+dUMhEQI0aN////+pdlREREVon////XVDIRECNFff//"
"//qodlVEVWeK////11QyERAjRXn////+qYdmVWZ4nf///5dUIhEQIjVo/////9qYd3d3ia7///+GUyIREBI0Z9/////9qpmIiZqv///+hkMiERASNFeP////"
"/+2qqqqq7////YZDIREQEiNGff///////u7u//////11QyEREBEjRWjv///////////////4ZTIhEQARIjRXjv//////////////91QyEREAERIjRXjf////"
"/////////XZDIhERABERIjRWee///////////odlQyEREAARESIjRWeJ3v//////7Yh2VDIhERAAARESIjRFZ4iZ3d3ZmId2VDMiEREAAAARERIjNEVmd3d3"
"d3ZlVEMyIREQAAAAARERIiM0RVVVZlVVREMyIhERAAAAAAAREREiIzM0REREQzMyIhEREQAAAAAAARERESIiIzMzMzIiIhEREQAAAAAAAAABEREREiIiIiIi"
"IRERERAAAAAAAAAAARERERERERERAAEREREREREREQAAAAARERIiIiIhEREREREREiIiIhEREAAAARESIjMzMiIhERERERIiMzMyIhERAAARESIzREREMiIR"
"ERERIjNEREMyIREAARESI0RWZlVDMiERESIjRVZmVEMiEQABERI0Vt3d3dQyIhESIjRd3d3dVDIRABERIjRt////5UMiIiIjRW////9kMiEAEREjRX7////W"
"QzIiIjNWf////2UyIQAREiNGj////4ZUMiIjNFff////dUMhARESNFef////h1QzIjNFaN////51QyEBESI0Z9////+XVDMzM0V57////XVDIQERIjVo3///"
"/pdlQzNEVon////5dUMhAREjRXnv///9l2VEREVXiv////l1QyERESNFef////qYZlVVVWid////+XVDIRESI0aK////+qh2ZmZmea3////pdUMhERIjVo3/"
"///6qYh3d3iarv///9hlMiEREjRXnf///+qqqZmZmqqv////qGQyIRESNFee////2qqqqqqqqq////+oZDIhESI0aK///////////////////5dUMhERIjRo"
"r//////////////////+l1QyEREiNWjf//////////////////2XVDIRESNFed//////////////////+oZTIhERI0V57//////////////////6hkMiERIj"
"Rnr///////////////////p2QyIREiNGiv////qqqqqqqqqu////6XVDIRESI0aN////+qqpmZmaqq7////ZdUMhERIjVo3////qqYd3d4iqr////6hkMiER"
"EjRXnv///9qXZmZmd5qv////qGQyIRESNFef////qYZVVVVnit////+XVDIRERI0V5////+odVRERFaJ3////pdUMhEQEjRn3////5dlRDNEVnnv///9hkMi"
"ERASNGjf///+l1RDMzRGef////l2QyIREBI0aO////2GVDMzM0V5////+XVDIREQEjRn////+XVDMiIzRX3////4ZTIhEQASNFf////4ZUMiIiNFbf///+dU"
"MhERABIjXf////dUMiIiIjRu////1lMiEREAEiNN////5UMiIREiNF////9lQyIREAARIzRWZ2ZUMyIRERIjRWZ3ZlQyIREQABEiM0RVVEMyIRERESI0RVVU"
"QyIREQAAERIiMzMzMiIREREREiMzMzMyIREQAAARERIiIiIiERERERERIiIiIiIRERAAAAEREREiIRERERERERERESIhERERAAAAAAAAAREREREREREQAAAA"
"ERESIiIiIRERAAABERIiMzMyIhERAAARESIzREREMiERAAEREiNEVWZVQyIRAAEREjRW3d3d1DIhABERIjRt////5UMhABERI0V+////1UMhABESI0aP////"
"hkMhARESNFeP////hkMhAREiNGff////hkMiAREiNGjf///+hkMiAREjRXjv///9hkMiEREjRXn////6dkMiERIjRnn////5dUMhERIjRo3////5dUMhERIz"
"Vo3////pdUMhERI0V57////YZTIhESI0V5////+oZDIhESI0aK////+XZDIhESI0aN////+XVDIhESNFaN////6XVDIRESNFee////2GUyIREiNFef////qG"
"QyIREiNGiv////l2QyIREiNGjf////l1QyEREiNWjf///+l1QyEREjRXnv///9hlMiEREjRXn////6hkMiEREjRXn////5dUMiEREjRX3////5dUMhEREjRo"
"3////oZTIhEQEjRn7////XZDIhEQEjRn////+XVDIREQEjRX////+GQyIREAEiNd////9lQyEREAEiNN////5UMiEREAESM0VmdmVDMhERAAESIzRFVUQzIh"
"ERAAERIiMzMzMiEREQAAERESIiIiIhEREAAAARERESIhERERAAAAAAAAAAAREREiIiIiIiIiIREREAAAAAABEREiIjMzMzMzMiIhEREAAAAAAREiIzNERERE"
"REQzIiERAAAAABERIzRFVWZmZmZVRDMhEQAAAAAREjNFZneIiIiHd2VDMhEAAAABESI0Xd3d3d3d3d3dxDIhAAAAAREjRe///////////9VDIQAAAAERI0b/"
"///////////VQyEAAAABESNG////////////hkMhAAAAAREjRv///////////4ZDIQAAAAERI03///////////6GQyEAAAABESNFzMzMzMzv///9hkMhAAAA"
"AREiNFZniaqq7////YZDIQAAAAAREiNEVmeJqv////p2QyEAAAAAEREiM0VWear////5dUMhAAAAAAEREiM0VXit////6XVDIQAAAAABEREiM0Vorv///9hl"
"MyEAAAAAABERIiNFaK7////YZDIhAAAAAAARESIjRWiv////qGQyIQAAAAAAEREiI0Vor////5dUMiEAAAAAARERIiNFed////6XVDIRAAAAABERESIzRXnv"
"///9hlMyEQAAABERERIiM0Z57////YZDIhEAAAEREREiIzRWiv////qGQyIRAAEREREiIjM0V4r////5dUMhEQARERIiIzMzRWed////6XVDIREAEREiMzNE"
"RFV4rv///9hlMiERARESI0RFVVVmea7///+nZDIhERERIjRW1mZmeJqv////l1QyEREREiNFbv2IiImq7////oZTIhEQERI0Vu//6qqqrf////12QyIREBES"
"NG7////t3f/////4ZUMhERAREjTv////////////12QyIREAERI07////////////oZTMhERABESNF7//////////+hlQyIREAAREjRW3/////////2GVDIh"
"ERAAERIjRWff//////2HZUMiEREAABERIjRWd4nd3cmHZlQyIRERAAABERIjRFZnd3d3ZlRDIhEREAAAARERIjNEVVVVVURDMiEREQAAAAARERIiMzREREMz"
"MiIRERAAAAAAAREREiIiMzMyIiIREREAAAAAAAARERESIiIiIiEREREQAAAAAAAAAAERERERERERERERERERERERERAAAAAAARERIiIiIhERERERIiIiIiIR"
"ERAAAAABERIiMzMzIiIRESIiIzMzMyIhERAAAAEREiM0RERDMiIiIiIzREREQzIhEQAAARESI0RWZmVEMyIiMzRVZmZVQzIREAAAEREjRW3d3d1UMzMzRFZ9"
"3d3d3SIREAARESI0bf///+ZUMzRFZ47////+MiERAAEREjRX7////WVURFVnjv////5TIhEQABESI0aP////l2VVVnif/////mQyIREAEREjRXn////5h2Zm"
"eJ/////9ZUMhEQABESI0Z9////6od3eJ3/////12QyIREAAREiNWjf///+qYiJnf/////HZUMhERAAERI0V57////aqZmt/////5dlQyIREAARESNFef////"
"qqqq7/////mGVDIhERAAERIjRor////6qqrv////6YZUMiEREAABESI1aN////6qqu/////phlQzIhERAAAREjRXnf///+qq/////+mGVDMiEREAAAERI0V5"
"7////ar/////2odUMyIRERAAABEiNGiv////rf/////al2VDIiEREAAAARIjRor////9/////6qoZUMyIREQAAAAESI1ad//////////+qmGVDIiERAAAAAB"
"EjRXnf//////////2phlQyIREQAAAAARI0V57///////////qoZUMiEREAAAAAEiNGiv///////////+qHVDIhERAAAAABIjRor////////////9l2QzIhEQ"
"AAAAASI1aN//////+q/////6hlQyIREAAAAAEjNXnf/////6qt/////pdkMiEREAAAABI0V57////+qqqu/////YZUMiERAAAAASNFef////+qqqqv////+X"
"ZDIhEQAAAAEjRXn////6qpiZrf////6GVDIREQAAABI0aN////6ph3eJr/////l2QyIREAAAASNGjf///+qHZmeJr/////hlQiERAAAAEjRo7////ZdlVWZ5"
"7////9dUMhEQAAABI0Z/////mGVERVaJ/////3ZDIREAAAASNFf////4dURERFaJ/////mQyERAAAAEiNd////52VDMzRFZ+/////UMhEQAAABIjTf///+ZU"
"MyIzNFZ/////9DIREAAAARIzRWZ2ZUQyIiIjNFVnd2ZUMiERAAAAESIzRFVUQzIiIiIiM0RVVUQzIREQAAABESIjMzMzIiERERIiIzNEMzIiERAAAAARERIi"
"IiIiIREREREiIiIiIiEREQAAAAARERESIhEREREREREREiIhERERAAAAAAAAAAEREREREREREAAAAAAAAAAAARERIiIiIhEREAAAAAAAAAABERIiMzMyIhER"
"AAAAAAAAAAEREiM0RERDIhERAAAAAAAAARESI0RVZlVDIhEQAAAAAAAAEREjRW3d3d1DIhEQAAAAAAARESI0bf///+VDIREAAAAAAAEREjRX7////VQyERAA"
"AAAAABESI0aP////hkMhERAAAAAAEREjRXj////4ZDIREQAAAAABESI0Z9////+GQyIREAAAAAAREiNGjf///+hkMiERAAAAAAERI0V47////YZDIhEQAAAA"
"ARESNFef////p2QyIREAAAAAERIjRnn////5dUMhERAAAAABESI0aN////+XVDIREQAAAAAREjNWjf///+l1QyERAAAAAAERI0V57////YZTIhEQAAAAABEi"
"NFef////qGQyIREAAAAAARIjRor////6hkMiEREAAAAAESI1aN////+XZDIhERAAAAABEjRWnf///+l1QyIREQAAAAARI0V57////ZdUMiIREQAAAAEiNGef"
"////qXVDMiERERAAABIjRor////6h1QzIiERERAAASI1aN////+odUQzIiEREREAEjNWnf///+qXZUQzMiIhEREBI0V57////amGZURDMyIhERASNFef////"
"qqh3ZlVUQzIhEREjRnr////6qqmId3ZlVDIhERI0aN////+qqqqZmId2VDIhESNGjf//////////////1DIREjRo7//////////////1QyERI0aP////////"
"//////9kMhESNGf///////////////ZDIREjRd///////////////lQyERIjTf//////////////1TIhERI0VneImZmZmZmYiHZlQyERESI0RWZnd3d3d3dm"
"ZVRDIhERESIzREVVVVVVVVVERDMiERERESIjMzNEREREQzMzMiIREQARERIiIiMzMzMzIiIiIRERAAAAAAEREREREREREAAAAAAAEREREREREREQAAAAARER"
"IiIiIhEREAAAAAAREREiIiIiEREQAAABERIiMzMyIhEREQAAAREREiIzMzMiIREQAAEREiM0RERDMiEREQABEREiIzREREMyIREAARESI0RWZlVDMiERERER"
"ESIzRVZmZUMyERAAERIjRW3d3WVDMiERERERIjNFZ93d3UMiEQARESI1bf///3ZDMiEREREiM0V4////5UMhEAEREjRX7////HVDIiEREiIzRXjv///+ZDIR"
"ABESI1aP////6GVDIiIiIjNFeN/////WUyIQEREjRXn/////l2QzIiIiM0V4r/////hlMiEBESI0aN/////9h1QzIiIzRXiv/////5dTIhAREiNWjf/////6"
"hlQzMzNFaK7/////6WUyIQERI0V57//////JdUQzM0Vonv/////+hlMiERESNGev//////6oZURERWed///////YZDIhERIjRor///////qXZURFZ5r/////"
"//qGQyIRESI1aN///////9qGVVVnmv///////6hkMhEREjRXnf///////6l2ZmeK7///////6XVDIRERI0V57////////ah3d4rf///////+l1QyEREiNGiv"
"///+////6piIms////7////ZdUMhERIjRor////t////qpmar////t////qGUyIRESI1aN////2v///9qqqu///+rv///6hkMiEREjRXnf///6rv///6qq7/"
"//+q////+XVDIhERI0V57///+qr////arf///9qv///+l1QyEREiNFef///+qq7///6q////2qr////ZdTMhERIjRor////qqt////r////qqt////qGQyIR"
"ESI0aN////2qqv////////qqrv///6hkMiEREiNWjf///6qqrf//////+qqq////+XVDIRERIzV57///+qmqr//////9qqqv///+l1QyERASNFef///+qZma"
"7/////6qqqr////YZTIhEQEjRXn////qiIia/////qmZmt////qGQyIREBI0V9////2Yd3if////qYiJrv///5dUMhERASNGff///6h2Znjf//+ph3eK////"
"+HUzIREQEjRn7///+YZVVnj//9h2Znif///+hkMiERABI0V////+h1VEVW3u12ZVVnn////XVDIREQASNFf////nZURERVZmVURFZ9////hlMiEREAEiNd//"
"//1lQzMzREREQzRFbv///3VDIhEQABIjTf///2VDMiIjMzMzMzNF////9UMiEREAARIzRWZmVUMyIiIiIiIiIjNFZmZlQzIREQAAERIzRFVEQyIiEREREREi"
"IjNEVVRDMiEREAABESIjMzMzIiERERERERERIiMzMzMiIREQAAARERIiIiIiEREREREREREREiIiIiIRERAAAAAREREREREREREREAEREREREREREREREAAA"
"AAAAAAEREREREREREREREREREREREREAAAAAERESIiIiIRERERERERIiIiIRERAAAAEREiIzMzMiIRERERESIjMzMiIREQAAEREiM0RERDMiIREREiIzRERD"
"MiERAAEREiNFVmZlVDMiIhIiI0VWZlRDIhEAAREiNFbd3d1lQzIiIiI0XN3d3VQyEQARESI1bf///4dUMyIiM0Vv////ZDIhABERI0V+////6HVDMyM0Vn//"
"//9lMiEAERIjVo/////9h1RDM0RXj////3VDIQEREjRXn/////+YZUQ0RWjf///+dUMhAREiNGjf/////9l2VERVeO////11QyEBESI1aN//////+pdlVVZ5"
"////+XVDIQERI0V57//////+qHZVZ4r////5dUMhEREjRnr////////ah2Znmv////l1QyEREiNGiv////////qYd3id////6GUyIRESI1aN/////////amI"
"ia7////YZTIhERI0V53/////////qpmar////6hkMiEREjRXnv///+/////qqqqv////p2QyIREiNGev////rv////yqqq////+XVDIRESI0aK////+q////"
"/6qq3////pdUMhERIjVo3////qrf////2qrv///9hlMiEREjRXnf///9qq/////6qv////qGQyIRESNFee////2qrf////6q////+oZDIhESI0V5////+qqq"
"7////6r////5dUMhERIjRor////6qqqv/////f///+l1QyEREiNGjf///+qpmq7/////////2XVDIRESI1aN////2piJqv////////+oZTIhERI0V57////a"
"h3ia3////////6hkMiEREjRXn////6l3Z3mu////////l1QyERESNFef////qHZmaJr///////6XVDIREBI0V9////6YZVVniu///////YZTIhEQEjRn3///"
"/YdlRFZ4r//////6hkMiERASNGfv///9hlRERWed//////l1QyEREBI0V/////l1RDM0Vnnv////+GUyIREAEjRX////+GVDMzNFaJ/////nZDIhEQASI13/"
"///nVDMiIzRWjv///9ZUMhERABIjTf///9VDMiIiM0Vn////ZUMiERAAESM0VmZmVDMiIiIjNFZnd3ZUMiEREAARIjNEVVRDMiERESIjRFVVVEMiEREAABES"
"IjMzMzIiEREREiIzNEQzMiEREAAAERESIiIiIhEREREREiIiIiIiEREQAAABEREREhERERERERERESIiIREREQAAAAAAAAAAAREREREiIiIiIREREREAAAAA"
"AAAAAAEREREiIiIiIiIiIiEREREAAAAAAAABERERIiIzMzMzMzMyIiERERAAAAAAABERESIjM0REVVVUREMzIiEREQAAAAABEREiIzRFVWZmdmZmVUQzIhER"
"EAAAABEREiM0RWZ3iIiYiId2ZUQyIhERAAABEREiM0VneJ3u///+7Zh3ZUMyIREQABEREiNFV3je/////////9h2VDMhEREAEREiNFZ43////////////9h1"
"QyIREQEREiNFZ57//////////////odUMiEREREiNFaJ////////////////6GVDIREREiNFaN/////////////////9hlMiERESM1Z5///////tqqre////"
"//+XVDIRESI0V5//////6qmZmZqt/////+hlMiERI0Vo7/////qph3d3iZrf////+XVDIRIjRn3/////qYdmZmZnia3////9hkMhEjRXjv////qYdlVEVVZ4"
"qv////6GQyISNGef////2odlRERERWea7////4ZTIiI0aO////+pdlRDMzNEV4rf////l1MiI0V5/////ahlRDMzMzRWiq////+XUyIjRXn////6l2VDMyIz"
"NFaJr////5dTIiNGff////qXVEMyIjM0VonP////l1MiI0aN////6odUMzIiMzRWit////+WUyIjRo7////qh1QzMiIzNFaK7////oZDIiNGjv///+qHVEMz"
"MzNEZ5r////9hkMiI0aO////6pdlQzMzNEVonP////l1QyEjRo3////6l2VEMzNEVnmu////+HUzISNFff////qodlREREVnit/////YZDIhI0V5/////qmH"
"ZVVVZnia/////5dUMhEiNWj/////2ph3Zmd4mq/////9hkMiESI0aN/////9qpiIiJmt/////+h1QyIREjRXn//////qqqqqre//////l2QyIRESI0Z9////"
"///+7u7///////l2VDIREREjRWjv////////////////mGVDIhERESI0V47///////////////mGVDIhERAREiNFeN/////////////+h2VDIhERABERIzRX"
"ie///////////ph2VDIhEREAEREiM0Vnie///////+2YdlRDIhEREAABERIjNFVniJnd3dyYh3ZVQzIhEREAAAARESIjNFVmd3eId3dmVUMyIhEREAAAAAER"
"ESIjNEVVVmZVVURDMiIREREAAAAAABERESIjM0RERERDMzIiIREREAAAAAAAARERESIiMzMzMzIiIiERERAAAAAAAAAAERERESIiIiIiIiERERERAAAAAAAA"
"AAABERESIiIiIiIiIiIREREQAAAAAAARESIiMzMzMzMzMyIiIREREAAAAAEREiMzRERERERERDMyIiEREQAAABERIjRFVWZmZmZmVVRDMyIRERAAARESI0Vm"
"d4iIiIiId3ZVRDIhEREAAREiNFbd3d3d3d3dyYh2ZUMiEREAEREiNW3///////////7YdlQyIREAEREjRX7/////////////2GVDIhEAERIjVo//////////"
"/////oZUMhEBERI0V5///////////////+hlMiEBESI0aN////////////////11QyEBESI1aN/////d3d3e//////+GQyIBESNFee////2qqqqqrf////+X"
"UyIRESNFef////qqmZmaqt/////XVDIREiNGiv////qpiIiJqq/////XVDIREiNWjf////qph3d4mq7////oZDIREjRXnf///+qYd2Z4mq7////XZDIREjRX"
"nv///9qYd2d4mq7////XVDIRIjRnr////6qYh3eImq////+XVDIRIjRor////6qpiIiZqu////+GUyIRIjVo3////6qqmZmqrf////6GQyIRI0V53////qqq"
"qqqq7/////l1QyERI0V57////+7u7u7//////9hkMiESI0aK/////////////////oZUMhESI0aK////////////////6HVDIhESI1aN///////////////9"
"h1QyIRESNFad//////////////6YZUMyIRESNFee////////////7Zh2VDMiERESNFef////6qqqqqmYh3ZUQyIhERASNFef////qqqZiId2ZVRDMiIREQAS"
"NGjf////qph3ZmVVREMyIhEREAASNGjf///+qXZlVERDMzIiERERAAASNGjv///9l2VEQzMyIiIREREQAAASNGf////5hlQzMyIiIRERERAAAAASNFf////4"
"dUMyIiIREREREQAAAAASJF3////3ZEMiIhEREREQAAAAAAASI03////mVDIiEREREQAAAAAAAAARIzRWd3ZUQyIREREQAAAAAAAAAAARIjNEVVVDMiERERAA"
"AAAAAAAAAAAREiIzMzMyIhEREAAAAAAAAAAAAAARERIiIiIiERERAAAAAAAAAAAAAAABERERIiEREREAAAAAAAAAAAAAAAAAAAAAAAERERERIiIiIiERERER"
"AAAAAAAAAAABERERIiIiIiIiIiIhERERAAAAAAAAARERESIiMzMzMzMzMiIhEREQAAAAAAAREREiIzNERFVVVERDMyIhEREAAAAAARERIiM0RVVmZnZmZlVE"
"MyIRERAAAAARERIjNEVmd4iImIiHdmVEMiIREQAAARERIjNFZ3id7v///u2Yd2VDMiEREAARERIjRVd43v/////////YdlQzIRERABERIjRWeN//////////"
"///YdUMiEREBERIjRWee//////////////6HVDIhERERIjRWif///////////////+hlQyERERIjRWjf/////////////////YZTIhEREjNWef//////7aqq"
"3v//////l1QyEREiNFef/////+qpmZmarf/////oZTIhESNFaO/////6qYd3d4ma3/////l1QyESI0Z9/////6mHZmZmZ4mt/////YZDIRI0V47////6mHZV"
"RFVWeKr////+hkMiEjRnn////9qHZUREREVnmu////+GUyIiNGjv////qXZUQzMzRFeK3////5dTIiNFef////2oZUQzMzM0Voqv////l1MiI0V5////+pdl"
"QzMiMzRWia////+XUyIjRn3////6l1RDMiIzNFaJz////5dTIiNGjf///+qHVDMyIjM0Vorf////llMiI0aO////6odUMzIiMzRWiu////6GQyIjRo7////q"
"h1RDMzMzRGea/////YZDIiNGjv///+qXZUMzMzRFaJz////5dUMhI0aN////+pdlREM0RFZ5rv////l1MyEjRX3////6qHZVRERVZ4rf////2GQyISNFef//"
"//6ph2ZVVWd4qv////+XVDIRIjVo/////9qYh3d3eJqv/////YZTMhEiNGjf/////aqZmJmarf/////pdUMiERI0V5//////6qqqqq3v/////5hlQyEREiNG"
"jf///////u7u///////6h1QyIRERI0Vo7////////////////6l2UzIhEREiNFeO///////////////6mGVDIhERERIjRXjf/////////////qqXZUMiERER"
"ESM0V4nv//////////6qqodlQyIREBERIjNFZ4rv///////tqqqql2VDIhEQARESIzRWeJqv/////9qqqqrudUMiERAAEREiI0RWeJrv/////sqq7//WQyIR"
"EAABEREiM0VWia////////////ZTIhEQAAARERIiNEVniv///////////kMhEQAAAAERESIjNFZ4nv/////////9QyERAAAAABERESIjRFaJ3////////mVD"
"IREAAAAAABEREiIzRWeJ3v////6GVDIhEQAAAAAAARERIiM0RWd4iJmId2VDIhEQAAAAAAAAERERIiM0RVZmd2ZlRDIhERAAAAAAAAAAERERIiM0REVVVEQz"
"IhERAAAAAAAAAAABERERIiMzMzMzMyIhEREAAAAAAAAAAAABERERIiIiIiIiIREREAAAAAAAAAAAAAABERERERIiIREREREAAAAAAAABERESIiIiIiIiIiIR"
"ERERAAAAAAARESIiMzMzMzMzMyIiIREREAAAAAEREiMzRERERERERDMzIiEREQAAABERIjRFVWZmZmZmVVREMyIRERAAARESI0Vmd4iIiIiId3ZVRDIhEREA"
"AREiNFbd3d3d3d3d2Zh2ZUMyEREAEREiNW3///////////7YdlQyIREAEREjRX7/////////////2HVDIhEAERIjVo///////////////oZUMiEBERI0V5//"
"/////////////+hlMiEBESI0aN////////////////11QyEBESI1aN/////d3d3e//////+GUyIBESNFee////2qqqqqrP/////HVCIRESNFef////qqmZma"
"qq/////XVDIREiNGiv////qpiIiJqq7////oZDIREiNWjf////qph3d4mq7////oZDIREjRXnf///+qYd2d4mq7////oZDIREjRXnv///9qYd3d4mq7////X"
"VDIRIjRnr////6qoh3eJqq/////HVDIRIjRor////6qpiIiaqt////+HUyIRIjVo3////6qqqZqqrf////6GQyIRI0V53////qqqqqqq7/////l1QyERI0V5"
"7////+7u7u7//////9hlMiESI0aK/////////////////odUMiESI0aK////////////////6XZDIhESI1aN///////////////9mGVDIhESNFad////////"
"//////6qhlQyIRESNFee/////////////6qpdkMyIRESNFef////2qqq3////9qoZUMiERESNFef////qqqqrv////qXZUMiERASNGjf////qqmaqv////2Y"
"ZTMiERASNGjf///+qYiImt////+oZDIhERASNGjv///9mHZnea/////oZTIhEQASNGf////6h2VWZ53////4ZTIhEQASNFf////5dlRFVnnv///+ZTIhEQAS"
"JF3////3ZURERWeP////1DIhEQASI03////mVEMzNFZ+////9DIREAARIzRWd3ZVQzMiM0RWZ3ZlQyIREAARIjNEVVVEMyIiIjNEVVVEMyEREAAREiIzMzMz"
"IiIiIiIzMzMzIiERAAARERIiIiIiIRERERIiIiIiIRERAAABERERIiERERERERERIiIREREQAAAAAAAAAAERERERIiIiIREREREQAAAAAAAAERERESIiIiIi"
"IiIREREQAAAAAAAREREiIjMzMzMzMyIiEREQAAAAABEREiIzNERFVVREQzMiEREQAAAAERESIzRFVmZmZmZlVEMiEREAAAARESIjRFZneIiIiId2VUMiEREA"
"ABERIjNFZ3id7v///u2HZUMiERAAARESI0Vnjf/////////+ZUMhEQABERIjRXjf////////////5DIREAAREiNFeO/////////////+UyERAAERI0Vo7///"
"//////////9lMiEQARESNGff///////u/////+ZDIREAERIjVo/////+qqqqqt7/91QyERABESNFff////qqqpmYiIjuZDIhEQAREjRo7////aqpmId3d2ZV"
"QyIREAERI0aP////qqqYh3ZmZVRDIhERABESNGj////9qqmId2ZVVEMyIREQAREjRo/////6qqmIdmVUQzIhERAAERI0aO/////+qqqYd2VEMyIREQABESNF"
"ff///////aqph2VEMiEREAAREiNWj/////////7Zh2VDMiERAAERIjRXn//////////9l2VDIhEQABESI0Vnnv//////////l2QyIREAAREiM0Vonf//////"
"////h1QyEREAERIiNEVnma3/////////hkMiERARESIjNFZniaqt7//////9ZTIhEQEREiM0RVZniJqqrf/////nVCIREBESIzRFVVZneJmqqv////51QyER"
"EREiNEVWZmZ3eJqqrf///+dUMhERESI0Vmd3d3d4iaqq3////nVDIREREjRW/9iIiIiJmqqv////11QiERERI0bf//2qqqqqqq7////4ZTIhEREiNW/////+"
"3ard7/////52QyERERIkXf///////////////4ZTMhEQESI1////////////////h1QyIREBEiNN//////////////6HZDIhERAREjRW3////////////YdU"
"MyEREAERIjRWeO////////7IdlQzIhERABERIzRWZ4id3d3ZmIdmVDMiEREAABESIjRFVmd3d3d3ZlVDMiIREQAAARERIjNERVVWZVVVRDMyIRERAAAAARER"
"IiMzNEREREQzMiIhEREAAAAAAREREiIiMzMzMzIiIhEREQAAAAAAARERERIiIiIiIiIRERERAAAAAAABERESIiIiIiIiIiIiIiIREREAABERIiIzMzMzMzMz"
"MzMzIiIRERABERIjM0RERFVVVVVERERDMiIREBERIjNFVWZmZmZmZmZmVVRDMiERERIjRWZ3iIiIiIiIiIh3ZlRDIREREjRd3d3d3d3d3d3d3d3d3UMiEREi"
"NG//////////////////VDIRESJFb/////////////////9UMhERI0Xf/////////////////2QyEREjRe/////////////////+VDIRESI1////////////"
"//////1UIhEREjTd3d3d3f////3d3d3d1UMiERESNFZniaqq////+qqZh3ZUMiERERIjRFZ4mq3////qqYdmVEMyEREBESIzRWeJrv///9qXZlRDMiEREAER"
"EiM0Vnmv////2oZUQzIiEREQABERIjRGea////+pdkQzIiEREQAAEREiM0V5r////6hlQzIiEREQAAABESIjRXnf///+qGQzIhEREAAAAAEREiNGee////6X"
"VDIiEREAAAAAARESI0Z5/////YdUMiEREAAAAAABERIjRor////6hlMyEREAAAAAAAAREiNWiv////l2QyIREQAAAAAAABESM1aN////6XVDIhEQAAAAAAAB"
"ERI0V57////pdUMhERAAAAAAAAEREjRXn////9hlMiERAAAAAAAAARESNFef////qGQyIREAAAAAAAABESI0aK////+XVDIhEQAAAAAAAAERIjRo3////pdU"
"MhERAAAAAAAAAREiNWjv///+hlMiERAAAAAAAAABESI1aP////2GQyIREAAAAAAAAAERIjV4////+XVDIhEQAAAAAAAAAREiNXj////4dUMhERAAAAAAAAAB"
"ESI1bf///+hkMiERAAAAAAAAAAERIjVu////51QyEREAAAAAAAAAARESNF7////WQyIREQAAAAAAAAAAERI0X////2VDIREQAAAAAAAAAAAREiNFZndmVDIh"
"ERAAAAAAAAAAABERIjRFVVRDIhERAAAAAAAAAAAAARESIzMzMzIhERAAAAAAAAAAAAAAEREiIiIiIhEREAAAAAAAAAAAAAABERERIhEREREAAAAAAAAAAAAA"
"ERERERERERERAREREREREREREAAAERERIiIiIRERERERERIiIiIRERAAERESIjMzMiIRERERESIjMzMiIREQEREiIzRERDMiERERESIzRERDMiERARESM0VW"
"ZVQzIhEREiI0RWZlRDIhERESI0Xd3d3UMyIREiI0Xd3d3VQyERESI0Vv////VDIiEiI0Vv////VDIRERIjVt////5lMyIiIzRn////9lMiEREjRX7////mVD"
"IiIjRXj////nVCIREiNWjv///9dUMiIjNGff///+dUMhESNFef////l1QyIiNFaO////11QyESI0V5////+XVDIiM0V5////+XVDIRIjRo3////odUMiIzRn"
"n////5dUMhEiNWjv///+hlQzIzNGiv///+h1MiESNFee////2GVDMjNFaN////6GUyISI0V5////+oZUMzM0V57////YZDIhIjRor////5dkMzMzRXn////6"
"hkMiEiNWjf///+l2QzMzRWiv////l1QyISM1eO////6XVDMzNFaK////6XVDIRI0V57////YdUMzM0V43////oZTMhEjRXn////6hlQzM0RXnv///9hkMiES"
"NGiv////qGVEMzRWif////qGQyIRI0aN////6oZUQzRFaK////+XVDIREjRo3////qhlRERFZ5z////pdUMhESNWjv///9qGVURFVonf///9hlMiERI1aO//"
"//2odlVFVniv////l2QyIREjVo/////al2ZVVnia////+XVDIRESNGjv///+qodmZnia7////oZTIhEQI0aO////+qmId4ia3////5dUMiERAjRX3////+qq"
"mZmqr////+hlQyERECJFef/////qqqqq3/////l1QyIREAIjRo7//////+7u//////2GUzIREQASNFef//////////////6HVDIhEQABIjRn3///////////"
"//6HVDIhERAAESNFaN////////////6HVDIhERAAARIjRWef/////////+yGVDMiEREAABESI0Vnje///////ph2VDMiEREAAAEREiNFVniJ3d3ZmIdlVDMi"
"EREAAAARERIjNFVmd3d3d2ZVQzIiEREAAAAAERESIjNEVVVmVVVEMyIhEREAAAAAABEREiIjM0REREQzMiIhEREAAAAAAAAREREiIiMzMzMyIiEREREAAAAA"
"AAAAARERESIiIiIiIhEREREAAAAAAAAAEREREREREREAAAAAEREREREREREQABEREiIiIiERERAAARERESIiIiIRERARESIjMzMiIRERERERERIiMzMyIhER"
"AREiM0REQzMiERERERESIzREREMiERERIjRFZmVUMyIRERERIiNEVWZVQyIRERI0Xd3d3VQzIhERESIjRW3d3d3DIRERI0X////2VDIiEREiI0Vn////9DIh"
"ESI1b////9ZUMiIiIiNFaP////1TIhESNFfv///+dkMyIiIjRWjf////ZTIhESNFff////hlQyIiI0Vnn////4ZTIhESNFff////l1QzMjM0V47////nZDIR"
"ESNFef////qGVDMzNFeN////+HVDIRESM1ef////2HVEM0RWif////12QyIRESI1aO////6XZUREVnnv///+h1QyERESI0aN////+oZVRFZ43////5dkMiER"
"ESI0aK////+pdlVWeJ/////YZUMiERARI0V5/////ZhmZnie////6XZDIhERARI0V57////qh2Z4mv////mGVDIhEQARIjVo3////6mHeJr////+h1QyIREQ"
"ARIjRor////6qYia3////5dkMyIREAARIjRnn////8qpmq/////YZUMiEREAAREjRXnv///9qqqu////6XVDIhERAAAREjRXnf///+qqrf////mGVDIhEQAA"
"AREiNWjP////qqr////9h1QyIREQAAAREiNGiv////qq7////pdkMiEREAAAARESNFef////2s////+oZUMiEREAAAABESNFee////6v////2XVDIhERAAAA"
"ABESI1aN/////v////mGQzIREQAAAAABESI0aK/////////6hlQyIREQAAAAABERI0V5/////////pdkMiEREAAAAAAAERI0V57///////+oZUMiERAAAAAA"
"AAERIjVo3///////2HVDIhERAAAAAAAAERIjRnr//////+l2QyIREQAAAAAAAAEREjRXn//////6hlQyIREAAAAAAAAAAREiNGjv/////YZUMiEREAAAAAAA"
"AAARESNFff////6HVDIhERAAAAAAAAAAABESI0V/////hlQzIREQAAAAAAAAAAABERIzRWd4h3ZUMyIREQAAAAAAAAAAAAERIjNFVmZVRDIiEREAAAAAAAAA"
"AAAAEREiIzREREMyIRERAAAAAAAAAAAAAAARERIiMzMyIiEREQAAAAAAAAAAAAAAABERESIiIiIREREAAAAAAAAAAAAAERERERERERERERERERERERERERER"
"ERERERERERAAERESIiIiERERERERIiIiIiIRERERERESIiIiEREQEREiIzMzIiERERERIiMzMzMiIRERERIiIzMyIiEREREiM0REQzIiERESIjNEVURDMiER"
"ERIiM0REQzIhEREiNFVmZUQyIiIiIzRVZmZlQzIiIiIjNFVmVUMyERESNF3d3dxUMiIiIzRW3d3d1UMyIiIjRFbd3d3TIhESI13////mVDMiIzRWj////3VD"
"MiIjRFfv///8QyERI0V/////dkMzMzRWjv////hlQzMzRGff///+VDIREjRn////+GVDMzRWif/////XZUMzRFeP////dUMhESNGj////5dlRERWee/////9"
"hlRDRFaN////51QyERI0aP////mGVERWec//////6XZURFZ5////+GVCIRIjRo7////YdVVVeK7//////phlVFZ43////oZDIhEiNGjv///9l2VVaJ3/////"
"//qHZVZ4n////5dUMhESI0aN////6YZmZ5r///////+pdmZonf///+hlMiERIjRo3////qh2Z4rf///////6mHZniv////l2QyIREiNGiv////qYd4mv////"
"////qod3iu////2GVDIRESI0Z5////+piImt////3////amIia////+XZDIhEQEjRXn////6qZmq////6v///9qZma7////YZUMiERASNFee////qqmq7///"
"+q////6qmqr////5dkMiEREBI0V57////aqqr////qrv///qqqrv///9hlQyIREAEjRXnf///9qqrv///6qu////qqqv////l2QyIREQASI1aN////6qqv//"
"/+qq3///+qqu////2GVDIhEQABIjRoz////qqu////qqrf///6qs////+XZDIhERAAEiNGiv////qq////2qqq////qq7////YZUMiERAAASI0aK////+q7/"
"//+qqqr////K3////pdUMiEREAABEjRXr////6z////amZqv///9r////6hlQyIREAAAESNFee////r////6mImq////3f///+l1QyIREQAAARI0V57/////"
"///9qIeJrv///v////qGVDIhEQAAABEiNXnf////////qXd3iu////////6XVDIhERAAAAESI1aN////////yYdmeK3///////+oZUMhERAAAAARIjRor///"
"////6odlVnnf///////pdUMiERAAAAABESNGev//////+pdlVWeK///////6hlMyEREAAAAAERI0V5///////phlREVor//////+l1QyIREAAAAAAREiNWj/"
"/////6hlRERWef//////l2QyIREQAAAAABERI0Z+/////+h1RDM0Vo//////2GVDIhEQAAAAAAEREjRW7/////h2RDMzNFfv////+HVDIhERAAAAAAABESI0"
"Xf////12RDIiI0Vu/////WVDIhERAAAAAAAAEREiNFZ4iHdlQzIiIiNEVniId2VDMhERAAAAAAAAABERIzRVZmZVQzIiIiIjNFVmZlRDIhEREAAAAAAAAAAR"
"ESIzREREMyIhERESIzNEREQzIiEREAAAAAAAAAABEREiIzMzMiIREREREiIjMzMiIhEREAAAAAAAAAAAAREREiIiIiERERERERESIiIiIREREAAAAAAAAAAA"
"AAAAAREREREREREREREREREREREREQAAAAAAABEREiIiIiEREREREiIiIiIhERAAAAAAABERIiMzMzIiIhEiIiIzMzMiIREQAAAAAAERIjNEREQzIiIiIjM0"
"RERDMiERAAAAAAERIjRVZmZUQzMiIzNFVWZlVDIhEQAAAAAREjTd3d3XZUQzMzRVZ93d3d0yERAAAAABESNO////92VURERWeP////9DIREAAAAAESI0b///"
"/9hlVEVWeO/////EMhEQAAAAARIjRu////+YdlVnie/////VQyERAAAAABESNGj////9mHd3ie/////WVDIREAAAAAERI0Z+////+piImu/////XZDIhEQAA"
"AAAREjRXn////9qqqt/////oZUMhERAAAAABESI1aN////+qqt/////odUMiERAAAAAAERIjRnn////+qq/////pdkMiEREAAAAAARESNFeN////+q/////p"
"dlQyIREAAAAAABERIzVor//////////5hlQyIREQAAAAAAEREiNGed/////////6h1QyIREQAAAAAAABESI0V4rv///////6l2QzIhERAAAAAAABERIjRWiq"
"///////6mGVDIhERAAAAAAAAEREiNFaKrv/////6qXVDMiEREAAAAAAAARESI0Voqt/////9qoZUMiEREAAAAAAAABESIzRXmq7/////qpdkQyIREQAAAAAA"
"ABERIjRWia7//////al2QzIhEQAAAAAAABERIjNFea7///////qYZDMiERAAAAAAAAEREiNFaK7////////qhlQyIREAAAAAAAEREiNFaJ7/////////qXVD"
"IhEQAAAAAAEREiM0Z53////+/////pdkMiERAAAAAAARESM0V43////+qv////qGVDIREQAAAAARESI0V43/////qq7////pdUMiERAAAAABESI0Von/////"
"qqqv////yGQyIREAAAABERIzVnn/////qqqq7////4dUMhEQAAAAERIjRXj/////2piJqv////12QyERAAAAAREjNWj/////2Yd3eJ7////4ZDIREAAAABES"
"NFf/////2YdmZniv////1lMiERAAAAERI0Xv////6HZVVVZ47////2UyIRAAAAAREjTv////6HZURERWeP////5DIhEAAAABESPv////52VUMzM0VW7////0"
"MhEQAAAAERIzRWZ3dmVEMzIjNEVmd2ZUMiERAAAAAREiM0VVVVRDMiIiIjNEVVVUMyIREAAAAAERIiMzRDMzIiIiIiIjMzQzMiIREAAAAAARESIiIiIiIhER"
"ERESIiIiIiEREQAAAAAAERERIiIiERERERERERIiIREREQAAAAAAEREREREREREREREREREREREREQAAERERIiIiIRERERERERIiIiIhEREAARESIjMzMiIR"
"ERERESIiMzMiIhEQARESIzRERDMiIREREiIzNEREMyIREBESI0VWZVRDIiIiIiI0RVZVVDMhEQERI03d3d1lQzIiIiM0Vm3d3d0yIRERIjT////9ZUMyIjM0"
"Vn7////TIhEREjRd////52VDMzNEVo7////lMiERESNFf////5dlRDNFV43////2UyIRERI0V+////2HZURFZ4n////3ZDIhEREiRXn////5hlVVZ4n////9"
"dUMhEQESI1aP////2XZmZ4nv///+dlMiERARIjRo3////ql3d4nv///+h1QyEREBESNFef////qpiJrf////l2QyIREQERIzVo7////aqarP////l2UzIREQ"
"AREiNGiv////qqqv////2GVDIhERABESI0V5/////aqv////6HVDIhERAAEREjRWjf///+qu////6XZDMiEREAABESI0aK////+t////+YZUMiEREAAAERIj"
"RXnv////////+oZUMiEREAAAARESNFaK/////////ZdUMiEREQAAAAERIjRor////////pdkMyIREQAAAAAREiNFed///////qhlQyIREQAAAAABERI0V4r/"
"/////6h1QyIREQAAAAAAEREjNWiu/////6l2QzIhEQAAAAAAABESI0aK3////+qGVDIhERAAAAAAAAERIjRXnP////qXVDIhERAAAAAAAAAREiNFed////6o"
"ZUMiERAAAAAAAAABESI0V57////ZdUMiEREAAAAAAAAAEREjRXn////8hlQyIREAAAAAAAAAARESNFef////qGQyIREQAAAAAAAAABERI0V5////+XVDIhEQ"
"AAAAAAAAAAAREjRX3////oZTMhERAAAAAAAAAAABESNFfv///9dkMiERAAAAAAAAAAAAERIkV/////xlQyEREAAAAAAAAAAAAREiNG////91QyIREAAAAAAA"
"AAAAABERI0X////2QzIREQAAAAAAAAAAAAAREiNFZnZlRDIhEQAAAAAAAAAAAAABERIzRFVUQzIhERAAAAAAAAAAAAAAARESIzMzMyIhERAAAAAAAAAAAAAA"
"AAEREiIiIiIRERAAAAAAAAAAAAAAAAARERERIhERERAAAAAAAAAAAAAAAAAAERERIiIiIiIiIiIiIiIhEREAAAAAARESIiIzMzMzMzMzMzMyIiEREAAAABER"
"IjM0RERERVVVVUREQzMiEREAAAEREiM0VVZmZmZmZmZmZlVEMiERAAABESI0VWd3iIiIiIiIiId2ZUMiEQAAAREjRd3d3d3d3d3d3d3d3d1UMhEAAAERI0b/"
"////////////////VDIRAAAREiRd/////////////////2QyEQAAERI0Xf////////////////9lMhEAABESNF7////////////////+ZDIRAAAREiNf////"
"////////////92QyEQAAARIjTd3d3d3d3d3f/////5dUMhEAAAERIzRWeImaqqqq7/////l2QyIRAAABESI0RWZ3iJqqrv////+oZUMhEQAAARESIzRVZnia"
"qu/////6h1QyIREAAAEREiIzRFZ4mq7/////qHVDIhERAAAAEREiM0RWeJrv////+odkMyIREAAAABERIjNEVnmu/////6h2QzIhERAAAAERESIzRWea7///"
"//qHZEMiEREAAAABERIiNFZ4rv////+odlQyIREQAAAAEREiI0Rniu/////6h2VDIiEREAAAARERIjRFeK7/////qHZUMiIREQAAAAEREiM0V4rv////+pdl"
"QzIhERAAAAARESIzRXiu/////6l2VDMiIREQAAABERIjNFeK7////+qYZUQzIhEREAAAARESI0V4rv////6ph2VEMyIRERAAABERIjRWiu/////qqYZlRDMi"
"IREQAAAREiNFaK7////+qqh3ZVRDMiEREAAAERI0Vonv////6qqph3ZlVEMiERAAABEiNGee/////qqqqpiHdmVUMiERAAARI0V57////+qqqqqqmZh3ZUMh"
"EQAAESNGjv////////////////5DIREAABIjVu/////////////////+UyIRAAASI1f//////////////////VMiEQAAEiRX//////////////////ZTIhEA"
"ABIjXf/////////////////2QyERAAARI07/////////////////9UMhEQAAESNFZ3iJmZmZmZmZmYiHdlQyIREAABEiNEVmZ3d3d3d3d3dmZlVDMhERAAAR"
"EiM0RFVVVVVVVVVVVURDMiEREAAAEREiIzM0REREREREQzMzMiIREQAAAAERESIiIjMzMzMzMzMiIiIREREAAAAAAAAAERERERERERAAAAAAERERIiIREREQ"
"AAAAERESIiIiIhEREAAAEREiIzNDMyIREQAAAREiM0VVVEMiERAAARESM0Vt3WVDIhEAABESI0Xf//9lQyEQAAERI0Xf////ZDIhAAEREjRv/////VMiEAAR"
"EiNW/////9VCIQABESI1b/////ZTIhAAERIjVu////5lMiEAEREiNGfv//51QyEQARESI0V4jdh2VDIRABERIzRWeIh2VDIhEAERIjRWd4h3ZUMiEQEREiNF"
"Z4iIdkMyERARESM1Z////WQyIREBESI0V9///3ZDIhERERIjRo7///hkMiEREREjRXj///+GUyIREREiNFef///4ZTIhERESI0aN///+hkMiERERI0V47///"
"2GQyIRERIjRXn////YZDIhEREiNGjf///8dkMiERESM1aO////l1QyIRERI0V5////+XVDIRERIjRXn////5dUMhEREiNGjf///+h1MiERASI1aO////6GUy"
"IREBIjVo/////YZDIhEQEiNXj////9dUMhERASI1bf////h1QyEREBIjVu////+GQyIREAESNG/////2VDIREQARI0X/////VDIhEQABEiNFZnd2VDMhERAA"
"ERIjRFVVRDMiERAAARESIzNEMzIiEREAAAEREiIiIiIhEREAAAARERESIiEREREAAAAAEREREREREREAARERIiIiIhEREBEREiIzMzIiERERESI0RERDMiER"
"ERIzRVZlVDMhEREiNFbv/sVDIhERI0Vv///9VDIRESNG/////2QyERIjVv////9lMhESJF3/////ZTIREiNd/////2QyERIjRv////1UMhERI0Vv///mUyIR"
"ESI0Vt7WZUMhERESI0VVVUQyIRERESIzREQzIhERERESIjMyIiEREAERERIiIiERERAAERERERERERAAAAAAAAAAARERERERERAAAAAAAAAAAAAREREiIhER"
"EQAAAAAAAAAAARESIiIiIiEREAAAAAAAAAARESIzM0MzIhERAAAAAAAAABESIzRVVUQyIREQAAAAAAABERI0VW3cZUMiERAAAAAAAAERIjRt///2VDIREAAA"
"AAAAEREjRd////9UMhEQAAAAAAAREiNG/////2UyEREAAAAAARESI1b/////ZTIhEQAAAAABERI0Vv////9lMiERAAAAABERIjRW7////mQyEREAAAAAEREi"
"NFZ////mVDIREQAAAAEREiI0VnjN2HZDMhEQAAAAERESIzRWeIiHZUMiERAAAAERESIzRWeIiHdkMyEREAAAARESIzRWeImYdlQyIREQAAARESIzRWeP///+"
"VDIhERAAARESIzRWeO////1UMiERAAARESIzRWed////5lQyEREAABESIzRWie////92UzIREQAAERIzRXif////+HVDIhEREAARIjRXjf////6XZUMiIREQ"
"ABIjRWjv////6YdUQyIhEREAEiNWjv////2odlRDMiIREQASNFff////qphmVEQzIiERECI0aP////yqh2ZVVEQyIREQIjV9////6qmHdmZlVUMyEREjRX7/"
"///qqph3d3fWVDIhESNFfv///+qqqZmZn/5lQyERI0V+/////aqqqq7///ZDIhEiRX7/////7d3e/////9MiESI1bf//////////////UyIREjRn7///////"
"/////+ZDIRESNFaP///////////+ZUMhERIjRWjv/////////odUMiERESI0Vnjv/////+2HZUMyEREREiNFZ3iZzdmYh2ZUMyEREBERIjRFZnd3d3ZlVEMi"
"EREQERESIzREVVVVVUQzMiEREQABEREiIzM0RERDMyIiEREQAAAREREiIiMzMyIiIhEREQAAAAERERESIiIiIiEREREQAAAAAAAAAAAAAAAAAAAAERERERER"
"ERAAAAAAAAAAAAAAAAAAABERERIiIiIREQAAAAAAAAAAAAAAAAABERESIjMzIiEREAAAAAAAAAAAAAAAABEREiMzREQzIhERAAAAAAAAAAAAAAABEREiM0VV"
"VUQyIREAAAAAAAAAAAAAAAEREiNFVu7u7uMhEQAAAAAAAAAAAAAAEREiNFZ/////QyERAAAAAAAAAAAAAAEREiNFZ////+VDIREAAAAAAAAAAAAAEREiNFZ/"
"///9ZDIhEQAAAAAAAAAAAAARESI0XP///nZUMiERAAAAAAAAAAAAARESI0Xf///YdUMyEREAAAAAAAAAAAARESIzRe7u6YdlQyIREQAAAAAAAAAAABESIjRW"
"eJmZh2RDIhERAAAAAAAAAAABERIjRWeJqqmHZDMiERAAAAAAAAAAABERIjRWed3d3ddlQyIREAAAAAAAAAAAERIjRGef////+GVDIhEQAAAAAAAAAAEREiNF"
"ee/////5dUMiERAAAAAAAAAAEREiNFaN//////2GQyIREQAAAAAAAAAREiM1aJ///////pdUMhERAAAAAAAAARESI0Z57///////l1QyIREAAAAAAAARESI0"
"Voz///////+oZDIhEQAAAAAAABERIzVon////////9hlMiERAAAAAAABERIjRXnv///6////6XVDIREQAAAAABERIjRWiv///+rv///5dUMiERAAAAAAERIj"
"NGif////qt////qGQyIREAAAAAEREiNFed////2qr////YZTIhEQAAAAEREiNFaK////6qqv///+l1QyEREAAAAREiM0Z57///+qqq7///+XVDIhEQAAARES"
"I0V53////aqqrv///6hkMiERAAARESI0Vor////6qqqt////2GUyIREAABESIzRnnv///9qqqqr////pdUMhEQABERIjRXnf///+qqqqqv////l1QyIRAAER"
"IjRWiv////qqqqqq7///+oZDIhEAEREiNGee////6qqqqqrv///9hlMiEQAREiNFeN////////////////6XVDIRARESM1aJ/////////////////5dUMhEB"
"ESI0V57/////////////////qGQyIRERIjVor//////////////////YZDIhEREjRXn//////////////////+hlMiEREiNWjv////qqqqqqqqqt////+XUy"
"IRESNFef////qqmZiIiZmqr////5dUIhERI0Z/////6ph3d2ZneImu////11QiERIjVt////+odmVVVVVWeJ3////XVCIREiNW/////YdlREREREVWjP///+"
"ZTIhESI17////odVQzMzMzNEVo////9kMiERIj3////3ZUQzIiIiIzRFb////8QyERESM0Vmd2VUMyIiIiIiIzRWZ3ZlQyIRERIjNFVVVEMyIhERERIiM0RV"
"VUQzIRERESIzM0MzMiIRERERERIiMzMzMyIREQEREiIiIiIiERERERERERIiIiIiIREQABERERIiIREREREAARERERESIhERERAAAAAAAAAAAAABERERERER"
"EREAAAAAAAAAAAAAAAERERIiIiIhERAAAAAAAAAAAAAAERERIiIzMyIiERAAAAAAAAAAAAAREREiM0RERDIiERAAAAAAAAAAABEREiI0RVVVVDIhEQAAAAAA"
"AAABERESIzRWbu7u7jIhEQAAAAAAAAERERIjNFZ/////QyIREAAAAAAAERERIiM0Vn////5UMiEREAAAAAAREREiIzRWf////WVDIhERAAAAABEREiIzNFbP"
"///odlQzIhEQAAAAEREiIzNEVt///9mHZUMyIREQAAARESIzREVmfu7uqph2ZUMyIREAABERIjRFZmeImqqqqYh2ZUMyERAAEREiNFZ3iJmqqqqqqZiHZUMi"
"EQABESI0Vt3d3d3d3d3d3d3d1TMhEAEREiNW3///////////////VDIRABERI0V+///////////////2QyEQAREiNWj///////////////5kMhEBERI0V5//"
"/////////////9VDIRAREiNGjf///////////////VQyEQERIjVo3////szMzMzMzMzMVDIhEBESNFee////2qqqqZmYh3ZUMyEREREjRXn////6qqmZiId3"
"ZVQzIhERESI0aK////+qqpiId3ZlVDMiEREREiNWjf///+qqqYiHd2VUQyIREQERI0V53////qqqqZmId2VDMiEREBESNFee////2qqqqqmYdlQzIhERARIj"
"Ror/////7u7u7u7u7lQyEREAESI0aK//////////////5UMhERABEiNWjf/////////////9VDIREQARI0V53/////////////91QyERAAESNFee////////"
"//////ZTIhEQABIjRor/////7u7u7u7u7lQyIREAASI0aK////+qqqqqqZh3ZUMhERAAEiNWjf///+qqqpmIh3ZlQyIREQABI0V53////qqpmId3ZlVEMiER"
"EAASNFee////2qqYh3dmZVQzIhERAAEjRXn////6qqmIh3dmVUQyIREQABI0Z6////+qqqmZiId2ZUMyEREAASNGjf///+qqqqqqmZiHZUMiERAAEjRo3///"
"/+7u7u7u7u7u5DIhEQABI0aO///////////////9UyIREAASNGj////////////////VMiERAAEjRn////////////////ZTIhEQABI0Xf//////////////"
"/2QyERAAASI03///////////////5UMhEQAAESNFZ3iJmZmZmZmIiHdlQyIREAABEiNEVmZ3d3d3d3dmZlVDIhERAAAREiM0RFVVVVVVVVVERDMiEREAAAER"
"EiIzMzREREREQzMzMiIREQAAAAERESIiIjMzMzMzIiIiIREREAAAAAAAAAAAABEREREREREQAAAAAAAAARERESIiIiEREAAAAAAAAREREiIzMyIhERAAAAAA"
"ARERIiM0REMyIREQAAAAARERIjNFVVVEMiERAAAAABERIjRVbu7u7jIREAAAABERIjRWf////0MhEQAAAAERIjRWf////lQyERAAAAEREjNFf////VQyIREA"
"AAAREiNFz///52VDIhEQAAARESM03///12VDIhEQAAABESI0Xu7uiHZEMiERAAABERIjRXiJiHZUMiEREAAAERIjRWeJmYdlQyIREQAAEREiNGfd3d3WVDIh"
"EQAAARESNFbf///+ZUMiERAAABESI0Z+////1lQyEREAAAERIjVo////+GVDIREQAAEREjRXn////4ZTMhEQAAAREiNGjf////hlMiERAAABESI1aN////6G"
"UyIREAAAERI0V57////YZDIhEQAAEREjRXn////6hkMiERAAAREiNGev////l1QyEREAABESI0aN////+XVDIREQAAERIzVo3////pdUMhEQAAAREjRXnv//"
"/9hlMiERAAABEiNFef////qGQyIREAAAESI0aK////+XZDIhEQAAARIjRo3////5dUMiERAAABEjRWjf///+l1QyERAAAAESNFee////2GUyIREAAAASI0V5"
"////+oZDIhEQAAABIjRor////5dkMiERAAAAEiNGjf////l1QyEREAAAASI1aN////6XVDIREAAAABI0V57////YZTIhEQAAAAEjRXn////6hkMiERAAAAAS"
"NFef////l1QyIREAAAABI0V9////+XVDIREQAAAAEjRo3////oZTIhEQAAAAASNGfv///9dkMiERAAAAABI0Z/////l1QyEREAAAAAEjRX////+GQyIREAAA"
"AAASI13////2VDIREQAAAAABIjTf///+VDIhERAAAAAAESM0VmdmVDMhERAAAAAAARIjNEVVRDMiEREAAAAAABESIjMzMzIhEREAAAAAAAERESIiIiIhEREA"
"AAAAAAABERERIiEREREAAAAAAAAAAAAAAAAAAAABEREREREREREREAAAAAAAAAAAAAAAARERERIiIREREREREAAAAAAAAAAAAAAREREiIiIiIiIiERERAAAA"
"AAAAAAAAARERIiMzMzMzMiIiIREQAAAAAAAAAAARERIjNEVVRERDMzMiERAAAAAAAAAAARERIjRVZmZlVVVUQzIhEQAAAAAAAAARERIjRW3//nd2Zt7tMyER"
"EAAAAAAAARERIjRW/////YiI//5DIhERAAAAAAAREREjNG7/////6Z7//EMiEREAAAAAAREREiNF3//////////2UzIRERAAAAARERIiM0Xv/9me/////8ZU"
"MiEREAAAARERIiM0Vv/+iIjf///9dlQzIhERAAABERIjNEVW3dd3d3je7Yd2VUMyIREAABERIjRFZmd3dmZmZ3eIh3dlVDMhEQABERIjRWZ3h3dmVVVWZnd3"
"h3dlQyIRAAERIjRW3d3dh2ZVRFVWZ3zd3d1UMhEAEREjRW3///+YdlVERFVnj////2QyIQAREiNGfv///+mHZURERWeP////dUMhABESI1aP/////ZdlVERV"
"aJ////91QyEBERI0V5//////qHZURFZ43////nVDIQERIjRo3//////ah2VVVnnv///9dUMhAREiNWjf//////qXZVVWif////l1QyEBESNFee///////ql2"
"ZWeK////+XVDIRERI0Z6////////2odmd5r////5dUMhERIjRor////////6mHd4rf///+hlMiEREiNWjf////////2piImu////2GUyIRESNFed////////"
"/6qZmq////+oZDIhERI0V57////v////6qqqr////6dkMiERIjRnr////67////8qqqv////l1QyEREiNGiv////qv////+qqt////6XVDIRESI1aN////6q"
"3////9qq7////YZTIhERI0V53////aqv////+qr////6hkMiEREjRXnv///9qq3////+qv////qGQyIREiNFef////qqqu////+q////+XVDIRESI0aK////"
"+qqqr/////3////pdUMhERIjRo3////qqZqu/////////9l1QyEREiNWjf///9qYiar/////////qGUyIRESNFee////2od4mt////////+oZDIhERI0V5//"
"//+pd2d5rv///////5dUMhEREjRXn////6h2Zmia///////+l1QyERASNFff///+mGVVZ4rv//////2GUyIREBI0Z9////2HZURWeK//////+oZDIhEQEjRn"
"7////YZUREVnnf/////5dUMhERASNFf////5dUQzNFZ57/////hlMiERABI0V/////hlQzMzRWif////52QyIREAEiNd////51QzIiM0Vo7////WVDIREQAS"
"I03////VQzIiIjNFZ////2VDIhEQABEjNFZmZlQzIiIiIzRWZ3d2VDIhERAAESIzRFVUQzIhEREiI0RVVVRDIhERAAAREiIzMzMyIhERERIiMzREMzIhERAA"
"ABEREiIiIiIRERERERIiIiIiIhEREAAAARERERIREREREREREREiIiEREREAAAAAAAAAAAAAAAAAABERERERERERAAAAAAAAAAAAAAAAAAABERESIiIiIREQ"
"AAAAAAAAAAAAAAAAABEREiIjMzIiEREAAAAAAAAAAAAAAAAREREiM0REQzIhERAAAAAAAAAAAAAAARERIiNEVVVUQyIREAAAAAAAAAAAAAERERIjNFZu7u7u"
"MhEQAAAAAAAAAAAAERERIjNFZ/////QyEREAAAAAAAAAABERESIjNFZ////+VDIhEQAAAAAAAAARERESIjNFZ////9ZUMiERAAAAAAAAAREREiIjNFbP///o"
"dlQyIREAAAAAAAERERIiIzRFbf///ZhlQzIhERAAAAAAERERIiM0RVZ+7u6ph2VEMiEREAAAAAERESIjNEVmeJmqqqqYdlQzIhERAAAAERESIzRFZ3iZqqqq"
"qpl2VUMiIREAAAERESIzRWeIne7///7tqph1VDIhERAAERESI0Vnid7/////////2YdlQyIREQARESI0Vnjf////////////2HZDMhERARESI0Vonv//////"
"///////+h1QyIRERESI0Von////////////////odUMiERESI0Vo3/////////////////2GUyIRERIzVnn//////+2qqt7//////5dUMhERIjRXn//////q"
"qZmZqq3/////6GUyIREjRWjv////+qmId3eJqt/////5dUMhEiNGff////+ph3ZmZmeJrf////2GQyESNFeO////+ph2VVRVVniq/////oZDIhI0Z5/////a"
"h2VERERFZ5rv////hlMiIjRo7////6l2VEMzM0RXit////+XUyIjRXn////9qGVEMzMzNFaKr////5dTIiNFef////qXZUMzIjM0Vomv////l1MiI0Z9////"
"+pdUQzIiMzRWic////+XUyIjRo3////qh1QzMiIzNFaK3////5ZTIiNGjv///+qHVDMyIjM0Vorv///+hkMiI0aO////6odUQzMzM0Rnmv////2GQyIjRo7/"
"///ql2VDMzM0RWic////+XVDISNGjf////qXZUQzM0RWea7////4dTMhI0V9////+qh2VERERWeK3////9hkMiEjRXn////+qYdlVVVmeJr/////l1QyESI1"
"aP/////amHdmZ3iar/////2GQyIRIjRo3/////2qmIiIma3/////6HVDIhESNFef/////+qqqqqt7/////+XZDIhERIjRn3///////7u7v//////+XZUMhER"
"ESNFaO////////////////+YZUMiERERIjRXjv//////////////+YZUMiEREBESI0V43/////////////6HZUMiEREAEREjNFeJ7//////////+mHZUMiER"
"EQARESIzRWeJ7///////7Zh2VEMiEREQAAEREiM0VWeImd3d3JiHdlVDMiEREQAAABERIiM0VWZ3d4h3d2ZVQzIiEREQAAAAARERIiM0RVVWZlVVREMyIhER"
"EQAAAAAAERERIiMzREREREMzMiIhEREQAAAAAAABERERIiIzMzMzMiIiIREREAAAAAAAAAARERERIiIiIiIiIREREREAAAAAAAAAAAAAAAAAAAARERERERER"
"EQAAAAAAAAAAAAAAAAEREREiIiIhERAAAAAAAAAAAAAAAAERERIiMzMiIhEQAAAAAAAAAAAAAAERESIjNERDMiIREAAAAAAAAAAAAAERESIzRVVVRDIhERAA"
"AAAAAAAAABERESI0RW7u7u4yIREAAAAAAAAAERERESI0Vm////9DIhERAAAAAAABERERESIzRW////5UMiEREAAAAAARERERESIjRW////1lQyIRERAAAAER"
"ERERIiIjNM///+dlVDMiEREQAAERESIiIiIjNN///9dmVEMzIhEREAERESIjMzMzM07u7nZmVVREMyIREQEREiIzREREQzRFVWZlVVVVVEMiERARESM0VWZl"
"VEREREVVVWZnZmVDIhERESI0Xd3d3VRDMzRERVbd3d3VQyERESI0Vv////ZUMzMzRFZ/////ZDIhERIjVt////5lQzMzNEV4////9lMiEREjRX7////nVDMy"
"M0Rnn////nVDIREiNWjv///9dUMyIzRWjf///+dUMhESNFef////l1QzIjNFeO////11QyESI0V5////+XVDMiM0V5////+XVDIRIjRo3////pdUMyMzRnn/"
"///5dUMhEiNWjv///+hlQzIzRWiv///+h1MiESNFee////2GVDMzNFaN////6GUyISI0V5////+oZUMzM0V57////YZDIhIjRor////5dkMzMzRXn////6hk"
"MiEiNWjf///+l2QzMzRWiv////l1QyISM1eO////6XVDMzNFaK////6XVDIRI0V57////YdUMzM0V43////oZTMhEjRXn////6hlQzM0RXnv///9hkMiESNG"
"iv////qGVEMzRWif////qGQyIRI0aN////6oZUQzRFaK////+XVDIREjRo3////qhlRERFZ5z////pdUMhESNWjv///9qGVURFVonf///9hlMiERI1aO////"
"2odlVFVniv////l2QyIREjVo/////al2ZVVnia////+XVDIRESNGjv///+qodmZnia7////oZTIhEQI0aO////+qmId4ia3////5dUMiERAjRX3////+qqmZ"
"mqr////+hlQyERECJFef/////qqqqq3/////l1QyIREAIjRo7//////+7u//////2GUzIREQASNFef//////////////6HVDIhEQABIjRn3/////////////"
"6HVDIhERAAESNFaN////////////6HVDIhERAAARIjRWef/////////+yGVDMiEREAABESI0Vnje///////ph2VDMiEREAAAEREiNFVniJ3d3ZmIdlVDMiER"
"EAAAARERIjNFVmd3d3d2ZVQzIiEREAAAAAERESIjNEVVVmVVVEMyIhEREAAAAAABEREiIjM0REREQzMiIhEREAAAAAAAAREREiIiMzMzMyIiEREREAAAAAAA"
"AAARERESIiIiIiIhEREREAAAAAAAAAAAAAAAABEREREREREREREAAAAAAAAAAAAAABERESIiIiIiEREREAAAAAAAAAAAABERIiIiIiIiIiIRERAAAAAAAAAA"
"ARERIjMzMzMzMzMyIREQAAAAAAAAABERIjRFVERERFRDMiEREAAAAAAAARERIjRd7mVVVc7lRDIREQAAAAAAAREREjRe//9mZt///UMiEREAAAAAAREREiNF"
"///+dn///+VDIRERAAAAAREREiM13///53f///5UMiEREAAAAREREiI0Vv///Xd////VQyIhERAAARERIiMzRW7//mZm7//mVUMiIREQARESIjNERVVm1mZV"
"Zn13ZlRDIiERARESIzRFVVVWZVVVVVZnd2ZVQzIREBERIzRWZmZmVVREREVWd3d3ZUMiERERIjRd3d3dZURDMzRFZ93d3dVDIRERIjRX////9lRDMzNEVn//"
"//9kMiEREiNW3////nVDMzM0RXj////2UyIRESNFfv///+dUMzMzRGef///+dUMhESI1aO////12QzMzNFaN////51QyERI0V5////+XVDMzM0V47////XVD"
"IRIjRXn////5dUMyMzRXn////5dUMhEiNGjf///+l1QzIzNGef////l1QyESI1aO////6GVDMzNFaK////6XUyIRI0V57////YZUMzM0Vo3////oZTIhIjRX"
"n////6hlQzMzRXnv///9hkMiEiNGiv////l2QzMzNFef////qGQyISI1aN////6XZDMzNFaK////+XVDIhIzV47////pdUMzM0Vor////pdUMhEjRXnv///9"
"h1QzMzRXjf///+hlMyESNFef////qGVDMzRFee////2GQyIRI0aK////+oZUQzNFaJ////+oZDIhEjRo3////qhlRDNEVor////5dUMhESNGjf///+qGVERE"
"VnnP///+l1QyERI1aO////2oZVREVWid////2GUyIREjVo7////ah2VUVWeK////+XZDIhESNWj////9qXZlVWeJr////5dUMhERI0aO////6qh2ZmeJrv//"
"/+hlMiERAjRo7////6qYh3iJrf////l1QyIRECNFff////6qqZmaqv////6GVDIREQIkV5/////+qqqqrf////+XVDIhEQAiNGjv//////7u7//////YZTMh"
"ERABI0V5///////////////odUMiERAAEiNGff/////////////odUMiEREAARI0Vo3////////////odUMiEREAABEiNFZ5//////////7IZUMyIREQAAER"
"IjRWeN7//////+mHZUMyIREQAAARESI0VWeInd3dmYh2VUMyIREQAAABEREiM0VWZ3d3d3ZlVDMiIREQAAAAARERIiM0RVVWZVVUQzIiEREQAAAAAAERESIi"
"MzRERERDMyIiEREQAAAAAAABERESIiIzMzMzIiIREREQAAAAAAAAABERERIiIiIiIiEREREQAAAAAAAAAAAAAAAAAAAREREREREREiIiIiIiIiIRERERERER"
"EAAAAAAAAAAAAAAAAAARERERERESIiIiIiIiIiIiIiERERERERAAAAAAAAAAAAAAAAERERERERIiIiIiMzMzMzMiIiIiIRERERERAAAAAAAAAAAAAAERERER"
"ESIiIiMzMzMzMzMzMzIiIiIREREREQAAAAAAAAAAAAEREREREiIiIzMzNEREREREQzMzMiIiIREREREAAAAAAAAAABERERERIiIjMzNERERVVVVVVEREMzMi"
"IiERERERAAAAAAAAABERERERIiIjM0REVVVVZmZmZVVVREMzMiIiEREREQAAAAAAABERERESIiIzNERVVWZmd3d3d2ZmVVVEMzMiIhEREREAAAAAABERERES"
"IiMzREVVZmd3eIiIiIh3dmZVVEMzIiIREREQAAAAABERERESIiMzRFVWZ3eIiZmZmZmYiId2ZVRDMyIiEREREAAAAAERERESIjM0RFVmd4iZ3e//////7ZmI"
"d2ZVRDMiIhERERAAAAERERESIjM0RVZneIne///////////+mId2VUQzIiEREREAAAERERESIjM0RVZ3iJ3//////////////+mHdlVEMyIhEREQAAERERES"
"IjM0RWZ3id/////////////////9iHZVRDMiIRERAAARERESIjM0RWZ4ie///////////////////ph2VUMyIiEREAARERESIjM0RWZ4nf//////////////"
"//////6YdlVDMiIREQARERESIiM0RWd4nv/////////////////////+mHZUQzIiERABERERIiM0RWd4nv///////////////////////odmVDMiIREBERER"
"IiM0RWZ4nv////////////////////////mHZUQzIiEQERERIiMzRVZ4nv/////////////////////////5h2VDMiIREREREiIzRVZ4nv//////////////"
"////////////2HZUQzIhEREREiIzRFZ4nf////////////7d7v///////////5h2VDMiIREREiIzNFZnif///////////aqqqqrf//////////2HZUQyIhER"
"ESIjNFVnie//////////6qqqqqqqr//////////pdlVDMiERESIjNEVnid//////////6qqqqqqqqq//////////mHZUMyIhERIiM0VWeJ//////////6qqq"
"mZmqqqrf////////+YdlQzIiEREiM0RWeJ7/////////6qqpmIiJmqqq/////////9h2VEMiIREiIzRVZ4n/////////+qqZiIiIiJqqrv////////6YZVQz"
"IhESIjNFZ4nv/////////aqZiHd3d4iaqt/////////ph2VDMiESIjNEVnif/////////qqZh3dmZ3eJmqr/////////mHZUMyIRIiM0Vnid/////////6qp"
"h3dmZmZ3iaqv////////+YdlQzIhEiI0RWeJ/////////+qpiHZmZmZneJqq/////////5h2VDMiEiIzRVZ5r/////////2pmHZmVVVmZ4iar/////////mH"
"ZUMyISIjNFZ4nf////////+qmHdmVVVVZneJqv////////+YdlQzIhIiNEVnif/////////aqYdmVVVVVWd4mq/////////5h2VDMiEiM0RWeK/////////6"
"qYd2VVVVVVZ3iar/////////mHZUMyIiIzRWeJ3/////////qph2ZVVUVVVmeJqv////////+YdlQzIiIjNFZ4nv////////6ph3ZVVERFVWZ4mq////////"
"/ph2VDMiIiNEVnif/////////amHZlVUREVVZniaz////////+mHZUMyIiI0RWeK/////////6qYdmVURERVVneJrf////////2YdlQzIiIzRFZ5r///////"
"//qph2ZVRERFVWd4mu/////////Zh2VDMiIjNFV4nf////////6qiHZVVEREVVZ4mq/////////6l2VEMyIiM0Vnid/////////amHdlVURERVZniar/////"
"////qHZUQyIiIzRWeJ7////////9qYdmVVRERVVmeJrP////////+YdlRDIiIjNFZ4nv////////qph2ZVVERFVWd4mt/////////ph2VDMiIiM0Vnif////"
"////+qmHdlVVRVVWZ4mq/////////9mHZUMyIiIzRWeJ/////////6qYd2VVVVVVZ3iar/////////qHZUQzIhIjNFZ4n/////////qph3ZlVVVVZniarf//"
"//////+YdlRDIiEiM0Vnif////////+qmIdmVVVVZneJqv/////////Zh2VDMiISIzRWeJ/////////6qph2ZlVVZmeJqt/////////6l2VUMyIhIjNFZ4n/"
"////////qqmHdmZmZneImq7/////////mHZUQyIhEiM0Vnif////////+qqYh3ZmZneImq3/////////2YdlQzIiESIzRWeJ7////////6qqmHd3Z3eImqr/"
"////////+YdlRDMiIRIjNFZ4nv////////2qqZiHd3eImarf/////////Zh2VDMiIREiM0RWed/////////qqqmYiIiImqqv/////////5h2VUMyIhESIjNF"
"Z43/////////qqqpmZiZmqqv/////////9mHZUMzIhERIiM0Vnif/////////qqqqpmZqqqu//////////mHZVQzIiERESIzRWeJ//////////2qqqqqqqqu"
"//////////2YdlRDMiERERIjNEVnjf/////////9qqqqqqqv//////////6YdlRDMiIREREiIzRWeJ///////////sqqqqrv//////////+YdlVDMiIRERES"
"IjNFV4nv///////////+7u/////////////Yh2VEMyIhEREREiM0RWeJ///////////////////////////Zh2VEMyIhERERESIjNFZnjv//////////////"
"///////////ph2VUMzIiERERAREiM0RWeJ/////////////////////////ph2ZUQzIiEREREBESIjNFVnjf///////////////////////ph2ZUQzIiERER"
"EAEREiI0RWd43//////////////////////Zh2ZUQzIiIREREQARESIjNEVneN/////////////////////Jh2ZUQzMiIREREQABEREiIzRVZ4jf////////"
"//////////6Yh2ZUQzMiIREREQAAERESIiM0VWd4nv////////////////yYd2VUQzMiIRERERAAAREREiIzNFVneI3//////////////tmIdmVUQzMiIRER"
"ERAAABERERIiMzRVZneJ3v//////////6ZiHdmVUQzMiIRERERAAAAERERESIjM0RVZ3iImd7v///u3ZmIh3ZlVEMzIiIRERERAAAAABERERIiIzNEVWZ3eI"
"iJmZmZmIiHd2ZVVEMzIiIRERERAAAAAAARERESIiMzREVWZnd3eIiIh3d3ZmVVRDMzIiIRERERAAAAAAAAERERESIiMzREVVZmZmd3dmZmZVVEQzMyIiIRER"
"EREAAAAAAAAREREREiIjMzRERVVVVWVVVVVUREMzMiIiERERERAAAAAAAAAAERERERIiIjMzNERERVVVREREQzMzIiIiERERERAAAAAAAAAAABERERERIiIi"
"MzMzREREREQzMzMyIiIhERERERAAAAAAAAAAAAARERERERIiIiIzMzMzMzMzMzIiIiIRERERERAAAAAAAAAAAAAAARERERERIiIiIiIzMzMiIiIiIiERERER"
"ERAAAAAAAAAAAAAAAAEREREREREiIiIiIiIiIiIiIREREREREQAAAAAAAAAAAAAAAAABERERERERERIiIiIiIiIREREREREREQAAAAAAAAAAAAAAERERERER"
"IiIiIiIiIiIiEREREREQAAARERERESIiIiIiIiIiIiIiIREREREQABEREREiIiIiMzMzMzMzMiIiIhEREREQERERESIiIzMzMzMzMzMzMzMiIiEREREBERER"
"IiIzMzREREREREREQzMyIiEREREREREiIzM0REVVVVVVVVVERDMzIiERERERESIjM0RFVVZmZmZmZmVVREMzIiERERERIiMzRFVmZnd3d3d3d2ZlVEMzIiER"
"ERESIzNFVmd3iIiIiIiIh3dmVUMzIhERERIiM0VWZ3iJmZmZmZmZiHdmVUMyIhERESIzRFb////////////////+VEMyIRERIiM0Vm/////////////////l"
"VDMiIRESIjRFbf////////////////1lRDIiEREiM0Vm7////////////////3ZUQyIhERIjNFZ/////////////////+HZUMyIRESIzRWf/////////////"
"///+h2VDMiEREiM0Vs/////////////////odlQzIhERIjNFbf////////////////2HZUMyIRESIzRW7////////////////5h2VDMiEREiM0Vv////////"
"////////+YdlQzIhERIjNFX////////////////+mHZUMyIRESIzRFZniaqqqq/////////ph2VDMiEREiIzRVZ4iaqqrf////////2YdlQzIiEREiM0RWZ4"
"maqq7////////6mHZUMyIRERIiM0RWZ4mqqv////////+phlVDMiERESIjNEVWeImqr////////+qHZUQyIhEREiIzNEVmeJqq/////////ph2VEMiIREREi"
"IzRFVniarf////////2YdlQzIiERERIiMzRFZ4mq7////////6mHZUMyIhERESIiM0RWeJqv////////+ph2VDMiIREREiIjNEVniar////////+qXZVQzIi"
"EREREiIzRFZ4mq/////////qh2VEMyIRERERIiM0RWeJrf////////2YdlRDIiERERESIjNEVnia7////////6mHZUMyIhEREREiIzRFZ4mv////////+ph2"
"VDMiIRERERIiM0RWeJr////////+qXZVQzIiERERESIjNEVnia/////////qh2VEMyIhEREREiIzRVZ4rf////////2YdlRDMiERERERIiM0VWea7///////"
"/6mHZUMyIhERERESIjNFZ4mv////////+ph2VDMiIREREREiIzRWeJr////////+qXZVQzIiERERERIiNEVnia/////////ph2VEMyIhEREREiIzRFZ4nf//"
"//////2YdlRDIiERERERIiM0RWeK7////////6mHZUMyIhERERESIjNFVnmv////////+ph2VDMiIREREREiIzRWeJr////////+qXZUQzIiERERERIiM0Vn"
"ia/////////ph2VEMyIhERERESIjRFZ4nf////////2YdlRDIiEREREREiM0RWeJ7////////6mHZUMyIhERERESIjNEVniv////////+phmVDMiIREREREi"
"IzRVaJr////////+qHZUQzIiERERERIiM0Vnia/////////ph2VEMyIRERERESIjNFZ4nf////////2YdlRDIiEREREREiI0RWeJ7////////6mHZUMyIhER"
"ERERIiNEVnif////////+pdlVDMiIRERERESIzRFZ4r////////+mHZUQzIhEREQEREiM0VWea/////////ph2VEMiIREREBERIjNFVonf////////2YdlQz"
"IiERERARESIzRWeJ7////////6h2VUMyIhEREQEREiM0Vnif////////+YdlRDMiIRERABERIjNFZ4n////////+mHZUMyIhEREQARESIzRWeJ/////////p"
"h2VDMiIREREAEREiM0Vnjf////////2HZUQzIiERERABERIjNFZ47////////5h2VDMiIREREAARESIzRVZ/////////+HZVQzIiERERAAEREiI0RWf/////"
"///+h2VEMiIREREQABERIiNEVn/////////nZVQzIiERERAAARESIjNFbf////////12VEMyIREREQAAERESIzRF7////////3ZUQzIiERERAAABEREiIzRf"
"////////9lVDMiIREREQAAARERIiM0RVZnd3d3ZmVUMzIiERERAAAAERERIiM0RVVmZmZmVUQzMiIREREQAAABERERIiM0RFVVVVVURDMyIhERERAAAAABER"
"ESIiMzRERERERDMzIiEREREQAAAAARERESIiMzMzREMzMzIiIRERERAAAAAAARERESIiIjMzMzMzIiIhEREREAAAAAAAARERERIiIiIiIiIiIhEREREQAAAA"
"AAAAARERERESIiIiIiIhERERERAAAAAAAAAAAREREREREREREREREREREAAAAAAAAAAAAAAAAAAAERERERERERIiIiIiIiIiIiEREREREREQAAAAAAAAAAAA"
"AAARERERERESIiIiIiIiIiIiIiIhEREREREQAAAAAAAAAAAAABEREREREiIiIiIzMzMzMzMiIiIiIRERERERAAAAAAAAAAAAEREREREiIiIjMzMzMzMzMzMz"
"MiIiIhERERERAAAAAAAAAAAREREREiIiIzMzNEREREREREMzMzIiIiERERERAAAAAAAAABERERESIiIzMzRERFVVVVVVREREMzMiIiERERERAAAAAAAAERER"
"ESIiIzM0REVVVWZmZmZlVVVEQzMyIiERERERAAAAAAARERERIiIjM0RFVWZmZ3d3d3dmZlVVRDMzIiIREREQAAAAAAEREREiIjM0RFVmZ3d4iIiIiIh3dmZV"
"VEMzIiIREREQAAAAARERESIiMzRFVmZ3iIiZmZmZmZiIh3ZlVEMzIiIREREAAAAAERERIiIzRFVmd3iJne7//////u2ZiHdmVUQzIiERERAAAAARERESIjNE"
"VWZ3id7////////////9mId2VUQzIiEREQAAAAERERIiM0RVZ3iO////////////////6Yd2VUQzIiEREAAAABEREiIzRFZniO///////////////////Yh2"
"VUMzIiERAAAAERERIiNEVWeO/////////////////////ph2VUMyIhEQAAABERESIzRFZ4///////////////////////5h2VEMyIhEAAAARERIiM0Vm3///"
"/////////////////////4d2VDMiIRAAAAERESIjRFbv/////////////////////////odlVDMiEQAAABEREiM0Ve///////////////////////////Ydl"
"QzIiEAAAARERIjNF7///////////////////////////+HZUQzIhAAAAERESIzTf////////////////////////////2HZUMyIQAAABEREiM0Xv////////"
"//7dyt3e////////////h2VDMiIAAAARERIiNEXf////////2qqqqqqq7//////////4dlRDIiAAAAERESIjRFZu//////2qqqqqqqqq3/////////+YZVQy"
"IgAAABERESIzRVZ9////6ZmZmZmaqqqq7/////////2HZUMyIAAAAREREiI0RVZ3///oiIiIiIiZqqqq/////////9h2VDMiAAAAARERIiM0RVZn79eHd3d3"
"eIiZqqqv/////////YdlQzIgAAAAERERIiM0RVVmZnd3dnd3d4iaqqrv////////2HZUMyIAAAABERERIiM0RFVWZmZmZmZnd4iaqq7////////5h2VDMiAA"
"AAABERESIiMzREVVVVVVVWZneImqqv////////+YdVQzIgAAAAARERESIiMzREREVVVVVWZniJqqr/////////l2VEMiIAAAAAARERESIiMzM0REREVVVmZ4"
"iaqt/////////odlRDIiAAAAAAERERESIiIzMzREREVVZneJmqrv////////2HZUMyIQAAAAABERERESIiIzMzNERFVWZ3iaqt/////////5h2VDMiEAAAAA"
"ABERERESIiIzMzRERVVneJmqr/////////2HZUQyIhAAAAAAAREREREiIiIzM0REVWZ4iaqv/////////5h2VDMiIQAAAAAAERERERIiIiMzNERVZneJqq//"
"////////2HZVQzIhEAAAAAABERERESIiIjMzRFVmd4mqr//////////ph2VDMiIRAAAAAAAREREREiIiMzNEVWZ3iZqv//////////mHZVQzIiEQAAAAABER"
"ERESIiIjM0RVZneJmt//////////+YdmVDMyIREAAAAAAREREREiIiMzREVmd4mq3//////////6mHZUQzIiERAAAAABERERESIiIzNEVWZ3iarv////////"
"//yYdlVDMiIREQAAAAARERERIiIjM0RVZniJqv//////////+ph2ZUQzIiEREAAAABEREREiIiMzRFVmeImt///////////6mHZlRDMiIRERAAAAERERESIi"
"IzNEVWd4ma7///////////qYd2VUMzIiEREQAAARERERIiIjM0RVZ3iarv//////////6piHZVRDMiIhEREAABEREREiIiM0RFZniJqv///////////qmIdl"
"VEMyIiERERAAERERESIiMzRFVmeImt///////////9qZh2ZUQzMiIhEREQABERERIiIzNEVWd4ma7///////////qpmHZlVEMyIiEREREAERERESIjM0RVZ3"
"iar///////////6qmYd2VUQzMiIhERERARERERIiMzRFVniJrf///////////aqph3ZlREMyIiIREREAEREREiIzNEVmeImt///////////8qqmIdmVUQzMi"
"IhERERARERESIjM0RWZ4ma7//////////+qqqph3ZlVEMzIiIREREQERERIiIzRFZniar///////////6qqqmYd2ZVREMyIiEREREBERESIjNEVmeJrf////"
"///////aqqqpmHdmZVRDMyIhERERARERIiM0RWZ4mu///////////6qqqqqZiHdmVURDMiIREREAERESIzRFVnia7//////////+qqqqqqqZiHdmVUQzIiER"
"ERABERIiM0VWeJr///////////2qqqqqqqqZiHdmVUQzIhEREQARESIzRFZ4nf//////////+qqqqqqqqqqpmHdmVEMyIhEREAERIiM0VneO///////////9"
"3d3d3d3d3d3d3d3cVDMiIRERABESIzRFZ4/////////////////////////////lRDIiEREQAREiM0Vn3/////////////////////////////5VQzIhEREA"
"ERIjNFZ//////////////////////////////9ZUMyIRERABEiI0RWf/////////////////////////////92VDMiEREQARIiNFVt//////////////////"
"////////////dlQzIhEREAESIjRWbv/////////////////////////////mZUMyIRERABEiM0VW//////////////////////////////5lVDMiEREQARIi"
"NEVv/////////////////////////////9ZUQyIhEREAESIjRFb/////////////////////////////9mVDMiERERABESIzRd//////////////////////"
"////////ZUQzIhEREQAREiM0Tv/////////////////////////////lVDMiIRERAAERIiM0VWZ3iJmZmqqqqqqqqqqqqqmZmZiHdmVUQzIhEREQABERIjNE"
"VWZ3eIiIiJmZmZmZmZmZiIiIh3d2ZVRDMiIREREAARESIjNERVZmZ3d3d3iIiIiIiHd3d3d3ZmZVREMyIhEREQAAERESIjM0RFVVZmZmZmZnd3ZmZmZmZmZl"
"VVREMzIiEREREAABEREiIjMzRERVVVVVVVVVVVVVVVVVVVVEREMzMiIhEREQAAAREREiIiMzM0RERERERVVVVVVERERERERDMzMiIiEREREAAAERERESIiIz"
"MzMzNERERERERERERDMzMzMzMiIiEREREQAAAAERERERIiIiIzMzMzMzMzMzMzMzMzMzMiIiIiIRERERAAAAAAEREREREiIiIiIiIjMzMzMzMzMiIiIiIiIi"
"EREREREQAAAAAAAAAAAAABERERERESIiIiIiIiIiIiIiIiIiIiIiEREREREQAAAAAAAAAAEREREREiIiIiIiIjMzMzMzMzMyIiIiIiIhEREREQAAAAAAAAAB"
"EREREiIiIiMzMzMzMzMzMzMzMzMzMyIiIiEREREQAAAAAAAAEREREiIiIzMzMzRERERERERERERDMzMzMiIiEREREAAAAAAAARERESIiIzM0RERERERVVVVV"
"VVREREREQzMyIiEREREAAAAAAAERERIiIzM0REVVVVVVVWZmZmZVVVVVVEREMzIiERERAAAAAAAREREiIjM0RVVWZmZmZmZ3d3d3ZmZmZmVVVEMzIiEREQAA"
"AAAAERERIiM0RVVmZ3d3d3eIiIiIiIh3d3d2ZlVEQzIiEREAAAAAAREREiIzRFVmd3iIiIiZmZmZmZmZmYiIh3dmVUQzIhERAAAAAAERERIjNEVWd4iJmZmq"
"qqqqqqqqqqqpmZiIdmVUMyIhEQAAAAABEREiIzRV7///////////////////////////VEMyIREAAAAAARERIjNEVv///////////////////////////2VD"
"MiERAAAAABERESIzRWb///////////////////////////9lQzIhEQAAAAAREREiNEVt///////////////////////////+ZUQyIREAAAAAERESIjRFbv//"
"/////////////////////////WVEMiIRAAAAABEREiI0VW7///////////////////////////xlVDIiEQAAAAARERIiNFVv////////////////////////"
"///3ZUQyIREAAAAAERESIjRFb///////////////////////////92VEMiERAAAAABEREiI0Rd///////////////////////////+dlQzIhEQAAAAAREREi"
"M0Xv//////////////////////////6HZUMyIREAAAAAARERIjNF7//////////////////////////odlVDMiERAAAAAAERESIzRFZneJmaqqqqqqqqqq//"
"///////+mHZUQyIhEQAAAAABEREiIzRFZneImZqqqqqqqqr/////////6YdlVDMiIREAAAAAAREREiMzRFZmd4iZqqqqqqqv/////////qmHZUQyIhERAAAA"
"AAERERIiM0RFVmd4iZqqqqqq/////////+qYdlVDMiIREQAAAAAAERERIiM0RFVmd3iZqqqqz/////////2ph2ZUQzIiEREAAAAAABERESIiMzRFVmd4iaqq"
"rf/////////amYdlRDMiIRERAAAAAAARERESIjM0RFVmeImqqt/////////9qph2VUQzIiEREQAAAAAAARERESIjM0RFZniJqq3/////////2qmHZlRDMiIh"
"EREAAAAAAAEREREiIjM0RWZ4mqrf////////+qqph2VUQzIiIRERAAAAAAAAEREREiIzNEVmeJqu/////////6qqmHZlRDMyIhEREQAAAAAAABERERIiIzRF"
"Z3ia7/////////qqqph2VUQzIiIREREAAAAAAAARERESIiM0RWeJrv////////+qqqqYdlVEMyIiERERAAAAAAAAERERESIjNFVnie/////////6qqqqmHZV"
"RDMiIhEREQAAAAAAAAEREREiIzRWZ47/////////qqqqqph2VUQzIiIREREAAAAAAAABERERIiM0Vnjf///////////aqqqYdmVEMyIiERERAAAAAAAAARER"
"ESIjNFZ47////////////+qqmHdlRDMiIhEREQAAAAAAAAEREREiI0RWeP//////////////6qmHZVQzIiIREREAAAAAAAARERERIiNEVnj/////////////"
"//+ph2ZUQzIiERERAAAAAAAAERERESIjRFZ9////////////////+ph2VEMyIhEREQAAAAAAAREREREiI0RWff////////////////6YdlVDMiIREREAAAAA"
"ABERERESIjNEVn7/////////////////6YdlQzIiERERAAAAAAEREREREiIzNFZv//////////////////mHZUQyIhEREQAAAAARERERESIiMzRVb///////"
"///////////+l2VUMyIREREAAAABERERERIiIjM0VWd4maqq3f///////////5h2VDMiERERAAAAEREREREiIiIzNEVmd4maqqqq7//////////YdlQzIhER"
"EQAAARERERESIiIjMzRFVmd4maqqqq7/////////6HZUMyIREREAABERERESIiIiMzNERVVmd4iaqqqq7////////+h2VDMiERERAAERERESIiIiMzMzREVV"
"Zmd4iaqqqt/////////5dlRDIiEREQARERERIiIjMzMzRERFVWZnd4iaqqqv////////+XZUQyIhEREBEREREiIjMzNEREREVVVWZneImqqqr/////////l2"
"VEMiIRERARERESIjMzRERERFVVVVZmZ3iJqqqq/////////4dlRDIiERERERERIiMzREVVVVVVVVZmZnd4iaqqqv////////6HZUMyIREREREREiIzREVVZm"
"ZmZmZmZnd3iJmqqq3////////9h2VDMiERERERESIjNEVW7Xd3d3d3d3d3iImaqqqv/////////YdlQzIhEREREREiIzRVZv/oiIiIiIiIiImZqqqq7/////"
"////mHVUMyIRERERESIjNFVn///+mZmZiIiZmZqqqqrv/////////odlRDIiEREREREiM0RWfv////6qqpmZqqqqqqqu//////////mHZUMyIhEREBERIjNF"
"Vn///////+2qqqqqqqre///////////odlVDMiERERARESI0RWf//////////+7u7u7/////////////mHZUQyIhEREQERIiNEVt////////////////////"
"/////////YdlVDMiIREREBESIjRVb////////////////////////////+mHZUQyIhEREQAREiI0Vf////////////////////////////6YdlRDMiIREREA"
"ERIiNF3////////////////////////////ph2VUMyIhEREQABESIjRN///////////////////////////+mHZlRDMiIREREAAREiIzRW7/////////////"
"////////////2YdmVEMyIhERERAAEREiM0VW3///////////////////////6Yh2ZUQzIiIREREAABERIjNEVmff////////////////////6Zh3ZVRDMiIh"
"ERERAAARESIjNEVmeN7/////////////////6ZiHZlVEMyIiEREREAAAERESIjNEVmd4je/////////////+2ZiHdlVUQzIiIREREQAAABEREiIzNEVWZ3iI"
"md7u/////u3ZmYh3dmVUQzMiIhEREREAAAAREREiIzNEVVZnd4iImZmZmZmYiId3ZmVURDMyIiEREREQAAAAEREREiIzNERVVmZ3d3iIiIiId3d2ZlVURDMy"
"IiIRERERAAAAAAEREREiIjM0RFVVZmZnd3d3d2ZmZVVURDMzIiIhEREREAAAAAABEREREiIjMzRERVVVVWZmZmVVVVVERDMzIiIhEREREQAAAAAAABEREREi"
"IiMzM0RERFVVVVVVREREQzMzIiIhERERERAAAAAAAAABERERESIiIjMzM0RERERERERDMzMyIiIiEREREREAAAAAAAAAABERERERIiIiIzMzMzMzMzMzMzMi"
"IiIiEREREREQAAAAAAAAAAABEREREREiIiIiIzMzMzMzMiIiIiIhEREREREQAAAAAAAAAAAAABERERERERIiIiIiIiIiIiIiIiIhERERERERAAAAAAAAAAAA"
"AAABERERERERESIiIiIiIiIiIiERERERERERAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARERERERERERERERERERERAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
"ABERERERERESIiIiERERERERAAAAAAAAAAAAAAAAAAAAAAAAAAAAERERERESIiIiIiIiIiERERERAAAAAAAAAAAAAAAAAAAAAAAAAAERERERESIiIiIiIiIi"
"IiIRERERAAAAAAAAAAAAAAAAAAAAAAAAARERERESIiIjMzMzMzMyIiIREREQAAAAAAAAAAAAAAAAAAAAAAABERERERIiIjMzNERERDMzIiIREREQAAAAAAAA"
"AAAAAAAAAAAAAAERERERIiIjM0RERFVEREQzMiIREREAAAAAAAAAAAAAAAAAAAAAAREREREiIjM0REVVVVVVVEQzMiIRERAAAAAAAAAAAAAAAAAAAAABERER"
"ESIiMzRFVWZmZmZmVUQzIiERERAAAAAAAAAAAAAAAAAAAAERERESIiIzREVWZ3d3d3dmVUQzIiEREQAAAAAAAAAAAAAAAAAAARERERIiIzNEVWZ37///////"
"//0yIhEREAAAAAAAAAAAAAAAAAABEREREiIjM0RVZnjv/////////UMyIRERAAAAAAAAAAAAAAAAAAERERESIiMzRFVneO/////////9RDMiEREQAAAAAAAA"
"AAAAAAAAARERERIiIzNEVmeI3/////////5UQzIhEREAAAAAAAAAAAAAAAABEREREiIjM0RWZ4jf/////////mVEMyIRERAAAAAAAAAAAAAAAAERERESIiMz"
"RFZnid/////////+dlRDMiEREQAAAAAAAAAAAAAAARERERIiIzNEVmeJn/////////93ZUQyIhEREAAAAAAAAAAAAAABEREREiIjM0RWZ4mf/////////4dl"
"VDMiIRERAAAAAAAAAAAAAAARERESIiMzRFZnia//////////iHZUQzIiEREQAAAAAAAAAAAAABERERIiIzNEVmeJr/////////+YdmVDMiIREREAAAAAAAAA"
"AAAAEREREiIjM0RWZ4mv/////////5iHZUQzIiERERAAAAAAAAAAAAARERESIiIzRFZnia7/////////2YdlVEMyIhERERAAAAAAAAAAABERERIiIjNEVmeJ"
"rv/////////Zh3ZUQzMiIREREQAAAAAAAAAAERERESIiM0RWZ4mu/////////9mYdlVEMyIiEREREAAAAAAAAAARERERIiIzRFVnia7/////////6ph3ZVRD"
"MiIhERERAAAAAAAAABEREREiIjNEVWeJnv/////////qmYdmVEMzIiIREREQAAAAAAAAERERESIiM0RVZ4md/////////+qph3ZVRDMyIiEREREAAAAAAAAR"
"ERERIiIzRFVniZ3/////////6qmYd2VURDMyIiEREREAAAAAABEREREiIjNEVWeJnf/////////6qpiHdmVUQzMiIhERERAAAAAAERERESIiM0RVZ4md////"
"//////qqqYh3ZlVUQzMiIhERERAAAAABERERIiIzRFVniZr/////////+qqqmYh3ZlVUQzMiIREREQAAAAEREREiIjNEVWeImv/////////6qqqpmIh3ZmVU"
"QzIiIREREQAAARERESIiM0RVZ4ia//////////2qqqqZmYiHdmVUQzIiEREREAAAERERIiIzNFVniJr//////////aqqqqqqmZiHdmVEMzIiEREREAARERES"
"IjM0RWd4mv/////////9qqqqqqqqqpmIdmVEMyIhERERABERERIiIzRFZ3ia7/////////6qqqqq////////9lVDMiIhEREQAREREiIjNEVmeJrv////////"
"/qqqqqqv///////+dlRDMiIRERERERERIiM0RWZ4mu/////////+qqqqqq3////////XZlQzMiIREREREREiIzNFVnia7/////////6qqqqqqu////////yH"
"ZUQzIiERERERERIiM0VWeJrv/////////6qqqqqqrv///////5h2ZUQzIiEREREREiIzRFZ4mt//////////qqqqqqqq////////+ph2VUMyIiERERERIiNE"
"VneJ3/////////+qqqqqqqqv///////+qYd2VEMyIhEREREiIzRVZ4nf/////////6qqqqqqqq3////////aqYdmVEMyIhERERIiNEVnid//////////yqqq"
"qqqqqu////////yqqYdlVDMiIRERESIzRWZ4r//////////aqqqqqqqqrv///////6qqmIdlRDMiERERIiNEVnif/////////9qqqqqqqqqq////////+qqq"
"mHZlQzIiERESIzRWZ5////////////////////////////////////1UQyIhEREiM0Vnj////////////////////////////////////9VUMyIRERIjNFZ+"
"////////////////////////////////////92VDMiEREiI0RWf/////////////////////////////////////dlQzIhERIiNFVn//////////////////"
"///////////////////nZUMyIRESIjRVZ/////////////////////////////////////12VDMiEREiI0VW3///////////////////////////////////"
"/9dlQzIhERIiNEVu////////////////////////////////////+HZUMyIRERIjNFbv////////////////////////////////////dlRDIiERESIzRW//"
"///////////////////////////////////nZUMyIREREiM0Rf////////////////////////////////////1lVDMiERERIiM0VWd4iZmqqqqqqqqqqqqq"
"qt/////////6qqqph2ZUQyIhERERIjNEVWZ3iIiZmZmaqqqqqqqqqv////////yqqpmHZlRDMiERERESIjNEVVZnd3eIiIiJmZmqqqqq3////////6qqmIdm"
"VEMyIhERERESIjNERVVmZmd3d3d4iImZqqqu////////+qqYh2ZUQzMiEREREREiIjM0REVVVWZmZmZ3d4iJqqrv///////+qpiHZVRDMyIhEREREREiIjMz"
"NERERVVVVVZmZ3iJqq/////////amIdlVEMzIiERERERERESIiIzMzNEREREVVVWZneJqv////////yZh2VUQzMiIREREQERERESIiIiMzMzM0REREVVZniJ"
"3////////6mHZVRDMyIiERERAAERERERIiIiIiMzMzMzRERVZnie////////+YdmVEMzIiIREREQAAERERERESIiIiIiIzMzM0RVZnjv///////+h2ZUQzMi"
"IhERERAAAAEREREREREiIiIiIiIzM0RVZn/////////XdlRDMyIiEREREAAAAAERERERERERIiIiIiIjM0RVZ/////////x2VUMzIiIRERERAAAAAAARERER"
"ERERERIiIiIjM0RV3////////2ZVQzMiIhEREREAAAAAAAARERERERERERESIiIjM0RVZnd4iId3ZlVEMyIiEREREQAAAAAAAAAAERERERERERERIiIjM0RF"
"VmZ3d2ZlVEQzIiIRERERAAAAAAAAAAAAABEREREREREREiIjM0RFVVVmVVVEQzMiIhEREREAAAAAAAAAAAAAAAAREREREREREiIiMzRERFVVREQzMyIiERER"
"EQAAAAAAAAAAAAAAAAAAERERERERESIiIzM0REREMzMyIiERERERAAAAAAAAAAAAAAAAAAAAABERERERESIiIjMzMzMzMyIiIREREREAAAAAAAAAAAAAAAAA"
"AAAAAAARERERERIiIiIiIiIiIiIREREREQAAAAAAAAAAAAAAAAAAAAAAAAABERERERESIiIiIiIiIRERERERAAAAAAAAAAAAAAAAAAAAAAAAAAAAERERERER"
"ERIiIhERERERERAAAAAAAAAAAAAAAAAAAAAAERERERERIiIiIiIiIiIiIiIiIiIREREREREAAAAAAAAAAAAAEREREREiIiIiIiIjMzMyIiIiIiIiIhERERER"
"AAAAAAAAAAAAERERESIiIiIzMzMzMzMzMzMzMzIiIiIhEREREQAAAAAAAAAAERERESIiIzMzMzREREREREQzMzMzMyIiIhERERAAAAAAAAAAEREREiIiMzM0"
"REREREREREREREREQzMzIiIREREQAAAAAAAAEREREiIjMzRERVVVVVVVVVVVVVVVRERDMyIiERERAAAAAAAAERERESIjM0RFVWZmZmZmZmZmZmZmVVVURDMy"
"IhEREAAAAAAAARERESIjNERVZmd3d3d3d3d3d3d3dmZlVUQzMiIREQAAAAAAARERESIjNEVWZ3eIiIiIiYiIiIiIiHd3ZlVEMyIhERAAAAAAARERESIjNEVW"
"d4iJmZqqqqqqqZmZmZiIh3ZlRDMiEREAAAAAABERERIiM0VW3////////////////////////UMyIhEQAAAAAAERERIiM0VWfv//////////////////////"
"/1RDMiERAAAAAAEREREiM0RWeP////////////////////////VUMyIREAAAAAAREREiIzRWZ4////////////////////////5lQzIhEQAAAAARERESIzRF"
"Z43////////////////////////mVDMiERAAAAABERESIjNFZ4nv///////////////////////9ZUMyIREAAAABERERIjNEVnif////////////////////"
"////dlQzIhEQAAAAERERIiM0VWed////////////////////////9mVDMiERAAAAAREREiIzRWeJ7////////////////////////mVEMyIREAAAAREREiIz"
"RFZ4r////////////////////////+ZUQyIhEQAAABERESIjNFZ4mv////////////////////////1VQzIhERAAAAERERIiNEVnid////////qqqqqqqqqq"
"qZmYh3ZVRDMiEREAAAARERIiM0RWeK7///////+qqqqqqqqpmZiId2ZVRDMiIREQAAAREREiIzRWeJr////////aqqqqqpmZiIh3dmZVRDMiIRERAAABERES"
"IjNFZ4nf///////6qqqqqZmIiHd3ZmVVRDMyIhEREAAAERERIiNEVnie////////qqqqqZiIh3d2ZlVURDMyIiEREQAAARERIiM0VWia////////6qqqqZmI"
"h3dmZlVURDMyIiERERAAAREREiIzRWeJr////////aqqqpmYiHd2ZlVURDMzIiIREREAABERESIjNFZ4nf///////6qqqqqZiId3ZmVURDMzIiIREREQAAER"
"ERIiNEVniu///////+qqqqqqmYiHdmZVREMzMiIhERERAAAREREiM0RWea////////2qqqqqqpmYh3ZlVUQzMiIiEREREAABERESIzRWeJ3///////+qqqqq"
"qqqpmId2ZVREMzIiIREREQAAERERIjNFZ4nv///////93d3d2qqqqpmHdmVUQzMiIhERERAAAREREiM0Vnif////////////////7aqZiHZlVEMyIiEREREA"
"ABEREiIzRWeJ///////////////////tqYh2ZUQzMiIREREQAAERESIjRFZ43////////////////////9mIdlVEMyIhERERAAERERIiNEVnj///////////"
"///////////piHZVQzIiIREREAAREREiI0RWeP//////////////////////+Yd2VEMyIhEREQABERESIjRFZ9////////////////////////mHZVQzIiER"
"ERAAERERIiNEVn7////////////////////////5h2VEMyIREREAAREREiI0RWf/////////////////////////6HZVQzIhEREQABERESIjNFZ/////////"
"/////////////////9h2VDMiIRERABERERIiM0Vd//////////////////////////+HZUQyIhEREAEREREiIzRV3u7u7u7u7u7u7v/////////////5h2VD"
"MiEREQARERESIjNEVWd4iZqqqqqqqqqs7///////////2HZUMyIRERABERERIiM0RVZneIiZmaqqqqqqqq7//////////odlQzIhEREBEREREiIzNEVWZ3eI"
"iImZmaqqqqqt//////////l2VEMiIREQEREREiIjM0RVVmZ3d4iIiJmaqqqqrv////////+XZUQyIhERARERESIiM0RFVWZmZ3d3eIiJmaqqqq/////////5"
"hlRDIiERERERESIiMzREVVVmZmZ3d3d4iJmqqqr/////////mGVEMiIRERERERIiMzREVVVmZmZmZ3d3eImaqqqv////////+YZUQyIhERERERIiIzREVVVm"
"ZmZmZmd3d4iJqqqq/////////5hlRDIiEREREREiIzNEVVZmZmZmZmd3d3iJmqqqr////////+l2VEMiIREREREiIzNEVWZnd3d3d3d3d3iImaqqrf//////"
"//6HZUMyIRERERESIjNEVm3nd4iId3d3eIiImaqqqv/////////YdlQzIhERERERIjNEVmf/6IiIiIiIiIiZmaqqqu/////////5h2VDMiERERERIiM0VWfv"
"//6ZmZmZmZmZqqqqqu/////////+h2VEMiIREREREiM0RWff////6qqqqqqqqqqqqu//////////mHZUMyIhERERESIzRWZ////////tqqqqqqqq3v//////"
"////6XZVQzIhERERESIjNFZ+//////////7u7u7v////////////+YdlRDIiERERERIiNEVt/////////////////////////////odlVDMiIREREREiI0RW"
"/////////////////////////////ph2VEMiIREREBESIjRV7////////////////////////////5h2VUMyIhEREQERIiNE3///////////////////////"
"/////5h2VUMyIhERERAREiI0Rf///////////////////////////ph2ZUQzIiERERABERIjNFbv/////////////////////////Zh2ZUQzIiEREREAEREi"
"M0VW3///////////////////////6Yh2ZUQzIiIREREAARESIjRFZn3/////////////////////2Yd2VUQzIiIREREQABERIiM0RWZ4jv//////////////"
"///smIdmVUQzIiIREREQAAERESIjNEVmd4je/////////////+2ZiHdmVUQzIiIREREQAAAREREiMzRFVmd4iJne7v////7t2ZmIh3ZlVEMzIiIRERERAAAB"
"ERESIjM0RVVmd3iIiZmZmZmZiIh3dmZVREMzIiIRERERAAAAEREREiIjNERVVmZ3d3iIiIiId3d2ZlVURDMyIiIRERERAAAAABERERIiIzNERFVWZmZnd3d3"
"dmZmVVVEQzMyIiIRERERAAAAAAARERESIiIzNERFVVVVZmZmZVVVVUREMzMiIiERERERAAAAAAABEREREiIiMzM0REREVVVVVVVEREQzMzIiIiERERERAAAA"
"AAAAAREREREiIiIzMzNEREREREREQzMzMyIiIhERERERAAAAAAAAAAEREREREiIiIjMzMzMzMzMzMzMzIiIiIRERERERAAAAAAAAAAABEREREREiIiIiIzMz"
"MzMzMiIiIiIhERERERERAAAAAAAAAAAAARERERERESIiIiIiIiIiIiIiIiIREREREREQAAAAAAAAAAAAAAERERERERERIiIiIiIiIiIiIREREREREREQAAAA"
"AAAAAAAAAAAAAAAAAAABERERERERERIiIiIiIiIiIiEREREREREQAAAAAAAAAAAAAAARERERERERIiIiIiIiIiIiIiIiIRERERERAAAAAAAAAAAAABERERER"
"ERIiIiIiIzMzMzMzIiIiIiEREREREAAAAAAAAAAAARERERERIiIiIjMzMzMzMzMzMzMyIiIhEREREQAAAAAAAAAAEREREREiIiIjMzMzRERERERERDMzMiIi"
"IREREQAAAAAAAAARERERERIiIjMzNERERFVVVVVVREREMzMiIhERERAAAAAAAAEREREREiIiMzNEREVVVWZmZmZlVVVERDMyIiERERAAAAAAABERERESIiIz"
"M0RFVVZmZnd3d3d2ZmZVVEQzMiIREREAAAAAAREREREiIjMzREVVZmd3d4iIiIiHd3dmZVRDMyIhEREAAAAAERERERIiIzNERVZmd3iIiZmZmZmZiIh3ZlVE"
"MyIhEREAAAABEREREiIjMzRFVmd3iImc3u///////+3Yd2ZUQzIiEREAAAARERERIiIzNEVVZ3iImd///////////////nZlRDIiEREAAAERERESIiMzRFVm"
"eImd///////////////////VVDMiEREAAAEREREiIjM0RWZ3iZ7/////////////////////1DMiEREAABERERIiIzRFVmeInf//////////////////////"
"1EMiIREAARERESIiM0RVZ3ic////////////////////////VEMiIREAARERESIjNEVWd4nv///////////////////////2VEMiIREAEREREiIzNFVniJ7/"
"///////////////////////mVEMiIREBERERIiMzRVZ4mf////////////////////////92VDMiEREBERESIiM0VWeJn/////////////////////////1m"
"VDMiERERERESIjNEVniJ/////////////////////////+dlRDMiEREREREiIzRFZ3if/////////////t2qqqqt3v///3ZlQzIiEREREREiIzRVZ4nv////"
"///////9qqqqqqqqmZ3v/WZUQzIiERERERIiM0RWeJ3///////////2qqqqqmZmZmIiN5mVUMyIhERERESIjNEVnia///////////qqqqqmZmIiIiHd2ZlVE"
"MyIhERERESIjNFVnie//////////yqqqqpmYiId3d3ZmVVRDMiIRERERESIzRFZ4nf/////////6qqqqqZiIh3d3ZmZVVEQzIiIREREREiIzRVZ4r///////"
"//+qqqqqmZiId3dmZmVVREMzIiEREREREiI0RWeJ3/////////2qqqqqmZiId3dmZlVURDMyIiERERERIiM0VWeK/////////+qqqqqqqZmIh3dmZVVERDMy"
"IiERERERIiM0Vnid/////////6qqqqqqqpmZiHd2ZlVEQzMyIiERERERIjNEVniv/////////aqqqqqqqqqZmIh3ZlVUQzMiIiERERESIjNFZ4mv////////"
"+qqqqqqqqqqqqZiHdmVURDMyIiERERESIjNFZ4nv////////6qqqqq3e7u3aqqmYh2ZVRDMyIiERERESIjRFZ4r/////////qqqt7////////tqZiHZlVEMy"
"IiERERESIzRWeJr/////////qq7////////////qmIdmVEMzIiEREREiIzRWeJ3////////+rv//////////////6Yh2VUQzIiEREREiIzRWeK7////////9"
"/////////////////5iHZVQzMiIREREiI0RWea////////////////////////////mHdlRDMiIREREiM0RWia////////////////////////////+YdlVD"
"MiIREREiM0Vnid/////////////////////////////5h2VEMyIREREiM0Vnid/////////////////////////////+h2ZUMyIhEREiM0Vnie//////////"
"////////////////////2HZUQyIhEREiM0Vnie///////////////u7v////////////+HZVQzIhEREiM0Vnif////////////7aqqqqre//////////+Ydl"
"QzIhEREiM0Vniv///////////9qqqqqqqq3//////////odlQzIiEREiM0Vniv//////////+qqqqqqqqqrf/////////4dlRDIiEREiM0Vniv//////////"
"qqqqqqqqqqqt/////////5hlRDIiEREiM0Vnif/////////9qqqqqpmaqqqq/////////5h2VDMiEREiM0Vnif/////////6qqqpmZmZqqqq7////////5h2"
"VDMiEREiM0Vnie/////////aqqqZmIiZmqqq3////////8h2VDMiEREiM0Vnie////////+qqqqZiIiJmqqq3////////5h2VDMiEREiM0VXie////////+q"
"qqqZiIiImaqq3////////5h2VDMiEREiI0RWed////////+qqqqZiIiImaqq3////////5h2VDMiEREiIzRWeJ////////+qqqqZiIiJmqqq7////////5hl"
"VDIiEREiIzRWeJ/////////aqqqZmIiZmqqq/////////odlRDIiERESIzRVeJ7////////6qqqpmZmaqqqt/////////odlQzIiERESIzRFZ43////////9"
"qqqqqqqqqqqv////////+YdlQzIhERESIjNFZ4n/////////qqqqqqqqqqrv////////+XZVQzIhERERIjNFVnjf////////+qqqqqqqqq7/////////2HZU"
"QyIhERERIiNEVnif/////////+qqqqqqrf//////////mHZUMyIhERERIiM0VWeN///////////t3d3e///////////9h2VEMyIREREREiI0RWeJ7///////"
"///////////////////od2VDMiIREREREiIzRFZ4n/////////////////////////+YdlRDMiIRERERESIjNFZnjP////////////////////////mHZVQz"
"IiERERARESIjNEVmeN///////////////////////5h2ZUMzIiERERARERIiM0RWd4z/////////////////////+YdmVEMyIhEREQARERIiIzRFZ3if////"
"///////////////+mHZlRDMiIREREQAREREiIzNEVmeI3//////////////////Ih2ZUQzIiIREREAARERESIjM0RWZ3id///////////////tmHdlVEMzIi"
"EREREAARERERIiMzRFVneIne///////////+mIh2ZVRDMyIhERERAAABEREREiIzNEVWZ3iImd7v///+7dmYh3ZmVUQzIiIRERERAAAAERERESIiM0RFVmZ3"
"iIiJmZmZiIh3dmZVRDMyIiEREREQAAAAARERERIiIzNERVZmZ3d3iIiHd3dmZVVEQzMiIhEREREAAAAAABEREREiIjMzREVVVmZmZ3dmZmZVVURDMyIiIRER"
"ERAAAAAAAAERERESIiIzM0REVVVVVVVVVVVUREMzMiIiEREREQAAAAAAAAEREREREiIiMzM0REREVVVEREREMzMyIiIREREREAAAAAAAAAABERERESIiIiMz"
"MzNEREREQzMzMyIiIiERERERAAAAAAAAAAAAEREREREiIiIiMzMzMzMzMzMyIiIiIREREREQAAAAAAAAAAAAAREREREREiIiIiIiIzMiIiIiIiIREREREREA"
"AAAAAAAAAAAAABEREREREREiIiIiIiIiIiIiIRERERERERAAAAAAAAAAAAAAAAARERERERERESIiIiIiIiEREREREREREAAAAAAAAAAAAAERERERERIiIiIi"
"IiIiIiIiIiIiIiIiIiIiIREREREREAAAARERERESIiIiIiIjMzMzMzMzMzMzMzMiIiIiIiERERERAAABERERESIiIiMzMzMzMzMzMzMzMzMzMzMzMzIiIiIR"
"ERERAAERERESIiIzMzMzREREREREREREREREREQzMzMzIiIhERERARERERIiIzM0RERERERVVVVVVVVVVVVUREREREMzMiIhEREQEREREiIzM0REVVVVVVVW"
"ZmZmZmZmZmVVVVVVREQzMyIhERERERESIjM0RFVWZmZmZmZnd3d3d3d3d2ZmZmZlVURDMyIhERERERIiMzRFVmZ3d3d3eIiIiIiIiIiIiId3d3dmZVRDMyIR"
"EREREiIzRFVmd3iIiImZmZmZmZmZmZmZmZmYiId2ZVRDMiIRERERIiM0VWZ3iJmZqqqqqqqqqqqqqqqqqqqpmYh3ZlRDMiEREREiIzRFbf//////////////"
"/////////////////lQzIiERERIiNEVm7///////////////////////////////1UQyIhEREiIzRVZ////////////////////////////////2ZUMyIRER"
"IiM0Vnj///////////////////////////////92VDMiERESIzRFZ4////////////////////////////////dlQzIhEREiM0Vnjf//////////////////"
"/////////////nZUQyIRESIjNFZ47///////////////////////////////12VDMiEREiI0RWeP///////////////////////////////8dlQzIhERIjNF"
"Z4n///////////////////////////////+HZUMyIRESIzRWeJ///////////////////////////////+h2VDMiEREiM0Vnjf//////////////////////"
"////////+YdlQzIhERIjNFZ47///////6qqqqqqqqqqqqv/////////9h2VEMiIRESIzRWeP///////9qqqqqqqqqqqq3/////////6YdlQzIiEREiM0Vnj/"
"//////+qqqqqqqqqqqqv/////////6l2VUMyIRERIjNFZ4////////qqqqqqqqqqqq7/////////2YdlRDIiERESIzRWff///////qqpmZmZqqqqrf//////"
"///qmHZUMyIhEREiM0Vn7///////6qmYiImZqqqq//////////qYdlRDMiERERIjNFVv///////9mYiIiImaqqrv/////////qmGZUMyIhERESIjRFb/////"
"//+ZiId3iImqqt//////////qYdlRDMiIREREiIzRV////////iId3d3iJqqr//////////Zh2VUMyIhEREREiM0Tf///////od3d3d4iaqu/////////+qY"
"dlRDMiIRERERIiM0VWZ3d4h3d3ZmZ3eJmqr/////////+ph2VUMyIhERERESIjM0RVZmd3d2ZmZmd3iaqv/////////9qYdlRDMiIRERERESIjM0RVVmZmZm"
"ZmZneImq3/////////6ph2VEMyIhERERERESIjM0RFVVVVVVVmZ3iZqv/////////6mHZlQzIiIREREBEREiIjMzREREVVVVVmd4mq7/////////2ph2VEMy"
"IhERERARERESIiMzNERERFVVZniZrf/////////6mHZVQzIiIREREAERERESIiIzMzRERFVWd4mq//////////yYdmVEMyIhERERAAERERESIiIzMzNERVZn"
"iZrv/////////qmHZUQzIiIREREAAAERERESIiIjMzREVWZ4mq//////////qYdlVDMiIhEREQAAABERERESIiIjMzRFVniZr//////////Zh2ZUQzIiERER"
"EAAAABERERESIiIzNERWZ4mt/////////+qYdlRDMiIhEREQAAAAABEREREiIiMzRFVniar/////////+ph2VUMyIiEREREAAAAAARERERIiIjM0RWZ4mu//"
"///////9mHdlRDMiIREREQAAAAAAERERESIiIzNFVnia3/////////+ph2VEMyIiERERAAAAAAAAEREREiIjM0RWZ4mv/////////6mHZVQzMiIREREQAAAA"
"AAABERERIiIzNFVnia7/////////6ph2VEMyIiERERAAAAAAAAARERESIiM0RWd4nf/////////6mHZUQzIiIREREAAAAAAAAAEREREiIzNFVnia////////"
"//2YdmVEMyIhERERAAAAAAAAABERESIiM0RWeJrv/////////qmHZUQzIiIREREAAAAAAAAAEREREiIzNFVnia//////////qYdlVDMiIhEREQAAAAAAAAAB"
"ERERIiM0RWeJr//////////Zh2ZUQzIiEREREAAAAAAAAAAREREiIjNFVnid//////////qYdlRDMiIhEREQAAAAAAAAAAERERIiM0RWeJr/////////+ph2"
"VUMzIiEREREAAAAAAAAAABERESIjNFVnie/////////+mHdlRDMiIREREQAAAAAAAAAAERERIiM0RWeJ3/////////+ph2VEMyIiERERAAAAAAAAAAABERES"
"IjNFVnif/////////9mHZVQzMiIREREQAAAAAAAAAAAREREiM0RWeJ7/////////6Yh2VEMyIiERERAAAAAAAAAAAAERESIjNFVnif/////////6mHZUQzIi"
"IREREAAAAAAAAAAAAREREiIzRWeJ//////////2YdlVEMyIhERERAAAAAAAAAAAAERERIjNEVnjf/////////5h3ZUQzIiIREREAAAAAAAAAAAABERESIzRW"
"d4//////////qYdlRDMiIhERERAAAAAAAAAAAAAREREiM0Vnjv/////////ph2VUMzIiEREREAAAAAAAAAAAAAERESIjRFZ9//////////mHZlRDMiIREREQ"
"AAAAAAAAAAAAABEREiI0RWf//////////Yd2VEMyIiEREREAAAAAAAAAAAAAARERIjNEVu/////////+iHZVQzIiIREREQAAAAAAAAAAAAAAERESIzRF3///"
"//////+IdlVDMyIhERERAAAAAAAAAAAAAAABEREiI0Rf/////////9h2VUQzIiEREREQAAAAAAAAAAAAAAARERIiM07/////////53ZVRDMiIhERERAAAAAA"
"AAAAAAAAAAARERIjNP/////////3ZlVEMyIiEREREAAAAAAAAAAAAAAAAAERESIjRFVmd3d3d3dmVUQzIiIREREQAAAAAAAAAAAAAAAAABEREiIzRFVWZmZm"
"ZmVURDMiIhEREREAAAAAAAAAAAAAAAAAAREREiIzREVVVVVVVURDMyIiEREREQAAAAAAAAAAAAAAAAAAERERIiIzNEREVVRERDMzIiIRERERAAAAAAAAAAAA"
"AAAAAAAAERERIiIzMzREREQzMzIiIhEREREAAAAAAAAAAAAAAAAAAAABERERIiIjMzMzMzMzIiIhEREREQAAAAAAAAAAAAAAAAAAAAABEREREiIiIiIiIiIi"
"IiERERERAAAAAAAAAAAAAAAAAAAAAAARERERESIiIiIiIiIhEREREREAAAAAAAAAAAAAAAAAAAAAAAABERERERERIiIiEREREREREQAAAAAAAAAAAAAAAAAA"
"AAAAAAAAAAAAAAERERERERERIiIiIiIiIiIiERERERERERAAAAAAAAAAAAAAAAEREREREREiIiIiIiIiIiIiIiIhEREREREQAAAAAAAAAAAAABERERERESIi"
"IiIjMzMzMzMyIiIiIhEREREREQAAAAAAAAAAABEREREREiIiIjMzMzMzMzMzMzMyIiIiEREREREAAAAAAAAAABERERERIiIiMzMzRERERERERDMzMzIiIiER"
"ERERAAAAAAAAABERERESIiIjMzREREVVVVVVVUREQzMzIiIhEREREQAAAAAAABERERESIiMzNERFVVVWZmZmZlVVVERDMzIiIREREREAAAAAABEREREiIiMz"
"REVVVmZnd3d3d3dmZlVURDMyIiIRERERAAAAAAEREREiIjM0RFVWZnd3iIiIiIiHd3ZmVURDMyIiEREREAAAAAEREREiIjM0RVZmd4iImZmZmZmZmIh3dmVU"
"QzMiIhEREQAAAAEREREiIjNERVZneImd3v//////7tmYiHdmVUQzIiIRERAAAAARERESIjNEVWZ3iJ3/////////////7ZiHZlVEMyIhEREAAAARERESIjNE"
"VWd4ie/////////////////Zh3ZVRDMiIREQAAARERESIjNEVWd4nv///////////////////Yh2VUQzIiERAAABERESIjM0VWeI3///////////////////"
"//+YdlVDMiIREAAAERERIiM0VWeJ7///////////////////////mHZUQzIiEQAAERERIiM0RWeJ/////////////////////////5h2VEMiIRAAAREREiI0"
"RWeI7/////////////////////////+HZVQzIhEAAREREiIzRVZ47//////////////////////////+h2VEMiIQABERESIjRFZ43///////////////////"
"////////+XZVQzIhAAERERIjNFVnj///////////7aqqqt7//////////+h2VDMiEAARERIiM0Vnjf/////////+qqqqqqqq7/////////+HZUQyIgAREREi"
"I0RWeO/////////9qqqqqqqqqu/////////8hlRDIiABERESIzRVeJ/////////+qqqqqqqqqqr/////////2HZUMyIAERERIjNFZ4n/////////qqqqqqqq"
"qqqt/////////odlQzIgARERIiM0Vnjf////////2qqqqqqqqqqqr////////+h2VDMiAREREiI0RWeN////////+qqqqqmZqqqqqv////////6HZUMyIBER"
"ESIjRFZ57////////6qqqqqpqqqqqq/////////YdlQzIgERESIjNEVnnv////////qqqqqqqqqqqqr////////9h2VDMiERERIiM0VWid////////+qqqqq"
"qqqqqqrf////////mHZUMyIREREiIzRVaJ3////////9qqqqqqqqqqqv////////+YZVQzIhERESIjNFVomv////////+qqqqqqqqqqt/////////YdlRDIi"
"ERERIiM0Vmia/////////+qqqqqqqqqt/////////5h2VDMiIRERIiIzRWeJrf/////////qqqqqqqqu/////////+mHZUMyIhEREiIzRFZ4mq//////////"
"/tqqqq3v//////////qHZVQzIiERESIjNEVniarf//////////////////////////qYdlRDMiIRESIiM0RWeJqq7/////////////////////////2ph2VD"
"MiIRERIiMzRVZ4mqqu////////////////////////2ph2VUMyIhEREiIzRFZ4mqqq3///////////////////////qqmHZUQzIiEREiIjNFVniaqt//////"
"/////////////////9qqqYdlRDMiIRESIjNEVniaqv///////////////////////8qqqpdmVDMyIhERIiM0VWeJrf/////////////////////////aqqh2"
"VUMyIiERIiM0RWeJrv//////////////////////////2qmHZVQzIiEREiIzRVZ4nv///////////////////////////9qodlVDMiIRESIjRFZ4nv//////"
"/////+7d3e////////////+ql2VUMyIhERIjNFVnjf//////////6qqqqqqq3//////////+qYZlQzIiERIiM0Vnif/////////+qqqqqqqqqq3/////////"
"+ph2VDMiIREiI0RWeN/////////9qqqqqqqqqqqt/////////+mHZUMyIhESIzRVaJ/////////+qqqqqqqqqqqqrv////////+YdlQzIiERIjNFZ4n/////"
"////qqqqqZmZmaqqqq/////////5h2VDMiIREiM0Vnjf////////2qqqqZmIiZmqqqrv////////2HZUMyIhESIzRWeO////////+qqqqZiIiImZqqqt////"
"/////YdlQzIiESIjNFZ5/////////6qqqZiIiIiJmqqq3////////9h2VDMiIRIiNEVnn/////////qqqpmIiIiImaqqrv////////2HZUMyIREiI0RWef//"
"//////+qqqmYiIiIiZqqqu/////////IdlQzIhESIjRFZ5/////////6qqqpmIiIiZqqqq/////////5h2VDMiERIiM0Vnj/////////2qqqmZmImZmqqq3/"
"////////mGVEMiIRESIzRWeP/////////qqqqqmZmZqqqqr/////////6HZUQyIhERIjNFZ47/////////2qqqqqqqqqqqrv/////////YdlQzIhEREiM0Vn"
"jf/////////+qqqqqqqqqqr//////////5dlVDMiERESIjRFZ4///////////9qqqqqqqq7//////////9h2VEMiIRERIiM0Vnjv///////////+3Kqs3v//"
"//////////h2ZUMyIhERESIzRVZ4//////////////////////////////yHZUQzIhERERIiNEVnjf////////////////////////////2HZVQzIiEREREi"
"IzRVZ47///////////////////////////6HZlRDIiERERERIiNEVmeO//////////////////////////2IdlRDMiIREREREiIzRFZ3jf//////////////"
"//////////2IdlVDMiIRERERERIiM0VWd4z//////////////////////+mHdlVDMiIhEREQEREiIzNFVmeJ3////////////////////tiHZlVDMyIhERER"
"ARERIiMzRVZniJ3/////////////////7Jh3ZlVDMyIhERERABERESIjM0RVZ3iJnf/////////////tmIh2ZVRDMyIhEREREAEREREiIzNEVWZ3iImZ3u7/"
"///+7dmZiHdmZVRDMyIiEREREAARERERIiIzREVWZ3eIiImZmZmZmIiId3ZmVURDMyIiEREREAAAERERESIiMzREVWZmd3d4iIiIiHd3dmZVVEQzMiIhERER"
"EQAAABEREREiIjMzREVVVmZmd3d3d2ZmZlVVREMzMiIhEREREQAAAAARERERIiIjMzRERVVVVWZmZmVVVVRERDMzIiIhEREREQAAAAABERERERIiIjMzNERE"
"RFVVVVVUREREMzMyIiIREREREQAAAAAAABERERESIiIiMzMzRERERERERDMzMzIiIiEREREREQAAAAAAAAARERERESIiIiIzMzMzMzMzMzMzMiIiIhERERER"
"EAAAAAAAAAAAERERERERIiIiIiIzMzMzMzIiIiIiIREREREREAAAAAAAAAAAABEREREREREiIiIiIiIiIiIiIiIhEREREREREAAAAAAAAAAAAAABERERERER"
"ESIiIiIiIiIiIhERERERERERAAAAAAAAAAAAAAAAAAABERERERERERIiIiIiIiIiERERERERERAAAAAAAAAAAAAAAAARERERERESIiIiIiIiIiIiIiIRERER"
"EREQAAAAAAAAAAAAABERERERESIiIiIiMzMzMyIiIiIiIRERERERAAAAAAAAAAAAARERERERIiIiIzMzMzMzMzMzMiIiIiEREREREAAAAAAAAAAAEREREREi"
"IiIzMzNEREREREQzMzMyIiIhEREREQAAAAAAAAABERERESIiIjMzREREVVVVVVRERDMzMiIiERERERAAAAAAAAAREREREiIiMzNERVVVVmZmZlVVVERDMzIi"
"IhEREREAAAAAAAERERESIiIzNERVVWZmd3d3d2ZmZVVEQzMyIiEREREQAAAAABEREREiIjMzRFVWZnd3iIiIiId3dmZVVEMzIiIRERERAAAAABERERIiIzNE"
"VVZneIiJmZmZmZiIh3dmVURDMiIhEREREAAAARERESIiMzRFVmd4iZ3u//////7dmIh3ZlVEMyIiEREREQAAEREREiIjNEVWZ3iJ3////////////tmIdmVU"
"QzMiIREREQAAEREREiIzRFVmeInv///////////////ph3ZlRDMyIhEREQABERERIiM0RVZ3ie//////////////////2IdmVEMyIiEREQABERESIjNEVWeI"
"3////////////////////ph2ZUQzIiEREQAREREiIjNFVniO//////////////////////mHZlRDMiIREQAREREiIzRFZ4jv//////////////////////+Y"
"dmVEMiIhEQEREREiM0RWeI7////////////////////////5h2VUMyIhEQERERIiM0VWeN//////////////////////////mHZUQzIhEQERERIjNEVnjP//"
"////////////////////////6HdlQzIiERERESIjNFZ4n///////////7aqqre//////////+YdlRDMiERERESIjRFZ43//////////qqqqqqqrv////////"
"/ph2VDMiIRERESIzRWeJ//////////2qqqqqqqqt/////////8h2VEMyIREREiIzRWeN/////////9qqqqqqqqqq3////////+mHZUMyIREREiI0RWeO////"
"/////aqqqqmZqqqqrv////////mHZUMyIhEREiI0VWif////////+qqqqZmZmaqqqv////////2HZUQyIhEREiM0Vnif////////2qqqmZiImZqqqu//////"
"//6YdlQzIhEREiM0Vnjf////////qqqpmYiIiZqqqu////////+YdlQzIhEREiM0Vnjf////////qqqpmIiIiZqqqt////////+YdlQzIhEREiM0Vnjv////"
"////qqqpmIiIiZqqqt////////+odlRDIhEREiM0Vnjv////////qqqpmIiImZqqqu/////////ZdlRDIhEREiM0Vnjv////////qqqqmZiJmaqqqv//////"
"///ZhlVDMhEREiM0Vnjv////////2qqqmZmZmqqqrf/////////Zh2VDMhEREiM0Vnjf////////6qqqqqmqqqqqrv/////////Zh2VDMhEREiM0Vnjf////"
"/////Kqqqqqqqqqq7//////////ph2VDMhEREiM0Vnif/////////6qqqqqqqqqt///////////ph2VDMhEREiI0RWeP//////////2qqqqqqqrv////////"
"///Zh2VDMhEREiIzRWeN///////////9qqqqre/////////////Zh2VDMhEREiIzRWeJ///////////////////////////////Zh2VDMhERESIzRFZ47///"
"//////////////////////////+phlVDMhERESIjNFZ4n/////////////////////////////+pdlRDIhERESIjNFVnif//////////////////////////"
"//+pdlRDIhERERIjNEVneN////////////////////////////6odlQzIhERERIiM0RWeI3///////////////////////////6YdlQzIgERERIiMzRVZ4mf"
"////////////////3v////////2YdlQzIgERERIiIzRFZniJ3//////////////6r/////////qXZUQzIgERERIiIzNFVmeImu////////////2qr///////"
"//qHZUQyIgEREREiIjNEVWZ4iZre///////+2qqq7////////9mHZUMyIgEREREiIjM0RVZniJmqqq3d3cqqqqqq/////////6mGZUMyIgEREREiIjM0RFVm"
"d4iZmqqqqqqqqqqt/////////5h2VEMyIQEREREiIjM0RFVmZ3iImZqqqqqqqqqv/////////Zh2VDMiIQEREREiIjM0RFVWZneIiZmaqqqqqqrv////////"
"+pdlVDMiIQEREREiIjM0RFVWZmd3iImZqqqqqqr/////////2YdlRDIiERERERIiIzNERVVWZmd3iIiZmqqqqq//////////qYZVQzIiERERERIiIzNEVVVm"
"Znd3iIiZqqqqqu/////////9mHZUQzIhERERERIiM0RFVWZmd3d4iImZqqqqrv/////////5h2VUMyIhERERESIjM0RVZmd3d3iIiJmaqqqq7//////////Z"
"h2VEMyIhERERESIjNEVWZ3eIiIiZmZqqqqrf//////////6YdlVDMiIREREREiIzRFVt/YiJmZmZqqqqqq7///////////mHZlQzMiIREREREiIzRVZ///2Z"
"qqqqqqqq3v///////////6mHZUQzIiEREREREiM0RWfv////7d3Krd7v/////////////Zh2VUMyIhERERERIiM0Vmf/////////////////////////2Ydl"
"VDMyIhERERERIiNEVn/////////////////////////9mHZlRDMiIRERERERIiNFVt/////////////////////////Zh2ZUQzIiEREREBERIjNFVv//////"
"//////////////////2YdmVEMyIiEREREBERIjNFbv///////////////////////5mHZlRDMiIhERERABERIjNF3///////////////////////2Yh2VUQz"
"MiIREREQABERIjNF///////////////////////pmHdlVEMzIiEREREQABERIiNE3////////////////////+mYh2ZVRDMyIhEREREAABERIiM0Vd//////"
"////////////6ZiHdlVEQzIiIRERERAAABEREiM0RWZ97/////////////6ZiId2ZVRDMyIiEREREQAAABEREiIzRFZneI3e7////+7dmZiId3ZlVEQzMiIh"
"EREREAAAABERESIzNEVWZ3iIiJmZmZmIiId3ZmVURDMyIiIRERERAAAAABERESIjM0RVVmZ3d3iIiId3d3ZmVVREMzMiIhERERERAAAAAAERERIiMzREVVZm"
"ZmZnd2ZmZmVVVEQzMyIiIREREREAAAAAAAEREREiIjM0REVVVVVlZVVVVVRERDMzIiIiERERERAAAAAAAAARERESIiMzM0REREVVVVREREQzMzMiIiIRERER"
"EQAAAAAAAAARERERIiIjMzMzREREREREMzMzMyIiIhEREREREAAAAAAAAAABERERESIiIiMzMzMzMzMzMzMiIiIiERERERERAAAAAAAAAAAAEREREREiIiIi"
"IiMzMzMiIiIiIiEREREREREAAAAAAAAAAAAAAREREREREiIiIiIiIiIiIiIiIRERERERERAAAAAAAAAAAAAAABERERERERESIiIiIiIiIiEREREREREREAAA"
"AAAAAAAAAAAAAAABERERERERERIiIiIiIiIiIiEREREREREQAAAAAAAAABERERERERIiIiIiIiIiIiIiIiIREREREREAAAAAAAABERERERESIiIiIjMzMzMz"
"MyIiIiIhEREREREAAAAAABERERERIiIiIjMzMzMzMzMzMzMiIiIhERERERAAAAABERERERIiIiMzMzRERERERERDMzMyIiIhEREREQAAABERERESIiIzMzRE"
"REVVVVVVVUREQzMyIiIRERERAAABERERESIiIzNERFVVVmZmZmZlVVVEQzMyIiEREREQAAERERESIiMzREVVVmZnd3d3d3ZmZVVUQzMiIhEREREAEREREiIj"
"M0RFVWZnd3iIiIiIiHd2ZlVUQzMiIREREQAREREiIjM0RVZmd3iImZmZmZmZmIh3ZlVEMzIiERERARERESIjNERVZneImd3v//////7tmYh3ZlVDMyIhEREB"
"ERESIjNEVWZ3iJ7/////////////6Yh2ZVRDMiIRERERESIjNEVWd4jf////////////////6Yd2VUMyIiERERERIjNEVWd43///////////////////6Idl"
"RDMiIRERERIiM0VWd9/////////////////////+h3ZUQzIhEREREiM0RWd+///////////////////////4dmVDMiIREREiIzRWbf//////////////////"
"//////6HZUQzIhERESIjRFbf/////////////////////////9h2VDMiIRERIjNFXf//////////////////////////+HZUQyIhEREiM0Xf////////////"
"///////////////9h2VDMiERESIzRf////////////////////////////+HZUMyIRERIjNF7//////////tyqqqre///////////4dlQzIhEREiI0Rd////"
"////2qqqqqqqrP//////////l2VEMiIRESIjRFZv/////+qqqqqqqqqqr//////////YZUQyIhEREiM0VWfv///9mZmZmZmqqqqt/////////9hlRDIiERES"
"IjRFVn3//4iIiIiIiZmqqqr/////////2GVEMiIRERIiM0RVZn/3d3d3d3eIiZqqqv////////+YZUQyIhERESIjNEVVZmZ3ZmZmd3eImqqq/////////5dl"
"RDIiAREREiIzREVVVmZmZmZmd4iaqqr/////////h2VDMiEBERESIiMzREVVVVVVVmZ3iJqqqv////////6HZUMyIQEREREiIjMzRERFVVVVZneImqqu////"
"////+YZlQzIhABERERIiIzMzREREVVVmd4iaqt/////////odlRDIiEAARERESIiIzMzRERFVWZ3iaqq/////////5h2VDMiIQABEREREiIiIzMzREVVZ3iJ"
"qq/////////9h2VEMyIRAAAREREREiIiMzNERVZniJqt/////////+h3ZUMyIhEAAAERERESIiIjM0RFVneJqu/////////+mHZUQzIiEQAAAREREREiIiMz"
"RFVmeJmv/////////+mHZVQzIiERAAAAERERESIiIzNEVmeJnf/////////+mHZlRDMiIREAAAAREREREiIjNEVWd4nv/////////9mHZlRDMiIREQAAAAER"
"ERESIiM0RWZ4nv/////////5mHZlRDMiIRERAAAAARERERIiIzRVZ4n//////////pmHZlRDMiIhEREAAAAAEREREiIzRFZ3j//////////ZiHZlRDMyIhER"
"EQAAAAARERESIjNEVnj/////////6ph3ZVRDMyIhERERAAAAABERERIiM0VWfv////////6Zh3ZVRDMyIhEREREAAAAAEREREiM0RWff////////6Zh3ZVRD"
"MyIhEREREAAAAAAREREiIzRFZ/////////6Zh2ZVRDMyIhEREREAAAAAARERESIjNEVu////////+Yh2ZVRDMiIiEREREAAAAAABERERIiM0RW////////+Y"
"h2ZURDMiIiEREREQAAAAAAEREREiIzRVb////////oh2ZURDMiIiEREREAAAAAAAARERESIjNFXf///////9d2ZVRDMiIiEREREQAAAAAAABERESIjNEVWd4"
"iZmZmId2ZURDMiIiEREREQAAAAAAABERERIiM0RVZ3iImYiId2ZVRDMyIiEREREQAAAAAAAAERERIiIzRFZneIiIiId2ZVRDMyIiEREREQAAAAAAAAAREREi"
"IzRFVmd4iIiId2ZVRDMyIiEREREQAAAAAAAAARERESIjNEVmd4iIiIh3ZlREMyIiEREREQAAAAAAAAABERESIjNEVWd4iZmYiHdlVEMzIiIRERERAAAAAAAA"
"AAERERIiM0VWd43u7tmId2VUQzIiIRERERAAAAAAAAAAAREREiM0RWZ9/////+h3ZVRDMiIhERERAAAAAAAAAAABEREiIzRVZ+///////4dlVEMyIhEREREA"
"AAAAAAAAAAERESIjNFZ+////////92ZUQzIiEREREAAAAAAAAAAAERERIiNEVn/////////9dlRDIiEREREAAAAAAAAAAAAREREiM0VW7/////////92VEMi"
"IREREQAAAAAAAAAAABERESIzRWf//////////3ZUMyIhEREQAAAAAAAAAAAAERERIjNFZ///////////dlQzIiERERAAAAAAAAAAAAAREREiM0Vn////////"
"//92VDMiEREREAAAAAAAAAAAAAERESIzRVb//////////2VUMyIREREAAAAAAAAAAAAAARERIiNEVv/////////9ZUQyIhEREQAAAAAAAAAAAAABEREiIzRW"
"3/////////ZlQzIiERERAAAAAAAAAAAAAAERERIjNFVv////////dlRDMiERERAAAAAAAAAAAAAAAREREiI0RWb///////xlVDMiIREREAAAAAAAAAAAAAAB"
"ERESIjNEVm7////+dlVEMyIREREQAAAAAAAAAAAAAAAREREiIzRFVm3e3XZlVEMyIhEREQAAAAAAAAAAAAAAABERESIjM0RVVmZmZVVEMyIhERERAAAAAAAA"
"AAAAAAAAEREREiIzNERVVVVVRDMyIiERERAAAAAAAAAAAAAAAAABERERIiIzNERERERDMyIiEREREAAAAAAAAAAAAAAAAAARERESIiMzMzMzMzMiIiEREREA"
"AAAAAAAAAAAAAAAAABEREREiIiIzMzMzIiIhEREREAAAAAAAAAAAAAAAAAAAAREREREiIiIiIiIiIhEREREQAAAAAAAAAAAAAAAAAAAAEREREREiIiIiIiER"
"EREREQAAAAAAAAAAAAAAAAAAAAABEREREREREREREREREREQAAAAAAAAAAAAAAAAAAAAAAAREREREREREREREREREQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
"AAAAAAAAAAERERERERERESIREREREREREAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABERERERESIiIiIiIiIREREREQAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
"AAAAARERERESIiIiIiIiIiIiERERERAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAERERERIiIiMzMzMzMyIiIhEREREAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB"
"EREREiIiMzMzRERDMzMyIiEREREQAAAAAAAAAAAAAAAAAAAAAAAAAAAAARERERIiIzM0REREREREMzMiIhERERAAAAAAAAAAAAAAAAAAAAAAAAAAAAERERES"
"IiMzREVVVVVVVURDMyIiEREREAAAAAAAAAAAAAAAAAAAAAAAAAABEREREiIjNERVZmZmZmZlVEQzIiEREREAAAAAAAAAAAAAAAAAAAAAAAAAARERERIiIzRF"
"VmZ3d4d3d2ZVRDMiIREREQAAAAAAAAAAAAAAAAAAAAAAAAARERESIiM0RVZ3iIiZmYh3dlVEMyIhEREQAAAAAAAAAAAAAAAAAAAAAAAAEREREiIjNEVmd///"
"///////WVUMyIhERERAAAAAAAAAAAAAAAAAAAAAAABERERIiIzRFZnjf/////////nZVQzIiERERAAAAAAAAAAAAAAAAAAAAAAARERERIiM0RWZ4n///////"
"///4dlRDIiEREREAAAAAAAAAAAAAAAAAAAAAARERESIjM0VWeJ7//////////4dlVDMiIREREAAAAAAAAAAAAAAAAAAAAAEREREiIjNFVnid///////////9"
"h2VEMiIREREQAAAAAAAAAAAAAAAAAAABERERIiIzRFZ4mv///////////+h2VUMyIhEREQAAAAAAAAAAAAAAAAAAABERERIiM0RWd4nv////////////mHZU"
"MyIhEREQAAAAAAAAAAAAAAAAAAARERESIjM0VWeJ3/////////////mHZUQzIiERERAAAAAAAAAAAAAAAAAAEREREiIjNEVnia//////////////2YdlQzIi"
"ERERAAAAAAAAAAAAAAAAAAEREREiIzRFZ4mu//////////////6YdlQzIiERERAAAAAAAAAAAAAAAAABERERIiIzRVZ4nf//////////////+odlRDMiIRER"
"AAAAAAAAAAAAAAAAABERESIiM0RWeJr///////////////+phmVDMiIREREAAAAAAAAAAAAAAAARERESIjNEVnia7////////////////Zh2VDMiIREREAAA"
"AAAAAAAAAAAAEREREiIjNFVnic////////7////////ph2VEMiIREREAAAAAAAAAAAAAAAERERIiIzRFZ4mv////////rv///////6l2VEMyIhEREQAAAAAA"
"AAAAAAABERERIiM0RWd4nv///////9rf///////6mHZUMyIhEREQAAAAAAAAAAAAARERESIjM0VWeJr////////6rf///////8mHZUMyIhEREQAAAAAAAAAA"
"AAAREREiIjNEVnia/////////Kqv///////9qHZUQzIhEREQAAAAAAAAAAAAEREREiIzRFZnid////////6qqv///////+qXZVQzIiERERAAAAAAAAAAABER"
"ERIiMzRVZ4mv////////qqqu////////qYdlQzIiERERAAAAAAAAAAABERESIiM0RWeJrv///////9qqqt////////yYdlQzIiERERAAAAAAAAAAARERESIj"
"NEVmeJ3////////6qqqq////////2odlRDMiIRERAAAAAAAAAAEREREiIzNFVnia////////+qqqqq////////6pdlVDMiIREREAAAAAAAAAERERIiIzRFZ4"
"mu////////6qqqqq7///////+ph2VDMiIREREAAAAAAAABERERIiM0RWZ4nf////////qqqqqq7///////+ph2VEMiIREREAAAAAAAARERESIjM0VWeJr///"
"/////9qqqqqq3////////ah2VEMyIhEREAAAAAAAAREREiIjNEVnia7////////qqqqqqqr////////qmGVUMyIhEREQAAAAAAEREREiIzRFZ3it////////"
"+qqqqqqqr////////6mHZUMyIhEREQAAAAAAERERIiIzRVZ4mv////////6qqqqqqqrv///////6mHZUQyIhEREQAAAAABERESIiM0RWeJrv////////qqqq"
"qqqqrf///////9qHZUQzIiEREQAAAAARERESIjNEVneKz////////9qqqqqqqqqv///////+qYZVQzIiEREQAAAAAREREiIjNFVnia/////////qqqqqqqqq"
"qv////////qYdlQzIiEREQAAAAERERIiIzRFZ4mu////////+qqqqqqqqqqv////////qYdlRDIiEREQAAABERERIiM0RWZ4mv////////2qqqqqqqqqqu//"
"//////2odlRDMiIREQAAABERESIiM0RWeJr/////////qqqqqqqqqqqt////////6phlVDMiIREQAAARERESIjNEVnia3////////9qqqqqqqqqqqq//////"
"//+ph2VDMiIREQAAAREREiIzNFVnia/////////93d3d3d3d3d3d////////+ph2VEMiIREQAAEREREiIzRFZ4mu///////////////////////////////a"
"h2VEMyIREQAAERERIiM0RWZ4rf///////////////////////////////ql2VUMyIhEQABERERIiM0VWeJr////////////////////////////////6mHZU"
"MyIhEQARERESIjNEVnia7////////////////////////////////6mHZUMyIhEQARERESIjNFVnid/////////////////////////////////9mHZUQyIh"
"EQAREREiIzRFZ4mv/////////////////////////////////+qHZUQzIhEQEREREiIzRVZ4nv//////////////////////////////////qYZVQzIiEQER"
"ERIiM0RWeJ3///////////////////////////////////qYdlQzIiERERERIiM0VmeJ////////////////////////////////////2YdlQzIiERERERIj"
"NEVnie////////////////////////////////////6YdlQzIiERERESIjNFZnjP////////+qqqqqqqqqqqqqqqqqqu////////+YdlQzIiERERESIjRFZ4"
"n/////////2qqqqqqZmZmZmqqqqqqt////////+odlRDIiEREREiIzRWZ47////////+qqqpmZiIiIiIiJmZqqqq/////////YdlRDIiEREREiI0RWeJ////"
"/////6qpmYiHd3d3d3d3iImaqq/////////pdlRDIiEREREiM0RWeP/////////qqYh3d2ZmZmZmZmd3iJmq7////////5dlRDIiEREREiM0Vmff////////"
"+qmId2ZmVVVVVVVWZmd4ma3////////5dlRDIiEREREiM0Vnj/////////2Zh3ZlVVVEREREVVVWZ3iZr////////9dlRDIiERERIiM0Vn7////////+mYdm"
"VVRERERERERERVVmeJn////////+dlRDIiERERIiNEVt/////////5iHZlVERDMzMzMzM0REVWZ4nv////////dlRDIiERERIiM0Vv/////////Yh2ZVREMz"
"MzMzMzMzNERVZnjv////////dlQzIiERERIiM0Xv////////+HdlVEQzMzIiIiIiIzMzRFVmff////////1VQzIhEREREiM03/////////x3ZVREMzIiIiIi"
"IiIiMzNEVWZ/////////5UQyIhEREREiM0/////////+dmVUQzMyIiIiIiIiIiIjM0RVZv////////9UMyIhEREREiIzRFVmd3d3d2ZVVEMzIiIiIiIiIiIi"
"IiMzREVmZ3d3d3ZlVEMyIREREREiIzNEVWZmZmZlVURDMyIiIiEREREREiIiIzNERVVmZmZmVVRDMiIRERERESIjM0RFVVVVVVREMzIiIiERERERERERIiIi"
"MzREVVVVVVVEQzIiEREQERERIiMzREREREREQzMyIiIRERERERERERESIiIzM0RERERERDMyIiEREQEREREiIjMzNEREMzMzIiIhEREREREREREREREiIiMz"
"MzREQzMzIiIhEREQARERESIiIzMzMzMzIiIiERERERERERERERERERIiIiMzMzMzMiIiEREREQARERERIiIiIiIiIiIiIRERERERERERERERERERESIiIiIi"
"IiIiIhEREREAABERERERIiIiIiIiIhEREREREREAAAAAEREREREREiIiIiIiIhERERERAAAAERERERERESIiEREREREREREAAAAAAAABERERERERERERERER"
"EREREQAAAAAAAAAAARERERERIiIiIiIiIiIiIiIiIiIiIiIiIREREREREAAAAAAAAAARERERESIiIiIiIjMzMzMzMzMzIiIiIiIiIiERERERAAAAAAAAARER"
"ERIiIiIjMzMzMzMzMzMzMzMzMzMzMiIiIiEREREQAAAAAAARERERIiIjMzMzNEREREREREREREQzMzMzMzIiIhEREREAAAAAARERERIiIzM0RERERERERVVE"
"REREREREREQzMzIiIhEREQAAAAARERESIiMzNERVVVVVVVVVVVVVVVVVVVVVRERDMzIiIRERAAAAABERERIiMzREVVZmZmZmZmZmZmZmZmZmZmVVVURDMyIh"
"EREAAAABERERIiM0RFVmZ3d3d3d3d3d3d3d3d3d3dmZmVURDMiIREQAAAAERERIiM0RVZnd4iIiJmZmZmZmZiIiIiIiId3ZmVUQzIhERAAAAERERIiM0RVZ3"
"iImZmqqqqqqqqqqqqZmZmZmIh3ZlVDMiIREAAAAREREiIzRVbf/////////////////////////////kQzIhEQAAAREREiIzRFZ+////////////////////"
"/////////+VDMiERAAABERESIjNFZ3//////////////////////////////1UQyIhEAABERERIjNFVnj/////////////////////////////9lRDIiEQAA"
"ERERIiM0Vnjf/////////////////////////////2VUMiIRAAAREREiM0RWeN/////////////////////////////+ZUQyIhEAAREREiIzRWeJ7///////"
"//////////////////////5lRDIiEQABERESIjNFZ4n//////////////////////////////WVEMiERAAERESIjNEVniv//////////////////////////"
"///2ZUMyIREAERERIiM0Vnid//////////////////////////////ZUQzIhEQAREREiIzRWeJ3/////////////////////////////5VQzIiERABERESIj"
"RFZ4rv////////qqqqqqqqqqqqqqqZmYh3ZlRDMiEREAERESIjNFVomv////////6qqqqqqqqqqpmZmYiId3ZVRDMiIREQERERIiM0Vnia/////////aqqqq"
"qqmZmZiIiIh3dmZVRDMyIhERAREREiIzRWeJ3////////9qqqqqZmZiIiId3d2ZmVVRDMyIhEREBERESIjRFZ4rf////////qqqqqZmIiIh3d3ZmZlVURDMy"
"IhEREQERESIjNFVnmu////////+qqqqZiIiHd3d2ZmZVVURDMyIiERERERERIiM0Vnia/////////qqqqpmIiHd3d2ZmZVVURDMyIiEREREREREiIzRWeJr/"
"///////9qqqqmYiIh3d3ZmZlVURDMzIiIRERERERESIjRFZ4nf////////2qqqqZmIiId3d2ZmVVREMzIiIRERERERERIjNEVnmt////////+qqqqqqZmYiI"
"h3d2ZlVEQzMiIRERERARERIiM0VWia7////////6qqqqqqqZmZiIh3dmZVRDMyIhEREREBEREiIzRWeJr////////+qqqqqqqqqqmZmIh3ZlVEMyIiEREREA"
"ERESIjNFZ4mv////////2qqqqqqqqqqqqZmId2ZVQzIiIREREQARERIjNEVnit///////////////////////////9VEMiIREREQABEREiM0RWea3///////"
"////////////////////ZUQzIhERERAAEREiIzRVeJrv//////////////////////////9lRDMiEREREAARESIjNFZ4mv//////////////////////////"
"/mVEMyIREREAABERIiM0Vnia///////////////////////////+ZUQyIhEREQAAEREiM0RWeJ3///////////////////////////1lRDIiERERAAARESIz"
"RFZ5rf//////////////////////////92VEMiIREREAABESIjNFV4mu///////////////////////////3ZUMyIhEREAAAERIiM0Vnia//////////////"
"/////////////+ZVQzIhEREQAAAREiIzRWeJr///////////////////////////5lRDMiERERAAABESIzRFZ4nf/////////u7u7u7u7u7u7u7u7u7VVDMi"
"IREREAAAERIjNEVnmt////////+qqqqqqqqqqqqpmZiHdlVEMyIhEREQAAARIiM0VWia7////////6qqqqqqqqqZmZmIh3dmVEMzIiERERAAABEiIzRWeJr/"
"///////+qqqqqqmZmZiIiHd3ZlVUQzIiEREREAAAESIjNFZ4mv////////2qqqqpmYiIiHd3d2ZlVUQzMiIREREQAAARIiNEVnid/////////aqqqpmIiId3"
"d3ZmZVVEQzMiIhERERAAABEiI0RWeK3////////6qqqpmIiHd3d2ZmZVVURDMyIiEREREAAAESIzRFZ5rv////////qqqpmIiHd3d2ZmZVVUREMzIiIREREQ"
"AAARIjNFZomv////////6qqqmYiId3d3ZmZlVVVEQzMiIhERERAAABIiM0Vnia/////////aqqqZmIiHd3d2ZmZlVURDMyIiEREREAAAEiIzRWeJ3///////"
"/9qqqqmZiIiId3d3ZmZVVEQzMiIREREQAAASIjNFZ4nf////////qqqqqqmZmIiIiHd3dmZVREMyIhERERAAABIiNEVniu////////+qqqqqqqmZmZmIiIh3"
"dmVUQzMiIREREAAAEiI0RWeK/////////qqqqqqqqqqqqZmZmIh3ZlVEMyIhEREQAAASIjRFZ5r////////+qqqqqqqqqqqqqqqZmYh3ZVQzIiERERAAABIi"
"NEVonf//////////////////////////////VEMyIREREAAAEiI0RWid//////////////////////////////9VQzIhEREQAAASIjRVaJ7/////////////"
"/////////////////mVDMiERERAAABIiNFVon//////////////////////////////+ZUMyIREREAAAEiI0RWef//////////////////////////////1l"
"QzIhEREQAAASIjRFZ9//////////////////////////////92VDMiERERAAABEiM0Vn3//////////////////////////////2ZUMyIRERAAAAESIzRWfv"
"/////////////////////////////+ZVQzIhEREAAAARIjNFVv//////////////////////////////5lRDIiEREQAAABEiI0RW////////////////////"
"///////////VVDMiERERAAAAESIjNF3//////////////////////////////2VEMyIREREAAAAREiMzRVZ3iImZmaqqqqqqqqqpmZmZmZmIh3dlVEMyIhER"
"EAAAABESIjNEVWZ3eIiIiIiJmZmIiIiIiIiIh3d2ZlVEMyIhEREQAAAAEREiIzRFVWZmd3d3d3d3d3d3d3d3d3dmZmZVVEMyIiERERAAAAARERIiMzRFVVVm"
"ZmZmZmZmZmZmZmZmZmVVVURDMyIiERERAAAAABEREiIjM0RERVVVVVVVVVVVVVVVVVVVVUREQzMyIiEREREAAAAAERERIiIjMzNERERERERERERERERERERE"
"RDMzMiIiEREREAAAAAARERERIiIjMzMzMzRERERERERERDMzMzMzMzIiIiEREREAAAAAAAERERESIiIiIzMzMzMzMzMzMzMzMzMzMyIiIiIhEREREQAAAAAA"
"ABERERERIiIiIiIiIiMzMzMzMiIiIiIiIiIiIREREREQAAAAAAAAAAAAAAABEREREREREREREREREREAAAAAAAAAARERERERESIiIhEREREREQAAAAAAAAER"
"EREREiIiIiIiIiERERERAAAAAAABERERESIiIiIiIiIiIhEREREAAAAAARERERIiIjMzMzMzMiIiEREREAAAAAERERESIiMzM0RERDMzIiIREREAAAAAERER"
"EiIjM0REVVRERDMyIhEREAAAABERERIiMzREVVVVVVVEMzIiEREAAAABERESIjM0RVZmZmZmVURDMiIREAAAARERESIjNEVmZ3d3d3ZlVDMiIREAAAAREREi"
"IzRFbf////////xEMyIREAAAEREREiM0RWbv////////ZUMyIhEAAAERERIiM0VWf/////////ZUQzIhEAABERERIjNEVnj////////+ZlQzIhEAABERESIj"
"NFZn3////////9dlQzIhEAABERESIjRFZ43////////8dlQzIiEAAREREiIzRVZ47////////4dlRDIiEAAREREiIzRWeJ/////////4dlRDIiEAAREREiM0"
"RWeJ/////////odlRDIiEAERERIiM0VWeN/////////YdlRDIiEAERERIiM0Vnid/////////IdlRDIiEAERERIiNEVnie////////+YdlQzIiEAERESIjNE"
"Vnif////////+YdlQzIiEBERESIjNFZomv////////6YdlQzIiEBERESIjNFZ4nf////////2YdlQzIiEBERESIjRFZ4nf////////yYdlQzIiEBEREiIzRF"
"Z4nv////////qXZVQzIiEREREiIzRVZ5r/////////qHZUQzIhEREREiIzRWeJr////////+mHZUQzIhEREREiIzRWeJ3////////9mHZUQyIhEREREiM0RW"
"eJ3////////8mHZUMyIhERERIiM0RWeK7////////6mHZUMyIhERERIiM0Vmia/////////6mGVUMyIhERERIiM0Vnia/////////qh2VEMyIhERERIiM0Vn"
"id/////////Zh2VEMyIRERERIjNEVnid/////////Jh2VDMiIRERESIjNEVniu////////+ph2VDMiIRERESIjNFZomv////////+phlVDMiIRERESIjNFZ4"
"mv////////6odlRDMiIRERESIjNFZ4nf////////2YdlRDMiERERESIzRFZ4nf////////yYdlQzIiEREREiIzRFZ4rv////////qYdlQzIiEREREiIzRWaJ"
"r/////////qYZVQzIiEREREiIzRWeJr////////+qHZUQzIiEREREiIzRWeJ3////////9mHZUQzIhEREREiM0RWeJ3////////8mHZUMyIhERERIiM0RWeK"
"7////////6mHZUMyIhERERIiM0VWia/////////6mGVUMyIhERERIiM0Vnia/////////qh2VEMyIhERERIiM0Vnid/////////Zh2VEMyIRERERIiNEVnid"
"/////////Jh2VDMiIRERERIjNEVnie////////+ph2VDMiIRERERIjNEVniv////////+pdlVDMiIRERASIjNFVomv////////6YdlRDMiIRERASIjNFZ4nf"
"////////2YdlRDIiERERASIjNFZ4nf////////yYdlQzIiERERASIjNFZ4nv////////qXZVQzIiERERASIjNFZ4n/////////mHZUQzIhEREQASIjRFZ4n/"
"///////+mHZUQyIhEREQASIjRFZ43////////9mHZUMyIhEREQASIjRFZ43////////8h2VUMyIhEREQASIjRFZ47////////5h2VEMyIREREAASIjRFZ4//"
"///////5h2VDMiIREREAASIjRFZ4/////////odlVDMiIREREAASIjNFZ9/////////YdlRDIiERERAAARIjNFZ9/////////HZVQzIiERERAAARIjNFVu//"
"//////+HZUQzIhERERAAARIiNEVv////////92VUMyIhEREQAAARIiM0Vf////////5lVEMyIREREQAAAREiM0Tf////////1lRDMiIREREAAAAREiIzRFZm"
"d3d3dmVUQzIiEREREAAAARESIzNEVWZmZmZVVEMyIiERERAAAAARESIjM0RFVVVVVURDMyIhERERAAAAARERIiMzNEREREREMzIiIREREQAAAAAREREiIjMz"
"M0RDMzMyIiEREREQAAAAARERESIiIzMzMzMyIiIhEREREAAAAAARERERIiIiIiIiIiIiERERERAAAAAAABERERERIiIiIiIiIREREREQAAAAAAAAERERERER"
"EREREREREREREAAAAAAAAAAAAAAAAAEREREREREREREREREREREREREREREREREREREREREREREQAAAAAAAAAAABERERERERIiIiIRERERERERERERERERES"
"IiIiIiIRERERERAAAAAAAAAAARERERESIiIiIiIiIREREREREREREREiIiIiIiIiIiIREREREAAAAAAAAAERERERIiIiIiIiIiIiIREREREREREiIiIiIjMz"
"MiIiIiEREREQAAAAAAABEREREiIiMzMzMzMzIiIiIRERERESIiIiIzMzMzMzMyIiIREREQAAAAAAARERERIiIzMzREREMzMyIiIiIhEiIiIiIzMzREREREMz"
"MyIiERERAAAAAAARERESIiMzRERVVUREMzMiIiIiIiIiIjMzRERVVVVEREMzIiERERAAAAAAEREREiIzNERVVWZVVURDMzIiIiIiIjMzNERVVWZmVVVUQzMi"
"IRERAAAAAAERERIiMzRFVmZnZmZlVEQzMiIiIiMzM0RFVmZmd2ZmZVRDMyIhEREAAAABERERIiM0RWZ3d4d3dmVURDMzIiIzMzREVWZnd3d3d3ZlVEMyIhER"
"EAAAABERESIjNEVt////////9lREMzMzMzNERVZnd+//////////8yIhEREAAAARERESIzRFZu////////9lVEQzMzMzREVWZ3jv//////////QzIhEREAAA"
"AREREiIzRVZ/////////52VURDMzRERVZneI7//////////kQzIhEREAAAEREREiM0RWeP////////52ZVRERERFVWZ4if//////////5UQzIhEREAAAERER"
"IiM0Vmff////////2HZVRERERVZneJn//////////+ZUQyIhEREAAAERERIiNEVnjf////////mHZlVURVVWZ4id///////////WVUMyIhEREAABERESIjNF"
"Vnjv////////mHdmVVVVZneInf//////////12VEMyIhEREAABERESIjNFZ4n////////+qYdmZVVmZ3iZ3//////////4dmVEMiIRERAAABERESIzRFZ4n/"
"///////+qYd2ZmZneImu//////////+HdlRDMiIRERAAAREREiIzRVaJ3////////9qYh3dnd4iZrv/////////+iHZVQzIiERERAAAREREiIzRWeJ3/////"
"///6qZh3d3eImq7//////////ph2VUQzIiERERAAAREREiI0RWeJ7////////6qpiIiIiZqv//////////6YdmVEMyIhEREQAAARERIiM0RWea/////////q"
"qpmIiJmqr//////////9mHZlRDMiIREREQAAERERIiM0Vnia/////////qqqmZmaqt///////////Zh2ZUQzIiIREREQAAERERIiM0Vnid/////////aqqqq"
"qqrf//////////mYdmVEMyIiEREREAAAERERIjNEVnit////////+qqqqqqq7//////////6mHZlRDMyIhERERAAAAERESIjNEVnmu////////+qqqqqqu//"
"////////6ph2ZUQzMiIRERERAAABERESIjNFZ4mv////////6qqqqqrv/////////+qYdmVUMzIiEREREQAAABERESIjNFZ4mv////////6qqqqq////////"
"///qmHZlVDMyIiEREREQAAABERESIzRFZ4nf////////2qqqqv//////////2ph3ZVRDMiIhEREREAAAABERESIzRFZ5rf////////qqqqz//////////9qY"
"d2VUQzIiIRERERAAAAABEREiIzRVeJrv////////qqqt///////////amHdlVEMzIiIREREQAAAAABEREiIzRWeJr////////+qqrf//////////qpiHZVRD"
"MyIiEREREQAAAAABEREiIzRWeJr////////+qq7//////////6qZh2ZUQzMiIhEREREAAAAAABEREiM0RWeK3////////9qu//////////6qqYdmVEQzIiIR"
"ERERAAAAAAABERIiM0VWea3////////6rv/////////+qqmHdlVEMzIiIREREQAAAAAAABERIiM0Vnia7////////6///////////qqqmHZlRDMyIiEREREA"
"AAAAAAABERIiM0Vnia////////////////////6qqqmHZVRDMyIiEREREAAAAAAAABERIiNEVnia////////////////////6qqqiHZVRDMiIhERERAAAAAA"
"AAABERIjNEVnit/////////////////////aqpiHZUQzMiIhERERAAAAAAAAABESIjNFVnmt/////////////////////6qql3ZUQzMiIhERERAAAAAAAAAB"
"ESIjNFZ4mu/////////////////////+qqmHZVRDMiIhEREQAAAAAAAAABESIjNFZ4mv//////////////////////2qmHZVRDMiIhEREQAAAAAAAAABESIj"
"NFZ4mv//////////////////////+qmHZlRDMiIhEREQAAAAAAAAABESIzRFZ4rf///////////////////////qqYdlRDMyIhERERAAAAAAAAABESIzRFZ5"
"rf///////////////////////9qYdlVDMyIhERERAAAAAAAAABEiIzRVaJrv/////////////v//////////qph2VEMyIiERERAAAAAAAAABEiIzRWeJr///"
"//////////6q//////////6ph2VUMzIiERERAAAAAAAAABEiIzRWeJr////////////+qq3//////////KmHZUQzIiEREREAAAAAAAABEiIzRWeJ3///////"
"/////aqqrv/////////6mHZVQzIiIREREAAAAAAAABEiI0RWeK3///////////2qqqqv/////////+qYdlRDMiIREREAAAAAAAABEiM0RWea7//////////6"
"qqqqqt//////////qYdlVDMiIhEREQAAAAAAABEiM0VWia//////////+qqqqqqq//////////+ph2VEMyIhEREQAAAAAAABIiM0Vnia/////////+qqqqqq"
"qqr//////////Zh2VUMyIhERERAAAAAAABIiM0Vnid/////////6qqqZmZmqrf/////////6mHZUQzIiERERAAAAAAABIiM0Vnid////////+qqqmYiImaqv"
"/////////+mHZVQzIiERERAAAAAAABIiM0Vnie////////+qqpiId4iJqq//////////2YdlRDMiERERAAAAAAABIiM0Vnif////////6qqYh3d3eImq7///"
"//////+YdlVDMiIREREAAAAAABIiNEVniv////////6qmId2ZmZ3iar//////////ph2VEMiIREREAAAAAABIiNEVnjf////////2piHZmVWZneJqv//////"
"///9h2VUMyIREREAAAAAABIiNEVnjf////////qph2ZVVVVmd4mu//////////mHZUMyIhEREAAAAAABIiNEVnjv////////qYd2VVVVVWZ3ia//////////"
"6HZUQyIhEREAAAAAABIiNEVnj////////+mIdlVURERVVneJr//////////HZVQzIhEREAAAAAABIiNEVnj////////+mHZlREREREVWd4nv/////////3Zl"
"QzIhEREAAAAAABIiM0Vn3////////9h3ZVREMzRERVZ3if/////////+dlQzIhEREAAAAAABEiM0Vn3////////5h2VUQzMzM0RFVniM//////////dlQzIh"
"EREAAAAAABEiM0VW7////////4dlVEMzMzMzNEVWd47/////////9lQzIhEREAAAAAABEiM0RW/////////nZlRDMzMzMzM0RVZ3j//////////VQzIhEREA"
"AAAAABEiIzRV/////////mZVQzMyIiIjMzRFVnff/////////0QzIhEREAAAAAABESIzRN/////////WVUMzMiIiIiIzNEVWZ+/////////+QyIhEREAAAAA"
"ABESIjNEVmZ3d3d2ZVREMyIiIiIiIjM0RFVmd3d3d3ZlVEMyIhEREAAAAAABERIjM0RVZmZmZlVUQzMiIiIiIiIiIzNEVVZmZmZmZVRDMiIREREAAAAAABER"
"IiMzREVVVVVVREMzIiIiEREiIiIjM0RFVVVWVVVEQzMiIREREAAAAAABEREiIzM0REREREQzMyIiIREREREiIiIzNEREVVVEREMzIiERERAAAAAAABERESIi"
"MzMzREMzMzIiIhERERERERIiIjMzM0RERDMzMiIhERERAAAAAAABERERIiIjMzMzMzIiIiERERERERERESIiIjMzMzMzMyIiIREREQAAAAAAABEREREiIiIi"
"IiIiIiIREREREREREREREiIiIiIiIiIiIiEREREQAAAAAAAAEREREREiIiIiIiIRERERERERERERERERIiIiIiIiIiIREREREAAAAAAAAAARERERERERERER"
"EREREREREREREREREREREiIiIhERERERERAAAAAAAAAAAAAAAAAAAREREREREREREREREREREREAAAAREREREREREREREREREREQAAAAAAAAABEREREREiIi"
"IiIhERERERERERERERERERERESIiIhEREREREQAAAAAAAAERERERIiIiIiIiIiIRERERERERERERERERIiIiIiIiIhEREREQAAAAAAARERERIiIiIzMzIiIi"
"IhERERERERERERERIiIiIiIiIiIiEREREQAAAAABEREREiIiMzMzMzMzMiIiERERERERERERIiIiMzMzMzMyIiIREREAAAAAERERESIiMzNEREREQzMyIiIR"
"EREREREREiIjMzNEREQzMzIiIRERAAAAABERERIiMzRERVVVVERDMyIiIRERERERESIiMzREREREREMzIiIREQAAAAEREREiIzNEVVZmZmVVREMzIiIhERER"
"ESIiIzREVVVVVVVEQzIiEREAAAABERESIjNEVVZnd3d2ZlVEMzIiIhERERIiIjNEVVZmZmZlVUQzIiERAAAAERERIiM0RVZneIiIh3dmVEMzIiIiIiIiIiM0"
"RVZnd3d3dmVUQzIhEQAAABERESIjNFVt///////9dmVUMzIiIiIiIiIzRFVv////////5UMyIhEAAAERERIiM0RWfv///////4d2VUMzIiIiIiIjNEVWf///"
"/////9VEMyIRAAABERESIjRFZ3/////////od2VEMzIiIiIiMzRFZ9////////9lVDMiEQAAEREREiM0VWeP////////+Yd2VEMzMiIiIzNEVmfv////////"
"dlQzIiEAABERESIjNFZ43/////////6YdmVEMzMiIjM0RVZ4/////////nZUQyIhAAAREREiM0RWeN//////////2YdmVEMzMzMzNEVnif////////52VEMi"
"IQABERESIjNFZ4nv//////////qYdlVEMzMzM0RVZ4n////////9dlVDIiEAAREREiI0RWeJ///////////+qYdlVEQzMzREVniN////////+XZVQzIhAAER"
"ESIjNFVnmv///////////6mHdlVEQzNERVZ4nv////////mGVUMyIQAREREiIzRWeJ3////////////qmHZlVEREREVmeJ/////////phlVDMiEAERERIiNE"
"Vnid/////////////amHZlVERERVZ4mv////////6XZUQyIhABERESIzRFZ4rv////////////+qmHZVVERFVmeJr////////9h2VEMyIQARERIiM0Vnia//"
"////////////2ph3ZVVVVVZ4it////////+odlRDIiEBERESIjNFZ4mv//////////////qph3ZVVVVmeJrv////////mHZUQyIhAREREiIzRWeJ3///////"
"///////+qph2ZlVWZ3ia/////////ph2VDMiIQERERIjNEVnmt///////////////9qYh2ZmZmeJqv////////6YdlQzIiEBEREiIzRVaJrv////////////"
"///6qYh2ZmZ3iar////////9mHZUMyIhERERIiM0Vnia/////////////////aqYd3d3eImt////////+ph2VDMiIRERESIjNFZ4mv////////////////+q"
"qYd3d4iarv////////qXZVQzIiEREREiI0RWeK3/////////////////6qmYh3iJmq/////////qh2VEMyIRERERIjNEVnmt//////////////////yqmYiI"
"iaqv////////6YdlRDMiEREREiIzRVeJrv//////////////////qqmZiZmqr////////9mHZUQyIhERERIiM0Vnia/////////v/////////9qqmZmaqt//"
"//////+ph2VDMiIRERESIjNFZ4mv////////3f/////////6qqqqqqrv////////qYdlQzIiEREREiI0RWeJ3////////8rv/////////qqqqqqq////////"
"/ql2VEMyIhERERIjNEVnit////////+qr/////////+qqqqqqv////////6odlRDMiEREREiIzRVaJrv////////qq7/////////+qqqqqr////////9mHZU"
"QyIhERERIiM0Vnia/////////qqq//////////2qqqqt////////+ph2VDMiIRERESIjNFZ4mv////////2qqt//////////qqqqrv////////qYdlQzIiER"
"EREiI0RWeJ3////////8qqqu/////////+qqqq/////////qmGVUMyIhERERIjNEVnit////////+qqqqv/////////6qqqv////////6odlRDMiIREREiIz"
"RFZ5rv////////qqqqrv/////////qqqr////////9mHZUQyIhERERIiM0Vnia/////////qqqqqr//////////aqt////////+ph2VDMiIRERESIjNFZ4mv"
"////////2qqqqq3/////////+qrv////////qYdlQzIiEREREiI0RWeJ3////////8qqqqqq//////////6q/////////qmHVUMyIhERERIjNEVnid//////"
"//+qqqqqqq//////////qv////////6pdlRDMiIREREiIzRFZ4rv////////qqqpqqqu/////////+r////////9qHZUQyIhERERIiM0VWia/////////qqp"
"mZmqqv//////////////////+ph2VDMiIRERESIjNFZ4mv////////2qqZiJmqrf//////////////////qYdlQzIiEREREiIzRWeJ3////////8qpmIiImq"
"r//////////////////qmHZUMyIhERERIiNEVnid////////+qmYh3iImqz/////////////////6pdlRDMiERERESIzRFZ4nv////////qpiHd3eImq7///"
"/////////////9qHZUQyIhEREREiM0RWeK/////////qqYd2ZneJmq////////////////+ph2VDMiIRERESIjNFVnmv////////2piHZmZneJqt////////"
"////////qYdlQzIiEREREiIzRWeJ3////////8qYd2ZmZneJqv///////////////qmHVUMyIhEREBIiM0Vnid////////+piHZlVVZneJrf////////////"
"//6odlRDMiERERASIjNFZ4nv////////qYdmVVVVZniarv/////////////9mHZUQyIhEREQEiIzRWeJ/////////qmHZlVVVVZniar/////////////+ph2"
"VDMiIREREBIiNEVnif////////2Yd2VURERVZnia3/////////////qYZVQzIiEREQASIjRFZ43////////8mHZVRERERVZ4ia/////////////qh2VEMyIR"
"EREAEiI0RWeN////////+ph2VURERERVZ4mt////////////6YdlQzIiERERABIiNEVnjv////////mHZlREMzNERWZ4mu///////////9mHZUMyIhEREQAS"
"IjRFZ4/////////ph2VUQzMzNERWZ4mv//////////+odlRDMiERERAAEiI0RWeP////////2HZlRDMzMzNEVWd4nf//////////mHZUMyIhEREQABEiM0Vn"
"3////////8h2VEMzMzMzNEVWeJn//////////odlVDMiIREREAARIjNFZ9////////+HZlRDMyIiMzNEVWeJ3/////////6HZUQyIhEREQAAESIzRVbv////"
"////d2VEMzIiIiIzNEVWeJ7////////9dlVDMiIREREAABEiI0RW/////////nZVQzMiIiIiIzNEVWeI////////93ZUQyIhERERAAARIiM0Vf////////1l"
"VEMyIiIiIiIzNEVWeO////////dlVDMiIREREAAAERIjNE3////////8VUQzIiIiIiIiIzNEVWd////////mVUMyIhERERAAABESIjNEVmZ3d3d2ZVRDMiIi"
"IhIiIiIzNEVWZ3eIiId2ZVRDMiIREREAAAARESIzNEVWZmZmZVVEMzIiIRERESIiIjNERVZmd3dmZVVEMyIhERERAAAAEREiIzNERVVVVVVEQzMiIhERERER"
"IiIjM0RFVWZmVVVEQzIiEREREAAAABEREiIzM0REREREQzMiIhEREREREREiIjMzRERVVVREQzMiIRERERAAAAAREREiIjMzM0RDMzMiIiEREREREREREiIi"
"MzNEREREMzMiIhEREREAAAAAEREREiIiMzMzMzMiIiIRERERERERERESIiIzMzMzMzMiIiEREREQAAAAABERERESIiIiIiIiIiIRERERERERERERESIiIiIi"
"MiIiIiIRERERAAAAAAABERERERIiIiIiIiEREREREREREREREREREiIiIiIiIiIREREREAAAAAAAABEREREREREREREREREREREREREREREREREREiIiIRER"
"EREREQAAAAAAAAAAAAAAAAERERERESIiIiIiIiIiIiIiIiIiIiIRERERERERAAAAAAAAAAAAAAEREREREiIiIiIiIzMzMzMzMiIiIiIiIhERERERERAAAAAA"
"AAAAAAERERESIiIiIzMzMzMzMzMzMzMzMzIiIiIiEREREREQAAAAAAAAAAERERESIiIzMzMzRERERERERERDMzMzMyIiIiEREREREAAAAAAAAAERERESIiMz"
"NERERERERFVUREREREREMzMzIiIiERERERAAAAAAAAEREREiIjMzREVVVVVVVVVVVVVVVVVVRERDMzIiIhEREREQAAAAAAAREREiIjM0RFVWZmZmZmZmZmZm"
"ZmZmVVVURDMzIiIhEREREAAAAAARERESIjNERVZmd3d3d3d3h3d3d3d3dmZlVUREMzIiIRERERAAAAABERESIjNEVWZ3eIiImZmZmZmZmIiIiId3ZmVVRDMy"
"IiEREREQAAABERESIjNEVWd4iJmZqqqqqqqqqqqpmZmIh3dmVURDMiIhERERAAAAERERIiM0VW3///////////////////7tmZiHdmVUQzIiIREREAAAERER"
"IiM0RWfv//////////////////////7ZiHdlVEMyIiEREQAAAREREiI0RWd//////////////////////////piHZlRDMiIRERAAARERESIzRVZ4////////"
"///////////////////Yh2ZUQzIiEREAABERESIjNFZ43///////////////////////////+YdmVEMyIhEQAAERERIjNEVnjf//////////////////////"
"//////mHZVQzIiERAAERERIiM0Vnie/////////////////////////////5h2VUMyIhEAAREREiIzRWeJ//////////////////////////////+YdlRDIi"
"EQABERESIzRFZ4r//////////////////////////////+h2VUMyIhABERESIjNFZ4nf///////////////////////////////YdlRDIiEAERERIiM0Vnid"
"////////////////////////////////h2VUMyIQAREREiI0RWeJ7////////6qqqqqqqqrd7////////////IdlQzIhABEREiIzRFZ5r/////////qqqqqq"
"qqqqqq7//////////+h2VEMiIBERESIjNFZ4mv////////6qqqqqqqqqqqqq//////////+YZUQyIgERERIiM0Vnid/////////aqqqZmZmZmqqqqv//////"
"///5h2VDMiAREREiI0RWeJ3////////9qqqZiIiIiZmqqq3/////////yHZUMyIBEREiIzRFZ5rv////////qqqZiId3eIiZqqqv/////////YdlQzIhERES"
"IjNFZ4mv////////+qqZiHd3d3eImaqq7////////9h2VDMiERERIiM0Vnia/////////qqpiHd3Znd3iJqqrv////////2HZUMyIREREiI0RWeJ3///////"
"/9qqmHd2ZmZneImqqt/////////YdlQzIhERESIzRFZ4rf////////2qmId2ZmZmd3iaqq3////////9h2VDMiERESIjNFVnmu////////+qqYh3ZmZmZ3eJ"
"qqrv////////2HZUMyIRERIiM0Vnia/////////6qpiHdmZmZneImqqu////////+YdlQzIhEREiIzRWeJr////////+qqmId3ZmZnd4iaqq/////////5h2"
"VDMiERESIzRFZ4nf////////2qqZh3d3d3d4iZqqr/////////mHVUMyIRESIjNEVnmt/////////aqpmId3d3eIiZqqrv////////6XZUQyIhERIiM0VXia"
"7////////6qqqZiIiIiIiZqqqv////////+YdlQzIiEREiIzRWeJr/////////qqqqmZiIiJmaqqqv/////////5h2VDMiERESIjNFZ4mv////////6qqqqq"
"mZmZmqqqqu/////////9l2VUMyIRERIjNEVnid/////////aqqqqqqqqqqqqqv//////////mHZUMyIhEREiM0RWea3////////9qqqqqqqqqqqqrv//////"
"////2XZlQzIiEREiIzRVeJrv////////qqqqqqqqqqre////////////+YdlRDMiERESIjNFZ4mv////////////////////////////////+YdlVDMiIRER"
"IiM0Vnia/////////////////////////////////Zh2VEMyIREREiM0RWeK3////////////////////////////////ph2VUMyIhERESIzRFZ5rf//////"
"/////////////////////////ph3ZUQzIiERESIjNFVomu///////////////////////////////ZmHZUQzIiERERIiM0Vnia//////////////////////"
"////////+qmHZVQzMiIREREiIzRWeJr/////////////////////////////6qmHZlRDMiIRERESIjNFZ4nf///////////////////////////+qqmHZlRD"
"MiIhERERIiNEVnit///////////////////////////qqqmHdlVDMyIhEREREiM0RWea7/////////////////////////6qqqqYdlVEMyIiERERESIzRVaJ"
"r//////////////////////////6qqqYdmVEMzIiERERESIjNFZ4mv////////+qqqqqqqr/////////2qqph2VUQzIiIREREBIiM0Vnid/////////aqqqq"
"qqqt/////////6qqh3ZUQzMiIREREQEiIzRWeJ3////////9qqqqqqqqr/////////6qmHZlRDMiIhEREQASIjNFZ4nv////////qqqqqqqqqt/////////6"
"qYdmVEMyIiERERABIiM0Vnif////////+qqqmZmZqqrv////////6ph2ZUQzIiEREREAEiI0RWeK/////////qqpmIiIiZqq/////////9qYdlRDMiIREREA"
"ASIjRFZ43////////9qpmId3eImarv////////+ph2VEMyIhEREQABIiNEVnjf////////2pmHd2Z3eImq/////////9mHZUQzIiERERAAEiI0RWeO//////"
"//+qmHdmZmZ3iJrf////////+YdlRDMiEREREAASIjRFZ4/////////6mHdmVVVmZ4ia/////////+h2VEMyIREREAABIiNEVnj////////+mIdmVVVVVmeI"
"nf////////+HZUQzIhEREQAAEiIzRWff////////2YdmVUREVVZniJ7////////+dlRDMiERERAAARIjNFZ9/////////YdmVUREREVWZ4if/////////GVE"
"MyIREREAABEiM0VW7////////4d2VURERERFVmeI7/////////ZUQyIhEREAAAESIzRFb/////////d2VURDMzM0RVZneP/////////VQzIiEREQAAARIiM0"
"Vf////////52VURDMzMzNERVZ33/////////VDMiERERAAABESIzRN/////////mVUQzMzMzMzREVWZ//////////kMyIRERAAAAERIiM0RWZ3d3d3dmVUQz"
"MyIiIjMzRFVmZ3d3d3ZlVEMyIhEREAAAARESIzNEVWZmZmZlVEQzMiIiIiIjM0RFVWZmZmZlVEMzIhEREQAAABERIiMzREVVVVVVREMzMiIiIiIiIjM0RFVV"
"VVVVREMzIiERERAAAAERESIjMzRERERERDMzIiIiIiIiIiIzM0RERERERDMzIiERERAAAAAREREiIjMzNEREMzMyIiIiERERIiIiIzMzNEREMzMyIiEREREA"
"AAABERERIiIjMzMzMzMiIiIREREREREiIiIjMzMzMzMiIiEREREAAAAAERERESIiIiIiIiIiIiERERERERERESIiIiIiIiIiIiEREREQAAAAABERERERIiIi"
"IiIiIRERERERERERERESIiIiIiIiIREREREQAAAAAAARERERERERERERERERERERERERERERERESIhEREREREREQAAAAAAAAAAAAAAAAAAABERERERERESIi"
"IiIiIiIiIiIhEREREREREAAAAAAAAAAAAAAAERERERERESIiIiIiIiIiIiIiIiIhEREREREQAAAAAAAAAAAAABERERERESIiIiIiMzMzMzMzMyIiIiIhERER"
"ERAAAAAAAAAAAAARERERERIiIiIzMzMzMzMzMzMzMzIiIiIREREREAAAAAAAAAAAEREREREiIiIzMzNEREREREREREMzMzIiIiEREREQAAAAAAAAARERERES"
"IiIzMzREREVVVVVVVVVEREQzMyIiIRERERAAAAAAAAERERERIiIjMzRERVVVVmZmZmZmVVVVREMzMiIhERERAAAAAAABERERESIiIzNERVVWZmZ3d3d3d3Zm"
"ZlVURDMzIiEREREAAAAAABEREREiIjM0RFVWZnd3iIiIiIiIh3d2ZlVUQzMiIREREAAAAAARERERIiIzNEVWZnd4iImZmZmZmZmIiHd2ZVRDMyIhEREAAAAA"
"ERERESIjM0RVZmd4iZnd7v///////u2YiHdmVUMzIhEREAAAAAEREREiIzNEVWZ3iJ3v///////////////oh2ZUQzIiEREAAAABERERIiMzRFZneJnv////"
"///////////////nZlRDMiEREAAAARERESIjM0RWZ4ie//////////////////////7VVDMiIREAAAAREREiIjNEVneJ7////////////////////////+VE"
"MiIREAAAEREREiIzRFZ3if//////////////////////////1EMiIREAAAERERIiM0RWd43///////////////////////////VUMyIREAAAERERIiM0VmeN"
"///////////////////////////9VUMyIREAABERESIjNFVnif///////////////////////////2VUMiIREAABERESIjRFZ4n/////////////////////"
"///////WVEMiIREAABEREiIzRVZ47///////////////////////////92VEMiIREAAREREiIzRWeN///////////+3Kqqqqqt3v//////1mVDMiEREAARER"
"EiM0RWeP//////////6qqqqqqqqqqqrv////dlRDMiEREAAREREiM0Vnif/////////6qqqqqpmZmZmZmZ3v/9ZVQzIiEREAARERIiM0Vnjv////////+qqq"
"qpmZiIiIiIiIh8/2VUQzIhEREAARERIiNEVnj/////////qqqqqZmIiHd3d3d3d2ZlVEMyIhEREAARERIiNFVon////////+qqqqmYiHd3d3ZmZmZmVVRDMy"
"IhEREAARERIiNFZ4n////////9qqqqmYh3d2ZmZmZmVVVEQzMiIREREAARERIjNFZ43////////9qqqqmYh3d2ZmZVVVVUREMzIiIREREAERERIjNFZ43///"
"/////9qqqqmYiHd2ZmVVVVRERDMyIiEREREAARERIjNFZ43/////////qqqqqZiHd2ZmVVVUREMzMiIiEREREAARERIjNFZ43/////////6qqqqpmId3ZmZV"
"VUREMzMiIhEREREAARERIiNFVoz//////////qqqqqmZiHd2ZlVURDMzMiIhEREREAARERIiNEVnn///////////6qqqqpmIh3ZmVVREMzIiIhERERAAARER"
"IiM0Vnj////////////+2qqqqZiId2ZVVEMzIiIhERERAAARERIiM0Vnjv//////////////2qqqmYh3ZlVEQzMiIhERERAAAREREiM0VWeP////////////"
"////7aqpmId2VURDMiIhERERAAAREREiI0RWeN//////////////////7aqZh3ZVRDMyIiERERAAAREREiIzRWZ47///////////////////7JmHdmVEMyIi"
"EREREAAREREiIzRFZ4n/////////////////////2Yh2VUQzIiEREREAAREREiIjNFVnie/////////////////////piHZVQzIiEREREAARERESIjNEVmeJ"
"7/////////////////////2IdlRDMiIREREAARERESIjM0VWd4mu/////////////////////odmVDMiIREREAARERESIjM0RVZ3iZrv////////////////"
"///9h2VEMyIREREAARERESIiMzRFVneJmq3///////////////////mGZUMyIhEREAERERESIiMzRFVWd4iZqq3/////////////////6HZUQyIhEREAERER"
"ESIiMzNEVWZneImaqq3v///////////////HZVQzIhEREAEREREiIiMzNERVZmd4iZmqqqrv/////////////odlQzIhEREAEREREiIjMzNERVVmZ3eImZqq"
"qqrv///////////4dlQzIhEREBEREREiIjMzRERFVWZmd4iJmaqqqq7//////////4dlQzIiEREBERERIiIzM0RERVVVZmZ3eIiZqqqqqv/////////5dlRD"
"IiERERERESIiMzRERFVVVVZmZnd4iJmqqqqt/////////5dlRDIiERERERESIjM0RFVVVVVmZmZnd3iImaqqqq/////////5hlRDIiEREREREiIzREVVVmZm"
"ZmZmZnd3eImaqqqq7////////5dlRDIiERERERIiMzRFVmZnd3dmZmd3d3iImaqqqq7////////5dlRDIiERERERIiM0RW39d3d3d3d3d3d4iImaqqqq////"
"/////odlQzIhERERERIjNEVm//6IiIiIiIiIiIiZmqqqqt/////////YdlQzIhERERESIjRFZt///+mZmZmZmZmZmZqqqqqv////////+YdlQzIhERERESIz"
"RFZ//////tqqqZmZmaqqqqqqz/////////+HZUQyIhERERESIzRWbf///////tqqqqqqqqqqqu//////////2HZUMyIhEREREiI0RWf//////////+3aqqqq"
"3e////////////l2VUMyIREREREiI0RW7//////////////////////////////8h2VEMiIREREREiI0VW///////////////////////////////odlVDMi"
"IREREREiI0Ve//////////////////////////////6IdlRDIiERERAREiI0Rf/////////////////////////////+mHZUQzIiERERAREiI0Rf////////"
"/////////////////////ph2VUMyIhERERARESIzRW7///////////////////////////2IdlVEMyIhEREQARESIzRFZ+/////////////////////////p"
"iHZVRDMiIREREQARESIjNFVnfv/////////////////////+yYd2VUQzIiEREREAARERIjNEVWd4zv//////////////////6ZiHZlVEMyIiEREREAARERIi"
"M0RVZneIne//////////////7ZiId2VURDMiIhERERAAAREREiIzRFVmd3iImc3u7////+7dmZiId3ZlVEMzIiIREREQAAAREREiIjNERVZmd3iIiJmZmZmZ"
"mYiId3ZmVURDMyIiEREREAAAARERESIiMzREVVZmd3d4iIiIiIh3d3ZmVVREMzIiIhEREREAAAABERERIiIzM0RFVVZmZmd3d3d3ZmZmVVVEQzMyIiERERER"
"AAAAAAEREREiIiMzNEREVVVVVmZmZmVVVVRERDMzIiIhEREREQAAAAAAERERERIiIjMzNERERFVVVVVVVERERDMzMiIiEREREREAAAAAAAAREREREiIiIjMz"
"MzREREREREREMzMzMiIiIRERERERAAAAAAAAABERERERIiIiIjMzMzMzMzMzMzMzMiIiIhEREREREQAAAAAAAAAAERERERERIiIiIiIzMzMzMzMyIiIiIiER"
"ERERERAAAAAAAAAAAAARERERERESIiIiIiIiIiIiIiIiIiEREREREREQAAAAAAAAAAAAABERERERERERIiIiIiIiIiIiIiERERERERERAAAAAAAAAAAAABER"
"ERERESIiIiIiIiIiIiIiIiIiIiIiIiIiIiEREREREQAAAAEREREREiIiIiIiMzMzMzMzMzMzMzMzMiIiIiIiIhEREREQAAARERERIiIiIjMzMzMzMzMzMzMz"
"MzMzMzMzMzMiIiIiEREREQABEREREiIiMzMzM0REREREREREREREREREQzMzMzMiIiIREREQARERESIiMzM0REREREVVVVVVVVVVVVVEREREREQzMzIiIRER"
"EBERERIiMzNERFVVVVVVZmZmZmZmZmZlVVVVVVVEREMzIiIREREREREiIzNERVVWZmZmZnd3d3d3d3d3dmZmZmZmVVVEQzIiIRERERESIjNERVZmZ3d3d4iI"
"iIiIiIiIiIh3d3d3dmZlVUQzIiEREREREiM0RVZnd4iIiJmZmZmZmZmZmZmZmYiIiId3dmVUQzIiERERESIjNFVmd4iZmZqqqqqqqqqqqqqqqqqqmZmYiHdm"
"VUQyIhEREREiM0Rd//////////////////////////////////9UMyIRERERIjNFbf/////////////////////////////////+VEMiIREREiI0RW7/////"
"/////////////////////////////lRDIiERERIjNFZv//////////////////////////////////1VQyIhERESIzRWf///////////////////////////"
"///////2VUMyIREREiM0Vt//////////////////////////////////9mVDMiERERIjNFbf/////////////////////////////////+ZVQyIhERESIzRW"
"7//////////////////////////////////mVEMiIREREiM0Vv//////////////////////////////////1lQzIhERERIjNFX/////////////////////"
"/////////////2VUMyIRERESIjRN//////////////////////////////////9lRDIiEREREiIzRVZ3iJmqqqqqqt/////////qqqqqqqmZiHdmVEMyIRER"
"EREiM0RVZneImaqqqqrf////////2qqqqpmYiHdmVUQzIiERERERIiM0RVZnd4iZqqqq7////////6qqqpmIh3dmZVRDMiIRERERERIiM0RVVmd4iZqqqv//"
"//////+qqpmId3ZmVVREMzIiERERERESIiM0RFVmZ3iZqqr////////+qqmYd2ZlVVREMzIiIRERERERESIiMzREVWZ3iaqs/////////aqZh3ZlVUREMzMi"
"IhERERABERESIiMzREVWZ4mqrf////////2qmHdlVUREMzMiIiEREREAARERESIiMzREVWd4mq7////////6qYd2VUREMzMiIiERERERAAARERESIiMzRFVm"
"eJqv////////+qmHZVREMzMiIiIREREREAAAERERESIiMzRFVniar////////+qpd2VUQzMyIiIREREREQAAAAEREREiIjM0RVZ4ms/////////amHZVRDMy"
"IiIhERERERAAAAAAEREREiIjNEVWeJrf////////2ph2VEMzIiIiERERERAAAAAAABERERIiIzNFVnia7////////6qHZlRDMiIiEREREREAAAAAAAABERES"
"IiMzRVZ4mv////////+ph2VUMzIiIREREREQAAAAAAAAARERESIiM0VWeJr////////+qYdlRDMiIiEREREQAAAAAAAAAAEREREiIzNFVnis/////////amH"
"ZUMzIiIRERERAAAAAAAAAAAAERERIiIzRWZ5rf////////2YdlVDMiIhEREREAAAAAAAAAAAABERESIiM0Vnia7////////6mHZUQzIiIREREQAAAAAAAAAA"
"AAAREREiIjNFZ4mv////////+ph2VDMyIhERERAAAAAAAAAAAAAAERERIiM0RWeJr////////+qXZVQzIiIREREAAAAAAAAAAAAAABERESIjNEVnic//////"
"///ah2VEMyIhEREQAAAAAAAAAAAAAAAREREiIzRFZ4nf////////2YdlRDMiIREREAAAAAAAAAAAAAAAERERIiM0VWeK7////////6mHZUMyIhEREQAAAAAA"
"AAAAAAAAABERESIjNFZomv////////+phmVDMiIREREAAAAAAAAAAAAAAAAREREiIzRWeJr////////+qHZUQzIiERERAAAAAAAAAAAAAAAAERERIiM0Vnic"
"/////////Zh2VEMyIhEREAAAAAAAAAAAAAAAABERESIjRFZ4nf////////2YdlQzIiERERAAAAAAAAAAAAAAAAARERIiM0RWeK7////////6mHZUMyIhEREQ"
"AAAAAAAAAAAAAAAAERESIjNEVnmv////////+phlVDMiIREREAAAAAAAAAAAAAAAAREREiIzRWeJr////////+qHZUQzIiEREQAAAAAAAAAAAAAAAAERERIi"
"M0Vnic/////////Zh2VEMiIREREAAAAAAAAAAAAAAAABERESIjNFZ4nf////////2YdlQzIiERERAAAAAAAAAAAAAAAAAREREiI0RWeJ7////////6mHZUMy"
"IhEREQAAAAAAAAAAAAAAAAERERIjNEVnif////////+odlRDMiIRERAAAAAAAAAAAAAAAAABERESIzRFZ4r////////+mHZUQzIhEREQAAAAAAAAAAAAAAAA"
"AREREiM0VWec/////////Zh2VEMiIREREAAAAAAAAAAAAAAAAAERERIjNFZ4nf////////2YdlQzIiERERAAAAAAAAAAAAAAAAABERESIzRWeJ7////////6"
"h2VUMyIhEREQAAAAAAAAAAAAAAAAAREREiM0Vnif////////+YdlRDMiERERAAAAAAAAAAAAAAAAAAERERIjNFZ4n////////+mHZUMyIhEREQAAAAAAAAAA"
"AAAAAAABERESIzRWeM/////////ZhlVDMiIREREAAAAAAAAAAAAAAAAAAREREiM0Vnjf////////2HZUQzIhEREQAAAAAAAAAAAAAAAAAAERERIjNFZ47///"
"/////5h2VDMiIREREAAAAAAAAAAAAAAAAAABERESIzRWZ/////////+HZVQzIiERERAAAAAAAAAAAAAAAAAAAREREiI0RWf////////+h2VEMiIREREAAAAA"
"AAAAAAAAAAAAAAARERIiNEVn/////////XZVQzIiERERAAAAAAAAAAAAAAAAAAAAERESIjNFbf////////1mVEMyIREREQAAAAAAAAAAAAAAAAAAABERESIz"
"RF7////////2ZUQzIiERERAAAAAAAAAAAAAAAAAAAAAREREiIzRf////////9lVDMiIREREQAAAAAAAAAAAAAAAAAAAAARERIiM0RVZnd3d3ZmVUMzIiERER"
"AAAAAAAAAAAAAAAAAAAAAAERERIiM0RVVmZmZmVUQzMiIREREQAAAAAAAAAAAAAAAAAAAAABERERIiM0RFVVVVVURDMyIhERERAAAAAAAAAAAAAAAAAAAAAA"
"ABERESIiMzRERERERDMzIiEREREQAAAAAAAAAAAAAAAAAAAAAAARERESIiMzMzREMzMzIiIRERERAAAAAAAAAAAAAAAAAAAAAAAAARERESIiIjMzMzMzIiIh"
"EREREAAAAAAAAAAAAAAAAAAAAAAAAAARERERIiIiIiIiIiIhEREREQAAAAAAAAAAAAAAAAAAAAAAAAAAARERERESIiIiIiIhERERERAAAAAAAAAAAAAAAAAA"
"AAAAAAAAAAAREREREREREREREREREREAAAAAAAAAAAAAAAAAAAAA";

static uint32_t g_hr[HR_W * HR_H];         /* cuadro del menu */
static uint32_t g_bg[HR_W * HR_H];         /* fondo mosaico (se arma una vez) */
static uint8_t  g_fgd[FG_NDATA];           /* alfa de 4 bits de las letras, ya decodificado */
static int      g_hr_on;                   /* 1 = el cuadro que se esta armando es de 1280x720 */
static int      g_hr_ready;
static int      hr_usable(void);           /* definida junto al video (PS3 y PC) */

static void hr_b64(void)
{
    static signed char T[256];
    const char *p = fg_b64;
    uint32_t acc = 0;
    int bits = 0, i;
    size_t n = 0;
    for (i = 0; i < 256; i++) T[i] = -1;
    for (i = 0; i < 26; i++) { T['A' + i] = (signed char)i; T['a' + i] = (signed char)(26 + i); }
    for (i = 0; i < 10; i++) T['0' + i] = (signed char)(52 + i);
    T['+'] = 62; T['/'] = 63;
    for (; *p && n < FG_NDATA; p++) {
        if (T[(unsigned char)*p] < 0) continue;
        acc = (acc << 6) | (uint32_t)T[(unsigned char)*p];
        bits += 6;
        if (bits >= 8) { bits -= 8; g_fgd[n++] = (uint8_t)(acc >> bits); }
    }
}

static void hr_make_bg(void)
{
    /* fondo ochentero: cielo morado con estrellas, sol a rayas y suelo con rejilla en perspectiva (se arma una vez) */
    const int HZ = 430, SCX = 1030, SCY = 250, SR = 150;
    uint32_t rs = 777u;
    int x, y, i;
    for (y = 0; y < HR_H; y++) {
        int r, g, b;
        if (y < HZ) {                                         /* cielo: indigo arriba, magenta cerca del horizonte */
            int t = y * 256 / HZ;
            r = 8 + (t * t >> 8) * 84 / 256; g = 2 + (t * t >> 8) * 8 / 256; b = 30 + t * 62 / 256;
        } else {                                              /* suelo oscuro */
            int t = (y - HZ) * 256 / (HR_H - HZ);
            r = 22 - t * 14 / 256; g = 3; b = 40 - t * 18 / 256;
        }
        for (x = 0; x < HR_W; x++) g_bg[y * HR_W + x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    for (i = 0; i < 150; i++) {                               /* estrellas */
        int sx, sy, v;
        rs = rs * 1103515245u + 12345u; sx = (int)((rs >> 8) % HR_W);
        rs = rs * 1103515245u + 12345u; sy = (int)((rs >> 8) % (HZ - 40));
        rs = rs * 1103515245u + 12345u; v = 90 + (int)((rs >> 16) % 120);
        g_bg[sy * HR_W + sx] = 0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | 255u;
        if ((rs & 7) == 0) { g_bg[sy * HR_W + sx + 1] = g_bg[sy * HR_W + sx]; g_bg[(sy + 1) * HR_W + sx] = g_bg[sy * HR_W + sx]; }
    }
    for (y = SCY - SR; y <= SCY + SR && y < HZ; y++) {        /* sol: degradado amarillo -> rosa, con franjas que lo cortan hacia abajo */
        int dy = y - SCY, k = y - (SCY - SR), cut = 0;
        if (k > SR) { int d = k - SR, band = d / 20; cut = (d % 20) < band * 2 + 1; }
        if (cut) continue;
        for (x = SCX - SR; x <= SCX + SR; x++) {
            int dx = x - SCX;
            if (dx * dx + dy * dy <= SR * SR) {
                int t = k * 256 / (2 * SR);
                int r = 255, g = 235 - t * 190 / 256, b = 60 + t * 120 / 256;
                g_bg[y * HR_W + x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }
    for (y = HZ; y < HR_H; y++) {                             /* rejilla: horizontales que se juntan hacia el horizonte + verticales que convergen */
        int dy = y - HZ;
        int hl = 0;
        {
            int n;
            for (n = 1; n < 14; n++) { int yy = HZ + (n * n * n) / 5 + n * 2; if (yy == y || yy + 1 == y) hl = 1; }
        }
        for (x = 0; x < HR_W; x++) {
            int vx = (x - 640) * 100 / (dy + 30);             /* posicion proyectada en el suelo */
            int ph = ((vx % 34) + 34) % 34;
            int vl = (dy > 14 && (ph < 2 || ph > 32));
            if (hl || vl) {
                uint32_t p = g_bg[y * HR_W + x];
                int fade = dy * 256 / (HR_H - HZ);            /* mas intensa cerca del espectador */
                int a = 70 + fade * 110 / 256;
                int r = (int)((p >> 16) & 255), g = (int)((p >> 8) & 255), b = (int)(p & 255);
                r = (r * (256 - a) + 255 * a) >> 8; g = (g * (256 - a) + 60 * a) >> 8; b = (b * (256 - a) + 230 * a) >> 8;
                g_bg[y * HR_W + x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }
    for (y = 0; y < HR_H; y += 3)                             /* lineas de barrido tipo monitor CRT */
        for (x = 0; x < HR_W; x++) {
            uint32_t p = g_bg[y * HR_W + x];
            g_bg[y * HR_W + x] = 0xFF000000u | ((((p >> 16) & 255) * 225 >> 8) << 16) | ((((p >> 8) & 255) * 225 >> 8) << 8) | (((p & 255) * 225) >> 8);
        }
    for (y = 0; y < HR_H; y++) {                              /* vineta: oscurece hacia los bordes */
        int dy = (y - 360) * 100 / 360;
        for (x = 0; x < HR_W; x++) {
            int dx = (x - 640) * 100 / 640;
            int d2 = dx * dx + dy * dy;
            int w = d2 > 10000 ? 10000 : d2;
            int f = w * 40 / 10000 * 256 / 100;
            uint32_t p = g_bg[y * HR_W + x];
            int r = (int)((p >> 16) & 255), g = (int)((p >> 8) & 255), b = (int)(p & 255);
            r = (r * (256 - f) + 2 * f) >> 8; g = (g * (256 - f) + 1 * f) >> 8; b = (b * (256 - f) + 10 * f) >> 8;
            g_bg[y * HR_W + x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

static void hr_rect(int x0, int y0, int x1, int y1, uint32_t col, int alpha)   /* x1,y1 exclusivos; alpha 0..256 */
{
    int x, y;
    int cr = (int)((col >> 16) & 255), cg = (int)((col >> 8) & 255), cb = (int)(col & 255);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > HR_W) x1 = HR_W;
    if (y1 > HR_H) y1 = HR_H;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            uint32_t *d = &g_hr[y * HR_W + x];
            if (alpha >= 256) { *d = 0xFF000000u | (col & 0xFFFFFF); continue; }
            {
                int r = (int)((*d >> 16) & 255), g = (int)((*d >> 8) & 255), b = (int)(*d & 255);
                r = (r * (256 - alpha) + cr * alpha) >> 8; g = (g * (256 - alpha) + cg * alpha) >> 8; b = (b * (256 - alpha) + cb * alpha) >> 8;
                *d = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
}

static void hr_frame_rect(int x0, int y0, int x1, int y1, int t, uint32_t col)
{
    hr_rect(x0, y0, x1, y0 + t, col, 256); hr_rect(x0, y1 - t, x1, y1, col, 256);
    hr_rect(x0, y0, x0 + t, y1, col, 256); hr_rect(x1 - t, y0, x1, y1, col, 256);
}

static void hr_panel(int x0, int y0, int x1, int y1)
{
    hr_rect(x0, y0, x1, y1, 0xFF0A0220u, 215);
    hr_rect(x0 - 3, y0 - 3, x1 + 3, y0, HC_GRID, 40); hr_rect(x0 - 3, y1, x1 + 3, y1 + 3, HC_GRID, 40);   /* halo exterior */
    hr_rect(x0 - 3, y0, x0, y1, HC_GRID, 40); hr_rect(x1, y0, x1 + 3, y1, HC_GRID, 40);
    hr_frame_rect(x0, y0, x1, y1, 3, HC_GRID);
    hr_frame_rect(x0 + 8, y0 + 8, x1 - 8, y1 - 8, 1, 0xFFB0207Au);                                           /* filete rosa interior */
}

/* barra de la fila marcada: tubo rosa de neon con relleno tenue y halo */
static void hr_selbar(int x0, int y0, int x1, int y1)
{
    hr_rect(x0 - 4, y0 - 4, x1 + 4, y1 + 4, HC_RED, 34);
    hr_rect(x0, y0, x1, y1, 0xFF3A0A40u, 235);
    hr_frame_rect(x0, y0, x1, y1, 3, HC_RED);
}

static void hr_init(void)
{
    if (g_hr_ready) return;
    hr_b64();
    hr_make_bg();
    g_hr_ready = 1;
}

static void hr_begin(void)
{
    hr_init();
    memcpy(g_hr, g_bg, sizeof g_hr);
    g_hr_on = 1;
}

static const FGlyph *hr_glyph(int sz, int cp)
{
    int lo = fg_start[sz], hi = fg_start[sz + 1] - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (fg_tab[mid].cp == cp) return &fg_tab[mid];
        if (fg_tab[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* siguiente caracter UTF-8 (solo 1 y 2 bytes: ASCII y Latin-1: acentos, n con tilde, signos de apertura) */
static int hr_next(int sz, const char **ps)
{
    const unsigned char *p = (const unsigned char *)*ps;
    int c = *p++;
    if (c >= 0xC2 && c <= 0xC3 && (*p & 0xC0) == 0x80) { c = ((c & 0x1F) << 6) | (*p & 0x3F); p++; }
    else if (c >= 0x80) c = '?';
    if (c < 32 || (c > 126 && c < 0xA0)) c = '?';
    if ((sz == FS_H || sz == FS_X) && c >= 'a' && c <= 'z') c -= 32;
    *ps = (const char *)p;
    return c;
}

static int hr_tw(const char *s, int sz)
{
    int w = 0;
    while (*s) {
        const FGlyph *g = hr_glyph(sz, hr_next(sz, &s));
        w += g ? g->adv : fg_asc[sz] / 3;
    }
    return w;
}

static int g_neon = 1;      /* 1 = letras de neon (tubo + halo); 0 = plano (botones de PlayStation) */

static void hr_glyph_draw(int x, int base, const FGlyph *g, uint32_t col)
{
    int gx, gy, cr = (int)((col >> 16) & 255), cg = (int)((col >> 8) & 255), cb = (int)(col & 255);
    int gk = 16, wr, wg, wb, glowc = 1;
    uint32_t gcol = col;
    if (!g || !g->w) return;
    if (col == HC_SHAD) return;                               /* el neon no lleva sombra */
    if (col == HC_DIM || col == HC_MUTE) gk = 6;              /* apagadas: halo tenue */
    if (col == HC_WHITE) gcol = HC_GRID;                      /* blanco = tubo blanco con halo cian */
    wr = (cr * 90 + 255 * 166) >> 8; wg = (cg * 90 + 255 * 166) >> 8; wb = (cb * 90 + 255 * 166) >> 8;   /* nucleo: color mezclado con blanco */
    if (!g_neon) { wr = cr; wg = cg; wb = cb; glowc = 0; }
    for (gy = 0; gy < g->h; gy++) {
        int py = base + g->by + gy;
        if (py < 0 || py >= HR_H) continue;
        for (gx = 0; gx < g->w; gx++) {
            int px = x + g->bx + gx, idx = gy * g->w + gx, v;
            uint32_t *d;
            int r, gg, b;
            if (px < 0 || px >= HR_W) continue;
            v = g_fgd[g->off + (uint32_t)(idx >> 1)];
            v = (idx & 1) ? (v & 15) : (v >> 4);
            if (!v) continue;
            d = &g_hr[py * HR_W + px];
            r = (int)((*d >> 16) & 255); gg = (int)((*d >> 8) & 255); b = (int)(*d & 255);
            if (glowc) {
                int a = (v > 11 ? 11 : v) * gk;
                r = (r * (256 - a) + (int)((gcol >> 16) & 255) * a) >> 8;
                gg = (gg * (256 - a) + (int)((gcol >> 8) & 255) * a) >> 8;
                b = (b * (256 - a) + (int)(gcol & 255) * a) >> 8;
            }
            if (v > 11) {
                int a = (v - 11) * 64;
                if (a > 256) a = 256;
                r = (r * (256 - a) + wr * a) >> 8; gg = (gg * (256 - a) + wg * a) >> 8; b = (b * (256 - a) + wb * a) >> 8;
            }
            *d = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8) | (uint32_t)b;
        }
    }
}

/* texto con sombra; x = borde izquierdo, base = linea base. Devuelve el ancho. */
static int hr_text(int x, int base, const char *s, int sz, uint32_t col, int shadow)
{
    int x0 = x;
    while (*s) {
        const FGlyph *g = hr_glyph(sz, hr_next(sz, &s));
        if (g) {
            if (shadow) hr_glyph_draw(x + shadow, base + shadow, g, HC_SHAD);
            hr_glyph_draw(x, base, g, col);
            x += g->adv;
        } else x += fg_asc[sz] / 3;
    }
    return x - x0;
}

static uint32_t hr_col(uint32_t c)
{
    switch (c) {
    case 0xFFFFFFFFu: return HC_WHITE;
    case 0xFF8080A0u: return 0xFF8C9CD2u;       /* apagado, un poco mas claro para que se lea en la tele */
    case 0xFFC0C0FFu: return HC_DIM;
    case 0xFF80FF80u: return HC_GREEN;
    case 0xFFFFC040u: return HC_AMBER;
    case 0xFFFFFF40u: return HC_YEL;
    case 0xFFFFD040u: return HC_YEL;
    case 0xFFA0A0B0u: return 0xFFC8CCE0u;
    case 0xFF505068u: return HC_MUTE;
    default: return c;
    }
}

static void hr_tri(int x, int yc, int h)
{
    int i, j;
    for (i = 0; i < h; i++) {
        int half = i < h / 2 ? i : h - 1 - i;
        for (j = 0; j <= half; j++) hr_rect(x + j * 2, yc - h / 2 + i, x + j * 2 + 2, yc - h / 2 + i + 1, HC_YEL, 256);
    }
}

/* Logo "NES" blanco + "NET" rojo con barra roja y la version chica a la derecha, centrado */
static void hr_logo(int y, const char *ver)
{
    int base = y * 3 + 44, wn = hr_tw("ARCADE ", FS_H), wt = hr_tw("STATION", FS_H), wv = hr_tw(ver, FS_S);
    int total = 26 + wn + wt + 16 + wv, x = (HR_W - total) / 2, cap = fg_asc[FS_H] * 3 / 4;
    hr_rect(x, base - cap, x + 10, base, HC_RED, 256);
    x += 26;
    x += hr_text(x, base, "ARCADE ", FS_H, HC_WHITE, 4);
    x += hr_text(x, base, "STATION", FS_H, HC_RED, 4);
    hr_text(x + 16, base, ver, FS_S, HC_DIM, 2);
}

/* Traduce una llamada de ui_text / ui_center (coordenadas del lienzo 256x240) al lienzo de 1280x720 */
static void hr_ui_text(int cx, int y, const char *s, int sc, uint32_t col, int center)
{
    const char *p = s;
    int sel, row, sz, w, x, yc, cap, base;
    if (!strncmp(s, "ARCADE STATION V", 16)) { hr_logo(y, s + 15); return; }
    sel = (s[0] == '>' && s[1] == ' ');
    row = sel || (s[0] == ' ' && s[1] == ' ');
    if (sel) p += 2;
    while (*p == ' ') p++;
    if (!*p) return;
    if (sc >= 2 && col == 0xFFFFD040u) {                  /* titulo de pantalla: barra roja + texto grande */
        sz = FS_H;
        w = hr_tw(p, sz);
        x = (HR_W - w - 26) / 2;
        yc = y * 3 + 21;
        cap = fg_asc[sz] * 3 / 4;
        base = yc + cap / 2;
        hr_rect(x, base - cap, x + 10, base, HC_RED, 256);
        hr_text(x + 26, base, p, sz, HC_WHITE, 3);
        return;
    }
    sz = sc >= 2 ? FS_H : FS_M;
    w = hr_tw(p, sz);
    if (w > 1180) { sz = sc >= 2 ? FS_L : FS_S; w = hr_tw(p, sz); }
    if (w > 1180) { sz = FS_S; w = hr_tw(p, sz); }
    yc = y * 3 + (21 * sc + 1) / 2;
    cap = fg_asc[sz] * 3 / 4;
    base = yc + cap / 2;
    if (center) x = (HR_W - w) / 2;
    else x = row ? cx - 16 + 52 : cx;
    if (!center && row) {                    /* filas "NOMBRE   VALOR" (MANDOS): el valor va en una segunda columna fija */
        const char *dbl = strstr(p, "  ");
        if (dbl && dbl[2]) {
            char nm[40];
            size_t n = (size_t)(dbl - p);
            const char *val = dbl;
            uint32_t c1 = sel ? (col == 0xFFFFFFFFu ? HC_WHITE : 0xFFE0E0EEu) : hr_col(col);
            while (*val == ' ') val++;
            if (n >= sizeof nm) n = sizeof nm - 1;
            memcpy(nm, p, n); nm[n] = 0;
            if (sel) {
                int x0 = cx - 16;
                hr_selbar(x0, yc - 17, HR_W - x0, yc + 17);
                hr_tri(x0 + 18, yc, 20);
            }
            hr_text(x, base, nm, sz, c1, 2);
            hr_text(x + 330, base, val, sz, c1, 2);
            return;
        }
    }
    if (sel) {
        int bh = center ? 44 : 34, x0, x1;
        if (center) { x0 = HR_W / 2 - 380; x1 = HR_W / 2 + 380; }
        else { x0 = cx - 16; x1 = HR_W - x0; }
        hr_selbar(x0, yc - bh / 2, x1, yc - bh / 2 + bh);
        hr_tri(x0 + 18, yc, 20);
        hr_text(x, base, p, sz, col == 0xFFFFFFFFu ? HC_WHITE : 0xFFE0E0EEu, 2);
    } else {
        hr_text(x, base, p, sz, hr_col(col), 2);
    }
}

/* Barra de volumen (10 tramos) para OPCIONES > SONIDO */
static void hr_vol_bar(int y, int n)
{
    int i, x0 = (HR_W - (10 * 56 + 9 * 10)) / 2, yc = y * 3 + 21;
    for (i = 0; i < 10; i++)
        hr_rect(x0 + i * 66, yc - 15, x0 + i * 66 + 56, yc + 15, i < n ? HC_GREEN : 0xFF28368Au, 256);
}

/* ---- piezas de los disenos aprobados (logo a la izquierda, cuadros, filas con barra roja, botones X / O abajo) ---- */
typedef struct { const char *b; const char *t; uint32_t c; } HHint;
#define HC_BX 0xFF3460D2u            /* boton X (azul) */
#define HC_BO 0xFFDE263Au            /* boton O (rojo) */
#define HC_BG 0xFF46466Eu            /* otros botones */

static void hr_px(int x, int y, uint32_t col, int a)
{
    uint32_t *d;
    int r, g, b;
    if (x < 0 || y < 0 || x >= HR_W || y >= HR_H) return;
    d = &g_hr[y * HR_W + x];
    r = (int)((*d >> 16) & 255); g = (int)((*d >> 8) & 255); b = (int)(*d & 255);
    r = (r * (256 - a) + (int)((col >> 16) & 255) * a) >> 8;
    g = (g * (256 - a) + (int)((col >> 8) & 255) * a) >> 8;
    b = (b * (256 - a) + (int)(col & 255) * a) >> 8;
    *d = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void hr_disc(int cx, int cy, int r, uint32_t col)      /* circulo relleno con borde suave */
{
    int x, y;
    for (y = cy - r - 1; y <= cy + r + 1; y++)
        for (x = cx - r - 1; x <= cx + r + 1; x++) {
            int d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (d2 <= (r - 1) * (r - 1)) hr_px(x, y, col, 256);
            else if (d2 <= r * r) hr_px(x, y, col, 160);
        }
}

static int hr_badge(int x, int y, const char *label, uint32_t col)   /* devuelve el ancho */
{
    int w = 34, tw = hr_tw(label, FS_S);
    if (tw > 20) w = tw + 24;
    hr_disc(x + 17, y + 17, 17, HC_WHITE);
    hr_disc(x + 17, y + 17, 15, col);
    if (w > 34) {
        hr_disc(x + w - 17, y + 17, 17, HC_WHITE);
        hr_rect(x + 17, y, x + w - 17, y + 34, HC_WHITE, 256);
        hr_disc(x + w - 17, y + 17, 15, col);
        hr_rect(x + 17, y + 2, x + w - 17, y + 32, col, 256);
    }
    g_neon = 0;
    hr_text(x + (w - tw) / 2, y + 25, label, FS_S, HC_WHITE, 0);
    g_neon = 1;
    return w;
}

static void hr_hints(const HHint *h, int n)
{
    int i, total = 0, x, y = 668;
    for (i = 0; i < n; i++) total += (hr_tw(h[i].b, FS_S) > 20 ? hr_tw(h[i].b, FS_S) + 24 : 34) + 10 + hr_tw(h[i].t, FS_S);
    total += 44 * (n - 1);
    x = (HR_W - total) / 2;
    for (i = 0; i < n; i++) {
        int bw = hr_badge(x, y, h[i].b, h[i].c);
        x += bw + 10;
        x += hr_text(x, y + 25, h[i].t, FS_S, HC_DIM, 2) + 44;
    }
}

/* fondo + logo a la izquierda (barra roja, NES blanco, NET rojo, version) + subtitulo */
static void hr_head(const char *sub)
{
    int x = 96, base = 128;
    hr_begin();
    hr_rect(70, 52, 82, 82, HC_RED, 256); hr_rect(70, 90, 82, 118, 0xFFFF8A2Au, 256); hr_rect(70, 126, 82, 144, HC_YEL, 256);
    x += hr_text(x, base - 12, "ARCADE ", FS_H, HC_WHITE, 4);
    x += hr_text(x, base - 12, "STATION", FS_H, HC_RED, 4);
    hr_text(x + 16, base - 8, "v0.1", FS_S, HC_DIM, 2);
    if (sub) hr_text(98, 172, sub, FS_M, HC_DIM, 2);
}

/* filas del menu: barra roja con flecha en la marcada; 'dis' = bits de filas apagadas (bloqueadas) */
static void hr_rows(int x, int y, int w, const char *const *rows, int n, int sel, unsigned dis, int rh)
{
    int i, bh = rh - 8;
    for (i = 0; i < n; i++) {
        int yy = y + i * rh, base = yy + bh / 2 + 12;
        if (i == sel) {
            hr_selbar(x, yy, x + w, yy + bh);
            hr_tri(x + 16, yy + bh / 2, 22);
            hr_text(x + 52, base, rows[i], FS_L, HC_WHITE, 2);
        } else {
            hr_text(x + 52, base, rows[i], FS_L, (dis >> i) & 1 ? HC_MUTE : HC_DIM, 2);
        }
    }
}

static void hr_textc(int cx, int base, const char *s, int sz, uint32_t col) { hr_text(cx - hr_tw(s, sz) / 2, base, s, sz, col, 2); }

static void hr_arrow(int cx, int cy, int up)       /* triangulo chico para "hay mas arriba / abajo" */
{
    int r;
    for (r = 0; r < 14; r++) {
        int half = up ? r : 13 - r;
        hr_rect(cx - half, cy - 7 + r, cx + half + 1, cy - 6 + r, HC_DIM, 256);
    }
}

/* fila "NOMBRE ... VALOR" con barra roja si esta marcada; el valor empieza en x + vx */
static void hr_row2(int x, int y, int w, int rh, const char *nm, const char *val, int sel, int vx)
{
    int bh = rh - 8, base = y + bh / 2 + 12;
    if (sel) { hr_selbar(x, y, x + w, y + bh); hr_tri(x + 16, y + bh / 2, 22); }
    hr_text(x + 52, base, nm, FS_L, sel ? HC_WHITE : HC_DIM, 2);
    if (val) hr_text(x + vx, base, val, FS_L, sel ? HC_WHITE : HC_DIM, 2);
}

/* Fondo del menu R3 en partida: la imagen del juego (16:9) oscurecida + un cuadro al centro */
static uint32_t g_bd[HR_W * HR_H];         /* fondo del menu R3 (juego oscurecido + cuadro), se arma una vez por apertura */
static int      g_bd_dirty;
static void ui_text(int x, int y, const char *s, int sc, uint32_t col)
{
    if (g_hr_on) { hr_ui_text(x * 5, y, s, sc, col, 0); return; }
    for (; *s; s++, x += 6 * sc) {
        int c = (unsigned char)*s, cx, cy, i, j;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == '_') c = '-';                /* v1.7: la fuente no trae _ ni [ ]: nombres de ROM mas legibles */
        else if (c == '[') c = '(';
        else if (c == ']') c = ')';
        if (c < 32 || c > 90) c = '?';
        for (cx = 0; cx < 5; cx++) {
            uint8_t colbits = font5x7[c - 32][cx];
            for (cy = 0; cy < 7; cy++) {
                if (!(colbits & (1 << cy))) continue;
                for (j = 0; j < sc; j++)
                    for (i = 0; i < sc; i++) {
                        int px = x + cx * sc + i, py = y + cy * sc + j;
                        if (px >= 0 && px < SCR_W && py >= 0 && py < SCR_H)
                            g_fb[py * SCR_W + px] = col;
                    }
            }
        }
    }
}

static void ui_center(int y, const char *s, int sc, uint32_t col)
{
    if (g_hr_on) { hr_ui_text(0, y, s, sc, col, 1); return; }
    int w = (int)strlen(s) * 6 * sc - sc;
    ui_text((SCR_W - w) / 2, y, s, sc, col);
}

static void ui_clear(void)
{
    int i;
    if (hr_usable()) { hr_begin(); return; }      /* v1.9.7 */
    g_hr_on = 0;
    for (i = 0; i < SCR_W * SCR_H; i++) g_fb[i] = 0xFF101838;
}

/* ------------------------------------------------------------------ */
/* Plataforma: video, mando                                            */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Audio (v1.5)                                                        */
/* El nucleo entrega PCM de 16 bits estereo. Se re-muestrea a 48 kHz con un ajuste fino de   */
/* velocidad (maximo +-1 %) para seguir el reloj de la consola, y se escribe en el anillo de */
/* bloques de libaudio. Ninguna espera es bloqueante: si algo falla, el juego sigue sin sonido. */
/* ------------------------------------------------------------------ */
#define AU_BLK       256            /* muestras por bloque (AUDIO_BLOCK_SAMPLES) */
#define AU_NBLK      32             /* bloques del anillo (AUDIO_BLOCK_32, ~170 ms) */
#define AU_TARGET    8              /* bloques por delante que intento mantener (~43 ms) */
#define AU_PRIME     6              /* tras un corte, relleno con silencio hasta este nivel */
#define AU_URGENT    2              /* si no hay audio, mantengo al menos esto con silencio */
#define AU_QCAP      8192           /* cola de entrada (cuadros estereo) */
#define AU_OUT_RATE  48000.0
#define AU_FILL_T    (AU_TARGET * AU_BLK + 800.0)

static int      g_au_on;                    /* 1 = audio activo */
static double   g_au_inrate = 48000.0;      /* frecuencia que entrega el nucleo */
static int16_t  g_aq[AU_QCAP * 2];
static unsigned g_aq_head, g_aq_cnt;        /* cola circular de entrada (cuadros) */
static double   g_aq_pos;                   /* posicion fraccionaria respecto a head */
static float    g_au_ll, g_au_lr;           /* ultima muestra emitida (para el fundido a silencio) */
static int      g_au_queued;                /* bloques escritos por delante de la lectura */
static int      g_au_reprime;
static uint32_t g_au_lastrd, g_au_lastpush, g_au_lastpump, g_au_logt;
static uint32_t g_au_blocks, g_au_under, g_au_drop;
static double   g_au_step_s = 1.0;          /* paso suavizado */

#ifdef NESNET_PS3
static u32  g_au_port;
static audioPortConfig g_au_cfg;
static sys_event_queue_t g_au_q;
static sys_ipc_key_t g_au_key;
static int  g_au_init, g_au_portok, g_au_qok, g_au_started;

static uint32_t au_hw_readidx(void) { return (uint32_t)(*(volatile u64 *)(uintptr_t)g_au_cfg.readIndex); }
static float *au_hw_block(uint32_t i) { return (float *)(uintptr_t)g_au_cfg.audioDataStart + (size_t)i * AU_BLK * 2; }
static void au_hw_poll(void) { if (g_au_qok) sysEventQueueDrain(g_au_q); }

static void au_close(void)
{
    g_au_on = 0;
    if (g_au_started) { audioPortStop(g_au_port); g_au_started = 0; }
    if (g_au_qok) { audioRemoveNotifyEventQueue(g_au_key); sysEventQueueDestroy(g_au_q, 0); g_au_qok = 0; }
    if (g_au_portok) { audioPortClose(g_au_port); g_au_portok = 0; }
    if (g_au_init) { audioQuit(); g_au_init = 0; }
}
#else
static uint32_t au_hw_readidx(void) { return 0; }
static float *au_hw_block(uint32_t i) { static float b[AU_BLK * 2]; (void)i; return b; }
static void au_hw_poll(void) {}
static void au_close(void) {}
#endif

/* AUDIO-GENERIC-BEGIN */
static int      g_volume = 100;        /* v1.9: volumen 0..100 (100 = igual que siempre) */
/* Arcade Station: la app va clavada a 60 cuadros/s (vsync) y el nucleo entrega SIEMPRE el mismo numero de muestras por cuadro,
 * aunque declare 59,6 cuadros/s (CPS1: 31831 Hz declarados, pero llegan 32040). Esa diferencia de 0,66 % se comia casi todo el
 * ajuste fino de +-1 % y dejaba que la cola de audio creciera (retraso). Se corrige la frecuencia de entrada a lo que de verdad llega. */
static void au_set_inrate(double sample_rate, double fps)
{
    if (sample_rate < 8000.0 || sample_rate > 96000.0) return;
    g_au_inrate = sample_rate;
    if (fps >= 40.0 && fps <= 70.0) g_au_inrate = sample_rate * (60.0 / fps);
}

static void au_reset(void)
{
    g_aq_head = 0; g_aq_cnt = 0; g_aq_pos = 0.0;
}

/* Recibe PCM del nucleo (se llama desde retro_run) */
static void au_push(const int16_t *d, size_t frames)
{
    size_t i;
    if (!g_au_on || !frames) return;
    g_au_lastpush = now_ms();
    if (frames > AU_QCAP) { d += (frames - AU_QCAP) * 2; frames = AU_QCAP; }
    if (g_aq_cnt + frames > AU_QCAP) {              /* la cola se llena: descarto lo mas viejo */
        unsigned over = (unsigned)(g_aq_cnt + frames - AU_QCAP);
        if (over > g_aq_cnt) over = g_aq_cnt;
        g_aq_head = (g_aq_head + over) % AU_QCAP;
        g_aq_cnt -= over;
        g_aq_pos -= (double)over;
        if (g_aq_pos < 0.0) g_aq_pos = 0.0;
        g_au_drop++;
    }
    for (i = 0; i < frames; i++) {
        unsigned w = (g_aq_head + g_aq_cnt) % AU_QCAP;
        if (g_volume >= 100) {                      /* volumen normal: las muestras pasan sin tocarse (igual que antes) */
            g_aq[w * 2]     = d[i * 2];
            g_aq[w * 2 + 1] = d[i * 2 + 1];
        } else {                                    /* v1.9: volumen bajado desde OPCIONES > SONIDO */
            g_aq[w * 2]     = (int16_t)((int)d[i * 2]     * g_volume / 100);
            g_aq[w * 2 + 1] = (int16_t)((int)d[i * 2 + 1] * g_volume / 100);
        }
        g_aq_cnt++;
    }
}

/* Paso de lectura de la entrada por cada muestra de salida: nominal +- 1 %, segun cuanto audio
 * hay acumulado (en la cola de salida + en la de entrada) respecto al objetivo. */
static double au_calc_step(void)
{
    double base = g_au_inrate / AU_OUT_RATE;
    double fill = (double)g_au_queued * AU_BLK + (double)g_aq_cnt / base;
    double e = (fill - AU_FILL_T) / AU_FILL_T;
    double s;
    if (e > 1.0) e = 1.0; else if (e < -1.0) e = -1.0;
    s = base * (1.0 + 0.01 * e);
    g_au_step_s += 0.05 * (s - g_au_step_s);
    if (g_au_step_s > base * 1.01) g_au_step_s = base * 1.01;
    if (g_au_step_s < base * 0.99) g_au_step_s = base * 0.99;
    return g_au_step_s;
}

static void au_silence_block(float *o)
{
    int i;
    for (i = 0; i < AU_BLK; i++) {
        g_au_ll *= 0.995f; g_au_lr *= 0.995f;
        if (g_au_ll > -1e-6f && g_au_ll < 1e-6f) g_au_ll = 0.0f;
        if (g_au_lr > -1e-6f && g_au_lr < 1e-6f) g_au_lr = 0.0f;
        o[i * 2] = g_au_ll; o[i * 2 + 1] = g_au_lr;
    }
}

/* Un bloque real: interpolacion lineal entre muestras de entrada. El que llama garantiza que
 * hay suficiente entrada (au_need). */
static int au_need(double step) { return (int)(g_aq_pos + AU_BLK * step) + 4; }   /* +4: la interpolacion cubica mira 1 muestra antes y 2 despues */

static void au_real_block(float *o, double step)
{
    int i, n;
    for (i = 0; i < AU_BLK; i++) {
        int k = (int)g_aq_pos;
        double t = g_aq_pos - (double)k, t2 = t * t, t3 = t2 * t;
        unsigned ia = (g_aq_head + (unsigned)(k > 0 ? k - 1 : 0)) % AU_QCAP;
        unsigned ib = (g_aq_head + (unsigned)k) % AU_QCAP;
        unsigned ic = (g_aq_head + (unsigned)k + 1) % AU_QCAP;
        unsigned id = (g_aq_head + (unsigned)k + 2) % AU_QCAP;
        double l, r;
        /* interpolacion cubica (Catmull-Rom) en vez de lineal: menos "suciedad" al subir de 32 kHz a 48 kHz */
        {
            double p0 = g_aq[ia * 2], p1 = g_aq[ib * 2], p2 = g_aq[ic * 2], p3 = g_aq[id * 2];
            l = 0.5 * (2.0 * p1 + (p2 - p0) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 + (3.0 * p1 - p0 - 3.0 * p2 + p3) * t3);
            p0 = g_aq[ia * 2 + 1]; p1 = g_aq[ib * 2 + 1]; p2 = g_aq[ic * 2 + 1]; p3 = g_aq[id * 2 + 1];
            r = 0.5 * (2.0 * p1 + (p2 - p0) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 + (3.0 * p1 - p0 - 3.0 * p2 + p3) * t3);
        }
        if (l > 32767.0) l = 32767.0; else if (l < -32768.0) l = -32768.0;
        if (r > 32767.0) r = 32767.0; else if (r < -32768.0) r = -32768.0;
        g_au_ll = (float)(l / 32768.0); g_au_lr = (float)(r / 32768.0);
        o[i * 2] = g_au_ll; o[i * 2 + 1] = g_au_lr;
        g_aq_pos += step;
    }
    n = (int)g_aq_pos;
    if (n > (int)g_aq_cnt) n = (int)g_aq_cnt;
    g_aq_head = (g_aq_head + (unsigned)n) % AU_QCAP;
    g_aq_cnt -= (unsigned)n;
    g_aq_pos -= (double)n;
    if (g_aq_pos < 0.0) g_aq_pos = 0.0;
}

/* Se llama muy seguido (desde pump()): repone bloques en el anillo segun avance el reloj de audio */
static void au_pump(void)
{
    uint32_t rd, now;
    int d, idle, guard = 0;
    double step;
    if (!g_au_on) return;
    now = now_ms();
    idle = (uint32_t)(now - g_au_lastpush) > 500;
    au_hw_poll();
    if (g_au_lastpump && (uint32_t)(now - g_au_lastpump) > 120) {
        /* el programa estuvo detenido: el anillo pudo dar la vuelta; lo dejo en silencio y empiezo de nuevo */
        int j;
        for (j = 0; j < AU_NBLK; j++) memset(au_hw_block((uint32_t)j), 0, AU_BLK * 2 * sizeof(float));
        g_au_queued = 0; g_au_reprime = 1;
        g_au_lastrd = au_hw_readidx() % AU_NBLK;
        if (!idle) g_au_under++;
    }
    g_au_lastpump = now;
    rd = au_hw_readidx() % AU_NBLK;
    d = (int)((rd + AU_NBLK - g_au_lastrd) % AU_NBLK);
    if (d > 0) {
        int j;
        for (j = 0; j < d; j++) memset(au_hw_block((g_au_lastrd + (uint32_t)j) % AU_NBLK), 0, AU_BLK * 2 * sizeof(float));
        if (d > g_au_queued) { if (!idle) { g_au_under++; g_au_reprime = 1; } g_au_queued = 0; }
        else g_au_queued -= d;
        g_au_lastrd = rd;
    }
    step = au_calc_step();
    while (g_au_queued < AU_TARGET && guard++ < AU_NBLK) {
        float *o = au_hw_block((rd + 1 + (uint32_t)g_au_queued) % AU_NBLK);
        if (g_au_reprime && g_au_queued < AU_PRIME) {
            au_silence_block(o);
            if (g_au_queued + 1 >= AU_PRIME) g_au_reprime = 0;
        } else if ((int)g_aq_cnt >= au_need(step)) {
            au_real_block(o, step);
            g_au_blocks++;
        } else if (g_au_queued < AU_URGENT) {
            au_silence_block(o);
        } else break;
        g_au_queued++;
    }
    if (!idle && (uint32_t)(now - g_au_logt) >= 2000) {
        g_au_logt = now;
        logf_("audio: bloques=%u cortes=%u descartes=%u cola=%d entrada=%u paso=%d/10000",
              (unsigned)g_au_blocks, (unsigned)g_au_under, (unsigned)g_au_drop,
              g_au_queued, (unsigned)g_aq_cnt, (int)(step * 10000.0));
    }
}
/* AUDIO-GENERIC-END */

#ifdef NESNET_PS3
static void au_open(void)
{
    audioPortParam pr;
    s32 r;
    g_au_on = 0;
    if (!g_cfg.audio) { logf_("audio: apagado (audio=off o R2 al abrir)"); return; }
    logf_("audio: llamando a audioInit...");
    r = audioInit();
    logf_("audio: audioInit=%08x", (unsigned)r);
    if (r != 0) return;
    g_au_init = 1;
    memset(&pr, 0, sizeof pr);
    pr.numChannels = AUDIO_PORT_2CH;
    pr.numBlocks   = AUDIO_BLOCK_32;
    pr.attrib      = AUDIO_PORT_INITLEVEL;
    pr.level       = 1.0f;
    logf_("audio: llamando a audioPortOpen...");
    r = audioPortOpen(&pr, &g_au_port);
    logf_("audio: audioPortOpen=%08x puerto=%u", (unsigned)r, (unsigned)g_au_port);
    if (r != 0) { au_close(); return; }
    g_au_portok = 1;
    r = audioGetPortConfig(g_au_port, &g_au_cfg);
    logf_("audio: config=%08x bloques=%u canales=%u indice=%08x datos=%08x", (unsigned)r,
          (unsigned)g_au_cfg.numBlocks, (unsigned)g_au_cfg.channelCount,
          (unsigned)g_au_cfg.readIndex, (unsigned)g_au_cfg.audioDataStart);
    if (r != 0 || g_au_cfg.numBlocks != AU_NBLK || g_au_cfg.channelCount != 2 ||
        !g_au_cfg.readIndex || !g_au_cfg.audioDataStart) {
        logf_("AVISO: el puerto de audio no tiene el formato esperado; sigo sin sonido");
        au_close(); return;
    }
    r = audioCreateNotifyEventQueue(&g_au_q, &g_au_key);
    logf_("audio: cola de eventos=%08x", (unsigned)r);
    if (r == 0) {
        g_au_qok = 1;
        r = audioSetNotifyEventQueue(g_au_key);
        logf_("audio: audioSetNotifyEventQueue=%08x", (unsigned)r);
        sysEventQueueDrain(g_au_q);
    } else {
        logf_("AVISO: sin cola de eventos de audio; sigo leyendo el indice directamente");
    }
    { int i; for (i = 0; i < AU_NBLK; i++) memset(au_hw_block((uint32_t)i), 0, AU_BLK * 2 * sizeof(float)); }
    g_au_lastrd = au_hw_readidx() % AU_NBLK;
    g_au_queued = 0; g_au_reprime = 1; g_au_lastpush = 0; g_au_lastpump = 0;
    logf_("audio: llamando a audioPortStart...");
    r = audioPortStart(g_au_port);
    logf_("audio: audioPortStart=%08x", (unsigned)r);
    if (r != 0) { au_close(); return; }
    g_au_started = 1;
    g_au_on = 1;
    logf_("audio: listo (anillo de %d bloques)", AU_NBLK);
}
#else
static void au_open(void) {}
#endif

/* v1.8.1: palanca izquierda como cruz. Los ejes van de 0 a 255 con el centro cerca de 128. La zona muerta es
 * amplia (50 de 128) para que un stick flojo o con deriva no mande movimientos sueltos. Funcion pura: se
 * puede probar en PC. Devuelve -1 (izq/arriba), 0 (centro) o +1 (der/abajo) para cada eje. */
#define STICK_DEAD 50
static int stick_axis(unsigned v)
{
    if (v < 128u - STICK_DEAD) return -1;
    if (v > 128u + STICK_DEAD) return 1;
    return 0;
}
static uint16_t stick_nes_bits(unsigned h, unsigned v)
{
    uint16_t b = 0;
    int ax = stick_axis(h), ay = stick_axis(v);
    if (ax < 0) b |= NB_LEFT;
    if (ax > 0) b |= NB_RIGHT;
    if (ay < 0) b |= NB_UP;
    if (ay > 0) b |= NB_DOWN;
    return b;
}

/* ------------------------------------------------------------------ */
/* v1.8.2: OPCIONES - mapeo de botones, turbo y pantalla               */
/* Todo esto es LOCAL. Por la red sigue viajando el estado de los botones del NES (el mensaje no cambia),   */
/* asi que el mapeo de cada jugador no hace falta enviarlo: cada consola traduce sus propios botones antes. */
/* Las funciones de esta seccion son puras (no tocan la PS3), asi que se prueban tal cual en PC.             */
/* ------------------------------------------------------------------ */
enum { PB_CROSS, PB_CIRCLE, PB_SQUARE, PB_TRIANGLE, PB_L1, PB_R1, PB_L2, PB_R2, PB_L3, PB_R3, PB_START, PB_SELECT, PB_COUNT };
#define PB_NONE   (-1)
#define PB_BIT(b) (1u << (b))
enum { MA_A, MA_B, MA_SEL, MA_START, MA_TA, MA_TB, MA_COUNT };    /* MA_TA / MA_TB = turbo A / turbo B */

static const char *const pb_names[PB_COUNT] = { "X", "O", "CUADRADO", "TRIANGULO", "L1", "R1", "L2", "R2", "L3", "R3", "START", "SELECT" };
static const char *const ma_names[MA_COUNT] = { "A", "B", "SELECT", "START", "TURBO A", "TURBO B" };

/* boton de la PS3 que dispara cada boton del NES, por mando. Los valores por defecto son EXACTAMENTE los de la v1.8.1
 * (O = A, X = B, SELECT, START); los turbo empiezan sin asignar. Las direcciones (cruz y palanca) no se remapean. */
static int g_map[2][MA_COUNT] = {
    { PB_CIRCLE, PB_CROSS, PB_SELECT, PB_START, PB_NONE, PB_NONE },
    { PB_CIRCLE, PB_CROSS, PB_SELECT, PB_START, PB_NONE, PB_NONE }
};
static unsigned g_turbo_cnt[2][2];     /* cuadros seguidos que lleva apretado cada turbo (A, B) de cada mando */
static int      g_screen;              /* 0 = original (pixeles cuadrados, como hasta ahora), 1 = 4:3 real, 2 = 16:9 estirada */
/* v1.9.3: la opcion de pantalla (g_screen) es solo para la imagen del JUEGO. Los menus y avisos (todo lo que se dibuja fuera de una
 * partida) usan siempre MENU_SCREEN, asi no cambian de tamano al mover ORIGINAL / 4:3 / 16:9. El ajuste de imagen (tamano y
 * desplazamiento, para el overscan de la TV) sigue valiendo para todo. Usan la pantalla elegida: session_loop / session_loop4
 * (g_ingame = 1 mientras corren; el menu de R2 sobre el juego cuenta como juego) y la pantalla AJUSTE DE IMAGEN (g_adjusting = 1),
 * que tiene que mostrar el marco con la geometria real del juego. */
#define MENU_SCREEN 2                  /* 2 = 16:9 a pantalla completa (1 = 4:3 centrado, 0 = original) */
static int      g_ingame;
static int      g_adjusting;
static int      g_netinfo;             /* v1.9: info de red en partida: 0 = solo al inicio (como siempre), 1 = siempre, 2 = no */
static char     g_last_rom[ROM_NAMELEN];   /* v1.9: nombre del archivo de la ultima ROM elegida */
static char     g_last_code[16];           /* v1.9.3: ultimo codigo de sala escrito al UNIRSE (10 digitos, o vacio) */
static int      g_showfps;             /* v1.9.2: 1 = FPS abajo a la derecha (en SOLO y online), 0 = no */
/* v1.9.2: ajuste de imagen (OPCIONES > AJUSTE DE IMAGEN): tamano en % de la imagen de la pantalla elegida y
 * desplazamiento en pixeles de la TV. Con los valores de siempre (100, 0, 0) se usa el dibujo de antes, sin cambios. */
#define IMG_SIZE_MIN   70
#define IMG_SHIFT_MAX  60
static int      g_img_size = 100;
static int      g_img_dx, g_img_dy;

static void map_defaults(int p)
{
    g_map[p][MA_A] = PB_CIRCLE; g_map[p][MA_B] = PB_CROSS;
    g_map[p][MA_SEL] = PB_SELECT; g_map[p][MA_START] = PB_START;
    g_map[p][MA_TA] = PB_NONE; g_map[p][MA_TB] = PB_NONE;
    g_turbo_cnt[p][0] = g_turbo_cnt[p][1] = 0;
}

/* Convierte los botones de PS3 apretados (mascara PB_BIT) y las direcciones ya calculadas (cruz + palanca) en el byte NES.
 * Turbo: mientras el boton esta apretado alterna 2 cuadros apretado / 2 sueltos (15 pulsos por segundo), empezando apretado. */
static uint16_t map_nes_bits(int port, uint32_t btns, uint16_t dirs)
{
    const int *m = g_map[port & 1];
    uint16_t b = (uint16_t)(dirs & (NB_UP | NB_DOWN | NB_LEFT | NB_RIGHT));
    int t;
    if (m[MA_A]     >= 0 && (btns & PB_BIT(m[MA_A])))     b |= NB_A;
    if (m[MA_B]     >= 0 && (btns & PB_BIT(m[MA_B])))     b |= NB_B;
    if (m[MA_SEL]   >= 0 && (btns & PB_BIT(m[MA_SEL])))   b |= NB_SEL;
    if (m[MA_START] >= 0 && (btns & PB_BIT(m[MA_START]))) b |= NB_START;
    for (t = 0; t < 2; t++) {
        int id = m[MA_TA + t];
        unsigned *c = &g_turbo_cnt[port & 1][t];
        if (id >= 0 && (btns & PB_BIT(id))) {
            if (((*c >> 1) & 1u) == 0) b |= (t == 0 ? NB_A : NB_B);
            *c = (*c + 1) & 0xFFFFu;
        } else {
            *c = 0;
        }
    }
    if ((b & NB_UP) && (b & NB_DOWN))    b &= (uint16_t)~NB_DOWN;
    if ((b & NB_LEFT) && (b & NB_RIGHT)) b &= (uint16_t)~NB_RIGHT;
    return b;
}

/* ¿Puede la accion 'act' del mando 'port' tomar el boton 'btn'? Reglas: solo los turbo pueden quedar sin boton;
 * R3 y L2 del mando 1 estan reservados (R3 abre el menu, L2 hace la captura); un boton que ya usa OTRA fila del mismo mando no se puede tomar
 * (nunca se mueve un boton ajeno: para cambiarlo, primero se libera en su fila). */
static int map_allowed(int port, int act, int btn)
{
    int a;
    if (btn == PB_NONE) return act >= MA_TA;
    if (btn < 0 || btn >= PB_COUNT) return 0;
    if (port == 0 && (btn == PB_R3 || btn == PB_L2)) return 0;
    for (a = 0; a < MA_COUNT; a++)
        if (a != act && g_map[port][a] == btn) return 0;
    return 1;
}

static void map_assign(int port, int act, int btn) { g_map[port][act] = btn; }

/* Pasa al siguiente (dir = +1) o anterior (dir = -1) boton permitido, saltando los que no se pueden tomar.
 * Orden: SIN ASIGNAR (solo turbo), X, O, CUADRADO, TRIANGULO, L1, R1, L2, R2, L3, R3, START, SELECT */
static void map_cycle(int port, int act, int dir)
{
    int n = PB_COUNT + 1, cur = g_map[port][act] + 1, i;
    for (i = 1; i <= n; i++) {
        int idx = (((cur + dir * i) % n) + n) % n;
        int btn = idx - 1;
        if (map_allowed(port, act, btn)) { map_assign(port, act, btn); return; }
    }
}

/* Comprueba un mapeo leido de archivo: botones normales siempre asignados, sin repetidos, sin R3 en el mando 1 */
static int map_valid(const int *m, int port)
{
    int a, c;
    for (a = 0; a < MA_COUNT; a++) {
        if (m[a] == PB_NONE) { if (a < MA_TA) return 0; continue; }
        if (m[a] < 0 || m[a] >= PB_COUNT) return 0;
        if (port == 0 && (m[a] == PB_R3 || m[a] == PB_L2)) return 0;
        for (c = a + 1; c < MA_COUNT; c++) if (m[c] == m[a]) return 0;
    }
    return 1;
}

/* Pantalla: ancho de salida (en pixeles de la TV) para un alto de salida 'oh'.
 *   0 original  = SCR_W * z  (pixeles cuadrados, 768x720 en 720p: lo de siempre)
 *   1 4:3 real  = oh * 4 / 3 (960x720 en 720p: columnas repetidas 3 o 4 veces, el 3,75 no es entero)
 *   2 16:9      = oh * 16 / 9 (1280x720 en 720p: cada columna x5 y cada fila x3, todo entero) */
#define COLMAP_MAX 2048
static const char *const screen_names[3] = { "ORIGINAL", "4:3 REAL", "16:9 ESTIRADA" };
static int screen_out_w(int mode, int dw, int oh, int z)
{
    int w;
    if (mode == 1) w = oh * 4 / 3;
    else if (mode == 2) w = oh * 16 / 9;
    else w = SCR_W * z;
    if (w > dw) w = dw;
    if (w > COLMAP_MAX) w = COLMAP_MAX;
    return w;
}

/* v1.9.9: lo que queda fuera de la imagen (barras de ORIGINAL y 4:3) se rellena con el fondo mosaico de NesNet en vez de negro.
 * Solo si g_bars_mosaic = 1 (lo pone la PS3 con salida de 1280x720); en las pruebas queda en negro como siempre. */
static int g_bars_mosaic;
static void bars_fill(uint32_t *base, int dw, int x0, int y0, int x1, int y1)     /* rectangulo [x0,x1) x [y0,y1) */
{
    int y;
    if (x1 <= x0 || y1 <= y0) return;
    if (g_bars_mosaic && dw == HR_W && x0 >= 0 && x1 <= HR_W && y0 >= 0 && y1 <= HR_H) {
        for (y = y0; y < y1; y++) memcpy(base + (size_t)y * dw + x0, g_bg + (size_t)y * HR_W + x0, (size_t)(x1 - x0) * 4);
    } else {
        for (y = y0; y < y1; y++) memset(base + (size_t)y * dw + x0, 0, (size_t)(x1 - x0) * 4);
    }
}

/* Fondo del menu R3 en partida: la imagen del juego oscurecida con la proporcion de la pantalla elegida (ORIGINAL / 4:3 / 16:9,
 * sin estirarla) y el mosaico a los lados, mas un cuadro al centro. v1.9.9: antes siempre se estiraba a 16:9. */
static void hr_backdrop(const uint32_t *fb)
{
    int x, y, ow, ox;
    hr_init();
    if (g_bd_dirty) {
        ow = screen_out_w(g_screen, HR_W, HR_H, 3);
        ox = (HR_W - ow) / 2;
        memcpy(g_hr, g_bg, sizeof g_hr);
        for (y = 0; y < HR_H; y++) {
            const uint32_t *src = fb + (y / 3) * SCR_W;
            for (x = 0; x < ow; x++) {
                uint32_t c = src[x * SCR_W / ow];
                uint32_t v = ((((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255)) / 3) >> 2;     /* gris al 25 % */
                g_hr[y * HR_W + ox + x] = 0xFF000000u | (v << 16) | (v << 8) | v | 0x00020410u;
            }
        }
        hr_panel(290, 120, 990, 610);
        memcpy(g_bd, g_hr, sizeof g_hr);
        g_bd_dirty = 0;
    }
    memcpy(g_hr, g_bd, sizeof g_hr);
    g_hr_on = 1;
}

/* Dibuja el lienzo 256x240 en el buffer de pantalla 'base' (dw x dh, ARGB) centrado, escalado segun 'mode' (vertical entero z).
 * Cada 4 cuadros, DE CADA BUFFER, borra lo que queda fuera de la imagen (residuo del XMB / modo anterior); la imagen se repinta siempre.
 * fcnt = contador de cuadros de ese buffer. Version unica para la PS3 y para las pruebas de PC. */
static void canvas_blit_cpu(uint32_t *base, int dw, int dh, int mode, int z, const uint32_t *fb, unsigned *fcnt)
{
    static uint32_t row[COLMAP_MAX];
    static int cm[COLMAP_MAX];
    static int l_ow = -1;
    int oh = SCR_H * z, ow = screen_out_w(mode, dw, oh, z);
    int ox = (dw - ow) / 2, oy = (dh - oh) / 2, x, y, k;
    uint32_t *dst;
    if (z < 1 || ow < 1 || ox < 0 || oy < 0 || oy + oh > dh) return;
    if (ow != l_ow) { for (x = 0; x < ow; x++) cm[x] = x * SCR_W / ow; l_ow = ow; }
    if (((*fcnt)++ & 3) == 0) {
        int below = dh - oy - oh;
        if (oy > 0) bars_fill(base, dw, 0, 0, dw, oy);
        if (below > 0) bars_fill(base, dw, 0, oy + oh, dw, dh);
        if (ox > 0) {
            int right = dw - ox - ow;
            bars_fill(base, dw, 0, oy, ox, oy + oh);
            if (right > 0) bars_fill(base, dw, ox + ow, oy, dw, oy + oh);
        }
    }
    dst = base + (size_t)oy * dw + ox;
    for (y = 0; y < SCR_H; y++) {
        const uint32_t *src = fb + y * SCR_W;
        for (x = 0; x < ow; x++) row[x] = src[cm[x]];
        for (k = 0; k < z; k++) { memcpy(dst, row, (size_t)ow * 4); dst += dw; }
    }
}

/* v1.9.2: dibujo con ajuste de imagen (tamano en % del lienzo de la pantalla elegida y desplazamiento en pixeles de la TV).
 * Con size = 100 y dx = dy = 0 da EXACTAMENTE la misma imagen que canvas_blit_cpu (lo comprueba screentest); la PS3 sigue
 * usando canvas_blit_cpu mientras no se toque el ajuste. Si la imagen se sale por un borde se recorta: nunca escribe fuera.
 * Filas y columnas por vecino mas cercano (como el resto). Borra todo el buffer cuando cambia la posicion o el tamano
 * (2 veces seguidas: un buffer de pantalla cada vez) y despues cada 4 cuadros de cada buffer, como canvas_blit_cpu. */
static void canvas_blit_adj(uint32_t *base, int dw, int dh, int mode, int z, const uint32_t *fb, unsigned *fcnt,
                            int size, int dx, int dy)
{
    static uint32_t row[COLMAP_MAX];
    static int cm[COLMAP_MAX];
    static int l_ow = -1;
    static int geo[4] = { -1, -1, -1, -1 }, force_clear;
    int oh0 = SCR_H * z, ow0 = screen_out_w(mode, dw, oh0, z);
    int ow, oh, ox, oy, x0, x1, y0, y1, y, x, last_sy = -1;
    if (z < 1 || ow0 < 1) return;
    if (size < IMG_SIZE_MIN) size = IMG_SIZE_MIN;
    if (size > 100) size = 100;
    ow = ow0 * size / 100; oh = oh0 * size / 100;
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    ox = (dw - ow) / 2 + dx; oy = (dh - oh) / 2 + dy;
    if (ow != l_ow) { for (x = 0; x < ow; x++) cm[x] = x * SCR_W / ow; l_ow = ow; }
    if (ox != geo[0] || oy != geo[1] || ow != geo[2] || oh != geo[3]) { geo[0] = ox; geo[1] = oy; geo[2] = ow; geo[3] = oh; force_clear = 2; }
    if (force_clear > 0) { force_clear--; (*fcnt)++; bars_fill(base, dw, 0, 0, dw, dh); }
    else if (((*fcnt)++ & 3) == 0) bars_fill(base, dw, 0, 0, dw, dh);
    x0 = ox < 0 ? 0 : ox;  x1 = ox + ow > dw ? dw : ox + ow;
    y0 = oy < 0 ? 0 : oy;  y1 = oy + oh > dh ? dh : oy + oh;
    if (x1 <= x0 || y1 <= y0) return;
    for (y = y0; y < y1; y++) {
        int sy = (y - oy) * SCR_H / oh;
        if (sy != last_sy) {
            const uint32_t *src = fb + sy * SCR_W;
            for (x = x0; x < x1; x++) row[x - x0] = src[cm[x - ox]];
            last_sy = sy;
        }
        memcpy(base + (size_t)y * dw + x0, row, (size_t)(x1 - x0) * 4);
    }
}

/* Guardado de las opciones: USRDIR/opciones.txt (aparte de config.txt, que el instalador puede reemplazar) */
static void opts_defaults(void)
{
    map_defaults(0); map_defaults(1);
    g_screen = 0;
    g_volume = 100;
    g_netinfo = 0;
    g_showfps = 0;
    g_img_size = 100; g_img_dx = 0; g_img_dy = 0;
    g_last_rom[0] = 0;
    g_last_code[0] = 0;
}

static void opts_save(void)
{
    char path[300], tmp[300];
    FILE *f;
    int p;
    snprintf(path, sizeof path, "%s/opciones.txt", g_dir);
    snprintf(tmp, sizeof tmp, "%s/opciones.tmp", g_dir);
    f = fopen(tmp, "w");
    if (!f) { logf_("AVISO: no pude guardar opciones.txt"); return; }
    fprintf(f, "# Arcade Station - opciones (las cambia el menu OPCIONES)\n");
    fprintf(f, "screen=%d\n", g_screen);
    fprintf(f, "volume=%d\n", g_volume);
    fprintf(f, "netinfo=%d\n", g_netinfo);
    fprintf(f, "fps=%d\n", g_showfps);
    fprintf(f, "imgsize=%d\n", g_img_size);
    fprintf(f, "imgx=%d\n", g_img_dx);
    fprintf(f, "imgy=%d\n", g_img_dy);
    if (g_last_rom[0]) fprintf(f, "rom=%s\n", g_last_rom);
    if (g_last_code[0]) fprintf(f, "code=%s\n", g_last_code);
    for (p = 0; p < 2; p++)
        fprintf(f, "pad%d=%d,%d,%d,%d,%d,%d\n", p + 1, g_map[p][0], g_map[p][1], g_map[p][2], g_map[p][3], g_map[p][4], g_map[p][5]);
    fclose(f);
    remove(path);
    if (rename(tmp, path) != 0) logf_("AVISO: no pude renombrar opciones.tmp");
}

static void opts_load(void)
{
    char path[300], line[200];
    FILE *f;
    opts_defaults();
    snprintf(path, sizeof path, "%s/opciones.txt", g_dir);
    f = fopen(path, "r");
    if (!f) { logf_("sin opciones.txt: pantalla y mandos por defecto"); return; }
    while (fgets(line, sizeof line, f)) {
        int v[MA_COUNT], p;
        if (line[0] == '#') continue;
        if (sscanf(line, "screen=%d", &v[0]) == 1) {
            if (v[0] >= 0 && v[0] <= 2) g_screen = v[0];
        } else if (sscanf(line, "volume=%d", &v[0]) == 1) {
            if (v[0] >= 0 && v[0] <= 100) g_volume = v[0];
        } else if (sscanf(line, "netinfo=%d", &v[0]) == 1) {
            if (v[0] >= 0 && v[0] <= 2) g_netinfo = v[0];
        } else if (sscanf(line, "fps=%d", &v[0]) == 1) {
            if (v[0] == 0 || v[0] == 1) g_showfps = v[0];
        } else if (sscanf(line, "imgsize=%d", &v[0]) == 1) {
            if (v[0] >= IMG_SIZE_MIN && v[0] <= 100) g_img_size = v[0];
        } else if (sscanf(line, "imgx=%d", &v[0]) == 1) {
            if (v[0] >= -IMG_SHIFT_MAX && v[0] <= IMG_SHIFT_MAX) g_img_dx = v[0];
        } else if (sscanf(line, "imgy=%d", &v[0]) == 1) {
            if (v[0] >= -IMG_SHIFT_MAX && v[0] <= IMG_SHIFT_MAX) g_img_dy = v[0];
        } else if (!strncmp(line, "rom=", 4)) {
            size_t n = strlen(line + 4);
            while (n > 0 && (line[4 + n - 1] == '\n' || line[4 + n - 1] == '\r')) n--;
            if (n > 0 && n < ROM_NAMELEN && !memchr(line + 4, '/', n) && line[4] != '.') { memcpy(g_last_rom, line + 4, n); g_last_rom[n] = 0; }
        } else if (!strncmp(line, "code=", 5)) {
            /* v1.9.3: solo se acepta si son exactamente 10 digitos */
            size_t n = strlen(line + 5), k;
            int okc;
            while (n > 0 && (line[5 + n - 1] == '\n' || line[5 + n - 1] == '\r')) n--;
            okc = (n == 10);
            for (k = 0; okc && k < n; k++) if (line[5 + k] < '0' || line[5 + k] > '9') okc = 0;
            if (okc) { memcpy(g_last_code, line + 5, 10); g_last_code[10] = 0; }
        } else if ((line[0] == 'p' && line[1] == 'a' && line[2] == 'd' && (line[3] == '1' || line[3] == '2') && line[4] == '=') &&
                   sscanf(line + 5, "%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
            p = line[3] - '1';
            if (map_valid(v, p)) { int a; for (a = 0; a < MA_COUNT; a++) g_map[p][a] = v[a]; }
            else logf_("AVISO: opciones.txt: mapeo del mando %d no valido, uso el de por defecto", p + 1);
        }
    }
    fclose(f);
    logf_("opciones: pantalla=%d volumen=%d info_red=%d fps=%d imagen=%d%%,%d,%d  mando1=%d,%d,%d,%d,%d,%d  mando2=%d,%d,%d,%d,%d,%d", g_screen, g_volume, g_netinfo,
          g_showfps, g_img_size, g_img_dx, g_img_dy,
          g_map[0][0], g_map[0][1], g_map[0][2], g_map[0][3], g_map[0][4], g_map[0][5],
          g_map[1][0], g_map[1][1], g_map[1][2], g_map[1][3], g_map[1][4], g_map[1][5]);
    if (g_last_rom[0]) logf_("opciones: ultima ROM = %s", g_last_rom);
}

#ifdef NESNET_PS3

#define CB_SIZE   0x100000
#define HOST_SIZE (32 * 1024 * 1024)
#define GCM_LABEL_INDEX 255
#define LABEL_TEX0      253          /* 253 y 254: una por textura */

static gcmContextData *g_ctx;
static u32 g_dw, g_dh, g_pitch, g_off[2], g_dpitch, g_doff;
static u32 g_cur, g_first = 1, g_label = 1;
static u32 *g_cbuf[2], *g_dbuf;
static u32 *g_tex[2];
static u32  g_texoff[2];
static int  g_texsel;
static u32  g_texval[2];
static int  g_zoom = 3;
static padData g_pd[4];
static uint16_t g_ui_prev;

static void gpu_wait_finish(void)
{
    rsxSetWriteBackendLabel(g_ctx, GCM_LABEL_INDEX, g_label);
    rsxFlushBuffer(g_ctx);
    {
        int guard = 0;
        while (*(vu32 *)gcmGetLabelAddress(GCM_LABEL_INDEX) != g_label) {
            usleep(30);
            if ((++guard & 0x3FF) == 0) pump();
            if (guard > 100000) { logf_("AVISO: gpu_wait_finish agotó el tiempo (3 s)"); break; }
        }
    }
    ++g_label;
}

static void gpu_wait_idle(void)
{
    rsxSetWriteBackendLabel(g_ctx, GCM_LABEL_INDEX, g_label);
    rsxSetWaitLabel(g_ctx, GCM_LABEL_INDEX, g_label);
    ++g_label;
    gpu_wait_finish();
}

static void gpu_set_target(u32 idx)
{
    gcmSurface sf;
    memset(&sf, 0, sizeof sf);
    sf.colorFormat      = GCM_SURFACE_X8R8G8B8;
    sf.colorTarget      = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX;
    sf.colorOffset[0]   = g_off[idx];
    sf.colorPitch[0]    = g_pitch;
    sf.colorLocation[1] = GCM_LOCATION_RSX;
    sf.colorLocation[2] = GCM_LOCATION_RSX;
    sf.colorLocation[3] = GCM_LOCATION_RSX;
    sf.colorOffset[1]   = 0;
    sf.colorOffset[2]   = 0;
    sf.colorOffset[3]   = 0;
    sf.colorPitch[1]    = 64;
    sf.colorPitch[2]    = 64;
    sf.colorPitch[3]    = 64;
    sf.depthFormat      = GCM_SURFACE_ZETA_Z16;
    sf.depthLocation    = GCM_LOCATION_RSX;
    sf.depthOffset      = g_doff;
    sf.depthPitch       = g_dpitch;
    sf.type             = GCM_SURFACE_TYPE_LINEAR;
    sf.antiAlias        = GCM_SURFACE_CENTER_1;
    sf.width            = g_dw;
    sf.height           = g_dh;
    sf.x                = 0;
    sf.y                = 0;
    rsxSetSurface(g_ctx, &sf);
}

static void gpu_init(void)
{
    videoState state;
    videoConfiguration vconfig;
    videoResolution res;
    void *host_addr = NULL;
    u32 host_sz = HOST_SIZE;
    int i;

    {   /* sondeo: cuanta memoria libre hay en el heap antes de pedir el bloque de video */
        static const unsigned mb[] = { 192, 160, 128, 96, 64, 48, 32, 16, 8 };
        unsigned k;
        for (k = 0; k < sizeof mb / sizeof mb[0]; k++) {
            void *t = malloc((size_t)mb[k] << 20);
            if (t) { free(t); logf_("memoria: caben %u MB seguidos en el heap", mb[k]); break; }
        }
        if (k == sizeof mb / sizeof mb[0]) logf_("memoria: NO caben ni 8 MB seguidos");
    }
    /* Arcade Station: el nucleo FBNeo ocupa mucha memoria; si no cabe el bloque de 32 MB se prueban 16 y 8 MB */
    while (host_sz >= 8 * 1024 * 1024) {
        host_addr = memalign(1024 * 1024, host_sz);
        logf_("video: bloque RSX de %u MB -> %s", (unsigned)(host_sz >> 20), host_addr ? "ok" : "SIN MEMORIA");
        if (host_addr) break;
        host_sz >>= 1;
    }
    if (!host_addr) { logf_("ERROR: no hay memoria para iniciar el video"); return; }

    rsxInit(&g_ctx, CB_SIZE, host_sz, host_addr);
    logf_("video: rsxInit ok");

    videoGetState(0, 0, &state);
    logf_("video: modo de la TV resolucion id=%d", (int)state.displayMode.resolution);
    {
        int rid = (int)state.displayMode.resolution;
        if (g_cfg.res == 720 && rid != VIDEO_RESOLUTION_720) {
            if (videoGetResolutionAvailability(VIDEO_PRIMARY, VIDEO_RESOLUTION_720, VIDEO_ASPECT_AUTO, 0) == 1) {
                rid = VIDEO_RESOLUTION_720;
                logf_("video: forzando 720p (res=720)");
            } else logf_("AVISO: la TV no ofrece 720p, sigo con la resolucion actual");
        }
        videoGetResolution(rid, &res);
        state.displayMode.resolution = (u8)rid;
    }

    memset(&vconfig, 0, sizeof vconfig);
    vconfig.resolution = state.displayMode.resolution;
    vconfig.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vconfig.pitch      = res.width * sizeof(u32);

    logf_("video: configurando...");
    gpu_wait_idle();
    logf_("video: gpu libre");
    videoConfigure(0, &vconfig, NULL, 0);
    videoGetState(0, 0, &state);
    logf_("video: configurado %ux%u", (unsigned)res.width, (unsigned)res.height);

    gcmSetFlipMode(GCM_FLIP_VSYNC);

    g_dw = res.width;
    g_dh = res.height;
    g_pitch = g_dw * sizeof(u32);
    g_cbuf[0] = (u32 *)rsxMemalign(64, g_dh * g_pitch);
    g_cbuf[1] = (u32 *)rsxMemalign(64, g_dh * g_pitch);
    rsxAddressToOffset(g_cbuf[0], &g_off[0]);
    rsxAddressToOffset(g_cbuf[1], &g_off[1]);
    gcmSetDisplayBuffer(0, g_off[0], g_pitch, g_dw, g_dh);
    gcmSetDisplayBuffer(1, g_off[1], g_pitch, g_dw, g_dh);

    g_dpitch = g_dw * sizeof(u32);
    g_dbuf = (u32 *)rsxMemalign(64, (g_dh * g_dpitch) * 2);
    rsxAddressToOffset(g_dbuf, &g_doff);

    for (i = 0; i < 2; i++) {
        g_tex[i] = (u32 *)rsxMemalign(64, SCR_W * SCR_H * 4);
        rsxAddressToOffset(g_tex[i], &g_texoff[i]);
        memset(g_tex[i], 0, SCR_W * SCR_H * 4);
        g_texval[i] = 0;
        *(vu32 *)gcmGetLabelAddress(LABEL_TEX0 + i) = 0;
    }

    g_zoom = (int)(g_dh / SCR_H);
    if (g_cfg.render && g_zoom > 3) g_zoom = 3;      /* escribir a mano en la VRAM es lento: x3 maximo (v1.3; antes x2) */
    if (g_cfg.zoom > 0 && g_cfg.zoom < g_zoom) g_zoom = g_cfg.zoom;
    if (g_zoom < 1) g_zoom = 1;
    if (g_zoom > 4) g_zoom = 4;
    memset(g_cbuf[0], 0, g_dh * g_pitch);
    memset(g_cbuf[1], 0, g_dh * g_pitch);
    logf_("video: zoom x%d  render=%s", g_zoom, g_cfg.render ? "cpu" : "gpu");

    gpu_set_target(g_cur);
}

static void gpu_flip(void)
{
    if (!g_first) {
        int guard = 0;
        while (gcmGetFlipStatus() != 0) {
            usleep(200);
            if ((++guard & 0xFF) == 0) pump();
            if (guard > 15000) { logf_("AVISO: el flip no termino en 3 s"); break; }
        }
        gcmResetFlipStatus();
    } else {
        gcmResetFlipStatus();
    }
    gcmSetFlip(g_ctx, g_cur);
    rsxFlushBuffer(g_ctx);
    gcmSetWaitFlip(g_ctx);
    g_cur ^= 1;
    gpu_set_target(g_cur);
    g_first = 0;
}

/* v1.9.7: copia el cuadro de menu de 1280x720 al buffer de pantalla (igual de ancho: fila por fila; otra resolucion: vecino mas cercano) */
static void hr_blit(uint32_t *base, int dw, int dh)
{
    int x, y;
    if (dw == HR_W && dh == HR_H) { memcpy(base, g_hr, sizeof g_hr); return; }
    for (y = 0; y < dh; y++) {
        const uint32_t *s = g_hr + (size_t)(y * HR_H / dh) * HR_W;
        uint32_t *d = base + (size_t)y * dw;
        for (x = 0; x < dw; x++) d[x] = s[x * HR_W / dw];
    }
}

static void gpu_blit_canvas(void)
{
    gcmTransferScale scale;
    gcmTransferSurface surface;
    int sel = g_texsel;
    float zoom = (float)g_zoom;

    if (g_cfg.render) {
        /* Modo CPU: sin transferencia ni clear ni blend de la GPU; escribo directo en el buffer de pantalla */
        static unsigned cpu_frame[2];     /* un contador por buffer de pantalla (v1.4) */
        int smode = (g_ingame || g_adjusting) ? g_screen : MENU_SCREEN;   /* v1.9.3 */
        if (!g_first) {
            int guard = 0;
            while (gcmGetFlipStatus() != 0 && guard++ < 15000) usleep(200);
        }
        g_stage = "cpu-copia";
        if (g_hr_on) {                    /* v1.9.7: cuadro de menu de 1280x720 */
            g_stage = "cpu-copia-hr";
            hr_blit((uint32_t *)g_cbuf[g_cur], (int)g_dw, (int)g_dh);
            g_hr_on = 0;
            cpu_frame[0] = cpu_frame[1] = 0;   /* al volver al juego se limpian los bordes enseguida */
            return;
        }
        /* v1.3: cada 4 cuadros DE CADA BUFFER se limpia lo que queda fuera del juego (bordes) para que no se quede el
         * residuo del XMB (reloj / bateria del mando); el juego mismo se repinta siempre.
         * v1.8.2: el escalado (modo de pantalla elegido en OPCIONES) y esa limpieza viven en canvas_blit_cpu. */
        g_bars_mosaic = (g_dw == HR_W && g_dh == HR_H);      /* v1.9.9: barras con el mosaico (solo en 1280x720) */
        if (g_bars_mosaic) hr_init();
        if (g_img_size == 100 && g_img_dx == 0 && g_img_dy == 0)
            canvas_blit_cpu((uint32_t *)g_cbuf[g_cur], (int)g_dw, (int)g_dh, smode, g_zoom, (const uint32_t *)g_fb, &cpu_frame[g_cur & 1]);
        else   /* v1.9.2: con el ajuste de imagen tocado */
            canvas_blit_adj((uint32_t *)g_cbuf[g_cur], (int)g_dw, (int)g_dh, smode, g_zoom, (const uint32_t *)g_fb, &cpu_frame[g_cur & 1],
                            g_img_size, g_img_dx, g_img_dy);
        g_bars_mosaic = 0;
        return;
    }

    g_texsel ^= 1;

    /* Esperar a que la GPU termine de leer ESTA textura (la uso cada 2 cuadros) antes de
     * escribirla otra vez; con tope de ~0,5 s para no quedarse esperando para siempre. */
    g_stage = "gpu-espera-textura";
    {
        int guard = 0;
        while (*(vu32 *)gcmGetLabelAddress(LABEL_TEX0 + sel) != g_texval[sel] && guard++ < 16000)
            usleep(30);
        if (guard >= 16000) logf_("AVISO: la GPU no libero la textura %d a tiempo", sel);
    }
    g_stage = "gpu-copia-textura";
    memcpy(g_tex[sel], g_fb, SCR_W * SCR_H * 4);

    rsxSetClearColor(g_ctx, 0x00000000);
    rsxSetClearDepthStencil(g_ctx, 0xffff);
    rsxClearSurface(g_ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
                           GCM_CLEAR_S | GCM_CLEAR_Z);

    rsxSetBlendFunc(g_ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
                    GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(g_ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(g_ctx, GCM_TRUE);

    memset(&scale, 0, sizeof scale);
    scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
    scale.format     = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
    scale.origin     = GCM_TRANSFER_ORIGIN_CORNER;
    scale.operation  = GCM_TRANSFER_OPERATION_SRCCOPY_AND;
    scale.interp     = GCM_TRANSFER_INTERPOLATOR_NEAREST;
    scale.clipX = 0;
    scale.clipY = 0;
    scale.clipW = g_dw;
    scale.clipH = g_dh;
    scale.outX  = (g_dw - SCR_W * g_zoom) / 2;
    scale.outY  = (g_dh - SCR_H * g_zoom) / 2;
    scale.outW  = SCR_W * g_zoom;
    scale.outH  = SCR_H * g_zoom;
    scale.ratioX = rsxGetFixedSint32(1.f / zoom);
    scale.ratioY = rsxGetFixedSint32(1.f / zoom);
    scale.inX = rsxGetFixedUint16(0);
    scale.inY = rsxGetFixedUint16(0);
    scale.inW = SCR_W;
    scale.inH = SCR_H;
    scale.offset = g_texoff[sel];
    scale.pitch  = sizeof(u32) * SCR_W;

    surface.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
    surface.pitch  = g_pitch;
    surface.offset = g_off[g_cur];

    g_stage = "gpu-encolar-blit";
    rsxSetTransferScaleMode(g_ctx, GCM_TRANSFER_LOCAL_TO_LOCAL, GCM_TRANSFER_SURFACE);
    rsxSetTransferScaleSurface(g_ctx, &scale, &surface);

    /* la GPU escribe esta marca cuando termine el blit de esta textura */
    g_texval[sel]++;
    rsxSetWriteBackendLabel(g_ctx, LABEL_TEX0 + sel, g_texval[sel]);
}

static void present(void)
{
    uint64_t t0 = now_us(), t1;
    gpu_blit_canvas();
    g_stage = "gpu-flip";
    t1 = now_us();
    gpu_flip();
    g_stage = "ok";
    g_tm_draw += now_us() - t1;
    g_tm_pres += now_us() - t0;
    heartbeat();
}

static void sysutil_cb(u64 status, u64 param, void *usrdata)
{
    (void)param; (void)usrdata;
    if (status == SYSUTIL_EXIT_GAME) g_running = 0;
}

static void pump(void) { sysUtilCheckCallback(); au_pump(); }

static void exit_cb(void)
{
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
    au_close();
    if (g_ctx) { gcmSetWaitFlip(g_ctx); rsxFinish(g_ctx, 1); }
}

static void platform_init(void)
{
    gpu_init();
    atexit(exit_cb);
    sysUtilRegisterCallback(0, sysutil_cb, NULL);
}

static void net_ensure(void)
{
    static int ready;
    if (!ready) { logf_("iniciando red"); netInitialize(); ready = 1; logf_("red lista"); }
}

/* Lee el mando 'port'; conserva el ultimo estado si no hay cambios */
static padData *pad_read(int port)
{
    padInfo pi;
    padData d;
    ioPadGetInfo(&pi);
    /* v1.9: un mando apagado/desconectado (o sin lectura todavia) debe quedar con las palancas en el CENTRO (128).
     * Con 0 la palanca parecia empujada a la izquierda/arriba y el jugador 2 se movia solo (error de la v1.8.1 en adelante). */
    if (!pi.status[port]) { memset(&g_pd[port], 0, sizeof g_pd[port]); g_pd[port].ANA_L_H = g_pd[port].ANA_L_V = g_pd[port].ANA_R_H = g_pd[port].ANA_R_V = 128; return &g_pd[port]; }
    memset(&d, 0, sizeof d);
    ioPadGetData(port, &d);
    if (d.len > 0) g_pd[port] = d;
    else if (!g_pd[port].ANA_L_H && !g_pd[port].ANA_L_V) g_pd[port].ANA_L_H = g_pd[port].ANA_L_V = g_pd[port].ANA_R_H = g_pd[port].ANA_R_V = 128;   /* aun sin lectura valida */
    return &g_pd[port];
}

/* v1.8.2: botones de la PS3 apretados, como mascara PB_BIT (los que se pueden asignar en OPCIONES) */
static uint32_t pad_buttons(const padData *d)
{
    uint32_t m = 0;
    if (d->BTN_CROSS)    m |= PB_BIT(PB_CROSS);
    if (d->BTN_CIRCLE)   m |= PB_BIT(PB_CIRCLE);
    if (d->BTN_SQUARE)   m |= PB_BIT(PB_SQUARE);
    if (d->BTN_TRIANGLE) m |= PB_BIT(PB_TRIANGLE);
    if (d->BTN_L1)       m |= PB_BIT(PB_L1);
    if (d->BTN_R1)       m |= PB_BIT(PB_R1);
    if (d->BTN_L2)       m |= PB_BIT(PB_L2);
    if (d->BTN_R2)       m |= PB_BIT(PB_R2);
    if (d->BTN_L3)       m |= PB_BIT(PB_L3);
    if (d->BTN_R3)       m |= PB_BIT(PB_R3);
    if (d->BTN_START)    m |= PB_BIT(PB_START);
    if (d->BTN_SELECT)   m |= PB_BIT(PB_SELECT);
    return m;
}

static uint16_t pad_nes_bits(int port)
{
    padData *d = pad_read(port);
    uint16_t dirs = 0;
    if (d->BTN_UP)     dirs |= NB_UP;
    if (d->BTN_DOWN)   dirs |= NB_DOWN;
    if (d->BTN_LEFT)   dirs |= NB_LEFT;
    if (d->BTN_RIGHT)  dirs |= NB_RIGHT;
    dirs |= stick_nes_bits(d->ANA_L_H, d->ANA_L_V);       /* v1.8.1: palanca izquierda = cruz */
    {
        uint16_t b = map_nes_bits(port, pad_buttons(d), dirs);   /* v1.8.2: botones segun OPCIONES > MANDOS (por defecto, igual que antes) */
        if (d->BTN_SQUARE)   b |= NB_Y;                           /* Arcade Station: botones extra para 3 y 6 botones */
        if (d->BTN_TRIANGLE) b |= NB_X;
        if (d->BTN_L1)       b |= NB_L;
        if (d->BTN_R1)       b |= NB_R;
        if (d->BTN_R2)       b |= NB_R2;                          /* Arcade Station: 7o y 8o boton de algunos juegos */
        if (port > 0 && d->BTN_L2) b |= NB_L2;
        return b;
    }
}

static uint16_t ui_edges(void)
{
    padData *d = pad_read(0);
    uint16_t c = 0, e;
    if (d->BTN_UP)     c |= UI_UP;
    if (d->BTN_DOWN)   c |= UI_DOWN;
    if (d->BTN_LEFT)   c |= UI_LEFT;
    if (d->BTN_RIGHT)  c |= UI_RIGHT;
    {   /* v1.8.1: palanca izquierda tambien mueve los menus */
        uint16_t sb = stick_nes_bits(d->ANA_L_H, d->ANA_L_V);
        if (sb & NB_UP)    c |= UI_UP;
        if (sb & NB_DOWN)  c |= UI_DOWN;
        if (sb & NB_LEFT)  c |= UI_LEFT;
        if (sb & NB_RIGHT) c |= UI_RIGHT;
    }
    if (d->BTN_CROSS)  c |= UI_OK;
    if (d->BTN_CIRCLE) c |= UI_CANCEL;
    if (d->BTN_START)  c |= UI_START;
    e = c & ~g_ui_prev;
    g_ui_prev = c;
    return e;
}

/* v1.9.5: R3 (clic de la palanca izquierda) abre el menu en partida (antes R2: a un amigo se le escapaba el dedo y el menu abierto
 * frena la partida online). prime=1 solo memoriza el estado actual, para ignorar un R3 que siga apretado al empezar la partida. */
static int exit_pressed(int prime, uint32_t frame)
{
    static int prev;
    padData *d = pad_read(0);
    int now = d->BTN_R3 ? 1 : 0;
    int e = now && !prev;
    (void)frame;
    prev = now;
    return prime ? 0 : e;
}

/* v1.9.6: L2 del mando 1 = captura de pantalla (solo en partida; en OPCIONES > MANDOS el mando 1 no puede asignarse L2).
 * prime=1 solo memoriza el estado actual, para ignorar un L2 que siga apretado al empezar la partida. */
static int shot_pressed(int prime, uint32_t frame)
{
    static int prev;
    padData *d = pad_read(0);
    int now = d->BTN_L2 ? 1 : 0;
    int e = now && !prev;
    (void)frame;
    prev = now;
    return prime ? 0 : e;
}

/* v1.9.7: menus en 1280x720 solo en modo CPU (el de siempre), con pantalla de 640x360 o mas y fuera del AJUSTE DE IMAGEN */
static int hr_usable(void) { return g_cfg.render && !g_adjusting && g_dw >= 640 && g_dh >= 360; }

/* medidas reales de la pantalla de la consola (para que la captura salga del mismo tamano que la imagen del juego) */
static void shot_geometry(int *dw, int *dh, int *z) { *dw = (int)g_dw; *dh = (int)g_dh; *z = g_zoom; }

/* descarta pulsaciones viejas del menu (memoriza lo que esta apretado ahora) */
static void ui_prime(void) { (void)ui_edges(); }

#else /* ------------------------- version de pruebas (PC) ------------- */

static void pump(void) {}
static int hr_usable(void) { const char *e = getenv("NESNET_HR"); return !g_adjusting && !(e && e[0] == '0'); }   /* prueba v1.9.7: NESNET_HR=0 = dibujo viejo */
static void present(void);                 /* definida mas abajo (puede volcar capturas de pantalla) */
static void net_ensure(void) {}

/* Pulsaciones simuladas del menu: u d l r = direcciones, x = OK, o = cancelar, s = START.
 * Al agotarse el guion (si habia uno) la app se cierra, para que las pruebas terminen solas. */
static const char *g_ui_script = "";
static int g_ui_has_script;
static uint32_t g_exit_at = 0xFFFFFFFEu;   /* prueba: frame en el que "se aprieta R2" (NESNET_R2) */
static uint32_t g_exit_at2 = 0xFFFFFFFEu;  /* prueba v1.9.1: segundo R2 (NESNET_R2B) */
static uint16_t ui_edges(void)
{
    char c = *g_ui_script;
    if (!c) { if (g_ui_has_script) g_running = 0; return 0; }
    g_ui_script++;
    switch (c) {
    case 'u': return UI_UP;
    case 'd': return UI_DOWN;
    case 'l': return UI_LEFT;
    case 'r': return UI_RIGHT;
    case 'x': return UI_OK;
    case 'o': return UI_CANCEL;
    case 's': return UI_START;
    default:  return 0;
    }
}
static uint16_t pad_nes_bits(int port) { (void)port; return 0; }
static void ui_prime(void) {}
static int exit_pressed(int prime, uint32_t frame) { return !prime && (frame == g_exit_at || frame == g_exit_at2); }
/* prueba v1.9.6: "se aprieta L2" en los cuadros de NESNET_SHOTF=a,b,c (hasta 8) */
static uint32_t g_shot_at[8] = { NO_FRAME, NO_FRAME, NO_FRAME, NO_FRAME, NO_FRAME, NO_FRAME, NO_FRAME, NO_FRAME };
static int shot_pressed(int prime, uint32_t frame)
{
    int k;
    if (prime) return 0;
    for (k = 0; k < 8; k++) if (frame == g_shot_at[k]) return 1;
    return 0;
}
static void shot_geometry(int *dw, int *dh, int *z) { *dw = 1280; *dh = 720; *z = 3; }

/* Capturas de prueba: NESNET_SHOTDIR=carpeta NESNET_SHOTS=3,40,90 (numeros de present()) */
static void dump_ppm(const char *path);
static uint32_t g_present_n;
static void present(void)
{
    const char *dir = getenv("NESNET_SHOTDIR"), *lst = getenv("NESNET_SHOTS");
    heartbeat();
    g_present_n++;
    if (dir && lst) {
        const char *p = lst;
        while (*p) {
            if ((uint32_t)atoi(p) == g_present_n) {
                char path[400];
                snprintf(path, sizeof path, "%s/shot_%u.ppm", dir, (unsigned)g_present_n);
                dump_ppm(path);
            }
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
        }
    }
    g_hr_on = 0;
}

/* Mando simulado: determinista por (jugador, frame); cambia cada 6 frames */
static uint16_t script_input(int slot, uint32_t f)
{
    uint32_t x = (f / 6 + 1) * 2654435761u ^ ((uint32_t)(slot + 1) * 40503u * 2246822519u);
    uint16_t b;
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; x *= 3266489917u; x ^= x >> 16;
    b = (uint16_t)(x & 0x3FFF);                  /* Arcade Station: 14 bits (6 botones y L2/R2), no solo los 8 de NES */
    if ((b & NB_UP) && (b & NB_DOWN))   b &= ~NB_DOWN;
    if ((b & NB_LEFT) && (b & NB_RIGHT)) b &= ~NB_RIGHT;
    return b;
}
#endif

/* ------------------------------------------------------------------ */
/* Nucleo libretro (FCEUmm)                                            */
/* ------------------------------------------------------------------ */
/* Arcade Station: los mensajes del nucleo (carga de ROM, errores) se guardan en arcade.log. Maximo 600 lineas (despues se ignoran) */
static void core_log_cb(enum retro_log_level level, const char *fmt, ...)
{
    static int n = 0;
    char buf[200];
    va_list ap;
    int i;
    if (n >= 600) return;
    n++;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (i = 0; buf[i]; i++) if (buf[i] == '\n' || buf[i] == '\r') buf[i] = ' ';
    logf_("nucleo[%d]: %s", (int)level, buf);
    if (n == 600) logf_("nucleo: demasiados mensajes, se dejan de guardar");
}

/* Arcade Station: interruptores DIP de cada juego. El nucleo (FBA 2012) los pide como variables con nombre
 *   fbalpha2012_dipswitch_<juego>_<nombre con _ en vez de espacios>
 * y si la app no responde usa el valor por defecto (en Moonwalker: "2 Players", por eso solo salian 2 jugadores).
 * Tabla propia + archivo USRDIR/dips.txt (una linea por ajuste:  juego|Nombre del interruptor|Valor), por ejemplo:
 *   mwalkbl|Play Mode|3 Players
 *   mwalkbl|Lives|3
 * Las lineas de dips.txt mandan sobre la tabla. Si el valor no existe en ese juego, el nucleo lo ignora. */
struct dip_ov { char key[96]; char val[48]; };
static struct dip_ov g_dips[64];
static int g_ndips;
static int g_dips_loaded;

static void dip_add(const char *rom, const char *name, const char *val)
{
    char k[96];
    size_t i;
    int j;
    if (!rom[0] || !name[0] || !val[0]) return;
    snprintf(k, sizeof k, "%s_%s", rom, name);
    for (i = 0; k[i]; i++) if (k[i] == ' ' || k[i] == '=') k[i] = '_';
    for (j = 0; j < g_ndips; j++)
        if (!strcmp(g_dips[j].key, k)) { snprintf(g_dips[j].val, sizeof g_dips[j].val, "%s", val); return; }
    if (g_ndips >= 64) return;
    snprintf(g_dips[g_ndips].key, sizeof g_dips[g_ndips].key, "%s", k);
    snprintf(g_dips[g_ndips].val, sizeof g_dips[g_ndips].val, "%s", val);
    g_ndips++;
}

static void dips_load(void)
{
    char path[300], line[200];
    FILE *f;
    g_ndips = 0; g_dips_loaded = 1;
    dip_add("mwalkbl", "Play Mode", "3 Players");        /* Moonwalker (bootleg): 3 jugadores */
    dip_add("mwalk",   "Play Mode", "3 Players");
    snprintf(path, sizeof path, "%s/dips.txt", g_dir);
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *a = line, *b, *c, *e;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        b = strchr(a, '|'); if (!b) continue; *b++ = 0;
        c = strchr(b, '|'); if (!c) continue; *c++ = 0;
        for (e = c + strlen(c); e > c && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
        dip_add(a, b, c);
    }
    fclose(f);
    logf_("dips.txt: %d ajuste(s) de interruptores DIP", g_ndips);
}

static const char *dip_lookup(const char *key)
{
    const char *p = strstr(key, "_dipswitch_");
    int j;
    if (!p) return NULL;
    p += 11;
    if (!g_dips_loaded) dips_load();
    for (j = 0; j < g_ndips; j++)
        if (!strcmp(g_dips[j].key, p)) { logf_("dip: %s = %s", p, g_dips[j].val); return g_dips[j].val; }
    return NULL;
}

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(enum retro_pixel_format *)data;
        if (f == RETRO_PIXEL_FORMAT_RGB565 || f == RETRO_PIXEL_FORMAT_XRGB8888) {
            g_pixfmt = (unsigned)f;
            return true;
        }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = g_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log_cb;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = (struct retro_variable *)data;
        const char *val = v && v->key ? dip_lookup(v->key) : NULL;
        if (!val) return false;               /* sin cambio: el nucleo usa su valor por defecto */
        v->value = val;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_SET_ROTATION:                 /* Arcade Station: juegos verticales */
        g_rot = *(const unsigned *)data & 3u;
        return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: {      /* Arcade Station: el nucleo dice cuantos jugadores tiene el juego */
        const struct retro_input_descriptor *d = (const struct retro_input_descriptor *)data;
        int mx = -1;
        for (; d && d->description; d++) if ((int)d->port > mx && d->port < 8) mx = (int)d->port;
        g_core_ports = mx + 1;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        return true;
    default:
        return false;
    }
}

/* Arcade Station: FBNeo entrega cuadros de tamanos distintos (320x224, 384x224, 304x224...) y los juegos verticales piden
 * rotacion (SET_ROTATION, antihoraria x90). Todo se escala por vecino mas cercano al lienzo fijo de 320x240 (4:3, como el monitor
 * de un arcade). Decide solo con el cuadro actual y la rotacion (sin estado oculto): dos consolas hacen lo mismo. */
/* Arcade Station: pixel (X,Y) de la imagen YA rotada, convertido a 0xAARRGGBB */
static inline uint32_t vid_px(const void *data, unsigned w, unsigned h, size_t pitch, unsigned X, unsigned Y)
{
    unsigned sx, sy;
    const uint8_t *row;
    switch (g_rot) {
    case 1:  sx = w - 1 - Y; sy = X; break;
    case 2:  sx = w - 1 - X; sy = h - 1 - Y; break;
    case 3:  sx = Y; sy = h - 1 - X; break;
    default: sx = X; sy = Y; break;
    }
    row = (const uint8_t *)data + (size_t)sy * pitch;
    if (g_pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
        uint32_t q = ((const uint16_t *)row)[sx];
        uint32_t r = (q >> 11) & 31, g = (q >> 5) & 63, b = q & 31;
        return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
    }
    return 0xFF000000u | (((const uint32_t *)row)[sx] & 0x00FFFFFFu);
}

/* mezcla a (peso 256-t) y b (peso t), t = 0..255, por canal */
static inline uint32_t vid_mix(uint32_t a, uint32_t b, uint32_t t)
{
    uint32_t it = 256u - t;
    uint32_t rb = (((a & 0x00FF00FFu) * it + (b & 0x00FF00FFu) * t) >> 8) & 0x00FF00FFu;
    uint32_t g  = (((a & 0x0000FF00u) * it + (b & 0x0000FF00u) * t) >> 8) & 0x0000FF00u;
    return 0xFF000000u | rb | g;
}

/* Arcade Station: la imagen del nucleo (CPS1 = 384x224, otras 320x240, 256x224...) se pasa al lienzo de 320x240.
 * Si el tamano coincide se copia tal cual; si no, se escala con interpolacion bilineal en enteros (sin decimales: igual en
 * todas las consolas). El escalado "al vecino mas cercano" de antes tiraba columnas a trozos y deformaba los textos. */
static int g_need_crc = 1;               /* 0 en partida SOLO: el hash del cuadro solo sirve para comparar dos consolas */
/* un renglon de la imagen origen (ya rotada) llevado a 320 pixeles de ancho; xf = 0 -> copia directa */
static void vid_hline(const void *data, unsigned w, unsigned h, size_t pitch, unsigned sy,
                      const unsigned *x0, const unsigned *x1, const unsigned *xf, uint32_t *out)
{
    unsigned x;
    if (g_rot == 0 && g_pixfmt == RETRO_PIXEL_FORMAT_RGB565) {       /* camino rapido (el de FBA 2012): tabla de 64K colores */
        static uint32_t lut[65536];
        static int lut_ok;
        const uint16_t *row = (const uint16_t *)((const uint8_t *)data + (size_t)sy * pitch);
        if (!lut_ok) {
            unsigned q;
            for (q = 0; q < 65536; q++) {
                uint32_t r = (q >> 11) & 31, g = (q >> 5) & 63, b = q & 31;
                lut[q] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
            }
            lut_ok = 1;
        }
        for (x = 0; x < SCR_W; x++) {
            uint32_t a = lut[row[x0[x]]];
            out[x] = xf[x] ? vid_mix(a, lut[row[x1[x]]], xf[x]) : a;
        }
        return;
    }
    for (x = 0; x < SCR_W; x++) {
        uint32_t a = vid_px(data, w, h, pitch, x0[x], sy);
        if (xf[x]) a = vid_mix(a, vid_px(data, w, h, pitch, x1[x], sy), xf[x]);
        out[x] = a;
    }
}

/* Arcade Station: la imagen del nucleo (CPS1 = 384x224, otras 320x240, 256x224...) se pasa al lienzo de 320x240.
 * Escalado bilineal en enteros (igual en todas las consolas). Cada renglon origen se escala a lo ancho UNA vez y los
 * renglones destino vecinos lo reutilizan (antes cada pixel leia 4 pixeles del origen: mucho mas lento). */
static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    unsigned x, y, ew, eh;
    unsigned x0[SCR_W], x1[SCR_W], xf[SCR_W];
    static uint32_t line[2][SCR_W];
    int tag[2] = { -1, -1 };
    uint64_t tm0 = now_us();
    if (!data) return;                      /* frame repetido: no hay cambios */
    g_vid_fresh = 1;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 1024) w = 1024;
    if (h > 1024) h = 1024;
    ew = (g_rot & 1u) ? h : w;              /* tamano de la imagen YA rotada */
    eh = (g_rot & 1u) ? w : h;
    for (x = 0; x < SCR_W; x++) {
        int c = (int)(((2u * x + 1u) * ew * 128u) / SCR_W) - 128;   /* centro del pixel destino, en 1/256 de pixel origen */
        unsigned a;
        if (c < 0) c = 0;
        a = (unsigned)c >> 8;
        if (a > ew - 1) a = ew - 1;
        x0[x] = a; x1[x] = a + 1 < ew ? a + 1 : a; xf[x] = (unsigned)c & 255u;
    }
    for (y = 0; y < SCR_H; y++) {
        int c = (int)(((2u * y + 1u) * eh * 128u) / SCR_H) - 128;
        unsigned y0, y1, yf;
        uint32_t *dst = &g_fb[y * SCR_W];
        const uint32_t *la, *lb;
        int k;
        if (c < 0) c = 0;
        y0 = (unsigned)c >> 8;
        if (y0 > eh - 1) y0 = eh - 1;
        y1 = y0 + 1 < eh ? y0 + 1 : y0;
        yf = (unsigned)c & 255u;
        /* renglon y0 -> linea 'la', renglon y1 -> linea 'lb' (se reutiliza lo ya calculado) */
        if (tag[0] == (int)y0) k = 0; else if (tag[1] == (int)y0) k = 1; else k = -1;
        if (k < 0) { k = (tag[0] == (int)y1) ? 1 : 0; vid_hline(data, w, h, pitch, y0, x0, x1, xf, line[k]); tag[k] = (int)y0; }
        la = line[k];
        if (yf && y1 != y0) {
            int m = 1 - k;
            if (tag[m] != (int)y1) { vid_hline(data, w, h, pitch, y1, x0, x1, xf, line[m]); tag[m] = (int)y1; }
            lb = line[m];
            for (x = 0; x < SCR_W; x++) dst[x] = vid_mix(la[x], lb[x], yf);
        } else {
            for (x = 0; x < SCR_W; x++) dst[x] = la[x];
        }
    }
    if (g_need_crc) {
        uint32_t hsh = 2166136261u;
        for (y = 0; y < SCR_W * SCR_H; y++) hsh = fnv_u32(hsh, g_fb[y]);
        g_frame_crc = hsh;
    } else g_frame_crc = 0;
    g_tm_scale += now_us() - tm0;
}

static void audio_cb(int16_t l, int16_t r) { int16_t s2[2]; s2[0] = l; s2[1] = r; au_push(s2, 1); }
static size_t audio_batch_cb(const int16_t *d, size_t frames) { au_push(d, frames); return frames; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    unsigned bit;
    (void)idx;
    if (port > 3u || dev != RETRO_DEVICE_JOYPAD) return 0;
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_A:      bit = 0; break;
    case RETRO_DEVICE_ID_JOYPAD_B:      bit = 1; break;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: bit = 2; break;     /* en arcade: moneda */
    case RETRO_DEVICE_ID_JOYPAD_START:  bit = 3; break;
    case RETRO_DEVICE_ID_JOYPAD_UP:     bit = 4; break;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   bit = 5; break;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   bit = 6; break;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  bit = 7; break;
    case RETRO_DEVICE_ID_JOYPAD_Y:      bit = 8; break;
    case RETRO_DEVICE_ID_JOYPAD_X:      bit = 9; break;
    case RETRO_DEVICE_ID_JOYPAD_L:      bit = 10; break;
    case RETRO_DEVICE_ID_JOYPAD_R:      bit = 11; break;
    case RETRO_DEVICE_ID_JOYPAD_L2:     bit = 12; break;
    case RETRO_DEVICE_ID_JOYPAD_R2:     bit = 13; break;
    default: return 0;
    }
    if (bit == 2 || bit == 3) {      /* diagnostico: avisa en el log cada vez que el nucleo lee SELECT (moneda) o START apretado */
        static uint8_t prev[4][2];
        int v = (g_pad[port] >> bit) & 1;
        if (v && !prev[port][bit - 2]) logf_("%s: el nucleo lee el boton del puerto %u (mando %u)", bit == 2 ? "moneda" : "start", port, port + 1);
        prev[port][bit - 2] = (uint8_t)v;
        return (int16_t)v;
    }
    return (g_pad[port] >> bit) & 1;
}

static int read_whole_file(const char *path, uint8_t **buf, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    *buf = (uint8_t *)malloc((size_t)n);
    if (!*buf || fread(*buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(*buf); *buf = NULL; return 0; }
    fclose(f);
    *size = (size_t)n;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Selector de ROMs (v1.7): USRDIR/roms/, reconocidas por su cabecera  */
/* ------------------------------------------------------------------ */
typedef struct { char name[ROM_NAMELEN]; uint32_t size; uint32_t crc; int has_crc; } RomEnt;   /* v1.9: tamano y CRC32 (el CRC se calcula al detenerse el cursor) */
static RomEnt g_roms[ROM_MAX];
static int    g_nroms;

static int name_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}

static int rom_cmp(const void *a, const void *b)
{
    return name_cmp(((const RomEnt *)a)->name, ((const RomEnt *)b)->name);
}

/* Arcade Station: 1 si el archivo es un romset .zip (archivo normal de al menos 1 KB). Los BIOS (neogeo.zip, etc.) tambien salen en la
 * lista: el nucleo los busca solo en esta misma carpeta. */
static int is_nes_file(const char *path)
{
    struct stat st;
    const char *e = strrchr(path, '.');
    if (!e) return 0;
    if (name_cmp(e, ".zip") != 0) return 0;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    if (st.st_size < 1024) return 0;
    return 1;
}

static void roms_scan(void)
{
    char dir[300], path[420];
    DIR *d;
    struct dirent *e;
    g_nroms = 0;
    snprintf(dir, sizeof dir, "%s/roms", g_dir);
    mkdir(dir, 0777);                    /* si no existe, la crea para que se sepa donde van las ROMs */
    d = opendir(dir);
    if (!d) { logf_("roms: no pude abrir %s (errno=%d)", dir, errno); return; }
    while ((e = readdir(d)) != NULL && g_nroms < ROM_MAX) {
        if (e->d_name[0] == '.') continue;
        if (strlen(e->d_name) >= ROM_NAMELEN) { logf_("roms: nombre demasiado largo, lo salto"); continue; }
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (!is_nes_file(path)) continue;
        snprintf(g_roms[g_nroms].name, ROM_NAMELEN, "%s", e->d_name);
        {
            struct stat st;
            g_roms[g_nroms].size = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
            g_roms[g_nroms].crc = 0;
            g_roms[g_nroms].has_crc = 0;
        }
        g_nroms++;
    }
    closedir(d);
    if (g_nroms > 1) qsort(g_roms, (size_t)g_nroms, sizeof g_roms[0], rom_cmp);
    logf_("roms: %d ROM(s) en %s", g_nroms, dir);
}

/* Lee la ROM g_roms[idx] y la deja como la activa (g_rom/g_romcrc). 1 = bien */
static int rom_load(int idx)
{
    char path[420];
    uint8_t *buf = NULL;
    size_t sz = 0;
    struct stat st;
    snprintf(path, sizeof path, "%s/roms/%s", g_dir, g_roms[idx].name);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) { logf_("roms: no pude leer %s", path); return 0; }
    sz = (size_t)st.st_size;
    buf = (uint8_t *)malloc(1);               /* marcador "hay ROM elegida"; el nucleo abre el .zip por ruta, no se carga en memoria */
    if (!buf) return 0;
    buf[0] = 0;
    free(g_rom);
    g_rom = buf;
    g_romsz = sz;
    g_romcrc = crc32_buf((const uint8_t *)g_roms[idx].name, strlen(g_roms[idx].name)) ^ (uint32_t)sz;   /* identifica nombre + tamano del .zip */
    snprintf(g_cfg.rom, sizeof g_cfg.rom, "%s", path);
    snprintf(g_rom_name, sizeof g_rom_name, "%s", g_roms[idx].name);
    logf_("ROM elegida: %s  %u bytes  crc32=%08X", g_rom_name, (unsigned)g_romsz, (unsigned)g_romcrc);
    return 1;
}

static void core_boot(void)
{
    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();
}

static int g_core_dirty;                  /* 1 = ya se cargo un juego: antes de otro hay que reiniciar el nucleo (retro_deinit + retro_init) */
static int g_mode4;                       /* Arcade Station: multijugador online: 0 = 2vs2, 1 = 2vs1 (host 2 mandos, amigo 1), 2 = 1vs2 (host 1, amigo 2) */
static int g_nplayers = 2;                /* Arcade Station: jugadores del juego cargado (2 por defecto, maximo 4) */
static int core_load(void)
{
    struct retro_game_info gi;
    int p;
    if (g_core_loaded) return 1;
    if (g_core_dirty) {   /* FBA 2012: retro_unload_game NO libera el juego anterior; cargar otro encima colgaba la PS3 */
        logf_("nucleo: reiniciando (deinit + init) antes de otro juego");
        retro_deinit();
        retro_init();
        g_core_dirty = 0;
    }
    g_core_dirty = 1;
    memset(&gi, 0, sizeof gi);
    gi.path = g_cfg.rom;                      /* FBNeo abre el .zip por ruta (need_fullpath): sin datos en memoria */
    gi.data = NULL;
    gi.size = 0;
    g_core_ports = 0;
    g_rot = 0;
    logf_("nucleo: cargando juego %s", g_cfg.rom);
    if (!retro_load_game(&gi)) { logf_("nucleo: retro_load_game devolvio ERROR"); g_err = "EL NUCLEO NO PUDO CARGAR LA ROM"; return 0; }
    logf_("nucleo: retro_load_game OK");
    for (p = 0; p < 4; p++) retro_set_controller_port_device((unsigned)p, RETRO_DEVICE_JOYPAD);
    g_nplayers = g_core_ports < 1 ? 2 : g_core_ports > 4 ? 4 : g_core_ports;
    logf_("jugadores del juego (segun el nucleo): %d  rotacion: %u", g_nplayers, g_rot);
    {
        struct retro_system_av_info av;
        memset(&av, 0, sizeof av);
        retro_get_system_av_info(&av);
        au_set_inrate(av.timing.sample_rate, av.timing.fps);
        logf_("video: base %ux%u  audio: %d Hz a %d cuadros/s", av.geometry.base_width, av.geometry.base_height, (int)g_au_inrate, (int)(av.timing.fps + 0.5));
    }
    g_core_loaded = 1;
    return 1;
}

/* v1.8: copia de core_load() para el modo de 4. Declara los puertos 3 y 4 como mando (GAMEPAD): con eso FCEUmm
 * enciende solo el adaptador Four Score. Los juegos que no lo usan simplemente ignoran los mandos 3 y 4. */
static int core_load4(void)
{
    struct retro_game_info gi;
    if (g_core_loaded) return 1;
    if (g_core_dirty) { logf_("nucleo: reiniciando (deinit + init) antes de otro juego"); retro_deinit(); retro_init(); g_core_dirty = 0; }
    g_core_dirty = 1;
    memset(&gi, 0, sizeof gi);
    gi.path = g_cfg.rom;
    gi.data = g_rom;
    gi.size = g_romsz;
    if (!retro_load_game(&gi)) { g_err = "EL NUCLEO NO PUDO CARGAR LA ROM"; return 0; }
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(2, NES_DEV_GAMEPAD);   /* GAMEPAD de FCEUmm */
    retro_set_controller_port_device(3, NES_DEV_GAMEPAD);
    logf_("4 jugadores: puertos 3 y 4 como mando (Four Score)");
    {
        struct retro_system_av_info av;
        memset(&av, 0, sizeof av);
        retro_get_system_av_info(&av);
        au_set_inrate(av.timing.sample_rate, av.timing.fps);
        logf_("audio: el nucleo entrega %d Hz a %d cuadros/s", (int)g_au_inrate, (int)(av.timing.fps + 0.5));
    }
    g_core_loaded = 1;
    return 1;
}

static void core_unload(void)
{
    if (g_core_loaded) { logf_("nucleo: juego cerrado"); retro_unload_game(); g_core_loaded = 0; }
}

/* ------------------------------------------------------------------ */
/* v1.9: guardado de bateria (SRAM) - SOLO en el modo SOLO              */
/* Archivo: USRDIR/saves/<CRC32 de la ROM>.srm (asi renombrar la ROM no pierde el guardado).                      */
/* En online NO se carga ni se guarda nada: las dos consolas tienen que empezar con la misma memoria, si no se       */
/* desincronizan, y ademas una partida online no debe pisar tu guardado.                                          */
/* ------------------------------------------------------------------ */
#define SRAM_MAX 0x20000
static uint8_t g_sram_last[SRAM_MAX];     /* lo ultimo escrito en disco (para guardar solo si cambio) */
static size_t  g_sram_lastsz;
static int     g_sram_on;                 /* 1 = esta partida guarda (solo SOLO, y solo si el juego tiene bateria) */

static void sram_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/saves/%08X.srm", g_dir, (unsigned)g_romcrc);
}

static uint8_t *sram_ptr(size_t *sz)
{
    void *p = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!p || n == 0 || n > SRAM_MAX) return NULL;
    *sz = n;
    return (uint8_t *)p;
}

/* Llamar justo despues de core_load() en el modo SOLO: carga el guardado si existe */
static void sram_begin(void)
{
    size_t n = 0, fsz = 0;
    uint8_t *p, *buf = NULL;
    char path[420], tmp[440];
    int got;
    g_sram_on = 0; g_sram_lastsz = 0;
    p = sram_ptr(&n);
    if (!p) { logf_("guardado: este juego no usa memoria de bateria"); return; }
    g_sram_on = 1;
    sram_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    got = read_whole_file(path, &buf, &fsz);
    if (!got) got = read_whole_file(tmp, &buf, &fsz);          /* si se corto la luz justo al guardar, queda el .tmp completo */
    if (got) {
        if (fsz == n) { memcpy(p, buf, n); logf_("guardado: cargado %s (%u bytes)", path, (unsigned)n); }
        else {
            char old[450];
            snprintf(old, sizeof old, "%s.old", path);
            remove(old);
            rename(path, old);                                   /* no lo borro: queda como .old */
            logf_("AVISO: el guardado tenia %u bytes y el juego pide %u: lo movi a .old y empiezo de cero", (unsigned)fsz, (unsigned)n);
        }
        free(buf);
    } else {
        logf_("guardado: partida nueva (aun no hay %s)", path);
    }
    memcpy(g_sram_last, p, n);
    g_sram_lastsz = n;
}

/* Escribe el guardado si cambio desde la ultima vez (force = siempre). */
static void sram_save(int force)
{
    size_t n = 0;
    uint8_t *p;
    char dir[320], path[420], tmp[440];
    FILE *f;
    int ok;
    if (!g_sram_on) return;
    p = sram_ptr(&n);
    if (!p || n != g_sram_lastsz) return;
    if (!force && !memcmp(p, g_sram_last, n)) return;
    snprintf(dir, sizeof dir, "%s/saves", g_dir);
    mkdir(dir, 0777);
    sram_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) { logf_("AVISO: no pude crear %s (errno=%d)", tmp, errno); return; }
    ok = fwrite(p, 1, n, f) == n;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { logf_("AVISO: no pude escribir el guardado"); remove(tmp); return; }
    remove(path);
    if (rename(tmp, path) != 0) { logf_("AVISO: no pude renombrar el guardado (queda %s)", tmp); return; }
    memcpy(g_sram_last, p, n);
    logf_("guardado: escrito %s (%u bytes)", path, (unsigned)n);
}

/* ------------------------------------------------------------------ */
/* v1.9.1: estados guardados (savestates) - SOLO en el modo SOLO         */
/* Archivo: USRDIR/saves/<CRC32 de la ROM>_<ranura 1..3>.sta. En online NO existen (las dos consolas tendrian que     */
/* cargar el mismo estado a la vez y se desincronizarian).                                                         */
/* ------------------------------------------------------------------ */
#define STATE_SLOTS 6                     /* v1.9.4: 6 ranuras (antes 3); las 1 a 3 de antes siguen siendo las mismas */
#define STATE_MAX   0x1000000
static int  g_state_slot;                 /* ranura elegida en el menu (0..STATE_SLOTS-1); se recuerda mientras la app esta abierta */
static char g_toast[48];                  /* aviso corto sobre la imagen (guardado, cargado, error) */
static int  g_toast_n;

static void toast(const char *s) { snprintf(g_toast, sizeof g_toast, "%s", s); g_toast_n = 150; }

static void state_path(int slot, char *out, size_t cap)
{
    snprintf(out, cap, "%s/saves/%08X_%d.sta", g_dir, (unsigned)g_romcrc, slot + 1);
}

static int state_exists(int slot)
{
    char path[420];
    FILE *f;
    state_path(slot, path, sizeof path);
    f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int state_save(int slot)
{
    size_t n = retro_serialize_size();
    uint8_t *buf;
    char dir[320], path[420], tmp[440];
    FILE *f;
    int ok;
    if (n == 0 || n > STATE_MAX) { logf_("AVISO: estado: el nucleo pide %u bytes (no valido)", (unsigned)n); return 0; }
    buf = (uint8_t *)malloc(n);
    if (!buf) { logf_("AVISO: estado: sin memoria para %u bytes", (unsigned)n); return 0; }
    if (!retro_serialize(buf, n)) { free(buf); logf_("AVISO: estado: el nucleo no pudo guardar"); return 0; }
    snprintf(dir, sizeof dir, "%s/saves", g_dir);
    mkdir(dir, 0777);
    state_path(slot, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) { free(buf); logf_("AVISO: no pude crear %s (errno=%d)", tmp, errno); return 0; }
    ok = fwrite(buf, 1, n, f) == n;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { free(buf); remove(tmp); logf_("AVISO: no pude escribir el estado"); return 0; }
    remove(path);
    if (rename(tmp, path) != 0) { free(buf); logf_("AVISO: no pude renombrar el estado (queda %s)", tmp); return 0; }
    logf_("estado: guardado ranura %d (%u bytes, crc %08X)", slot + 1, (unsigned)n, (unsigned)crc32_buf(buf, n));
    free(buf);
    return 1;
}

static int state_load(int slot)
{
    size_t n = retro_serialize_size(), fsz = 0;
    uint8_t *buf = NULL;
    char path[420];
    int ok;
    state_path(slot, path, sizeof path);
    if (!read_whole_file(path, &buf, &fsz)) { logf_("estado: la ranura %d esta vacia", slot + 1); return -1; }
    if (fsz != n) {
        free(buf);
        logf_("AVISO: estado: la ranura %d tiene %u bytes y el nucleo pide %u: no la cargo", slot + 1, (unsigned)fsz, (unsigned)n);
        return 0;
    }
    ok = retro_unserialize(buf, fsz) ? 1 : 0;
    logf_("estado: cargada ranura %d (%u bytes, crc %08X) %s", slot + 1, (unsigned)fsz, (unsigned)crc32_buf(buf, fsz), ok ? "OK" : "FALLO");
    free(buf);
    return ok;
}

/* v1.9.4: lee la ranura a memoria (para mandarsela al amigo en ONLINE). 1 = bien (el que llama libera *buf) */
static int state_read(int slot, uint8_t **buf, size_t *n)
{
    size_t want = retro_serialize_size(), fsz = 0;
    char path[420];
    uint8_t *b = NULL;
    state_path(slot, path, sizeof path);
    if (!read_whole_file(path, &b, &fsz)) { logf_("estado: la ranura %d esta vacia", slot + 1); return 0; }
    if (fsz != want || fsz == 0 || fsz > STATE_MAX) {
        free(b);
        logf_("AVISO: estado: la ranura %d tiene %u bytes y el nucleo pide %u: no la cargo", slot + 1, (unsigned)fsz, (unsigned)want);
        return 0;
    }
    *buf = b;
    *n = fsz;
    return 1;
}

/* ------------------------------------------------------------------ */
/* v1.9.6: CAPTURA DE PANTALLA (L2 del mando 1) -> USRDIR/screenshots/<ROM>_NNNN.png                                  */
/* Sale LIMPIA (solo el juego: sin FPS, info de red, avisos ni marco) y del tamano de la pantalla elegida en OPCIONES    */
/* (ORIGINAL 768x720, 4:3 960x720, 16:9 1280x720 en 720p), con el mismo escalado de canvas_blit_cpu, pero SIN el ajuste   */
/* de imagen (tamano / desplazamiento del overscan). Se toma justo despues de emular el cuadro, antes de dibujar textos.   */
/* PNG propio sin librerias: bloques "stored" de deflate (sin compresion), una fila por bloque, escrito en streaming       */
/* (no reserva memoria para la imagen). Solo es local: no toca la red ni el juego (en online cada consola captura lo suyo). */
/* ------------------------------------------------------------------ */
static void shot_geometry(int *dw, int *dh, int *z);

/* v0.1.4: comprobar si un archivo existe con fopen (la copia propia de NesNet, probada en PS3); ya no se usa path_is_valid del nucleo Stella */
static int kt_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static uint32_t g_png_tab[256];
static int      g_png_tab_ok;

static uint32_t png_crc_upd(uint32_t c, const uint8_t *d, size_t n)
{
    if (!g_png_tab_ok) {
        uint32_t i;
        int k;
        for (i = 0; i < 256; i++) {
            uint32_t v = i;
            for (k = 0; k < 8; k++) v = (v & 1u) ? (0xEDB88320u ^ (v >> 1)) : (v >> 1);
            g_png_tab[i] = v;
        }
        g_png_tab_ok = 1;
    }
    while (n--) c = g_png_tab[(c ^ *d++) & 0xFFu] ^ (c >> 8);
    return c;
}

static void png_adler_upd(uint32_t *a, uint32_t *b, const uint8_t *d, size_t n)
{
    while (n) {
        size_t m = n > 5552 ? 5552 : n;
        n -= m;
        while (m--) { *a += *d++; *b += *a; }
        *a %= 65521u;
        *b %= 65521u;
    }
}

static void png_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* Escribe el lienzo 'fb' (256x240) como PNG RGB escalado igual que canvas_blit_cpu (modo mode, ancho de pantalla dw, vertical z).
 * 1 = bien (con el ancho y el alto en outw y outh), 0 = fallo (el archivo parcial se borra) */
static int shot_write_png(const char *path, const uint32_t *fb, int mode, int dw, int z, int *outw, int *outh)
{
    static const uint8_t sig[8]  = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    static const uint8_t iend[12] = { 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82 };
    static uint8_t blk[5 + 1 + 3 * COLMAP_MAX];       /* cabecera del bloque (5) + filtro (1) + pixeles RGB */
    static int cm[COLMAP_MAX];
    int oh = SCR_H * z, ow = screen_out_w(mode, dw, oh, z);
    int rowb, x, y, k, ok, row = 0;
    uint32_t crc, ad_a = 1, ad_b = 0, idat_len;
    uint8_t hd[41], z2[2], tail[4];
    FILE *f;
    if (z < 1 || ow < 1) return 0;
    rowb = 1 + 3 * ow;
    for (x = 0; x < ow; x++) cm[x] = x * SCR_W / ow;
    f = fopen(path, "wb");
    if (!f) return 0;
    memcpy(hd, sig, 8);
    png_be32(hd + 8, 13); memcpy(hd + 12, "IHDR", 4);
    png_be32(hd + 16, (uint32_t)ow); png_be32(hd + 20, (uint32_t)oh);
    hd[24] = 8; hd[25] = 2; hd[26] = 0; hd[27] = 0; hd[28] = 0;                 /* 8 bits, RGB, sin interlace */
    png_be32(hd + 29, ~png_crc_upd(0xFFFFFFFFu, hd + 12, 17));
    idat_len = 2u + (uint32_t)oh * (uint32_t)(5 + rowb) + 4u;                  /* zlib: 2 de cabecera + bloques + 4 de Adler-32 */
    png_be32(hd + 33, idat_len); memcpy(hd + 37, "IDAT", 4);
    ok = fwrite(hd, 1, 41, f) == 41;
    crc = png_crc_upd(0xFFFFFFFFu, hd + 37, 4);
    z2[0] = 0x78; z2[1] = 0x01;
    crc = png_crc_upd(crc, z2, 2);
    ok = ok && fwrite(z2, 1, 2, f) == 2;
    blk[5] = 0;                                                                 /* filtro 0 (sin filtro) */
    for (y = 0; y < SCR_H && ok; y++) {
        const uint32_t *src = fb + y * SCR_W;
        uint8_t *p = blk + 6;
        for (x = 0; x < ow; x++) { uint32_t px = src[cm[x]]; *p++ = (uint8_t)(px >> 16); *p++ = (uint8_t)(px >> 8); *p++ = (uint8_t)px; }
        for (k = 0; k < z && ok; k++) {
            row++;
            blk[0] = (uint8_t)(row == oh ? 1 : 0);                              /* ultimo bloque */
            blk[1] = (uint8_t)(rowb & 0xFF); blk[2] = (uint8_t)((rowb >> 8) & 0xFF);
            blk[3] = (uint8_t)(~rowb & 0xFF); blk[4] = (uint8_t)((~rowb >> 8) & 0xFF);
            ok = fwrite(blk, 1, (size_t)(5 + rowb), f) == (size_t)(5 + rowb);
            crc = png_crc_upd(crc, blk, (size_t)(5 + rowb));
            png_adler_upd(&ad_a, &ad_b, blk + 5, (size_t)rowb);
        }
    }
    png_be32(tail, (ad_b << 16) | ad_a);
    ok = ok && fwrite(tail, 1, 4, f) == 4;
    crc = png_crc_upd(crc, tail, 4);
    png_be32(tail, ~crc);
    ok = ok && fwrite(tail, 1, 4, f) == 4;
    ok = ok && fwrite(iend, 1, 12, f) == 12;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(path); return 0; }
    if (outw) *outw = ow;
    if (outh) *outh = oh;
    return 1;
}

static char g_shot_last[48];                  /* ROM de la ultima captura y su ultimo numero (para no revisar desde el 1 cada vez) */
static int  g_shot_n;

/* Guarda la captura de 'fb'. Devuelve el numero de la captura (1..9999) o 0 si fallo */
static int shot_take(const uint32_t *fb)
{
    char dir[320], base[48], path[420], tmp[440];
    int dw = 1280, dh = 720, z = 3, mode, n, w = 0, h = 0, k = 0;
    uint32_t t0 = now_ms();
    size_t len, dot, j;
    logf_("captura: 1 inicio");
    shot_geometry(&dw, &dh, &z);
    (void)dh;
    mode = g_cfg.render ? g_screen : 0;       /* con el dibujo por GPU la pantalla siempre es ORIGINAL */
    snprintf(dir, sizeof dir, "%s/screenshots", g_dir);
    logf_("captura: 2 carpeta %s", dir);
    mkdir(dir, 0777);
    logf_("captura: 3 carpeta lista");
    /* nombre: la ROM sin extension; solo letras, numeros, - y _ (lo demas pasa a _) */
    len = strlen(g_rom_name);
    dot = len;
    for (j = len; j > 0; j--) if (g_rom_name[j - 1] == '.') { dot = j - 1; break; }
    if (dot == 0) dot = len;
    for (j = 0; j < dot && k < 40; j++) {
        char c = g_rom_name[j];
        base[k++] = (char)(((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_') ? c : '_');
    }
    base[k] = 0;
    if (!base[0]) snprintf(base, sizeof base, "captura");
    if (strcmp(base, g_shot_last)) { snprintf(g_shot_last, sizeof g_shot_last, "%s", base); g_shot_n = 0; }
    for (n = g_shot_n + 1; n < 10000; n++) {
        snprintf(path, sizeof path, "%s/%s_%04d.png", dir, base, n);
        if (!kt_exists(path)) break;
    }
    if (n >= 10000) { logf_("AVISO: captura: ya hay 9999 capturas de este juego"); return 0; }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    logf_("captura: 4 escribiendo %s", tmp);
    if (!shot_write_png(tmp, fb, mode, dw, z, &w, &h)) { logf_("AVISO: captura: no pude escribir %s (errno=%d)", tmp, errno); return 0; }
    logf_("captura: 5 renombrando");
    remove(path);
    if (rename(tmp, path) != 0) { logf_("AVISO: captura: no pude renombrar (queda %s)", tmp); return 0; }
    g_shot_n = n;
    logf_("captura: guardada %s (%dx%d, pantalla=%s) en %u ms", path, w, h, screen_names[mode], (unsigned)(now_ms() - t0));
    return n;
}

/* Toma la captura de la imagen del juego (g_fb, todavia sin textos) y avisa en pantalla */
static void shot_do(void)
{
    char tb[48];
    int n = shot_take(g_fb);
    if (n > 0) snprintf(tb, sizeof tb, "CAPTURA GUARDADA %04d", n);
    else snprintf(tb, sizeof tb, "NO SE PUDO GUARDAR LA CAPTURA");
    toast(tb);
}

static void step_frame(uint16_t p1, uint16_t p2)
{
    uint64_t t0 = now_us();
    g_pad[0] = p1;
    g_pad[1] = p2;
    retro_run();
    g_tm_run += now_us() - t0;
}

/* v1.8: copia de step_frame() para 4 mandos */
static void step_frame4(uint16_t p1, uint16_t p2, uint16_t p3, uint16_t p4)
{
    g_pad[0] = p1;
    g_pad[1] = p2;
    g_pad[2] = p3;
    g_pad[3] = p4;
    retro_run();   /* (en el modo de 4 no se mide el tiempo del nucleo) */
}

/* ------------------------------------------------------------------ */
/* Red: mensajes de 16 bytes, TCP, poll() con espera propia            */
/* ------------------------------------------------------------------ */
enum { M_HELLO = 1, M_PING, M_PONG, M_CFG, M_START, M_INPUT, M_QUIT, M_REJECT, M_RESET, M_GAME, M_STATE, M_INPUT2 };   /* v1.9.1: M_RESET (frame = cuadro en que ambos reinician); v1.9.2: M_GAME (cambio de juego) */
/* v1.9.4: M_STATE = el host manda un estado guardado: frame = cuadro en que los dos lo cargan, v1 = bytes, v2 = CRC32 de los datos;
 * justo despues de este mensaje de 16 bytes vienen v1 bytes sin marco (ver net_send_raw / net_recv_raw). */
/* v1.9.2: M_GAME, campo 'a': pregunta del host (v1 = CRC32 de la ROM, v2 = tamano) / "la tengo" / "no la tengo" /
 * corte (frame = cuadro en que los dos cambian, v1 = CRC32) / marca de fin de lo viejo (ver game_switch_apply) */
enum { GM_ASK = 1, GM_HAVE, GM_MISS, GM_GO, GM_MARK };

typedef struct {
    uint8_t  type, a;
    uint16_t input;
    uint32_t frame, v1, v2;
} Msg;

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static int net_send(const Msg *m)
{
    uint8_t b[MSG_SIZE];
    int off = 0;
    memset(b, 0, sizeof b);
    b[0] = m->type; b[1] = m->a; b[2] = m->input >> 8; b[3] = m->input & 0xFF;
    put32(b + 4, m->frame); put32(b + 8, m->v1); put32(b + 12, m->v2);
#ifdef NESNET_HOST
    { const char *j = getenv("NESNET_JITTER_MS"); if (j && atoi(j) > 0) sleep_ms(rand() % (atoi(j) + 1)); }
#endif
    while (off < MSG_SIZE) {
        int n = (int)send(g_net, (const char *)b + off, MSG_SIZE - off, 0);
        if (n <= 0) return 0;
        off += n;
    }
    return 1;
}

/* Espera a que 'fd' tenga evento; 1 = listo, 0 = agotado, -1 = error */
static int fd_wait(int fd, short ev, int timeout_ms)
{
    uint32_t t0 = now_ms();
    for (;;) {
        struct pollfd p;
        int r;
        p.fd = fd; p.events = ev; p.revents = 0;
        r = poll(&p, 1, 0);
        if (r < 0) return -1;
        if (r > 0) return 1;
        if ((int)(now_ms() - t0) >= timeout_ms) return 0;
        pump();
        if (!g_running) return -1;
        sleep_ms(2);
    }
}

/* 1 = mensaje, 0 = tiempo agotado, -1 = error / conexion cerrada */
static int net_recv(Msg *m, int timeout_ms)
{
    uint32_t t0 = now_ms();
    for (;;) {
        if (g_rxlen >= MSG_SIZE) {
            const uint8_t *b = g_rx;
            m->type = b[0]; m->a = b[1]; m->input = (uint16_t)((b[2] << 8) | b[3]);
            m->frame = get32(b + 4); m->v1 = get32(b + 8); m->v2 = get32(b + 12);
            memmove(g_rx, g_rx + MSG_SIZE, g_rxlen - MSG_SIZE);
            g_rxlen -= MSG_SIZE;
            return 1;
        }
        {
            int left = timeout_ms - (int)(now_ms() - t0);
            int r = fd_wait(g_net, POLLIN, left > 0 ? left : 0);
            int n;
            if (r < 0) return -1;
            if (r == 0) return 0;
            n = (int)recv(g_net, (char *)g_rx + g_rxlen, (int)sizeof g_rx - g_rxlen, 0);
            if (n <= 0) return -1;
            g_rxlen += n;
        }
    }
}

/* v1.9.4: bloques grandes sin marco (estados guardados), justo despues de un mensaje M_STATE. net_recv_raw primero vacia lo que
 * net_recv ya dejo leido en g_rx (el mensaje y los primeros bytes pueden llegar juntos). Cada espera se renueva con cada trozo
 * recibido o enviado: se rinde solo si pasan 20 s sin avanzar nada. */
static int net_send_raw(const uint8_t *src, size_t len)
{
    size_t off = 0;
    uint32_t t0 = now_ms();
    while (off < len) {
        int r = fd_wait(g_net, POLLOUT, 100), n;
        size_t chunk = len - off > 16384 ? 16384 : len - off;
        if (r < 0) return 0;
        if (r == 0) { if ((int)(now_ms() - t0) > 20000) return 0; continue; }
        n = (int)send(g_net, (const char *)src + off, chunk, 0);
        if (n <= 0) return 0;
        off += (size_t)n;
        t0 = now_ms();
    }
    return 1;
}

static int net_recv_raw(uint8_t *dst, size_t len)
{
    size_t got = 0;
    uint32_t t0 = now_ms();
    if (g_rxlen > 0) {
        size_t k = (size_t)g_rxlen < len ? (size_t)g_rxlen : len;
        memcpy(dst, g_rx, k);
        memmove(g_rx, g_rx + k, (size_t)g_rxlen - k);
        g_rxlen -= (int)k;
        got = k;
    }
    while (got < len) {
        int r = fd_wait(g_net, POLLIN, 200), n;
        size_t want = len - got > 65536 ? 65536 : len - got;
        if (r < 0) return 0;
        if (r == 0) { if ((int)(now_ms() - t0) > 20000) return 0; continue; }
        n = (int)recv(g_net, (char *)dst + got, (int)want, 0);
        if (n <= 0) return 0;
        got += (size_t)n;
        t0 = now_ms();
    }
    return 1;
}

static void sock_nonblock(int fd, int on)
{
#ifdef NESNET_PS3
    int v = on;
    setsockopt(fd, SOL_SOCKET, SO_NBIO, &v, sizeof v);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

static void sock_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   /* si falla, no pasa nada */
}

/* v1.6: diagnostico de connect. kind: 0 ok, 1 sin respuesta (timeout), 2 rechazado o error de red, 3 sin socket */
static int      g_conn_kind;
static int      g_conn_err;
static uint32_t g_conn_ms;

static int tcp_connect(uint32_t ip, int port, int timeout_ms)
{
    struct sockaddr_in a;
    uint32_t t0 = now_ms();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    g_conn_kind = 0; g_conn_err = 0; g_conn_ms = 0;
    if (fd < 0) { g_conn_kind = 3; g_conn_err = errno; goto fail; }
    sock_nonblock(fd, 1);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(ip);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        int err = 0, r;
        socklen_t l = sizeof err;
        r = fd_wait(fd, POLLOUT, timeout_ms);
        if (r <= 0) { g_conn_kind = (r == 0) ? 1 : 2; closesocket(fd); goto fail; }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) == 0 && err != 0) {
            g_conn_kind = 2; g_conn_err = err; closesocket(fd); goto fail;
        }
    }
    sock_nonblock(fd, 0);
    sock_nodelay(fd);
    g_conn_ms = now_ms() - t0;
    return fd;
fail:
    g_conn_ms = now_ms() - t0;
    logf_("connect %u.%u.%u.%u:%d FALLO: %s (error=%d) tras %u ms",
          (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255), (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255), port,
          g_conn_kind == 1 ? "SIN RESPUESTA (timeout)" : g_conn_kind == 3 ? "NO SE PUDO CREAR EL SOCKET" : "RECHAZADO O ERROR DE RED",
          g_conn_err, (unsigned)g_conn_ms);
    return -1;
}

static int tcp_listen(int port)
{
    struct sockaddr_in a;
    int one = 1, rb, rl;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { logf_("escucha: socket() fallo (errno=%d)", errno); return -1; }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    rb = bind(fd, (struct sockaddr *)&a, sizeof a);
    rl = rb < 0 ? -1 : listen(fd, 1);
    logf_("escucha: puerto TCP %d  bind=%d listen=%d", port, rb, rl);
    if (rb < 0 || rl < 0) { closesocket(fd); return -1; }
    return fd;
}

static int parse_ip(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) < 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 1;
}

/* IP publica: GET http://api.ipify.org (devuelve solo la IP en texto) */
static int get_public_ip(uint32_t *ip)
{
    static const char req[] =
        "GET / HTTP/1.0\r\nHost: api.ipify.org\r\nConnection: close\r\n\r\n";
    struct hostent *he;
    uint8_t *b;
    char buf[512];
    int fd, n, len = 0;
    char *body;
    uint32_t sip;

    he = gethostbyname("api.ipify.org");
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) return 0;
    b = (uint8_t *)he->h_addr_list[0];
    sip = ((uint32_t)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    fd = tcp_connect(sip, 80, 6000);
    if (fd < 0) return 0;
    if (send(fd, req, (int)sizeof req - 1, 0) <= 0) { closesocket(fd); return 0; }
    while (len < (int)sizeof buf - 1 && fd_wait(fd, POLLIN, 6000) > 0) {
        n = (int)recv(fd, buf + len, (int)sizeof buf - 1 - len, 0);
        if (n <= 0) break;
        len += n;
    }
    closesocket(fd);
    buf[len] = 0;
    body = strstr(buf, "\r\n\r\n");
    if (!body) return 0;
    body += 4;
    while (*body == ' ' || *body == '\r' || *body == '\n') body++;
    return parse_ip(body, ip);
}

/* ------------------------------------------------------------------ */
/* UPnP (v1.6): el host abre su puerto TCP en el router y lee la IP del */
/* router. Los pasos (SSDP -> descripcion del router -> llamadas SOAP)  */
/* siguen el netplay de Melee-PS3 (ps3/net/upnp.c, GPLv2 o posterior),  */
/* que ya conecto entre estas dos casas; aqui es una version propia     */
/* sobre los sockets de NesNet.                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    int      router;      /* un router contesto por UPnP */
    int      opened;      /* el puerto lleva a esta consola */
    int      http;        /* estado HTTP del AddPortMapping (0 = no hubo) */
    int      error;       /* codigo de error UPnP (0 = ninguno) */
    uint32_t public_ip;   /* IP WAN que dice el router (0 = no la dio) */
    uint32_t local_ip;    /* IP de esta consola en la LAN (0 = no se pudo leer) */
} Upnp;

#define UPNP_BUF 12288
static char     g_ubuf[UPNP_BUF];
static uint32_t g_ctl_ip;
static int      g_ctl_port;
static char     g_ctl_path[160], g_ctl_svc[96];
static int      g_upnp_added, g_upnp_port;

#define IP4(ip) (unsigned)((ip) >> 24), (unsigned)(((ip) >> 16) & 255), (unsigned)(((ip) >> 8) & 255), (unsigned)((ip) & 255)

static int ip_privada(uint32_t ip)
{
    return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 22) == ((100u << 2) | 1) ||
           (ip >> 24) == 127 || (ip >> 16) == 0xA9FE || ip == 0;
}

static void sa_fill(struct sockaddr_in *s, uint32_t ip, int port)
{
    memset(s, 0, sizeof *s);
#ifdef NESNET_PS3
    s->sin_len = sizeof *s;               /* PSL1GHT es BSD: con sin_len=0, sendto falla con ENOENT */
#endif
    s->sin_family = AF_INET;
    s->sin_port = htons((uint16_t)port);
    s->sin_addr.s_addr = htonl(ip);
}

/* IP de esta consola en su red local */
static uint32_t local_ip_get(void)
{
#ifdef NESNET_PS3
    static int loaded;
    union net_ctl_info info;
    if (!loaded) { loaded = 1; sysModuleLoad(SYSMODULE_NETCTL); netCtlInit(); }
    if (netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info) < 0) return 0;
    if (inet_addr(info.ip_address) == INADDR_NONE) return 0;
    return ntohl(inet_addr(info.ip_address));
#else
    const char *e = getenv("NESNET_LOCAL_IP");
    uint32_t ip;
    return (e && parse_ip(e, &ip)) ? ip : 0x7F000001u;
#endif
}

/* Un datagrama UDP y la primera respuesta (en resp). Devuelve los bytes o -1. Sigue atendiendo al sistema (pump). */
static int upnp_udp_ask(uint32_t ip, int port, const char *req, int len, char *resp, int cap, int ms)
{
    struct sockaddr_in s;
    struct timeval tv;
    uint32_t t0;
    int fd, n = -1;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    tv.tv_sec = 0; tv.tv_usec = 100000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sa_fill(&s, ip, port);
    if (sendto(fd, req, (size_t)len, 0, (struct sockaddr *)&s, sizeof s) == len) {
        t0 = now_ms();
        for (;;) {
            socklen_t sl = sizeof s;
            int r = (int)recvfrom(fd, resp, (size_t)cap, 0, (struct sockaddr *)&s, &sl);
            if (r > 0) { n = r; break; }
            if ((int)(now_ms() - t0) >= ms) break;
            pump();
            if (!g_running) break;
            sleep_ms(2);
        }
    }
    closesocket(fd);
    return n;
}

/* Pide por HTTP/TCP y deja la respuesta (terminada en 0) en g_ubuf. Devuelve los bytes o -1. */
static int upnp_http(uint32_t ip, int port, const char *req, int ms)
{
    int fd, len = 0, n;
    fd = tcp_connect(ip, port, ms);
    if (fd < 0) return -1;
    if (send(fd, req, (int)strlen(req), 0) <= 0) { closesocket(fd); return -1; }
    while (len < UPNP_BUF - 1 && fd_wait(fd, POLLIN, ms) > 0) {
        n = (int)recv(fd, g_ubuf + len, UPNP_BUF - 1 - len, 0);
        if (n <= 0) break;
        len += n;
    }
    closesocket(fd);
    g_ubuf[len] = 0;
    return len > 0 ? len : -1;
}

/* Texto entre <name> y </name> a partir de p; devuelve donde acaba o NULL */
static const char *x_tag(const char *p, const char *name, char *out, size_t cap)
{
    char open[64], close_[64];
    const char *s, *e;
    size_t n;
    snprintf(open, sizeof open, "<%s>", name);
    snprintf(close_, sizeof close_, "</%s>", name);
    s = strstr(p, open);
    if (!s) return NULL;
    e = strstr(s, close_);
    if (!e) return NULL;
    s += strlen(open);
    n = (size_t)(e - s) < cap - 1 ? (size_t)(e - s) : cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
    return e;
}

/* "http://a.b.c.d[:puerto]/ruta" */
static int x_url(const char *url, uint32_t *ip, int *port, char *path, size_t cap)
{
    unsigned a, b, c, d, pt = 80;
    const char *slash;
    if (strncmp(url, "http://", 7) != 0 || sscanf(url + 7, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    slash = strchr(url + 7, '/');
    sscanf(url + 7, "%*[0-9.]:%u", &pt);
    *ip = (a << 24) | (b << 16) | (c << 8) | d;
    *port = (int)pt;
    snprintf(path, cap, "%s", slash ? slash : "/");
    return 1;
}

static int ci_prefix(const char *s, const char *pre)
{
    for (; *pre; s++, pre++) {
        char x = *s, y = *pre;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

/* Busca el router (SSDP), lee su descripcion y guarda la URL de control. 1 = encontrado */
static int upnp_discover(void)
{
    static const char search[] =
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
        "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
    char url[192], path[160], req[360], type[96], ctl[160];
    uint32_t dip = 0;
    int dport = 0, n;
    const char *p;
    url[0] = 0;
#ifdef NESNET_HOST
    if (getenv("NESNET_UPNP_URL")) snprintf(url, sizeof url, "%s", getenv("NESNET_UPNP_URL"));
#endif
    if (!url[0]) {
        n = upnp_udp_ask(0xEFFFFFFAu, 1900, search, (int)sizeof search - 1, g_ubuf, UPNP_BUF - 1, 2000);
        if (n <= 0) { logf_("upnp: SSDP sin respuesta en 2 s (el router no tiene UPnP o lo bloquea)"); return 0; }
        g_ubuf[n] = 0;
        p = g_ubuf;
        while (p && !ci_prefix(p, "location:")) { p = strchr(p, '\n'); p = p ? p + 1 : NULL; }
        if (!p || sscanf(p + 9, " %191s", url) != 1) { logf_("upnp: la respuesta SSDP no trae LOCATION"); return 0; }
    }
    if (!x_url(url, &dip, &dport, path, sizeof path)) { logf_("upnp: URL de descripcion no valida: %s", url); return 0; }
    snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %u.%u.%u.%u:%d\r\nConnection: close\r\n\r\n", path, IP4(dip), dport);
    if (upnp_http(dip, dport, req, 3000) <= 0) { logf_("upnp: no pude leer la descripcion del router (%s)", url); return 0; }
    for (p = g_ubuf; (p = x_tag(p, "serviceType", type, sizeof type)) != NULL;) {
        if ((strstr(type, "WANIPConnection") || strstr(type, "WANPPPConnection")) && x_tag(p, "controlURL", ctl, sizeof ctl)) {
            snprintf(g_ctl_svc, sizeof g_ctl_svc, "%s", type);
            g_ctl_ip = dip; g_ctl_port = dport;
            if (ctl[0] == '/') { snprintf(g_ctl_path, sizeof g_ctl_path, "%s", ctl); return 1; }
            if (!strncmp(ctl, "http://", 7)) return x_url(ctl, &g_ctl_ip, &g_ctl_port, g_ctl_path, sizeof g_ctl_path);
            snprintf(g_ctl_path, sizeof g_ctl_path, "/%s", ctl);
            return 1;
        }
    }
    logf_("upnp: el router no ofrece WANIPConnection/WANPPPConnection");
    return 0;
}

/* Una llamada SOAP al router: estado HTTP (o -1); respuesta en g_ubuf; error UPnP en *err */
static int upnp_soap(const char *action, const char *args, int *err)
{
    static char body[1024], req[1792];
    char code[16], status[8];
    snprintf(body, sizeof body,
             "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s>"
             "</s:Body></s:Envelope>\r\n", action, g_ctl_svc, args, action);
    snprintf(req, sizeof req,
             "POST %s HTTP/1.0\r\nHost: %u.%u.%u.%u:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
             "Content-Length: %d\r\nSOAPAction: \"%s#%s\"\r\nConnection: close\r\n\r\n%s",
             g_ctl_path, IP4(g_ctl_ip), g_ctl_port, (int)strlen(body), g_ctl_svc, action, body);
    *err = 0;
    if (upnp_http(g_ctl_ip, g_ctl_port, req, 3000) <= 0 || sscanf(g_ubuf, "HTTP/%*s %7s", status) != 1) return -1;
    if (x_tag(g_ubuf, "errorCode", code, sizeof code)) *err = atoi(code);
    return atoi(status);
}

/* Abre el puerto TCP en el router hacia esta consola y lee la IP del router */
static void upnp_open(int port, Upnp *u)
{
    char args[640], ip[20], value[40];
    unsigned a, b, c, d;
    int status, err = 0, lease = 7200;

    memset(u, 0, sizeof *u);
    u->local_ip = local_ip_get();
    logf_("upnp: IP local de la consola = %u.%u.%u.%u", IP4(u->local_ip));
    if (!upnp_discover()) return;
    u->router = 1;
    logf_("upnp: router en %u.%u.%u.%u:%d servicio=%s", IP4(g_ctl_ip), g_ctl_port, g_ctl_svc);
    if (upnp_soap("GetExternalIPAddress", "", &err) == 200 && x_tag(g_ubuf, "NewExternalIPAddress", value, sizeof value) &&
        sscanf(value, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a < 256 && b < 256 && c < 256 && d < 256) {
        u->public_ip = (a << 24) | (b << 16) | (c << 8) | d;
        logf_("upnp: IP WAN del router = %u.%u.%u.%u%s", IP4(u->public_ip), ip_privada(u->public_ip) ? "  (PRIVADA: CGNAT o doble NAT)" : "");
    } else {
        logf_("upnp: el router no dio su IP WAN (error=%d)", err);
    }
    if (!u->local_ip) { logf_("upnp: no conozco la IP local, no abro el puerto"); return; }
    snprintf(ip, sizeof ip, "%u.%u.%u.%u", IP4(u->local_ip));
    for (;;) {
        snprintf(args, sizeof args,
                 "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>TCP</NewProtocol>"
                 "<NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
                 "<NewPortMappingDescription>ArcadeStation</NewPortMappingDescription><NewLeaseDuration>%d</NewLeaseDuration>",
                 port, port, ip, lease);
        status = upnp_soap("AddPortMapping", args, &err);
        if (err == 725 && lease != 0) { lease = 0; continue; }      /* el router solo acepta reglas permanentes */
        break;
    }
    u->http = status; u->error = err;
    if (status == 200) {
        u->opened = 1; g_upnp_added = 1; g_upnp_port = port;
    } else if (err == 718) {
        /* ya existia una regla: sirve si lleva a esta consola */
        snprintf(args, sizeof args,
                 "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>TCP</NewProtocol>", port);
        u->opened = upnp_soap("GetSpecificPortMappingEntry", args, &err) == 200 &&
                    x_tag(g_ubuf, "NewInternalClient", value, sizeof value) && !strcmp(value, ip);
    }
    logf_("upnp: AddPortMapping TCP %d -> %s: http=%d error=%d abierto=%d", port, ip, status, u->error, u->opened);
}

/* Quita la regla que puso upnp_open (si puso alguna) */
static void upnp_close(void)
{
    char args[256];
    int err = 0;
    if (!g_upnp_added) return;
    g_upnp_added = 0;
    snprintf(args, sizeof args,
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>TCP</NewProtocol>", g_upnp_port);
    upnp_soap("DeletePortMapping", args, &err);
    logf_("upnp: regla del puerto %d quitada del router (error=%d)", g_upnp_port, err);
}

/* v1.9.1: Codigo = IP publica del host como numero, en decimal de 10 digitos (con ceros a la izquierda).
 * El puerto ya NO viaja en el codigo: los dos usan el de config.txt (7779 por defecto). */
static void code_encode(uint32_t ip, int port, char *out)
{
    uint32_t v = ip;
    int i;
    (void)port;
    for (i = 9; i >= 0; i--) { out[i] = (char)('0' + (int)(v % 10)); v /= 10; }
    out[10] = 0;
}

static int code_decode(const char *code, uint32_t *ip, int *port)
{
    uint64_t v = 0;
    int i, n = 0;
    for (i = 0; code[i]; i++) {
        if (code[i] < '0' || code[i] > '9') continue;     /* ignora espacios y guiones */
        v = v * 10 + (uint64_t)(code[i] - '0');
        n++;
        if (n > 10) return 0;
    }
    if (n != 10 || v >= (1ULL << 32)) return 0;
    *ip = (uint32_t)v;
    *port = g_cfg.port;
    return *port > 0 && *ip != 0;
}

/* "1234567890" -> "123 456 7890" (solo para mostrarlo en pantalla) */
static void code_pretty(const char *code, char *out)
{
    int i, k = 0;
    for (i = 0; code[i] && i < 10; i++) {
        if (i == 3 || i == 6) out[k++] = ' ';
        out[k++] = code[i];
    }
    out[k] = 0;
}

/* ------------------------------------------------------------------ */
/* Handshake                                                           */
/* ------------------------------------------------------------------ */
static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int clamp_i(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int handshake_host(int *outD)
{
    Msg m;
    uint32_t rtt[5];
    int i, D, r;

    r = net_recv(&m, 10000);
    if (r <= 0 || m.type != M_HELLO) { g_err = "EL AMIGO NO RESPONDIO"; return 0; }
    if (m.frame != PROTO_VER) {
        memset(&m, 0, sizeof m); m.type = M_REJECT; m.v1 = 1; net_send(&m);
        g_err = "VERSIONES DISTINTAS DE ARCADE STATION"; return 0;
    }
    if (m.v1 != g_romcrc) {
        memset(&m, 0, sizeof m); m.type = M_REJECT; m.v1 = 2; net_send(&m);
        g_err = "ROM DISTINTA A LA DEL AMIGO"; return 0;
    }
    memset(&m, 0, sizeof m); m.type = M_HELLO; m.frame = PROTO_VER; m.v1 = g_romcrc;
    if (!net_send(&m)) { g_err = "SE CORTO LA CONEXION"; return 0; }

    for (i = 0; i < 5; i++) {
        uint32_t t0 = now_ms();
        memset(&m, 0, sizeof m); m.type = M_PING; m.v1 = (uint32_t)i;
        if (!net_send(&m)) { g_err = "SE CORTO LA CONEXION"; return 0; }
        r = net_recv(&m, 5000);
        if (r <= 0 || m.type != M_PONG || m.v1 != (uint32_t)i) { g_err = "FALLO LA MEDICION DE LATENCIA"; return 0; }
        rtt[i] = now_ms() - t0;
    }
    qsort(rtt, 5, sizeof rtt[0], cmp_u32);
    D = g_cfg.delay > 0 ? g_cfg.delay : clamp_i((int)((rtt[2] / 2 + 16) / 17) + 2, 3, 12);
    logf_("RTT mediana %u ms -> delay %d frames", (unsigned)rtt[2], D);

    memset(&m, 0, sizeof m); m.type = M_CFG; m.v1 = (uint32_t)D; m.v2 = rtt[2];
    if (!net_send(&m)) { g_err = "SE CORTO LA CONEXION"; return 0; }
    memset(&m, 0, sizeof m); m.type = M_START;
    if (!net_send(&m)) { g_err = "SE CORTO LA CONEXION"; return 0; }
    *outD = D;
    return 1;
}

static int handshake_client(int *outD)
{
    Msg m;
    int r, D = 0, haveD = 0;
    uint32_t t0;

    memset(&m, 0, sizeof m); m.type = M_HELLO; m.frame = PROTO_VER; m.v1 = g_romcrc;
    if (!net_send(&m)) { g_err = "SE CORTO LA CONEXION"; return 0; }
    r = net_recv(&m, 10000);
    if (r <= 0) { g_err = "EL HOST NO RESPONDIO"; return 0; }
    if (m.type == M_REJECT) { g_err = (m.v1 == 2) ? "ROM DISTINTA A LA DEL HOST" : "VERSIONES DISTINTAS DE ARCADE STATION"; return 0; }
    if (m.type != M_HELLO) { g_err = "RESPUESTA INVALIDA DEL HOST"; return 0; }

    t0 = now_ms();
    while (now_ms() - t0 < 20000) {
        r = net_recv(&m, 2000);
        if (r < 0) { g_err = "SE CORTO LA CONEXION"; return 0; }
        if (r == 0) continue;
        if (m.type == M_PING) {
            Msg p; memset(&p, 0, sizeof p); p.type = M_PONG; p.v1 = m.v1;
            if (!net_send(&p)) { g_err = "SE CORTO LA CONEXION"; return 0; }
        } else if (m.type == M_CFG) {
            D = (int)m.v1; haveD = 1;
        } else if (m.type == M_START && haveD) {
            *outD = clamp_i(D, 1, 20);
            return 1;
        }
    }
    g_err = "TIEMPO AGOTADO EN EL INICIO";
    return 0;
}

/* v1.8: COPIA de handshake_host / handshake_client para el modo de 4 (protocolo PROTO_VER4). Misma logica. */
static int handshake_host4(int *outD)
{
    Msg m;
    uint32_t rtt[5];
    int i, D, r;

    r = net_recv(&m, 10000);
    if (r <= 0 || m.type != M_HELLO) { g_err = "EL AMIGO NO RESPONDIO"; return 0; }
    if (m.frame != PROTO_VER4) {
        memset(&m, 0, sizeof m); m.type = M_REJECT; m.v1 = 1; net_send(&m);
        g_err = "AMIGO EN OTRO MODO O VERSION DISTINTA"; return 0;
    }
    if (m.v1 != g_romcrc) {
        memset(&m, 0, sizeof m); m.type = M_REJECT; m.v1 = 2; net_send(&m);
        g_err = "ROM DISTINTA A LA DEL AMIGO"; return 0;
    }
    memset(&m, 0, sizeof m); m.type = M_HELLO; m.frame = PROTO_VER4; m.v1 = g_romcrc;
    if (!net_send(&m)) { g_err = "SE CORTO LA CONEXION"; return 0; }

    for (i = 0; i < 5; i++) {
        uint32_t t0 = now_ms();
        memset(&m, 0, sizeof m); m.type = M_PING; m.v1 = (uint32_t)i;
        if (!net_send(&m)) { g_err = "SE CORTO LA CONEXION"; return 0; }
        r = net_recv(&m, 5000);
        if (r <= 0 || m.type != M_PONG || m.v1 != (uint32_t)i) { g_err = "FALLO LA MEDICION DE LATENCIA"; return 0; }
        rtt[i] = now_ms() - t0;
    }
    qsort(rtt, 5, sizeof rtt[0], cmp_u32);
    D = g_cfg.delay > 0 ? g_cfg.delay : clamp_i((int)((rtt[2] / 2 + 16) / 17) + 2, 3, 12);
    logf_("RTT mediana %u ms -> delay %d frames", (unsigned)rtt[2], D);

    memset(&m, 0, sizeof m); m.type = M_CFG; m.v1 = (uint32_t)D; m.v2 = rtt[2]; m.a = (uint8_t)g_mode4;   /* modo multijugador (2v2 / 2v1 / 1v2) */
    if (!net_send(&m)) { g_err = "SE CORTO LA CONEXION"; return 0; }
    memset(&m, 0, sizeof m); m.type = M_START;
    if (!net_send(&m)) { g_err = "SE CORTO LA CONEXION"; return 0; }
    *outD = D;
    logf_("multijugador: modo %d (0=2v2 1=2v1 2=1v2)", g_mode4);
    return 1;
}

static int handshake_client4(int *outD)
{
    Msg m;
    int r, D = 0, haveD = 0;
    uint32_t t0;

    memset(&m, 0, sizeof m); m.type = M_HELLO; m.frame = PROTO_VER4; m.v1 = g_romcrc;
    if (!net_send(&m)) { g_err = "SE CORTO LA CONEXION"; return 0; }
    r = net_recv(&m, 10000);
    if (r <= 0) { g_err = "EL HOST NO RESPONDIO"; return 0; }
    if (m.type == M_REJECT) { g_err = (m.v1 == 2) ? "ROM DISTINTA A LA DEL HOST" : "ESA SALA NO ES DE 4 (O VERSION DISTINTA)"; return 0; }
    if (m.type != M_HELLO) { g_err = "RESPUESTA INVALIDA DEL HOST"; return 0; }

    t0 = now_ms();
    while (now_ms() - t0 < 20000) {
        r = net_recv(&m, 2000);
        if (r < 0) { g_err = "SE CORTO LA CONEXION"; return 0; }
        if (r == 0) continue;
        if (m.type == M_PING) {
            Msg p; memset(&p, 0, sizeof p); p.type = M_PONG; p.v1 = m.v1;
            if (!net_send(&p)) { g_err = "SE CORTO LA CONEXION"; return 0; }
        } else if (m.type == M_CFG) {
            D = (int)m.v1; haveD = 1; g_mode4 = m.a <= 2 ? (int)m.a : 0;
        } else if (m.type == M_START && haveD) {
            *outD = clamp_i(D, 1, 20);
            logf_("multijugador: modo %d (0=2v2 1=2v1 2=1v2)", g_mode4);
            return 1;
        }
    }
    g_err = "TIEMPO AGOTADO EN EL INICIO";
    return 0;
}

/* ------------------------------------------------------------------ */
/* Bucle de partida                                                    */
/* ------------------------------------------------------------------ */
static uint32_t g_run_hash, g_frames_done;
static uint32_t g_maxframes;              /* 0 = sin limite (solo la version de pruebas lo usa) */
static int      g_desyncs;
static uint32_t g_first_desync;

static uint16_t local_bits(int port, int slot, uint32_t frame)
{
#ifdef NESNET_HOST
    (void)port;
    return script_input(slot, frame);
#else
    (void)slot; (void)frame;
    return pad_nes_bits(port);
#endif
}

/* v1.7: mini menu de salida (se abre con R2). Devuelve 1 si confirma salir (X), 0 si sigue jugando (O).
 * Dibuja encima de la imagen del juego y la restaura al cerrar. */
static uint32_t g_fb_save[SCR_W * SCR_H];

static int exit_menu(int online) __attribute__((unused));
static int exit_menu(int online)
{
    int r = -1, x, y;
    memcpy(g_fb_save, g_fb, sizeof g_fb);
    g_stage = "menu-salir";
    ui_prime();
    while (g_running && r < 0) {
        uint16_t e;
        pump();
        e = ui_edges();
        if (e & UI_OK) r = 1;
        else if (e & UI_CANCEL) r = 0;
        for (y = 76; y < 164; y++)
            for (x = 20; x < 236; x++)
                g_fb[y * SCR_W + x] = (y < 78 || y >= 162 || x < 22 || x >= 234) ? 0xFFFFFFFF : 0xFF101838;
        ui_center(90, "SALIR DE LA PARTIDA?", 1, 0xFFFFFFFF);
        ui_center(116, "X: SI      O: NO", 1, 0xFFFFD040);
        if (online) ui_center(138, "TERMINA LA PARTIDA PARA LOS DOS", 1, 0xFFFFC040);
        present();
#ifdef NESNET_HOST
        sleep_ms(16);
#endif
    }
    memcpy(g_fb, g_fb_save, sizeof g_fb);
    g_stage = "juego";
    return r == 1;
}

/* v1.9.2: mientras hay una partida online abierta, los menus (R2 y el selector de juego) mandan un aviso corto (M_PING, que
 * la partida ignora) cada ~1 s: el otro lado solo se rinde tras 15 s sin recibir nada, y un menu abierto mas tiempo lo cortaba. */
static int      g_ka_on;                 /* 1 = hay partida online abierta */
static uint32_t g_ka_last;
static void ui_keepalive(void)
{
    uint32_t now;
    if (!g_ka_on || g_net < 0) return;
    now = now_ms();
    if (now - g_ka_last < 1000) return;
    g_ka_last = now;
    { Msg k; memset(&k, 0, sizeof k); k.type = M_PING; k.v1 = 0xFFFFFFFFu; (void)net_send(&k); }
}

static int ui_rom_select(int pick_only);     /* v1.9.2: definido mas abajo; con pick_only = 1 solo elige (no carga) */
static void ui_show(const char *l1, const char *l2, const char *l3);

enum { MK_CONT, MK_SAVE, MK_LOAD, MK_GAME, MK_RESET, MK_EXIT };

/* v1.9.1: menu de R2.
 *   SOLO:   CONTINUAR / GUARDAR ESTADO / CARGAR ESTADO / REINICIAR EL JUEGO / SALIR DE LA PARTIDA
 *   ONLINE: CONTINUAR / REINICIAR EL JUEGO / SALIR DE LA PARTIDA (reiniciar y salir valen para los dos; sin estados)
 *   v1.9.2: el HOST en online tiene ademas CAMBIAR DE JUEGO (despues de CONTINUAR).
 *   v1.9.4: el HOST en online tiene tambien GUARDAR ESTADO / CARGAR ESTADO (6 ranuras, las mismas que en SOLO); el invitado no.
 * Reiniciar, salir, cargar y sobreescribir piden confirmar con X.
 * Devuelve 0 = seguir jugando, 1 = salir, 2 = reiniciar, 3 = cambiar de juego, 10+n = guardar en la ranura n, 20+n = cargar la ranura n (n = 0..STATE_SLOTS-1). */
static int solo_menu(int online, int host)
{
    int kinds[6], nk = 0, sel = 0, r = -1, confirm = -1, x, y, i;
    int st = !online || host;                 /* v1.9.4: online, solo el HOST ve guardar/cargar estado */
    int ex[STATE_SLOTS];
    const char *note = NULL;
    kinds[nk++] = MK_CONT;
    if (st) { kinds[nk++] = MK_SAVE; kinds[nk++] = MK_LOAD; }
    if (online && host) kinds[nk++] = MK_GAME;
    kinds[nk++] = MK_RESET; kinds[nk++] = MK_EXIT;
    for (i = 0; i < STATE_SLOTS; i++) ex[i] = st ? state_exists(i) : 0;
    memcpy(g_fb_save, g_fb, sizeof g_fb);
    g_bd_dirty = 1;
    g_stage = "menu-solo";
    ui_prime();
    while (g_running && r < 0) {
        uint16_t e;
        int k;
        pump();
        ui_keepalive();
        e = ui_edges();
        if (confirm >= 0) {
            if (e & UI_OK) {
                if (confirm == MK_RESET) r = 2;
                else if (confirm == MK_EXIT) r = 1;
                else if (confirm == MK_SAVE) r = 10 + g_state_slot;
                else r = 20 + g_state_slot;
            } else if (e & UI_CANCEL) confirm = -1;
        } else {
            if (e & UI_UP)   { sel = (sel + nk - 1) % nk; note = NULL; }
            if (e & UI_DOWN) { sel = (sel + 1) % nk; note = NULL; }
            k = kinds[sel];
            if ((k == MK_SAVE || k == MK_LOAD) && (e & (UI_LEFT | UI_RIGHT))) {
                g_state_slot = (g_state_slot + ((e & UI_RIGHT) ? 1 : STATE_SLOTS - 1)) % STATE_SLOTS;
                note = NULL;
            }
            if (e & UI_CANCEL) r = 0;
            else if (e & (UI_OK | UI_START)) {
                if (k == MK_CONT) r = 0;
                else if (k == MK_SAVE) { if (ex[g_state_slot]) confirm = MK_SAVE; else r = 10 + g_state_slot; }
                else if (k == MK_LOAD) { if (ex[g_state_slot]) confirm = MK_LOAD; else note = "ESA RANURA ESTA VACIA"; }
                else if (k == MK_GAME) r = 3;          /* el selector de juegos hace de confirmacion (O: volver) */
                else confirm = k;
            }
        }
        if (hr_usable()) hr_backdrop(g_fb_save);     /* v1.9.7: juego oscurecido + cuadro, a 1280x720 */
        else
        for (y = 46; y < 198; y++)
            for (x = 20; x < 236; x++)
                g_fb[y * SCR_W + x] = (y < 48 || y >= 196 || x < 22 || x >= 234) ? 0xFFFFFFFF : 0xFF101838;
        if (g_hr_on) {
            static const HHint hs[2] = { { "X", "ELEGIR", HC_BX }, { "O", "SEGUIR", HC_BO } };
            static const HHint hc[2] = { { "X", "SÍ", HC_BX }, { "O", "NO", HC_BO } };
            if (confirm >= 0) {
                char l[48];
                const char *m2 = NULL, *m3 = NULL;
                uint32_t c2 = HC_DIM;
                if (confirm == MK_RESET) { snprintf(l, sizeof l, "¿REINICIAR EL JUEGO?"); m2 = online ? "Se reinicia para los dos" : "Lo guardado no se borra"; if (online) c2 = HC_AMBER; }
                else if (confirm == MK_EXIT) { snprintf(l, sizeof l, "¿SALIR DE LA PARTIDA?"); if (online) { m2 = "Termina la partida para los dos"; c2 = HC_AMBER; } }
                else if (confirm == MK_SAVE) snprintf(l, sizeof l, "¿SOBREESCRIBIR LA RANURA %d?", g_state_slot + 1);
                else {
                    snprintf(l, sizeof l, "¿CARGAR LA RANURA %d?", g_state_slot + 1);
                    m2 = online ? "Se pierde lo que llevan los dos" : "Se pierde lo que llevas"; c2 = HC_AMBER;
                    if (online) m3 = "Se envía al amigo (tarda unos segundos)";
                }
                hr_textc(640, 330, l, FS_L, HC_WHITE);
                if (m2) hr_textc(640, 392, m2, FS_M, c2);
                if (m3) hr_textc(640, 436, m3, FS_S, HC_DIM);
                hr_hints(hc, 2);
            } else {
                const char *rows[6];
                char rl[6][48];
                const char *hl = NULL;
                for (i = 0; i < nk; i++) {
                    switch (kinds[i]) {
                    case MK_CONT:  snprintf(rl[i], sizeof rl[i], "CONTINUAR"); break;
                    case MK_SAVE:  snprintf(rl[i], sizeof rl[i], "GUARDAR ESTADO < %d >", g_state_slot + 1); break;
                    case MK_LOAD:  snprintf(rl[i], sizeof rl[i], "CARGAR ESTADO < %d >%s", g_state_slot + 1, ex[g_state_slot] ? "" : " (vacía)"); break;
                    case MK_GAME:  snprintf(rl[i], sizeof rl[i], "CAMBIAR DE JUEGO"); break;
                    case MK_RESET: snprintf(rl[i], sizeof rl[i], "REINICIAR EL JUEGO"); break;
                    default:       snprintf(rl[i], sizeof rl[i], "SALIR DE LA PARTIDA"); break;
                    }
                    rows[i] = rl[i];
                }
                hr_text(322, 176, "MENÚ", FS_H, HC_YEL, 3);
                hr_rows(312, 200, 656, rows, nk, sel, 0, 56);
                if (kinds[sel] == MK_SAVE || kinds[sel] == MK_LOAD) hl = "IZQ/DER: cambiar de ranura";
                else if (online && kinds[sel] == MK_GAME) hl = "Se le pregunta antes a tu amigo";
                else if (online) hl = "Reiniciar y salir: para los dos";
                if (note) hr_text(322, 580, "Esa ranura está vacía", FS_S, HC_AMBER, 2);
                else if (hl) hr_text(322, 580, hl, FS_S, HC_DIM, 2);
                hr_hints(hs, 2);
            }
        } else if (confirm >= 0) {
            char l[40];
            if (confirm == MK_RESET) {
                ui_center(88, "REINICIAR EL JUEGO?", 1, 0xFFFFFFFF);
                ui_center(108, online ? "SE REINICIA PARA LOS DOS" : "LO GUARDADO NO SE BORRA", 1, online ? 0xFFFFC040 : 0xFFC0C0FF);
            } else if (confirm == MK_EXIT) {
                ui_center(88, "SALIR DE LA PARTIDA?", 1, 0xFFFFFFFF);
                if (online) ui_center(108, "TERMINA LA PARTIDA PARA LOS DOS", 1, 0xFFFFC040);
            } else if (confirm == MK_SAVE) {
                snprintf(l, sizeof l, "SOBREESCRIBIR LA RANURA %d?", g_state_slot + 1);
                ui_center(88, l, 1, 0xFFFFFFFF);
            } else {
                snprintf(l, sizeof l, "CARGAR LA RANURA %d?", g_state_slot + 1);
                ui_center(88, l, 1, 0xFFFFFFFF);
                ui_center(108, online ? "SE PIERDE LO QUE LLEVAN LOS DOS" : "SE PIERDE LO QUE LLEVAS", 1, 0xFFFFC040);
                if (online) ui_center(120, "SE ENVIA AL AMIGO (TARDA UNOS SEGUNDOS)", 1, 0xFFC0C0FF);
            }
            ui_center(136, "X: SI      O: NO", 1, 0xFFFFD040);
        } else {
            ui_center(56, "MENU", 1, 0xFFFFD040);
            for (i = 0; i < nk; i++) {
                char l[48];
                switch (kinds[i]) {
                case MK_CONT:  snprintf(l, sizeof l, "%s", "CONTINUAR"); break;
                case MK_SAVE:  snprintf(l, sizeof l, "GUARDAR ESTADO < %d >", g_state_slot + 1); break;
                case MK_LOAD:  snprintf(l, sizeof l, "CARGAR ESTADO < %d >%s", g_state_slot + 1, ex[g_state_slot] ? "" : " (VACIA)"); break;
                case MK_GAME:  snprintf(l, sizeof l, "%s", "CAMBIAR DE JUEGO"); break;
                case MK_RESET: snprintf(l, sizeof l, "%s", "REINICIAR EL JUEGO"); break;
                default:       snprintf(l, sizeof l, "%s", "SALIR DE LA PARTIDA"); break;
                }
                {
                    char l2[56];
                    snprintf(l2, sizeof l2, "%s %s", i == sel ? ">" : " ", l);
                    ui_center(68 + i * 14, l2, 1, i == sel ? 0xFFFFFFFF : 0xFF8080A0);
                }
            }
            if (note) ui_center(154, note, 1, 0xFFFFC040);
            ui_center(168, "ARRIBA/ABAJO + X   O: SEGUIR", 1, 0xFF8080A0);
            if (kinds[sel] == MK_SAVE || kinds[sel] == MK_LOAD) ui_center(181, "IZQ/DER: CAMBIAR DE RANURA", 1, 0xFF8080A0);
            else if (online && kinds[sel] == MK_GAME) ui_center(181, "SE LE PREGUNTA ANTES A TU AMIGO", 1, 0xFF8080A0);
            else if (online) ui_center(181, "REINICIAR Y SALIR: PARA LOS DOS", 1, 0xFF8080A0);
        }
        present();
#ifdef NESNET_HOST
        sleep_ms(16);
#endif
    }
    memcpy(g_fb, g_fb_save, sizeof g_fb);
    g_stage = "juego";
    return r < 0 ? 0 : r;
}

/* role: 0 = host (P1), 1 = cliente (P2), -1 = local directo (2 mandos),
 *       -2 = referencia de pruebas: local con el mismo retraso D     */
/* v1.9: info de red en partida (solo dibujo; no toca la red). Mide cuanto tarda cada cuadro: si el juego tiene que
 * esperar al otro lado, el cuadro tarda mas de 24 ms (a 60 cuadros por segundo lo normal son 17 ms). */
static uint32_t g_ni_last, g_ni_t0, g_ni_n, g_ni_stall, g_ni_fps, g_ni_st;

static void netinfo_tick(void)
{
    uint32_t now = now_ms();
    if (g_ni_last && now - g_ni_last > 24 && now - g_ni_last < 1000) g_ni_stall++;   /* >1 s = menu abierto, no cuenta */
    g_ni_last = now;
    g_ni_n++;
    if (g_ni_t0 == 0 || now - g_ni_t0 >= 1000) {
        if (g_ni_t0) { g_ni_fps = g_ni_n; g_ni_st = g_ni_stall; }
        g_ni_n = 0; g_ni_stall = 0; g_ni_t0 = now;
    }
}

/* v1.9.2: FPS abajo a la derecha (OPCIONES > FPS EN PANTALLA). Usa la misma medicion de netinfo_tick (cuadros por segundo reales,
 * incluida la espera por el otro lado); el primer segundo sale "--". Solo dibujo: no toca la red ni el juego. */
static void ui_fps(void)
{
    char fp[16];
    int x0, x1, y0, y1, x, y;
    if (g_ni_fps) snprintf(fp, sizeof fp, "FPS %u", (unsigned)g_ni_fps); else snprintf(fp, sizeof fp, "FPS --");
    x0 = SCR_W - (int)strlen(fp) * 6 - 5; x1 = SCR_W; y0 = SCR_H - 13; y1 = SCR_H;        /* recuadro negro para que se lea sobre cualquier juego */
    for (y = y0; y < y1; y++) for (x = x0; x < x1; x++) g_fb[y * SCR_W + x] = 0xFF000000u;
    ui_text(SCR_W - (int)strlen(fp) * 6 - 3, SCR_H - 12, fp, 1, 0xFF80FF80);
}

static int netinfo_show(uint32_t frame)
{
    if (g_desyncs) return 1;                 /* el aviso rojo de desync siempre sale */
    if (g_netinfo == 2) return 0;
    if (g_netinfo == 1) return 1;
    return frame < 300;                      /* por defecto: como siempre, los primeros ~5 s */
}

/* v1.9.1: reinicios sincronizados. Quien pide reiniciar manda M_RESET con frame = F (= su cuadro actual + D) ANTES de su
 * entrada de ese cuadro; por el orden del TCP el otro lado lo lee antes de poder pasar del cuadro F, asi los dos llaman a
 * retro_reset() justo antes de emular el cuadro F. Si piden los dos a la vez, cada reinicio tiene su propio F. */
#define RST_MAX 4
static void rst_add(uint32_t *rst, uint32_t f)
{
    int k;
    for (k = 0; k < RST_MAX; k++) if (rst[k] == f) return;
    for (k = 0; k < RST_MAX; k++) if (rst[k] == NO_FRAME) { rst[k] = f; return; }
}
static int rst_due(uint32_t *rst, uint32_t frame)
{
    int k, due = 0;
    for (k = 0; k < RST_MAX; k++) if (rst[k] == frame) { rst[k] = NO_FRAME; due = 1; }
    return due;
}
#ifdef NESNET_HOST
static uint32_t g_ref_reset[2] = { NO_FRAME, NO_FRAME };   /* pruebas: cuadros en que la referencia reinicia (NESNET_RESET_REF=a,b) */
#endif

/* ------------------------------------------------------------------ */
/* v1.9.2: CAMBIAR DE JUEGO en partida online (lo pide el HOST desde R2; vale para 1vs1 y para el modo de 4).        */
/*  1) El host elige la ROM en el selector (no la carga) y manda GM_ASK con su CRC32 y tamano. La partida SIGUE.        */
/*  2) El otro lado busca esa ROM en su carpeta roms (por CRC32, no por nombre) y contesta GM_HAVE o GM_MISS.          */
/*     Si no la tiene, el host avisa en pantalla y nadie cambia nada.                                                  */
/*  3) Con GM_HAVE el host manda GM_GO con el cuadro de corte S = su cuadro + D + 1 (igual que el reinicio de la        */
/*     v1.9.1: por el orden del TCP el otro lado lo lee antes de poder pasar de S) y los dos terminan la partida en    */
/*     ese cuadro SIN avisar de salida. Despues cada lado manda GM_MARK y descarta lo que llegue hasta recibir la del    */
/*     otro (entradas viejas ya adelantadas), carga la ROM, comprueba el CRC32 y empieza una partida nueva con el mismo  */
/*     delay D (sin repetir el handshake). Lo de antes (entradas, desync, reinicio) no cambia.                           */
/* ------------------------------------------------------------------ */
typedef struct {
    int      asked;      /* host: ya mando la pregunta y espera la respuesta */
    uint32_t asked_ms;   /* host: cuando la mando */
    int      have;       /* el que se une: ya contesto que si tiene la ROM */
    uint32_t go;         /* cuadro del corte (NO_FRAME = ninguno) */
    int      mark_rx;    /* ya llego la marca GM_MARK del otro lado (el bucle de la partida la puede leer antes de salir) */
} GameSw;
static GameSw   g_gsw;
static char     g_sw_name[ROM_NAMELEN];   /* ROM nueva: nombre del archivo en roms/ */
static uint32_t g_sw_crc, g_sw_size;
static int      g_sw_pending;             /* 1 = el bucle termino por un cambio de juego: run_online / run_online4 hacen el corte */
static char     g_pick_name[ROM_NAMELEN]; /* resultado de ui_rom_select(1) */
static uint32_t g_pick_crc, g_pick_size;

/* Busca en USRDIR/roms la ROM con ese CRC32 y tamano (solo lee los archivos que tienen ese tamano). 1 = la encontro */
static int rom_find_crc(uint32_t crc, uint32_t size, char *name_out)
{
    int i;
    roms_scan();
    for (i = 0; i < g_nroms; i++) {
        uint8_t *b = NULL;
        size_t bs = 0;
        char rp[420];
        uint32_t c;
        if (g_roms[i].size != size) continue;
        snprintf(rp, sizeof rp, "%s/roms/%s", g_dir, g_roms[i].name);
        if (!read_whole_file(rp, &b, &bs)) continue;
        c = crc32_buf(b, bs);
        free(b);
        if (c == crc && bs == size) { snprintf(name_out, ROM_NAMELEN, "%s", g_roms[i].name); return 1; }
    }
    return 0;
}

/* Atiende un M_GAME que llega durante la partida. role: 0 = host, 1 = el que se une; frame = cuadro actual de este lado.
 * 0 = fallo al mandar la respuesta (el que llama lo trata como conexion caida) */
static int gsw_on_msg(const Msg *m, int role, uint32_t frame, int D)
{
    Msg r;
    memset(&r, 0, sizeof r);
    r.type = M_GAME;
    if (role == 1 && m->a == GM_ASK) {
        char nm[ROM_NAMELEN];
        r.v1 = m->v1;
        if (g_gsw.go == NO_FRAME && rom_find_crc(m->v1, m->v2, nm)) {
            snprintf(g_sw_name, sizeof g_sw_name, "%s", nm);
            g_sw_crc = m->v1; g_sw_size = m->v2; g_gsw.have = 1;
            r.a = GM_HAVE;
            logf_("cambio de juego: tengo la ROM %s (crc32 %08X)", nm, (unsigned)m->v1);
        } else {
            r.a = GM_MISS;
            logf_("cambio de juego: NO tengo la ROM (crc32 %08X)", (unsigned)m->v1);
        }
        return net_send(&r);
    }
    if (role == 0 && m->a == GM_HAVE && g_gsw.asked && m->v1 == g_sw_crc) {
        uint32_t S = frame + (uint32_t)D + 1;
        g_gsw.asked = 0; g_gsw.go = S;
        r.a = GM_GO; r.frame = S; r.v1 = g_sw_crc;
        logf_("cambio de juego: el amigo tiene la ROM; los dos cambian a %s en el frame %u (ahora %u)", g_sw_name, (unsigned)S, (unsigned)frame);
        return net_send(&r);
    }
    if (role == 0 && m->a == GM_MISS && g_gsw.asked && m->v1 == g_sw_crc) {
        g_gsw.asked = 0;
        toast("TU AMIGO NO TIENE ESA ROM");
        logf_("cambio de juego: el amigo NO tiene la ROM %s", g_sw_name);
        return 1;
    }
    if (m->a == GM_MARK) {
        /* El otro lado ya salio de su partida. Despues de su marca no manda NADA hasta recibir la mia, asi que no hay datos
         * de la partida nueva mezclados con los de la vieja. Solo se anota: game_switch_apply no la espera de nuevo. */
        g_gsw.mark_rx = 1;
        return 1;
    }
    if (role == 1 && m->a == GM_GO && g_gsw.have && m->v1 == g_sw_crc) {
        g_gsw.have = 0; g_gsw.go = m->frame;
        logf_("cambio de juego: el host manda cambiar a %s en el frame %u (ahora %u)", g_sw_name, (unsigned)m->frame, (unsigned)frame);
        return 1;
    }
    return 1;
}

/* Host: abre el selector de juegos y, si elige uno distinto del actual, le pregunta al amigo. 0 = fallo al mandar */
static int gsw_ask_host(void)
{
    Msg q;
    if (g_gsw.asked || g_gsw.go != NO_FRAME) { toast("ESPERA: YA SE LE PREGUNTO"); return 1; }
#ifdef NESNET_HOST
    if (getenv("NESNET_STALL_MS")) {                              /* prueba: menu abierto mucho rato (comprueba el aviso de ui_keepalive) */
        uint32_t t0 = now_ms(), lim = (uint32_t)atoi(getenv("NESNET_STALL_MS"));
        while (g_running && now_ms() - t0 < lim) { pump(); ui_keepalive(); sleep_ms(50); }
    }
#endif
    if (!ui_rom_select(1)) return 1;                              /* volvio sin elegir */
    if (g_pick_crc == g_romcrc) { toast("YA ESTAS CON ESE JUEGO"); return 1; }
    snprintf(g_sw_name, sizeof g_sw_name, "%s", g_pick_name);
    g_sw_crc = g_pick_crc; g_sw_size = g_pick_size;
    memset(&q, 0, sizeof q);
    q.type = M_GAME; q.a = GM_ASK; q.v1 = g_sw_crc; q.v2 = g_sw_size;
    g_gsw.asked = 1; g_gsw.asked_ms = now_ms();
    toast("PREGUNTANDO A TU AMIGO...");
    logf_("cambio de juego: pregunto por %s (%u bytes, crc32 %08X)", g_sw_name, (unsigned)g_sw_size, (unsigned)g_sw_crc);
    return net_send(&q);
}

/* Despues de que session_loop termino por un cambio de juego (g_sw_pending), en las DOS consolas: marca de fin de lo viejo
 * (descarta lo que llegue hasta recibir la del otro), carga la ROM nueva y comprueba su CRC32. 1 = listo para la partida nueva */
static int game_switch_apply(void)
{
    Msg m;
    uint32_t t0 = now_ms();
    int i;
    ui_show("CAMBIANDO DE JUEGO...", g_sw_name, NULL);
    memset(&m, 0, sizeof m); m.type = M_GAME; m.a = GM_MARK;
    if (!net_send(&m)) { g_err = "SE PERDIO LA CONEXION"; return 0; }
    while (!g_gsw.mark_rx) {                                      /* si la marca ya llego durante la partida, no hay nada que esperar */
        int r = net_recv(&m, 100);
        if (r < 0) { g_err = "SE PERDIO LA CONEXION"; return 0; }
        if (r > 0) {
            if (m.type == M_GAME && m.a == GM_MARK) break;
            if (m.type == M_QUIT) { g_err = "EL AMIGO SALIO DE LA PARTIDA"; return 0; }
            continue;                                             /* entradas viejas y avisos: se descartan */
        }
        if (!g_running) return 0;
        if (now_ms() - t0 > 15000) { g_err = "EL AMIGO NO RESPONDE (15 S)"; return 0; }
    }
    for (i = 0; i < g_nroms; i++) if (!strcmp(g_roms[i].name, g_sw_name)) break;
    if (i >= g_nroms) { roms_scan(); for (i = 0; i < g_nroms; i++) if (!strcmp(g_roms[i].name, g_sw_name)) break; }
    if (i >= g_nroms || !rom_load(i)) { g_err = "NO PUDE CARGAR LA ROM NUEVA"; return 0; }
    if (g_romcrc != g_sw_crc) { g_err = "LA ROM NO ES LA DEL AMIGO"; return 0; }
    snprintf(g_last_rom, sizeof g_last_rom, "%s", g_rom_name);   /* el menu y el proximo arranque la recuerdan */
    opts_save();
    g_toast_n = 0;
    logf_("cambio de juego: listo, ROM nueva %s (crc32 %08X)", g_rom_name, (unsigned)g_romcrc);
    return 1;
}

/* ------------------------------------------------------------------ */
/* v1.9.4: GUARDAR / CARGAR ESTADO en partida online (solo el HOST; vale para 1vs1 y para el modo de 4).                 */
/* Guardar solo escribe la ranura del host (no toca la partida). Al CARGAR, el host le manda el estado al amigo           */
/* (M_STATE + datos) ANTES de su entrada de este cuadro y los dos lo cargan justo antes de emular el cuadro F = cuadro    */
/* actual + D (mismo truco que el reinicio sincronizado). Mientras llegan los datos los dos esperan (el juego se detiene  */
/* unos segundos); no se pierde ni se adelanta ningun cuadro.                                                            */
/* ------------------------------------------------------------------ */
/* Host: lee la ranura y la manda. 1 = enviado (queda pendiente en st_in), 0 = no se pudo leer (aviso en pantalla), -1 = se corto la conexion */
static int state_online_host_load(int slot, uint32_t frame, int D, uint8_t **st_in, size_t *st_in_n, uint32_t *st_in_F, const char *stage0)
{
    uint8_t *sb = NULL;
    size_t sn = 0;
    char tb[40];
    Msg rq;
    uint32_t F = frame + (uint32_t)D, t0 = now_ms();
    int ok;
    if (!state_read(slot, &sb, &sn)) {
        snprintf(tb, sizeof tb, "NO SE PUDO CARGAR (RANURA %d)", slot + 1);
        toast(tb);
        return 0;
    }
    memset(&rq, 0, sizeof rq);
    rq.type = M_STATE; rq.frame = F; rq.v1 = (uint32_t)sn; rq.v2 = crc32_buf(sb, sn);
    g_stage = "enviando-estado";
    ok = net_send(&rq) && net_send_raw(sb, sn);
    g_stage = stage0;
    logf_("estado: ranura %d (%u bytes, crc %08X) enviado en %u ms; los dos lo cargan en el frame %u (pedido en el %u)",
          slot + 1, (unsigned)sn, (unsigned)rq.v2, (unsigned)(now_ms() - t0), (unsigned)F, (unsigned)frame);
    free(*st_in);
    *st_in = sb; *st_in_n = sn; *st_in_F = F;
    return ok ? 1 : -1;
}

/* Invitado: recibe el estado entero (nunca se pasa de largo el cuadro de carga: no puede emular m->frame sin la entrada del host,
 * que viene DESPUES de este mensaje). 1 = guardado como pendiente; 0 = algo no cuadra (g_err puesto: se termina la partida con aviso,
 * porque cargarlo en un solo lado la desincronizaria); -1 = se corto la conexion */
static int state_online_guest_recv(const Msg *m, uint32_t frame, uint8_t **st_in, size_t *st_in_n, uint32_t *st_in_F, const char *stage0)
{
    uint8_t *sb;
    if (m->v1 == 0 || m->v1 > STATE_MAX || (size_t)m->v1 != retro_serialize_size() || m->frame < frame) {
        g_err = "ESTADO DEL HOST NO VALIDO";
        return 0;
    }
    sb = (uint8_t *)malloc(m->v1);
    if (!sb) { g_err = "SIN MEMORIA PARA EL ESTADO"; return 0; }
    g_stage = "recibiendo-estado";
    if (!net_recv_raw(sb, m->v1)) { free(sb); return -1; }
    g_stage = stage0;
    if (crc32_buf(sb, m->v1) != m->v2) { free(sb); g_err = "ESTADO RECIBIDO CON ERRORES"; return 0; }
    free(*st_in);
    *st_in = sb; *st_in_n = m->v1; *st_in_F = m->frame;
    logf_("estado recibido del host: %u bytes, crc %08X; lo cargo en el frame %u (voy en el %u)", (unsigned)m->v1, (unsigned)m->v2, (unsigned)m->frame, (unsigned)frame);
    return 1;
}

/* Arcade Station: salto de dibujo automatico (SOLO y online). Ver el comentario de g_debt. En online el tiempo que se pasa
 * esperando al otro lado (g_netwait) NO cuenta como atraso propio. El hash de imagen de un cuadro saltado es 0 y no se compara. */
static uint64_t g_netwait;
static int skip_decide(void)
{
    uint64_t nw = now_us();
    int sk;
    if (g_fs_prev) {
        int64_t T = (int64_t)(nw - g_fs_prev) - (int64_t)g_netwait;
        if (T > 100000) g_debt = 0;                          /* menu, pausa o carga: no cuenta */
        else if (T > 17500) g_debt += T - 16667;             /* perdio el cuadro de pantalla */
        else { g_debt -= 16700 - T; if (g_debt < 0) g_debt = 0; }
    } else g_debt = 0;
    g_netwait = 0;
    g_fs_prev = nw;
    sk = (!g_noskip && g_debt > SKIP_ON_US && g_skip_run < SKIP_MAXRUN && !g_shot_req) ? 1 : 0;
    core_skip_draw(sk);
    if (sk) g_frame_crc = 0;
    return sk;
}
static void skip_wait(void)       /* cuadro sin dibujar: espera lo justo para pagar la deuda */
{
    int64_t want = 16667 - g_debt;
    while ((int64_t)(now_us() - g_fs_prev) < want) { pump(); usleep(300); }
    g_skip_run++; g_skipped++;
}

static int session_loop(int role, int D, uint32_t maxframes)
{
    /* v1.9.4: estado guardado pendiente de cargar en el cuadro st_in_F (el host lo lee de su ranura; el invitado lo recibe por red) */
    uint8_t *st_in = NULL;
    size_t st_in_n = 0;
    uint32_t st_in_F = NO_FRAME;
    const char *stage0;
    uint16_t loc[RING], rem[RING];
    uint8_t  have[RING];
    uint32_t mine_f[RING], mine_c[RING], rem_f[RING], rem_c[RING];
    uint32_t frame = 0, last_rx = now_ms();
    uint32_t rst[RST_MAX];
    int i, ended_ok = 1, peer_gone = 0, peer_quit = 0, user_exit = 0;
    char ov[64];

    memset(loc, 0, sizeof loc); memset(rem, 0, sizeof rem); memset(have, 0, sizeof have);
    for (i = 0; i < RING; i++) { mine_f[i] = NO_FRAME; rem_f[i] = NO_FRAME; mine_c[i] = rem_c[i] = 0; }
    for (i = 0; i < RST_MAX; i++) rst[i] = NO_FRAME;
    if (role >= 0) for (i = 0; i < D; i++) have[i] = 1;

    g_run_hash = 2166136261u; g_frames_done = 0; g_desyncs = 0; g_first_desync = NO_FRAME;
    g_stage = role == 0 ? "juego-host" : role == 1 ? "juego-cliente" : "juego-local";
    stage0 = g_stage;
    g_ingame = 1;                           /* v1.9.3: desde aqui la imagen usa la pantalla elegida en OPCIONES */
    g_need_crc = (role != -1);              /* solo en SOLO no se compara nada con otra consola: se ahorra el hash de cada cuadro */
    memset(&g_gsw, 0, sizeof g_gsw); g_gsw.go = NO_FRAME; g_sw_pending = 0;      /* v1.9.2: cambio de juego */
    g_ka_on = role >= 0; g_ka_last = now_ms();
    g_ni_t0 = 0; g_ni_last = 0; g_ni_fps = 0;

#define COMPARE(f) do { uint32_t _f = (f); \
        if (mine_f[_f % RING] == _f && rem_f[_f % RING] == _f && \
            mine_c[_f % RING] && rem_c[_f % RING] && mine_c[_f % RING] != rem_c[_f % RING]) { \
            if (!g_desyncs) { g_first_desync = _f; logf_("DESYNC en frame %u", (unsigned)_f); } \
            g_desyncs++; \
            mine_f[_f % RING] = NO_FRAME; } } while (0)

    (void)exit_pressed(1, 0);               /* ignora un R2 que siga apretado desde antes */

    (void)shot_pressed(1, 0); g_shot_req = 0;          /* v1.9.6: lo mismo con L2 (captura) */

    g_fs_prev = 0; g_debt = 0; g_skip_run = 0; g_netwait = 0;
    { char np[300]; FILE *nf; snprintf(np, sizeof np, "%s/noskip.txt", g_dir); nf = fopen(np, "r"); g_noskip = nf ? 1 : 0; if (nf) fclose(nf); logf_("salto de dibujo automatico: %s", g_noskip ? "APAGADO (noskip.txt)" : "activo"); }
    while (g_running && (maxframes == 0 || frame < maxframes)) {
        uint16_t p1, p2;
        int skip_now = 0;
        pump();
        /* v1.9.2: cambio de juego: llego el cuadro de corte (se mira ANTES de mandar la entrada de este cuadro) */
        if (g_gsw.go != NO_FRAME && frame >= g_gsw.go) { g_sw_pending = 1; goto out; }
        if (g_gsw.asked && now_ms() - g_gsw.asked_ms > 20000) { g_gsw.asked = 0; toast("TU AMIGO NO RESPONDIO"); logf_("cambio de juego: sin respuesta del amigo (20 s)"); }

        /* v1.7: R2 abre el menu de salir. Se mira antes de mandar la entrada de este frame; si sigue, el
         * otro lado simplemente espera como siempre. No cambia nada del protocolo ni del lockstep. */
        if (role != -2 && exit_pressed(0, frame) && !(st_in && st_in_F > frame)) {   /* v1.9.4: con un estado por cargarse en unos cuadros no se abre el menu */
            /* v1.9.1: SOLO = continuar / guardar y cargar estado / reiniciar / salir; online = continuar / reiniciar / salir */
            int xr = solo_menu(role >= 0, role == 0);
            if (xr == 1) { user_exit = 1; logf_("salida pedida por el jugador (R2) en frame %u", (unsigned)frame); goto out; }
            if (xr == 3 && role == 0) { if (!gsw_ask_host()) peer_gone = 1; }       /* v1.9.2: cambiar de juego (solo el host) */
            if (xr == 2) {
                if (role >= 0) {
                    Msg rq;
                    uint32_t F = frame + (uint32_t)D;
                    memset(&rq, 0, sizeof rq); rq.type = M_RESET; rq.frame = F;
                    if (!peer_gone && !net_send(&rq)) peer_gone = 1;
                    rst_add(rst, F);
                    logf_("reinicio pedido por el jugador (R2) en frame %u: los dos reinician en el frame %u", (unsigned)frame, (unsigned)F);
                } else {
                    retro_reset();
                    logf_("juego reiniciado por el jugador (R2) en frame %u", (unsigned)frame);
                }
            } else if (xr >= 10 && xr < 10 + STATE_SLOTS && (role == -1 || role == 0)) {   /* v1.9.4: el host tambien guarda en ONLINE (solo escribe su ranura: no toca la partida) */
                char tb[40];
                snprintf(tb, sizeof tb, state_save(xr - 10) ? "ESTADO GUARDADO EN RANURA %d" : "NO SE PUDO GUARDAR (RANURA %d)", xr - 9);
                toast(tb);
            } else if (xr >= 20 && xr < 20 + STATE_SLOTS && role == -1) {
                char tb[40];
                int lr = state_load(xr - 20);
                snprintf(tb, sizeof tb, lr > 0 ? "ESTADO CARGADO: RANURA %d" : "NO SE PUDO CARGAR (RANURA %d)", xr - 19);
                toast(tb);
            } else if (xr >= 20 && xr < 20 + STATE_SLOTS && role == 0) {
                /* v1.9.4: el host carga un estado EN ONLINE: se lo manda al amigo y los dos lo cargan en el mismo cuadro */
                if (!peer_gone && state_online_host_load(xr - 20, frame, D, &st_in, &st_in_n, &st_in_F, stage0) < 0) peer_gone = 1;
            }
            last_rx = now_ms();
            g_ni_t0 = 0; g_ni_last = 0;               /* v1.9.2: el FPS vuelve a medir despues del menu */
        }

        if (role != -2 && shot_pressed(0, frame)) g_shot_req = 1;        /* v1.9.6: L2 = captura (se toma despues de emular este cuadro) */
        if (role == -1) {
            p1 = local_bits(0, 0, frame);
            p2 = local_bits(1, 1, frame);
        } else if (role == -2) {
            p1 = frame >= (uint32_t)D ? local_bits(0, 0, frame - D) : 0;
            p2 = frame >= (uint32_t)D ? local_bits(0, 1, frame - D) : 0;
        } else {
            Msg m;
            uint32_t tgt = frame + (uint32_t)D;
            uint16_t bits = local_bits(0, role, frame);
            loc[tgt % RING] = bits;

            memset(&m, 0, sizeof m);
            m.type = M_INPUT; m.input = bits; m.frame = tgt;
            if (frame > 0) { m.v1 = frame - 1; m.v2 = mine_c[(frame - 1) % RING]; } else { m.v1 = NO_FRAME; }
            if (!peer_gone && !net_send(&m)) peer_gone = 1;   /* lo decide el bucle de espera */

            for (;;) {
                int r;
                if (peer_gone) {
                    /* el otro lado cerro: seguimos solo mientras queden entradas ya recibidas */
                    if (have[frame % RING]) break;
                    g_err = peer_quit ? "EL AMIGO SALIO DE LA PARTIDA" : "SE PERDIO LA CONEXION";
                    ended_ok = 0; goto out;
                }
                { int blk = !have[frame % RING]; uint64_t tw = now_us(); r = net_recv(&m, blk ? 100 : 0); if (blk) g_netwait += now_us() - tw; }
                if (r < 0) { if (!g_running) goto out; peer_gone = 1; continue; }
                if (r == 0) {
                    if (have[frame % RING]) break;
                    if (!g_running) goto out;
                    if (now_ms() - last_rx > 15000) { g_err = "EL AMIGO NO RESPONDE (15 S)"; ended_ok = 0; goto out; }
                    continue;
                }
                last_rx = now_ms();
                if (m.type == M_INPUT) {
                    rem[m.frame % RING] = m.input;
                    have[m.frame % RING] = 1;
                    if (m.v1 != NO_FRAME) { rem_f[m.v1 % RING] = m.v1; rem_c[m.v1 % RING] = m.v2; COMPARE(m.v1); }
                } else if (m.type == M_RESET) {
                    rst_add(rst, m.frame);
                } else if (m.type == M_STATE) {                     /* v1.9.4: el host manda un estado guardado */
                    int sr = state_online_guest_recv(&m, frame, &st_in, &st_in_n, &st_in_F, stage0);
                    if (sr == 0) { ended_ok = 0; goto out; }
                    if (sr < 0) { if (!g_running) goto out; peer_gone = 1; continue; }
                    last_rx = now_ms();
                } else if (m.type == M_GAME) {                      /* v1.9.2: cambio de juego */
                    if (!gsw_on_msg(&m, role, frame, D)) peer_gone = 1;
                } else if (m.type == M_QUIT) {
                    peer_quit = 1; peer_gone = 1;
                }
                if (g_gsw.go != NO_FRAME && frame >= g_gsw.go) { g_sw_pending = 1; goto out; }   /* el corte llego mientras esperaba */
            }
            have[frame % RING] = 0;
            {
                uint16_t mine = loc[frame % RING], theirs = rem[frame % RING];
                p1 = (role == 0) ? mine : theirs;
                p2 = (role == 0) ? theirs : mine;
            }
        }

        /* v1.9.1: reinicio sincronizado (o, en las pruebas de PC, el de la referencia) justo antes de emular este cuadro */
        {
            int due = rst_due(rst, frame);
#ifdef NESNET_HOST
            if (role == -2 && (frame == g_ref_reset[0] || frame == g_ref_reset[1])) due = 1;
#endif
            if (due) { retro_reset(); logf_("reinicio aplicado en el frame %u", (unsigned)frame); }
        }
        if (st_in && st_in_F == frame) {                  /* v1.9.4: estado del host (el mismo bloque en los dos lados) justo antes de este cuadro */
            int okl = retro_unserialize(st_in, st_in_n) ? 1 : 0;
            logf_("estado cargado en el frame %u: %u bytes, crc %08X %s", (unsigned)frame, (unsigned)st_in_n, (unsigned)crc32_buf(st_in, st_in_n), okl ? "OK" : "FALLO");
            toast(role == 0 ? (okl ? "ESTADO CARGADO" : "NO SE PUDO CARGAR") : "EL HOST CARGO UN ESTADO");
            free(st_in);
            st_in = NULL; st_in_n = 0; st_in_F = NO_FRAME;
        }

#ifdef NESNET_HOST
        { const char *b = getenv("NESNET_BREAK");        /* prueba: corrompe a proposito un frame */
          if (b && role == 1 && frame >= (uint32_t)atoi(b) && frame < (uint32_t)atoi(b) + 30) p1 ^= 0xFF; }
#endif
        g_vid_fresh = 0;
        if (role == -1) {                    /* Arcade Station: solo local, hasta 4 mandos segun el juego */
            g_pad[2] = local_bits(2, 2, frame);      /* siempre se leen los 4 mandos: el nucleo a veces declara menos jugadores de los reales (kod: 2 en vez de 3) */
            g_pad[3] = local_bits(3, 3, frame);
        }
        skip_now = role != -2 ? skip_decide() : 0;
        step_frame(p1, p2);
        if (g_shot_req && g_vid_fresh) { g_shot_req = 0; shot_do(); }   /* v1.9.6: captura limpia: g_fb todavia no tiene textos */

        mine_f[frame % RING] = frame;
        mine_c[frame % RING] = g_frame_crc;
        if (role >= 0) COMPARE(frame);
        g_run_hash = fnv_u32(g_run_hash, g_frame_crc);

        if (skip_now) skip_wait();
        else {
        g_skip_run = 0;
        if (role >= 0 && netinfo_show(frame)) {
            if (g_desyncs) snprintf(ov, sizeof ov, "DESYNC EN FRAME %u", (unsigned)g_first_desync);
            else if (g_netinfo == 1) snprintf(ov, sizeof ov, "P%d DELAY %d (%dMS) FPS %u TRABAS %u", role + 1, D, D * 17, (unsigned)g_ni_fps, (unsigned)g_ni_st);
            else snprintf(ov, sizeof ov, "ARCADE  P%d  DELAY %d", role + 1, D);
            ui_text(2, 2, ov, 1, g_desyncs ? 0xFFFF4040 : 0xFFFFFFFF);
        }
        if (g_toast_n > 0) { ui_text(4, 224, g_toast, 1, 0xFFFFFF40); g_toast_n--; }
        if (g_showfps && role != -2) ui_fps();                           /* v1.9.2: FPS en pantalla (SOLO y online) */
        present();
        }
        if ((role >= 0 && g_netinfo == 1) || (g_showfps && role != -2)) netinfo_tick();
        frame++;
        g_frames_done = frame;
        if (role == -1 && (frame % 300) == 0) sram_save(0);      /* v1.9: guardado de bateria cada ~5 s si cambio (solo SOLO) */
    }
out:
    core_skip_draw(0); g_fs_prev = 0; g_debt = 0; g_skip_run = 0;
    g_ingame = 0;                           /* v1.9.3: al salir de la partida, los menus vuelven a MENU_SCREEN */
    g_need_crc = 1;
    g_ka_on = 0;
    free(st_in);                            /* v1.9.4: estado pendiente que nunca llego a cargarse */
    st_in = NULL;
    if (role >= 0 && g_net >= 0 && !g_sw_pending) {       /* v1.9.2: en un cambio de juego no se avisa de salida */
        Msg q; memset(&q, 0, sizeof q); q.type = M_QUIT; net_send(&q);
        if (user_exit) {
            /* v1.7: vacia lo que el otro lado alcance a mandar (hasta 300 ms o hasta que cierre), para que
             * al cerrar el socket no se pierda el aviso de salida (cerrar con datos sin leer lo descarta) */
            uint32_t t0 = now_ms();
            while (now_ms() - t0 < 300) { if (net_recv(&q, 20) < 0) break; }
        }
    }
#undef COMPARE
    return ended_ok;
}

/* ------------------------------------------------------------------ */
/* v1.8: bucle de partida de 4 jugadores (COPIA adaptada de session_loop; el 1vs1 de arriba no se toca)  */
/* Cada consola manda en un solo mensaje sus 2 mandos: byte bajo = mando 1 de esa consola, byte alto = mando 2.   */
/* Host: mandos 1 y 2 (P1+P2). El que se une: mandos 3 y 4 (P3+P4).                                                */
/* ------------------------------------------------------------------ */
/* Arcade Station: mando 'slot' (0 o 1) de ESTA consola en el modo multijugador. Segun g_mode4 cada consola usa 1 o 2 mandos. */
static uint16_t local_pad4(int role, int slot, uint32_t frame)
{
    int n = role == 0 ? (g_mode4 == 2 ? 1 : 2) : (g_mode4 == 1 ? 1 : 2);
    if (slot >= n) return 0;
#ifdef NESNET_HOST
    return script_input((role == 1 ? 2 : 0) + slot, frame);   /* pruebas: host = jugadores 0 y 1, cliente = jugadores 2 y 3 */
#else
    (void)frame;
    return pad_nes_bits(slot);
#endif
}

/* role: 0 = host (P1+P2), 1 = el que se une (P3+P4), -2 = referencia de pruebas: 4 jugadores locales con retraso D */
static int session_loop4(int role, int D, uint32_t maxframes)
{
    /* v1.9.4: estado guardado pendiente de cargar (igual que en el 1vs1) */
    uint8_t *st_in = NULL;
    size_t st_in_n = 0;
    uint32_t st_in_F = NO_FRAME;
    const char *stage0;
    uint16_t loc[RING], rem[RING], loc2[RING], rem2[RING];
    int skip_now = 0;
    uint8_t  have[RING];
    uint32_t mine_f[RING], mine_c[RING], rem_f[RING], rem_c[RING];
    uint32_t frame = 0, last_rx = now_ms();
    uint32_t rst[RST_MAX];
    int i, ended_ok = 1, peer_gone = 0, peer_quit = 0, user_exit = 0;
    char ov[64];

    memset(loc2, 0, sizeof loc2); memset(rem2, 0, sizeof rem2);
    memset(loc, 0, sizeof loc); memset(rem, 0, sizeof rem); memset(have, 0, sizeof have);
    for (i = 0; i < RING; i++) { mine_f[i] = NO_FRAME; rem_f[i] = NO_FRAME; mine_c[i] = rem_c[i] = 0; }
    for (i = 0; i < RST_MAX; i++) rst[i] = NO_FRAME;
    if (role >= 0) for (i = 0; i < D; i++) have[i] = 1;

    g_fs_prev = 0; g_debt = 0; g_skip_run = 0; g_netwait = 0;
    { char np[300]; FILE *nf; snprintf(np, sizeof np, "%s/noskip.txt", g_dir); nf = fopen(np, "r"); g_noskip = nf ? 1 : 0; if (nf) fclose(nf); logf_("salto de dibujo automatico: %s", g_noskip ? "APAGADO (noskip.txt)" : "activo"); }
    g_run_hash = 2166136261u; g_frames_done = 0; g_desyncs = 0; g_first_desync = NO_FRAME;
    g_stage = role == 0 ? "juego4-host" : role == 1 ? "juego4-cliente" : "juego4-ref";
    stage0 = g_stage;
    g_ingame = 1;                           /* v1.9.3: desde aqui la imagen usa la pantalla elegida en OPCIONES */
    g_need_crc = (role != -1);              /* solo en SOLO no se compara nada con otra consola: se ahorra el hash de cada cuadro */
    memset(&g_gsw, 0, sizeof g_gsw); g_gsw.go = NO_FRAME; g_sw_pending = 0;      /* v1.9.2: cambio de juego */
    g_ka_on = role >= 0; g_ka_last = now_ms();
    g_ni_t0 = 0; g_ni_last = 0; g_ni_fps = 0;

#define COMPARE4(f) do { uint32_t _f = (f); \
        if (mine_f[_f % RING] == _f && rem_f[_f % RING] == _f && \
            mine_c[_f % RING] && rem_c[_f % RING] && mine_c[_f % RING] != rem_c[_f % RING]) { \
            if (!g_desyncs) { g_first_desync = _f; logf_("DESYNC en frame %u", (unsigned)_f); } \
            g_desyncs++; \
            mine_f[_f % RING] = NO_FRAME; } } while (0)

    (void)exit_pressed(1, 0);               /* ignora un R2 que siga apretado desde antes */

    (void)shot_pressed(1, 0); g_shot_req = 0;          /* v1.9.6: lo mismo con L2 (captura) */

    while (g_running && (maxframes == 0 || frame < maxframes)) {
        uint16_t p1, p2, p3, p4;
        pump();
        /* v1.9.2: cambio de juego (igual que en el 1vs1) */
        if (g_gsw.go != NO_FRAME && frame >= g_gsw.go) { g_sw_pending = 1; goto out; }
        if (g_gsw.asked && now_ms() - g_gsw.asked_ms > 20000) { g_gsw.asked = 0; toast("TU AMIGO NO RESPONDIO"); logf_("cambio de juego: sin respuesta del amigo (20 s)"); }

        /* R2 del mando 1 de esta consola abre el menu (igual que en el 1vs1): continuar / reiniciar / salir */
        if (role != -2 && exit_pressed(0, frame) && !(st_in && st_in_F > frame)) {   /* v1.9.4: con un estado por cargarse en unos cuadros no se abre el menu */
            int xr = solo_menu(1, role == 0);
            if (xr == 1) { user_exit = 1; logf_("salida pedida por el jugador (R2) en frame %u", (unsigned)frame); goto out; }
            if (xr == 3 && role == 0) { if (!gsw_ask_host()) peer_gone = 1; }       /* v1.9.2: cambiar de juego (solo el host) */
            if (xr == 2) {
                Msg rq;
                uint32_t F = frame + (uint32_t)D;
                memset(&rq, 0, sizeof rq); rq.type = M_RESET; rq.frame = F;
                if (!peer_gone && !net_send(&rq)) peer_gone = 1;
                rst_add(rst, F);
                logf_("reinicio pedido por el jugador (R2) en frame %u: los dos reinician en el frame %u", (unsigned)frame, (unsigned)F);
            } else if (xr >= 10 && xr < 10 + STATE_SLOTS && role == 0) {          /* v1.9.4: el host guarda (solo escribe su ranura) */
                char tb[40];
                snprintf(tb, sizeof tb, state_save(xr - 10) ? "ESTADO GUARDADO EN RANURA %d" : "NO SE PUDO GUARDAR (RANURA %d)", xr - 9);
                toast(tb);
            } else if (xr >= 20 && xr < 20 + STATE_SLOTS && role == 0) {          /* v1.9.4: el host carga: se lo manda al amigo y los dos lo cargan en el mismo cuadro */
                if (!peer_gone && state_online_host_load(xr - 20, frame, D, &st_in, &st_in_n, &st_in_F, stage0) < 0) peer_gone = 1;
            }
            last_rx = now_ms();
            g_ni_t0 = 0; g_ni_last = 0;               /* v1.9.2: el FPS vuelve a medir despues del menu */
        }

        if (role != -2 && shot_pressed(0, frame)) g_shot_req = 1;        /* v1.9.6: L2 = captura (se toma despues de emular este cuadro) */
        if (role == -2) {
#ifdef NESNET_HOST
            uint32_t f0 = frame >= (uint32_t)D ? frame - (uint32_t)D : 0;
            int ok0 = frame >= (uint32_t)D;
            uint16_t s0 = ok0 ? script_input(0, f0) : 0, s1 = g_mode4 == 2 ? 0 : ok0 ? script_input(1, f0) : 0;
            uint16_t s2 = ok0 ? script_input(2, f0) : 0, s3 = g_mode4 == 1 ? 0 : ok0 ? script_input(3, f0) : 0;
            if (g_mode4 == 1)      { p1 = s0; p2 = s1; p3 = s2; p4 = 0; }
            else if (g_mode4 == 2) { p1 = s0; p2 = s2; p3 = s3; p4 = 0; }
            else                   { p1 = s0; p2 = s1; p3 = s2; p4 = s3; }
#else
            p1 = p2 = p3 = p4 = 0;           /* la referencia solo existe en la version de pruebas (PC) */
#endif
        } else {
            Msg m;
            uint32_t tgt = frame + (uint32_t)D;
            uint16_t bits = local_pad4(role, 0, frame), bits2 = local_pad4(role, 1, frame);
            loc[tgt % RING] = bits; loc2[tgt % RING] = bits2;

            {   /* el segundo mando va en un mensaje aparte (antes del principal: TCP respeta el orden), cada mando con sus 16 bits */
                Msg m2; memset(&m2, 0, sizeof m2);
                m2.type = M_INPUT2; m2.input = bits2; m2.frame = tgt;
                if (!peer_gone && !net_send(&m2)) peer_gone = 1;
            }
            memset(&m, 0, sizeof m);
            m.type = M_INPUT; m.input = bits; m.frame = tgt;
            if (frame > 0) { m.v1 = frame - 1; m.v2 = mine_c[(frame - 1) % RING]; } else { m.v1 = NO_FRAME; }
            if (!peer_gone && !net_send(&m)) peer_gone = 1;   /* lo decide el bucle de espera */

            for (;;) {
                int r;
                if (peer_gone) {
                    /* el otro lado cerro: seguimos solo mientras queden entradas ya recibidas */
                    if (have[frame % RING]) break;
                    g_err = peer_quit ? "EL AMIGO SALIO DE LA PARTIDA" : "SE PERDIO LA CONEXION";
                    ended_ok = 0; goto out;
                }
                { int blk = !have[frame % RING]; uint64_t tw = now_us(); r = net_recv(&m, blk ? 100 : 0); if (blk) g_netwait += now_us() - tw; }
                if (r < 0) { if (!g_running) goto out; peer_gone = 1; continue; }
                if (r == 0) {
                    if (have[frame % RING]) break;
                    if (!g_running) goto out;
                    if (now_ms() - last_rx > 15000) { g_err = "EL AMIGO NO RESPONDE (15 S)"; ended_ok = 0; goto out; }
                    continue;
                }
                last_rx = now_ms();
                if (m.type == M_INPUT) {
                    rem[m.frame % RING] = m.input;
                    have[m.frame % RING] = 1;
                    if (m.v1 != NO_FRAME) { rem_f[m.v1 % RING] = m.v1; rem_c[m.v1 % RING] = m.v2; COMPARE4(m.v1); }
                } else if (m.type == M_INPUT2) {
                    rem2[m.frame % RING] = m.input;
                } else if (m.type == M_RESET) {
                    rst_add(rst, m.frame);
                } else if (m.type == M_STATE) {                     /* v1.9.4: el host manda un estado guardado */
                    int sr = state_online_guest_recv(&m, frame, &st_in, &st_in_n, &st_in_F, stage0);
                    if (sr == 0) { ended_ok = 0; goto out; }
                    if (sr < 0) { if (!g_running) goto out; peer_gone = 1; continue; }
                    last_rx = now_ms();
                } else if (m.type == M_GAME) {                      /* v1.9.2: cambio de juego */
                    if (!gsw_on_msg(&m, role, frame, D)) peer_gone = 1;
                } else if (m.type == M_QUIT) {
                    peer_quit = 1; peer_gone = 1;
                }
                if (g_gsw.go != NO_FRAME && frame >= g_gsw.go) { g_sw_pending = 1; goto out; }   /* el corte llego mientras esperaba */
            }
            have[frame % RING] = 0;
            {
                uint16_t mA = loc[frame % RING], mB = loc2[frame % RING], tA = rem[frame % RING], tB = rem2[frame % RING];
                uint16_t hA = role == 0 ? mA : tA, hB = role == 0 ? mB : tB;      /* mandos del host */
                uint16_t cA = role == 0 ? tA : mA, cB = role == 0 ? tB : mB;      /* mandos del que se une */
                if (g_mode4 == 1)      { p1 = hA; p2 = hB; p3 = cA; p4 = 0; }     /* 2 vs 1 */
                else if (g_mode4 == 2) { p1 = hA; p2 = cA; p3 = cB; p4 = 0; }     /* 1 vs 2 */
                else                   { p1 = hA; p2 = hB; p3 = cA; p4 = cB; }   /* 2 vs 2 */
            }
        }

        /* v1.9.1: reinicio sincronizado (igual que en el 1vs1) */
        {
            int due = rst_due(rst, frame);
#ifdef NESNET_HOST
            if (role == -2 && (frame == g_ref_reset[0] || frame == g_ref_reset[1])) due = 1;
#endif
            if (due) { retro_reset(); logf_("reinicio aplicado en el frame %u", (unsigned)frame); }
        }
        if (st_in && st_in_F == frame) {                  /* v1.9.4: estado del host justo antes de este cuadro (igual que en el 1vs1) */
            int okl = retro_unserialize(st_in, st_in_n) ? 1 : 0;
            logf_("estado cargado en el frame %u: %u bytes, crc %08X %s", (unsigned)frame, (unsigned)st_in_n, (unsigned)crc32_buf(st_in, st_in_n), okl ? "OK" : "FALLO");
            toast(role == 0 ? (okl ? "ESTADO CARGADO" : "NO SE PUDO CARGAR") : "EL HOST CARGO UN ESTADO");
            free(st_in);
            st_in = NULL; st_in_n = 0; st_in_F = NO_FRAME;
        }

#ifdef NESNET_HOST
        { const char *b = getenv("NESNET_BREAK");        /* prueba: corrompe a proposito un frame */
          if (b && role == 1 && frame >= (uint32_t)atoi(b) && frame < (uint32_t)atoi(b) + 30) p3 ^= 0xFF; }
#endif
        g_vid_fresh = 0;
        skip_now = role != -2 ? skip_decide() : 0;
        step_frame4(p1, p2, p3, p4);
        if (g_shot_req && g_vid_fresh) { g_shot_req = 0; shot_do(); }   /* v1.9.6: captura limpia */

        mine_f[frame % RING] = frame;
        mine_c[frame % RING] = g_frame_crc;
        if (role >= 0) COMPARE4(frame);
        g_run_hash = fnv_u32(g_run_hash, g_frame_crc);

        if (skip_now) skip_wait();
        else {
        g_skip_run = 0;
        if (role >= 0 && netinfo_show(frame)) {
            if (g_desyncs) snprintf(ov, sizeof ov, "DESYNC EN FRAME %u", (unsigned)g_first_desync);
            else if (g_netinfo == 1) snprintf(ov, sizeof ov, "4J %s DELAY %d (%dMS) FPS %u TRABAS %u", role == 0 ? "P1+P2" : "P3+P4", D, D * 17, (unsigned)g_ni_fps, (unsigned)g_ni_st);
            else snprintf(ov, sizeof ov, "ARCADE 4J  %s  DELAY %d", role == 0 ? "P1+P2" : "P3+P4", D);
            ui_text(2, 2, ov, 1, g_desyncs ? 0xFFFF4040 : 0xFFFFFFFF);
        }
        if (g_toast_n > 0) { ui_text(4, 224, g_toast, 1, 0xFFFFFF40); g_toast_n--; }      /* v1.9.2: avisos tambien en el modo de 4 */
        if (g_showfps && role != -2) ui_fps();                           /* v1.9.2: FPS en pantalla */
        present();
        }
        if ((role >= 0 && g_netinfo == 1) || (g_showfps && role != -2)) netinfo_tick();
        frame++;
        g_frames_done = frame;
    }
out:
    core_skip_draw(0); g_fs_prev = 0; g_debt = 0; g_skip_run = 0;
    g_ingame = 0;                           /* v1.9.3: al salir de la partida, los menus vuelven a MENU_SCREEN */
    g_need_crc = 1;
    g_ka_on = 0;
    free(st_in);                            /* v1.9.4: estado pendiente que nunca llego a cargarse */
    st_in = NULL;
    if (role >= 0 && g_net >= 0 && !g_sw_pending) {       /* v1.9.2: en un cambio de juego no se avisa de salida */
        Msg q; memset(&q, 0, sizeof q); q.type = M_QUIT; net_send(&q);
        if (user_exit) {
            /* vacia lo que el otro lado alcance a mandar (hasta 300 ms o hasta que cierre), para que
             * al cerrar el socket no se pierda el aviso de salida */
            uint32_t t0 = now_ms();
            while (now_ms() - t0 < 300) { if (net_recv(&q, 20) < 0) break; }
        }
    }
#undef COMPARE4
#ifdef NESNET_HOST
    if (getenv("NESNET_RAMDUMP")) {            /* prueba: muestra lo que el juego leyo de los 4 mandos (RAM $00-$07) */
        const uint8_t *ram = (const uint8_t *)retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        if (ram) printf("RAM role=%d: %02X %02X %02X %02X %02X %02X %02X %02X\n", role,
                        ram[0], ram[1], ram[2], ram[3], ram[4], ram[5], ram[6], ram[7]);
    }
#endif
    return ended_ok;
}

/* ------------------------------------------------------------------ */
/* Interfaz                                                            */
/* ------------------------------------------------------------------ */
static void code_pretty(const char *code, char *out);
/* v1.9.7: pantalla de "sala creada" (codigo, estado del puerto, esperando al amigo) */
static void hr_room_screen(const char *sub, const char *code, const char *l2, const char *l3, uint32_t col3)
{
    static const HHint hh[1] = { { "O", "CANCELAR", HC_BO } };
    char pc[24];
    int dots = (int)((now_ms() / 400) % 3), k;
    code_pretty(code, pc);
    hr_head(sub);
    hr_panel(230, 200, 1050, 600);
    hr_text(270, 252, "TU CÓDIGO DE SALA", FS_M, HC_DIM, 2);
    hr_text(270, 350, pc, FS_X, HC_YEL, 4);
    hr_text(270, 410, "Dáselo a tu amigo", FS_L, HC_WHITE, 2);
    hr_text(270, 452, l3, FS_M, hr_col(col3), 2);
    hr_text(270, 486, l2, FS_S, HC_DIM, 2);
    hr_text(270, 552, "Esperando al amigo...", FS_L, HC_WHITE, 2);
    for (k = 0; k < 3; k++) hr_rect(760 + k * 34, 534, 782 + k * 34, 556, k == dots ? HC_RED : 0xFF5A283Cu, 256);
    hr_hints(hh, 1);
}

static void ui_show(const char *l1, const char *l2, const char *l3)
{
    if (hr_usable()) {
        hr_head(NULL);
        hr_panel(230, 250, 1050, 480);
        if (l1) hr_textc(640, 340, l1, FS_L, HC_WHITE);
        if (l2) hr_textc(640, 396, l2, FS_M, HC_DIM);
        if (l3) hr_textc(640, 438, l3, FS_M, HC_DIM);
        present();
        return;
    }
    ui_clear();
    ui_center(20, "ARCADE STATION V0.1", 2, 0xFFFFD040);
    if (l1) ui_center(90, l1, 1, 0xFFFFFFFF);
    if (l2) ui_center(110, l2, 1, 0xFFC0C0FF);
    if (l3) ui_center(130, l3, 1, 0xFFC0C0FF);
    present();
}

static void ui_message(const char *l1, const char *l2, int frames)
{
    int i;
    for (i = 0; i < frames && g_running; i++) {
        uint16_t e;
        pump();
        ui_show(l1, l2, NULL);
        e = ui_edges();
        if (i > 60 && (e & (UI_OK | UI_CANCEL | UI_START))) break;   /* ~1 s minimo en pantalla */
#ifdef NESNET_HOST
        sleep_ms(16);
#endif
    }
}

/* Copia 'src' en 'dst' (cap bytes) acortando con "..." si no cabe en 'maxc' caracteres */
static void fit_name(char *dst, size_t cap, const char *src, int maxc)
{
    size_t n = strlen(src);
    if ((int)n <= maxc && n < cap) { snprintf(dst, cap, "%s", src); return; }
    if ((size_t)maxc >= cap) maxc = (int)cap - 1;
    memcpy(dst, src, (size_t)maxc);
    dst[maxc] = 0;
    if (maxc >= 3) { dst[maxc - 3] = '.'; dst[maxc - 2] = '.'; dst[maxc - 1] = '.'; }
}

/* ------------------------------------------------------------------ */
/* v1.9.8: caratulas. Lector de PNG propio (inflate + filtros), sin librerias externas.                 */
/* Soporta PNG de 1/2/4/8/16 bits, gris, RGB, paleta, gris+alfa y RGBA, con o sin entrelazado (Adam7). */
/* ------------------------------------------------------------------ */
#define CV_MAXDIM    1024            /* lado maximo aceptado (px) */
#define CV_MAXFILE   (3u << 20)      /* archivo maximo: 3 MB */
#define CV_MAXRAW    (9u << 20)      /* datos descomprimidos maximos: 9 MB */

#define CI_MAXBITS 15
#define CI_MAXL    286
#define CI_MAXD    30
#define CI_FIXL    288

typedef struct {
    const uint8_t *in; size_t inlen, incnt;
    uint32_t bitbuf; int bitcnt;
    uint8_t *out; size_t outlen, outcnt;
} CInf;

typedef struct { short *count; short *symbol; } CHuff;

static int ci_bits(CInf *s, int need)
{
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt >= s->inlen) return -1;
        val |= (uint32_t)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1u));
}

static int ci_decode(CInf *s, const CHuff *h)
{
    int len, code = 0, first = 0, count, index = 0;
    for (len = 1; len <= CI_MAXBITS; len++) {
        int b = ci_bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -10;
}

static int ci_construct(CHuff *h, const short *length, int n)
{
    int symbol, len, left;
    short offs[CI_MAXBITS + 1];
    for (len = 0; len <= CI_MAXBITS; len++) h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;
    left = 1;
    for (len = 1; len <= CI_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (len = 1; len < CI_MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;
    return left;
}

static int ci_codes(CInf *s, const CHuff *lencode, const CHuff *distcode)
{
    static const short lens[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const short dists[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    int symbol;
    do {
        symbol = ci_decode(s, lencode);
        if (symbol < 0) return symbol;
        if (symbol < 256) {
            if (s->outcnt == s->outlen) return 1;
            s->out[s->outcnt++] = (uint8_t)symbol;
        } else if (symbol > 256) {
            int len, b;
            size_t dist;
            symbol -= 257;
            if (symbol >= 29) return -10;
            b = ci_bits(s, lext[symbol]);
            if (b < 0) return -1;
            len = lens[symbol] + b;
            symbol = ci_decode(s, distcode);
            if (symbol < 0) return symbol;
            if (symbol >= 30) return -10;
            b = ci_bits(s, dext[symbol]);
            if (b < 0) return -1;
            dist = (size_t)dists[symbol] + (size_t)b;
            if (dist > s->outcnt) return -11;
            if (s->outcnt + (size_t)len > s->outlen) return 1;
            while (len--) { s->out[s->outcnt] = s->out[s->outcnt - dist]; s->outcnt++; }
        }
    } while (symbol != 256);
    return 0;
}

static int ci_stored(CInf *s)
{
    unsigned len;
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) return 2;
    len = s->in[s->incnt++]; len |= (unsigned)s->in[s->incnt++] << 8;
    if (s->in[s->incnt++] != (~len & 0xFFu) || s->in[s->incnt++] != ((~len >> 8) & 0xFFu)) return -2;
    if (s->incnt + len > s->inlen) return 2;
    if (s->outcnt + len > s->outlen) return 1;
    while (len--) s->out[s->outcnt++] = s->in[s->incnt++];
    return 0;
}

static int ci_fixed(CInf *s)
{
    static int virgin = 1;
    static short lc[CI_MAXBITS + 1], ls[CI_FIXL], dc[CI_MAXBITS + 1], ds[CI_MAXD];
    static CHuff lencode, distcode;
    if (virgin) {
        short lengths[CI_FIXL];
        int sym;
        lencode.count = lc; lencode.symbol = ls; distcode.count = dc; distcode.symbol = ds;
        for (sym = 0; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < CI_FIXL; sym++) lengths[sym] = 8;
        ci_construct(&lencode, lengths, CI_FIXL);
        for (sym = 0; sym < CI_MAXD; sym++) lengths[sym] = 5;
        ci_construct(&distcode, lengths, CI_MAXD);
        virgin = 0;
    }
    return ci_codes(s, &lencode, &distcode);
}

static int ci_dynamic(CInf *s)
{
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int nlen, ndist, ncode, index, err, b;
    short lengths[CI_MAXL + CI_MAXD];
    short lc[CI_MAXBITS + 1], ls[CI_MAXL], dc[CI_MAXBITS + 1], ds[CI_MAXD];
    CHuff lencode, distcode;
    b = ci_bits(s, 5); if (b < 0) return -1; nlen = b + 257;
    b = ci_bits(s, 5); if (b < 0) return -1; ndist = b + 1;
    b = ci_bits(s, 4); if (b < 0) return -1; ncode = b + 4;
    if (nlen > CI_MAXL || ndist > CI_MAXD) return -3;
    for (index = 0; index < ncode; index++) { b = ci_bits(s, 3); if (b < 0) return -1; lengths[order[index]] = (short)b; }
    for (; index < 19; index++) lengths[order[index]] = 0;
    lencode.count = lc; lencode.symbol = ls;
    err = ci_construct(&lencode, lengths, 19);
    if (err != 0) return -4;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = ci_decode(s, &lencode);
        if (symbol < 0) return symbol;
        if (symbol < 16) lengths[index++] = (short)symbol;
        else {
            int len = 0, rep;
            if (symbol == 16) {
                if (index == 0) return -5;
                len = lengths[index - 1];
                b = ci_bits(s, 2); if (b < 0) return -1; rep = 3 + b;
            } else if (symbol == 17) { b = ci_bits(s, 3); if (b < 0) return -1; rep = 3 + b; }
            else { b = ci_bits(s, 7); if (b < 0) return -1; rep = 11 + b; }
            if (index + rep > nlen + ndist) return -6;
            while (rep--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -9;
    err = ci_construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) return -7;
    distcode.count = dc; distcode.symbol = ds;
    err = ci_construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return -8;
    return ci_codes(s, &lencode, &distcode);
}

/* inflate de un flujo "raw deflate"; 0 = bien (la salida tiene que llenarse exacta) */
static int ci_inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    CInf s;
    int last, type, err = 0;
    s.in = in; s.inlen = inlen; s.incnt = 0; s.bitbuf = 0; s.bitcnt = 0;
    s.out = out; s.outlen = outlen; s.outcnt = 0;
    do {
        last = ci_bits(&s, 1);
        type = ci_bits(&s, 2);
        if (last < 0 || type < 0) { err = -1; break; }
        err = type == 0 ? ci_stored(&s) : type == 1 ? ci_fixed(&s) : type == 2 ? ci_dynamic(&s) : -1;
        if (err) break;
    } while (!last);
    if (s.outcnt == outlen) return 0;      /* si ya se lleno lo esperado, vale (aunque sobre flujo) */
    return err ? err : -12;
}

static uint32_t cv_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]; }

/* quita los filtros PNG de "rows" filas de (1 + rb) bytes, en el mismo lugar */
static int cv_unfilter(uint8_t *buf, int rb, int rows, int bpb)
{
    int r, i;
    for (r = 0; r < rows; r++) {
        uint8_t *row = buf + (size_t)r * (size_t)(rb + 1);
        uint8_t ft = row[0];
        uint8_t *x = row + 1;
        const uint8_t *up = r ? row - (rb + 1) + 1 : NULL;
        if (ft > 4) return -1;
        for (i = 0; i < rb; i++) {
            int a = i >= bpb ? x[i - bpb] : 0;
            int b = up ? up[i] : 0;
            int c = (up && i >= bpb) ? up[i - bpb] : 0;
            int v = x[i];
            switch (ft) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: break;
            }
            x[i] = (uint8_t)v;
        }
    }
    return 0;
}

typedef struct { int ct, depth, bpp, npal; uint8_t pal[256][4]; } CvFmt;

static uint32_t cv_pixel(const uint8_t *row, int x, const CvFmt *f, uint32_t bg)
{
    int r, g, b, a = 255, d = f->depth;
    switch (f->ct) {
    case 0: case 3: {
        int v, bit = x * d;
        if (d == 16) v = row[x * 2];
        else if (d == 8) v = row[x];
        else v = (row[bit >> 3] >> (8 - d - (bit & 7))) & ((1 << d) - 1);
        if (f->ct == 3) {
            if (v >= f->npal) v = 0;
            r = f->pal[v][0]; g = f->pal[v][1]; b = f->pal[v][2]; a = f->pal[v][3];
        } else {
            if (d < 8) v = v * 255 / ((1 << d) - 1);
            r = g = b = v;
        }
        break;
    }
    case 4: {
        int st = d == 16 ? 4 : 2;
        r = g = b = row[x * st];
        a = row[x * st + (d == 16 ? 2 : 1)];
        break;
    }
    case 2: {
        int st = d == 16 ? 6 : 3, o = d == 16 ? 2 : 1;
        r = row[x * st]; g = row[x * st + o]; b = row[x * st + 2 * o];
        break;
    }
    default: {   /* 6 */
        int st = d == 16 ? 8 : 4, o = d == 16 ? 2 : 1;
        r = row[x * st]; g = row[x * st + o]; b = row[x * st + 2 * o]; a = row[x * st + 3 * o];
        break;
    }
    }
    if (a < 255) {
        int br = (int)((bg >> 16) & 255), bgc = (int)((bg >> 8) & 255), bb = (int)(bg & 255);
        r = (r * a + br * (255 - a)) / 255; g = (g * a + bgc * (255 - a)) / 255; b = (b * a + bb * (255 - a)) / 255;
    }
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Decodifica un PNG en memoria. Devuelve 0 = bien (*px = malloc de w*h ARGB opacos sobre "bg"; hay que liberarlo con free).
 * 1 = no es PNG, 2 = demasiado grande, 3 = formato no soportado, 4 = archivo danado, 5 = sin memoria */
static int cv_png_decode(const uint8_t *f, size_t fn, uint32_t bg, uint32_t **px, int *ow, int *oh)
{
    static const int XO[7] = { 0, 4, 0, 2, 0, 1, 0 }, YO[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const int DX[7] = { 8, 8, 4, 4, 2, 2, 1 }, DY[7] = { 8, 8, 8, 4, 4, 2, 2 };
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    size_t pos = 8, zlen = 0, zpos = 0, rawn = 0;
    uint32_t w = 0, h = 0;
    int have_ihdr = 0, interlace = 0, channels, pass, npass, rc = 4, bpb, ntrns = 0;
    CvFmt fm;
    uint8_t *z = NULL, *raw = NULL;
    uint32_t *out = NULL;
    int pw[7], ph[7], prb[7];
    size_t poff[7];

    *px = NULL; *ow = *oh = 0;
    memset(&fm, 0, sizeof fm);
    if (fn < 8 + 25 || memcmp(f, sig, 8)) return 1;
    /* 1a pasada: cabecera, paleta, transparencia y tamano del flujo comprimido */
    while (pos + 12 <= fn) {
        uint32_t len = cv_be32(f + pos);
        const uint8_t *t = f + pos + 4, *d = f + pos + 8;
        if (len > fn - pos - 12) { if (!have_ihdr) return 4; break; }
        if (!memcmp(t, "IHDR", 4)) {
            if (len < 13) return 4;
            w = cv_be32(d); h = cv_be32(d + 4);
            fm.depth = d[8]; fm.ct = d[9]; interlace = d[12];
            if (d[10] != 0 || d[11] != 0 || interlace > 1) return 3;
            have_ihdr = 1;
        } else if (!memcmp(t, "PLTE", 4) && have_ihdr) {
            uint32_t i;
            fm.npal = (int)(len / 3);
            if (fm.npal > 256) fm.npal = 256;
            for (i = 0; i < (uint32_t)fm.npal; i++) { fm.pal[i][0] = d[i * 3]; fm.pal[i][1] = d[i * 3 + 1]; fm.pal[i][2] = d[i * 3 + 2]; }
        } else if (!memcmp(t, "tRNS", 4) && have_ihdr && fm.ct == 3) {
            uint32_t i;
            for (i = 0; i < len && i < 256; i++) fm.pal[i][3] = d[i];
            ntrns = (int)(len < 256 ? len : 256);
        } else if (!memcmp(t, "IDAT", 4)) zlen += len;
        else if (!memcmp(t, "IEND", 4)) break;
        pos += 12 + (size_t)len;
    }
    if (!have_ihdr || zlen < 6) return 4;
    if (w < 1 || h < 1 || w > CV_MAXDIM || h > CV_MAXDIM) return 2;
    switch (fm.ct) {
    case 0: if (fm.depth != 1 && fm.depth != 2 && fm.depth != 4 && fm.depth != 8 && fm.depth != 16) return 3; channels = 1; break;
    case 2: if (fm.depth != 8 && fm.depth != 16) return 3; channels = 3; break;
    case 3: if (fm.depth != 1 && fm.depth != 2 && fm.depth != 4 && fm.depth != 8) return 3; channels = 1; break;
    case 4: if (fm.depth != 8 && fm.depth != 16) return 3; channels = 2; break;
    case 6: if (fm.depth != 8 && fm.depth != 16) return 3; channels = 4; break;
    default: return 3;
    }
    {   /* paleta: las entradas sin alfa explicito son opacas */
        int i;
        for (i = ntrns; i < 256; i++) fm.pal[i][3] = 255;
        if (fm.ct == 3 && fm.npal == 0) return 4;
    }
    fm.bpp = channels * fm.depth;
    bpb = fm.bpp >= 8 ? fm.bpp / 8 : 1;
    /* tamano de los datos descomprimidos */
    npass = interlace ? 7 : 1;
    for (pass = 0; pass < npass; pass++) {
        if (interlace) {
            pw[pass] = (int)w > XO[pass] ? ((int)w - XO[pass] + DX[pass] - 1) / DX[pass] : 0;
            ph[pass] = (int)h > YO[pass] ? ((int)h - YO[pass] + DY[pass] - 1) / DY[pass] : 0;
        } else { pw[pass] = (int)w; ph[pass] = (int)h; }
        prb[pass] = (pw[pass] * fm.bpp + 7) / 8;
        poff[pass] = rawn;
        if (pw[pass] > 0 && ph[pass] > 0) rawn += (size_t)ph[pass] * (size_t)(1 + prb[pass]);
    }
    if (rawn > CV_MAXRAW) return 2;
    z = (uint8_t *)malloc(zlen);
    raw = (uint8_t *)malloc(rawn ? rawn : 1);
    out = (uint32_t *)malloc((size_t)w * h * 4);
    if (!z || !raw || !out) { rc = 5; goto done; }
    /* 2a pasada: juntar los IDAT */
    pos = 8;
    while (pos + 12 <= fn && zpos < zlen) {
        uint32_t len = cv_be32(f + pos);
        if (len > fn - pos - 12) break;
        if (!memcmp(f + pos + 4, "IDAT", 4)) { memcpy(z + zpos, f + pos + 8, len); zpos += len; }
        else if (!memcmp(f + pos + 4, "IEND", 4)) break;
        pos += 12 + (size_t)len;
    }
    if (zpos != zlen) { rc = 4; goto done; }
    if ((z[0] & 15) != 8 || (((unsigned)z[0] << 8) | z[1]) % 31 != 0 || (z[1] & 32)) { rc = 3; goto done; }
    if (ci_inflate(z + 2, zlen - 2, raw, rawn) != 0) { rc = 4; goto done; }
    for (pass = 0; pass < npass; pass++) {
        uint8_t *pb = raw + poff[pass];
        int x, y;
        if (pw[pass] <= 0 || ph[pass] <= 0) continue;
        if (cv_unfilter(pb, prb[pass], ph[pass], bpb)) { rc = 4; goto done; }
        for (y = 0; y < ph[pass]; y++) {
            const uint8_t *row = pb + (size_t)y * (size_t)(1 + prb[pass]) + 1;
            for (x = 0; x < pw[pass]; x++) {
                int dx = interlace ? XO[pass] + x * DX[pass] : x, dy = interlace ? YO[pass] + y * DY[pass] : y;
                out[(size_t)dy * w + (size_t)dx] = cv_pixel(row, x, &fm, bg);
            }
        }
    }
    *px = out; *ow = (int)w; *oh = (int)h; out = NULL; rc = 0;
done:
    free(z); free(raw); free(out);
    return rc;
}

/* Reduce (o agranda) src de sw x sh para que quepa en bw x bh sin deformar; deja el resultado en dst (bw*bh) y su tamano en *ow,*oh */
static void cv_fit(const uint32_t *src, int sw, int sh, uint32_t *dst, int bw, int bh, int *ow, int *oh)
{
    int dw, dh, x, y;
    if ((long)sw * bh > (long)sh * bw) { dw = bw; dh = (int)((long)sh * bw / sw); }
    else { dh = bh; dw = (int)((long)sw * bh / sh); }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    *ow = dw; *oh = dh;
    for (y = 0; y < dh; y++) {
        for (x = 0; x < dw; x++) {
            uint32_t rr = 0, gg = 0, bb = 0;
            if (dw < sw || dh < sh) {            /* achicar: promedio del rectangulo de origen */
                int x0 = (int)((long)x * sw / dw), x1 = (int)((long)(x + 1) * sw / dw);
                int y0 = (int)((long)y * sh / dh), y1 = (int)((long)(y + 1) * sh / dh), xx, yy, n;
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                if (x1 > sw) x1 = sw;
                if (y1 > sh) y1 = sh;
                n = (x1 - x0) * (y1 - y0);
                for (yy = y0; yy < y1; yy++)
                    for (xx = x0; xx < x1; xx++) {
                        uint32_t p = src[(size_t)yy * sw + xx];
                        rr += (p >> 16) & 255; gg += (p >> 8) & 255; bb += p & 255;
                    }
                rr /= (uint32_t)n; gg /= (uint32_t)n; bb /= (uint32_t)n;
            } else {                              /* agrandar: bilineal */
                long fx = dw > 1 ? (long)x * (sw - 1) * 256 / (dw - 1) : 0, fy = dh > 1 ? (long)y * (sh - 1) * 256 / (dh - 1) : 0;
                int x0 = (int)(fx >> 8), y0 = (int)(fy >> 8), x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
                int ax = (int)(fx & 255), ay = (int)(fy & 255), ch;
                uint32_t p00 = src[(size_t)y0 * sw + x0], p10 = src[(size_t)y0 * sw + x1], p01 = src[(size_t)y1 * sw + x0], p11 = src[(size_t)y1 * sw + x1];
                uint32_t v[3];
                for (ch = 0; ch < 3; ch++) {
                    int sft = 16 - 8 * ch;
                    int a = (int)((p00 >> sft) & 255), b = (int)((p10 >> sft) & 255), c = (int)((p01 >> sft) & 255), d = (int)((p11 >> sft) & 255);
                    int top = a * (256 - ax) + b * ax, bot = c * (256 - ax) + d * ax;
                    v[ch] = (uint32_t)((top * (256 - ay) + bot * ay) >> 16);
                }
                rr = v[0]; gg = v[1]; bb = v[2];
            }
            dst[(size_t)y * bw + x] = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
        }
    }
}

/* ---- v1.9.8: cuadro de caratula en ELEGIR ROM (carpeta USRDIR/covers, mismo nombre que la ROM, .png) ---- */
#define CV_BX0 868            /* cuadro de la caratula (marco) */
#define CV_BX1 1188
#define CV_BY0 140
#define CV_BY1 548
#define CV_BW  312            /* area de imagen dentro del marco */
#define CV_BH  400
enum { CVS_PEND = 0, CVS_OK, CVS_NONE, CVS_BAD, CVS_BIG, CVS_UNSUP };
static uint32_t g_cv_px[CV_BW * CV_BH];
static int g_cv_w, g_cv_h, g_cv_state;
static char g_cv_file[ROM_NAMELEN + 8];      /* nombre de la caratula esperada: <ROM sin .nes>.png */

static int cv_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int cv_ceq(const char *a, const char *b, size_t n)     /* n caracteres iguales sin distinguir mayusculas */
{
    size_t i;
    for (i = 0; i < n; i++) if (cv_lower((unsigned char)a[i]) != cv_lower((unsigned char)b[i])) return 0;
    return 1;
}
/* largo del nombre sin la extension ".a26" / ".bin" / ".rom" (cualquier mayuscula/minuscula) */
static size_t rom_base_len(const char *name)
{
    size_t n = strlen(name);
    if (n > 4 && name[n - 4] == '.' && (cv_ceq(name + n - 3, "zip", 3) || cv_ceq(name + n - 3, "a26", 3) || cv_ceq(name + n - 3, "bin", 3) || cv_ceq(name + n - 3, "rom", 3))) return n - 4;
    return n;
}
static void rom_title(char *dst, size_t cap, const char *name)
{
    size_t n = rom_base_len(name);
    if (n >= cap) n = cap - 1;
    memcpy(dst, name, n);
    dst[n] = 0;
}
/* recorta por ancho en pixeles (con "..."), sin partir un caracter UTF-8 */
static void fit_px(char *dst, size_t cap, const char *src, int maxpx, int fs)
{
    size_t n;
    snprintf(dst, cap, "%s", src);
    if (hr_tw(dst, fs) <= maxpx) return;
    n = strlen(dst);
    while (n > 4) {
        n--;
        while (n > 4 && ((unsigned char)dst[n] & 0xC0) == 0x80) n--;
        dst[n] = '.'; dst[n + 1] = '.'; dst[n + 2] = '.'; dst[n + 3] = 0;
        if (hr_tw(dst, fs) <= maxpx) return;
    }
}
/* busca en covers/ un .png que se llame como la ROM (sin distinguir mayusculas). 1 = lo encontro y deja la ruta en path */
static int cv_find(const char *romname, char *path, size_t cap)
{
    char dir[300];
    DIR *d;
    struct dirent *e;
    size_t bl = rom_base_len(romname);
    int found = 0;
    snprintf(dir, sizeof dir, "%s/covers", g_dir);
    d = opendir(dir);
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n != bl + 4) continue;
        if (!cv_ceq(e->d_name, romname, bl) || e->d_name[bl] != '.' || !cv_ceq(e->d_name + bl + 1, "png", 3)) continue;
        snprintf(path, cap, "%s/%s", dir, e->d_name);
        found = 1;
        break;
    }
    closedir(d);
    return found;
}
/* carga la caratula de la ROM g_roms[idx] (o deja el motivo en g_cv_state) */
static void cv_update(int idx)
{
    char path[420];
    uint8_t *b = NULL;
    size_t bs = 0;
    uint32_t *px = NULL;
    int w = 0, h = 0, rc;
    struct stat st;
    snprintf(g_cv_file, sizeof g_cv_file, "%.*s.png", (int)rom_base_len(g_roms[idx].name), g_roms[idx].name);
    g_cv_state = CVS_NONE;
    if (!cv_find(g_roms[idx].name, path, sizeof path)) return;
    if (stat(path, &st) != 0) return;
    if ((unsigned long)st.st_size > CV_MAXFILE) { g_cv_state = CVS_BIG; logf_("caratula: %s pesa mas de 3 MB, no se muestra", g_cv_file); return; }
    if (!read_whole_file(path, &b, &bs)) { g_cv_state = CVS_BAD; return; }
    rc = cv_png_decode(b, bs, 0xFF02050Eu, &px, &w, &h);
    free(b);
    if (rc == 0) {
        cv_fit(px, w, h, g_cv_px, CV_BW, CV_BH, &g_cv_w, &g_cv_h);
        g_cv_state = CVS_OK;
    } else {
        g_cv_state = rc == 2 ? CVS_BIG : rc == 3 ? CVS_UNSUP : CVS_BAD;
        logf_("caratula: %s no se pudo leer (codigo %d)", g_cv_file, rc);
    }
    free(px);
}
static void cv_draw(void)
{
    int cx = (CV_BX0 + CV_BX1) / 2, y, x0, y0;
    char t[ROM_NAMELEN + 8];
    hr_rect(CV_BX0, CV_BY0, CV_BX1, CV_BY1, 0xFF02050Eu, 256);
    hr_frame_rect(CV_BX0, CV_BY0, CV_BX1, CV_BY1, 2, HC_GRID);
    if (g_cv_state == CVS_PEND) return;
    if (g_cv_state == CVS_OK) {
        x0 = CV_BX0 + 4 + (CV_BW - g_cv_w) / 2;
        y0 = CV_BY0 + 4 + (CV_BH - g_cv_h) / 2;
        for (y = 0; y < g_cv_h; y++) memcpy(&g_hr[(y0 + y) * HR_W + x0], &g_cv_px[(size_t)y * CV_BW], (size_t)g_cv_w * 4);
        return;
    }
    hr_textc(cx, 330, g_cv_state == CVS_NONE ? "Sin carátula" : g_cv_state == CVS_BIG ? "PNG muy grande" : g_cv_state == CVS_UNSUP ? "PNG no compatible" : "PNG inválido", FS_M, HC_DIM);
    if (g_cv_state == CVS_BIG) hr_textc(cx, 372, "máx. 1024 px y 3 MB", FS_S, HC_DIM);
    else hr_textc(cx, 372, g_cv_state == CVS_NONE ? "Pon en la carpeta covers:" : "Revisa el archivo:", FS_S, HC_DIM);
    fit_px(t, sizeof t, g_cv_file, CV_BW - 8, FS_S);
    hr_textc(cx, 408, t, FS_S, HC_YEL);
}

/* Selector de ROM: lista de USRDIR/roms. 1 = eligio y cargo una ROM, 0 = volvio sin elegir.
 * v1.9.2: con pick_only = 1 (el host en plena partida online) NO carga nada: deja nombre, CRC32 y tamano de la ROM elegida
 * en g_pick_* y devuelve 1; la partida sigue en marcha por debajo (ver gsw_ask_host). */
static int ui_rom_select(int pick_only)
{
    enum { VIS = 9 };
    int sel = 0, top = 0, i, last_sel = -1, hold = 0;
    g_stage = "elegir-rom";
    roms_scan();
    { char cdir[300]; snprintf(cdir, sizeof cdir, "%s/covers", g_dir); mkdir(cdir, 0777); }   /* v1.9.8: carpeta de caratulas */
    g_cv_state = CVS_PEND;
    for (i = 0; i < g_nroms; i++) if (g_rom_name[0] && !strcmp(g_roms[i].name, g_rom_name)) sel = i;
    while (g_running) {
        uint16_t e;
        char line[64], nm[48];
        pump();
        ui_keepalive();
        e = ui_edges();
        if (e & UI_CANCEL) return 0;
        if (g_nroms > 0) {
            if (e & UI_UP)    sel = (sel + g_nroms - 1) % g_nroms;
            if (e & UI_DOWN)  sel = (sel + 1) % g_nroms;
            if (e & UI_LEFT)  sel = sel - 10 < 0 ? 0 : sel - 10;
            if (e & UI_RIGHT) sel = sel + 10 >= g_nroms ? g_nroms - 1 : sel + 10;
            if ((e & (UI_OK | UI_START)) && pick_only) {
                uint8_t *b = NULL;
                size_t bs = 0;
                char rp[420];
                snprintf(rp, sizeof rp, "%s/roms/%s", g_dir, g_roms[sel].name);
                if (read_whole_file(rp, &b, &bs)) {
                    g_pick_crc = crc32_buf(b, bs); g_pick_size = (uint32_t)bs;
                    snprintf(g_pick_name, sizeof g_pick_name, "%s", g_roms[sel].name);
                    free(b);
                    return 1;
                }
                ui_message("NO PUDE LEER LA ROM", g_roms[sel].name, 300);
            } else if (e & (UI_OK | UI_START)) {
                ui_show("CARGANDO ROM...", g_roms[sel].name, NULL);
                if (rom_load(sel)) {
                    snprintf(g_last_rom, sizeof g_last_rom, "%s", g_rom_name);     /* v1.9: la recuerda para la proxima vez */
                    opts_save();
                    return 1;
                }
                ui_message("NO PUDE LEER LA ROM", g_roms[sel].name, 300);
            }
            if (sel < top) top = sel;
            if (sel >= top + VIS) top = sel - VIS + 1;
            /* v1.9: el CRC32 se calcula cuando el cursor se queda ~0,2 s sobre una ROM (no al pasar rapido) */
            if (sel != last_sel) { last_sel = sel; hold = 0; g_cv_state = CVS_PEND; }
            else if (hold < 1000) hold++;
            if (hold == 12 && hr_usable()) cv_update(sel);       /* v1.9.8: la caratula se carga cuando el cursor se queda ~0,2 s */
            if (hold == 12 && !g_roms[sel].has_crc) {
                uint8_t *b = NULL;
                size_t bs = 0;
                char rp[420];
                (void)rp; (void)b; (void)bs;      /* Arcade Station: un .zip puede pesar decenas de MB; el "CRC" es de nombre + tamano */
                g_roms[sel].crc = crc32_buf((const uint8_t *)g_roms[sel].name, strlen(g_roms[sel].name)) ^ g_roms[sel].size;
                g_roms[sel].has_crc = 1;
            }
        }
        if (hr_usable()) {
            static const HHint hh1[3] = { { "<>", "SALTAR 10", HC_BG }, { "X", "ELEGIR", HC_BX }, { "O", "VOLVER", HC_BO } };
            static const HHint hh0[1] = { { "O", "VOLVER", HC_BO } };
            hr_begin();
            hr_text(70, 100, "ELEGIR ROM", FS_H, HC_WHITE, 3);
            if (g_nroms == 0) {
                hr_panel(70, 130, 1210, 400);
                hr_text(110, 200, "NO HAY ROMS EN LA CARPETA:", FS_L, HC_WHITE, 2);
                hr_text(110, 250, "KATA00001/USRDIR/roms", FS_L, HC_YEL, 2);
                hr_text(110, 310, "Pon ahí tus juegos arcade (.zip) y vuelve a entrar aquí.", FS_M, HC_DIM, 2);
                hr_hints(hh0, 1);
            } else {
                snprintf(line, sizeof line, "%d / %d", sel + 1, g_nroms);
                hr_text(1210 - hr_tw(line, FS_L), 84, line, FS_L, HC_DIM, 2);
                hr_panel(70, 124, 1210, 558);
                for (i = 0; i < VIS && top + i < g_nroms; i++) {
                    int k = top + i, yy = 138 + i * 46;
                    char tt[ROM_NAMELEN + 4], nm2[ROM_NAMELEN + 4];
                    rom_title(tt, sizeof tt, g_roms[k].name);            /* v1.9.8: sin ".nes" y recortado por ancho */
                    fit_px(nm2, sizeof nm2, tt, 700, FS_L);
                    if (k == sel) {
                        hr_selbar(86, yy, 852, yy + 42);
                        hr_tri(100, yy + 21, 22);
                        hr_text(132, yy + 31, nm2, FS_L, HC_WHITE, 2);
                    } else hr_text(132, yy + 31, nm2, FS_L, HC_DIM, 2);
                }
                if (top > 0) hr_arrow(838, 156, 1);
                if (top + VIS < g_nroms) hr_arrow(838, 536, 0);
                cv_draw();
                hr_panel(70, 568, 1210, 656);
                fit_name(nm, sizeof nm, g_roms[sel].name, 60);
                hr_text(96, 604, nm, FS_M, HC_YEL, 2);
                if (g_roms[sel].has_crc) snprintf(line, sizeof line, "%u KB     CRC32 %08X", (unsigned)((g_roms[sel].size + 512) / 1024), (unsigned)g_roms[sel].crc);
                else snprintf(line, sizeof line, "%u KB     CRC32 ........", (unsigned)((g_roms[sel].size + 512) / 1024));
                hr_text(96, 640, line, FS_S, HC_DIM, 2);
                hr_hints(hh1, 3);
            }
            present();
#ifdef NESNET_HOST
            sleep_ms(16);
#endif
            continue;
        }
        ui_clear();
        ui_center(14, "ELEGIR ROM", 2, 0xFFFFD040);
        if (g_nroms == 0) {
            ui_center(80, "NO HAY ROMS EN LA CARPETA:", 1, 0xFFFFFFFF);
            ui_center(100, "KATA00001/USRDIR/ROMS", 1, 0xFFC0C0FF);
            ui_center(124, "PON AHI TUS JUEGOS ARCADE (.ZIP)", 1, 0xFFC0C0FF);
            ui_center(140, "Y VUELVE A ENTRAR AQUI", 1, 0xFFC0C0FF);
        } else {
            snprintf(line, sizeof line, "%d / %d", sel + 1, g_nroms);
            ui_center(38, line, 1, 0xFFC0C0FF);
            for (i = 0; i < VIS && top + i < g_nroms; i++) {
                int k = top + i;
                fit_name(nm, sizeof nm, g_roms[k].name, 38);
                snprintf(line, sizeof line, "%s %s", k == sel ? ">" : " ", nm);
                ui_text(8, 52 + i * 12, line, 1, k == sel ? 0xFFFFFFFF : 0xFF8080A0);
            }
            if (top > 0)              ui_text(244, 52, "^", 1, 0xFFC0C0FF);
            if (top + VIS < g_nroms)  ui_text(244, 52 + (VIS - 1) * 12, "V", 1, 0xFFC0C0FF);
            {
                /* v1.9: nombre completo de la ROM marcada (hasta 2 lineas de 40 letras), tamano y CRC32 */
                const char *full = g_roms[sel].name;
                size_t fl = strlen(full);
                char l1[48], l2[48], l3[64], crc[12];
                snprintf(l1, sizeof l1, "%.40s", full);
                if (fl > 40) fit_name(l2, sizeof l2, full + 40, 40); else l2[0] = 0;
                if (g_roms[sel].has_crc) snprintf(crc, sizeof crc, "%08X", (unsigned)g_roms[sel].crc); else snprintf(crc, sizeof crc, "........");
                snprintf(l3, sizeof l3, "%u KB    CRC32 %s", (unsigned)((g_roms[sel].size + 512) / 1024), crc);
                ui_text(8, 164, l1, 1, 0xFFFFD040);
                if (l2[0]) ui_text(8, 175, l2, 1, 0xFFFFD040);
                ui_text(8, 188, l3, 1, 0xFFC0C0FF);
            }
        }
        if (g_nroms) ui_center(204, "IZQ/DER: SALTAR 10", 1, 0xFF8080A0);
        ui_center(218, g_nroms ? "X: ELEGIR   O: VOLVER" : "O: VOLVER", 1, 0xFF8080A0);
        present();
#ifdef NESNET_HOST
        sleep_ms(16);
#endif
    }
    return 0;
}

/* v1.8.2: OPCIONES > PANTALLA. El modo se aplica en el acto al mover el cursor (esta misma pantalla ya sale con el modo elegido) */
static void ui_opt_screen(void)
{
    static const char *desc[3] = { "PIXELES CUADRADOS (COMO SIEMPRE)", "PROPORCION DE TV 4:3", "ESTIRADA A PANTALLA ANCHA 16:9" };
    int i;
    g_stage = "opciones-pantalla";
    ui_prime();
    while (g_running) {
        uint16_t e;
        pump();
        e = ui_edges();
        if (e & UI_UP)   { g_screen = (g_screen + 2) % 3; opts_save(); }
        if (e & UI_DOWN) { g_screen = (g_screen + 1) % 3; opts_save(); }
        if (e & (UI_OK | UI_CANCEL | UI_START)) break;
        if (hr_usable()) {
            static const HHint hh[2] = { { "X", "VOLVER", HC_BX }, { "O", "VOLVER", HC_BO } };
            hr_head("PANTALLA  ·  solo cambia la imagen del juego");
            hr_panel(70, 210, 900, 480);
            hr_rows(96, 232, 778, screen_names, 3, g_screen, 0, 58);
            hr_text(96, 446, desc[g_screen], FS_M, HC_GREEN, 2);
            hr_hints(hh, 2);
            present();
#ifdef NESNET_HOST
            sleep_ms(16);
#endif
            continue;
        }
        ui_clear();
        ui_center(20, "PANTALLA", 2, 0xFFFFD040);
        ui_center(46, "SOLO CAMBIA LA IMAGEN DEL JUEGO", 1, 0xFFC0C0FF);
        for (i = 0; i < 3; i++) {
            char line[48];
            snprintf(line, sizeof line, "%s %s", i == g_screen ? ">" : " ", screen_names[i]);
            ui_center(78 + i * 22, line, 1, i == g_screen ? 0xFFFFFFFF : 0xFF8080A0);
        }
        ui_center(150, desc[g_screen], 1, 0xFF80FF80);
        if (!g_cfg.render) ui_center(170, "AVISO: EL MODO GPU NO USA ESTO", 1, 0xFFFFC040);
        ui_center(200, "ARRIBA/ABAJO: ELEGIR", 1, 0xFF8080A0);
        ui_center(214, "X / O: VOLVER", 1, 0xFF8080A0);
        present();
#ifdef NESNET_HOST
        sleep_ms(16);
#endif
    }
    logf_("opciones: pantalla = %s", screen_names[g_screen]);
}

/* v1.8.2: OPCIONES > MANDOS > MANDO n: un boton de la PS3 por cada boton del NES, y turbo A / turbo B.
 * IZQ/DER cambia el boton de la fila marcada y SALTA los que ya usa otra fila (no mueve botones ajenos). */
static void ui_opt_pad(int port)
{
    int sel = 0, i;          /* filas 0..5 = acciones, 6 = RESTAURAR ESTE MANDO, 7 = VOLVER */
    g_stage = "opciones-mando";
    ui_prime();
    while (g_running) {
        uint16_t e;
        char title[16];
        pump();
        e = ui_edges();
        if (e & UI_UP)   sel = (sel + 7) % 8;
        if (e & UI_DOWN) sel = (sel + 1) % 8;
        if (e & UI_CANCEL) break;
        if (sel < MA_COUNT) {
            if (e & UI_LEFT)            { map_cycle(port, sel, -1); opts_save(); }
            if (e & (UI_RIGHT | UI_OK)) { map_cycle(port, sel, +1); opts_save(); }
        } else if (e & (UI_OK | UI_START)) {
            if (sel == 6) { map_defaults(port); opts_save(); }
            else break;
        }
        if (hr_usable()) {
            static const HHint hh[1] = { { "O", "VOLVER", HC_BO } };
            char sub[48];
            snprintf(sub, sizeof sub, "MANDO %d  ·  botones y turbo", port + 1);
            hr_head(sub);
            hr_panel(70, 206, 880, 606);
            for (i = 0; i < 8; i++) {
                const char *nm = i < MA_COUNT ? ma_names[i] : (i == 6 ? "RESTAURAR ESTE MANDO" : "VOLVER");
                const char *vv = i < MA_COUNT ? (g_map[port][i] == PB_NONE ? "NO" : pb_names[g_map[port][i]]) : NULL;
                hr_row2(86, 222 + i * 46, 780, 46, nm, vv, i == sel, 330);
            }
            hr_panel(910, 206, 1210, 606);
            hr_text(934, 252, "IZQ/DER", FS_M, HC_YEL, 2);
            hr_text(934, 290, "cambia el botón", FS_S, HC_DIM, 2);
            hr_text(934, 322, "(salta los usados)", FS_S, HC_DIM, 2);
            hr_text(934, 388, "TURBO", FS_M, HC_YEL, 2);
            hr_text(934, 426, "15 pulsos por", FS_S, HC_DIM, 2);
            hr_text(934, 458, "segundo", FS_S, HC_DIM, 2);
            if (port == 0) {
                hr_text(934, 524, "MANDO 1", FS_M, HC_AMBER, 2);
                hr_text(934, 562, "R3 = menú", FS_S, HC_AMBER, 2);
                hr_text(934, 594, "L2 = captura", FS_S, HC_AMBER, 2);
            }
            hr_hints(hh, 1);
            present();
#ifdef NESNET_HOST
            sleep_ms(16);
#endif
            continue;
        }
        ui_clear();
        snprintf(title, sizeof title, "MANDO %d", port + 1);
        ui_center(14, title, 2, 0xFFFFD040);
        for (i = 0; i < 8; i++) {
            char line[56];
            uint32_t col = i == sel ? 0xFFFFFFFF : 0xFF8080A0;
            if (i < MA_COUNT)
                snprintf(line, sizeof line, "%s %-8s %s", i == sel ? ">" : " ", ma_names[i], g_map[port][i] == PB_NONE ? "NO" : pb_names[g_map[port][i]]);
            else
                snprintf(line, sizeof line, "%s %s", i == sel ? ">" : " ", i == 6 ? "RESTAURAR ESTE MANDO" : "VOLVER");
            ui_text(40, 44 + i * 16, line, 1, col);
        }
        ui_center(176, "IZQ/DER: CAMBIAR (SALTA LOS USADOS)", 1, 0xFFC0C0FF);
        ui_center(190, "TURBO: 15 PULSOS POR SEGUNDO", 1, 0xFFC0C0FF);
        if (port == 0) ui_center(204, "MANDO 1: R3 = MENU   L2 = CAPTURA", 1, 0xFFFFC040);
        ui_center(218, "O: VOLVER", 1, 0xFF8080A0);
        present();
#ifdef NESNET_HOST
        sleep_ms(16);
#endif
    }
}

static void ui_opt_pads(void)
{
    static const char *items[4] = { "MANDO 1", "MANDO 2", "RESTAURAR LOS DOS", "VOLVER" };
    int sel = 0, i;
    g_stage = "opciones-mandos";
    ui_prime();
    while (g_running) {
        uint16_t e;
        pump();
        e = ui_edges();
        if (e & UI_UP)   sel = (sel + 3) % 4;
        if (e & UI_DOWN) sel = (sel + 1) % 4;
        if (e & UI_CANCEL) break;
        if (e & (UI_OK | UI_START)) {
            if (sel < 2) { ui_opt_pad(sel); ui_prime(); g_stage = "opciones-mandos"; }
            else if (sel == 2) { map_defaults(0); map_defaults(1); opts_save(); }
            else break;
        }
        if (hr_usable()) {
            static const HHint hh[2] = { { "X", "ENTRAR", HC_BX }, { "O", "VOLVER", HC_BO } };
            hr_head("MANDOS  ·  botones y turbo de cada mando");
            hr_panel(70, 210, 900, 540);
            hr_rows(96, 232, 778, items, 4, sel, 0, 58);
            hr_text(96, 512, "Vale para SOLO, 1 vs 1 y 4 jugadores", FS_S, HC_DIM, 2);
            hr_hints(hh, 2);
            present();
#ifdef NESNET_HOST
            sleep_ms(16);
#endif
            continue;
        }
        ui_clear();
        ui_center(20, "MANDOS", 2, 0xFFFFD040);
        ui_center(46, "BOTONES Y TURBO DE CADA MANDO", 1, 0xFFC0C0FF);
        for (i = 0; i < 4; i++) {
            char line[40];
            snprintf(line, sizeof line, "%s %s", i == sel ? ">" : " ", items[i]);
            ui_center(84 + i * 22, line, 1, i == sel ? 0xFFFFFFFF : 0xFF8080A0);
        }
        ui_center(190, "VALE PARA SOLO, 1 VS 1 Y 4 JUGADORES", 1, 0xFF8080A0);
        ui_center(204, "ARRIBA/ABAJO + X", 1, 0xFF8080A0);
        present();
#ifdef NESNET_HOST
        sleep_ms(16);
#endif
    }
}

/* v1.9: OPCIONES > SONIDO (volumen 0 a 100, de 10 en 10; se guarda solo). En el menu no suena nada: se oye al jugar. */
static void ui_opt_sound(void)
{
    int i;
    g_stage = "opciones-sonido";
    ui_prime();
    while (g_running) {
        uint16_t e;
        char line[48], bar[16];
        pump();
        e = ui_edges();
        if (e & (UI_LEFT | UI_DOWN))  { g_volume = g_volume - 10 < 0 ? 0 : g_volume - 10; opts_save(); }
        if (e & (UI_RIGHT | UI_UP))   { g_volume = g_volume + 10 > 100 ? 100 : g_volume + 10; opts_save(); }
        if (e & (UI_OK | UI_CANCEL | UI_START)) break;
        if (hr_usable()) {
            static const HHint hh[2] = { { "<>", "CAMBIAR", HC_BG }, { "O", "VOLVER", HC_BO } };
            char num[12];
            int nx;
            hr_head("SONIDO  ·  volumen");
            hr_panel(230, 210, 1050, 560);
            hr_text(270, 262, "VOLUMEN", FS_M, HC_DIM, 2);
            snprintf(num, sizeof num, "%d", g_volume);
            nx = 270 + hr_text(270, 360, num, FS_X, HC_YEL, 4);
            hr_text(nx + 8, 360, "%", FS_H, HC_YEL, 3);
            for (i = 0; i < 10; i++)
                hr_rect(270 + i * 76, 392, 270 + i * 76 + 66, 428, i < g_volume / 10 ? HC_GREEN : 0xFF28368Au, 256);
            hr_text(270, 484, g_volume == 0 ? "Silencio" : "100 % es el volumen de siempre", FS_S, HC_DIM, 2);
            if (!g_cfg.audio) hr_text(270, 524, "AVISO: SONIDO APAGADO (R2 AL ABRIR)", FS_S, HC_AMBER, 2);
            hr_hints(hh, 2);
            present();
#ifdef NESNET_HOST
            sleep_ms(16);
#endif
            continue;
        }
        ui_clear();
        ui_center(20, "SONIDO", 2, 0xFFFFD040);
        snprintf(line, sizeof line, "VOLUMEN  %d%%", g_volume);
        ui_center(76, line, 2, 0xFFFFFFFF);
        for (i = 0; i < 10; i++) bar[i] = (char)(i < g_volume / 10 ? '#' : '-');
        bar[10] = 0;
        if (g_hr_on) hr_vol_bar(108, g_volume / 10); else ui_center(108, bar, 2, 0xFF80FF80);
        ui_center(140, g_volume == 0 ? "SILENCIO" : "100% ES EL VOLUMEN DE SIEMPRE", 1, 0xFFC0C0FF);
        if (!g_cfg.audio) ui_center(158, "AVISO: SONIDO APAGADO (R2 AL ABRIR)", 1, 0xFFFFC040);
        ui_center(190, "IZQ/DER: BAJAR / SUBIR", 1, 0xFF8080A0);
        ui_center(204, "X / O: VOLVER", 1, 0xFF8080A0);
        present();
#ifdef NESNET_HOST
        sleep_ms(16);
#endif
    }
    logf_("opciones: volumen = %d", g_volume);
}

/* v1.9: OPCIONES > INFO DE RED: que se ve arriba a la izquierda durante una partida online (no cambia la red, solo lo que se dibuja) */
static void ui_opt_netinfo(void)
{
    static const char *names[3] = { "SOLO AL INICIO (5 S)", "SIEMPRE", "NO MOSTRAR" };
    static const char *desc[3] = { "COMO HASTA AHORA", "DELAY, FPS Y TRABAS EN VIVO", "SOLO SALE EL AVISO ROJO DE DESYNC" };
    int i;
    g_stage = "opciones-info-red";
    ui_prime();
    while (g_running) {
        uint16_t e;
        pump();
        e = ui_edges();
        if (e & UI_UP)   { g_netinfo = (g_netinfo + 2) % 3; opts_save(); }
        if (e & UI_DOWN) { g_netinfo = (g_netinfo + 1) % 3; opts_save(); }
        if (e & (UI_OK | UI_CANCEL | UI_START)) break;
        if (hr_usable()) {
            static const HHint hh[2] = { { "X", "VOLVER", HC_BX }, { "O", "VOLVER", HC_BO } };
            hr_head("INFO DE RED  ·  en partida online, arriba a la izquierda");
            hr_panel(70, 210, 900, 570);
            hr_rows(96, 232, 778, names, 3, g_netinfo, 0, 58);
            hr_text(96, 446, desc[g_netinfo], FS_M, HC_GREEN, 2);
            hr_text(96, 496, "DELAY = cuadros de espera (1 cuadro = 17 ms)", FS_S, HC_DIM, 2);
            hr_text(96, 532, "TRABAS = cuadros lentos en el último segundo", FS_S, HC_DIM, 2);
            hr_hints(hh, 2);
            present();
#ifdef NESNET_HOST
            sleep_ms(16);
#endif
            continue;
        }
        ui_clear();
        ui_center(20, "INFO DE RED", 2, 0xFFFFD040);
        ui_center(46, "EN PARTIDA ONLINE (ARRIBA A LA IZQUIERDA)", 1, 0xFFC0C0FF);
        for (i = 0; i < 3; i++) {
            char line[48];
            snprintf(line, sizeof line, "%s %s", i == g_netinfo ? ">" : " ", names[i]);
            ui_center(84 + i * 22, line, 1, i == g_netinfo ? 0xFFFFFFFF : 0xFF8080A0);
        }
        ui_center(156, desc[g_netinfo], 1, 0xFF80FF80);
        ui_center(176, "DELAY = CUADROS DE ESPERA (1 CUADRO = 17 MS)", 1, 0xFF8080A0);
        ui_center(190, "TRABAS = CUADROS LENTOS EN EL ULTIMO SEGUNDO", 1, 0xFF8080A0);
        ui_center(210, "ARRIBA/ABAJO   X / O: VOLVER", 1, 0xFF8080A0);
        present();
#ifdef NESNET_HOST
        sleep_ms(16);
#endif
    }
    logf_("opciones: info de red = %d", g_netinfo);
}

/* v1.9.2: OPCIONES > AJUSTE DE IMAGEN: tamano (70 a 100 %) y desplazamiento horizontal / vertical (de 2 en 2 pixeles de la TV).
 * La imagen cambia en vivo; el marco blanco es el borde del lienzo: hay que ajustar hasta verlo entero en la TV. */
static void img_clamp(void)
{
    if (g_img_size < IMG_SIZE_MIN) g_img_size = IMG_SIZE_MIN;
    if (g_img_size > 100) g_img_size = 100;
    if (g_img_dx < -IMG_SHIFT_MAX) g_img_dx = -IMG_SHIFT_MAX;
    if (g_img_dx > IMG_SHIFT_MAX) g_img_dx = IMG_SHIFT_MAX;
    if (g_img_dy < -IMG_SHIFT_MAX) g_img_dy = -IMG_SHIFT_MAX;
    if (g_img_dy > IMG_SHIFT_MAX) g_img_dy = IMG_SHIFT_MAX;
}

static int hr_adj_ok(void)
{
    int a = g_adjusting, ok, dw, dh, z;
    g_adjusting = 0;
    ok = hr_usable();
    g_adjusting = a;
    shot_geometry(&dw, &dh, &z);
    return ok && dw == HR_W && dh == HR_H;
}

static void ui_opt_image(void)
{
    int sel = 0, i, x, y;
    g_stage = "opciones-imagen";
    g_adjusting = 1;                        /* v1.9.3: esta pantalla muestra el marco con la geometria real del juego */
    ui_prime();
    while (g_running) {
        uint16_t e;
        char line[48];
        pump();
        e = ui_edges();
        if (e & UI_UP)   sel = (sel + 4) % 5;
        if (e & UI_DOWN) sel = (sel + 1) % 5;
        if (e & UI_CANCEL) break;
        if (sel <= 2 && (e & (UI_LEFT | UI_RIGHT))) {
            int dir = (e & UI_RIGHT) ? 1 : -1;
            if (sel == 0) g_img_size += dir;
            else if (sel == 1) g_img_dx += 2 * dir;
            else g_img_dy += 2 * dir;
            img_clamp();
            opts_save();
        }
        if (e & (UI_OK | UI_START)) {
            if (sel == 3) { g_img_size = 100; g_img_dx = 0; g_img_dy = 0; opts_save(); }
            else if (sel == 4) break;
        }
        ui_clear();
        if (!hr_adj_ok()) {
        ui_center(20, "AJUSTE DE IMAGEN", 2, 0xFFFFD040);
        for (i = 0; i < 5; i++) {
            uint32_t col = i == sel ? 0xFFFFFFFF : 0xFF8080A0;
            if (i == 0)      snprintf(line, sizeof line, "%s TAMANO          %d%%", i == sel ? ">" : " ", g_img_size);
            else if (i == 1) snprintf(line, sizeof line, "%s HORIZONTAL    %+d", i == sel ? ">" : " ", g_img_dx);
            else if (i == 2) snprintf(line, sizeof line, "%s VERTICAL      %+d", i == sel ? ">" : " ", g_img_dy);
            else             snprintf(line, sizeof line, "%s %s", i == sel ? ">" : " ", i == 3 ? "RESTAURAR (100%, 0, 0)" : "VOLVER");
            ui_text(40, 56 + i * 16, line, 1, col);
        }
        ui_center(150, "IZQ/DER: CAMBIAR   ARRIBA/ABAJO: FILA", 1, 0xFFC0C0FF);
        ui_center(166, "AJUSTA HASTA VER EL MARCO BLANCO ENTERO", 1, 0xFF80FF80);
        ui_center(180, "(ES EL BORDE DE LA IMAGEN)", 1, 0xFF80FF80);
        if (!g_cfg.render) ui_center(196, "AVISO: EL MODO GPU NO USA ESTO", 1, 0xFFFFC040);
        ui_center(212, "O: VOLVER", 1, 0xFF8080A0);
        }
        /* marco blanco en el borde del lienzo, con una marca en cada esquina y en el centro de cada lado */
        for (x = 0; x < SCR_W; x++) { g_fb[x] = 0xFFFFFFFF; g_fb[(SCR_H - 1) * SCR_W + x] = 0xFFFFFFFF; }
        for (y = 0; y < SCR_H; y++) { g_fb[y * SCR_W] = 0xFFFFFFFF; g_fb[y * SCR_W + SCR_W - 1] = 0xFFFFFFFF; }
        for (i = 0; i < 8; i++) {
            g_fb[(1 + i) * SCR_W + 1] = 0xFFFF4040; g_fb[(1 + i) * SCR_W + SCR_W - 2] = 0xFFFF4040;
            g_fb[(SCR_H - 2 - i) * SCR_W + 1] = 0xFFFF4040; g_fb[(SCR_H - 2 - i) * SCR_W + SCR_W - 2] = 0xFFFF4040;
            g_fb[SCR_W + 1 + i] = 0xFFFF4040; g_fb[SCR_W + SCR_W - 2 - i] = 0xFFFF4040;
            g_fb[(SCR_H - 2) * SCR_W + 1 + i] = 0xFFFF4040; g_fb[(SCR_H - 2) * SCR_W + SCR_W - 2 - i] = 0xFFFF4040;
        }
        if (hr_adj_ok()) {          /* v1.9.7: el marco (con la geometria real) y encima el cuadro de ajustes a 1280x720 */
            static const HHint hh[2] = { { "<>", "CAMBIAR", HC_BG }, { "O", "VOLVER", HC_BO } };
            static unsigned fcn;
            int dw, dh, z;
            char v[24];
            shot_geometry(&dw, &dh, &z);
            hr_init();
            memcpy(g_hr, g_bg, sizeof g_hr);                 /* v1.9.9: las barras con el mosaico, no negras */
            g_bars_mosaic = (dw == HR_W && dh == HR_H);
            canvas_blit_adj(g_hr, dw, dh, g_screen, z, g_fb, &fcn, g_img_size, g_img_dx, g_img_dy);
            g_bars_mosaic = 0;
            hr_panel(300, 150, 980, 590);
            hr_text(330, 204, "AJUSTE DE IMAGEN", FS_H, HC_YEL, 3);
            for (i = 0; i < 5; i++) {
                const char *nm = i == 0 ? "TAMAÑO" : i == 1 ? "HORIZONTAL" : i == 2 ? "VERTICAL" : i == 3 ? "RESTAURAR  (100 %, 0, 0)" : "VOLVER";
                v[0] = 0;
                if (i == 0) snprintf(v, sizeof v, "%d %%", g_img_size);
                else if (i == 1) snprintf(v, sizeof v, "%+d", g_img_dx);
                else if (i == 2) snprintf(v, sizeof v, "%+d", g_img_dy);
                hr_row2(320, 226 + i * 46, 640, 46, nm, v[0] ? v : NULL, i == sel, 400);
            }
            hr_text(330, 500, "Ajusta hasta ver el marco blanco entero", FS_S, HC_GREEN, 2);
            hr_text(330, 534, "(es el borde de la imagen)", FS_S, HC_GREEN, 2);
            hr_hints(hh, 2);
            g_hr_on = 1;
        }
        present();
#ifdef NESNET_HOST
        sleep_ms(16);
#endif
    }
    g_adjusting = 0;
    logf_("opciones: imagen = %d%% %+d %+d", g_img_size, g_img_dx, g_img_dy);
}

static void ui_options(void)
{
    enum { N_OPT = 7 };
    static const char *items[N_OPT] = { "PANTALLA", "MANDOS (BOTONES Y TURBO)", "SONIDO (VOLUMEN)", "INFO DE RED EN PARTIDA", "AJUSTE DE IMAGEN", "", "VOLVER" };
    int sel = 0, i;
    g_stage = "opciones";
    ui_prime();
    while (g_running) {
        uint16_t e;
        pump();
        e = ui_edges();
        if (e & UI_UP)   sel = (sel + N_OPT - 1) % N_OPT;
        if (e & UI_DOWN) sel = (sel + 1) % N_OPT;
        if (e & UI_CANCEL) break;
        if (sel == 5 && (e & (UI_OK | UI_START | UI_LEFT | UI_RIGHT))) { g_showfps = !g_showfps; opts_save(); }   /* FPS: se prende y apaga en esta misma fila */
        else if (e & (UI_OK | UI_START)) {
            if (sel == 0) { ui_opt_screen(); ui_prime(); g_stage = "opciones"; }
            else if (sel == 1) { ui_opt_pads(); ui_prime(); g_stage = "opciones"; }
            else if (sel == 2) { ui_opt_sound(); ui_prime(); g_stage = "opciones"; }
            else if (sel == 3) { ui_opt_netinfo(); ui_prime(); g_stage = "opciones"; }
            else if (sel == 4) { ui_opt_image(); ui_prime(); g_stage = "opciones"; }
            else break;
        }
        if (hr_usable()) {
            static const HHint hh[2] = { { "X", "ENTRAR", HC_BX }, { "O", "VOLVER", HC_BO } };
            char fps[40], val[24];
            const char *rows[7];
            for (i = 0; i < 7; i++) rows[i] = items[i];
            snprintf(fps, sizeof fps, "FPS EN PANTALLA: %s", g_showfps ? "SÍ" : "NO");
            rows[5] = fps;
            hr_head("OPCIONES  ·  se guardan solas");
            hr_panel(70, 210, 900, 650);
            hr_rows(96, 232, 778, rows, 7, sel, 0, 58);
            hr_panel(940, 210, 1210, 430);
            hr_text(962, 250, "Ahora", FS_M, HC_YEL, 2);
            hr_text(962, 290, "Pantalla:", FS_S, HC_DIM, 2);
            hr_text(962, 324, screen_names[g_screen], hr_tw(screen_names[g_screen], FS_M) > 236 ? FS_S : FS_M, HC_WHITE, 2);
            hr_text(962, 368, "Volumen:", FS_S, HC_DIM, 2);
            snprintf(val, sizeof val, "%d %%", g_volume);
            hr_text(962, 402, val, FS_M, HC_WHITE, 2);
            hr_hints(hh, 2);
            present();
#ifdef NESNET_HOST
            sleep_ms(16);
#endif
            continue;
        }
        ui_clear();
        ui_center(20, "OPCIONES", 2, 0xFFFFD040);
        ui_center(46, "SE GUARDAN SOLAS", 1, 0xFFC0C0FF);
        for (i = 0; i < N_OPT; i++) {
            char line[48];
            if (i == 5) snprintf(line, sizeof line, "%s FPS EN PANTALLA: %s", i == sel ? ">" : " ", g_showfps ? "SI" : "NO");
            else snprintf(line, sizeof line, "%s %s", i == sel ? ">" : " ", items[i]);
            ui_center(66 + i * 18, line, 1, i == sel ? 0xFFFFFFFF : 0xFF8080A0);
        }
        ui_center(200, "ARRIBA/ABAJO + X", 1, 0xFF8080A0);
        ui_center(214, "O: VOLVER", 1, 0xFF8080A0);
        present();
#ifdef NESNET_HOST
        sleep_ms(16);
#endif
    }
}

static int ui_menu(void)
{
    static const char *items[4] = { "ELEGIR JUEGO", "JUGAR LOCAL (1 A 4 MANDOS)", "JUGAR ONLINE", "OPCIONES" };
    static const int modes[4] = { MODE_ROM, MODE_SOLO, MODE_ONLINE, MODE_OPT };
    int i;
    g_stage = "menu";
    ui_prime();
    while (g_running) {
        uint16_t e;
        char l[80], nm[48];
        pump();
        e = ui_edges();
        if (e & UI_UP)   g_menu_sel = (g_menu_sel + 3) % 4;
        if (e & UI_DOWN) g_menu_sel = (g_menu_sel + 1) % 4;
        if (e & UI_CANCEL) {
            /* v1.9.4: O en el menu principal sale al XMB, sin confirmar (igual que la salida del sistema: g_running = 0) */
            logf_("salida al XMB pedida con O en el menu principal");
            g_running = 0;
            return -1;
        }
        if (e & (UI_OK | UI_START)) {
            /* sin ROM elegida el juego queda en stand-by: solo se puede entrar a ELEGIR ROM y a OPCIONES */
            if (modes[g_menu_sel] != MODE_ROM && modes[g_menu_sel] != MODE_OPT && !g_rom) ui_message("PRIMERO ELIGE UN JUEGO", "OPCION: ELEGIR JUEGO", 120);
            else return modes[g_menu_sel];
        }
        if (hr_usable()) {
            static const HHint hh[2] = { { "X", "ELEGIR", HC_BX }, { "O", "SALIR AL XMB", HC_BO } };
            unsigned dis = 0;
            for (i = 0; i < 4; i++) if (!g_rom && modes[i] != MODE_ROM && modes[i] != MODE_OPT) dis |= 1u << i;
            hr_head("ARCADE PARA PS3  -  LOCAL Y ONLINE (V0.2)");
            hr_panel(70, 210, 790, 710);
            if (g_rom) {
                fit_name(nm, sizeof nm, g_rom_name, 34);
                snprintf(l, sizeof l, "JUEGO: %s", nm);
                hr_text(96, 252, l, FS_M, HC_GREEN, 2);
            } else hr_text(96, 252, "JUEGO: NINGUNO (ELIGE UNO)", FS_M, HC_AMBER, 2);
            hr_rows(96, 280, 668, items, 4, g_menu_sel, dis, 58);
            hr_panel(830, 210, 1210, 440);
            hr_text(856, 252, "EN PARTIDA", FS_M, HC_YEL, 2);
            hr_text(856, 302, "R3", FS_M, HC_WHITE, 2);  hr_text(930, 302, "menú", FS_S, HC_DIM, 2);
            hr_text(856, 352, "L2", FS_M, HC_WHITE, 2);  hr_text(930, 352, "captura de pantalla", FS_S, HC_DIM, 2);
            hr_text(856, 402, "O", FS_M, HC_WHITE, 2);   hr_text(930, 402, "salir al XMB (aquí)", FS_S, HC_DIM, 2);
            hr_hints(hh, 2);
            present();
#ifdef NESNET_HOST
            sleep_ms(16);
#endif
            continue;
        }
        ui_clear();
        ui_center(20, "ARCADE STATION V0.1", 2, 0xFFFFD040);
        ui_center(46, "ARCADE PARA PS3 - LOCAL Y ONLINE (V0.2)", 1, 0xFFC0C0FF);
        if (g_rom) {
            fit_name(nm, sizeof nm, g_rom_name, 34);
            snprintf(l, sizeof l, "JUEGO: %s", nm);
            ui_center(66, l, 1, 0xFF80FF80);
        } else {
            ui_center(66, "JUEGO: NINGUNO (ELIGE UNO)", 1, 0xFFFFC040);
        }
        for (i = 0; i < 4; i++) {
            char line[40];
            uint32_t col = i == g_menu_sel ? 0xFFFFFFFF : 0xFF8080A0;
            if (!g_rom && modes[i] != MODE_ROM && modes[i] != MODE_OPT) col = i == g_menu_sel ? 0xFFA0A0B0 : 0xFF505068;   /* bloqueadas sin ROM */
            snprintf(line, sizeof line, "%s %s", i == g_menu_sel ? ">" : " ", items[i]);
            ui_center(84 + i * 17, line, 1, col);
        }
        ui_center(196, "ARRIBA/ABAJO + X", 1, 0xFF8080A0);
        ui_center(209, "EN PARTIDA: R3 = MENU   L2 = CAPTURA", 1, 0xFF8080A0);
        ui_center(222, "O: SALIR AL XMB", 1, 0xFF8080A0);
        present();
#ifdef NESNET_HOST
        sleep_ms(16);
#endif
    }
    return -1;
}

/* Arcade Station: submenu JUGAR ONLINE. Devuelve 1 = CREAR 1vs1, 2 = UNIRSE 1vs1, 3 = CREAR multijugador (g_mode4 ya puesto),
 * 4 = UNIRSE multijugador (el modo lo manda el host), -1 = volver. */
static int ui_menu_online(void)
{
    static const char *items[7] = { "1 VS 1  -  CREAR SALA", "1 VS 1  -  UNIRSE A SALA", "2 VS 2  -  CREAR (TU: 2 MANDOS)",
                                    "2 VS 1  -  CREAR (TU: 2, AMIGO: 1)", "1 VS 2  -  CREAR (TU: 1, AMIGO: 2)",
                                    "MULTIJUGADOR  -  UNIRSE A SALA", "VOLVER" };
    int sel = 0, i;
    g_stage = "menu-online";
    ui_prime();
    while (g_running) {
        uint16_t e;
        pump();
        e = ui_edges();
        if (e & UI_UP)   sel = (sel + 6) % 7;
        if (e & UI_DOWN) sel = (sel + 1) % 7;
        if (e & UI_CANCEL) return -1;
        if (e & (UI_OK | UI_START)) {
            switch (sel) {
            case 0: return 1;
            case 1: return 2;
            case 2: g_mode4 = 0; return 3;
            case 3: g_mode4 = 1; return 3;
            case 4: g_mode4 = 2; return 3;
            case 5: return 4;
            default: return -1;
            }
        }
        if (hr_usable()) {
            static const HHint hh[2] = { { "X", "ELEGIR", HC_BX }, { "O", "VOLVER", HC_BO } };
            hr_head("JUGAR ONLINE");
            hr_panel(70, 190, 900, 700);
            hr_rows(96, 214, 780, items, 7, sel, 0, 58);
            hr_hints(hh, 2);
            present();
#ifdef NESNET_HOST
            sleep_ms(16);
#endif
            continue;
        }
        ui_clear();
        ui_center(14, "JUGAR ONLINE", 2, 0xFFFFD040);
        for (i = 0; i < 7; i++) ui_center(44 + i * 20, items[i], 1, i == sel ? 0xFFFFFFFF : 0xFF8080A0);
        ui_center(200, "ARRIBA/ABAJO + X    O: VOLVER", 1, 0xFF8080A0);
        present();
#ifdef NESNET_HOST
        sleep_ms(16);
#endif
    }
    return -1;
}

/* v1.8: submenu del modo de 4 jugadores. Devuelve MODE_HOST, MODE_JOIN o -1 (volver). Copia de la idea de ui_menu. */
static int ui_menu4(void)
{
    static const char *items[3] = { "CREAR SALA DE 4 (HOST)", "UNIRSE A SALA DE 4", "VOLVER" };
    int sel = 0, i;
    g_stage = "menu-4";
    ui_prime();
    while (g_running) {
        uint16_t e;
        pump();
        e = ui_edges();
        if (e & UI_UP)   sel = (sel + 2) % 3;
        if (e & UI_DOWN) sel = (sel + 1) % 3;
        if (e & UI_CANCEL) return -1;
        if (e & (UI_OK | UI_START)) return sel == 0 ? MODE_HOST : sel == 1 ? MODE_JOIN : -1;
        if (hr_usable()) {
            static const HHint hh[2] = { { "X", "ELEGIR", HC_BX }, { "O", "VOLVER", HC_BO } };
            hr_head("MULTIJUGADOR DE 4  ·  Four Score");
            hr_panel(70, 210, 790, 570);
            hr_text(96, 252, "HOST: mandos 1 y 2", FS_M, HC_GREEN, 2);
            hr_text(96, 290, "EL QUE SE UNE: mandos 3 y 4", FS_M, HC_GREEN, 2);
            hr_rows(96, 316, 668, items, 3, sel, 0, 58);
            hr_text(96, 540, "Solo juegos con Four Score", FS_S, HC_DIM, 2);
            hr_hints(hh, 2);
            present();
#ifdef NESNET_HOST
            sleep_ms(16);
#endif
            continue;
        }
        ui_clear();
        ui_center(20, "ARCADE STATION V0.1", 2, 0xFFFFD040);
        ui_center(46, "MULTIJUGADOR DE 4 (FOUR SCORE)", 1, 0xFFC0C0FF);
        ui_center(66, "HOST: MANDOS 1 Y 2", 1, 0xFF80FF80);
        ui_center(80, "EL QUE SE UNE: MANDOS 3 Y 4", 1, 0xFF80FF80);
        for (i = 0; i < 3; i++) {
            char line[40];
            snprintf(line, sizeof line, "%s %s", i == sel ? ">" : " ", items[i]);
            ui_center(110 + i * 22, line, 1, i == sel ? 0xFFFFFFFF : 0xFF8080A0);
        }
        ui_center(190, "SOLO JUEGOS CON FOUR SCORE", 1, 0xFF8080A0);
        ui_center(204, "EN PARTIDA: R3 = MENU   L2 = CAPTURA", 1, 0xFF8080A0);
        present();
#ifdef NESNET_HOST
        sleep_ms(16);
#endif
    }
    return -1;
}

static int ui_enter_code(char *out)
{
    int d[10], pos = 0, i, saved = 0;
    memset(d, 0, sizeof d);
    /* v1.9.3: si hay un codigo guardado de la vez anterior, aparece ya escrito; se confirma con START (no se conecta solo) */
    if (strlen(g_last_code) == 10) {
        for (i = 0; i < 10; i++) d[i] = g_last_code[i] - '0';
        saved = 1;
    }
    while (g_running) {
        uint16_t e;
        char line[40];
        pump();
        e = ui_edges();
        if (e & UI_UP)    { d[pos] = (d[pos] + 1) % 10; saved = 0; }
        if (e & UI_DOWN)  { d[pos] = (d[pos] + 9) % 10; saved = 0; }
        if (e & UI_LEFT)  { if (pos > 0) pos--; }
        if (e & (UI_RIGHT | UI_OK)) { if (pos < 9) pos++; }
        if (e & UI_CANCEL) return 0;
        if (e & UI_START) {
            for (i = 0; i < 10; i++) out[i] = (char)('0' + d[i]);
            out[10] = 0;
            {
                uint32_t vip; int vport;
                if (strcmp(g_last_code, out) != 0 && code_decode(out, &vip, &vport)) {   /* lo recuerda para la proxima vez (aunque se apague la PS3); un codigo invalido no se guarda */
                    snprintf(g_last_code, sizeof g_last_code, "%.10s", out);
                    opts_save();
                    logf_("codigo de sala guardado en opciones.txt");
                }
            }
            return 1;
        }
        if (hr_usable()) {
            static const HHint hh[2] = { { "START", "CONECTAR", HC_BX }, { "O", "CANCELAR", HC_BO } };
            int adv = hr_tw("0", FS_X), tot = 10 * adv + 2 * 36, x0 = (HR_W - tot) / 2, k;
            hr_head("UNIRSE A UNA SALA");
            hr_panel(230, 200, 1050, 600);
            hr_text(270, 252, "ESCRIBE EL CÓDIGO DEL HOST", FS_M, HC_DIM, 2);
            for (k = 0; k < 10; k++) {
                char c1[2] = { (char)('0' + d[k]), 0 };
                int x = x0 + k * adv + (k >= 3 ? 36 : 0) + (k >= 6 ? 36 : 0);
                hr_text(x, 372, c1, FS_X, k == pos ? HC_YEL : HC_WHITE, 4);
                if (k == pos) hr_rect(x + 4, 392, x + adv - 8, 402, HC_YEL, 256);
            }
            snprintf(line, sizeof line, "Dígito %d de 10", pos + 1);
            hr_text(270, 460, line, FS_S, HC_DIM, 2);
            if (saved) hr_text(270, 500, "Último código usado: START para conectar", FS_S, HC_GREEN, 2);
            hr_text(270, 548, "ARRIBA/ABAJO cambia el dígito  ·  IZQ/DER mueve", FS_S, HC_DIM, 2);
            hr_hints(hh, 2);
            present();
#ifdef NESNET_HOST
            sleep_ms(16);
#endif
            continue;
        }
        ui_clear();
        ui_center(20, "ARCADE STATION V0.1", 2, 0xFFFFD040);
        ui_center(60, "ESCRIBE EL CODIGO DEL HOST", 1, 0xFFFFFFFF);
        for (i = 0; i < 10; i++) {
            char c[2] = { (char)('0' + d[i]), 0 };
            int x = 24 + i * 20 + (i >= 3 ? 8 : 0) + (i >= 6 ? 8 : 0);     /* grupos 3-3-4 */
            ui_text(x, 100, c, 2, i == pos ? 0xFFFFFF40 : 0xFFFFFFFF);
            if (i == pos) ui_text(x, 118, "-", 2, 0xFFFFFF40);
        }
        snprintf(line, sizeof line, "DIGITO %d DE 10", pos + 1);
        ui_center(140, line, 1, 0xFFC0C0FF);
        if (saved) ui_center(154, "ULTIMO CODIGO USADO (START: CONECTAR)", 1, 0xFF80FF80);
        ui_center(170, "ARRIBA/ABAJO: CAMBIAR   IZQ/DER: MOVER", 1, 0xFF8080A0);
        ui_center(184, "START: CONECTAR    O: CANCELAR", 1, 0xFF8080A0);
        present();
#ifdef NESNET_HOST
        sleep_ms(16);
#endif
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Modos                                                               */
/* ------------------------------------------------------------------ */
static void net_close(void)
{
    if (g_net >= 0) { closesocket(g_net); g_net = -1; }
    g_rxlen = 0;
}

static int run_online(int is_host, int D)
{
    int ok;
    for (;;) {
        if (!core_load()) { net_close(); return 0; }
        ok = session_loop(is_host ? 0 : 1, D, g_maxframes);
        core_unload();
        if (!g_sw_pending) break;
        /* v1.9.2: cambio de juego: marca de fin, ROM nueva y otra sesion con el mismo delay (sin repetir el handshake) */
        g_sw_pending = 0;
        if (!game_switch_apply()) { ok = 0; break; }
    }
    net_close();
    return ok;
}

static int run_host_inner(void)
{
    uint32_t ip = 0;
    char code[20], l2[48], l3[48];
    int lfd, D = 0, got_ip = 0, mism = 0;
    uint32_t col3;
    Upnp up;

    net_ensure();
    memset(&up, 0, sizeof up);
    if (g_cfg.upnp) {
        g_stage = "upnp";
        ui_show("CREANDO SALA...", "ABRIENDO EL PUERTO (UPNP)", NULL);
        upnp_open(g_cfg.port, &up);
        if (!g_running) return 1;
    } else {
        logf_("upnp: apagado (upnp=off en config.txt)");
    }
    g_stage = "buscando-ip";
    ui_show("CREANDO SALA...", "BUSCANDO TU IP PUBLICA", NULL);
    if (g_cfg.public_ip[0] && parse_ip(g_cfg.public_ip, &ip)) { got_ip = 1; logf_("IP publica fijada a mano (public_ip=)"); }
    else if (get_public_ip(&ip)) got_ip = 1;
    else logf_("ipify: no pude obtener la IP publica");
    if (!got_ip && up.public_ip && !ip_privada(up.public_ip)) {
        ip = up.public_ip; got_ip = 1;
        logf_("uso la IP WAN que dio el router por UPnP");
    }
    if (!got_ip) { g_err = "NO PUDE OBTENER TU IP PUBLICA"; return 0; }
    if (up.public_ip && up.public_ip != ip) {
        mism = 1;
        logf_("AVISO: la IP del router (%u.%u.%u.%u) es distinta de la que ve internet (%u.%u.%u.%u): "
              "probable CGNAT o doble NAT, desde afuera no se llega al puerto", IP4(up.public_ip), IP4(ip));
    }
    code_encode(ip, g_cfg.port, code);
    logf_("sala creada: ip=%u.%u.%u.%u puerto=%d codigo=%s", IP4(ip), g_cfg.port, code);

    lfd = tcp_listen(g_cfg.port);
    if (lfd < 0) { g_err = "NO PUDE ABRIR EL PUERTO"; return 0; }

    snprintf(l2, sizeof l2, "PUERTO TCP %d", g_cfg.port);
    col3 = 0xFFFFC040;
    if (!g_cfg.upnp)                                   snprintf(l3, sizeof l3, "UPNP APAGADO: ABRE EL PUERTO");
    else if (!up.router)                               snprintf(l3, sizeof l3, "SIN UPNP: ABRE EL PUERTO A MANO");
    else if (up.public_ip && ip_privada(up.public_ip)) snprintf(l3, sizeof l3, "ROUTER SIN IP PUBLICA (CGNAT)");
    else if (mism)                                     snprintf(l3, sizeof l3, "ROUTER E INTERNET: IP DISTINTA");
    else if (up.opened)                              { snprintf(l3, sizeof l3, "UPNP: PUERTO ABIERTO"); col3 = 0xFF80FF80; }
    else                                               snprintf(l3, sizeof l3, "UPNP RECHAZADO: ABRE EL PUERTO");
    logf_("estado en pantalla: %s", l3);
    g_stage = "esperando-amigo";
    for (;;) {
        uint16_t e;
        pump();
        if (!g_running) { closesocket(lfd); return 1; }
        e = ui_edges();
        if (e & UI_CANCEL) { closesocket(lfd); g_err = ""; return 1; }
        if (hr_usable()) {
            hr_room_screen("CREAR SALA (HOST)", code, l2, l3, col3);
            present();
        } else {
        ui_clear();
        ui_center(20, "ARCADE STATION V0.1", 2, 0xFFFFD040);
        ui_center(64, "TU CODIGO DE SALA:", 1, 0xFFFFFFFF);
        { char pc[24]; code_pretty(code, pc); ui_center(84, pc, 2, 0xFFFFFF40); }
        ui_center(116, "DASELO A TU AMIGO", 1, 0xFFC0C0FF);
        ui_center(136, l2, 1, 0xFFC0C0FF);
        ui_center(154, l3, 1, col3);
        ui_center(176, "ESPERANDO AL AMIGO...", 1, 0xFFFFFFFF);
        ui_center(200, "O: CANCELAR", 1, 0xFF8080A0);
        present();
        }
        {
            int r = fd_wait(lfd, POLLIN, 0);
            if (r > 0) {
                struct sockaddr_in pa;
                socklen_t pl = sizeof pa;
                int c;
                memset(&pa, 0, sizeof pa);
                c = accept(lfd, (struct sockaddr *)&pa, &pl);
                if (c >= 0) {
                    uint32_t pip = ntohl(pa.sin_addr.s_addr);
                    logf_("conexion entrante de %u.%u.%u.%u", IP4(pip));
                    g_net = c; sock_nodelay(c); break;
                }
                logf_("accept fallo (errno=%d)", errno);
            }
#ifdef NESNET_HOST
            sleep_ms(16);
#endif
        }
    }
    closesocket(lfd);
    ui_show("AMIGO CONECTADO", "MIDIENDO LATENCIA...", NULL);
    if (!handshake_host(&D)) { net_close(); return 0; }
    return run_online(1, D);
}

static int run_host(void)
{
    int ok = run_host_inner();
    upnp_close();
    return ok;
}

static int run_client(void)
{
    char code[20];
    uint32_t ip;
    int port, D = 0;

    if (g_cfg.code[0]) snprintf(code, sizeof code, "%s", g_cfg.code);
    else if (!ui_enter_code(code)) { g_err = ""; return 1; }

    if (!code_decode(code, &ip, &port)) { logf_("codigo escrito=%s: INVALIDO", code); g_err = "CODIGO INVALIDO"; return 0; }
    logf_("codigo escrito=%s -> destino %u.%u.%u.%u:%d", code, IP4(ip), port);
    net_ensure();
    g_stage = "conectando";
    {
        char l2[48];
        snprintf(l2, sizeof l2, "%u.%u.%u.%u:%d", IP4(ip), port);
        ui_show("CONECTANDO...", l2, NULL);
    }
    g_net = tcp_connect(ip, port, 10000);
    if (g_net < 0) {
        g_err = g_conn_kind == 1 ? "SIN RESPUESTA DEL HOST (10 S)" : "EL HOST RECHAZO LA CONEXION";
        return 0;
    }
    logf_("connect OK en %u ms", (unsigned)g_conn_ms);
    ui_show("CONECTADO", "SINCRONIZANDO...", NULL);
    if (!handshake_client(&D)) { net_close(); return 0; }
    return run_online(0, D);
}

/* v1.8: COPIAS de run_host_inner / run_host / run_client para el modo de 4. El 1vs1 de arriba no se toca. */
/* v1.8: COPIA de run_online() para el modo de 4: carga el nucleo con 4 mandos (Four Score) y corre session_loop4 */
static int run_online4(int is_host, int D)
{
    int ok;
    g_four = 1;
    for (;;) {
        if (!core_load()) { g_four = 0; net_close(); return 0; }
        ok = session_loop4(is_host ? 0 : 1, D, g_maxframes);
        core_unload();
        if (!g_sw_pending) break;
        g_sw_pending = 0;                                  /* v1.9.2: cambio de juego (igual que en el 1vs1) */
        if (!game_switch_apply()) { ok = 0; break; }
    }
    g_four = 0;
    net_close();
    return ok;
}

static int run_host4_inner(void)
{
    uint32_t ip = 0;
    char code[20], l2[48], l3[48];
    int lfd, D = 0, got_ip = 0, mism = 0;
    uint32_t col3;
    Upnp up;

    net_ensure();
    memset(&up, 0, sizeof up);
    if (g_cfg.upnp) {
        g_stage = "upnp";
        ui_show("CREANDO SALA...", "ABRIENDO EL PUERTO (UPNP)", NULL);
        upnp_open(g_cfg.port, &up);
        if (!g_running) return 1;
    } else {
        logf_("upnp: apagado (upnp=off en config.txt)");
    }
    g_stage = "buscando-ip";
    ui_show("CREANDO SALA...", "BUSCANDO TU IP PUBLICA", NULL);
    if (g_cfg.public_ip[0] && parse_ip(g_cfg.public_ip, &ip)) { got_ip = 1; logf_("IP publica fijada a mano (public_ip=)"); }
    else if (get_public_ip(&ip)) got_ip = 1;
    else logf_("ipify: no pude obtener la IP publica");
    if (!got_ip && up.public_ip && !ip_privada(up.public_ip)) {
        ip = up.public_ip; got_ip = 1;
        logf_("uso la IP WAN que dio el router por UPnP");
    }
    if (!got_ip) { g_err = "NO PUDE OBTENER TU IP PUBLICA"; return 0; }
    if (up.public_ip && up.public_ip != ip) {
        mism = 1;
        logf_("AVISO: la IP del router (%u.%u.%u.%u) es distinta de la que ve internet (%u.%u.%u.%u): "
              "probable CGNAT o doble NAT, desde afuera no se llega al puerto", IP4(up.public_ip), IP4(ip));
    }
    code_encode(ip, g_cfg.port, code);
    logf_("sala de 4 creada: ip=%u.%u.%u.%u puerto=%d codigo=%s", IP4(ip), g_cfg.port, code);

    lfd = tcp_listen(g_cfg.port);
    if (lfd < 0) { g_err = "NO PUDE ABRIR EL PUERTO"; return 0; }

    snprintf(l2, sizeof l2, "PUERTO TCP %d", g_cfg.port);
    col3 = 0xFFFFC040;
    if (!g_cfg.upnp)                                   snprintf(l3, sizeof l3, "UPNP APAGADO: ABRE EL PUERTO");
    else if (!up.router)                               snprintf(l3, sizeof l3, "SIN UPNP: ABRE EL PUERTO A MANO");
    else if (up.public_ip && ip_privada(up.public_ip)) snprintf(l3, sizeof l3, "ROUTER SIN IP PUBLICA (CGNAT)");
    else if (mism)                                     snprintf(l3, sizeof l3, "ROUTER E INTERNET: IP DISTINTA");
    else if (up.opened)                              { snprintf(l3, sizeof l3, "UPNP: PUERTO ABIERTO"); col3 = 0xFF80FF80; }
    else                                               snprintf(l3, sizeof l3, "UPNP RECHAZADO: ABRE EL PUERTO");
    logf_("estado en pantalla: %s", l3);
    g_stage = "esperando-amigo";
    for (;;) {
        uint16_t e;
        pump();
        if (!g_running) { closesocket(lfd); return 1; }
        e = ui_edges();
        if (e & UI_CANCEL) { closesocket(lfd); g_err = ""; return 1; }
        if (hr_usable()) {
            hr_room_screen("SALA DE 4 (HOST)", code, l2, l3, col3);
            present();
        } else {
        ui_clear();
        ui_center(20, "ARCADE STATION V0.1", 2, 0xFFFFD040);
        ui_center(64, "SALA DE 4 - TU CODIGO:", 1, 0xFFFFFFFF);
        { char pc[24]; code_pretty(code, pc); ui_center(84, pc, 2, 0xFFFFFF40); }
        ui_center(116, "DASELO A TU AMIGO", 1, 0xFFC0C0FF);
        ui_center(136, l2, 1, 0xFFC0C0FF);
        ui_center(154, l3, 1, col3);
        ui_center(176, "ESPERANDO AL AMIGO...", 1, 0xFFFFFFFF);
        ui_center(200, "O: CANCELAR", 1, 0xFF8080A0);
        present();
        }
        {
            int r = fd_wait(lfd, POLLIN, 0);
            if (r > 0) {
                struct sockaddr_in pa;
                socklen_t pl = sizeof pa;
                int c;
                memset(&pa, 0, sizeof pa);
                c = accept(lfd, (struct sockaddr *)&pa, &pl);
                if (c >= 0) {
                    uint32_t pip = ntohl(pa.sin_addr.s_addr);
                    logf_("conexion entrante de %u.%u.%u.%u", IP4(pip));
                    g_net = c; sock_nodelay(c); break;
                }
                logf_("accept fallo (errno=%d)", errno);
            }
#ifdef NESNET_HOST
            sleep_ms(16);
#endif
        }
    }
    closesocket(lfd);
    ui_show("AMIGO CONECTADO", "MIDIENDO LATENCIA...", NULL);
    if (!handshake_host4(&D)) { net_close(); return 0; }
    return run_online4(1, D);
}

static int run_host4(void)
{
    int ok = run_host4_inner();
    upnp_close();
    return ok;
}

static int run_client4(void)
{
    char code[20];
    uint32_t ip;
    int port, D = 0;

    if (g_cfg.code[0]) snprintf(code, sizeof code, "%s", g_cfg.code);
    else if (!ui_enter_code(code)) { g_err = ""; return 1; }

    if (!code_decode(code, &ip, &port)) { logf_("codigo escrito=%s: INVALIDO", code); g_err = "CODIGO INVALIDO"; return 0; }
    logf_("codigo escrito=%s -> destino %u.%u.%u.%u:%d", code, IP4(ip), port);
    net_ensure();
    g_stage = "conectando";
    {
        char l2[48];
        snprintf(l2, sizeof l2, "%u.%u.%u.%u:%d", IP4(ip), port);
        ui_show("CONECTANDO...", l2, NULL);
    }
    g_net = tcp_connect(ip, port, 10000);
    if (g_net < 0) {
        g_err = g_conn_kind == 1 ? "SIN RESPUESTA DEL HOST (10 S)" : "EL HOST RECHAZO LA CONEXION";
        return 0;
    }
    logf_("connect OK en %u ms", (unsigned)g_conn_ms);
    ui_show("CONECTADO", "SINCRONIZANDO...", NULL);
    if (!handshake_client4(&D)) { net_close(); return 0; }
    return run_online4(0, D);
}

static int run_solo(void)
{
    if (!core_load()) return 0;
    sram_begin();                         /* v1.9: carga el guardado de bateria (si el juego lo usa) */
    session_loop(-1, 0, g_maxframes);
    sram_save(0);                         /* v1.9: guarda al salir (si cambio) */
    g_sram_on = 0;
    core_unload();
    return 1;
}

/* ------------------------------------------------------------------ */
/* Programa                                                            */
/* ------------------------------------------------------------------ */
static int g_prepared;
static void app_prepare(const char *argv0)
{
    if (g_prepared) return;
    g_prepared = 1;
    if (argv0) {
        const char *s = strrchr(argv0, '/');
        if (s && s > argv0) {
            size_t n = (size_t)(s - argv0);
            if (n < sizeof g_dir) { memcpy(g_dir, argv0, n); g_dir[n] = 0; }
        }
    }
    {
        char p[300], q[300];
        snprintf(p, sizeof p, "%s/arcade.log", g_dir);
        snprintf(q, sizeof q, "%s/arcade.old.log", g_dir);
        remove(q);
        rename(p, q);                     /* conserva el log de la vez anterior */
        g_log = fopen(p, "w");
    }
    logf_("BUILD fba2012-1 (nucleo FB Alpha 2012, solo arcade; los mensajes del nucleo van a este log)");
    logf_("Arcade Station v0.1.1 (nucleo FB Alpha 2012, solo local; 1 a 4 mandos segun el juego; lienzo 320x240; render por CPU; base Katari v0.1.4 / NesNet v1.9.9) - dir=%s", g_dir);
    { char fd[300]; snprintf(fd, sizeof fd, "%s/fbneo", g_dir); mkdir(fd, 0777); }   /* Arcade Station: carpeta de FBNeo (muestras, nvram, hiscore) */
    cfg_load();
    logf_("config: res=%d render=%s zoom=%d audio=%s", g_cfg.res, g_cfg.render ? "cpu" : "gpu", g_cfg.zoom, g_cfg.audio ? "on" : "off");
    opts_load();                          /* v1.8.2: pantalla y mandos guardados en opciones.txt */
}

static int app_main(const char *argv0)
{
    int choice = -1, from_cfg;

    app_prepare(argv0);

    /* v1.7: arranca en stand-by, sin ROM. La ROM se elige en el menu (ELEGIR ROM) antes de crear/unirse/jugar */
    logf_("stand-by: sin ROM elegida (roms en %s/roms)", g_dir);

    core_boot();

    /* v1.9: si hay una ultima ROM recordada y el archivo sigue en la carpeta, queda ya elegida (el juego sigue sin arrancar) */
    if (g_last_rom[0]) {
        int i, found = -1;
        roms_scan();
        for (i = 0; i < g_nroms; i++) if (!strcmp(g_roms[i].name, g_last_rom)) found = i;
        if (found >= 0 && rom_load(found)) { logf_("ultima ROM recordada y cargada: %s", g_rom_name); g_menu_sel = 1; }
        else logf_("la ultima ROM recordada (%s) ya no esta o no se pudo leer: stand-by", g_last_rom);
    }

    from_cfg = g_cfg.mode[0] != 0;
    if (from_cfg) { logf_("config.txt trae mode=%s: se ignora, primero hay que elegir ROM en el menu", g_cfg.mode); from_cfg = 0; }
    while (g_running) {
        int ok = 1;
        if (choice < 0) choice = ui_menu();
        if (choice < 0) break;
        if (choice == MODE_ROM) {
            if (ui_rom_select(0)) g_menu_sel = 1;          /* tras elegir, el cursor pasa a CREAR SALA */
            choice = -1;
            continue;
        }
        if (choice == MODE_OPT) {                         /* v1.8.2: OPCIONES (pantalla y mandos) no necesita ROM */
            ui_options();
            choice = -1;
            continue;
        }
        if (!g_rom) { choice = -1; continue; }             /* seguridad: sin ROM no se arranca nada */
        if (choice == MODE_ONLINE) {
            int sub = ui_menu_online();
            if (sub < 0) { choice = -1; continue; }
            logf_("modo elegido: ONLINE %s", sub == 1 ? "1VS1 CREAR" : sub == 2 ? "1VS1 UNIRSE" : sub == 3 ? (g_mode4 == 0 ? "2VS2 CREAR" : g_mode4 == 1 ? "2VS1 CREAR" : "1VS2 CREAR") : "MULTIJUGADOR UNIRSE");
            g_err = "";
            ok = sub == 1 ? run_host() : sub == 2 ? run_client() : sub == 3 ? run_host4() : run_client4();
            choice = -1;
        } else if (choice == MODE_FOUR) {
            /* v1.8: modo de 4 jugadores: submenu aparte con host y unirse (copias del 1vs1, otro protocolo) */
            int sub = ui_menu4();
            if (sub < 0) { choice = -1; continue; }         /* VOLVER / O: regresa al menu principal */
            logf_("modo elegido: MULTIJUGADOR DE 4 - %s", sub == MODE_HOST ? "CREAR SALA (host, mandos 1-2)" : "UNIRSE (mandos 3-4)");
            g_err = "";
            ok = sub == MODE_HOST ? run_host4() : run_client4();
            choice = -1;
        } else {
        logf_("modo elegido: %s", choice == MODE_SOLO ? "SOLO (2 mandos)" : choice == MODE_HOST ? "CREAR SALA (host)" : "UNIRSE (cliente)");
        g_err = "";
        if (choice == MODE_SOLO) ok = run_solo();
        else if (choice == MODE_HOST) ok = run_host();
        else ok = run_client();
        choice = -1;
        }
#ifdef NESNET_HOST
        if (g_maxframes && !g_ui_has_script) g_running = 0;   /* pruebas: una sola sesion (con guion, sigue hasta que se agote) */
#endif
        if (!ok || g_err[0]) {
            logf_("fin con error: %s", g_err);
            if (g_err[0]) ui_message(g_err, "X: VOLVER AL MENU", 600);
        }
    }
    logf_("salida");
    au_close();
    if (g_core_loaded) { retro_unload_game(); g_core_loaded = 0; }
    retro_deinit();
    if (g_log) fclose(g_log);
    return 0;
}

#ifdef NESNET_PS3
int main(int argc, const char *argv[])
{
    app_prepare(argc > 0 ? argv[0] : NULL);
    ioPadInit(7);
    /* Modo seguro sin tocar archivos: mantener L1 al arrancar = forzar 720p; R1 = render por CPU */
    {
        int i;
        for (i = 0; i < 25; i++) {            /* ~0,5 s para que el mando responda */
            padData *d = pad_read(0);
            if (d->BTN_L1) { g_cfg.res = 720; }
            if (d->BTN_R1) { g_cfg.render = 1; }
            if (d->BTN_R2) { g_cfg.audio = 0; }
            usleep(20000);
        }
        if (g_cfg.res || g_cfg.render) logf_("config final: res=%d render=%s", g_cfg.res, g_cfg.render ? "cpu" : "gpu");
    }
    platform_init();
    au_open();
    return app_main(argc > 0 ? argv[0] : NULL);
}
#endif

#ifdef NESNET_HOST
/* Pruebas en PC:
 *   nesnet_test ROM local   FRAMES            -> sin retraso, 2 jugadores simulados
 *   nesnet_test ROM ref     FRAMES D          -> referencia local con retraso D
 *   nesnet_test ROM host    FRAMES PUERTO [D] -> host de red
 *   nesnet_test ROM client  FRAMES PUERTO     -> cliente de red (127.0.0.1)
 *   nesnet_test --ui DIR                      -> vuelca pantallas de la interfaz en PPM
 */
static void dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) return;
    if (g_hr_on) {      /* v1.9.7: cuadro de menu de 1280x720 */
        fprintf(f, "P6\n%d %d\n255\n", HR_W, HR_H);
        for (i = 0; i < HR_W * HR_H; i++) {
            uint8_t px[3] = { (uint8_t)(g_hr[i] >> 16), (uint8_t)(g_hr[i] >> 8), (uint8_t)g_hr[i] };
            fwrite(px, 1, 3, f);
        }
        fclose(f);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", SCR_W, SCR_H);
    for (i = 0; i < SCR_W * SCR_H; i++) {
        uint8_t px[3] = { (uint8_t)(g_fb[i] >> 16), (uint8_t)(g_fb[i] >> 8), (uint8_t)g_fb[i] };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *mode;
    uint32_t frames;
    int D = 6, port = 7777, ok = 1;

    if (argc >= 3 && !strcmp(argv[1], "--ui")) {
        char p[300];
        static const char *items[3] = { "CREAR SALA (HOST)", "UNIRSE A UNA SALA", "SOLO - 2 MANDOS" };
        int i;
        ui_clear();
        ui_center(20, "ARCADE STATION V0.1", 2, 0xFFFFD040);
        ui_center(46, "ARCADE PARA PS3 - LOCAL Y ONLINE (V0.2)", 1, 0xFFC0C0FF);
        for (i = 0; i < 3; i++) {
            char line[40];
            snprintf(line, sizeof line, "%s %s", i == 0 ? ">" : " ", items[i]);
            ui_center(90 + i * 22, line, 1, i == 0 ? 0xFFFFFFFF : 0xFF8080A0);
        }
        snprintf(p, sizeof p, "%s/menu.ppm", argv[2]); dump_ppm(p);
        {
            char code[20];
            code_encode(((uint32_t)181 << 24) | (45u << 16) | (200u << 8) | 17u, 7777, code);
            if (hr_usable()) hr_room_screen("SALA DE 4 (HOST)", code, "puerto 7777", "UPNP: PUERTO ABIERTO", 0xFF80FF80u); else {
            ui_clear();
            ui_center(20, "ARCADE STATION V0.1", 2, 0xFFFFD040);
            ui_center(64, "TU CODIGO DE SALA:", 1, 0xFFFFFFFF);
            { char pc[24]; code_pretty(code, pc); ui_center(84, pc, 2, 0xFFFFFF40); }
            ui_center(116, "DASELO A TU AMIGO", 1, 0xFFC0C0FF);
            ui_center(136, "PUERTO 7777", 1, 0xFFC0C0FF);
            ui_center(170, "ESPERANDO AL AMIGO...", 1, 0xFFFFFFFF);
            ui_center(200, "O: CANCELAR", 1, 0xFF8080A0);
            }
            snprintf(p, sizeof p, "%s/host.ppm", argv[2]); dump_ppm(p);
        }
        return 0;
    }

    if (argc < 4) { fprintf(stderr, "uso: %s ROM modo FRAMES [...]\n", argv[0]); return 2; }
    mode = argv[2];
    frames = (uint32_t)atoi(argv[3]);
    if (argc > 4) port = atoi(argv[4]);
    g_maxframes = frames;
    if (getenv("NESNET_MODE4")) g_mode4 = atoi(getenv("NESNET_MODE4"));          /* prueba: modo multijugador del host / de la referencia */
    if (getenv("NESNET_UI")) { g_ui_script = getenv("NESNET_UI"); g_ui_has_script = 1; }   /* pruebas del menu de salir */
    if (getenv("NESNET_R2")) g_exit_at = (uint32_t)atoi(getenv("NESNET_R2"));
    if (getenv("NESNET_R2B")) g_exit_at2 = (uint32_t)atoi(getenv("NESNET_R2B"));
    if (getenv("NESNET_NETINFO")) g_netinfo = atoi(getenv("NESNET_NETINFO"));   /* prueba: modo de info de red */
    if (getenv("NESNET_FPS")) g_showfps = atoi(getenv("NESNET_FPS"));           /* prueba v1.9.2: FPS en pantalla */
    if (getenv("NESNET_SCREEN")) g_screen = atoi(getenv("NESNET_SCREEN"));       /* prueba v1.9.6: pantalla ORIGINAL / 4:3 / 16:9 (0, 1, 2) */
    if (getenv("NESNET_SHOTF")) {                                                /* prueba v1.9.6: "se aprieta L2" en estos cuadros (a,b,c) */
        const char *sl = getenv("NESNET_SHOTF");
        int si = 0;
        while (*sl && si < 8) { g_shot_at[si++] = (uint32_t)atoi(sl); while (*sl && *sl != ',') sl++; if (*sl == ',') sl++; }
        g_cfg.render = 1;
    }
    if (getenv("NESNET_RESET_REF")) {                                            /* prueba v1.9.1: reinicios de la referencia "a" o "a,b" */
        const char *rr = getenv("NESNET_RESET_REF");
        g_ref_reset[0] = (uint32_t)atoi(rr);
        if (strchr(rr, ',')) g_ref_reset[1] = (uint32_t)atoi(strchr(rr, ',') + 1);
    }
    signal(SIGPIPE, SIG_IGN);
    if (!strcmp(mode, "app")) {
        /* flujo real: lee DIR/config.txt y ejecuta app_main (argv[4] = carpeta) */
        char exe[300];
        int rc;
        snprintf(exe, sizeof exe, "%s/EBOOT.BIN", argv[4]);
        if (getenv("NESNET_UI")) { g_ui_script = getenv("NESNET_UI"); g_ui_has_script = 1; }
        if (getenv("NESNET_R2")) g_exit_at = (uint32_t)atoi(getenv("NESNET_R2"));
        rc = app_main(exe);
        printf("RESULT mode=app frames=%u hash=%08X desyncs=%d rc=%d\n", (unsigned)g_frames_done,
               (unsigned)g_run_hash, g_desyncs, rc);
        return rc;
    }
    snprintf(g_cfg.rom, sizeof g_cfg.rom, "%s", argv[1]);
    g_cfg.port = port;
    if (!read_whole_file(argv[1], &g_rom, &g_romsz)) { fprintf(stderr, "no puedo leer la ROM\n"); return 2; }
    g_romcrc = crc32_buf(g_rom, g_romsz);
    snprintf(g_dir, sizeof g_dir, "/tmp");
    if (getenv("NESNET_DIR")) snprintf(g_dir, sizeof g_dir, "%s", getenv("NESNET_DIR"));   /* prueba v1.9.2: carpeta propia (con roms/) por proceso */
    core_boot();

    if (!strcmp(mode, "local")) {
        core_load(); session_loop(-1, 0, frames); core_unload();
    } else if (!strcmp(mode, "ref")) {
        D = argc > 4 ? atoi(argv[4]) : 6;
        core_load(); session_loop(-2, D, frames); core_unload();
    } else if (!strcmp(mode, "host")) {
        int lfd = tcp_listen(port);
        if (lfd < 0) { fprintf(stderr, "no puedo escuchar\n"); return 2; }
        if (argc > 5) g_cfg.delay = atoi(argv[5]);
        g_net = accept(lfd, NULL, NULL);
        closesocket(lfd);
        sock_nodelay(g_net);
        ok = handshake_host(&D);
        if (ok) ok = run_online(1, D) ;
        if (!ok) fprintf(stderr, "ERROR: %s\n", g_err);
    } else if (!strcmp(mode, "client")) {
        int tries;
        for (tries = 0; tries < 50 && g_net < 0; tries++) {
            g_net = tcp_connect(0x7F000001u, port, 2000);
            if (g_net < 0) sleep_ms(100);
        }
        if (g_net < 0) { fprintf(stderr, "no pude conectar\n"); return 2; }
        ok = handshake_client(&D);
        if (ok) ok = run_online(0, D);
        if (!ok) fprintf(stderr, "ERROR: %s\n", g_err);
    } else if (!strcmp(mode, "sticktest")) {
        /* v1.8.1: prueba de la palanca: recorre los 256 valores de cada eje y comprueba direcciones y zona muerta */
        unsigned v, bad = 0;
        for (v = 0; v < 256; v++) {
            uint16_t h = stick_nes_bits(v, 128), w = stick_nes_bits(128, v);
            int exp = v < 78 ? -1 : v > 178 ? 1 : 0;
            if (h != (exp < 0 ? NB_LEFT : exp > 0 ? NB_RIGHT : 0)) bad++;
            if (w != (exp < 0 ? NB_UP : exp > 0 ? NB_DOWN : 0)) bad++;
        }
        if (stick_nes_bits(0, 0) != (NB_LEFT | NB_UP) || stick_nes_bits(255, 255) != (NB_RIGHT | NB_DOWN) ||
            stick_nes_bits(255, 0) != (NB_RIGHT | NB_UP) || stick_nes_bits(0, 255) != (NB_LEFT | NB_DOWN) ||
            stick_nes_bits(128, 128) != 0 || stick_nes_bits(120, 140) != 0) bad++;
        printf("sticktest bad=%u (centro 128, zona muerta 78..178)\n", bad);
        return bad != 0;
    } else if (!strcmp(mode, "maptest")) {
        /* v1.8.2: pruebas del mapeo de botones, el turbo y el guardado de opciones (funciones puras) */
        unsigned bad = 0, n = 0, btns, d, p, i, f;
        const uint16_t dirbits[4] = { NB_UP, NB_DOWN, NB_LEFT, NB_RIGHT };
        /* 1) con el mapeo por defecto el resultado es IDENTICO a la formula de la v1.8.1 (todas las combinaciones de botones y direcciones) */
        opts_defaults();
        for (p = 0; p < 2; p++)
            for (btns = 0; btns < (1u << PB_COUNT); btns++)
                for (d = 0; d < 16; d++) {
                    uint16_t dirs = 0, ref = 0;
                    for (i = 0; i < 4; i++) if (d & (1u << i)) dirs |= dirbits[i];
                    if (btns & PB_BIT(PB_CIRCLE)) ref |= NB_A;
                    if (btns & PB_BIT(PB_CROSS))  ref |= NB_B;
                    if (btns & PB_BIT(PB_SELECT)) ref |= NB_SEL;
                    if (btns & PB_BIT(PB_START))  ref |= NB_START;
                    ref |= dirs;
                    if ((ref & NB_UP) && (ref & NB_DOWN))    ref &= (uint16_t)~NB_DOWN;
                    if ((ref & NB_LEFT) && (ref & NB_RIGHT)) ref &= (uint16_t)~NB_RIGHT;
                    n++;
                    if (map_nes_bits((int)p, btns, dirs) != ref) bad++;
                }
        printf("maptest defecto==v1.8.1: %u combinaciones, bad=%u\n", n, bad);
        /* 2) turbo: apretado 2 cuadros, suelto 2, empezando apretado; soltar reinicia; cada mando y cada turbo es independiente */
        {
            unsigned tb = 0;
            opts_defaults();
            g_map[0][MA_TA] = PB_SQUARE; g_map[0][MA_TB] = PB_TRIANGLE;
            for (f = 0; f < 16; f++) {
                uint16_t a = map_nes_bits(0, PB_BIT(PB_SQUARE), 0);
                int want = ((f >> 1) & 1) == 0;
                if (((a & NB_A) != 0) != want || (a & NB_B)) tb++;
            }
            (void)map_nes_bits(0, 0, 0);                                      /* soltar */
            for (f = 0; f < 6; f++) {                                         /* turbo B: tambien empieza apretado */
                uint16_t a = map_nes_bits(0, PB_BIT(PB_TRIANGLE), 0);
                int want = ((f >> 1) & 1) == 0;
                if (((a & NB_B) != 0) != want || (a & NB_A)) tb++;
            }
            (void)map_nes_bits(0, 0, 0);
            if (!(map_nes_bits(0, PB_BIT(PB_SQUARE), 0) & NB_A)) tb++;        /* primer cuadro tras apretar = apretado */
            (void)map_nes_bits(0, PB_BIT(PB_SQUARE), 0);
            if (map_nes_bits(0, PB_BIT(PB_SQUARE), 0) & NB_A) tb++;           /* tercer cuadro = suelto */
            for (f = 0; f < 8; f++) {                                         /* el mando 2 no tiene turbo asignado: nunca dispara */
                if (map_nes_bits(1, PB_BIT(PB_SQUARE) | PB_BIT(PB_TRIANGLE), 0) & (NB_A | NB_B)) tb++;
            }
            (void)map_nes_bits(0, 0, 0);
            for (f = 0; f < 8; f++) {                                         /* con A normal apretada, el turbo no la suelta */
                if (!(map_nes_bits(0, PB_BIT(PB_CIRCLE) | PB_BIT(PB_SQUARE), 0) & NB_A)) tb++;
            }
            printf("maptest turbo: bad=%u\n", tb);
            bad += tb;
        }
        /* 3) reglas de asignacion */
        {
            unsigned rb = 0, seen[PB_COUNT + 1], k;
            opts_defaults();
            map_cycle(0, MA_A, +1);                                           /* A: O -> CUADRADO (libre) */
            if (g_map[0][MA_A] != PB_SQUARE) rb++;
            opts_defaults();
            map_cycle(0, MA_A, -1);                                           /* A hacia atras: salta X (B), "sin asignar" (no vale), SELECT y START (usados) -> L3 (R3 esta reservado) */
            if (g_map[0][MA_A] != PB_L3 || g_map[0][MA_B] != PB_CROSS) rb++;
            opts_defaults();
            map_cycle(0, MA_TA, +1);                                          /* turbo A: sin asignar -> salta X y O (usados) -> CUADRADO */
            if (g_map[0][MA_TA] != PB_SQUARE) rb++;
            map_cycle(0, MA_TA, -1);                                          /* y hacia atras vuelve a sin asignar */
            if (g_map[0][MA_TA] != PB_NONE) rb++;
            map_assign(0, MA_TA, PB_NONE); map_assign(0, MA_TB, PB_NONE);     /* quitar un turbo ya quitado no toca nada mas */
            if (g_map[0][MA_TA] != PB_NONE || g_map[0][MA_TB] != PB_NONE || g_map[0][MA_A] != PB_CIRCLE) rb++;
            /* R3: el mando 1 nunca puede tenerlo; el mando 2 si. Se recorre todo el ciclo de un turbo */
            for (p = 0; p < 2; p++) {
                opts_defaults();
                memset(seen, 0, sizeof seen);
                for (k = 0; k < 40; k++) { map_cycle((int)p, MA_TA, +1); seen[g_map[p][MA_TA] + 1]++; }
                if (p == 0 && (seen[PB_R3 + 1] != 0 || seen[PB_L2 + 1] != 0)) rb++;   /* v1.9.6: R3 y L2 reservados en el mando 1 */
                if (p == 1 && (seen[PB_R3 + 1] == 0 || seen[PB_L2 + 1] == 0)) rb++;
                if (seen[0] == 0) rb++;                                       /* "sin asignar" tambien se alcanza */
                if (seen[PB_CROSS + 1] != 0 || seen[PB_CIRCLE + 1] != 0 || seen[PB_START + 1] != 0 || seen[PB_SELECT + 1] != 0) rb++;   /* nunca pisa botones usados */
            }
            /* mapeos guardados: validacion */
            {
                int ok[MA_COUNT]  = { PB_CIRCLE, PB_CROSS, PB_SELECT, PB_START, PB_NONE, PB_NONE };
                int dup[MA_COUNT] = { PB_CIRCLE, PB_CIRCLE, PB_SELECT, PB_START, PB_NONE, PB_NONE };
                int non[MA_COUNT] = { PB_NONE, PB_CROSS, PB_SELECT, PB_START, PB_NONE, PB_NONE };
                int r2[MA_COUNT]  = { PB_R3, PB_CROSS, PB_SELECT, PB_START, PB_NONE, PB_NONE };
                int big[MA_COUNT] = { PB_CIRCLE, PB_CROSS, PB_SELECT, PB_START, 99, PB_NONE };
                if (!map_valid(ok, 0) || !map_valid(ok, 1)) rb++;
                if (map_valid(dup, 0) || map_valid(non, 0) || map_valid(r2, 0) || map_valid(big, 1)) rb++;
                if (!map_valid(r2, 1)) rb++;                                  /* R3 vale en el mando 2 */
                {   /* v1.9.6: L2 reservado en el mando 1 (captura), libre en el mando 2 */
                    int l2m[MA_COUNT] = { PB_L2, PB_CROSS, PB_SELECT, PB_START, PB_NONE, PB_NONE };
                    if (map_valid(l2m, 0) || !map_valid(l2m, 1)) rb++;
                }
            }
            printf("maptest reglas: bad=%u\n", rb);
            bad += rb;
        }
        /* 4) fuzz: 300000 cambios al azar; el mapeo siempre queda valido (botones normales siempre con boton, sin repetidos, sin R2 en el mando 1) */
        {
            unsigned fb = 0;
            srand(12345);
            opts_defaults();
            for (f = 0; f < 300000; f++) {
                int pp = rand() & 1, act = rand() % MA_COUNT, dd = (rand() & 1) ? 1 : -1;
                if ((rand() % 50) == 0) map_defaults(pp);
                else if ((rand() % 7) == 0) { int b = (rand() % (PB_COUNT + 1)) - 1; if (map_allowed(pp, act, b)) map_assign(pp, act, b); }
                else map_cycle(pp, act, dd);
                if (!map_valid(g_map[0], 0) || !map_valid(g_map[1], 1)) fb++;
            }
            printf("maptest fuzz: bad=%u\n", fb);
            bad += fb;
        }
        /* 5) guardar y cargar */
        {
            unsigned sb = 0;
            char path[300], dir[] = "/tmp/nesnet_maptest";
            FILE *fp;
            int save_map[2][MA_COUNT], save_screen;
            mkdir(dir, 0755);
            snprintf(g_dir, sizeof g_dir, "%s", dir);
            g_log = NULL;
            opts_defaults();
            g_map[0][MA_A] = PB_SQUARE; g_map[0][MA_TA] = PB_L1; g_map[1][MA_TB] = PB_R2; g_screen = 2;
            opts_save();
            memcpy(save_map, g_map, sizeof save_map); save_screen = g_screen;
            opts_defaults();
            opts_load();
            if (memcmp(save_map, g_map, sizeof save_map) || g_screen != save_screen) sb++;
            /* archivo estropeado: lineas basura, mapeo invalido del mando 1, pantalla fuera de rango: valen los defectos / lo valido */
            snprintf(path, sizeof path, "%s/opciones.txt", dir);
            fp = fopen(path, "w");
            fprintf(fp, "basura\nscreen=9\npad1=3,3,3,3,3,3\npad2=1,0,11,10,-1,2\nxx=1\n");
            fclose(fp);
            opts_load();
            if (g_screen != 0) sb++;                                          /* 9 no vale: queda la de por defecto */
            if (g_map[0][MA_A] != PB_CIRCLE || g_map[0][MA_B] != PB_CROSS || g_map[0][MA_TA] != PB_NONE) sb++;   /* mando 1: invalido -> defecto */
            if (g_map[1][MA_TB] != PB_SQUARE || g_map[1][MA_SEL] != PB_SELECT || g_map[1][MA_TA] != PB_NONE) sb++;   /* mando 2: valido, se respeta */
            printf("maptest guardado: bad=%u\n", sb);
            bad += sb;
        }
        printf("maptest TOTAL bad=%u\n", bad);
        return bad != 0;
    } else if (!strcmp(mode, "barstest")) {
        /* v1.9.9: barras con mosaico y fondo del menu R3 segun la pantalla (argv[4] = carpeta de salida) */
        static uint32_t fbuf[SCR_W * SCR_H], buf[HR_W * HR_H];
        const char *od = argc > 4 ? argv[4] : "/tmp";
        int md, x, y, bad = 0;
        for (y = 0; y < SCR_H; y++) for (x = 0; x < SCR_W; x++) fbuf[y * SCR_W + x] = 0xFF000000u | ((uint32_t)(x * 255 / 255) << 16) | ((uint32_t)(y * 255 / 239) << 8) | 0x60u;
        for (x = 0; x < SCR_W; x++) { fbuf[x] = 0xFFFFFFFFu; fbuf[(SCR_H - 1) * SCR_W + x] = 0xFFFFFFFFu; }
        for (y = 0; y < SCR_H; y++) { fbuf[y * SCR_W] = 0xFFFFFFFFu; fbuf[y * SCR_W + SCR_W - 1] = 0xFFFFFFFFu; }
        hr_init();
        for (md = 0; md < 3; md++) {
            unsigned fc = 0;
            char p[400];
            FILE *f;
            int i, ox, ow = screen_out_w(md, HR_W, SCR_H * 3, 3);
            ox = (HR_W - ow) / 2;
            for (i = 0; i < HR_W * HR_H; i++) buf[i] = 0xFF804020u;       /* "residuo" que debe desaparecer */
            g_bars_mosaic = 1;
            canvas_blit_cpu(buf, HR_W, HR_H, md, 3, fbuf, &fc);
            g_bars_mosaic = 0;
            for (y = 0; y < HR_H; y++) for (x = 0; x < ox; x++)
                if (buf[y * HR_W + x] != g_bg[y * HR_W + x] || buf[y * HR_W + HR_W - 1 - x] != g_bg[y * HR_W + HR_W - 1 - x]) bad++;
            snprintf(p, sizeof p, "%s/game_%d.ppm", od, md);
            f = fopen(p, "wb");
            if (f) { fprintf(f, "P6\n%d %d\n255\n", HR_W, HR_H); for (i = 0; i < HR_W * HR_H; i++) { uint8_t px[3] = { (uint8_t)(buf[i] >> 16), (uint8_t)(buf[i] >> 8), (uint8_t)buf[i] }; fwrite(px, 1, 3, f); } fclose(f); }
            g_screen = md; g_bd_dirty = 1;
            hr_backdrop(fbuf);
            snprintf(p, sizeof p, "%s/menu_%d.ppm", od, md);
            f = fopen(p, "wb");
            if (f) { fprintf(f, "P6\n%d %d\n255\n", HR_W, HR_H); for (i = 0; i < HR_W * HR_H; i++) { uint8_t px[3] = { (uint8_t)(g_hr[i] >> 16), (uint8_t)(g_hr[i] >> 8), (uint8_t)g_hr[i] }; fwrite(px, 1, 3, f); } fclose(f); }
        }
        printf("barstest bad=%d\n", bad);
        return bad != 0;
    } else if (!strcmp(mode, "imgtest")) {
        /* v1.9.2: ajuste de imagen (canvas_blit_adj) y guardado de las opciones nuevas */
        static const int dims[4][3] = { { 1280, 720, 3 }, { 1920, 1080, 3 }, { 1920, 1080, 4 }, { 720, 480, 2 } };
        static const int sizes[4] = { 100, 90, 80, 70 };
        static const int shifts[7][2] = { { 0, 0 }, { 2, 0 }, { 0, -2 }, { 60, 60 }, { -60, -60 }, { 60, -60 }, { -60, 60 } };
        static uint32_t fbuf[SCR_W * SCR_H], b0[1920 * 1080], b1[1920 * 1080], ref[1920 * 1080];
        unsigned bad = 0, ncheck = 0;
        int di, md, si, hi, x, y;
        for (y = 0; y < SCR_H; y++) for (x = 0; x < SCR_W; x++) fbuf[y * SCR_W + x] = 0xFF000000u | ((uint32_t)y << 8) | (uint32_t)x;
        for (di = 0; di < 4; di++) {
            int dw = dims[di][0], dh = dims[di][1], z = dims[di][2];
            for (md = 0; md < 3; md++) {
                /* 1) con 100 %, 0, 0 es IDENTICO a canvas_blit_cpu (lo que se usaba hasta la v1.9.1) */
                {
                    unsigned fa = 0, fb = 0;
                    size_t i;
                    for (i = 0; i < (size_t)dw * dh; i++) { b0[i] = 0xDEADBEEFu; b1[i] = 0xDEADBEEFu; }
                    canvas_blit_cpu(b0, dw, dh, md, z, fbuf, &fa);
                    canvas_blit_adj(b1, dw, dh, md, z, fbuf, &fb, 100, 0, 0);
                    ncheck++;
                    if (memcmp(b0, b1, (size_t)dw * dh * 4)) { bad++; printf("imgtest %dx%d modo=%d: 100%%,0,0 DIFIERE de canvas_blit_cpu\n", dw, dh, md); }
                }
                /* 2) cada tamano y desplazamiento contra el calculo pixel a pixel (con recorte), en dos buffers alternados
                 *    como en la PS3: tras cambiar la geometria, a las 2 llamadas (una por buffer) no queda ningun resto */
                for (si = 0; si < 4; si++)
                    for (hi = 0; hi < 7; hi++) {
                        int oh0 = SCR_H * z, ow0 = screen_out_w(md, dw, oh0, z);
                        int ow = ow0 * sizes[si] / 100, oh = oh0 * sizes[si] / 100;
                        int ox = (dw - ow) / 2 + shifts[hi][0], oy = (dh - oh) / 2 + shifts[hi][1];
                        unsigned fa = 0, fb = 0;
                        int k, errs = 0, dx, dy;
                        size_t i;
                        /* historia previa con OTRA geometria, para que haya restos que borrar */
                        for (i = 0; i < (size_t)dw * dh; i++) { b0[i] = 0xDEADBEEFu; b1[i] = 0xDEADBEEFu; }
                        canvas_blit_adj(b0, dw, dh, md, z, fbuf, &fa, 100, 0, 0);
                        canvas_blit_adj(b1, dw, dh, md, z, fbuf, &fb, 100, 0, 0);
                        for (k = 0; k < 4; k++) { canvas_blit_adj(b0, dw, dh, md, z, fbuf, &fa, 100, 0, 0); canvas_blit_adj(b1, dw, dh, md, z, fbuf, &fb, 100, 0, 0); }
                        for (k = 0; k < 2; k++) {          /* geometria nueva: una llamada por buffer */
                            canvas_blit_adj(b0, dw, dh, md, z, fbuf, &fa, sizes[si], shifts[hi][0], shifts[hi][1]);
                            canvas_blit_adj(b1, dw, dh, md, z, fbuf, &fb, sizes[si], shifts[hi][0], shifts[hi][1]);
                        }
                        for (dy = 0; dy < dh; dy++)
                            for (dx = 0; dx < dw; dx++) {
                                uint32_t want = 0;
                                if (dy >= oy && dy < oy + oh && dx >= ox && dx < ox + ow)
                                    want = fbuf[((dy - oy) * SCR_H / oh) * SCR_W + ((dx - ox) * SCR_W / ow)];
                                ref[(size_t)dy * dw + dx] = want;
                            }
                        for (i = 0; i < (size_t)dw * dh; i++) if (b0[i] != ref[i]) errs++;
                        for (i = 0; i < (size_t)dw * dh; i++) if (b1[i] != ref[i]) errs++;
                        ncheck++;
                        if (errs) { bad++; printf("imgtest %dx%d modo=%d tam=%d%% dx=%d dy=%d: %d pixeles mal\n", dw, dh, md, sizes[si], shifts[hi][0], shifts[hi][1], errs); }
                    }
            }
        }
        printf("imgtest imagen: %u comprobaciones, mal=%u\n", ncheck, bad);
        /* 3) las opciones nuevas se guardan y se leen (y valores fuera de rango o basura se ignoran) */
        {
            unsigned ob = 0;
            FILE *f;
            char p[300];
            snprintf(g_dir, sizeof g_dir, "/tmp/nn_img_test");
            { char cmd[320]; snprintf(cmd, sizeof cmd, "mkdir -p %s", g_dir); if (system(cmd)) ob++; }
            opts_defaults();
            if (g_showfps != 0 || g_img_size != 100 || g_img_dx != 0 || g_img_dy != 0) ob++;              /* valores de siempre por defecto */
            g_showfps = 1; g_img_size = 85; g_img_dx = -14; g_img_dy = 30;
            opts_save();
            opts_defaults();
            opts_load();
            if (g_showfps != 1 || g_img_size != 85 || g_img_dx != -14 || g_img_dy != 30) { ob++; printf("imgtest opciones: NO se leyeron bien (fps=%d size=%d dx=%d dy=%d)\n", g_showfps, g_img_size, g_img_dx, g_img_dy); }
            snprintf(p, sizeof p, "%s/opciones.txt", g_dir);
            f = fopen(p, "wb");
            if (f) { fprintf(f, "fps=7\nimgsize=5\nimgx=9999\nimgy=-9999\n"); fclose(f); }
            opts_defaults();
            opts_load();
            if (g_showfps != 0 || g_img_size != 100 || g_img_dx != 0 || g_img_dy != 0) { ob++; printf("imgtest opciones: valores malos NO se ignoraron\n"); }
            remove(p);
            opts_defaults();
            opts_load();                                                      /* sin archivo: valores de siempre */
            if (g_showfps != 0 || g_img_size != 100 || g_img_dx != 0 || g_img_dy != 0) ob++;
            printf("imgtest opciones: mal=%u\n", ob);
            bad += ob;
        }
        printf("imgtest TOTAL mal=%u\n", bad);
        return bad != 0;
    } else if (!strcmp(mode, "shottest")) {
        /* v1.9.6: el PNG de captura == lo que dibuja canvas_blit_cpu (la funcion de la pantalla de la PS3) recortado a la imagen.
         * argv[4] = carpeta de salida: para cada medida y modo deja ref_<d>_<m>.rgb (lo esperado) y png_<d>_<m>.png (lo guardado). */
        static const int dims[3][3] = { { 1280, 720, 3 }, { 1920, 1080, 3 }, { 720, 480, 2 } };
        static uint32_t fbuf[SCR_W * SCR_H], buf[1920 * 1080];
        const char *od = argc > 4 ? argv[4] : "/tmp";
        unsigned bad = 0;
        int di, md, x, y;
        for (y = 0; y < SCR_H; y++)
            for (x = 0; x < SCR_W; x++) {
                uint32_t v = (uint32_t)(x * 7919 + y * 104729 + (x ^ y) * 31);
                fbuf[y * SCR_W + x] = 0xFF000000u | ((v * 2654435761u) >> 8);
            }
        for (di = 0; di < 3; di++)
            for (md = 0; md < 3; md++) {
                int dw = dims[di][0], dh = dims[di][1], z = dims[di][2];
                int oh = SCR_H * z, ow = screen_out_w(md, dw, oh, z), ox = (dw - ow) / 2, oy = (dh - oh) / 2, w = 0, h = 0;
                unsigned fc = 0;
                char p1[400];
                FILE *f;
                size_t i;
                for (i = 0; i < (size_t)dw * dh; i++) buf[i] = 0xDEADBEEFu;
                canvas_blit_cpu(buf, dw, dh, md, z, fbuf, &fc);
                snprintf(p1, sizeof p1, "%s/ref_%d_%d.rgb", od, di, md);
                f = fopen(p1, "wb");
                for (y = 0; y < oh; y++)
                    for (x = 0; x < ow; x++) {
                        uint32_t px = buf[(size_t)(oy + y) * dw + ox + x];
                        uint8_t c3[3] = { (uint8_t)(px >> 16), (uint8_t)(px >> 8), (uint8_t)px };
                        fwrite(c3, 1, 3, f);
                    }
                fclose(f);
                snprintf(p1, sizeof p1, "%s/png_%d_%d.png", od, di, md);
                if (!shot_write_png(p1, fbuf, md, dw, z, &w, &h) || w != ow || h != oh) { bad++; printf("shottest: FALLO %dx%d modo %d\n", dw, dh, md); }
                else printf("shottest %dx%d modo %d: png %dx%d\n", dw, dh, md, w, h);
            }
        /* nombres: ROM con espacios, parentesis y corchetes; numeracion 0001, 0002...; otra ROM empieza de nuevo */
        {
            char nd[300], p2[420];
            int n1, n2, n3;
            snprintf(nd, sizeof nd, "%s/naming", od);
            mkdir(nd, 0777);
            snprintf(g_dir, sizeof g_dir, "%s", nd);
            g_cfg.render = 1; g_screen = 2;
            snprintf(g_rom_name, sizeof g_rom_name, "Super Mario Bros. 3 (USA) [!].nes");
            n1 = shot_take(fbuf); n2 = shot_take(fbuf);
            snprintf(g_rom_name, sizeof g_rom_name, "Otro Juego.nes");
            n3 = shot_take(fbuf);
            snprintf(g_rom_name, sizeof g_rom_name, "Super Mario Bros. 3 (USA) [!].nes");
            if (shot_take(fbuf) != 3) bad++;                                       /* vuelve a la primera ROM: sigue en 0003 */
            snprintf(p2, sizeof p2, "%s/screenshots/Super_Mario_Bros__3__USA______0003.png", nd);
            if (n1 != 1 || n2 != 2 || n3 != 1 || !kt_exists(p2)) { bad++; printf("shottest: numeracion/nombres MAL (%d %d %d) %s\n", n1, n2, n3, p2); }
            else printf("shottest nombres OK: %s\n", p2);
            g_rom_name[0] = 0;
            if (shot_take(fbuf) != 1) bad++;
            snprintf(p2, sizeof p2, "%s/screenshots/captura_0001.png", nd);
            if (!kt_exists(p2)) { bad++; printf("shottest: sin nombre de ROM deberia ser captura_0001.png\n"); }
        }
        printf("shottest TOTAL bad=%u\n", bad);
        return bad != 0;
    } else if (!strcmp(mode, "screentest")) {
        /* v1.8.2: pruebas del escalado de pantalla (canvas_blit_cpu, la misma funcion que usa la PS3) */
        static const int dims[4][3] = { { 1280, 720, 3 }, { 1920, 1080, 3 }, { 1920, 1080, 4 }, { 720, 480, 2 } };
        unsigned bad = 0;
        int di, md;
        static uint32_t fbuf[SCR_W * SCR_H], buf[1920 * 1080], ref[1920 * 1080];
        int x, y;
        for (y = 0; y < SCR_H; y++) for (x = 0; x < SCR_W; x++) fbuf[y * SCR_W + x] = 0xFF000000u | ((uint32_t)y << 8) | (uint32_t)x;
        for (di = 0; di < 4; di++) {
            int dw = dims[di][0], dh = dims[di][1], z = dims[di][2];
            for (md = 0; md < 3; md++) {
                int oh = SCR_H * z, ow = screen_out_w(md, dw, oh, z);
                int ox = (dw - ow) / 2, oy = (dh - oh) / 2, errs = 0, dx, dy;
                unsigned fc = 0;
                size_t i;
                for (i = 0; i < (size_t)dw * dh; i++) buf[i] = 0xDEADBEEFu;     /* basura previa: lo de fuera de la imagen debe quedar en 0 */
                canvas_blit_cpu(buf, dw, dh, md, z, fbuf, &fc);
                for (dy = 0; dy < dh; dy++)
                    for (dx = 0; dx < dw; dx++) {
                        uint32_t got = buf[(size_t)dy * dw + dx], want = 0;
                        if (dy >= oy && dy < oy + oh && dx >= ox && dx < ox + ow)
                            want = fbuf[((dy - oy) / z) * SCR_W + ((dx - ox) * SCR_W / ow)];
                        if (got != want) errs++;
                    }
                /* modo 0: identico, byte a byte, al escalado de la v1.8.1 (cada pixel repetido z veces) */
                if (md == 0) {
                    for (i = 0; i < (size_t)dw * dh; i++) ref[i] = 0;
                    for (dy = 0; dy < oh; dy++)
                        for (dx = 0; dx < SCR_W * z; dx++)
                            ref[(size_t)(oy + dy) * dw + ox + dx] = fbuf[(dy / z) * SCR_W + dx / z];
                    if (memcmp(buf, ref, (size_t)dw * dh * 4)) errs++;
                }
                /* repeticion de columnas en la primera fila: original a 720p = todas x3; 4:3 = 192 de x4 y 64 de x3; 16:9 = todas x5 */
                if (dw == 1280 && dh == 720) {
                    int run = 1, n3 = 0, n4 = 0, n5 = 0, nx = 0, row = oy * dw + ox;
                    for (dx = 1; dx <= ow; dx++) {
                        if (dx < ow && buf[row + dx] == buf[row + dx - 1]) { run++; continue; }
                        if (run == 3) n3++; else if (run == 4) n4++; else if (run == 5) n5++; else nx++;
                        run = 1;
                    }
                    if (md == 0 && !(ow == 768 && n3 == 256 && !n4 && !n5 && !nx)) errs++;
                    if (md == 1 && !(ow == 960 && n4 == 192 && n3 == 64 && !n5 && !nx)) errs++;
                    if (md == 2 && !(ow == 1280 && n5 == 256 && !n3 && !n4 && !nx)) errs++;
                }
                printf("screentest %dx%d z=%d modo=%d: imagen %dx%d en (%d,%d) errores=%d\n", dw, dh, z, md, ow, oh, ox, oy, errs);
                bad += (unsigned)errs;
            }
        }
        /* cambiar de modo no deja residuo: 16:9 durante 8 cuadros y luego original; a los 4 cuadros los bordes quedan limpios */
        {
            int dw = 1280, dh = 720, z = 3, k, dx, dy, errs = 0;
            unsigned fc = 0;
            size_t i;
            for (i = 0; i < (size_t)dw * dh; i++) buf[i] = 0;
            for (k = 0; k < 8; k++) canvas_blit_cpu(buf, dw, dh, 2, z, fbuf, &fc);
            for (k = 0; k < 4; k++) canvas_blit_cpu(buf, dw, dh, 0, z, fbuf, &fc);
            for (dy = 0; dy < dh; dy++)
                for (dx = 0; dx < dw; dx++)
                    if ((dx < 256 || dx >= 1024) && buf[(size_t)dy * dw + dx] != 0) errs++;
            printf("screentest cambio 16:9 -> original: residuos=%d\n", errs);
            bad += (unsigned)errs;
        }
        /* costo relativo en CPU (solo orientativo, en PC) */
        {
            int dw = 1280, dh = 720, z = 3, k;
            unsigned fc = 0;
            uint32_t t0;
            for (md = 0; md < 3; md++) {
                t0 = now_ms();
                for (k = 0; k < 600; k++) canvas_blit_cpu(buf, dw, dh, md, z, fbuf, &fc);
                printf("screentest tiempo modo %d: %u ms por 600 cuadros (PC)\n", md, (unsigned)(now_ms() - t0));
            }
        }
        printf("screentest TOTAL bad=%u\n", bad);
        return bad != 0;
    } else if (!strcmp(mode, "ref4")) {
        /* v1.8: referencia de 4 jugadores locales con el mismo retraso D */
        D = argc > 4 ? atoi(argv[4]) : 6;
        g_four = 1;
        core_load4(); session_loop4(-2, D, frames); core_unload();
        g_four = 0;
    } else if (!strcmp(mode, "host4")) {
        int lfd = tcp_listen(port);
        if (lfd < 0) { fprintf(stderr, "no puedo escuchar\n"); return 2; }
        if (argc > 5) g_cfg.delay = atoi(argv[5]);
        g_net = accept(lfd, NULL, NULL);
        closesocket(lfd);
        sock_nodelay(g_net);
        ok = handshake_host4(&D);
        if (ok) ok = run_online4(1, D);
        if (!ok) fprintf(stderr, "ERROR: %s\n", g_err);
    } else if (!strcmp(mode, "client4")) {
        int tries;
        for (tries = 0; tries < 50 && g_net < 0; tries++) {
            g_net = tcp_connect(0x7F000001u, port, 2000);
            if (g_net < 0) sleep_ms(100);
        }
        if (g_net < 0) { fprintf(stderr, "no pude conectar\n"); return 2; }
        ok = handshake_client4(&D);
        if (ok) ok = run_online4(0, D);
        if (!ok) fprintf(stderr, "ERROR: %s\n", g_err);
    } else if (!strcmp(mode, "reload")) {
        /* sesion solo, descarga, y luego sesion de referencia: debe dar igual que un proceso nuevo */
        core_load(); session_loop(-1, 0, 300); core_unload();
        D = 6;
        core_load(); session_loop(-2, D, frames); core_unload();
    } else if (!strcmp(mode, "codetest")) {
        /* v1.9.1: codigo de 10 digitos (solo la IP); el puerto sale de config.txt */
        char c[20], pc[24]; uint32_t ip; int p, n = 0, bad = 0;
        uint32_t t;
        g_cfg.port = 7777;
        for (t = 0; t < 200000; t++) {
            uint32_t a = t * 2654435761u;
            code_encode(a ? a : 1, 7777, c);
            if (strlen(c) != 10 || !code_decode(c, &ip, &p) || ip != (a ? a : 1) || p != 7777) bad++;
            code_pretty(c, pc);
            if (strlen(pc) != 12 || !code_decode(pc, &ip, &p) || ip != (a ? a : 1)) bad++;      /* con espacios tambien */
            n++;
        }
        code_encode(0xB92DC811u, 7777, c);                                  /* 185.45.200.17 */
        if (strcmp(c, "3106785297")) { bad++; printf("codetest muestra: %s\n", c); }
        code_encode(0x01020304u, 7777, c);
        if (strcmp(c, "0016909060")) { bad++; printf("codetest muestra2: %s\n", c); }
        if (code_decode("12345", &ip, &p) || code_decode("12345678901", &ip, &p) || code_decode("4294967296", &ip, &p) ||
            code_decode("0000000000", &ip, &p) || code_decode("123456789012345", &ip, &p)) { bad++; printf("codetest: acepto un codigo malo\n"); }
        printf("codetest n=%d bad=%d\n", n, bad);
        return bad != 0;
    } else if (!strcmp(mode, "statetest")) {
        /* v1.9.1: guardar y cargar estado. Tras cargar, los siguientes cuadros deben dar exactamente los mismos CRC de video. */
        static uint32_t ca[300], cb[300], cc[300];
        uint32_t f;
        size_t n;
        uint8_t *buf;
        unsigned bad = 0;
        core_load();
        n = retro_serialize_size();
        printf("statetest: el nucleo pide %u bytes de estado\n", (unsigned)n);
        for (f = 0; f < 300; f++) step_frame(script_input(0, f), script_input(1, f));
        buf = (uint8_t *)malloc(n);
        if (!buf || !retro_serialize(buf, n)) { printf("statetest: no pude serializar\n"); return 1; }
        for (f = 300; f < 600; f++) { step_frame(script_input(0, f), script_input(1, f)); ca[f - 300] = g_frame_crc; }
        if (!retro_unserialize(buf, n)) { printf("statetest: no pude cargar\n"); return 1; }
        for (f = 300; f < 600; f++) { step_frame(script_input(0, f), script_input(1, f)); cb[f - 300] = g_frame_crc; }
        for (f = 0; f < 300; f++) if (ca[f] != cb[f]) bad++;
        printf("statetest memoria: cuadros distintos tras cargar = %u\n", bad);
        /* por archivo: guardar en la ranura 2, seguir, cargar, repetir */
        retro_unserialize(buf, n);
        free(buf);
        mkdir("/tmp/nn_state_test", 0777);
        snprintf(g_dir, sizeof g_dir, "/tmp/nn_state_test");
        if (!state_save(1)) { printf("statetest: no pude guardar archivo\n"); return 1; }
        for (f = 300; f < 600; f++) { step_frame(script_input(0, f), script_input(1, f)); ca[f - 300] = g_frame_crc; }
        if (state_load(1) != 1) { printf("statetest: no pude cargar archivo\n"); return 1; }
        for (f = 300; f < 600; f++) { step_frame(script_input(0, f), script_input(1, f)); cc[f - 300] = g_frame_crc; }
        {
            unsigned bad2 = 0;
            for (f = 0; f < 300; f++) if (ca[f] != cc[f]) bad2++;
            printf("statetest archivo: cuadros distintos tras cargar = %u\n", bad2);
            bad += bad2;
        }
        printf("statetest existe(ranura 2)=%d existe(ranura 1)=%d\n", state_exists(1), state_exists(0));
        core_unload();
        return bad != 0;
    }
    printf("RESULT mode=%s frames=%u hash=%08X desyncs=%d D=%d first_desync=%d\n", mode,
           (unsigned)g_frames_done, (unsigned)g_run_hash, g_desyncs, D,
           g_first_desync == NO_FRAME ? -1 : (int)g_first_desync);
    return ok ? 0 : 1;
}
#endif
