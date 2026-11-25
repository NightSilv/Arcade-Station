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
/* ---- Arcade Station v0.3: fuente neon (Archivo Black inclinada, licencia OFL) con brillo horneado en el alfa: 1..10 = halo, 12..15 = tubo; 5 tamanos. Antes: DejaVu Sans Bold (licencia libre Bitstream Vera / DejaVu), 5 tamanos, alfa de 4 bits, generada fuera de la PS3; Katari: el tamano grande suma A, I, K y R para el logo ---- */
typedef struct { uint16_t cp; uint8_t adv; int8_t bx, by; uint8_t w, h; uint32_t off; } FGlyph;
enum { FS_S, FS_M, FS_L, FS_H, FS_X, FS_T, FS_N };        /* 22, 28, 34, 46, 88 y 84 px (alto); FS_T = solo los titulos grandes (letras de "Arcade Station" y "Elige tu juego") */
static const int fg_asc[FS_N] = { 21, 26, 32, 43, 82, 78 };
static const int fg_start[FS_N + 1] = { 0, 113, 226, 339, 399, 419, 436 };
#define FG_NDATA 250592
static const FGlyph fg_tab[436] = {
 {32,7,0,0,0,0,0u},{33,7,-2,-18,15,21,0u},{34,11,-1,-18,17,12,158u},{35,15,-3,-18,23,21,260u},{36,15,-2,-20,21,24,502u},
 {37,22,-1,-18,26,21,754u},{38,20,-2,-18,27,21,1027u},{39,6,-1,-18,12,12,1311u},{40,9,-2,-19,16,26,1383u},{41,9,-3,-19,16,26,1591u},
 {42,12,0,-18,16,14,1799u},{43,15,-2,-15,20,18,1911u},{44,7,-3,-7,13,14,2091u},{45,7,-2,-10,14,9,2182u},{46,7,-2,-7,12,10,2245u},
 {47,6,-4,-19,16,24,2305u},{48,15,-2,-18,22,21,2497u},{49,15,-1,-18,19,21,2728u},{50,15,-2,-18,22,21,2928u},{51,15,-2,-18,22,21,3159u},
 {52,15,-3,-18,22,21,3390u},{53,15,-2,-18,21,21,3621u},{54,15,-2,-18,22,21,3842u},{55,15,0,-18,20,21,4073u},{56,15,-2,-18,22,21,4283u},
 {57,15,-2,-18,22,21,4514u},{58,7,-2,-15,14,18,4745u},{59,7,-3,-15,15,22,4871u},{60,15,-2,-15,21,19,5036u},{61,15,-2,-13,20,14,5236u},
 {62,15,-3,-15,21,19,5376u},{63,13,-2,-18,21,21,5576u},{64,16,-3,-18,24,24,5797u},{65,17,-3,-18,24,21,6085u},{66,17,-2,-18,24,21,6337u},
 {67,17,-2,-18,24,21,6589u},{68,17,-2,-18,24,21,6841u},{69,16,-2,-18,23,21,7093u},{70,15,-2,-18,22,21,7335u},{71,18,-2,-18,25,21,7566u},
 {72,18,-2,-18,25,21,7829u},{73,9,-2,-18,15,21,8092u},{74,15,-3,-18,22,21,8250u},{75,18,-2,-18,26,21,8481u},{76,15,-2,-18,21,21,8754u},
 {77,21,-2,-18,28,21,8975u},{78,18,-2,-18,25,21,9269u},{79,18,-2,-18,25,21,9532u},{80,16,-2,-18,23,21,9795u},{81,18,-2,-18,25,24,10037u},
 {82,17,-2,-18,24,21,10337u},{83,16,-2,-18,23,21,10589u},{84,16,-1,-18,23,21,10831u},{85,18,-2,-18,25,21,11073u},{86,17,-1,-18,24,21,11336u},
 {87,22,-1,-18,29,21,11588u},{88,17,-3,-18,26,21,11893u},{89,17,-1,-18,24,21,12166u},{90,16,-3,-18,24,21,12418u},{91,9,-3,-19,17,27,12670u},
 {92,6,-1,-19,11,24,12900u},{93,9,-3,-19,17,27,13032u},{94,15,-1,-18,19,14,13262u},{95,11,-4,-1,18,7,13395u},{96,7,0,-19,13,9,13458u},
 {97,15,-3,-15,22,18,13517u},{98,15,-2,-19,21,22,13715u},{99,15,-2,-15,21,18,13946u},{100,15,-2,-19,22,22,14135u},{101,15,-2,-15,21,18,14377u},
 {102,9,-2,-19,17,22,14566u},{103,15,-4,-18,25,25,14753u},{104,15,-2,-19,21,22,15066u},{105,7,-2,-19,14,22,15297u},{106,7,-5,-19,17,26,15451u},
 {107,15,-2,-19,23,22,15672u},{108,7,-2,-19,14,22,15925u},{109,22,-2,-15,28,18,16079u},{110,15,-2,-15,21,18,16331u},{111,15,-2,-15,21,18,16520u},
 {112,15,-3,-15,22,22,16709u},{113,15,-2,-15,22,22,16951u},{114,10,-2,-15,18,18,17193u},{115,13,-3,-15,21,18,17355u},{116,10,-2,-18,17,21,17544u},
 {117,15,-2,-15,22,18,17723u},{118,13,-2,-15,22,18,17921u},{119,21,-2,-15,29,18,18119u},{120,15,-3,-15,23,18,18380u},{121,13,-3,-15,23,22,18587u},
 {122,12,-3,-15,21,18,18840u},{123,9,-2,-19,16,26,19029u},{124,6,-3,-19,14,26,19237u},{125,9,-3,-19,16,26,19419u},{126,15,-3,-12,22,12,19627u},
 {161,7,-3,-15,15,21,19759u},{176,9,-1,-19,15,13,19917u},{183,7,-2,-12,13,10,20015u},{191,13,-3,-15,20,21,20080u},{193,17,-3,-23,24,26,20290u},
 {201,16,-2,-23,23,26,20602u},{205,9,-2,-23,17,26,20901u},{209,18,-2,-23,25,26,21122u},{211,18,-2,-23,25,26,21447u},{218,18,-2,-23,25,26,21772u},
 {220,18,-2,-23,25,26,22097u},{225,15,-3,-19,22,22,22422u},{233,15,-2,-19,21,22,22664u},{237,7,-2,-19,16,22,22895u},{241,15,-2,-19,21,22,23071u},
 {243,15,-2,-19,21,22,23302u},{250,15,-2,-19,22,22,23533u},{252,15,-2,-19,22,22,23775u},{32,9,0,0,0,0,24017u},{33,9,-2,-22,17,25,24017u},
 {34,14,0,-22,19,14,24230u},{35,18,-3,-22,27,25,24363u},{36,18,-1,-24,24,29,24701u},{37,27,0,-22,31,25,25049u},{38,24,-1,-22,31,25,25437u},
 {39,8,0,-22,13,14,25825u},{40,10,-2,-23,19,31,25916u},{41,10,-3,-23,18,31,26211u},{42,15,1,-22,18,16,26490u},{43,18,-1,-18,22,21,26634u},
 {44,9,-3,-9,15,17,26865u},{45,9,-2,-12,15,10,26993u},{46,9,-2,-9,14,12,27068u},{47,8,-4,-23,19,28,27152u},{48,18,-2,-22,25,25,27418u},
 {49,18,-1,-22,22,25,27731u},{50,18,-2,-22,25,25,28006u},{51,18,-2,-22,25,25,28319u},{52,18,-3,-22,26,25,28632u},{53,18,-2,-22,25,25,28957u},
 {54,18,-2,-22,25,25,29270u},{55,18,0,-22,24,25,29583u},{56,18,-2,-22,24,25,29883u},{57,18,-2,-22,25,25,30183u},{58,9,-2,-18,16,21,30496u},
 {59,9,-3,-18,17,26,30664u},{60,18,-1,-18,23,22,30885u},{61,18,-2,-15,24,16,31138u},{62,18,-2,-18,23,22,31330u},{63,16,0,-22,22,25,31583u},
 {64,20,-3,-22,28,30,31858u},{65,21,-3,-22,28,25,32278u},{66,21,-1,-22,27,25,32628u},{67,21,-1,-22,27,25,32966u},{68,21,-1,-22,27,25,33304u},
 {69,20,-1,-22,26,25,33642u},{70,18,-1,-22,25,25,33967u},{71,22,-1,-22,28,25,34280u},{72,22,-1,-22,29,25,34630u},{73,10,-1,-22,17,25,34993u},
 {74,18,-2,-22,25,25,35206u},{75,22,-1,-22,30,25,35519u},{76,18,-1,-22,23,25,35894u},{77,25,-2,-22,33,25,36182u},{78,22,-1,-22,29,25,36595u},
 {79,22,-1,-22,29,25,36958u},{80,20,-1,-22,26,25,37321u},{81,22,-1,-22,28,29,37646u},{82,21,-1,-22,27,25,38052u},{83,20,-2,-22,26,25,38390u},
 {84,20,-1,-22,27,25,38715u},{85,22,-1,-22,29,25,39053u},{86,21,0,-22,28,25,39416u},{87,27,-1,-22,35,25,39766u},{88,21,-3,-22,30,25,40204u},
 {89,21,0,-22,28,25,40579u},{90,20,-3,-22,29,25,40929u},{91,10,-3,-23,20,31,41292u},{92,8,-1,-23,13,28,41602u},{93,10,-3,-23,20,31,41784u},
 {94,18,-1,-22,22,16,42094u},{95,14,-4,-2,21,8,42270u},{96,9,1,-23,14,10,42354u},{97,18,-2,-18,24,21,42424u},{98,18,-2,-23,25,26,42676u},
 {99,18,-2,-18,25,21,43001u},{100,18,-2,-23,26,26,43264u},{101,18,-2,-18,24,21,43602u},{102,10,-1,-23,19,26,43854u},{103,18,-4,-21,29,29,44101u},
 {104,18,-2,-23,25,26,44522u},{105,9,-2,-23,17,26,44847u},{106,9,-5,-23,20,31,45068u},{107,18,-2,-23,26,26,45378u},{108,9,-2,-23,17,26,45716u},
 {109,27,-2,-18,34,21,45937u},{110,18,-2,-18,25,21,46294u},{111,18,-2,-18,25,21,46557u},{112,18,-3,-18,26,26,46820u},{113,18,-2,-18,25,26,47158u},
 {114,12,-2,-18,20,21,47483u},{115,16,-2,-18,23,21,47693u},{116,12,-1,-22,19,25,47935u},{117,18,-2,-18,25,21,48173u},{118,16,-1,-18,24,21,48436u},
 {119,25,-1,-18,33,21,48688u},{120,18,-3,-18,27,21,49035u},{121,16,-3,-18,26,26,49319u},{122,15,-3,-18,24,21,49657u},{123,10,-2,-23,19,31,49909u},
 {124,8,-2,-23,15,31,50204u},{125,10,-3,-23,18,31,50437u},{126,18,-1,-16,24,14,50716u},{161,9,-3,-18,17,25,50884u},{176,11,0,-22,16,14,51097u},
 {183,9,0,-15,13,11,51209u},{191,16,-3,-18,23,25,51281u},{193,21,-3,-28,28,31,51569u},{201,20,-1,-28,26,31,52003u},{205,10,-1,-28,19,31,52406u},
 {209,22,-1,-28,29,31,52701u},{211,22,-1,-28,29,31,53151u},{218,22,-1,-28,29,31,53601u},{220,22,-1,-28,29,31,54051u},{225,18,-2,-23,24,26,54501u},
 {233,18,-2,-23,24,26,54813u},{237,9,-2,-23,19,26,55125u},{241,18,-2,-23,25,26,55372u},{243,18,-2,-23,25,26,55697u},{250,18,-2,-23,25,26,56022u},
 {252,18,-2,-23,25,26,56347u},{32,12,0,0,0,0,56672u},{33,12,-2,-27,21,31,56672u},{34,17,0,-27,24,18,56998u},{35,23,-3,-27,32,31,57214u},
 {36,23,-1,-29,29,36,57710u},{37,34,-1,-27,39,31,58232u},{38,30,-1,-27,38,31,58837u},{39,10,0,-27,17,18,59426u},{40,14,-2,-28,23,38,59579u},
 {41,14,-4,-28,22,38,60016u},{42,19,1,-27,22,20,60434u},{43,23,-1,-22,27,26,60654u},{44,12,-4,-11,19,22,61005u},{45,12,-2,-16,19,13,61214u},
 {46,12,-2,-11,17,15,61338u},{47,10,-5,-28,23,34,61466u},{48,23,-2,-27,30,31,61857u},{49,23,-1,-27,27,31,62322u},{50,23,-3,-27,31,31,62741u},
 {51,23,-3,-27,31,31,63222u},{52,23,-3,-27,31,31,63703u},{53,23,-2,-27,30,31,64184u},{54,23,-2,-27,30,31,64649u},{55,23,0,-27,30,31,65114u},
 {56,23,-3,-27,31,31,65579u},{57,23,-2,-27,30,31,66060u},{58,12,-2,-22,19,26,66525u},{59,12,-4,-22,21,33,66772u},{60,23,-1,-22,29,27,67119u},
 {61,23,-2,-19,29,20,67511u},{62,23,-3,-22,29,27,67801u},{63,21,-1,-27,28,31,68193u},{64,25,-3,-27,34,36,68627u},{65,27,-4,-27,35,31,69239u},
 {66,27,-2,-27,34,31,69782u},{67,27,-2,-27,34,31,70309u},{68,27,-2,-27,34,31,70836u},{69,25,-2,-27,33,31,71363u},{70,23,-2,-27,32,31,71875u},
 {71,28,-2,-27,35,31,72371u},{72,28,-2,-27,36,31,72914u},{73,14,-2,-27,21,31,73472u},{74,23,-3,-27,32,31,73798u},{75,28,-2,-27,38,31,74294u},
 {76,23,-2,-27,29,31,74883u},{77,32,-2,-27,41,31,75333u},{78,28,-2,-27,36,31,75969u},{79,28,-2,-27,35,31,76527u},{80,25,-2,-27,33,31,77070u},
 {81,28,-2,-27,35,35,77582u},{82,27,-2,-27,34,31,78195u},{83,25,-2,-27,32,31,78722u},{84,25,-1,-27,33,31,79218u},{85,28,-1,-27,35,31,79730u},
 {86,27,0,-27,34,31,80273u},{87,34,-1,-27,43,31,80800u},{88,27,-4,-27,38,31,81467u},{89,27,0,-27,35,31,82056u},{90,25,-4,-27,36,31,82599u},
 {91,14,-4,-28,25,39,83157u},{92,10,-1,-28,15,34,83645u},{93,14,-4,-28,25,39,83900u},{94,23,0,-27,27,20,84388u},{95,17,-5,-2,26,10,84658u},
 {96,12,1,-28,17,12,84788u},{97,23,-3,-22,30,26,84890u},{98,23,-2,-28,30,32,85280u},{99,23,-2,-22,30,26,85760u},{100,23,-3,-28,32,32,86150u},
 {101,23,-2,-22,30,26,86662u},{102,14,-2,-28,24,32,87052u},{103,23,-5,-25,35,36,87436u},{104,23,-2,-28,29,32,88066u},{105,12,-2,-28,20,32,88530u},
 {106,12,-8,-28,26,39,88850u},{107,23,-2,-28,32,32,89357u},{108,12,-2,-28,20,32,89869u},{109,34,-2,-22,40,26,90189u},{110,23,-2,-22,29,26,90709u},
 {111,23,-2,-22,30,26,91086u},{112,23,-4,-22,32,33,91476u},{113,23,-3,-22,31,33,92004u},{114,16,-2,-22,25,26,92516u},{115,21,-3,-22,29,26,92841u},
 {116,16,-1,-27,23,31,93218u},{117,23,-2,-22,30,26,93575u},{118,21,-2,-22,30,26,93965u},{119,32,-2,-22,41,26,94355u},{120,23,-4,-22,34,26,94888u},
 {121,21,-5,-22,33,33,95330u},{122,19,-4,-22,30,26,95875u},{123,14,-2,-28,23,39,96265u},{124,10,-4,-28,20,39,96714u},{125,14,-5,-28,23,39,97104u},
 {126,23,-2,-18,30,16,97553u},{161,12,-4,-22,21,31,97793u},{176,14,0,-27,20,18,98119u},{183,12,-1,-18,17,14,98299u},{191,21,-3,-22,27,31,98418u},
 {193,27,-4,-33,35,37,98837u},{201,25,-2,-33,33,37,99485u},{205,14,-2,-33,25,37,100096u},{209,28,-2,-34,36,38,100559u},{211,28,-2,-34,35,38,101243u},
 {218,28,-1,-34,35,38,101908u},{220,28,-1,-34,35,38,102573u},{225,23,-3,-28,30,32,103238u},{233,23,-2,-28,30,32,103718u},{237,12,-2,-28,23,32,104198u},
 {241,23,-2,-28,30,32,104566u},{243,23,-2,-28,30,32,105046u},{250,23,-2,-28,30,32,105526u},{252,23,-2,-28,30,32,106006u},{32,16,0,0,0,0,106486u},
 {33,16,-2,-36,26,41,106486u},{37,46,0,-36,50,41,107019u},{40,19,-2,-38,29,50,108044u},{41,19,-6,-38,30,50,108769u},{43,31,-1,-28,36,33,109519u},
 {44,16,-4,-14,24,27,110113u},{45,16,-3,-21,25,17,110437u},{46,16,-3,-14,23,19,110650u},{47,14,-6,-37,30,45,110869u},{48,31,-2,-36,40,41,111544u},
 {49,31,-1,-36,36,41,112364u},{50,31,-3,-36,41,41,113102u},{51,31,-3,-36,41,41,113943u},{52,31,-3,-36,40,41,114784u},{53,31,-2,-36,39,41,115604u},
 {54,31,-2,-36,40,41,116404u},{55,31,1,-36,38,41,117224u},{56,31,-3,-36,40,41,118003u},{57,31,-2,-36,40,41,118823u},{58,16,-3,-28,26,33,119643u},
 {60,31,-1,-30,38,36,120072u},{62,31,-3,-29,38,35,120756u},{63,29,-1,-36,37,41,121421u},{65,36,-5,-36,46,41,122180u},{66,36,-2,-36,45,41,123123u},
 {67,36,-2,-36,45,41,124046u},{68,36,-2,-36,44,41,124969u},{69,33,-2,-36,43,41,125871u},{70,31,-2,-36,42,41,126753u},{71,38,-2,-36,47,41,127614u},
 {72,38,-2,-36,48,41,128578u},{73,19,-2,-36,27,41,129562u},{74,31,-3,-36,41,41,130116u},{75,38,-2,-36,50,41,130957u},{76,31,-2,-36,39,41,131982u},
 {77,43,-3,-36,54,41,132782u},{78,38,-2,-36,48,41,133889u},{79,38,-2,-36,47,41,134873u},{80,33,-2,-36,43,41,135837u},{81,38,-2,-36,47,47,136719u},
 {82,36,-2,-36,45,41,137824u},{83,33,-2,-36,42,41,138747u},{84,33,0,-36,43,41,139608u},{85,38,-1,-36,47,41,140490u},{86,36,1,-36,44,41,141454u},
 {87,46,0,-36,56,41,142356u},{88,36,-5,-36,50,41,143504u},{89,36,0,-36,46,41,144529u},{90,33,-4,-36,46,41,145472u},{161,16,-5,-28,27,41,146415u},
 {183,16,-1,-23,22,18,146969u},{191,29,-4,-28,36,41,147167u},{193,36,-5,-44,46,49,147905u},{201,33,-2,-44,43,49,149032u},{205,19,-2,-44,32,49,150086u},
 {209,38,-2,-45,48,50,150870u},{211,38,-2,-45,47,50,152070u},{218,38,-1,-45,47,50,153245u},{220,38,-1,-45,47,50,154420u},{32,30,0,0,0,0,155595u},
 {48,58,-2,-70,73,80,155595u},{49,58,0,-69,66,78,158515u},{50,58,-5,-70,76,79,161089u},{51,58,-4,-70,74,80,164091u},{52,58,-5,-70,75,79,167051u},
 {53,58,-4,-69,74,79,170014u},{54,58,-3,-70,74,80,172937u},{55,58,2,-69,72,78,175897u},{56,58,-4,-70,74,80,178705u},{57,58,-3,-70,74,80,181665u},
 {63,54,1,-70,66,79,184625u},{65,68,-8,-69,84,78,187232u},{69,63,-3,-69,81,78,190508u},{73,34,-2,-69,49,78,193667u},{75,73,-3,-69,93,78,195578u},
 {78,73,-3,-69,89,78,199205u},{82,68,-3,-69,83,78,202676u},{83,63,-3,-70,78,80,205913u},{84,63,1,-69,79,78,209033u},{32,28,0,0,0,0,212114u},
 {65,65,-8,-65,81,74,212114u},{69,60,-3,-65,77,74,215111u},{83,60,-4,-66,76,76,217960u},{97,56,-6,-53,72,63,220848u},{99,56,-4,-53,70,63,223116u},
 {100,56,-4,-68,74,78,225321u},{101,56,-4,-53,70,63,228207u},{103,56,-11,-60,84,86,230412u},{105,28,-4,-68,46,77,234024u},{106,28,-16,-68,58,94,235795u},
 {108,28,-4,-68,46,77,238521u},{110,56,-4,-53,70,62,240292u},{111,56,-4,-53,70,63,242462u},{114,37,-4,-53,57,62,244667u},{116,37,-2,-65,53,75,246434u},
 {117,56,-3,-52,70,62,248422u},
};
static const char fg_b64[] =
"AAEREREREAABESIzIhEQABEjREQyEQARI0///jIRARJN///TIRARJN//9jIRESNN//9TIRESNd//1TIRESNd//xDIRESNd/+ZCEQESNe/9UyEQESNe/9QyEQ"
"ESNe/2QhEQESRXd1MhEQETTv/9MhEAEjXv/8MhEAEjT//1MhEAES3//0IRAAESM0QyIRAAARIiIiEQAAABEREREAAAAAEREREREREAABEiIzMyIhEAESNERE"
"REMhEBE0//1v/9IRESPP/33/9DEREj3/99//QxERI9/+fv/UIRASNf9m3/UyEQEST+VV/TIRABEiMzMzMiEQABESIiIiERAAABEREREREAAAAAABERERERER"
"AAAAAAERIiIiIiIRAAAAAREjNEREQzIRAAABESNF/9Vv/SEQAAESI0bv533+UyEQABEkVn/8d+/WQhEAESPf///////0IRABEk7///////9CEQESNO//////"
"/+QhEBEjRn7+d9/ndTIRARI1d+/Xfv11QiEREjRn3/d9/3dTIRARI9////////UyEQESTv///////1MhEBEk7///////5CEQARI1fv1379dkMhEAARNN/3ff"
"9lQyEQAAESP/5V7+UyIREAABESNERERDIhEQAAABESIiIiIhERAAAAABERERERERAAAAAAAAAAABERERAAAAAAAAEREiIREAAAAAABESIzMhEQAAAAAREjTv"
"4yERAAAAESI0Xv1DIREAABEiNGf/dkMhEAAREkVt7/7WQyEAARI17/////0yEQASNe//////9CEQESNf//d3//9CEQESRv//7Xd3ZCEQESRu/////nZCEQES"
"Rn3/////ZCEQEjRnd3ff//1CEQEj3//nd+//1CEQEjX//////+UyEQESTf/////9QhEAESNW7//+1kMhEAARI1bf52VDIRAAAREjXf5kMiERAAABEiPv1DIR"
"EQAAABESIzMiEREAAAAAAREiIREQAAAAAAABERERAAAAAAABEREREREREREQAAAAERIjMyIhEREREQAAAREjRFRDMiIjMiEQAAESNN//5UMzPf4hEAARI07/"
"//5URF/0IRAAESTf937/ZVXv0yERABEk7+d9/3Zu/UMhEQARJP/nf/533+VDIhEQESTf///3ff52VDIhEBEjTv/+d3/3dmVUMhEBEjRVZnfv137/7UIRAREj"
"NFVu/n3///9TIQAREiNF3+d//nf/1CEAERIjRv533/d3/9QhAAERJF/8Zt/nfP/TIQABEjTv1VXf937+UyEAARI+/kQ0X///9UIRAAAS3/QzI03v/lQyEQAA"
"ESIyIiIzREQyIRAAAAERERERIiMyIREAAAAAEREREREREREQAAAAABERIiIiIRERAAAAAAEREiMzMzMiIREAAAABESM0VVZlVDMhEQAAABEjRW7v//7VQyER"
"AAARI0b///////1DIRAAARJM/////////UIhEAASNO///Hd37//VMhEAESNe//53d3d3d2QyEQESNX////537///9CEQESRn3///537///5CEREjR+////53"
"7///5CEREjXv/+d3d3//52QyEREkb//3d3d9//1kMhEBEk3//nd3d+//ZDIREBEk3//+d3fv/+UyERABEjX////////2QyEQAAESTf//////9kMhEQAAESNF"
"3v//7mVDIREAAAARIzRVVlVUMiERAAAAARESIzMzMyIREQAAAAAAEREiIiIhERAAAAAAAAEREREAABEiIiEQARIzQzIRARJP/9IRESPf/0MRESPf/0MRESPf"
"/kIRASNP9TIRARI/4yERARIjMhEQABEREREAAAERERAAAAAAAREREAAAAAEREiIRAAAAERIjMyEQAAERI03/whAAARI03/0yEAARI0X/5DIRAREjXv/UIRAB"
"EjXf/1MhEBESRv/9QyEQESNd/+ZCERARJG7/1TIRABE03//FMhEAEjXf/2QyEQASNe//ZCERABI17/5kIRAAEjXv/lMhEAASNe/+UyEQABI17/5TIRAAETTf"
"/1MhEAARJG//UyEQABEjXv9TIRAAERJN/9MhEAABEjTv4yEQAAARIjMyEQAAAAERIiERAAAAABERERAAAAAAAREREQAAAAAREiIREAAAABEjMyIRAAABEj/9"
"MhEQAAESTv5TIRAAARJN/9QyEQABEk3/9TIRAAESRv/2QhEAERJG//1DEQAREkb//VMhABESRu/9UyEAESNG7/1TIQARI0b//UMRABEjXf/2QhEAESNd/+ZC"
"EQERJG7/5TIRARI03//UMREBEjXv/lMhEAESRv/2QyEQESNe/+UyEQARI1/+UyERABEj7+VCIRAAES3/QyIRAAABEjMyIREAAAERIiERAAAAABERERAAAAAA"
"ABEREREAAAABERIiEREAABESIzMiERABESNF/kMhEAESPWz9XTIRARPf/v7/MhEBI13//+UyEREjXf//1TIRESPv7v79MhABI9Vv1tMhEAESNN/UMiEQAREi"
"MzMhEQAAEREiIREQAAAAEREREQAAAAAAABERERAAAAAAABERIiERAAAAAAERIzMyERAAAAAREjT/4yERAAABESJF/9QiEQAAERIjXf/VMhEQABEjRW3/dkMh"
"EAESNFd+/3dkMhEBE9///////zIRASPf//////9CEQEj7//////+MhEBEjVn3/12VCEQARIjRu/3VDIhEAARIjXv9UMiEQAAARIk/+QyERAAAAERIjQzIREA"
"AAAAEREiIhEQAAAAAAAREREQAAAAABERERERAAESIiIhEQESNERDIRARNP//4yERI1///TIREj3//1MhESPf//UyERI1f/1CEREjXv5TIRESPf5TIREBIu/T"
"IREAESMzIREAABESEREAAAAREREAAAAREREREAABESIiIhEAARI0RDMhEAEj7//9IRARI////SEQESP///MhEAESNEQyIRABESIiIhEAABEREREQAAARERER"
"EAESIiIhEBEiNEQyEREk///iERE0///TIRE9//9DEREt//9CEREjREMiEQESIiIhEAARERERAAAAAAAREREAAAAAAREiERAAAAARIjMiEQAAAREj7+IRAAAB"
"EjX/0yEAABESTf9DIQAAESNe/kMRAAERJM/1QhEAARI17/UyEQAREkX/1DIRABEjTf9UIREAESNe/lMhEAERJF/9QiEQARI07/UyERABEjX/5DIRAAESTf9U"
"IREAESNO/lMhEAARI179QhEQABEj3/UyEQAAESP/5CERAAABEt0yIRAAAAESIiIRAAAAABERERAAAAAAARERAAAAAAAAARERIiIREQAAAAAREiMzMzIhEQAA"
"ARIjRVVVRDIRAAARIjVu//7lQyEQAREkXv/////UIRABEjXv/////+UyEREjTf//d8//9kIRESNe//x3f//2QhERJG//53d+//1CERI13//nd37/9kIREjXv"
"/9dnf//2QhESNe//13ff/+YyERI2//93d+//1TIREjX//3d3//9kIRASNe//537//lMhEBEk3//////2QhEQESNe/////2QyEQAREjXe//7VQyERAAESI0VV"
"VUMyERAAABESIzMzIiERAAAAARESIiIRERAAAAAAARERERERAAAAARESIiIhEQAAAREiM0RDIRAAAREjRVbf4yEQABEjVt7//kIRAAEj3////9QhEAASPf//"
"//ZCEQARI+////9kIRABEjRn3//mQhEAERI1ff/+ZCEQAREjV+//1kIRABESNX7//VMhEAESI1f//3VCERERI0Z//+dkMhERI0Z9//53UyEREk///////0IR"
"ET3///////QhERLf//////4yEREjRVZmZlRCEQARIjM0RDMyIRABERIiIiIhERAAAAABEREiIhEREAAAABESIzMzMiERAAABESNFVVVUMhEAABESNW7//+ZD"
"IRAAESNe/////9QxEAASNO//////9TIQARI9//533//9MhEBEj//93d///YyEQESNGZ3d9//5TIRARI0Vmd+///EMRABEjNWd+///FMhEAESNFff//52QyEQ"
"ESJFfv//x3VCIRARI13//9d3dkMhEBE03///////QyEQEjX///////9TIQASPf///////kMRABI+///////+QhEAESNFZmZmZVQiEAAREjM0REQzMiEQAAER"
"EiIiIiIREQAAAAABEREiIhEREAAAABESIzMzMiERAAABESNFVVVUMhEAAAESNc7//+ZDIRAAASNO/////+QxEAARI+//////9TIRABEt//13f//1MhEAESNF"
"Z3ff/+UyEQARIjRX///+dTIRABESM1f//+d0MhEBESIzXf///3QyEQESIzRWd+//1TIRESNFVVZ37//kMRARJP/+Znf//9QhEBE0//53fv/+ZCEQESTv////"
"//1DIRARI9//////1TIRABESRd7//tZDIREAARIjRVVVRDIREAAAERIjMzMyIREAAAABERIiIhEREAAAAAAAARERERERAAAAAAAREiMyIhERAAAAAREjRFRD"
"IhEAAAAREjTv/9VDIRAAABEjTf/+ff0xEAABEiRu//d+9TIRABESNd//5+/1MhEAESNG//59//YyEQERJG7/53//5TIRARI13/933//WMhEBE0b/93fv/9ZC"
"EREjXv93d+//dkIRESTf////////QxERJO////////5DEREj7////////kIRARI0Vnd9//1lMhEBESM0RW7/91MhEAAREiIzX//1MhEQAAERESI0VUMiEQAA"
"AAERESIzIhEQAAAAAAEREREREQAAAAAREiIiIiIREQAAERIzRERDMiEQABESNVZmZmVDIRABEjX//////0IRARI03//////lMhARI17//////lMhARJG////"
"///lMhARNH//3v/td1MhESNd//////51MhESNe///////VMhESNe/+d37//lMhESNWd3d33//lMhESTMx3d33//lMhEjT//nd3///UIRETX//8d+//5TIRES"
"Tv//////ZDERESNe/////2QyEQARJF3v/+1UMhEQARIjRVVVQzIREAABESIzMzIiERAAAAEREiIiEREQAAAAAAERESIiEREQAAAAERIjMzMyIREAAAESI0VV"
"VUQyEQAAESI1bv//5kMhEAERJF3/////1CEQARI17//////1MhARI0f//3d+//0yEREjXv/8d3fd1TIRESRv/+ff/td1MhESNd///////nUyERI17///////"
"1TIREjbv//d37//lMhESNv//13ff/+UyERI2//93d9//1DEQEjXv/9d9//9kIRARNN///////VMhEBEjXv/////VMhEAERI13v/+1kMhEQABEiNFVVVEMhEQ"
"AAAREiMzMzIhEQAAAAEREiIiEREQAAABERIiIiIiIREAERIjNEREQzIhEBEjRWZmZmVUMhESPf///////9IREj3///////9CERJO///////+QhESP///////"
"5TIREjRWd3f//mQxEREjRWd//+ZTIRARIjRn//91MhEQESI1fv/9VCIRABEiRu//5kMhEAARI1f//3UyERAAESNe//5kIREAABEk3//3UyEQAAARNO//5kIR"
"EAAAETTv/9QyEQAAABEt///TIREAAAARI0REMiEQAAAAARIiMiERAAAAAAAREREREAAAAAAAAAEREiIiEREQAAAAERIjMzMyIREAAAESI0VVVUQyEQAAESNF"
"bv/+5kMhEAARJF7/////1CEQARI17//////1MhABEk3//nd+//UyEREjTf/9d3//9TIRESNX//////91MhERI1ff////93QyEREkbf/////+dDIREjXf//d3"
"///VMhESNf//x3fv/+QxEBI2//93d+//1CEQEjb//9d+//9kIRASNe///////VMhEBEk3//////VMhEAESNF3v/+1lMhEQABEjRFVlVEMhEQAAERIjMzMzIh"
"EQAAABEREiIiEREQAAAAABEREiIiEREAAAABESIjMzMyIREAABESM0VVVUMyEQAAESNF3v/+1UMhEAESNM//////xCEQARJN///////lMhERI1///Xd///ZC"
"EREkb//3d37//EIRESTf//d33//9QhERJG////////dCEREkbv//////9kIRESRX3v/s3//mQhERJGd3d3fv/9UyEREk//53d///ZCEQESTv/9d+//5TIRAR"
"JN//////5kIREBEjXv////9kMhEAARI13v/+1UMhEAABESNFVVVDIhEQAAAREiMzMyIREQAAAAEREiIiEREAAAAAAREREREAABESIiIREAARI0RDIRABEj3/"
"/zIRARJO//9CEQESTv/+QhEBEk///TIRARI0VUMhERESMzMyIRAREjNDMiEQESNFVDIhEBEk///jIRASNP//0yEQEj3//0MhABEt//9CEQARI0RDIhEAARIi"
"IiEQAAARERERAAAAABEREREQAAAREiIiERAAARI0RDIRAAESPf//MhEAESTv//QhEAESTv/+QhEAEST//9MhEAESNFVDIREBESM0QyIRABEiM0MyIRABEjRV"
"RDIRARI0///jIRARI1///TIRARI9//9TIRARI9//9TIRARI1f/1CEQARI17+UyEQARI9/lMhEQABIu/TIREAABEjMyERAAAAERIREQAAAAAREREAAAAAAAAA"
"ABEREREAAAAAABEREREREQAAAAERESIiIiIRAAABERIjM0RNIRAAERIiNEVd7/MhAAESI0Vs7//+MhABEjRW7////kMRABEk3////tZDIRABI9///td2QyIR"
"ARI9//53dlQyERARI+///+12QyERABE03v///9VCEQABEjRW3////TEQAAESI0Vt7/9CEQAAEREiNEVe8yEQAAABERIjMzMhEAAAAAERERIiIREAAAAAABER"
"ERERAAAAAAAAABEREAAAAAABERIiIiIhERAAERIzMzMzMiEQARIjRVZmVVQyEQESPv//////4yEBEk7//////9MhESNP///////TIREjRnd3d3d2QxERI1Z3"
"d3d3dUIRESTv//////5CEREk7//////+MhERI+///////TIRARI0VWZmVUMhEAERIzMzMzMiERAAEREiIiIiEREAAAAAEREREAAAAAAAAREREREREAAAAAER"
"IjIiEREQAAAAESPURDMiEREAAAEj3/1lRDIhEQABEj7//+ZVQyEQABEj3////tVDIRABEjRX7////kIRABEiNGd97//0MRABEiNGd33//0MRABEjRW3v///k"
"IRARI0Xv///+ZDIRARI+///+xlQyEQARJP/+1lRDIhEQARLf1VRDMiEREAABEjMzMiIREQAAABESIiEREREAAAAAEREREREAAAAAAAABEREAAAAAAAAAAAER"
"EiIhEREAAAARESIzMzIhEQAAARIjRVVVQyEQAAESNF3v/+ZDIRAAETTf/////UMRABEj3//////1MhEBEk///X3//9MhEBEk//53z//1MhEBEj1sd37//lMh"
"ABEiNFd+//5kIRAAESNFbv/+ZDIRAAERI17//WQyEQAAERI17u1kMhEQAAARI1d3ZTIhEAAAARJO//5DIRAAAAARJO//1CERAAAAARJO//UyEQAAAAARI///"
"QhEQAAAAARIjRDMhEAAAAAABESIiIRAAAAAAAAERERERAAAAAAAAAAABERERERAAAAAAABEREiIiIREQAAAAAREiIzMzMiERAAAAERIjPv/+4zIREAABESNP"
"7ERE39MhEAAREj39RERERf0yEQARI0/FVVVEVd8yEQESNO5mZmZlVV9DIQESPfZn3//tdm9DIQETTud/////52/TIREjT9fv/n3/589TIREj33d97///599D"
"IREk33f/////1+5CEREk7n3/53//1/1CEREk7n7//////+UyEREk7n7//+///1QhEBEk7nfv/nfv5UMhEBEj32ZmZmZlVDIRAAEST+VVVURDMiEQAAESNP5V"
"REwyIREAAAARIz3v/+MhERAAAAAREiMzMyIREQAAAAABERIiIhERAAAAAAAAAREREREAAAAAAAAAAAARESIiEREAAAAAAAERIzMzIhEQAAAAABESNFVVQyEQ"
"AAAAAREjXv//9TIRAAAAARI13////UIRAAAAESNG7////VMhEAAAESNd/////mMhEAABEjR//////mQhEAAREkbf/+3//nQxEQARI1f//3f//9UyEQERJG7/"
"/nf//9UyEQESNX///Xf//+YyEQESRu//93f//+ZCEREjV////////+ZCEREjXv////////dCEREk3/////////1CEREk7//nd3ff//1CEREt///WVVVv//4y"
"EREjRFVEMzRFVUMhEAESIzMiIiIjMzIREAARERERERESIhERAAAAERIiIiIiIhERAAABEiMzRERDMyIREAARI0VWZmZmVUMhEAARJO///////tQyEQESNe//"
"//////1CEQESRu////////5TEREjR///13d///9TEREjXf//13d///5TEREjbf//d3ff//ZCEREkbv///////9ZCEREkfv///////XUyERI1f////////9Uy"
"ERI13//9d3f///UyERI13//3d3fv/+UyERI17//nd33//+UyEBI17////////9QhEBI17////////VMhEBEk///////+ZTIRABEjRVZmZmZVQyERAAESIzNE"
"REMzIhEQAAEREiIiIiIiEREAAAAAARERIiIiEREQAAAAEREiMzMzMiERAAABESI0VVVVQzIRAAAREjRW7v/+1UMhEAARI0bv/////9QxEQESNG////////Uy"
"ERESRv///Xff//5CEREjXv//d2d///5CEREkb//+dlVu//4yERE13//9dURWZlQyERI17//3ZURVVUMiERI17//3ZUVWZUMhEBI17//3ZVbf//MhEBI17//3"
"dmfv/+QhEBE03//+d37//8MhEBEjXv///////UMRABEjRv//////1DIRAAESNF3v//7VQyERAAERIzRVVVVDMhEQAAAREiIzMzMiIREAAAAAEREiIiIRERAA"
"AAAAERIiIiIiEREAAAABEiMzREMzIhERAAARI0VWZmZVQyIRAAARJO/////u1UMhEAESNe///////mQhEQESRu///////+UyEREjR///13fv//ZCEREjXf//"
"13ff//1DEREjXf//d3d///5TEREkbv/+d2ff//1TIREkbv/+dmff//1DERI0f//+dmfv//ZCERI13//9d3f///YyERI13//3d33//9UyERI17//nd9///mQh"
"EBI17///////9kMhEBI17//////+ZDIRABEk/////+1lQyERABEjRVZmZlVDIhEQAAESIzNEMzMiEREAAAEREiIiIiIREQAAAAAAERIiIiIiIREQAAARIjM0"
"RDMzMiEQABEjRVZmZmVUQyEQARJO////////0hEBEjXv///////0IRARJG7///////9CEREjR///13d3ZUMhERI13//9d3ZlQyEQESNt//93d3ZUMhEBEkbv"
"//////5DIRARJH7//////+QxEQEjV////////kIRABI13//9d3d2QyEQASNd//93d3ZUMhEAEjXv/+d3d3ZTIRABI17////////SEQASNe////////QhEAES"
"T////////zIRABEjRVZmZmVUQiEQABEiMzREQzMyIRAAARESIiIiIiEREAAAAAAREiIiIiIREQAAARIjM0QzMzIhEAARI0VWZmZVVDIRABEk7///////0hEB"
"EjXv///////CEQESRu///////0IRESNH///Xd3ZUMhERI13//9d3ZUMhEREjbf//d3dlQyEQESRu///////TIRARJG7//////0MRABI0f///////QhEAEjXf"
"//13ZlQyEQASNd//92VEMiEQABI17//nVDIhEQAAEjXv/+ZDIREQAAASNe//5TIREQAAABEk///UIhEQAAAAESNEVDIhEAAAAAABEiIyIhEAAAAAAAARERER"
"EAAAAAAAAAAAERESIiIhERAAAAABERIjMzMzIhERAAABESI0RVVVVDMhEAABESNFbe///tVDIRAAESNG7//////9QyEAESNG7////////DIRERJG///9d33/"
"/+QhERI17//9d3d+//5CEREkb//+d3d3d3dkIREjXf//133////9QhESNe//93fv////ZCERI17//3d+////9kIREjXv//d3d37//2QhESNe///Xd3f//+Yy"
"ERE03//+d33///1TIRESRv/////////UMRARI03////////2QhEAESNF3v/+7X7/QyEQAREjNFVmVVVUQyEQAAERIjMzMzMzMiERAAABEREiIiIiIRERAAAA"
"ABEREREREREREREAABEiIzIiESIjMiEQABEjRFRDIiNEVEMhEAESTv/+QzNN///TEQESNe//5kRF7//1MhARJG7//nVVfv//UyERI0f//9d2d///5TIREjXf"
"//13d3///VMhESNt/////////9UyERJG7/////////dTIREkbv////////90MREjR//////////mQhESNd///Xd3f//+ZCERI13//3d3d///1TIREjXv/+dl"
"V9///VMhESNe//5lRW7//2QhEBI17//lQ0Xv//UyEQEST//9QzNP//5DIRARI0RUQyIjRFRDIRAAESIjIiERIjMiIRAAABERERERERERERAAAAAAEREREREA"
"ABESIzIhEAAREjRUQyEQARI9///TEQESNO//9TIQESNe//9TIQESRv//5TIRETR///1TIREjXf//1TIREjXf//dTIREkbv//dDEREkbv/+ZCEREkb//+YyER"
"E0f//9UyERE13//9UyERE07//2QhEBEk7//1MhEBEj///kIREBEiNFRDIRAAESIjIiEQAAAREREREAAAAAAAAAABERERERAAAAAAABESIzIhEQAAAAABESNF"
"VDIRAAAAAAESNP//4yEAAAAAARJN///TIQAAAAARI13//2QhAAAAABEjXv//ZCEAAAABEiRv//9kIQAAARESNX///lMhAAERESI1z//9UyEAEREiI0bf//1T"
"IQERIzM0Vu//90MRARI0VVVn7//2QhEBI9//53f//+UyEREj3//nff//1DIRESPf//////5TIRABE07/////5UIREAESNN7//+ZUMhEAAREjRVVVVDIhEAAA"
"ERIjMzMyIREAAAABERIiIiEREAAAAAARERERERESERERAAABEiMzIiIiMzMiEQAAESNEVUMzNEVUQyEQABEk7//lRFbv//4hEAESNe//5mZ+///kIRABEkbv"
"/+d37///UyEQESNH///Xfv//9kMhEBEjXf//1////nUyEQARI23//t7//+dUIhEAESRu//////93UyERABEkfv//////11MhEAASNX////////dTIRAAEjXf"
"///////9ZCEQABI13///59///mQhEAASNe///Xd////VMhAAEjXv/+d2fv//5TIQABI17//mVW3///UyEAARJP//1URF7//+MRAAESNEVEMyM0VVQyEQAAES"
"IzMiIiIjMyIREAAAERERERERESEREQAAAAAREREREAAAAAAAESIjIiEQAAAAABEjNFQzIRAAAAABEk7//kIRAAAAARI17//kMRAAAAARJG7//lMhAAAAESNH"
"///VMhEAAAESNd///VMhEAAAESNd//91MhEAAAESRu//51MhEQAAESRu//51MiERAAEjR///51QyIREAEjXf//12VDMhEQEjXf//d3dlQyEQEjXv///////j"
"IQEjXv///////TIREjXv///////TIREST///////9DIQESNFZmZmZVQyEQARIjNEREQzIhEQARESIiIiIiEREAAAARESIiIhERIiIiERAAAREiMzMzIiIzMz"
"MiEQABEjRWZlRDNEVmVUMhEBEjX////lRF3///5CEQESRv///+dmfv///lMRESNd////93ff///+UyERI13////3d/////1TIREkbv////d9/////FMhESR/"
"//7/937/7//nQxESNX//7v/8f/7v/+dCERI13//u//3v/e//5kIREjXf/+3//v/87//WMhESNu//5////9z//9UyERJG7//X///+ff//dTIREkb//9f///1+"
"//5kIRASRv//d+//937//mMhEBI9//937//nfv/+UyEQEj3//mbv/3Zv//1CEQARI0VURFVVREVVQyIRABESIzMzMzMzMzMyERAAARERIiIiIiIiIhERAAAA"
"ABEREREREREREREAABEiMzMiIiIjIiEQABEjRVVEMiM0REMhEBESTv/+ZENF///SEQESNe///WVV3//1MhARJG7///d2bf//UyERI1f////nd+//5TIREjXf"
"////137//VMhESNt/////3f//9UyERJG7/////7f//dTIREkfv////////90MREjR///3//////mQhESNd//99/////+ZCERI13//nfv////1TIREjXv/+d3"
"7////VMhESNe//12ff///2QhEBI17/91VW3///UyEQEST//1QzRu//5DIRARI0REMyI0RVVDIRAAESIjIiIiIzMyIRAAABERERERERIhERAAAAAAABEREiIi"
"IREQAAAAARESIzMzMyIREQAAAREiNEVVVVQyIRAAAREjRW3v/+7FQyEQABEjRu//////5kMhEBEjRu///////+UyERESRv///Xd+///UIRESNe///XZ3f//+"
"UyERJG///nZWd///5TIRE03//9dVVn///lMhEjXv//dlVWz//9UyESNe//92VVff//1DERI17//3dVZ+//9kIREjXv//x3Z9///lMhERJN///nd9///mQhER"
"EjXv///////2QyEQERJF7//////mQyEQABEjRd7v/+1lQyERAAARIjRVVVVEMiERAAABERIjMzMzIhERAAAAABERIiIiEREQAAAAAAAREiIiIiIREQAAABEi"
"MzREQzMhEQAAESNFVmZmZUMiEQABEk7//////lQhEQESNe///////1MhEBEkbv///////UIRESNH///Xff//5TEREjXf//13f//+UyERI23//3d3///lMRES"
"Ru//53fv//xCEREkbv///////lMhESNH///////+VCEREjXf/////+1UMhEBI13//3d2ZUMiEQASNe//52VEMiERAAEjXv/+ZTMiEREAABI17//lQiEREQAA"
"ARJP//1DIREQAAAAESNEVDMhEQAAAAAAESIzIiERAAAAAAAAERERERAAAAAAAAAAAAARERIiIiEREAAAAAEREiMzMzMiEREAAAERIjRFVVVUMiEQAAERI0Vt"
"7//uxUMhEAARI0bv/////+ZCERARI0bv///////lMhEREkb///13fv//1CEREjXv//12d3///lMhESRv//52Vnf//+UyERNN///XVVZ///5TIRI17//3ZVVs"
"///VMhEjXv//dlVX3//9QxESNe//93Zmfv//ZCERI17//8d3ff//5TIRESTf//53ff//5kMhERI17///////91MhEBEjRu//////51QhEQARI0Xe////92Qy"
"EQABESM0Vn7//+ZCERAAAREiM0Vu///jIRAAAAAREiM0Xd3dMhEAAAAAEREiM0RDMhEAAAAAAAEREiIiIhEQAAAAAAAAEREREREAAAAAABESIiIiIiEREAAA"
"ARIjM0REQzMiEQAAESNFVmZmZlRDIRAAESTv//////5UMhEBEjXv///////1QhEBEkbv///////+QxERI0f//9d3f///UxERI13//9d3f//+UxERI23//3d3"
"7//9QhERJG7////////mQhERJH7///////50MhESNH///////9dUIRESNd///X3//+dTIRASNd//93f//+dDIRASNe//53fv//1TIRASNe//53ff//1TIRAS"
"Ne//5lbf//5DEQARJP//1URf//9CEQARI0RVQzNEVUQyEQABEiMzIiIjMzIhEAAAERERERERERERAAAAAAEREiIiIRERAAAAAREiMzMzMiERAAABEiNFVmVU"
"QyEQAAESNFbe//7mQyEQAREkXv/////9QhEAESNe///////9MhABE03//9d8///TIRESNe///Xd+7uwyEREjX////+13dlMhERI17//////9ZDIRESNd////"
"///9UyEREjV37//////1MhERJFd3fe////9TIRETT//td33//+UyEBI1///nd9///UIRARNO///////+UyEQESTf//////5UIREBEjRd7//+1lQyEQABEiNF"
"VmZVQyIRAAABESIzMzMyIREAAAABERIiIiIREQAAAAAREiIiIiIiIREQABEiM0REREQzMhEQESNFVmZmZmZUMhEBEk7////////+MhARNO/////////jIREj"
"X/////////0yERE9/////////8MhARI1Z33//3d2QyEQESI1Z+//52VDIREAESNFfv/+dTMhEQABESNX7//WQyERAAABEjXP//1UIREAAAARI13//3UyEQAA"
"AAESNe//9kMhEAAAABEjbv/+ZCEQAAAAARI17//lMhEAAAAAESNf//1DIRAAAAAAET3//9MhEAAAAAABEjRFQyERAAAAAAABEiMyIREAAAAAAAAREREREQAA"
"AAAAAAARERERERERERERAAARIiMiIREiIzIhEAARIzRUQyIjNFRDIRABEk7//kMjTf//0xEBEjXv/+UzNe//9TIQESRu//5UNG7//1MhESNH///WRFf//+Uy"
"ERI13//9VEV///1TIREjXf//dURt///VMhESRu//51VW3//3QyERJG7//nVVfv//ZCEREkb//+dlZ+//5kIRESRv//12Z3///lMhERJG///Xd33//2QxEBEj"
"b//+d33//+UyEQESNe///////+ZCERABEkXv/////+ZDIRAAESJFzu//7WVDIRAAABEiNFVVVUQyIREAAAAREiMzMzMiEREAAAAAEREiIiIhERAAAAAAERER"
"EREREREREQABESIzIiESIjMiERABEjRVQzIiNERDIRARI+///UM0Xv/+MhARJN///VRFz//9MhERJN///WVW7//1MhERJH///nZn///VMhARJG///nd+//9k"
"IRARJG7//nd///1TIRARJG7//nff/+ZCIRARI13//3fv/9UyEQARI13//37//mQiEQARI1f//9//91MhEAABEkf/////5kIREAABEkbv////dTIRAAABEjXv"
"///+ZCERAAABEjTf///2QyEQAAAAESPP///lMhEAAAAAERI0VlVDIREAAAAAAREjMzMiERAAAAAAABERIiIREQAAAAABEREiIREiIiEREREREAARIjMzIiMz"
"MyIiMzIhEBEjRVVEREVVVEREVEMhERNP//5Vbv//ZV3//9IREjXv/+d3///3d+//5CERI27//nff//93fv/9QhESRv//93////d9//9kIREkb///d////8f/"
"/+UyERJG7//n3////X///FMhESRu//7e/+//3f/+ZCEQEkbv/+7/7e/97//VMhEBJG7////9f/////dDIRASNu////93/////mQhEAEjXf///+d/////dTIR"
"ABI13////Xf////mQhEAASNd///+d37///1TIRAAESTf///XZ+///2QhEQABEj3///ZVX///1DIRAAABEjRVVUM0RVVUMhEAAAAREiMzMiIjMzMiERAAAAAR"
"ESIiEREiIhEREAAAAAAAARESIiEREREREAAAABEiMzMyIjMzIhEAAAESNFVVRERFVEMhEAABEt///9VW3//+IRAAARJO///nfP//5CEQAAESTf//937//1Mh"
"EAABEkbv//3v//ZDIRAAARI13//////VMhEAAAESNX7////9ZDIRAAABEjV9////51QhEQAAERI1ff///3dTIREAABEjRn/////nUyEQAAARI1f/////91Mh"
"EAABEjRv///v//5kIRAAARJF7//3f///1DIQAAEjTv//137//+UyEAABI9///nZt///8MhAAAS3///ZUVv///jIQAAESNFVEMzRFVUMhEAABESIzMiIiMzMi"
"ERAAABERERERERIhEREAAAARERIRERERERERAAERIjMyIiIiMyIREAESNFVUMzNEVEMhEAEj7//9VEVv//8yEBEj3//+dmbv//0yEBEjXv//133//9QxEAEj"
"Xf//53///lMhEAESRv//9///5kMREAESNd/////+dTIRAAESNX/////9UyERAAERJG3////WQyEQAAARI1f///50MhEAAAARI1fv//dTIREAAAARI1f//+ZD"
"IRAAAAARE0f//+UyERAAAAABE03//9UyEQAAAAABEk3//2QhEQAAAAABEj7//0MhEAAAAAAAESNFVDIRAAAAAAAAERIjMiERAAAAAAAAAREREREQAAAAAAAA"
"AREiIiIiIiEREAAAESIzNEREQzIhEAABEjRVZmZmZVQyEQABEs/////////SEQABI9/////////DEQABI+////////1CEQABEkVnd+///+UyEQABEjRXff//"
"/mQhEAABEjRX3///51MhEAAREjRn////dUIREAARIkV+///8ZDIRAAARI1fv///XUyIRAAESNG7///52QyIRAAESRd///+d2QyIRAAEjXf///nd3ZTIRABEj"
"X////////9IRABEk7////////9MRABEt/////////0IRAAEjRVZmZmZlVDIRAAESIzNEREQzMiEQAAAREiIiIiIiEREAAAAAAREREREQAAABEiIzMiEQAAES"
"NEVUMiEAABEk7///MhEAESNe///kIRABEkbv//4yEQERJG//dkMhEBEjXf/3UyEQARI17/5kIhEAESNu/+UyERABEkbv/VMhEAERJG//dDIRABEjTf/2QhEQ"
"ARI13/9kIRAAESNe/+UyEQABEkbv/VMhEAARJG7/1TIRAAEjRv/3QyEQABI13/9kMhEAASNe/+dTIRAAEjXv/nUyEQABI17//+QhEAASNf///kIRAAES3///"
"0yEQABEjRVVDIRAAABEiMzIhEQAAARERERERAAAAABEREQAAERIhEQARIjMiEQESP/0yERE0/9MhESNe/UIREjXv1CERI1/9QxESNf/UMhEjX/5DIRI179Qy"
"ESNe/VMhEjXv5TIRI1/+UyESNf/lMhEjXv5TIRI17+QyERNO/kMRESTv5CEREj7+MhERIt3CERARIiIhEAAREREQAAARERAAAAABERERERAAAAERIjMyIRAA"
"ABEjRVRDIRAAESPv//9CEQABEk7//+UyEAARJO///lMhAAESNX7/5TIQABEjV//9UyEAARI0f//EMhAAESNX/+ZCEQABEjXf/mQhEAARI17/5TIRAAESNe/9"
"UyEQABEkb//VMhEAERJG//ZDERABEjXf/mQhEAARI13/5kIRABESNe/+UyEQARI0b//VMhEAESNX//xDIRARI0Z//mQhEAESTv//5TIRABEk///+QyEQARLf"
"///TIRAAESNEVUMiEQAAESIzMiERAAAAEREREREAAAAAAAAREREREAAAAAAREiIiIREAAAARIjREQyEQAAARIkXv/kMhEAAREjXv//UyEQABEjXf///UMREB"
"ESRv/+/+UyEQESNf/9f/9kIRARJO/9d9/9QhEBI9/+ZVb/5CEQES7/VERO/9IRARIzMzIjNDIREAESIiIiIiIREAABEREREREREAAAARERERERAAABERIiIi"
"EREAABEiIzMyIhEAAB3//////9EAABEiIzMyIhEAABERIiIiEREAAAARERERERAAAAEREREAAAERIiERAAESIzMhEAARL//jIRABEj//0hEAASNO/iEQABEi"
"MyIQAAEREiERAAAAERERAAAAAAEREiIiIREQAAAAERIjMzMyIREAAAERI0VVZVQyIRAAERJFzv//7VQhEAARI1//////5TIRARI0///////9QhEBEj3//3d/"
"//1CEQESNXd3d37//UMRERJGfe7////3QxERI13///////dDIREkb//9d9//50MhESTf/+d9///nUyERJO////////5DEREk7////u///kIRASNN7/7Xff/9"
"MhEBEjRVVVVVVUMhEAARIjMzMzMzIhEQABEREiIiIiIREQAAABEREREQAAAAAAARIiMiEREAAAAAESNERDIhEQAAABEjTv/9MiERAAABEjX//9QyIREQABEk"
"b//3ZUMiERARI13//3d2VDIRARI13//n7/7UMhERJG7//////9QhERJG7//////+UyESNH///nff/+YyESNd//93d///ZCESNd//53fP//YyESNu//53ff/+"
"UyESRu//53fv/9UyESRv//533//2QhESRv///////VMhESPf/+////5TIRESPf/3fv/tVCIRARI0VVVVVUMiEQAREiMzMzMyIREAABEREiIiIhERAAAAABER"
"EiIiEREAAAAREiIzMzIhEQAAERIzRVVVQyERABESNF3v/+1TIRAREjXf/////1MhERI13//////+QhERJG///Xff/+QhESNe//13d+/+QhESNe//d2ZndlMh"
"ESNv//d2ZndkMhESRv//d3ff/9MhESNf//133//kMhASNe///////UIRARJF7/////1DIRARIkXe//7VQyEQABEiNFVVVDMhEQAAERIjMzMiIREAAAARESIi"
"IRERAAAAAAAAAAERERERAAAAAAABERIiIhEQAAAAARESI0RDIhEAAAERESI1//4yEQAAEREiM13//kMRABERIzRFbf/+UxEAESI0Vmd+//1TIQESNF3v/n//"
"/VMhERJF///////3QxERI17//////+ZCEREk3//9fP//5kIREjXv/9d37//WMhESNu//d3fv/9UyERJG//93d///dTIREkb//nd8//9kIRASRv//13///mQh"
"EBI17//////+UyEQETTf///+//1DIRARI03v/nf//TIRABESNFVVVVVDIREAAREiMzMzMyIREAAAERESIiIiEREAAAAAERESIiIREQAAABESIzMzMiERAAAR"
"EjRFVlVDIhEAESI1be//7VQhERESNd/////+UyEREjXf//////5DEREkb//9d+//5TIRI17//Xd8//9TIRI27///////9TIRJG////////5TIRJG//53d3x3"
"ZDERI2///XfP//1CERI17//////+UyEREkXv/////lQhEBEiRd7//+ZUIhEAESI0VWVVQyIRAAAREiMzMzIhEQAAABERIiIhEREAAAAAAREREREQAAARESIz"
"IhEQABESI0VUMhEAARI0Xf/+MhEBEjRv///kIRERJF3//+1CEREjV+//12QhERJO/////UIREjX/////UyERI1/////lMhESNX3//nZCEQEjR+//1kMhEBI0"
"fv/3UyEQASNH//90MhEAEjTf/+ZCERABI13//lMhEAARNe//1TIRAAESTv/9QhEAABEj//9DIRAAAREjREMhEAAAAREiIiERAAAAAREREREAAAAAAAAAAAAB"
"EREREQAAAAAAAAEREREiIREAAAAAARERESIjMyIRAAAAARESIiM0Xd0hEAAAERIjM0RVbv/jIQAAARIzRWZnfv/1MhAAARI0Xe/////9QxEAABEkb//////n"
"ZCEQABEjX///////xTIRAAESRv//x37//UMhEAARJG7//////2QhEAARI0d//////9UyEQABEjV/////7nZDIRAAESRv//d3d3ZUIhEAESNd//53d3d1QhEQ"
"ARI13//////+ZDIRABEjbf///////1MhEAESRt////////UyEQARNO//13d3//9TIQABE1/////////lMRAAETTv///////lQhEAARI17/////7VQiEQAAES"
"NFZmZmZUMiEQAAAREjM0REQzMhEQAAAAEREiIiIiIREQAAAAAAEREREREAAAAAAAESIjIhERAAAAABEjREQyIREAAAARI07//TIhEQAAARI1///UMiEREAAR"
"JG//92VDIhEQESNd//93ZlQyEQESNd//59//1DIRESRu///////UIRESRv///////kMREjR///7d///lMhEjXf//d37//lMhEjXf/+d37//VMhEjXv/+d3//"
"/FMhEkbv/9d3//9kIREkb//9d8//5kIREkb//3Z9//5TIREj3//2Vu//1DIREj3//lRf//0yEQESNERDM0RUMhEQERIjMiIiMyIREAARERERERERERAAAAAR"
"ERERAAABESIiIRAAERI0RDIRABEj7//jEQESNO//0yEBEjXv/8MhARJFd3dTIREjTf//UyERI17//lMhESNe//1TIREkbv/9UyERNH//90MREjXf//ZCERI1"
"3//mQhESNe//5TIREjXv/9UyERI17//EIRESNf//UyEQET3//0MhEBEjREQyEQABEiIiIRAAAREREREAAAAAAAEREREQAAAAAREiIiEQAAABESNEQyEQAAAR"
"I+//4xEAABEjTv/9MhAAARI17//DIQAAESRXd3UyEAARI03//1MhAAESNe//5TIQABEjXv/9UyEAARJG7//VMhABEjR///dDEQARI13//2QhEAESNd//5kIR"
"ABEkbv/+UyEQESJG7//VMhEBEjR+//xTIRARI1f//2QhEBEjRt//9kIRARI1fv/+UyEQETT////UIREBE9///+UyEQARLO//1TIREAESNEVEMhEQAAESIzMi"
"ERAAAAEREREREAAAAAEREREREAAAAAAAABEiIiIREQAAAAAAESNERDIREQAAAAARI07//TIhERAAAAESNf//1DIiEREAABEkb//3VDMyIRAAESNd//92VVVD"
"IRABEjXf/+d37//yEQARJG7//Xfv//QhEAESR///1+//5UIRABI1f//93//nUyEQASNd/////+dkIhEAEjXf/////nUyERABI27/////91MhEAASRu//////"
"5kIRAAEkb//+3f/+ZCEQABJG//933//9QhEAASPf//d37//0IRAAEj3//mVd///SEQABEjRVRERFVEMhEAAREiMzIiMzMiEQAAARERERERERERAAAAAAERER"
"ERAAARIiIiERABEjNEQyEQARJO//4yEBEjXv/9QhARJG7//EIRETRv//ZCERI13//2QhESNe//5TIREkbv/9UyERJG7//VMhEjR///dDIRI13//2QhESNd//"
"5kIREjXv/+UyERI17//VMhESNe//xCEREjX//1MhEBE9//9DIRARI0REMhEAARIiIiEQAAERERERAAAAARESIiIiIiIiIRERAAARIjMzMzMzMzMzIhEQARIk"
"RVVVVVVVVVVDIREBEk3/937/7Xfe/+UyEREjXv/+////7////kMRESRv///////////+UyERJG///nfv//19//5jIRI13//3d+//133//2QhEjXf/+d37//H"
"ff/+YyESNe//53f//3d+//5TIRJG7//Xd//+d37//VMhEkb//9d9//53f//9QyESRv//dn7//XZ///ZCERI9//9mbv/9Zt//9TIREj3//lRf//VV3//kIRER"
"I0REMzRVRDNEVEMhEBESIzIiIjMyIiMyIhEAAREREREREREREREREAAAARESIiIiEREQAAESIzMzMzMiERABEiRFVVVVVDIRABEk3/937//UMhERI17//v//"
"/9QhERJG///////+QxERJG///nfv/+UyESNd//93fv/+UyESNd//53fv/9UyESNe//53f//8UyESRu//13f//2QhESRv//13z//mQhESRv//dn3//lMhESPf"
"//ZW7//UMhESPf/+VF///TIRARI0REMzRFQyERAREiMyIiIzIhEQABEREREREREREAAAABEREiIiEREAAAAREiIzMzIhEQAAERI0RVVVQyIRABESNG3v/+1T"
"IREREjXf/////lMhERI13//////9QhERJG///Xfv/+UyESNe//13fP//UyESNe//d3fP//UyESNv//d3ff/+UyESRv/+d3fv/9QxESNf//133//mQhESNe//"
"/////UMhERJF7/////xTIRARIkXe//7VQyERABEiNFVVVDMhEQAAERIjMzMiIREAAAARESIiIRERAAAAABERIiIiIRERAAABESMzMzMzIhEQABEiNFVVVVVD"
"IRAAESTf/3fv/tQyEQESNe//7////UIRARJG7//////+UyEBEkb//+d9//5TIREjXf//d3f//2MhESNd//53fP//YyERI27//nd9//5TIREkbv/+d37//VMh"
"ESR///533//2QhESNX///////9UyERI13//////+UyEREjXf/+zv/tVCIRASNe//53ZlQyIRABI17//WVEMiERAAEjX//8UzIhERAAARLP//UyIRERAAABEj"
"REQyIREQAAAAARIiIiEREAAAAAAAERERERAAAAAAAAAAEREiIiIhEREAABERIjMzMzMiEQAAESI0VVVVVUMhEAESNF3v/nf//SEQERJF/////v/1MhARI17/"
"/////+UyERE03//9fP//5TIREjXv/9d37//VMhESNu//d3fv/9UyERJG//93d///dTIREkb//nd8//90MRESRv//13///mQhEBI17//////+ZCEQETTf////"
"//1TIRARI13v/n///VMhEBESNFZ3f//3QhEQAREiNFZ//+ZCEQAAERIiNd//5TIRAAABERIj7//UIRAAAAABESI0RDIhEAAAAAARESIyIREAAAAAAAARERER"
"EAAAAAEREiIhEREAABEiMzMzIhEAARI0RVVUQyEQARJN//fe/jIQESNe//7//TIQESRv/////TIQESR/////9DIQEjXf//52UyEQEjXf//dkMhEQEjXv/+dT"
"IhEAEkbv/9ZCIRAAEkb//9UyERAAEkb//2QyEQAAEj3//1MhEAAAEj3//kIhEAAAESNEQzIRAAAAERIiIiEQAAAAAREREREAAAAAAAABERIiIhERAAAAERIi"
"MzMyIREAAAESM0VVVUMiEQABEjRd7v/tVCEQABEkb/////9TIRARI1///////kIRARJN//533//kIRARJN///td3dkIRARJG/////+dkIRESNGfv////9kIR"
"ESNGd3d9///UIRESPf/+d3//9jIRASPf//////5TIRARJO//////VCEQARI0zv//7VQyEQABEjRFVVVDIhEAABESIzMzMiERAAAAERESIiIREQAAAAABERER"
"EQAAABERIiIhEQAAERIjREMhEQABEjRf/0MhEAESNG7/5kIRABEkZ//+dTIRESNf////9CEREk3/////QhERJO/////kIRESRn//93UyEREkbf/+dUIRARJG"
"3//nUyEQESRu//1kMhEBEkb//9ZDIRARJG///XQyEQESPP///9MhABEjXv//9TIQABEjXv/+QhEAARIjRVRDIRAAAREiMzIhEAAAAREREREQAAAAARERERER"
"EREQAAAREiMyIiIyIhEAARIjREQzNERDIRABEk7//kRf//0hEBEjXv/9ZW//9TIRESRv//1mf//lMhERJG//93ff/+UyERI13//nd+//1TIREjXf/+d37//V"
"MhESNe//53f//3QyERI27//Xd///ZCEQEjXv/+d///5kIRASNe///////lMhEBEk7////v/9QyEQESNN//53//0yEQABEjRVVVVVQyERAAERIjMzMzMiERAA"
"ABERIiIiIhERAAAAEREREREREREQAAERIjMiIiIiIREAARI0VUMzNEQyEQARI+//9URf//MhEBEk3//2Zt//4yEQESTP/+d37/9TIRARJG//933//VMhEBEk"
"bv/3fv/2QhEAESNu//x//9UyEQARI17//e//ZDIRABEjXf/+//1TIRAAERNH////5kIhEAABEkbv///VMhEAAAESNe///lQhEQAAABEk7//1QyEQAAAAESNF"
"VUQyEQAAAAABEiMzMiERAAAAAAARESIREQAAAAAAERERERESIiERERERAAAREiMzIiMzMyIiIiIRAAESNFVENEVVRDNERDIRARI+//9VXf//VV3//iEQESTf"
"//d3///3Z///QhEBEk3//nff//533//UIRARJG//537///d+/+ZCEQESRu//d////3f//VMhEBEkbv/37/7/9+//ZCERARI27//f/3//3//VMhEAESNd////"
"1////+ZCERABEjXf///nf////VMhEAABEkb///137//+ZCERAAARI17//3d+//9kMhEAAAERJO//5lbv/+UyERAAAAESNFVURERVVDIREAAAAAESIzMyIjMz"
"IhEQAAAAAAEREiIREiIRERAAAAAAABEREiIhEREREAAAERIjMzIjMyIRAAABEjRVVERFRDIRAAESP///Zm7//yEQABEj3//9ff//QhEAARI1///n//9kIRAA"
"ESNd/////9UyEQABEjV+////1kMhEAARI1ff///XUyERABESRn////51MhEAARI1fv////dTIRAAESRu//7v/+ZCEQABI07//n3//2QhEAASPf//d37//kIR"
"AAEt//9lVu//8yEQABEjRERDRFVDIhAAAREiMyIiMzMiEQAAARERERESERERAAAAARERERERERERAAABESIzIiIiIiERAAARI0RUMzNEQyEQABEj7//1RF//"
"8yEQARI9//9mbf/+MhEAESRv/+d37/9TIRABEkbv/3fP/9UyEQARI27//H7/9kIRAAESNd//1//9UyEQABEjXf/93/9kMhEAARI1f////9UyEQAAESNH7///"
"5kIhEAABEjRu///9UyEQAAARI0bf//5kIREAABEiRX3//3UyEQAAARI1Z///1TIREAAAEST////mQiEQAAABE9///+VDIRAAAAARLf/+1UMhEQAAAAESNEVE"
"MiERAAAAAAESIzIiEREAAAAAAAEREREREAAAAAAAAAAREiIiIiEREAAAESIzNEMzIhEAABEjRVZmZVQyEQABEk///////iEQABI9///////DEQARI9//////"
"1CEQARI0Z33//+UyEQARI0V8///mQyEQARI0Z///91QhEQARI1f///dkMhEAESNG7//9dkMhEAESNd///Xd1MhEAESTf//////QhEAESTv//////QhEAES3/"
"/////+MhEAESNFVmZmVEIhAAARIjM0RDMyIRAAABESIiIiIREQAAAAAAEREREQAAABERIiIREAAAESI0QyIRAAESNN//0hEAERJN///CEQARI17/5TIRAREk"
"bv9kMhEBEjTP/2QhEAESNd/+UyEQERJF7/1TIRARI0bv/UMhEBEkXf/lQhEAETT//nUyEQASPP/+ZDIRABI0b//UMhEAEjRu/9QyEQARJG7/xCIRABEkbv9k"
"IREAESRv/2QhEAARJN//ZCEQABEk3/5kIRAAESPf//QhEAARI03/8yEQAAESM0QyIRAAABEiIiERAAAAARERERAAAAAAABEREQAAABERIhEQAAARIzMhEAAB"
"Ej3/MhEAARJN/kIRABEjTv5CEQARI179QhEAESRf/UIRAREk3/ZCEQESNN/lMhEBEjXv5TIRARI17+UyEQESNf/UMhEBEkb/1CERERNN/2QhEBEjTf5TIRAR"
"I17+UyEQESNe/kMhEBEjX/1CERARJN/1QhEAESTf5TIRABEj7+QxEQARI+/jIRAAARIzMhEQAAEREhERAAAAAREREAAAAAABEREREAAAABEiIiERAAABEjNE"
"MiEQAAEST/7TIRAAARPf//0yEQARI03//TIRABEjRu/2QhEAESNG//ZCEQARI03/9jIRABEjXf/lMhEAESNd/9ZCEQARI17/5kIRABEjV+//QhEAESNX7/9C"
"EQERJG//dTIRARI03/1kIhEBEjXv/VMhEBESRu/2QyEQESNG//ZCERARI13/9TIRABEkbv/kMhEAES3//8MhEAARLv/lQhEQABEjNEMiEQAAARIiIhEQAAAA"
"EREREQAAAAAAAREREREREQAAAAARERIiEREREQAAAREiMzMiIiIREAARIjRVVERDMyEQABEkzv/tZlXuMhABEj7///////4yEAESP////////TIQARI/1WbO"
"7/5TIRAAESM0REVVRDIREAAREiIiMzMyIREAAAERERESIhEREAAAAAERERERERAAAAAAERERERAAABESIiIREAABEjREMhEAARI9//8yEQARJO//4yEQARNO"
"//1CEQESNO//0yEQESNGd2QyEQESNe/2QyERESRf/2QhEREiRv/2QhEREjXf/mQhEBEjXv/mQhEBEkbv/2QhEBE03//1MhEBE17//1MhEBE0///kMRABEt//"
"/jIRABEjRFQyIRAAESIjIhEQAAAREREREAAAAAARERERAAABERIiIREAABEiNEMiEQARI03/1DIQARJN///TIRASPf53/kIRESPvd3/0IRESPv19/TIRASPf"
"//5DIRARI9/9QyEQARIjRDMhEQABESIiIREAAAERERERAAAAARERERAAAREiIhEQAREjRDIhEBEj7//SEQESTv/0IRARJP//QhEBEt//4yEQESM0QyEQABEi"
"IiERAAARERERAAAAAAERERERAAAAAAAREiIiERAAAAAAESNEQyEQAAAAARI9//4yEQAAABESTv/+MhEAAAARI07//UIRAAABESNe//0yEQAAERI0V3dlMhEA"
"ABEiNW3u1DIRAAESI1bv/9QyERABEjV///5kMiEQESNd///XZUMhEBEkz//9d3zEMhARJO//93f//jIQESTv//d+//0yEBEk3//////0MhARI1/////+UyEQ"
"ARJF3v/+ZTIREAESI0VVVUMhEQAAERIjMzMiERAAAAEREiIhEREAAAAAAAAAABEREREAAAAAAAAAAREiIhEQAAAAAAAAERIzMyEQAAAAAAABESPf/+IQAAAA"
"AAAREjT//TIRAAAAAAERIk/+VDIRAAAAAAESI0VmVDIRAAAAABEiRWd3ZDIRAAAAAREkXv//9UIRAAAAARI13////UMhEAAAESNG7////VMhEAAAESNd////"
"/mQhEAABEjR//////mQhEAAREkbf/+3//nQxEQARI1f//3f//9UyEQERJG7//nf//9UyEQESNX///Xf//+YyEQESRu//93f//+ZCEREjV////////+ZCEREj"
"Xv////////dCEREk3/////////1CEREk7//nd3ff//1CEREt///WVVVv//4yEREjRFVEMzRFVUMhEAESIzMiIiIjMzIREAARERERERESIhERAAAAAAAAARER"
"ERAAAAAAAAABESIiERAAAAAAABESIzMyERAAAAABERIj3//SEQAAAAEREiNP/9MhEAAAAREiI0/+VDIhEAABEiM0RWZmVDIRAAESNFZnd3d2VDIRABEk7///"
"/////SEQESNe////////QhEBEkbv///////0IRESNH///Xd3dlQyEREjXf//13dmVDIRARI23//3d3dlQyEQESRu///////kMhEBEkfv//////5DERASNX//"
"/////+QhEAEjXf//13d3ZDIRABI13//3d3dlQyEQASNe//53d3dlMhEAEjXv///////9IRABI17///////9CEQARJP////////MhEAESNFVmZmZVRCIRAAES"
"IzNERDMzIhEAABERIiIiIiIREQAAAAAAAREREQAAAAABESIiEQAAAAESIzMyEQAAAREj3//REAAAESPf/0IRAAARIs/+QyEQAAESNFVUMhEAARI0VmVDIRAA"
"ESTf//0yEQARI17//1MhEAESNe//9jIRABEkb//+UyEQESNH///VMhEBEjXf//1TIRARI13//3UyEQESRu//90MRABEkbv/+ZCEQARJG///mMhEAETR///1T"
"IRABE13//9UyEQARNO//9kIRAAESTv//UyEQABEj///kIREAARIjRUQyEQAAARIiMiIRAAAAAREREREAAAAAAAAAEREREREREAAAAAAAERIiMyIiIRAAAAAA"
"ERI0REREMiEQAAAAERI03/5m39IRAAAAERI07/////QyEQAAERI0X/////1DIRAAARI0Xvx37/1lQiEQARI0V3d3Z3d3ZTIRARI07//ndmZ///0yEBEjXv//"
"12Z9//9TIQESRu///3d33//1MhESNX////53fv/+UyERI13////9d+//1TIREjbf////93///VMhESRu/////+3//3UyERJH7/////////dDERI0f//9////"
"//5kIREjXf//ff/////mQhESNd//537////9UyERI17//nd+////1TIREjXv/9dn3///9kIRASNe//dVVt///1MhEBEk//9UM0bv/+QyEQESNERDMiNEVVQy"
"EQABEiIyIiIiMzMiEQAAARERERERESIREQAAAAAAAAAAEREREAAAAAAAAAAAERIiIRAAAAAAAAABESIzMyEQAAAAAAABESNO//0RAAAAAAABESI9/+QyEQAA"
"AAARESI9/9VDIRAAAAARESM0VmZUMhEQAAAREiNFZnd2VDIREAAREjRW3v/+7GQyEQABEjRu//////5kMhEBEjRu///////+UyERESRv///Xd+///UIRESNe"
"///Xd3f//+UyERJG///nZWd///5TIRE03//9dVVn///lMhEjXv//dlVWz//9UyESNe//92VVff//1DERI17//3dVZ+//9kIREjXv//x3Z9///lMhERJN///n"
"d9///mQhEREjXv///////2QyEQERJF7//////mQyEQABEjRd7v/+1lQyERAAARIjRVVVVEMiERAAABERIjMzMzIhERAAAAABERIiIiEREQAAAAAAAAAAABER"
"ERAAAAAAAAAAABESIiERAAAAAAAAAREiMzMhEAAAAAAAEREiPv/9IRAAAAABERESPf/kMhEQAAABERIiLf/VQyIREAABEiMzMzREREMyERABEjRFVEM0RVVU"
"MhEAESTv/+RDRN///TEQESNe//5UNF7//1MhARJG7//mREbv//UyERI0f//9ZEV///5TIREjXf//1kRX///VMhESNd//91RW3//9UyERJG7//nVVbf//dDIR"
"Ekbv/+dVV+//9kIRESRv//52Vn7//mQhERJG///XZnf//+UyEREkb//9d3ff//ZDEQESNv//53ff//5TIRARI17///////5kIREAESRe//////5kMhEAARIk"
"XO7//tZUMhEAAAESI0VVVVRDIhEQAAABESIzMzMyIREQAAAAARESIiIiEREAAAAAAAAAARERERERAAAAAAAAERIiIiIiEQAAAAAAARIzRDNDMhEAAAAAARI9"
"/+Xf/jIRAAAAEREj3/5e/9MhEQAAEREjTv/V7/1CIRAAARIjRFVVVVZUMhEQARI0VVVURFVmVDIRABEk7//lREXf//0yEBEjXv/+VERe//9TIQESRu//5kRW"
"7//1MhESNH///WRFf//+UyERI13//9ZEV///1TIREjXf//dUVt///VMhESRu//51VW3//3QyERJG7//nVVfv//ZCEREkb//+dlZ+//5kIRESRv//12Z3///l"
"MhERJG///Xd33//2QxEBEjb//+d33//+UyEQESNe///////+ZCERABEkXv/////+ZDIRAAESJFzu//7WVDIRAAABEiNFVVVUQyIREAAAAREiMzMzMiEREAAA"
"AAEREiIiIhERAAAAAAAAAAARERERAAAAAAAAEREiIhEQAAAAAAERIzRDIRAAAAAAERI03//SEQAAAAERI03/9TIRAAAAESI0z/5lQiEQAAESI1Z3d3ZDIRAA"
"ESJFzv//7WQyEQARI1//////5TIRARI0///////9QhEBEj3//3d///1DEQESNXd3d37//VMRERJGfe7////3QxERI13///////dDIREkb//9d9//50MhESTf"
"/+d9///nUyERJO////////5DEREk7////u///kIRASNN7/7Xff/9MhEBEjRVVVVVVUMhEAARIjMzMzMzIhEQABEREiIiIiIREQAAAAAAARERERAAAAAAABER"
"IiIREAAAAAAREiNEMiEQAAAAERI0X//iEQAAABESNF//1CERAAAREjRe/nVDIRAAESI0V3d3ZDIREBEiNW3v/+1kMhEREjXf/////lQhERI13//////+QyER"
"JG///Xfv/+UyESNe//13fP//YyESNu////////UyESRv///////+UyESRv/+d3d8d2QxESNv//13z//9QhESNe///////lMhERJF7/////5UIRARIkXe///m"
"VCIRABEiNFVlVUMiEQAAERIjMzMyIREAAAARESIiIRERAAAAAAEREREQAAAAERIiIREAAAERIzMyIRAAARI0//4hEAARI07/4yEQAREj3/1TIRABEjRndkMh"
"EAESTf//QyEQESNe//5TIRARI17//VMhEBEkbv/9UyEQETRv//dDEQASNd//9kIRABI13//mQhEAEjXv/+UyEQASNe//1TIRABI17//EIREAEjX//1MhEAAR"
"Pf//QyEQABEjREQyEQAAARIiIiEQAAABEREREQAAAAAAAREREiEREQAAAAERIjMzMiIRAAABESNEVVREMhEAABEjRe/tdv4yEQARI0b/////0yEQERJG7///"
"/+UyEQESNX/nfe/nZCEREjTf/3fv/9ZCEREjXv/+////1DIREkb///////5TIRE0f//+d+//5TIRI13//3d+//5TIRI13//nd+//1TIRI17//nd///xTIRJG"
"7//Xd///ZCERJG///XfP/+ZCERJG//92ff/+UyERI9//9lbv/9QyERI9//5UX//9MhEBEjREQzNEVDIREBESIzIiIjMiERAAEREREREREREQAAAAAAABERER"
"EAAAAAAAEREiIhEQAAAAABESI0QyIRAAAAAREjRf/+IRAAAAERI0X//TIREAABESNF7+ZUIhEAAREjRWd3dkMhEQESI1be//7WQyERESNd/////+VCEREjXf"
"//////1DIREkb//9d+//5TIRI17//Xd8//9jIRI17/93d8//9jIRI2//93d9//5TIRJG//53d+//1DERI1///Xff/+ZCERI17//////9QyEREkXv/////FMh"
"EBEiRd7//tVDIREAESI0VVVUMyERAAAREiMzMyIhEQAAABERIiIhEREAAAAAAAABEREREAAAAAAAARESIiERAAAAAAAREiM0MiEQAAAAEREiNP/+IREAAAER"
"IjNP/9QyEQAAESIzRO/mVDIhEAESNEVVVmZmQyEQARJO//5mb//9MhARI17//WZ///UyEREkb//9d3//5TIRESRv//d33//lMhESNd//53fv/9UyERI13//n"
"d+//1TIREjXv/+d3//90MhESNu//13f//2QhEBI17//nf//+ZCEQEjXv//////5TIRARJO////7//UMhEBEjTf/+d//9MhEAARI0VVVVVUMhEQABESIzMzMz"
"IhEQAAARESIiIiIREQAAAAAAERERERERAAAAAAESIiIiIhEQAAAAESM0RERDIhEAAAESJP/8X/8yEQAAERI1//Zv/kMhEAERI03/5t/+UyEQARI0Z3d2d3dk"
"IRAREk7//nZ///0yEREjXv/9d3//9TIRESRv//13f//mMhERNH//93ff/+UyERI13//nd+//1TIREjXf/+d37//VMhESNe//53f//3QyERI27//Xd///ZCEQ"
"EjXv/+d///5kIRASNe///////lMhEBEk7////v/9QyEQESNN//53//0yEQABEjRVVVVVQyERAAERIjMzMzMiERAAABERIiIiIhERAAAAABEREiEREAAAERIj"
"MzIhEAABEjRVVDIRAAESPf///jIRABEk3///5CEQARNN///8QhEBEjXf//5kIRARI13//+UyEQESNd///VMhEBEjXv//dDIRARI17//mQhEAESNe//1TIRAB"
"EjXv/2QyEQARI17/5UIRAAESNe/9QyEQAREjVmZUIhEAESNFZ2VCERABEjz//+QhEAARJN///kIRAAESTv//1CEQABEk7//9MhEAARI///9DIRAAERI0VEMh"
"EAAAERIjMiERAAAAEREREREAAAAAERESIiIhERAAARIjMzMzMiEQARI0VVRFVEMhERI07/9m7/9CEREjX//3z//0MRESTf//ff//UxERJN//5+//5DEREk3/"
"/X7//UIRESTf/nff/lMhERI9/9Zt/2QhERETT/VEX+QyEQARIzMzMzMyEQAAERIiIiIiERAAABEREREREQAAAAAAAAARERERERERAAAAAAAAERIiIiIiIREA"
"AAAAAREiNEQzNEMiEQAAAAERIkz/5VXv8yEQAAABESI17/1m3/5CEQAAABEiNW//d37/ZCEQAAARIzVt/+d3/+ZDIRAAESNFd+/8d+/9dTIRAAESTv//////"
"///TIRAAETTv/////////TIRABEjT/////////9TIQABEjV33/53f/93VCEQARI0V37/d33/12QyEQARI1Z9/+d3//d2QyEQARNO/////////+QhEAASNf//"
"///////9QhEAASPf/////////1MhEAARNGf/93fv/XZTIRAAARJG3/53f/51QyERAAARI17/133/1kMhEQAAARI17/Zm7/ZDIREAAAABEt/+VF//QyERAAAA"
"ABEjNEMzREMhEQAAAAAAESIiIiIiIREAAAAAAAAREREREREQAAAAAAAAAAAAABERERAAAAAAAAAAEREiIhEQAAAAAAABESIzMyERAAAAAAAREiPf/TIREAAA"
"AAERIjXv9UIhEAAAABEiNFbv51MiEQAAARIjRnf/52UyEQAAESJFfv///+1TIRAAESRe///////kMRABEjXf///////9MhABEkb//+d37//9MhEBEk3//+d3"
"fd3VMhEBE13////+3XdkMhERI03///////1kMhERI0b////////VMhERI0Z+///////lMhERI1d3d97////lMhARJO7u93d9///lMhARJP///Xd+///EIRAR"
"JN////////5TIRARI17//////+VCEQABEkV+////51QyEQABEiNWfv53VDIhEAAAESI1f/5lQyIRAAAAERI0X/1TIhEQAAAAAREj3/0yIREAAAAAABESMzMi"
"ERAAAAAAAAERIiIREAAAAAAAAAARERERAAAAAAAAAREREREREREREREAAAAAAREiIzMiIREREiIRAAAAAREjNFVUMyIRIjMyEQAAABEjRe//5UMiIz3/0hAA"
"ABEjTf////ZDM0X/4yEQAAESTf/3fv/lREX/9DIRAAASNe/9d9/+ZVXv9UIREAABI1//d33/5lbf/UMhEQAAEjX/93fv/Xbf/VQyEREAASNe/9ff/3d+/mVD"
"IhEQABEk7////3d+/3ZUQzIREAESNN7/7Xd9/8dmZVQyERABEjRVZmZ9/9d87//VMhEAERIjNEVn/+d+////5DIRABESIjRW7/d9/+d+/9MhEAAREiNF7/d3"
"7/d37/1CEQABESI13/1nz/93fv/UIRAAARI0b/5lbf/nd//2QhEAABEjX/5lRd/+d3/+UyEQAAESTv9URF3/537/1CEQAAASPf/UMzRe////5DIRAAABLf/U"
"MiI0Xv/+VDIREAAAESMzIhESI0RVRDIREAAAABEiIRERESIzMiIREAAAAAAREREREREREREREAAAAAAAERESIiIiIhERAAAAAAAAEREiMzNDMzIhEQAAAAAA"
"ERIjRVVmZlVDIhEAAAAAERI0Vt7v//7WVDIRAAAAARI1bv///////mQiEQAAARI13/////////9TIREAABEkb//////////+QyEQAAEjXv///nd3fv//5TIR"
"EAESNe///Xd3d33d12QyEQARI17//+d3d3d3d3dkMhEBEjXf/////nff////9CERESRn3////+d+/////kIRESNG3/////137////+QhERI13//////Xfv//"
"//0yEREkb///13d3d///13VCEQEjXv//53d3d9///WUyIRASNf///nd3d37//3UyIRABI1////13d97//+ZCIREAETXv//////////xDIREAARJG////////"
"//1TIREAABEjRu////////ZTIREAAAARI0Vt7///7WVDIREAAAABESM0VWZmZVQzIREAAAAAAREiIzNEQzMiIREAAAAAAAERESIiIiIhEREAAAAAAAARERER"
"EAAREiIiIRABEjREQyEREjTv/0IRESNf//QxERJN//9TEREk7//kMRESTv/9QhERJN/+UyEREj3/xCERESNP5DIRABEiMyIREAAREiEREAAAERERAAAAAAAA"
"EREREQAAAAAAEREiIhEAAAAAERIzQzIRAAAAERI03/4hEAAAERI13/9CEQAAERI13//UIRAAARI0b//lMhEAAREkbv/8QyEQABEjXf/+VCEQABESRu//xTIR"
"AAESNd//5kIREAARJG///VMhEAARI0z//3QyEQABEjXf/+ZCERAAESRu//1TIRAAARJG7//FMhEAABE0f//3UyEQAAEjXf//dDIRAAASNd//50MhEAABI13/"
"/nQyEQAAETTf//dDIRAAARJG//90MhEAABEkb//3QyEQAAESNe//1TIRAAARI03//VMhEAAAESNe/+UyEQAAARI03/9TIQAAAAESNO/9IRAAAAAREjNDMhEA"
"AAAAERIiIhEAAAAAABEREREAAAAAAAAAEREREQAAAAABESIiERAAAAARIjRDIREAAAARLf/kMhEQAAARJO/9QyEQAAARNO//ZCEQAAARNN//1TIRAAARJG//"
"5TIRAAARJG//9kIRAAARJG7//UMhAAARJG7//VMhAAARJG7//VMhAAARJG7//lMhAAERJG7//VMhAAESNH7//VMhAAESNX7//EMhAAESNd//9kIRAAESRu//"
"9kIRABEiRv//1TIRABEjV///ZDERAREkXv/+UyEQARI0bv/2QyEQARI13//lMhEAARJG//9kMhEAESNd//1TIRAAESNf/9UyERAAESTv/lMiEQAAES7/5DIh"
"EAAAASI0MyERAAAAAREiIhEQAAAAABEREREAAAAAAAABEREREQAAAAARESIiERAAAAERIzMzIREAABEiNN/0MiEQARI0Vt/mVCEQARJO99/X/jIRASPf////"
"/jIRESNX7//+dTIRESNX7//+dTIRESPv/////TIQESPv3f3f5CEQARJFbv1lQyEQARIzT/1DMhEAABEiM0MyIREAAAERIiIhERAAAAAREREREAAAAAAAABER"
"EREAAAAAAAABESIiIRAAAAAAABESNEMyEQAAAAABESNP/8MhEAAAABESNN//UyERAAABESI17/5kIhEAABESI1bv/mQyIRABEiNFZ//9dUMhEQESRWd3//13"
"ZTIRESPf////////0xERJO/////////DIREk7////////1MhEST/////////QhERI0V3f//XdlQyEQESNFZ//9ZUMyEQAREiRd//dUMiEQAAERI03/5UIhER"
"AAABESPv/kMhEQAAAAARIjRDMhEQAAAAAAERIiIhEQAAAAAAABEREREAAAAAAAERESEREQABESIzMiEQAREjRVVDIRARI8///8IRARJN///1MhESNe///lMh"
"ESNe///lMhESNe///VMhESNf///UIRESNX3/5TIRESNX/+ZDIRESNe/9UyEQASPf/lMhEQARLv9DIREAARIzMyERAAABESIhEQAAAAEREREAAAAAEREiIRER"
"ABESIzMyIRABEjRVVUMhEBI9////0hERI9////QxERJO////QxERI/////QhEBEjRVVEMhEBESMzMyIRAAEREiIREQAAERESEREQAREiMzIhEAESNFVUMhER"
"I8///8IRESTf//9TIRE17//+UyESNe///lMhETTv//1CEREk///9MhERI0RVQyERARIjMyIREAERESEREQAAAAAAARERERAAAAAAAREiIhEQAAAAAREjMzIR"
"AAAAABEj3/4yEAAAABEjTv/TIRAAABESNf/1MhEAAAESNN/+UyEQAAARI17/1CERAAARIkb/5TIRAAABEjXf/VMhEAABESRe/2QyEQAAESNN//UyEQAAARI1"
"7/1TIRAAAREkb/5kIREAABEjXf/lMhEAAAESNe/8QyEQAAESNG//ZCERAAARI17/1TIRAAABEkX/9kIREAABETTf/lMhEAAAESNe/9QyEQAAARI17/VCERAA"
"ABEj3/5DIRAAAAESP/9TIREAAAABI93TIREAAAAAESIyIREAAAAAABEREREAAAAAAAAREREAAAAAAAAAABEREiIiIRERAAAAABERIjMzMzIhEQAAABESI0VW"
"ZlRDIREAABESNFbe//7WQyEQAAESNG7//////EMhEAESNG////////ZCEQERJG7////////UMRARI13//+d37//+UyEBEkbv//d3ff//5jIREjXf//53d8//"
"/mQhESNe///XZn3//+YyERJG///3dmff//5jIREkf//+dlZ+///VMhEjXf//52Zn7//9UyESNd///nZnfv//dCERI13//+d3ff//5kIRETXf//53d+///VMh"
"ERJH///9d+///mQhEBEkb////////9UyEQESNd///////9UyEQABEkXv/////9VCERAAESJF3u//7WUyIRAAABEiNFVmZUQyIRAAAAAREiMzMzMiERAAAAAA"
"EREiIiIRERAAAAAAAAABEREREREQAAAAARERIiIiIREAAAAREiMzREQyIRAAABEjRFZn7+MhEAABEjRn3v//5CEQAAEjT//////UMRAAASPP/////9UxEAAR"
"I9//////dDEQAAEj7/////50IRAAARJFd9///mQhEAABEjRn7//+ZCEQAAESJFfv//1TIRAAAREjV////VMhEAABEiRX///3UyEQABESNG3//+dTIRAAESI1"
"ff//51MhEQESI0Z+///XZDIRARI1Z3///9d1QhERI9/////////TIREk7////////8MhESTv////////UyERI/////////9CEQESNFZmZmZmVDIRARIjM0RE"
"REMyIRAAEREiIiIiIiERAAAAABEREiIiIhERAAAAABERIjMzMzIhEQAAABESI0VWZlVDIREAABESNFbe//7WUyEQAAESNG7//////VMhEAESNG////////ZC"
"EQARI1/////////lMRABEk3//+d37//+UyEAETTv/+d3ff//5TIQARLf//13d+///VMhABEjVmd3d9///2QhEAESNEVmd+///+UyEQARIjRWd////+ZDIRAB"
"EiNGff///+dTIRABESNGfv///9dUIhEAESNG3////3dkMhEQESJG7////Xd1QyEQARI17///53d3ZUIRABEk3/////////0xEAEjXv////////9TIQASPf//"
"///////0MRABE9/////////+QhEAESNFZmZmZmZVQyEQABEjM0REREQzMiEQAAEREiIiIiIiIhEQAAAAAAARERIiIiIREQAAAAARESIzMzMyIREAAAAREiNF"
"VmZVQyERAAABEjRW3v/+1lMhEAABEjRu//////5TIRAAESTf///////9QhEAARNP////////5DEQABE9///Xd+///lMRAAESzd13d33//9UxEAARI0RWd3fv"
"//ZCEQABESM0V/////dUIRAAERIjRd///+d1MhEAEREiNF3////XUyEQAREiI0Xv////1TIRARIjNERWd+///lMhEBEkVVVVZ33//+UyEREj7//3Z3fv//5T"
"IQESTv//13ff///UMRARJO/////////mQhEBEj3////////2QyEQESNN///////mQyEQABEjRd7v/+1lQyERAAERI0RVZmVUMyERAAABESIzMzMzIhERAAAA"
"ARERIiIiIRERAAAAAAAAAAEREiIREREAAAAAAAAREiMzMiEREAAAAAABEiNFVUQzIREAAAAAERI17//1VDIRAAAAAREjXf//59/zIRAAAAESNG///3f/5CEQ"
"AAAREkbf//1+/9QhEAABESNX///nz//UMRAAARI0bv/+d+//dDEQABEiRt//937//3QxEQARI1f//3d///5kMhEBEjRv//x33//+ZDIRARJG7//Xd9///XUy"
"EQETTf/8d3fv//11MhERI17//////////kIRESNf//////////1CEREj3//////////9QhEBI9//////////9TIRARI0Vmd3ff//11QhEAERIzREVn7//3ZD"
"IRAAEREiIzRu//9kMhEAAAARERIjX//+QyERAAAAARERIjRVVDIREAAAAAAAEREiMzIhEQAAAAAAAAARERERERAAAAAAARESIiIiIiEREAAAAREiM0RERDMy"
"ERAAABEjRWZmZmZUMhEAABEjXv///////jIRABESRv///////+MhEAESNd////////1CEQARJG7///////9TIRABEkf//3d3d3dUMhEBEjXf/+d3d3dlMhEQ"
"ESNd//597/7XVCERARJG7///////5kMhEBEkb////////9UyEQESRv////////5jIRESNN///Xd+///2QhERI0Z3d3d33///ZCEREjXMx3d3d///9jIREST/"
"//d3d+///lMhERJO///nd+///2QxEBEk3////////+UyEQESNf///////+VCERABEkbv/////9VDIRAAESNF3u//7WVCIRAAABEjNFVmVUQyIREAAAERIiMz"
"MzMiEREAAAAAEREiIiIRERAAAAAAAAABERIiIiIREQAAAAARESIzMzMyIREAAAAREiNFVmZVQyERAAAREjRW3v/+5lMhEAABEjRu//////5TIRABEjRv////"
"///9QhEBESRu////////5DEQESNd///nd9///kMRARJG7//3d3fd3dQhERI13//+fe/+13VCEREjbv//7/////dUIRESRv/////////mQhERJH/////////+"
"ZCERI13///53ff///UIREjXf//93d37//3QhESNd///nd3fv//ZCERE13//+d3fP//9TIRESTP//93ff///VMhARJG/////////lQhEBEjXf///////mQyEQ"
"ARI17//////mQyEQABEiRW7v/+1lQyERAAARIjRVZmVUMiERAAAAERIjMzMzIhERAAAAABERIiIiIREQAAAAABESIiIiIiIiIREAAREjM0RERERDMiEQARI0"
"VmZnd2ZmVDIRESPf/////////0IRESTv/////////0IRETTv/////////kIRETT/////////5kIREST////////+dDIRESNFZ3d37//3UyEQARI0RWd+///G"
"QiEQAREjNWfv//1kMhEAABEiNX3//+ZDIRAAABEjRn///nUyERAAABEjV+///VQiEQAAAREkbv//5kMhEAAAARI1f///dTIREAAAARJG7//+ZCIRAAAAARNH"
"///9UyEQAAAAASNe///mQyEQAAAAESNe///lMhEAAAAAASPf///UMhEAAAAAASPf//9TIRAAAAAAARI0VVQyERAAAAAAAREiMzIhEQAAAAAAABEREiEREAAA"
"AAAAAAAAERESIiIhEREAAAABERIjMzMzIhEQAAAREjNFVmZUQyERAAERI0Vt7//tZDIRAAESNG7//////UMhABEjRv///////2MhABEkb////////+QhABE0"
"3//+d33//+QxARI17//nd3fv/9QxARI17//9d37//2QhARI13////////WQhARI1fv//////11MhESNGfv//////d1MhESNX////////91MhESRu///nd+//"
"/lMhEjXf//53d9///1MhEjXv//13d9///lMhEjX///53ff///VMREjXv////////5kIRESTf////////1DIRESNd///////2UyEQARI1be///tZUMhEQAREj"
"RVZmZVQzIREAABESIzM0MzIiERAAAAEREiIiIiEREQAAAAAAERESIiIhEREAAAAAERIjMzMzMiERAAAAESI0RVZmVEMhEQAAESI1be7//tZDIRAAESJF3///"
"///2QyEQARJF7///////9kIRARI13////////9QxEBEkb///13fv//5TIQETXf//53d3///mMhASNe///Xd33///ZCERI17///d3f///5kIREjXv////////"
"/mMhESNd/////////9UyERI0bf////7v//1TIREjRnfu/+d+//90IRESNXd3d3d9///mQhERJN///Hd3///9UyEREk3//+d37//+ZCEQESPP////////xTIR"
"ABI17///////1TIRAAESRe//////dTIREAARIkXe///tZDIhEAAAESI0VWZVRDIREAAAABESIzMzMyIREAAAAAARESIiIhEREAAAAAAAEREREREAAAESIzMy"
"IRAAESNEVUMiEQARJO///jIRARI17//+QhEBEjXv//1CEQESRv///UIRARJN///2QhEBEj3//+UyEQESNGZmVCIRERI0VlVDIRARI0VmZUMhEBEjz///wyEQ"
"ESTf//9TIRARNe///lMhEBI17//+UyEAETTv//1CEQARJP///TIRABEjRFVDIREAARIjMyIREAABEREhEREAAAAAAREREREQAAABEiMzMiEQAAESNEVUMiEQ"
"ABEk7//+MhEAESNe///kIRABEjXv//1CEQARJG///9QhEAESTf//9kIRABEj3//+UyEQARI0ZmZUIhEBESNFZlQyEQARI0VmZUMhEAESTP///DIRABE03///"
"UyEQESNe///lMhEBEjXv//5TIRARI17//9UyEQESNf///UIRABEjV9/+UyEQARI1f/5kMhEAESNe/9UyEQAAEj3/5TIREAABEu/0MhEQAAARIzMyERAAAAAR"
"EiIREAAAAAAREREQAAAAAAAAAAAAAREREQAAAAAAAAEREREREQAAAAABERERIiIiEQAAABERESIjNEMyEQABEREiIzRVbv0hEAEREiNEVW3//0IRAREiNFZt"
"////9CEQESNFbe/////9MhERI0zv/////WVCERESTf////52VDMhEBEk7//+d3ZUMyERARNe///nd2VDIhEQEST////+12VDIRABEjXv/////mUyEQABEkVt"
"7/////wxEAARIjRWfv///9MhAAAREiNFVt//9DEQAAARESIzRVbvQhEAAAARERIiM0QyIRAAAAABERESIiIhEAAAAAAAEREREREQAAAAAAAAAAEREQAAAAAB"
"ERIiIiIiIhERAAABEiMzREREMzIhEAARI0VWZmZmZVQiEAARNP////////8yEQESNf////////9CEQESTf////////5CEQESTf////////1CEQESRXd3d3d3"
"d2UyEREjRnd3d3d3d2QyEREjX/////////QyEREj3////////+QxEBEj3////////+QhEBEj7////////9MhEAESNFVmZmZmVDIRAAERIjM0RERDMyERAAAR"
"ESIiIiIiIREQAAAAABEREQAAAAAAAAABEREREREAAAAAAAERIiIiEREQAAAAABEjREMyIhERAAAAESPv1lRDMiEREAABEk7//sZUMyIREAARJP////1lQzIR"
"AAESTv/////nVDIRABEjRt/////+1CEQARIjRnfv////0yEAESI0Vnd97//8MhAREiNFZ3ff//9TIQESI0Vn3/////QxEBEjVt/////+xTIRASNP/////tZU"
"MhEREj7////WZUMyERARI+//5mVEMiIREAASP+1VRDMiEREQAAESMzMyIiEREQAAABESIiIREREQAAAAABEREREREAAAAAAAAAERERAAAAAAAAAAABEREiIi"
"IREQAAABESIzMzMyIREAABEiNEVWZVQyERABEiNW3v/+1kMhEBESRd//////1DIRESNe///////2QhERNN////////5CERI1///3ff///lMREj3//9d37//9"
"QxESPv//d33///1CEREjRWd3f///5TIRESI0Vnf///5kIRABEiNG3///1kMhEAERI0z///dkMhEAABEjTf//dkMhEQAAESNG3ddkMhEQAAARI1Z3dlMhEQAA"
"ABEk3//+QyEQAAAAESTf//5DIRAAAAARNO///UIRAAAAABEk7//1MhEAAAAAESP///QyEQAAAAARIzRUQyEQAAAAAAESIjMiEQAAAAAAABERERERAAAAAAAA"
"AAABEREREREREAAAAAAAABERIiMzIiEREAAAAAABESIzREREMiERAAAAAREiNE3v/+1UMhEQAAABEjNO//////1DIRAAABEjTP/tZmZu/+QhEQABEiTf9mVV"
"VVbf9TIRAAESNf5mZVVVVW3+QhEAESNO9mZnd3ZmZv5DIQARJN/Xd97//sdm71MhARI073ff/////XfvUyEBEjX+d///////d+9TIQESTfd9//13//5371Mh"
"ESNO92d3d3f//nfuUyERI1/nd33v///+d/5DEREjX9d9//////13/UIRESTf13//937//X31MhERJN937/93fv/3fuUyEREk33fv/8fv//ff1CEQESTfd+//"
"//////5TIRARJN/H7///7v//9UIRABEk39d+/+137/1UIhEAESNf93d3dmZmVDIhEAABE07+d2ZmVUQzIREAAAESNf/tZmZdMyIREAAAAREkX/////4yEREA"
"AAAAESI03v/+0yERAAAAAAABEiM0REQyIRAAAAAAAAAREiIjIiERAAAAAAAAAAERERERERAAAAAAAAAAAAAAEREiIiIREAAAAAAAAAERIzMzMyIRAAAAAAAA"
"ERI0VmZVQiEQAAAAAAERI17////UIRAAAAAAARI1z////+UyEQAAAAARIkbv////5jIRAAAAABEjXf/////mQhEAAAABEjR+//////dDIRAAAAESNd///v//"
"/VMhEAAAESNH7//37//9UyEQAAERJG3//+fv//5kIRAAARI1f///d+///mQhEAAREkbf//533//+dDIQABEjV////Xff///FMhEBESRu///nd9///9UyEQES"
"NX//////////1jIRARJG7//////////mQhERI1f//////////+ZCEREjXv//////////5kIRESTf///Xd3d+///9QhERJO///3ZVVn7///1CEREt///9VDM0"
"Xf///jIRESNFVVQzIiM0VVVDIRABEiMzMiIREiIzMzIREAARESIhERERESIiEREAAAAREiIiIiIiIiEREAAAESIzREREREQzIhEQABEjRWZmZmZmZVQyEQAB"
"Ek7////////+ZDIRARI17//////////EIRARJG///////////lMhARNH///////////lMhESNd///9d3ff///lMhESNd///9d3d+///lMhESRu///3d3fv//"
"90MhESR///////////1kIRETR//////////ndTIREjV//////////9dTIREjXf//////////1TIREkbv//93d3z///5TIREkbv//93d3fv//9jIREkf///93"
"d3f///5TIREkf///53d37///5TIQE1z///////////ZCEQE03//////////9UyEQEk7//////////VQhEAEj/////////+ZUIhEAESRVZmZmZmZVQyIRAAES"
"IzREREREQzIhEQAAAREiIiIiIiIiEREAAAAAAAARERIiIiIhERAAAAAAERIiMzMzMyIREAAAABESM0VWZmVUMiEQAAARIjRW3v//7WVDIRAAESI1bv//////"
"5UIREBESNd/////////lMhEBEjXf/////////9QhERI0b////Xd+///+UxERI17///12Z37//+UxESNH///+dlVW7//+QxESNd///9dURF7u7tQhESRu///3"
"ZDM0VmZUMhESRv///nVDM0RVVDIhESR////nVDNEVmVDIRATTP///nVERW3u7UIRASR////nZVVu///kIRASRv///3dmd////UIRASNv///+d33///5TIRAS"
"Nd//////////1CEQARJG7////////9UyEQARI0bv//////5lMhEQABEjRW3v//7WVDIREAAAESI0VWZmVUMyERAAAAEREiMzMzMyIhEQAAAAABERIiIiIhER"
"EAAAAAAAERIiIiIiIhEREAAAABEiM0REREMzIhEQAAARI0VmZmZmZUQyERAAARJO//////7tZDIREAESNe/////////VMhEAESRv/////////9QyEQETR///"
"///////+ZCEREjXf///Xd9////1CEREjXf///Xd33///1TIREkbv//93Zn3///5TIREkb///92Vnf///5TIRE0f///52VX3///1TIRI1f///51VX3///1TIR"
"I13///11Vn7///dTERI27///d1Vn7//+ZCERJG7///d2Z9///9UyERJH////d3d////3QyERJH///+d33////VMhEBNc//////////5kIREBNN/////////+"
"ZDIRABJO/////////VQyEQABI///////7sZUMhEQABEkVWZmZmVUQyIREAABEiM0REREMzIhERAAAAERIiIiIiIhEREAAAAAAAAREiIiIiIiIiERAAABEiM0"
"RERERDMyIRAAESNFZmZmZmZlVDIRABEk7//////////SEQESNe//////////QxEBEkb//////////0MRARNH//////////5CEREjXf///Xd3d3ZUMhERI13/"
"//13d3dlQyEQESRu///3d3d3ZUIhEBEkf/////////5CERARNH/////////9QhEAEjV//////////TIRABI13/////////UyEQASRu///3d3d3ZUIREAEkbv"
"//93d2ZVQyERABJH////d3ZmVUMhEQASR////nd3d3ZUMhEAE1z//////////jIRABNN//////////0yEQASTv/////////9MhEAEj//////////9DIQABEk"
"VWZmZmZmVUMhEAARIjNEREREQzMiERAAAREiIiIiIiIiEREAAAAAERIiIiIiIiIREAAAESIzRERERDMyIRAAESNFZmZmZmZVQyEQARJO/////////0IRARI1"
"7/////////QxEBEkb/////////9CEQETR//////////kIRESNd///9d3d2ZUMhERI13///13ZlVDMhEBEkbv//93dmVEMhEQESR////3d3dlQyEQARNH////"
"/////jIRABI1f////////9MhEAEjXf////////wyEAASNu////////9DEQABJG7///d3dmVDIRAAEkb///92VEMyIRAAASR////nVDIiEREAABNM///9ZDIh"
"ERAAAAE03///1TIREQAAAAASTv//9kIhEQAAAAABI////1MhEAAAAAAAESRFVUMhEQAAAAAAARIjMzIhEQAAAAAAAAEREiEREQAAAAAAAAAAAAABEREiIiIi"
"EREAAAAAARESIzMzMzMiEREAAAAREiNEVWZmVUMiEQAAAREjRWzu///tZUMhEAAREjVt///////+VCERAREjV//////////lMhEBEjV///////////5CEREj"
"Rv///+d3fv///lMhESNe///9d3d37///UyERNG7//+d2Z3fN3d1TIRI13///13Z3d3d3dkMhEkbv//93d//////+QyESRv///nd9//////1TIRJH///+d33/"
"/////VMRE0z///53fv/////3QxESR////nd3d37///dCERJG////d3d33///5kIREkb////nd37////WMhESNd///////////9UyEREkbv//////////dDEQ"
"ESNG7//////+7/9kIRABEjRW3v//7XfP/kMhEAERIzRVZmZVVVVUMhEAABESIjM0QzMzMzIhEAAAABERIiIiIiIiEREAAAAAEREiERERERESIREQAAARIjMz"
"IhEREiMzMiEQABEjRVVUMiEiNFVUMhEQARJO///0MiI17//+MhEBEjXv//9UMjRv///kIRARJG////ZDNF3///1DEQETR////nVEVu///3QhERI13///11VW"
"fv//50IRESNd///9d2Z37//+ZCEREkbv//93d3d////mQhERJH////////////1jIRETR////////////9UyERI1f///////////91MhESNd///////////+"
"dDEQEjbv///////////mQhEBJG7///d3d3f///5kIRASRv///3dmd9///9UyEQEkf///52VWfv//91MhEBNM///9ZERX7//+dDEQATTf///VQzRu///mQhEA"
"Ek7///ZDI0X///5TIRABI////1MiI9///9QhEAARJEVVQyISI0VVRDIRAAESIzMyIRERIjMzIhEAAAEREiERERERESIREQAAAAAAEREiIREQAAAREiMzIhEQ"
"ABESNFVUMhEAARI9///9MhABEjTf///DIRARI17///ZCEQESRv///mQhEBI0f///5jIRESNd///9UyEREjXf///VMhERJG7///dTIRESRu///3QxEBE0f///"
"5kIRASNX///+ZCEQEjXf///VMhEBI23///1TIRASRu///3QyEQEkb///5kIRABJG///+ZCEQASTP///VMhEAEk3///1DEQABI+///1MhEAARI0VVQyERAAER"
"IjMyIREAAAEREiEREQAAAAAAAAAAAAEREiEREQAAAAAAAAESIzMyIRAAAAAAAAESNFVVQyEQAAAAAAARJO///0MRAAAAAAARI17///UyEAAAAAABEkb///9T"
"IQAAAAAAESR////lMhAAAAAAESNc///9UyEAAAAAARI13///1TIQAAAAAREkbv//91MhAAAAAREjRv///3QxEAARERESNX////ZCEQARERIiM1f///5kIRAR"
"IiMzM0bf///VMhERIzRVVEV9///8UyEREk3u7mZn7///dCEQEjX///d3f///9kIRASNf///Xfv///VMhEBI1/////////mQhEAETTv///////2QyEQARI17/"
"/////mUyEQABEiRc7v/+51QyERAAARIjRVZmVUMyERAAAAERIjMzMzIiERAAAAABERIiIiIRERAAAAAAABERIiERERESIiEREQAAARIjMzMiISIzMzMiEQAA"
"ESNFVVQzIjNFVVRDIRAAESTv//9UM0Xf///+IRABEjXv//9lRW3////kIRABEkb///92Z9////5TIRABE0f///53ff///+ZCEQARI13///133////mQyEQAR"
"I13///19////5lMhEAARJG7///ff///+dTIREAARJH////3////nZDIRAAARNX/////////XVCIRAAASNX/////////nVCERAAASNd/////////9ZCERAAAS"
"Nu/////////+dDIRAAASRu/////t////dTIRAAASR/////13////5kIRAAASR////3d33///90MRAAATTP///XZnf////VMhEAATTf///WVFfv///lMhEAAS"
"Tv//9lQ0Xf///9MhEAASP///9UMzRf///+MhAAARJEVVQyIiNEVVVDIRAAARIjMzIiERIjMzMyERAAABERIiERERERIiIREQAAAAABERIhEREAAAAAAAESIz"
"MyIRAAAAAAARI0VVVDIRAAAAAAESTv//9CEQAAAAARI17///UyEAAAAAESRv///1MhEAAAABE0f///5TIRAAAAESNd///9UyEQAAABEjXf///VMhEAAAARJG"
"7///dTIRAAAAESRv///3QyEQAAABE0f///50MhEAAAASNX///+ZDIREQAAEjXf///WQyIREQABJG7///d1QzIhERASRu///3dlRDMiEQEkf///93d3ZlQyER"
"JH//////////QhETXP/////////lMRE03/////////5TERJO/////////+QxESP//////////TIRESRVZmd2ZmZVQhEREiM0RERERDMyIRABESIiIiIiIiIR"
"EAAAABERIiIiIREREiIiIhERAAAREiM0QzMiIiIzNEMyIRAAARI0VmZlVDIjRVZmZUMhEAESNO/////UM0Xv////QhEAESRv/////VRG3/////UyEBEjXf//"
"///mVn/////+UyEBEjXf/////nZ3/////+UyEBEkbv/////nd+/////9UyEBEkbv/////nd//////8UyERE0fv/////nfv/////3UxERI1f//+///3f//+//"
"/3QhERI13//+7//33//v///mQhERJG3//+3//t7/7f///WQhERJG7//93//u//3f///VMhERJH7//33/////ff//91MhESNH///33////+fv//90MRASNd//"
"/3f////3f///5kIRASNd///nf////nf///5jIRASNe///nf////X3///1TIRASNe///Xfv//533///1DIRASNe//93fv//137///ZCEQARJP//9lbv//ZW//"
"//QyEQARI0VVVERVVVRERVVUMhEAABEiMzMyIzMzMjMzMyIREAABEREiIhIiIiISIiIhERAAAAAAEREiIREREREREREQAAARIjMzMiEREiMzIhEQABEjRVVV"
"QyIiNEVUMhEQARJO///lQyI07//+MhEBEjXv///WQzRe///kIRARJG////92RFf///1DEQETR/////52Vn///3QhERI13/////13ff//5kIRESNt//////d3"
"3//+ZCEREkbv/////+d+///mQhERJH///////9f///1jIRETR////////3///9UyERI1f///////////91MhESNd///u///////+dDEQEjbv//5+///////m"
"QhEBJG7//+d+//////5kIRASRv///Xff/////9UyEQEkf///d3fv////91MhEBNM///nZmf////+dDEQATTf//5lRW3////mQhEAEk7//+VDRW7///5TIRAB"
"I////UMjNG///9QhEAARI0VUQyIiNFVVVDIRAAESIzMyIRESIzMzIhEAAAEREREREREREiIREQAAAAAAAAERESIiIiIREQAAAAAAEREiMzNDMzIhEQAAAAAR"
"EiNEVWZmVEMiEQAAABESNFbe7//+1lMiEQAAERI1bf///////VQhEQAREjV//////////VMhEAESNX///////////EMRARI0b////nd87///5TIRESNe///9"
"d2Z3///+ZCERE0bv//52VVZ+///2QhESNd///9dURGfv//9kIREkbv//92RERn////ZCERJG///+dUM0Z////mQhESR////nVDRGz///5TIREkf///52REV9"
"///9UyERJH///+dlVWfv//9kIRASRv///3dmd+///9UyEQEjXv///nd37///5kIREBI03//////////2QyEQARI17/////////ZTIREAESJF3///////1kMh"
"EQAAESJFbe7//u1lQyERAAAAESI0VWZmVUQyIREAAAAAERIjMzQzMyIREQAAAAAAEREiIiIiEREQAAAAAAAAERIiIiIiIiEREAAAARIjNEREREMyIREAABEj"
"RWZmZmZlVDIREAARJO///////+ZTIRABEjXv////////1DIRARJG//////////ZCEQETR//////////9QxERI13///13fv///lMhESNd///9d33///1TIREk"
"bv//93d9///9UyERJG////d3fv//90IRETR////nd9////ZCERI1f//////////VMhESNd/////////+UyEQEjbv////////1UIhEBJG7///////5lQyEQAS"
"Rv///3d3ZlQyIRAAEkf///52VEMyIREQABNM///9ZTMiIREQAAATTf///VQiERERAAAAEk7///ZDIREQAAAAABI////1MhEQAAAAAAARJEVVQyERAAAAAAAA"
"ESIzMyIREAAAAAAAAAEREiEREQAAAAAAAAAAAAABEREiIiIiEREAAAAAARESIzMzMzMiERAAAAAREiNEVWZmVEMiEQAAAREjRW3u///tZTIhEAAREjVt////"
"///9VCERAREjV//////////VMhEBEjV///////////xDEREjRv///+d3zv///lMhESNe///9d2Z3///+ZCERNH7//+dlVWfv//9kIRI13///11REZ+///2Qh"
"Ekbv//92RERn////ZCESRv///nVDNGf///5kIRJH///+dUNEbP///lMhEkz///52REV9///9UyESR////nZVVn7///ZCERJG////d3Z37///5TIREjbv///n"
"d37///5kMhESNd///////////FMhEBEjXv/////////GQhEQESJF3///////52QyEQABEjRW3v////93UyERAAARIjRWd+///+dTIRAAAAERIjRWf////lMh"
"EAAAABERIjRd////0yEQAAAAARERI0Xd3d3SEQAAAAAAARESI0REQyIRAAAAAAAAARESIiIiIRAAAAAAAAAAARERERERAAAAAAAREiIiIiIiIhERAAAAESIz"
"REREREMzIREAABEjRWZmZmZmZUMiEQABEk7////////tVCERARI17/////////5TIRARJG///////////UMRARNH///////////lMhESNd///9d3ff///mMh"
"ESNd///9d3d+///mMhESRu///3d3d////lMhESR////3d3fv///UMRETR///////////5kIREjV//////////+ZDIREjXf/////////XUyEQEjbv////////"
"12QyEQEkbv//933///91QhEQEkb///93f///91MhEAEkf///53fv///mQhEAE0z///13ff///mQhEAE03///1lbP///9QhEAEk7///dUVu///9QhEAEj////"
"VDRe////MhEAESRFVUMyM0VVVEIRAAESIzMzIiIiMzMyIRAAARESIhERERIiIREQAAAAAAARERIiIiIhERAAAAABERIjMzQzMiERAAAAERIzRVZmZVQyIRAA"
"AREjRW3v//7WUyERABESNW7//////+UyEQARI13////////+QxEAESRv/////////1MhARI17///53fv///TIQESRu///3d3f///0yEBEkb////td3d3dkMh"
"ARJG///////td3VCEQESRv////////51QhEBEjXf////////5kIRERI1ff////////1DEREjRXd33v/////+UxERI1zd3Hd37////lMRESTv//93d37///1C"
"EREk7///13fP///mQhERJN//////////1TIRESNf/////////VQhEAESRu///////9VCIRABEjRW3v//7sZTIhEAABEiNFVmZmVEMiEQAAARESIzNEMzMiER"
"AAAAABERIiIiIhEREAAAABERIiIiIiIiIiIREAABEiM0RERERERDMiEQARI0VmZnd3dmZlVDIRARNO///////////0IRESNf///////////0IRESTf//////"
"/////kIRESTf///////////UIRESPv///////////TIRARJFZ3fv//93dmUyEQARIjRXfv//53VEMhEQABEiNGf///51QyIREAABESNG3///1kMhERAAAAES"
"JG3///1kIhEQAAAAERJG7///dTIREAAAAAERJG7///dDIRAAAAAAERNH///+ZCEQAAAAAAESNX///+ZCEQAAAAAAESNd///9UyEQAAAAAAESNd///9UyEQAA"
"AAAAASNe///2QhEAAAAAAAARNe///lMhEAAAAAAAARJP///kMREAAAAAAAARI0VVVDIRAAAAAAAAABEiMzMiEQAAAAAAAAAAEREiEREAAAAAAAAAABERIhER"
"EREREiEREAAAESIzMyIRERIjMzIhEAARI0VVVDIhIjRVVDIREAESTv//9DIiNe///jIRARI17///UyI0b///5CEQESRv///2QzNd///9QxEBE0f///5kM0bv"
"//90IRESNd///9ZDNG7//+dCEREjXf///VQzV+///mQhERJG7///dUNFf///5kIRESRv///3VDRt///9UyERE0f///51REbf///VMhESNX///+dURX7///dT"
"IREjXf///XVEV+///mQhEBI17///d1RWfv//5kIRASNe///3dlZ9///9UyEQEjXv///Xd3f///9kIRABI13////Hff///+UyEQARJG//////////5kIREAES"
"Nd/////////mQyEQAAESNd///////9ZDIREAABEiNW3u//7tZUMhEQAAABESNFVmZlVEMiERAAAAABESIzM0MzMiEREAAAAAABERIiIiIhEREAAAAAAAEREi"
"IRERERESIRERAAESIzMzIhERIjMzIhEAESNFVVQyIiI0RVVDIRARJO///1MiI03///0hEBE07///1DM0bv//9DEQEjXv///VQ0V////UMRASNd///+ZUVu//"
"/mQhEBI13///51Vn///9UyEQETR////nZn7//+ZDIRARJH7///d3f///1TIRABEkbv//93fv//9kMhEAESRu///3d////VMhEAARI13///d9///mQyEQABEj"
"Xf///X///9UyEQAAESNX///93//+ZDIRAAABEkfv//7///xTIRAAAAESRu//////5kMhEAAAARI27/////91MhEAAAABEjXf/////mQhEQAAAAESNd/////3"
"UyEQAAAAABEkb////+VCERAAAAAAESNP////ZDIRAAAAAAABEjRWZlVDIRAAAAAAAAERIjMzMyIRAAAAAAAAABERIiIiERAAAAAAAAARESIiERESIiIhEREi"
"IREQABESIzMzIiIzMzMiIjMzMiEQARI0VVVDM0VWZUQzRFVUMhEBEj3///5UVu///1RF7///MhERJN///+Zm3///92Z////kIRESRv///nd+////d33///ZC"
"ERE0f///53f////nd+///lMhERNH///+d9////53f///1TIRETR////nfv////d9///mQhEBE0fv//53/////3fv//1kIRARNH////fv////99///3UyEQES"
"R////n///f//fv//5kIREBEkf///7f/+f//t///9UyEQARJH7//+7/93//7v//50MhEAESRu/////+d//////9ZCEQABEkbv/////Xf/////91MhEAARJG7/"
"////d3/////+ZCEQAAESNu/////nd//////VMhEAABEjXf////13f////+ZCERAAARI13////nZn7////VMhEAAAARNN////1lVu////ZCERAAAAESPf///2"
"VEX////lMhEAAAABESNVZlVDMzRWZlQyERAAAAABEiMzMzIiIjMzMyIREAAAAAABERIiIhEREiIiIREQAAAAAAAAARESIiIhEREiIhERAAAAARIjMzMyIiIz"
"MzIREAAAESNFVlVEMzRVVUMhEAAAES3////VRFb////SEAAAETT////3Zn7///0yEAAAETTf///3d+///+QyEAAAESRv///+ff///lQhEAAAESNt///+3///"
"5kMhEAAAESNX////////dTIRAAAAESNG7//////3ZCIRAAAAESJGf//////WQyEQAAAAESJGfv////51QiEQAAAAESNGfv////11MhEQAAABESNX3/////51"
"MhEAAAABEjRt///////GQhEQAAARIkbf///////nQyEQAAARI1f////f///3UyEQAAESNG////d////+ZCEQAAESRu///9d9////xDEQAAEjXv///nZ3////"
"5TIQAAEj3///51VW7///9TIQAAEt////ZUNF3////jEQAAESNFVVQzIzRVZVQyEQAAESIzMzIiIiIzMzIhEQAAARESIiEREREiIiEREAAAARESIiERERESIh"
"ERAAARIjMzMiIRIiMzMiEQABIkRVVUMyIzRVVUMhEBEj7///1UM0Xv///iEQESTv///2VFbf///0IRARJG////12Z////2QhEBEjbv///nd+///9UyEQESNd"
"///+d9///9VCEQABEkbv///X///+ZDIRAAESNd///+////dTIRAAARI0f///////dTIREAAAESRt//////1kIhEAAAARI1f/////1kMhEAAAABEjR9////51"
"MhEQAAAAARJGf///92QhEQAAAAABEkZ////nUyEQAAAAAAESNt///9ZCERAAAAAAARI23///xTIRAAAAAAABEjbv//50MhEAAAAAAAESNe///mQhEAAAAAAA"
"ARI17//+UyEQAAAAAAAAETz///1CEQAAAAAAAAARI0VVRDIRAAAAAAAAAAESIzMyIRAAAAAAAAAAARERIhERAAAAAAAAAAABERIiIiIiIiIhERAAAAERIjNE"
"RERERDMyERAAABEjRVZmZ3dmZlQyEQAAASPf//////////4yEAABEj3//////////9MhAAARJO//////////5TIQAAESPv/////////mQhEAAAEjRWd3ff//"
"//dTIRAAABEjRWd8/////VQhEQAAARIjRXfv///9ZDIRAAAAERI0Z+////51MhEAAAABEiNX7////nUyERAAAAERI0bf///+dUIhEAAAABEjRt/////GQyER"
"AAAAESJFf////9dUMhEQAAABEjV+////12VDIREAAAESNG7////ndlRDIRAAABEkbv///+d3d2VDIRAAESNd///////////CEQABEk3///////////QxEAAR"
"JO///////////kIRAAESP///////////4yEQAAEjRVZmZ3ZmZmVDIhAAABEiM0REREREQzMiEQAAABESIiIiIiIiIiERAAAAAAAAEREiIRERAAAAARIjMzMi"
"EQAAABEjRFVVQyEQAAARJO////0hEAABEjXv///9MRAAARJG////9DEQAAESRv//dlMhEAARI13//nUyEQAAESNd//5kMhEAABEjXv/9UyERAAARJG7/91Mh"
"EAAAESRv//dDIRAAARI0f//2QhEAAAESNd//5jIRAAABEjXf/9UyEQAAARJG7//VMhEAAAESRu//dDIRAAARI0f//2QhEAAAESNd//5kIRAAABEjXf/+UyEQ"
"AAARI17//VMhEAAAESRu//dTIRAAABEkb//3UyEQAAARNH//91MhEAAAETTf/+dkMRAAABE03////jIQAAARJO////0yEAAAESPv///0MRAAABEiRFVVQyEQ"
"AAABEiMzMyIRAAAAABERIiIREQAAAAABERERAAABEiIhEQABEjMzIREBEj3/4yEQESPf/kIRARJN/+UyEREk3/9TIRESRv/lMhERJM/+UyEREk3/5TIRESRv"
"/2QhERJG//ZCEREkb/9kIRESRv/mQhERJG/+ZCEREkb/9kIRESRv/2QhERJG//ZCEREkb/9kIRESRv/mQhERJG7/ZCEREjXv9TIRESNf/1MhEBEk//QyEQES"
"Pd0yEQAREiMiERAAERERERAAAAEREQAAAAAAEREiIhERAAAAAREiMzMiERAAAAESNFVVQyEQAAARI+////0yEQAAESTv///9QhEAABEj////9kIRAAARI1d/"
"//ZCEQAAESNGf//mQhEAABESRt//5TIRAAAREkbv/9UyEQAAERJG7//VMhEAABESRv//dDIRAAARI0f//2QhEAAAESNd//5kIRAAABEjXf/9UyEQAAARI17/"
"/VMhEAAAESRu//dDIRAAAREkb//2QhEAAAESNX//5kIRAAABEjXf/+UyEQAAERI17//VMhEAABEiRu//1TIRAAARI0f//3QxEQABEjRX//9kIRAAARJFff/+"
"UyEQAAEj3////lMhEAABI+////1CEQAAASPv///1MhEAAAESNFVVQyERAAABESIzMyIREAAAABERIiIREQAAAAAAAAAREREREQAAAAAAEREiMzIhEAAAAAER"
"IjRVRCIRAAAAARIkXv/9QhEQAAARIkXv//5TIRAAAREjXf///2QhEQABEjRv////1TIRABESRu/+3v/mQhEAESNd//1+//ZCEQARJN//533//VMhARI1//9m"
"Z+//UyEBEj7/9lRF3//TIQES3//UMzRf/+MRABEjRDMiIzREMhEAERIiIhESIiIhEAABEREREREREREQABERERIiEREREAABEiIzMzMyIhEAARI0REREREMy"
"EQASP////////yEQAS3////////iEQARI0REREREMyEQAREiIzMzMyIhEAABERERIiEREREAABEREREQAAERIiIhEQABEjREMhEQAR3//+MhEAET3//0MhAB"
"Ek3//TIQARI03/8xEAARIjQzIRAAAREiIhEQAAARERERAAAAARERIiIiIREQAAAAERIjMzQzMiERAAABESNEVmZlVDIhEAAREjVt7//+5lQiEQARI13/////"
"/+UyEQESNO////////5TEQESPf////////9TIQESTv//53fv//9jIREiRnd3d3ff//5jIREjRnfe7/////1TIREkV+////////xTIRI03/////////dTIRI1"
"///+d3z///dTIRJG///3d8////dkIRJN///////////UIRJN///////////UIRI17////93///9TIREkXv/+13d+/+1CEREiNFZlVURVVVQyEQERIzMzMzMz"
"MzIhEAARESIiIiIiIhERAAAAAREREREQAAAAAAAAARIjMyIREAAAAAAAARI0VVQyERAAAAAAARE07//9MhERAAAAABEjXv//ZDIREQAAAAESRv//91QyIREQ"
"AAESNd///3ZUQyIREAARI13//+d3ZlQyERABEkbv//19//5kMhEAESRu///u////1DIRERNH7////////2QhERI1f/////////1CEREjXf///nff///VMRES"
"Rt///nd37//9UyERJG7//9d3fv//1TIREkfv//d3d+///EMRETR///93d3///2QhESNc///3d33//+YyERI13///533///1TIREjXv////////5kIRASNe//"
"///////EMhEBI17//e/////VMhEAEST//3fe/+1kMhEQARI0VVVVVlVEMhEQAAESIzMzMzMzIhEQAAABERIiIiIiEREQAAAAAAARESIiIiEREAAAAAEREiMz"
"MzMiERAAAAERIjRVZmVUMiEQAAERI0Vt7//uZUIhEAERI0bv/////+VCEQARI0bv///////lMhAREkb/////////UyEREjXv//93ff///TIRESRv///Hd37/"
"/9MhESNd///ndmZ3d2QyERI17//9dlVmd2VCIREjXv//12Zmd3ZTIRASNe///Xd3f//+MhEBI17///d33///0yEQETTf////////5TIRARI17///////9UIR"
"ABEjRu//////5UMhEAARI0Xe///tZUMhEAAAESI0VWZlVDIhEQAAARESIzMzMyIREQAAAAARESIiIiEREAAAAAAAAAAAAAAREREREQAAAAAAAAAREiIzIhEQ"
"AAAAAAABESM0VUMhEQAAAAABERIk3//+MhEAAAABEREiNe///UIRAAABERIiM0bv//1CEQAAERIjNEVn///3QhEAARIjRVZnd///5kIRABEiRW7//nz//+ZC"
"EQERJF3////u///mQhEBEjXf////////1jIRESNH/////////9UyEREjXv//93f///91MhERJG///3d33//+dDEQEjXf//53d9///mQhEBI17//9d3ff//5k"
"IRASNe///Xd37//9UyEQEjb///13d///91MhEBI17//+d3////dDEQASNe/////////mQhEAESTf////////5TIRABEjXv////3v/9QyEQAREjXe/+13///T"
"IRAAAREjRVZVVVVUMhEQAAAREiMzMzMzMyERAAAAARESIiIiIiEREAAAAAABEREiIiIhERAAAAARESIzMzMyIREAAAERIjRVZmVUMiEQABESNFbe//7WVCIR"
"AREjRu//////5UIRARI0bv///////VMhERJG/////////2QhESNe///3d9///9QhESRv//93d37//9QxEjXf/////////9QxEjXv/////////3QhEjXv////"
"/////2QhEjXv//13d3d3d1MhEjXv//53d+///lMhETTf/////////UIRESNe////////1TIRESNG7//////9UyEQARI0XO7//u1kMhEQABEiNFVmZlRDIREA"
"ABERIjM0MzMiERAAAAARESIiIiEREQAAAAABEREiEREQAAABESIzMyIREAABESNFVVQyEQABESNW7//tMhABESNd////0yERESNX/////TIRESNG7//+d1Mh"
"ERJGf///13UyERI1//////5TIREk3//////lMhASTf/////9QxEBJO//////1CEQEkZ+///XZDIRASNn///8ZTIRABI1f///dTIREAEjXf//51MhEAASRt//"
"/mQhEQABJG7//9YyEQAAEkb///1TIRAAASRv//90MhEAABJN///mQhEAAAEk3//+UyEQAAASPv//5CEQAAABEjRVRDIRAAAAERIjMyIRAAAAABERERERAAAA"
"AAAAAAAAAAABEREREREAAAAAAAAAEREREiIiIhEAAAAAAAEREREiM0REMhEAAAAAEREiIiM0Xv/+IRAAAAAREiMzREVs///TIQAAABEiNFVmZ33//+QxEAAA"
"ESI1be//////5kIRAAABEkXv///////HUyEQAAESNe////////1kIREAABEkz///x33//+ZCEQAAASNd///nd8///1MhEAABEjXf//53fv//5TIRAAARI1f/"
"///////mQhEAAAESRX3///////dTIRAAARI0bf/////+11QyEQAAESNd///Xd3d3ZTIREAABEkb///13d3d3VCIRAAARJH/////////mQyEQABEjV///////"
"///2QhEAARI1fv/////////UIRAAESRt//////////5CEQABI13//9d3d3///9QhEAASNf//93d3fP//9kIRAAEjX//////////9UyEQABE07/////////1T"
"IRAAARI17//////+1lMiEQAAARI0VmZmZmZVQyERAAAAERIjNERERDMyIREAAAAAEREiIiIiIiIREQAAAAAAAAEREREREAAAAAAAAAESIzMiERAAAAAAAAES"
"NFVUMhEQAAAAAAERNO///TIREQAAAAARI17//2QyEREAAAABEkb///dTMiEREAABEjXf//92VEMiERAAESNd///nd2ZUMhEQARJG7//9fe/+1TIRABEkbv//"
"7v///9QyEBEjR+////////9TIRESNX/////////2QhERI13///537///ZCEREkbf//53d///5kIRESRu///Xd9///mQhERJH7//3d33//+ZCERI0f///d3fv"
"//1TIREjXf//92d////VMhESNd///nZn///3QyERI17//+dmf//+ZCEQEjXv//1lbf//5TIRASNe//9lRe///lMhEBEk///1RE7//9MhEAESNFVUMzNFVUMi"
"EQABEiMzIiIiMzIhEQAAERERERERESEREQAAAAABERERERAAAAERIjMiERAAAREjRVQyEQAAESPf//0yEQARE07//9MhEAESNf//9TIRABEjX///UyEQARJG"
"d3d1MhEBEjTf//5TIRARI17//9UyEQESRu///VMhEBEkb///dDEQESNX///2QhEBEjXf//5kIRARI13//+YyEQESRu///VMhEBEkbv//dTIRARJH///3QxEA"
"ETXP//5kIRABI13//+UyEQARNe///VMhEAESTv//1CEQABEj///1MhEAARIkRVQyEQAAARIjMyIREAAAAREREREQAAAAAAAAEREREREAAAAAAREiMyIREAAA"
"ABESNFVDIRAAAAARI9///TIRAAABETTv//0yEQAAARI1///1MhEAAAESNf//9TIRAAABEkZ3d3UyEQAAESNN///lMhEAABEjXv//1TIRAAARJG7//9UyEQAA"
"ESRv//90MRAAARI1f///ZCEQAAESNd///mQhEAABEjXf//5jIRAAARJG7//9UyEQAAESRu//91MhEAARI1f///dDIRAAESNc///mQhEAAREjXf//5kIRAAES"
"JG7//9UyEQAREjV+///VMhEAESNGf///dDEQABEkV+///2QhEAASPP////5TIRAAEj3////9QxEQABI+////1TIRAAASPe//5lMhEQAAESNFVVQyERAAABES"
"IzMyIREAAAABERIiIREQAAAAAAABERERERAAAAAAAAAAESIzMiERAAAAAAAAARI0RVQyERAAAAAAABESTv//0yEREQAAAAARI17//2QiERERAAAAESRv//9k"
"MiIhEREAARI13///dUMzMyIRAAESNd///nVVVVRDIRABEkbv//12Z////iEQARJG7//9d3///+QhEBEjR+//93fv//5TIRARI1f///d////mQyEQESNd///3"
"///+dTIRABEkbf///v//51QiEQARJG7//////9dTIREAESR+///////nUyEQABI1f///////91MhEAASNd////////1kIRAAEjXf///n///+ZCEQABI17//+"
"d9///9UyEAASNe///Xd////lMhAAEjXv//dmfv//9TIQABEk///1RFz///4yEAARI0VVRDM0VVVDIRAAARIjMzIiIjMzIhEQAAERESIREREiIhERAAAAAAER"
"EREREAAAAREiMyIREAABEiNFVDIRAAARJN///TIRABEjXv//1CEQARJG///2QhEAESRv//9kIRARI13//+ZCEQESNd///mMhEBEjbv//1TIRARJG7//9UyEQ"
"ETR///91MhERI1f///ZCEQESNd///mQhEBEjXf//5jIRARJG7//9UyEQESRu//91MhEBEkf///dDEQARNc///mQhEAEjXf//5TIRABE17//9UyEQARJO///U"
"IRAAESP///UyEQABEiRFVDIRAAABEiMzIhEQAAABERERERAAAAAAEREiIiIiIiIiIiIhERAAAAESIzMzMzMzMzMzMzIhEQAAESNFVVVVVmVURFVVVDIREAAR"
"JO//533v/tZnzv/tUyEQARI17//d/////X7////UMRABEkbv//////////////UyEREjR///////////////9kIRESNd///+d+///+d+///2QhERI23//+d3"
"3//+d3f//+ZCEREkbv//13ff//53ff//5kIRESRu//93d+///Xd+///mQhESNH///3d37//8d37//9UyERI13///d3f///d3f///1TIREjXf//52d///52d/"
"//90MhESNe///nZt///nZs///mQhEBI17//9ZW7//9ZW3//+UyEQEjXv//ZVXv//1VXv//1TIRARJP//9URP//9URP///TIRABEjRVVDMzRVVEMzRVVDIhEA"
"ARIjMyIiIjMzIiIjMyIREAABEREREREREiEREREhEREAAAAAEREiIiIiIhERAAAAESIzMzMzMzIhEQAAESNFVVVVVVVDIREAARJO//533v/tUyEQARI17//c"
"/////UMhABEkbv////////UyEREjR/////////9kIRESNd///+d+///2QhERI23//+d3f//+ZCEREkbv//13ff//5kIRESRu//93d9///mQhESNH///3d37/"
"/9UyERI13///dmf///1TIREjXf//52Z///90MhESNe///nZn///mQhEBI17//9ZW3//+UyEQEjXv//ZUXv//5TIRARJP//9URO///TIRABEjRVVDMzRVVDIh"
"EAARIjMyIiIjMyIREAABEREREREREhEREAAAAAAAEREiIiIhERAAAAABERIjMzMzIhEQAAABESI0VWZlVDIhEAABESNFbe//7WVCIRABESNG7//////lMhEA"
"ESNG7///////1TIRERJG/////////2QhERI17///d33///1CEREkb///x3d+///UMREjXf//53d37//9UxESNe///Xdnfv//xCERI17//9d3d///9kIREjXv"
"//13d9///lMhESNe///nd9///9QyERE03////////+UyEQESNe///////+ZCIRARI0bv/////9VDIRAAESNF3v//7WVDIRAAABEiNFVmZUQyIREAAAEREiMz"
"MzMiEREAAAAAEREiIiIhERAAAAAAAAEREiIiIiIhERAAAAARIjMzMzMzMiERAAABEjRVVVVVZVQyERAAARJO//537//mQyEQABEjXv/93////9QyEQARJG7/"
"///////2QhEBEjR//////////UIRARI13///533///1TEQESNt///nd37//9UyEBEkbv//13d+///VMhARJH7//3d3fv//xDEREjV///93d3///2QhERI1z/"
"//d3ff//5jIRESNd///+d9///9UyEREkbv////////5kIRARJG7////////8QyEQETR+///v////1TIRABI1f///fe/+1kMhEQASNd///3dmZVQyERAAEjXf"
"//51REMyIREAABI17//9ZDIiIREQAAARNO///UMiEREQAAAAEST///UyEREQAAAAABEjRFVDIhEQAAAAAAABEiMzIhEQAAAAAAAAABERERERAAAAAAAAAAAA"
"ARESIiIiIiEREAAAAREiMzMzMzMyIRAAARIjRVZVVVVUQiEQARIkVu//533//jIRAREkXf////3v/+QhEBEjXf////////1TEREjR//////////VMRESNe//"
"/3d////3QxERJG///3d33//+ZCERI13//+d3ff//5kIREjXv//13d9///mQhESNe///Xd37//9UyERI2///9d3f///dTIREjXv//53f///91MhESNe//////"
"///nQhEBEk3////////+ZCEQESNe////7///1jIRAREjXe/+1////VMhEAERI0Vmd3///3UyEQABESM0RWf//+ZCEQAAARESI0Xf//5jIRAAAAEREiNe///l"
"MhEAAAAAERIk7//9QhEAAAAAABESNEVUMiEQAAAAAAABEiMzMhEQAAAAAAAAAREREREQAAAAABERIiIiEREQAAESIzMzMzIhEAARI0VVVVVEMhEAESTv/+d+"
"/9IRARI17//d///DIQESRu//////UyERI1f//////1MRESNd//////5CEREjbf///ndUMhERJG7//+dlQyEQESR+///XUyIREBI0f///dUIhEQASNd///3Uy"
"ERAAEjXf//5kIREAABI17//+UyEQAAASNe///VMhEAAAEjXv//ZCEQAAABEk///0MhEAAAARI0VVQyEQAAAAARIjMyIREAAAAAEREREREQAAAAAAAAEREiIi"
"IhERAAAAAREiMzMzMiERAAABEiNFVmZVQyIRAAESNFbe///sUyEQAREkbv/////+UyEQESNe///////+QhEBEk3////////0MRASNe///XfP//9DEREjXv//"
"53d3d2QhERI13/////7XdUIRESNX3//////9ZCEREjV3fN7////2QhERJN3d13d+///UIRETX///d3f///1CERE1/////////lMhERJO////////1DEQESNe"
"///////VMhEAESNW3v/+7WQyERABESNFVmZVRDIREAABESIzMzMzIhEQAAABERIiIiIRERAAAAAAAREREREQAAAAAREiIyIREAAAAREjREQyERAAAREjTv/9"
"MhEAAREjVv//ZCIRABEjRu//91MhEBEjRn///nZDIQESTf/////+MhERNe//////1CERI17//////DIREjX//////1MhASNXz///d1QhEBE0bf//51QyEQES"
"Ru///WQyEQARJG7//9ZDIRABE0f///dkIhEAETR///92QiEQARNd///nZDIRABE03///7tMhEAESTf////0yEQARI8/////TIQABEjRe///kMRAAARI0RVVU"
"MhEAAAESIzMzIhEAAAABERIiEREQAAAAABEREhERERERERAAABEiMzMiIiMzMiEQABEjRFVEMzRFVDIhEBESTv//1ETf//4yEQESNe///VVd///kIRARJG//"
"/3VW7//9QxERI1z///dmf///1DEREjXf//52Z///90MRESRu///nZ3///mQhERJG7//9d33//+ZCEREkfv//d3fv//5kIRETV///93d+///VMhESNd///3d3"
"///3UyERE13///13////dDEQETTf////////5kIRARJN/////////lMhEBEjXv///+fv/9QyEQARJF3//nd///0yEQABEiNFVVVVVVQyERAAAREiMzMzMzMy"
"ERAAAAEREiIiIiIhERAAAAERESIRERERERERAAESIzMyIiIjMyIREBEjRVVUMyNEVUMhEBEk///9Q0TP//4xEBI17//9ZVbv//0yEBI17//+dmf//+UyEBI1"
"7//+d37//2QhEBI13//+d3///lMhEBI1f//+d9//90MhEBE0f//+d+//1kIRABEkbv//ff//dDIRABEkbv//fv/9ZCERABEjXf/////3QyEQABEjXf/////V"
"MhEAAAETR/////5kMhEAAAESRu////1TIRAAAAESNe///+ZCERAAAAARJO///2QyEQAAAAARI0VWVUMhEAAAAAABEiMzMyIREAAAAAAAEREiIhERAAAAAAER"
"ESIhEREiIiERERERERAAESIzMyIiIzMzIiIjMyIREBEjRVVUMzRVVVQzNEVUMhEBEk///9VFb///1URf//4xEBI17//9Zm3///1lbv//0yEBI17//9d37///"
"13fv/+UyEBI17//+d9////533//2QhEBI13//+d+////53///lQhEBI1z//+d/////58//91MhEBI1f//+ff////5+//5kIRABEkfv/+3v/e//7e//dTIRAB"
"Ekbv/+7/99//7v/+ZCERABEkbv////99/////3UyEQABEjXf////18/////mQhEQABEjXf///+d3////51MhEAAAESRv///9d3////1TIRAAAAESNe///3Zn"
"7///ZDIRAAAAERNO///lRF///9QyEQAAAAARI0VVVEM0RVVUMhEQAAAAABEiMzMyIiMzMyIREAAAAAAAERIiIhERIiIRERAAAAAAAAABERIiIRESIhERAAAA"
"ARIjMzMiIzMzIhEAAAESNFVVVERFVUQyEQAAES3///5lVv///iEQAAESTv//93fv//5CEQAAESTf///X3///UyEQAAESRu///9///2QyEQAAESNX//////91"
"MhEAAAESNX7/////1kMhEAAAESNXz////9dTIREAABESNX3////3dTIRAAABEjRn/////9dTIRAAABEjV///////dTIRAAARI0b///7///1kIRAAARJF7//+"
"fv//5kIRAAASNO///nff///UMRAAASPf//92Z+///0MRAAAS3///1VVd///9IRAAARI0VVRDNEVVVDIRAAARIiMzMiIjMzMiEQAAABEREhERERIiEREQAAAA"
"ABERIhEREREREREAAAESIzMyIiIjMyIREAARI0VVVDMjRFVDIRAAEST///1DNM///jEQABE07//9VVbv//0yEAASNe///nZn///lMhAAEjXf//53fv//ZCEQ"
"ABE0f///d3///lMhEAARJH///nff//dDIRAAESRu//937//VQhEAABEjbf//ff//dDIRAAARI13//37//VQhEQAAESNX///v//dDIRAAABEjR//////VMhEA"
"AAARI0fv///+ZDIRAAABESNG3////VMhEAAAARIjV3///+ZDIRAAABESNGff//91MhEAAAARI1Z8///+ZCERAAAAETT/////5kMhEAAAABI9/////mQyEQAA"
"AAASPf///+ZDIREAAAAAET3v/+1UMhEQAAAAABEjRVVUMyERAAAAAAABEiMzMiIREAAAAAAAARERIiEREQAAAAAAAAAAEREiIiIiIhERAAABESMzREREMzIR"
"EAABEjRWZmZmZUMhEAARI9////////4yEAARJO////////0yEAARJO////////UyEAARJP///////2QhEAARI1Z3f///91MhEAARI0V37///1kIhEAARI0V9"
"///9dTIRAAERI0bf///nVCIRAAESJGf///92UyIRAAESNX////d2VCIRAAEjRv///3d3ZDIRABEjXv///////kIRABEk3////////kIRABEk7////////kIR"
"ABEs/////////TIRABEjRVZmZmZlQyEQAAESIzREREQzMhEQAAAREiIiIiIiEREAAAAAAAEREREREAAAAAERIiIiERAAAAERI0REMhEAAAERI13//jIQAAAR"
"I17//+MhEAARI03///0yEQABEjXv/+ZDIQAAESRv/+dTIRAAESNH//5kMhEAARI13//VMhEQABEjXf/9UyEQABESRu//ZDIRAAESNH//9kIRAAARJFf//lMh"
"EAARI1fv/2QyEQABEk7//nZCERAAESTv/+dTIRAAARJG3//lMhEAABEjV//+UyEQAAESNX//5TIRAAARI13//VMhEAABEjXf/9UyEQAAESNu//1TIRAAARJG"
"//91MhEAABEjb//9ZCEQAAESNf///kIRAAABI07//+QhEAAAESNe//0yEQAAAREjRFQyEQAAAAERIjMiERAAAAABERERERAAAAAAAAEREREQAAABESIiERAA"
"ABEjMzIRAAARI9/+MhEAARJN/+QhEAESNe/9QhEAESNf/9QhEAESRv/2QhEBESTP/mQhEBEjXf/lMhEBEjXv/VMhEBEjXv/VMhEBEkb/9kMhEREkb/5kIRAR"
"I03/5kIRARI13/5TIRARI17/1TIRARJG//1DIRARJG//ZCERASNG/+ZCEQASNd/+UyEQASNe/+UyEQASNe/9UyEQASNf/2QhEQASRv/mQhEAASTf/lMhEAAS"
"Pf/kMhEAASPv/TIRAAARIzMyERAAAREiIhEQAAAAEREREAAAAAAAAREREREQAAAAERIjMiERAAAAESNFVDIhEAABEj3//lQhEAABEj3///UyEQABEk7///wy"
"EQABEjV///ZCEQABEjV//+ZCEQABEjV//+ZCEQABEjXf/+YyEQABEjXf/9UyEQABEjXf/9UyEQABEjXv/3UyEQABEjXv/nUyEQABEjXf/9ZCEQABEjV+//9C"
"EQAREkZ+//5CEQARI0b//nUyEQARI17/91QhEAERJG7/5kMhEAESNH//5kIREAESNd//1TIRABEiRu//1TIRABEjV///ZCERABEkbf/+ZCEQABI9///+UyEQ"
"ABI9///1QhEAABE+//5UMhEAABEjREQyIRAAABESIzIhERAAAAEREREREAAAAAAAEREREREREAAAAAABEREiIREREREQAAAREiMzMiIiERERAAESI0VVVEMz"
"MiIhEBEiRd7+5mVERE0hEBEjX////+d2bf4yEBE0//////////0yEBI9//////////QxEBI9/mZ37////1QhEBEtRERFVt7/1UIhEBESIzMzNFVVQyIRAAER"
"ERIiIjMzIhEQAAARERERESIiEREAAAAAABERERERERAAAAAAAREREREQAAABESIzIhEQAAARI0VUMhEAABEj3//+MhEAARJO///TIRAAEjXv//0yEQARI1//"
"/1MhEAESNP//9DIRABEjRmdlMhEAARI0ZmVDIRAAESNe/9QyEQARI0b//UMhEAESNd//1TIRABEkbv/9UyEQESNH///VMhEBEjXf//1TIRARI27//3QxEQES"
"Rv//90MRABE03///ZCEQARNe///mQhEAETTv//5TIRABEt////QxEQARI0VVVDIRAAARIjMzIhEAAAARESIRERAAAAAAERERERAAABERIiIiERAAESI0RDMh"
"EAESNF7/1DIRARNN///9QhERI1/8ff9DEREk79d3/0MRESTv133+QxERJO/nf/1CEREj3///5DIRARJN7+1DIRABEiNEQzIREAAREiIiIREAAAEREREREAAA"
"ERERERAAESIjIiEQESNEVDIhERJP//8yERI1///0IREjz//+QhESPf//1CERI+///TIRESNERDIRAREiMyIREAEREREREAAAAAAAEREREREQAAAAAAABEiMz"
"IhEAAAAAAAESNEVUMhEAAAAAABEk7//9MRAAAAAAESNe//9TIQAAAAABEkb///UyEQAAAAERJN///lMhEAAAABEjTv//5DEQAAAAERI0Z3d1QhEAAAAREjRX"
"3dZTIRAAABESNFfv/+QyERAAARI0V+///UMhEQABEjRt///+ZDIhEAARJF7///12VDIhEBEjXf///Xd2VUIRARJG///9d37//jIRESTf//93fP//4yEREk3/"
"//537//9MhERJN////////5TIQESNf///////lQhEAETRv/////+VDIRABEjRt7//tZUMhEAABEjRFZmVUMiERAAAREiMzMzMiEREAAAARERIiIiEREAAAAA"
"AAAAAAAAAREREREAAAAAAAAAAAAREiIiERAAAAAAAAAAARIjREMhEAAAAAAAAAARI03//9IQAAAAAAAAAREkz//9MhEAAAAAAAAREjX//1QiEAAAAAAAABEj"
"T//mQyEQAAAAAAABEiRWd2VDIRAAAAAAABEiRWd3dlMhEQAAAAABESNe////1DIRAAAAAAESNc/////lMhEAAAAAESNG7////+ZCEQAAAAARI13/////5kIR"
"AAAAARI0fv/////3UyEQAAABEjXf//7///1TIRAAABEjR+//9+///VMhEAABESRt///n7//+ZCEQAAESNX///3fv//5kIRAAERJG3//+d9///nQyEAARI1f/"
"//133///xTIRAREkbv//53ff///VMhEBEjV//////////9YyEQESRu//////////5kIRESNX///////////mQhERI17//////////+ZCEREk3///13d3fv//"
"/UIRESTv//92VVZ+///9QhERLf///VQzNF3///4yEREjRVVUMyIjNFVVQyEQARIjMzIiERIiMzMyERAAEREiIREREREiIhERAAAAAAAAAAERERERAAAAAAAA"
"AAEREiIiERAAAAAAAAAREjNEQyEQAAAAAAABESNN///SEQAAAAABERI03//0MhEAAAABEREiRf/+ZDIREAAAERIiM0//5lQyIRAAARIjNEVmd3ZVQyERABEj"
"RWZ3d3d3dlQyEQARJO//////////0hEBEjXv/////////1MRARJG//////////9DEQETR//////////+QhERI13///13d3d2VDIRESNd///9d3d3ZUMhEBEk"
"bv//93d3d2VCIRARJH/////////+QhEQETR//////////UIRABI1f/////////0yEQASNd/////////1MhEAEkbv//93d3d2VCERABJG7///d3dmVUMhEQAS"
"R////3d2ZlVDIREAEkf///53d3d2VDIRABNc//////////4yEQATTf/////////9MhEAEk7//////////TIRABI///////////QyEAARJFVmZmZmZlVDIRAA"
"ESIzREREREMzIhEQAAERIiIiIiIiIhERAAAAAAAAERERERAAAAABESIiIhEAAAAAESM0RDIRAAAAESNO///REAAAESNN//9CEQAAARI1//5TIRAAARIk//1U"
"MhEAABEjRmZkMhEQABEjRnd2UyERAAESTf///UIRAAESNd///8QhEAARI27///ZCEQABEkb///5kIRAAEjR////mQhEAESNd///9UyEQARI13///1TIRABEk"
"bv//91MhEAESRu///3QxEAARNH///+ZCEQABI1f///5kIRAAEjXf///VMhEAASNt///9UyEQABJG7///dDIRAAEkb///5kIRAAASRv///mQhEAABJM///9Uy"
"EQAAEk3///1DEQAAASPv//9TIRAAABEjRVVDIREAAAERIjMyIREAAAABERIhEREAAAAAAAAAAAARERIhERERAAAAAAAAAREiIzMiIiERAAAAAAABESM0VURE"
"QyIRAAAAAAARI0zv/mZu/SEQAAAAAREjTv//////QhEQAAABERI9///////UIhEAAAARIjTv533v/mVDIREAABEjNFZmVVZmZlQzIRAAESNFZ3ZlREVWZmVD"
"IRABEk7//+dUREbv//4yEQESNe///9ZURW7//+QhEBEkb////3ZVV////UMRARNH/////nZmf///dCEREjXf/////Xd9///mQhERI23/////93ff//5kIRES"
"Ru//////537//+ZCEREkf///////1////WMhERNH////////f///1TIREjV////////////3UyERI13//+7///////50MRASNu///n7//////+ZCEQEkbv//"
"537//////mQhEBJG///9d9//////1TIRASR///93d+/////3UyEQE0z//+dmZ/////50MRABNN///mVFbf///+ZCEQASTv//5UNFbv///lMhEAEj///9QyM0"
"b///1CEQABEjRVRDIiI0VVVUMhEAARIjMzIhERIjMzMiEQAAARERERERERESIhERAAAAAAAAAAAAABEREREQAAAAAAAAAAABESIiIhEAAAAAAAAAAAERIzRE"
"MhEAAAAAAAAAAREjTv//0RAAAAAAAAABESNN//9DIRAAAAAAAAERIkz//lQiEQAAAAAAEREiNP/9ZDIhEAAAAAARESM0VndmUzIREAAAABESI0Vnd3d2VDIR"
"AAAAERI0Vt7v//7WVDIRAAAREjVt///////9VCERABESNX/////////9UyEQARI1f//////////8QxEBEjRv///+d3zv///lMhERI17///13Znf///5kIRET"
"Ru///nZVVn7///ZCERI13///11REZ+///2QhESRu///3ZERGf///9kIREkb///51QzRn///+ZCERJH///+dUNEbP///lMhESR////nZERX3///1TIREkf///"
"52VVZ+///2QhEBJG////d2Z37///1TIRASNe///+d3fv///mQhEQEjTf//////////ZDIRABEjXv////////9lMhEQARIkXf///////WQyERAAARIkVt7v/+"
"7WVDIREAAAARIjRVZmZVRDIhEQAAAAAREiMzNDMzIhERAAAAAAARESIiIiIRERAAAAAAAAAAAAAAABEREREQAAAAAAAAAAABERIiIhEQAAAAAAAAAAERIzRE"
"MhEAAAAAAAAAAREjTv//0hEAAAAAAAARESJN//9DIREAAAAAEREREjz//lQyERAAAAARESIiI//9VEMiERAAABEiMzMzNEVUREMyIRAAESNFVVRDM0RVZlVC"
"IRABEk7///VDM0Xv//4yEQESNe///1QzNW///+QhEBEkb///9kM0Xf///UMRARNH///+ZDNG7///dCEREjXf///WQzRu///nQhERI13///1UM1fv//5kIRES"
"Ru///3VDRX///+ZCEREkb///91Q0bf///VMhERNH///+dURG3///1TIREjV////nVEV+///3UyERI13///11RFfv//5kIRASNe///3dUVn7//+ZCEQEjXv//"
"93ZWff///VMhEBI17///13d3////ZCEQASNd////x33////lMhEAESRv/////////+ZCERABEjXf////////5kMhEAABEjXf///////WQyERAAARIjVt7v/+"
"7WVDIREAAAAREjRVZmZVRDIhEQAAAAAREiMzNDMzIhERAAAAAAARESIiIiIRERAAAAAAAAAAAAERERERERERAAAAAAAAAREiIiIiIiERAAAAAAABESNERERE"
"QyEQAAAAAAARI9//5d//0yEQAAAAABESTf/9bf/9MhEAAAAAERI07//G7/9UIREAAAAREiNP//Zv//VDIREAABEiNEVWZVVWZlVDIRAAESNFZmZVREVmd2VD"
"IRABEk7///VDNFbv//4yEQESNe///2QzRX///+QhEBEkb///9kM0Xf///UMRARNH///+ZDNG7///dDEREjXf///WQzRu///nQhERI13///1UM1fv//5kIRES"
"Ru///3VDRX///+ZCEREkb///91Q0bf///VMhERNH///+dURG3///1TIREjV////nVEV+///3UyERI13///11RFfv//5kIRASNe///3dUVn7//+ZCEQEjXv//"
"93ZWff///VMhEBI17///13d3////ZCEQASNd////x33////lMhEAESRv/////////+ZCERABEjXf////////5kMhEAABEjXf///////WQyERAAARIjVt7v/+"
"7WVDIREAAAAREjRVZmZVRDIhEQAAAAAREiMzNDMzIhERAAAAAAARESIiIiIRERAAAAAAAAAAAAABEREREQAAAAAAAAEREiIiIRAAAAAAABESI0RDIhEAAAAA"
"AREjRf//4hEAAAAAERI0Xv/9QhEAAAABESJF7//VMhEQAAAREjRe/+dlQiEQAAERI0V3d3d2QyEQABESNW3v//7nZDIRABEjXf//////5kIRARI07///////"
"/lMhARI9/////////1MhARJO///nd+///2MhESJGd3d3d9///mMhESNGd97v/////VMhESRX7////////FMhEjTf////////91MhEjX///53fP//91MhEkb/"
"//d3z///92QhEk3//////////9QhEk3//////////9QhEjXv////3f///1MhESRe//7Xd37/7UIRESI0VmVVRFVVVDIRAREjMzMzMzMzMiEQABERIiIiIiIi"
"EREAAAAAAAABERERERAAAAAAAAAREiIiIRAAAAAAAAERI0REMhEAAAAAABEiNe//8hEAAAAAEREjXv/+QyEQAAABESI03//lQyEQAAAREiNG//11QyEQAAER"
"I0Vnd3d2QyERABESNGfe//7XZDIRAREjVu//////5kIRARI0bv///////VMhERJG/////////2QhESNe///3d9///9QhESRv//93d37//9QxEjXf////////"
"/9QxEjXv/////////3QhEjXv/////////2QhEjXv//13d3d3d1MhEjXv//53d+///lMhETTf/////////UIRESNe////////1TIRESNG7//////9UyEQARI0"
"XO7//u1kMhEQABEiNFVmZlRDIREAABERIjM0MzMiERAAAAARESIiIiEREQAAAAAAAREREREAAAAAAREiIiERAAAAARIjREMhEAAAARI03//+IQAAAREkX//9"
"MhEAABEjXv/1QyEAABEjTv/mUyEQAAESNXd3ZDIRAAESNN///kMhEAARI17//9QyEQABEkbv//1DIRAAESRv//90MRAAESNX///2QhEAARI13//+ZCEQABEj"
"Xf//5jIRAAESRu///VMhEAARJG7//3UyEQABEkf///dDEQAAETXP//5kIRAAASNd///lMhEAABE17//9UyEQAAESTv//1CEQAAARI///9TIRAAABEiRFVDIR"
"AAAAARIjMyIREAAAAAEREREREAAAAAAAAAERESIhERERAAAAAAERIjMzMiIhEQAAAAERI0RVVERDIhAAAAERI03/7XZu/SEQAAARI07//////0IRAAARI03/"
"/////1QhEAAREjX/133v/nUyEQABEjVnd3d3d3dTIREBEjTv/+d97/7WQyEQESNe//3P////1TIRARJG7////////2QhERI1f/////////ZCEREjXf///nfv"
"//9kIRESNt///nd3///mQhERJG7//9d33//+ZCEREkbv//d3ff//5kIREjR///93d+///VMhESNd///3Zn///9UyERI13//+dmf///dDIREjXv//52Z///5k"
"IRASNe///WVt///lMhEBI17//2VF7//+UyEQEST///VETv//0yEQARI0VVQzM0VVQyIRAAESIzMiIiIzMiERAAARERERERERIRERAAAAAAAAAAEREREREAAA"
"AAAAAAERIiIiEQAAAAAAAAERI0REMhEAAAAAAAESI17//yEQAAAAABERI17//kMhEAAAABERI03//lQyEQAAABESI0b//XVDIRAAABESNFZ3d3dkMhEQABES"
"NFfe//7XZDIRABESNG7//////mQhEAESNG7///////1TIRERJG/////////2QhERI17///d33///1CEREkb///x3d+///VMREjXf//53d37//9UxESNe///X"
"dnfv//xCERI17//9d3d///9kIREjXv//13d9///lMhESNe///nd9///9QyERE03////////+UyEQESNe///////+ZCIRARI0bv/////9VDIRAAESNF3v//7W"
"VDIRAAABEiNFVmZUQyIREAAAEREiMzMzMiEREAAAAAEREiIiIhERAAAAAAAAAAABERERERAAAAAAAAABESIiIhEQAAAAAAARESNERDIRAAAAAAARESNO//8y"
"EQAAAAEREiNO//5DIRAAAAERIiNN//5kMhEQAAESIzRF//1mVDIRAAESNFVmZmZ3dlQyEQERNO///WZt///kIRARI17//9Zm3//+QxEBEkb///dmfv//1TER"
"EjXP//92Z////VMRESNd///nZn///3QxERJG7//+dnf//+ZCEREkbv//13ff//5kIRESR+//93d+///mQhERNX///3d37//9UyERI13///d3f///dTIRETXf"
"///Xf///90MRARNN/////////mQhEBEk3////////+UyEQESNe/////e//1DIRABEkXf/+13///TIRAAESI0VWVVVVVDIREAABESMzMzMzMzIREAAAARESIi"
"IiIiEREAAAAAAAERERERERERAAAAAAERIjMiIyIhEQAAAAERI0RERERDIRAAAAERI9//1d//0yEQAAARI07//X7//DIRAAAREjXv/3fv/2QhEQAREjRv//d/"
"//ZDIRABEjRnd3dnd3d1MhEBEjTv//1mbf//5CEQESNe///WZ9///lMhARJG///3Zn7//9UyERI1z///dmf///1TIREjXf//52d///90MRESRu///nZ3///m"
"QhERJG7//9d33//+ZCEREkfv//d3fv//5kIRETV///93d+///VMhESNd///3d3///3UyERE13///13////dDEQETTf////////5kIRARJN/////////lMhEB"
"EjXv///+fv/9QyEQARJF3//nd///0yEQABEiNFVVVVVVQyERAAAREiMzMzMzMyERAAAAEREiIiIiIhERAAAAABEREREREREAAAABESIiIiIREQAAAREiMzQz"
"MiERAAERIjRVVUQyERAAERI0/////TIRABESNN////9DIRABESNd////5TIREBESNd////1TIREBEiRd////ZTIREBEjRu///+ZDIREREjRu///9ZDIREREj"
"Ru///3VCIRAREjRu///mUyERAREjRu///WQyERAREjRu///FQiERAREjRv//9lMiEQAREjRv//5kMhEQAREjRv//1UMhEQARIjRf//ZDIhEQARIjRmd2VDIR"
"EAARI0Vnd2VDIREAARI07///5DIREAARI1////1DIREAASJF////1DIREAASJN////VCIRAAARI9////UyERAAARI+///+QyERAAARIjRFVEMhEQAAAREiMz"
"MyIhEQAAABERIiIiEREAAAAAERERERERAAAAAAABEREREiEREREQAAARESIiIiIiIhERAAERIjMzMzMzMyIREAERI0RVVVVVRDIhEBESNP///W7//9MhEREi"
"Rf//93///9MhEREjRv//53///1QhEREjTf//53///lQhEREjXv//19///lMhEREjXv//d9///VMhEREjTv/+d9//9UMhEREjTf/9Zn//1TIREBEiPf/mVW/+"
"VDIREBESNP/URF79QyERAAERIzMzMzRDMhERAAEREiIiIiIiIREQAAAREREiIiIREREAAAABERERERERERAAAAAAAAAAARERERERERERAAAAAAAAABEREiIi"
"IiIhEREAAAAAAAEREiMzMyMzMiERAAAAAAEREiM0REQ0REMyERAAAAARESI0X//lVd//0iEQAAABERIjRd//1mb//0MhEQAAAREiNFb//nd3//1DIREAABES"
"I0Vt//13fv/2UyERAAERIjRWfv/3d3//51QiEQABEiNFZ3//53ff/3dUMhEAERI07///////////9DIRABESNf///////////+QyEQARIkX////////////k"
"MhEBESNN////////////1DIRAREjRXd+/+d33//XdkMhERESI0V33//Hd+//d2VDIREREjRWd///d3f//ndUMiEQESI1Z3z//Xd9//x3VDIREBEjTv//////"
"/////9QyERARI07///////////9UMhEAESNf////////////UyERABEjz////////////0MhEQARI0Z3//13ff/3dlQyIRAAESJFfv/3d37/52VDIhEQABES"
"NG//53ff/9ZUMiERAAAREjTf/2Zm7/9kMyIREAAAAREj//9VVf/9QzIRERAAAAERIjRERERERDIhEREAAAAAERIjMzMzMzIiEREAAAAAAAEREiIiIiIiEREQ"
"AAAAAAAAEREREREREREQAAAAAAAAAAAAAAEREREREAAAAAAAAAAAAREREhEREAAAAAAAAAARERIiIiIREQAAAAAAABEREiM0MyIREQAAAAAAEREiM0//0yIR"
"EQAAAAARESI0TP/1QyIREAAAABERIjRW3/9lQyIREAAAERIjNFZ9/+dlQyIRAAAREiNFZ33v/tdlQyERAAERI0Vu///////VQyEQAREjRd/////////kMhEA"
"ERI13//////////TIREBEjRv///9d37///1CIRERI13///53d97u7tQiERESNd////7Xd3d3ZTIhEREjXv///////td2QyIRESI13/////////1lMhEREiNW"
"7/////////dTIRERIkVn7////////+UyERESNFd3fe///////lQhEREjRnd3d3fO////5TIRERI8///+d3d+///9UyERESNf///9d33///9kMhEBEjX/////"
"/////+VCIRARI03/////////5kMhEQESI13////////mQyIRABESNFbe////7WVDIhEQABESNFZ37/d3ZUMiERAAAREiNFbf/3ZUMyIREQAAAREiNF3/9lQy"
"IhERAAAAEREiNO/+QzIhEREAAAAAEREiPd1DMiEREAAAAAAAEREiMzMiEREQAAAAAAAAEREiIiIRERAAAAAAAAAAERERERERAAAAAAAAAAAAARERERAAAAAA"
"AAAAAAEREREREREREREREREAAAAAAAABERIiIiIiERERERERERAAAAAAAREiIzMzMyIhEREiIiIREAAAAAERIjRFVVVEMiIiIjMzIhEAAAABESI0Xe//7VQz"
"IiM07/4hEAAAABESNF/////+VDMzRN//MhEQAAABEjRf//////1UM0TP/0MhEQAAAREjXf/+d9//5lRFX//UMhEQAAAREkXv/3d3//5lVW7/1UMhEQAAARI0"
"X//nd3//52Zu/+VDIhERAAARI03//nd9//12ff/2VDIhERAAARIk3//nff/+d3f//VVDIiEREAAREjX//////9d3//1lVDMyIREQAREjTf/////Xd+/+dmVU"
"QzIhEQABEiNF7//+d3ff/3d2ZmVUMiERABESI0VWZmZ33/93d97//lQyIRAAERIjNEVVZ3//133/////1DIREAEREiIzRFZ+/+d8//////5TIREAARESIjNF"
"bv/nd+//137//UIhEAABERIjNF3/93ff/9d37//UMhEAABERIjRW//d3fv/3d37/9kMhEAAAERIjRf/9Zmf//3d3//9UIhEAAAERI0Xv/mVVb//3d3//5TIR"
"EAAAERI03/5VRFbv/9d+//1DIREAAAERI9//VEM0Xf/////+VCIRAAAAERI//9QzMzRe/////lQyERAAAAARL//kMyIjNF3v/+VUMhERAAAAARIjMzIiIiI0"
"RVVUQzIhEQAAAAAREiIiIRERIiMzMzMiEREAAAAAABERERERERESIiIiIRERAAAAAAAAEREREREREREREREREQAAAAAAAAERERESIiIiIREREAAAAAAAAAAR"
"ERIiIjMzMiIiEREQAAAAAAABERIiMzREREQzMiIREQAAAAAAEREiNEVVZmZlVUQyIREAAAAAAREiM0Vm3u///u1lQzIREAAAAAERI0Vt/////////sVDIREA"
"AAAREjRd///////////9QyIREAAAESI13////////////9QyERAAAREjRu////53d33////kMhERAAERI17////Xd3d3f///5TIhEQABEiRe////d3dmZ33d"
"3WVDIhEQARI0Xv///nd3ZmZ3d3dmVDIREBESNF3////Xd3Zmd3d3dmVDIRAREjRn7//////3Z+//////4yEQESI0Z37/////53fv/////9MhEBEiNWfv////"
"/+d3///////DIRARI0Z////////Xd///////QyEQESNW////53d3d33///13VDIhEBIkXv///nd3Zmd9///9dUMiERASNG7///13dmZnfv//92QyIREAEjTP"
"///9d3Zmd37//+ZTIhERABI0z////nd3d3fv///VQyEREAASJF/////nd3fe///+ZDIhEQAAESNe////////////5lMiERAAABEjRe///////////2VDIREQ"
"AAAREjRd/////////9ZUMhERAAAAERIjRWze////7WZUMyEREAAAAAERIjNFVWZmZVVEMyIREQAAAAAAERIiMzRERERDMyIhERAAAAAAAAEREiIiMzMzIiIh"
"EREAAAAAAAAAEREREiIiIiEREREAAAAAAAAAARERERERAAABERIiIhERAAERIiMzIhEQABESNERDMhEQERI0///zIhEBESNf//9DIRARI03//+QyERESNN//"
"/kMhEREjXv//1DIRERI17//1QyERESNO//5TIREBEjTf/1QyERARIj3/5DIhEQERI8/0MiERAAERIzMyIREAABERIiIhERAAABEREREREAAAAAEREREAAAAA"
"AAAAABEREREQAAAAAAABEREREREQAAAAAAEREiIiIhEQAAAAARESIzQzIhEAAAABERIzTf/9IREAAAEREjRd//8yERAAARESNFb//1MhEQAAERIjVv//1DIR"
"EAAREiNG7//lQyERAAERI0Xf//1UIhEAAREiNW///2QyERAAERI0bv//1UMhEQAREiRc///mUyIREAERI0bv//1kMhEQABEiNW///3VDIREAARI0Xv//5lMi"
"ERAAESNG///9ZDIREAABEjVv//91QyERAAASI13//+dTIhEQAAEiRd///mUyIRAAABI0bv//5kMhEQAAASNG7//9ZDIREAAAEjRv///WQyERAAABI0bv//1k"
"MhEQAAASNG7//9ZDIREAAAEjRe///WQyERAAABIkXf//5kMhEQAAARI1b//+ZDIREAAAESNG///2QyERAAABEjRe//9kMhEQAAAREjTf//1DIREAAAERI0Xv"
"/+QyERAAAAERI0X//0MhEQAAABESI07//SIRAAAAABESIzRDMhEQAAAAABESIiIiERAAAAAAABEREiEREQAAAAAAABEREREQAAAAAAAAAAAREREREAAAAAAA"
"ERESIRERAAAAAAAREiIiIhEQAAAAAREiNEMyIREAAAABEi3//kMiERAAAAESNP//1DIREAAAERI07//lQyERAAAREjTf//1TIREAABESNG///lQyEQAAERI0"
"bv//ZDIRAAAREjRu///VMhEAABESNG7//9VCIQAAERI0bf//5kMhAAAREjRt///mQyEAABESNG3//+ZDIQAAERI0bf//9kMhAAAREjRt///2QyEAABESNG7/"
"/+ZDIQAAESI1bv//5kMhAAERIjV+///VQiEAAREjRX///3UyEQABESNG3///ZDIRAAESI0bv//5kMhEAERIkV////FMiEQAREjRd///mQyERAREiNG7//9VC"
"IREBESNF3//+ZDIREAERI0bv//ZUMhEQARIjXP//5UMhEQABEjRe//5kMiERABESNN///FMiERAAERI07//VQyEREAAREj7//VQyEREAABES3//UMyEREAAA"
"ARIjNDMiEREAAAABESIiIiEREAAAAAAREREREREAAAAAAAERERERAAAAAAAAAAABERERERAAAAAAARERERERERAAAAERESIiIiIREQAAAREiIzMzMiEREAAR"
"EiM0T/5DMhEQABEiNFXP/VQzIREBESNO19/nbuMhEQESJO/93+3/9DIRARI07//////0MhEREjRs7///51QyERESNWfv///XVCIRERI9///////TIREREj7/"
"7f7f/9MhEQESPf19/nb9QyEQARIjRW7+ZUQyERABESI0T/5EMyIRAAAREiMzQzMiEREAABEREiIiIhEREAAAABERESEREREAAAAAARERERERAAAAAAAAAAAR"
"EREREAAAAAAAAAAAERESIRERAAAAAAAAARESIjIiEREAAAAAAAEREiNEQzIREQAAAAABERIjT//0MhEQAAAAARESI0X//0MiERAAAAEREiNF3//lQyEREAAB"
"ERIjNG3//mQyIREQAREiI0RX7//WVDIhEQARIjNFZ37/93ZUMiEQERI0Vmd3//93dlQyERESJP///////////TIRESNP///////////TIRESPP//////////"
"9DIRESPf//////////5CIRERI0Vnd///d3ZlQyERARIjRWd///d2VDMiEQAREiNFbf/+dUQyIhEQARESI0bf/+ZDMiEREAABERI0Xv/9VDIhERAAAAERIjTv"
"/1QyIREQAAAAAREjT//0MiEREAAAAAARESM0RDIhERAAAAAAABERIiMiIREQAAAAAAAAERESIRERAAAAAAAAAAABEREREQAAAAAAAAAREREREREQAAARESIi"
"IiEREAAREiMzMzMhEQABEiNEVVRDIREBESNO////QyEQESI1////9DIRARI0b////lMhEREjTP///+UyERESNd////1TIRERI17///9kMhEREjT////2QyER"
"ESNGff/9UyIRERI0Z//+ZDIREREjRu//ZTIhERESNM//1UMhEQERI07/5UMhERAREj3/9UMiERAAESL//UMiERAAAREjMzMiEREAAAERIiIhERAAAAARERER"
"ERAAAAAAARERERAAAAAAEREREREREQAAEREiIiIiEREAAREiMzMzMiERAREjNFVVRDIhEBESPv////4yEQESNO/////TIRERI0/////9MhEREjT/////QyER"
"ESLf////9DIRAREjRFVVRDIREAERIzMzMzIhEQARESIiIiIREQAAEREREREREQAAABERERERERAAEREiIiIhERARESMzMzIhEQERI0RVVEMhEREjTv///zIh"
"ERI1////9DIREiRf///+QyERI0z////kMhESNN////1DIREiTv///1QiEREj////9DIRERIzRVVEMiEQERIjMzMyIREBERIiIiIREQABERERERERAAAAAAAA"
"AAERERERAAAAAAAAARERERERAAAAAAABERIiIiERAAAAAAEREiMzMiERAAAAABESM+//MhEQAAAAERIjT//zIhEAAAARESNN//1CIRAAAAERIkXv/lQiEQAA"
"ABESNG//5TIREAAAERIjXf/9QyERAAABESNF7/9UMhEQAAERIjVv/+VCIREAABESNF7/9kMhEQAAAREjRv/+VDIREAABESI13//VMiERAAAREjRe//ZDIREA"
"ABESI0b//lQyERAAAREjRd//1TIhEQAAERI0b//mQyERAAAREiNd//1UMhEQAAERI0Xv/2QyIREAABESNG//5UMhEQAAERIjXf/9UyIREAABESNF//5kMhEQ"
"AAAREjRv/+VDIREAAAERI17//EMhERAAABESNe/+VDIREAAAAREk3//UMhERAAAAERI+//UyIREAAAABESP//kMhEQAAAAABEiMzMyEREAAAAAAREiIiIREQ"
"AAAAAAARERERERAAAAAAAAAREREREAAAAAAAAAAAARERESIiIREREQAAAAAAERESIiIzIiIhERAAAAABERIjM0RERDMiEREAAAAREiM0RVZmZVQzIhEQAAER"
"IjRFbe7//tZUMiERABERI0Vu///////lQyERABESNF3////////+VDIRAREiRc//////////1DIRARIjRu///+d37///1UIhERI0Xf///nd33///5kMhERI1"
"b///93d3f///9kMhESNF3///53Z3f///5kMhESNG7///12Znz///5kMhESNW7///x2Zn3///5kMhEiRd////d2Zn7///5kMhEjRd////d2Zn7///5kMhEjRu"
"///+dmZ3////1UIhEjRu///9dmZ3////dTIhEjRu///9dmZ9///+ZDIREjRu///8d2d+///9VDIREjRu///3d3d+///2UyIREiRe///9d3ff///mQyERESNd"
"////d33///9lMiERESNG//////////1UMhEQESI13////////9VDIREAERI0Xf//////9lQyIREAAREjRW3u//7WVUMiERAAARESM0VVZmVUQzIhEQAAABER"
"IjNEREQzMiEREAAAAAEREiIiMzIiIRERAAAAAAARERESIiIREREQAAAAAAAAAAAREREREREQAAAAAAABEREREiIRERAAAAAAARERIiIjMiEREAAAAAEREiIz"
"NEQzIREAAAARESIjNEVd/9MhEQAAAREiM0Vmz//0MhEQAAERIjRWff///lQhEQAAESI0Z+/////lQiEQAAESNP///////lQiEQAAESPf///////VQiEQAAES"
"Pf///////VQiEQAAESPv//////91MiEQABESNFZ33///51MhEQAAESI0V37///51MhEQAAESI0Vn7///1kMhEQABERIzVn////1kMhEQABERIzVn////dkMh"
"EQABESI0Vt///+dkMhEQABESI0V9///+dkMiERAREiNFZ+///+dlMiERARIjRWd+///9d1QyERERI0Vnd////9d2VDIRESNO///////////TIRESNO//////"
"////9TIRESNO//////////5TIRESNP//////////5DIRESPf//////////5CIRERI0VWZmZ3ZmZUQyERARIjNERFVVVERDMhEQARESIjMzMzMzMiIREQABER"
"EiIiIiIiIhEREAAAAAAAEREREiIiIREREAAAAAAAERESIiMzMiIhEREAAAAAEREiMzREREMzIhERAAAAEREjNEVWZmVUQyIREAAAEREjRFbe///uxUMiERAA"
"AREjRc///////+ZDIREAABEjRe/////////mQyEQABESNe//////////5TIRAAESNN////13fv///1QhEAARI07///d3d9////ZDIQABEjT///53d33///9k"
"IhAAESPe7u53d3fv///lQiEAARIjRWZmd3fv///9UyEQABESM0VWZ3fv///+ZDIRAAERIjRFZ3f/////ZTIhEAEREiNFZ33/////dUMhEQAREiM0Z3/////+"
"dkMiERABESM0Z9/////9dlQyIREBESI0V+/////nd2QzIREAERI0V+////53d3ZUMhEQERI0V/////13d3dlQyERAREjRv///////////TIREBEiRe//////"
"/////1MhEQESNN///////////+UyERARI07///////////5TIREBEjTv///////////kMhEAESJP///////////9MiEQAREjRVZmd3d3dmZUQyERAAERIzRE"
"VVVVVUREMiERAAARESIjMzMzMzMzIiEREAAAERESIiIiIiIiIhEREAAAAAAAABERERIiIiERERAAAAAAABEREiIiMzIiIRERAAAAABERIiM0RERDMyIREAAA"
"ABERIjRFVmZlVEMiERAAAAERIzRW3u//7WVDIhEQAAERI0Xf///////mQyERAAAREjXv////////5UMhEAABEjTv/////////+UyEQAAESPf///8d9////9T"
"IRAAARI+///3d3fv///1QhEAABEi3d3Wd3d9///+VCEQAAERI0RVZnd37///1TIRAAAREiM0RWd37///5kMhEAABERIjNFb/////52QyEQAAEREiI0Xf////"
"13VDIRAAERESIjRd/////ndUMhEAERESIjNF7/////91QiEQAREiIzM0VnfP///+VCIRAREiM0RERVZ33///5UIhEBEiNFVVVVVnfv///lQiEQESPf///WZm"
"d+///+VCERERI9///9d3d9////1TIREREj3////Xd9////5kMhEBESNf//////////9lMiEQARI03/////////9lQyERABESNN////////5lQyERAAERIjRd"
"7v//7tZUMyEREAABESI0RVZmZVVEMiEREAAAEREiMzREREQzIiEREAAAABEREiIjMzIiIhEREAAAAAARERESIiIiEREREAAAAAAAAAAAERERERERERAAAAAA"
"AAAAEREiIiIiEREQAAAAAAAAEREiM0MzMiEREAAAAAAAERIjRFVVRDIiERAAAAAAEREjRf///9QzIhEQAAAAEREjRd////ZVQyIRAAAAAREiNW////12/+Mh"
"EQAAAREiNG3///537/1CIRAAARESNFf///933//UMhEAABESI1bv//93f//2QyERABESI0bf///Xf///ZDIREBERI0V////Xfv//5kMhEQERI0Vu///nff//"
"/mQyERARIjRt///nd+///9ZDIREREjRd///nd37///dlMiEREiNW///nd3f///93UyIRESNG///Xd3d////3dUMhERI03////////////+QyEREjXf//////"
"//////5DIRESNe/////////////UMhERI07////////////9QyEREjT/////////////VCIRERI0VWZ3d3z///53VDIREBEiM0RVVnff///nZDIhEAEREiMz"
"NFV+///9ZDIhEQABEREiIjNF7///ZTIhEQAAAREREiIzT///9UMhERAAAAAREREiM0VVVEMhERAAAAAAAREREiMzMzIhERAAAAAAAAARERIiIiIhERAAAAAA"
"AAAAARERERERERAAAAAAAAARERIiIiIiIiEREQAAAAERIiMzMzMzMyIhERAAABESIzREVVVUREMyERAAAREiNFZmd3dmZlRDIREAAREjTv/////////zIhEA"
"ERI0X//////////kMhEAERI1b//////////kMhEAESJF3//////////UMhEBESNG7/////////9kMhEBESNW//////////9UMhEBESNX///3d3d3d3dUMhEB"
"EjRd///n3v/+13ZTIhEREjRu/////////8ZUIhEREjRu//////////dUMhEREjTf//////////5kMhEREjTf///nd9////9kMhERIjRX3u13d37///9kMhER"
"IjRWd3d3d33///9kMhERI0Vnd3dmZ33///5kMhERI07u7ndmZ33///5kMhERI9///9d2d37///1UIhERJN///+d3d9////ZDIRERI1////13fv///9VDIRER"
"I07//////////mQyERARI0Xv////////1kMiERAREjRe///////2VDIhEQAREiNFbe///tZVQyIREAABESIzRVVmZVRDMiEREAAAEREiM0RERDMyIRERAAAA"
"ARESIiIzMiIhEREQAAAAABERERIiIhERERAAAAAAAAABEREREiIhERERAAAAAAARERIiIjMiIiEREAAAAAEREiIzREREMyIhEQAAABERIjRFVWZlVEMiERAA"
"ARESM0Vt7v/+5lQyIREAEREjRW7//////+VDIREAERI0Vv////////5UMhEBESI1b//////////kMhEBESNG7///53ff///1MhEREjRd///+d3d+///1MhER"
"EjVv///3d3d+///1MhERIkXf///nd3d3d3dkMhERI0bv///X3v/+13dUMhERI1fv///u/////9dUMhESJFz///////////1UMhESNG3///////////9kMhES"
"NG7////9d3////9kMhESNG7///93d33////VMhESNG7///13d3f////VMhESNG7///x3d3f///9kMhESNG7///d3d33///5kMhESJF7///13d3////1TIRER"
"I13///53fP///+ZDIRERI0b//////////mUyIRARIjXf////////5lQyERAREjRd///////9ZUMhEQABESNFbe7//u1lQzIREQABERIzRVVmZVVEMiEREAAA"
"EREiMzREREMyIhERAAAAARESIiIzMiIiEREQAAAAABERERIiIhERERAAAAAAERESIiIiIiIiIhEREAABERIiMzMzMzMzMyIhEQABESI0REVVVVVURDMiERAR"
"EjNFZmZ3d3dmZVQyIRARIj3////////////jIRARI07////////////TIRERI17////////////TIRERI17///////////5TIRARI0///////////+ZDIRAR"
"I9///////////2UyIRARIjRWd3d3f///91QyERAREjNFVnd3////dkMiEQABESM0VWd+///9ZTIhEQABESIzRWfv///XVDIREAABERIzRn7///51QyIREAAB"
"ERI0Vt////dkMiERAAABESI0Z////9ZTIhEQAAABESI1bv///nVDIREQAAABESNG3///92QyIREAAAABEiNW7///5lMiERAAAAABEjRd////dkMhERAAAAAR"
"EjRv////ZTIhEQAAAAAREjXf///9ZDIREAAAAAAREjXv///3VCIREAAAAAAREkX////mQyERAAAAAAAREj3////lQyERAAAAAAAREj7////kMhEQAAAAAAAB"
"EiNFVVRDIhEQAAAAAAABESIzMzMyIREAAAAAAAAAEREiIiIhERAAAAAAAAAAAREREREREQAAAAAAAAAAAAAAERERESIiEREREAAAAAAAERESIiIzIiIhERAA"
"AAAAEREiMzREREMyIREQAAAAEREjNEVWZmVUMyIREAAAERIjRFbe7//tZUMhEQAAAREjRW7//////9VDIREAAREjRd/////////lQiEQABEiNd//////////"
"1DIRABESNG7///13fv///lMhEAERI13///d3d37//+UyEQARIkXv//93d3f///5TIRAREjRu///nd3d////VMhEBESNG7///13ff///mQyEQERI0bP//////"
"///3ZDIRARIjRn3////////ndUMhEREiRXff///////+d1QiERESNGf//////////nVCIRESI1b////nd9////1UIhERI0Xv///nd3fv///lQiEREjRv///8"
"d3d+///+VCIRESNN////d3d37///5UIRERI03///93d3f////VMhEREjTf///+d33////mQyERESJF///////////9UyIRAREjXf/////////9VDIREBESNF"
"3////////mVDIREAAREjRW3u///u1lRDIhEQABERIzRVVmZmVUQyIhEQAAARESIzNERERDMyIREQAAAAERESIiMzMyIiEREQAAAAABERERIiIiIREREQAAAA"
"AAAAARERESIiIREREQAAAAABEREiIiMzIiIhERAAAAARESIjM0RERDMiEREAAAEREiNEVVZmZVQzIhEQABERIzRWbe///tZUMiERABESNFbf///////VQyER"
"AREjRW/////////+VDIRAREjRv//////////1DIRERI0bv///9d37///5UIhERI13///93d3f///9kMhESJF7///13d3f///9kMhESNG////13d3f///5lMh"
"ESNG////13d37///5lMhESNG7////Xd+////5kMhESNF7///////////5kMhESJFf///////////5kMhESI1bP/////+////1UMhESI1Z33v/+13////dTIR"
"ESNFZ3d3d3d9///+ZDIRESNO/u/nd3d+///9ZDIRESNe///3d3d////3UyIRESNf///3d3fv///mQyERESNO///+d37///5lMiEQESNN//////////1UMhEQ"
"ERI17////////9VDIREAERI0Xf//////5lQyIREAAREjRW3v//7WVEMiERAAARESM0VVZmVUQyIREQAAABERIjM0REQzMiEREAAAAAEREiIiMyIiIRERAAAA"
"AAARERESIiIREREQAAAAAAAREREREREQAAARESIiIiEREAARESMzMzIhEQABESNEVVRDIREBESNO////MiEQERI1////9DIRARI0X////kMhEBEjTf///9Qy"
"ERESNN////1DIRERI07///9UMhEREjT////0MhERESNEVmZVQyERERIzRVVVQyIRERIjRVVVRDIhEREjRFZmZUMhEQESNO////QyERARI1////9TIREBI0X/"
"///lMhEQEjTP///+UyERASNN////1DIRABIk7///9UIhEAESP////0MhEQARIzRVVEMiERABESIzMzMiERAAEREiIiIhERAAABERERERERAAAAAAABERERER"
"ERAAAAARESIiIiEREAAAEREjMzMyIREAAAERI0RVVEMhEQABESNO////MiEQABESNf////QyEQABEjRf///+QyEQABEjTf///9QyEQAREjTf///9QyEQAREj"
"Tv///1QyEQAREjT////0MhEQAREjRVZmZUMhEQAREjRFVVVDIhEQERIjRVVVRDIhEQERI0VWZmVDIhEQERI07///9DIREAESNF////9TIREBESNG////5TIR"
"EBESNc////5TIREBESNd////1TIREBESNe////ZDIREBESNf////ZDIREBESNGff/9UyIRABESNGf//mQyERABESNG7/9lMiERABESNM//1UMhEQABESNO/+"
"VDIREQABESPf/1QyIREAAAESL//UMiERAAAAERIzMzIhERAAAAAREiIiEREAAAAAARERERERAAAAAAAAEREREQAAAAAAAAAAAAAAAAERERERAAAAAAAAAAAR"
"EREREREREAAAAAAAARERERESIiEREAAAAAARERESIiIzMiERAAAAAREREiIjM0REMyERAAAREREiIzNEVV3/0iEQABERIiMzRFVt7//8MhEAEREiM0RVZ+//"
"//5DIRABESM0VWff/////+QyEQERI0VW3v//////1DIREBEiRd7//////9ZUMyERARI07//////XdlRDIREAESNf////53d2VDMiERABEkX///53d3ZUQyIR"
"EQARJN/////nd2ZUMiEREAESPf//////12VEMhEQABEjRe///////mVDIhEAAREjRW3///////1DIRAAERIjRVZ+/////+QyEQAAERIjNEVm3////kMhEAAA"
"EREiM0RVZu//5DIRAAAAERESIjNEVV39MhEQAAAAERERIiMzREQyIRAAAAAAAREREiIiIiIhEQAAAAAAABEREREiIhERAAAAAAAAAAEREREREREAAAAAAAAA"
"AAABEREREAAAAAAAABERESIiIiIiEREREAAAEREiIjMzMzMiIiEREAAREiMzRERERERDMiERAAERI0RVZmZmZlVUMyERAREjT///////////MhEQERI9////"
"///////kIhEBESPf//////////5DIRARIj7//////////+MiEQESI0Vmd3d3d3dmVDIREREiNFVmZ3d3ZmVUMyERERIzRWZmd3d2ZlVDIhERESNFZnd3d3d3"
"ZlQyIRARIj7//////////+MiEQESJO///////////TIREBEiT///////////0yERAREj///////////0MhEQARIzRVVmZmZmVUQyERAAERIjNEREREREMzIh"
"EQABERIiIjMzMzMiIhERAAABERESIiIiIiIREREAAAAAAAEREREREAAAAAAAAAAAEREREREREQAAAAAAAAARESIiIhEREREAAAAAAAERIjMzIiIREREQAAAA"
"AREjNERDMyIiEREQAAAAESI+/sVVRDMiIREQAAABEjT///1mVEMyIREQAAARI0/////nZVQzIhEQABESPf/////+1mVDIhEQABEjTe///////mVDIREAERIj"
"RW3//////+1DIRABESI0RWfe//////0yEQARESM0Vnd33////1MhEAERIjNFVnd3fv//5TIRABESI0RWd33v///+UyEQERIjRVZ97//////kMhEBESNFbe//"
"/////WQyERARIj3v//////12VDIhEQESNP/////+dmVEMiERABEj3////tZlRDMiIREAARI9///WVURDMiIRERAAESPv1VVEMzIiIRERAAABESM0QzMyIiER"
"EREAAAABESIiIiIhEREREAAAAAARERERERERERAAAAAAAAARERERERERAAAAAAAAAAABEREREAAAAAAAAAAAAAABERERIiIiERERAAAAABEREiIiMyIiIREQ"
"AAABERIiM0RERDMiEREAABERIjRFVWZlVDMhERAAERIzRW3u//7WVDIREAERIjRu///////VQyERARIjRv////////1TIREBEjTf/////////9QyEQESNe//"
"/9d+////1TIRERI9///+d33////lMhEREj7///x3ff///9UyERESPu7u53d+////1DIRARIzRWZnd9////5UMhEBESM0VWd9////9lMhEQERIjNFZ+////9l"
"QyERABESI0Vu////5lQyERAAEREjRd////1lQyIREAARESNF7//+dlQyIREAAAERI0Xv//1lQyIREAAAAREjRWd3dlQyIREQAAABESNFZ3d1QyIREQAAAAER"
"I1////1DIhEQAAAAARIkX////EMhERAAAAABEjTP///lQyERAAAAAAESNN///+VCIRAAAAAAARIk7///5DIREAAAAAABESP////TIhEQAAAAAAERIjRFVDMh"
"EQAAAAAAABESIzMzIhEQAAAAAAAAAREiIiIhERAAAAAAAAABERERERERAAAAAAAAAAAAAAEREREREREREQAAAAAAAAABERERIiIiIREREAAAAAAAABEREiIj"
"MzMiIhERAAAAAAABERIiMzREREMyIREQAAAAABESIjNE3v/+7UMyIREAAAABESIzTf///////UMiEQAAABESI03//lVVVd//1DIREAABESI03/1VRERFVd/9"
"QyERAAERI03/VVRERERFXf9DIREAERI03/VVVEREREVW79QiEQARIjT/1VVVVVVVVVbv5DIRAREjTf5mZmZmZmZVVt/kMhEBEiNP/GZn3v//7XZmz+QyERES"
"NN/mZ+//////93bP5DIRERI079Zu///////+d9/kMhERIjX/Zn///nff//933+QyEREjRf9md3d3d3///nff5DIRESNN/md3fe7////+d+/UMhERI079d33/"
"//////13/1QyEREjXvd37//+d+//933+VCIRESNe93f//+d37//3fv1DIRESJF/3fv//d33///ff9UMhEBIkX+d+///X3//////VMhEQEiRf537/////////"
"/lQyERARI1/3ff////7v///lQyERABEjTv133v/+d37/7VQyIREAESNO/nd3d3dmZmZUQyIREAARI0X/ZmZmZlVVREMyIREAABESNO/2ZmZVVUQzIiEREAAA"
"ERIjTv/mZVVd0zIhEREAAAABESM07//////TIhEREAAAAAAREjNE3v/+7TIhERAAAAAAABERIjNEREQzIhERAAAAAAAAARESIiMzMyIhERAAAAAAAAAAERER"
"IiIiEREQAAAAAAAAAAAAEREREREREQAAAAAAAAAAAAAAAAAREREiIiIRERAAAAAAAAAAABERIiMzMyIhERAAAAAAAAAAEREiM0RERDMhEQAAAAAAAAARESM0"
"VmZmVUMhEQAAAAAAAAERI0Xf////9UMhEQAAAAAAAREiNW//////1TIREAAAAAAAERI0bf/////+VDIRAAAAAAAREjRX///////mQyERAAAAABESI1bv////"
"//91MhEQAAAAAREjRX////////dUIhEAAAABESI1bv///////9ZDIREAAAAREjRn///+3////WQyERAAABESI1fv///X////5lMhEQAAAREjRt////d+///+"
"dTIREAABESNFfv///nfv///3VCIRAAAREjRt////d37///92QyEQABESNFf////nd+////1kMhEQAREjRt////13ff///+ZDIREBESNFfv///nd33////nUy"
"ERAREjRt////////////91MhEQESJFf/////////////dTIREREjRu/////////////9VCIRERI1b//////////////VQiEREiRe//////////////5UIhER"
"Ik3////nd3d3fv///+VCERESJO////12ZmZn3////1MhERES3////2VUREVd////9DIREREjNFVVVEMzMzRFVmVUMhEQAREiM0RDMyIiIjM0REMyIREAEREi"
"IiIiIiESIiIiIiIREQAAERERERERERERERIiERERAAAAAAERESIiIiIiIiIhEREQAAAAEREiIzMzMzMzMzIiEREAAAERIjNERFVVVVVEQzIhERAAAREjRVZm"
"d3d3dmZlRDIREAAREjTf//////////7VQyERABEiNe////////////5UIhEBESNG7////////////9QyEQERI1bv////////////5TIRARIkV///////////"
"///1QhEREjRt////53d3fv////VCERESNG3////nd3d+////5UIRERI1bv///9d3d37///91MhEREjV+////13d33////mQyEREiRX7////////////XVDIR"
"ESNGz///////////53dUIhERI0bf///////////+dlMiEREjRu/////////////nUyIRESNX7////Xd3d/////1TIhESI1fv///8d3d33////VMhERIkV///"
"//d3d3fv///9UyEREjRt////53d3d+////1TIRESNG3////nd3d+////9kMhERI0bv/////////////mQyEQEjRu/////////////9UyERASNG7/////////"
"///9VDIREBEkX////////////9VDIREAESPf//////////7WVDIhEQARIjRWZmd3d3ZmZVQzIhEQABESIzREVVVVVUREMyIREQAAAREiIzMzMzMzMzIiIREQ"
"AAAAERESIiIiIiIiIhEREQAAAAAAAAARERESIiIiIREREAAAAAAAARERIiIzMzMiIhERAAAAAAERESIzNEREREMyIhEQAAAAEREiM0RVZmZmVUMyIREAAAAR"
"EiNFVn3u//7tZUMiERAAAREiNFbf///////9VDIREAAREiNF3//////////lQyERABEiNG3///////////1TIREBESNF3////////////2QyERESI1b/////"
"////////1DIRERI0bv////53d9/////VMhERIjV/////53d3fv///+UyEREjRt////53ZmZ9////5DIRESNG7////XZVVW3////UMhERI1f////3dlRFVmd3"
"ZUMiERIkXf////d1RERVZmZUQyEREiRd////52VERFVmZVQyIRESNG3////nZURFVmZmVDIhERIkbv///9d2VVZu///9MiEQEiRe////53ZmZ9////0yERAR"
"I13////3d3d3////9TIREBEjVv////53d9/////UMhEQESNG/////////////lQiEQARIjXf///////////mQyERABESNF3//////////mQyIREAAREjRW7/"
"///////VQyIREAABESI0Vm3u///tZlQyIREAAAAREiM0VWZmZlVUMyIREQAAAAEREiM0REREQzMiIREQAAAAABERIiIzMzMiIiEREQAAAAAAAREREiIiIiIR"
"EREAAAAAAAABEREiIiIiIiIhEREQAAAAABERIiMzMzMzMyIiEREAAAABESIzREVVVVREQzIhERAAAAERI0VWZnd3ZmZVQzIhEQAAERI03///////7tZlQyER"
"EAARIjXv/////////+ZUMhEQAREjRu///////////9VDIREBESNW7///////////9lMhEQESI1f////////////+VDIRERI0bf////////////9kMhEREjRt"
"////53d9/////9UyERESNW7////Xd3ff////1TIRERI1fv///9d3d3/////VQhERIkV+////d3Z3fv///9VCIREjRs////93Zmd+////xTIRESNG3////ndm"
"Z3////91MhERI0bv///+d2Znz////2UyEREjV+////12Znff///+ZDIREiNX7////Hdmd+////1UIhESJFf////3d3d9////9lMhERI0bf///+d3d37////V"
"QyEREjRt////53d9/////2UyIRASNG7////////////3VDIREBI0bv///////////9VDIREAEjRu///////////2VDIhEQARJF///////////WVDIhEQABEj"
"3///////7tdlRDIhEQAAESI0VmZmdmZmVUQyIhEQAAAREiM0RFVVRERDMiIREQAAAAERIiMzMzMzMyIiEREQAAAAABEREiIiIiIiIREREQAAAAAAAAERESIi"
"IiIiIiIiEREQAAABERIiMzMzMzMzMyIhERAAAREiM0RFVVVVVEREMyERAAAREjRVZmd3d3ZmZlRDIREAERI03////////////zIhEAESI17////////////0"
"MhEBESNG7////////////kMhEBESNW7////////////UMhEBEiRX/////////////UMhEREjRt////////////9TIREREjRt////53d3d3d2VDIREREjV+//"
"//13d3d3dlQyIRAREjV+////13d3d3dlMyERARIkV+///////////kMiERARI0bP///////////UMhEQARI0bf///////////EMhEQARI1bv//////////9U"
"MhEQARI1fv//////////5TIhEQASI1fv///8d3d3d2VDIhEAASJFf////3d3d3dlVDIREAASNG3////nd3d3dmVDIhEAASNG3////nd3d3d2VDIhEAASNG7/"
"///////////0MhEAASNG7////////////kMhEAASNG7////////////kMhEAARJF/////////////UIhEAARI9/////////////TIREAARIjRWZmd3dmZmZV"
"QyIRAAAREiM0RFVVVVRERDMiERAAABESIjMzMzMzMzMiIhEQAAAAERESIiIiIiIiIhEREQAAAAAAARERIiIiIiIiIhEREQAAABERIiMzMzMzMzMiIREQAAER"
"IjNERVVVVUREMyIREAABESNFVmZ3d3ZmZlQzIRAAERI03///////////4yERABEiNe///////////+QiEQERI0bv///////////UMhEBESNW7///////////"
"1DIRARIkV////////////1QhERESNG3///////////9DIREREjRt////53d3d3ZkMiEQERI1fv///9d3d3dlRDIREBESNX7////Xd3d2VUMiERARIkV+////"
"d3d3dmVDIREAESNGz//////////9QiERABEjRt//////////9TIhEQARI0bv//////////UyERAAESNX7//////////kMhEQABIjV+//////////5CIRAAAS"
"JFf////3d3dmVEMhEQAAEiRd////53ZlVEMyERAAABI0bf///+dlRDMyIREQAAASNG7////XVDMiIRERAAAAEjRu////1kMiIREREAAAABIkXv///2VDIhER"
"EAAAAAARI1////9UMiEREAAAAAAAESPf///+QyIREQAAAAAAABEiNFVVVDIhERAAAAAAAAAREiMzQzMiERAAAAAAAAAAARESIiIiEREAAAAAAAAAAAARERER"
"EREQAAAAAAAAAAAAAAAAEREREiIiIiIREREQAAAAAAARERIiIzMzMyIiEREQAAAAABERIiM0RERERDMiIREQAAAAERIiNEVWZmZmVUQyIREQAAAREiNEVn3u"
"//7uxlQyIREAABESI0Vt7////////WQyIREAERIjRW7//////////mQyERABESNFz////////////lQyEQERI0XP/////////////VMhEBEiNW//////////"
"////9TIRERI0bv/////Xd33/////VCIRESNX/////nd3d3f////lQiERI0bf///+d3d3d33d3d1UIhESNG7////Xd3d3d3d3d3VCIREjV/////d3ff//////"
"/VMiESJF3////3d37///////1TIhEiRd////53d+///////3UyERI0bf///+d3f///////51MhESNG7////Xd3d3d+///+ZTIREiRu////53d3d33////mQy"
"EREkXv////d3d3d/////1kMhERI13/////13d+/////9VCIRESNG//////////////91MhEREiRd/////////////+ZDIREREjRe/////////////lQyERAR"
"IjRd////////59//1TIREAERIjRW3u///u13ff/9QyERAAERIjNFVmZmZlVVVVRDIREAAAERIiM0REREREMzMzIhERAAAAEREiIjMzMzMiIiIiEREAAAAAER"
"ERIiIiIiIiEREREQAAAAAAEREREREREREREREREREQAAABERIiIiIhERERIiIiIhERAAARESMzRDMyIhEiIzRDMyERAAAREjRFVVRDIiIjRFVVRDIREAERI0"
"3////kMiI0X////0IhEAESI17////VQzNFz////kMhEBESNG7////VQzRG3////lMhEBESNW7///92VERW7////VMhEBEiRX////92VVVn7////FMhEREjRt"
"////53ZmZ3////91MhEREjRt////53d3d3////91MhEREjVu////13d3d9////5kMhEREjV+//////////////5kMhERIkV+//////////////1kMhERI0bP"
"//////////////dkMhERI0bf//////////////dUIhERI0bv//////////////dTIRERI1fv/////////////+ZTIRESI1fv///8d3d3fv///9ZDIRESJFf/"
"///3d3d3fv///9ZDIRESJF3////ndmZnf////3VCIRASNG3////nZVVnf////3UyERASNG7////XVERW3////mQyERASNG7////WVERW3////mQyERASJF7/"
"//91QzNG7////VMiEQARI1////9UMzNF7///9kMhEQARI9////5DMiI0////9DIhEAARIjRVVVQzIiIjRFVVQyIREAAREiMzQzMiIRIiMzQzMiERAAABERIi"
"IiIhERERIiIiIhEQAAAAEREREREREREREREREREQAAAAAAEREREREREQAAAAERIiIiIhERAAABESIzNDMyERAAAREiNFVVRDIREAARIjT////0MhEAERI0z/"
"///0MhEAESI13////lMhEBESNF7////VMhEBESNG7////VMhEBESNX////91MhEBEiRX////91MhEREjRt////5kMhEREjRt////5kMhEREjVu////1kMhER"
"EjV+////dUMhERIkV/////dUIhERI0V/////dTIRERI0bf///+ZDIRERI0bf///9ZDIRERI0bv///9ZDIRERI1fv///3VCIRARI1f////3UyERARI13////m"
"QyERARI13////lQyERARI17////VMhEQARI07///9kMhEQARI0////9DIhEAAREjRFVVQyIREAABESMzQzMiERAAABERIiIiIhEQAAAAEREREREREQAAAAAA"
"AAAAAAAAEREREREREQAAAAAAAAAAARESIiIiIREAAAAAAAAAABESIzNDMyIREAAAAAAAAAARIjRFVVQyIREAAAAAAAABESNO////0yERAAAAAAAAARIkXv//"
"/9QiEQAAAAAAABESNG////9kMhEAAAAAAAAREjV////+ZDIRAAAAAAABESJF3////mQyEQAAAAAAAREjRt////5kMhEAAAAAABERI0bv///9ZDIRAAAAAAAR"
"EiNX7////VQiEQAAAAARERI0V/////dUIhEAAAEREREiNGz////nUyIRAAEREREiIzVt////5lMhEQARERIiIjNFfv///+ZDIREBERIiMzM0Rn7////WQyER"
"AREjNERERFZ/////dUIhEBESNFVmZlVnf////nUyERARIk7///53d9////5kMhEQESNO////d3fv///9VDIRABEjT////9d9////9lMhEQARI07/////////"
"/+VDIREAESNN//////////5kMiEQABEiNe/////////mVCIREAAREjRe///////9ZUMhEQAAAREjRW3v//7tZUMyEREAAAERIjRFVmZmVUQyIREQAAAAEREi"
"M0REREMyIhERAAAAAAEREiIjMzMiIhEREAAAAAAAERERIiIiIREREQAAAAAAAAEREREREREREREREREREQAAAAARESIiIiIiESIiIiIiIREQAAABERIzNEQz"
"IiIiMzREMzIhEQAAAREjRFVVVDMzM0VVVVRDIhEAABESNN////5ENEVd/////9IRAAARIjXv///9VUVW3/////0yEQABESNG7////WZmff/////UMhEAAREj"
"Vu////d3d9/////9UyERAAESJFf////3d33/////1UMhEQAREjRt////53ff////92QyERAAERI0bf///+d9/////3ZTIhEQABESNX7////X3/////dlQyER"
"AAAREjV+////3f////93VDIhEQAAESJFfv///+/////9dlQyEREAABEjRs///////////XZTIhEQAAARI0bf//////////92UyIREAAAESNW7///////////"
"11MiERAAABEjV+///////////+dUMhEQAAASI1fv///////////9ZDIREAAAEiRX//////59/////mUyERAAABIkXf/////Xd/////91QiEQAAASNG3////3"
"d3fv////5kMhEAAAEjRu////13d3f////+ZDIREAABI0bv///9dlZn7////9UyERAAASJF7///92VEVt/////lMhEQAAESNf////ZUREVu/////TIREAABEj"
"3////lQzM0Xv////4yEQAAARIjRVVVRDIiI0RVZlVDIhEAAAERIjNEQzIiIiIzNERDMiERAAAAEREiIiIiERERIiIiIiIREAAAAAERERERERERERERIhEREQ"
"AAAAAAABEREREREREAAAAAAAAAEREiIiIiEREAAAAAAAARESMzRDMiERAAAAAAAAERI0RVVUMyERAAAAAAAREjTf///+MhEQAAAAAAESI17////UMhEAAAAA"
"AREjRe////1DIRAAAAAAERI0bv///2UyERAAAAABEiNX////9lMhEQAAAAERI0Xf///+ZDIREAAAABESNG3////mQyERAAAAAREjVu////1kMhEQAAAAERI1"
"fv///9ZDIREAAAABEiRX7///92QyEREAAAARI0bP////dUMiERAAAAESNG3////nVDIhEREAABEjRu////51QyIhEREAARI1fv///9dkQzIiEREAEiNX7///"
"/HZUQzMiIREBIkV/////d3ZlVEMyIRESNG3////nd3d2ZVQyEREjRt////////////0yERI0bv///////////0MhESNG7///////////5TIREjRu////////"
"///+UyEREkX////////////UMhERI9////////////0yERESI0VmZ3d3dmZmVDMhERESIzREVVVVVUREMiERABESIjMzMzMzMzMiIREQABEREiIiIiIiIiIR"
"ERAAAAABEREiIiIhERERESIiIiEREQAAAAERIiMzMzIiIhIiIjMzMiIREQAAAREiM0REREMyIiIzREREQzIhEAABESI0VmZmZVQzMzRVZmZlVDIREAAREjT/"
"/////9RERF7/////9DIRABESNG///////WVFZ///////UyERAREjV///////1mVn7//////lMhEQESJF3//////9dmd///////1UIREREjRu///////nd33/"
"/////9VCERERI1bv//////53d///////91MhERESNX////////d33///////dTIRERIkV////////nd+///////nUyERESNG3///7v//53z//+7///5kMhER"
"EjRt///+7///d+///v///9ZDIRERI1bv///e///3f//97////WQyERESNX////3v//9+//9/////dUIhEBIkV////93///3//+f////3UyIRASJFf////X//"
"/////X////51MhEQEjRt////d//////+ff///+ZDIREBI0bv///3f//////X7////WQyERASNG7///53//////d+////dUIhEAEjVv///+d//////nf////3"
"UyERABI1b////Xf/////d3////5kMhEQASNd////137////nff///+ZDIREAEjTf///3d+////x33////VMiEQABI07///53fv///nd+////1DIREAASI///"
"/+ZW3///xlb////1MiEQAAESM0VVVURFVmZVREVVZVQzIREAABESIzREMzM0REQzMzREQzIhEQAAARESIiIiIiIjMyIiIiIiIiEREAAAARERERERERIiIhER"
"IiIREREQAAAAAAABEREREREREREREREREREAAAARESIiIiIhERESIiIiIREQAAERIjNERDMiIiIiMzMzMhEQAAERI0VWZVRDIiIzRFVUQyERABESNN////ZU"
"MzM03///9CIRABEiNe////5lRDRF7///5DIRAREjRu/////XVERW7///5TIRAREjVu/////3dlVn////1TIRARIkV//////+d2Z9////xTIRERI0bf//////"
"13d9////dTIRERI0bf//////93d+////ZTIRERI1fv///////nd+///+ZDIRERI1fv///////9d////+ZDIRESJFfv////////d////9ZDIRESNGz///////"
"//7f///3VCIRESNG3//////////////3VCIRESNG7////f/////////3UyERESNX7///5+/////////mUyEREiNX7///137////////WQyEREiRX////133/"
"///////WQyEREiRd////d3fv//////91QiEQEjRt////d3d///////91MhEQEjRu///+d2Z9//////5kMhEQEjRu///+dlVn3/////5kMhEQEiRe///9ZURW"
"fv////1TIhEAESNf///9VDNFbf////ZDIREAESPf///1QzMzRe////UyIRAAESI0VVVEMiIjNFVmVUMyERAAERIjMzMyIiIiIzNEQzIhEQAAARESIiIiERER"
"IiIiIiIREAAAABEREREREREREREREREREAAAAAAAABERERIiIiIiERERAAAAAAAAERESIiMzMzMiIhEREAAAAAARESIjNEREREQzIiEREAAAABESIjRFVmZm"
"ZlVEMiEREAAAERIjRFZ97v//7dZUMiERAAAREiNFbf////////1kMiERABESI0V+//////////5kMiEQAREjRd////////////5UMhEBESNF3///////////"
"//1TIRARIjVv/////////////+ZDIRESNG7/////13d+/////2QyEREjV/////53d3d+////91MhESNF3////ndmZnff////1TIREjRu////12ZVVnf////3"
"UyERI1f////3dlVVZ3////91MhESRd////93VURWff////ZTIRIkXf///+dlVEVn3////mQyESJG3////nZVRVZ+////1UMhEiRd////13ZVVnf////3UyER"
"IkXf///+d2Zmd9////5kMhERI13////3d3d33////3VCIRESNG/////9d3fv////5kMhEREjRu/////////////mUyIREREjVv///////////+ZUMhEQERI0"
"XP//////////1lQyERAAERI0Vu////////5lQzIREQABERI0RW3u///u1lVDIhERAAABERIzRFVmZmZVRDMiEREAAAABERIjM0REREQzMiEREQAAAAABEREi"
"IjMzMyIiERERAAAAAAABERERIiIiIhERERAAAAAAAAABEREiIiIiIiIhEREQAAAAARESIjMzMzMzMyIhERAAAAERIjNERVVVVURDMiEREAAAERI0VWZnd3dm"
"ZlQzIREQABESNN/////////uZUMhEQABEiNe///////////VQyERAREjRu///////////9UyERAREjVu///////////+VCIRARIkV/////////////ZDIRER"
"I0bf///+d3ff////xDIRERI0bf///+d3d/////ZDIRERI1bv///9d3d/////ZDIRERI1fv///9d3ff///+ZDIRESJFfv///3d33////9VCIRESNGz///////"
"/////2QyERESNG3////////////VQyERESNG7///////////1UMhEQESNX7//////////9VDIhEQEiNX7////////+1lQyIREAEiRX////93d3dmVEMiERAA"
"EiRd////53ZlVEMyIhERAAEjRt////52VEMzIiEREAAAEjRu////11QzIiIRERAAAAEjRu////1kMyIREREAAAAAEiRe////ZUMiEREQAAAAAAESNf////VD"
"IhERAAAAAAAAESPf///+QyIREQAAAAAAAAESI0VVVUMiEREAAAAAAAAAERIjM0MzIhEQAAAAAAAAAAARESIiIiEREAAAAAAAAAAAABERERERERAAAAAAAAAA"
"AAAAAAAAEREREiIiIiIREREAAAAAAAARERIiIzMzMyIiEREQAAAAABERIiM0RERERDMiIREQAAAAERIiNEVWZmZmVUQyIREAAAAREiNEVn3u///t1lQyIREA"
"ABESI0Vt/////////WQyIREAERIjRW7//////////mQyIRABESNFz////////////lQyEQERI0XP/////////////VMhEBEiNW7/////////////5kMhERI0"
"bv/////Xd37/////ZDIRESNX/////nd3d37////3UyERI0Xf///+d2Zmd9/////FMhESNG7////XZlVWd/////dTIREjV/////d2VVVnf////3UyERJF3///"
"/3dVRFZ9////9lMhEiRd////52VURWff///+ZDIRIkbf///+dlVFV37////WQyESJG7////XdlVWd/////dTIhEiRe////53dmZ33////mQyEREjXf////d3"
"d3ff////xUMhERI1b/////13d+/////mQyERESNG7////////////+dUMhERESNW////////////51QyERAREjRd///////////nZDIhEQAREjRW7///////"
"/9dlQyERAAERIjRWfe7/////93ZDIhEQAAERIjNFVnd/////52QyERAAAAEREiM0RWff////5UMhEQAAAAEREiIzRVfv////1CIRAAAAAAERESIjNFbv////"
"0hEQAAAAAAARERIiNEVWZVQzIREAAAAAAAAREREiIzREQzIhEQAAAAAAAAABERESIiIiIiERAAAAAAAAAAAAERERESEREREAAAAAAAABEREiIiIiIiIiIRER"
"AAAAABERIiMzMzMzMzMiIRERAAABESIzRERVVVVUREMyIREAAAERI0VWZmd3d2ZmVUMiERAAERI03//////////tVDIhEQARIjXv///////////mQyERAREj"
"Ru////////////5UMhEBESNW7////////////9UyEQESJFf/////////////5TIRERI0bf///+d3d3/////1QhEREjRt////53d3fv///+ZCIRESNW7////X"
"d3d+////5UIRERI1fv///9d3d3/////VMhERIkV+////d3d37////2QyEREjRs/////////////9VDIRESNG3////////////+ZTIRERI0bv///////////9"
"dUMhEREjV+///////////ndkMiEQEiNX7//////////9d1QyERASJFf////3d+////52UyIREBIkXf///+d37////3ZDIhEQEjRt////53d/////5lMhEQAS"
"NG7////Xd3/////mUyERABI0bv///9dnff////1TIREAEiRe////dmZt/////lMhEQARI1////9lVVbv////QyERABEj3////lRERe/////TIRAAESI0VVVU"
"QzM0RVZVVDIREAAREiMzRDMyIiMzREQzIhEAAAEREiIiIiIiIiIiIiIREQAAABEREREREREREREREREQAAAAAAABERERIiIiIhEREQAAAAAAERESIiMzMzIi"
"IREQAAAAARESIzNEREREMyIREQAAABESIzRVVmZmZVQzIREQAAERIjRWbe7//+7WVDIREAAREiNFbv///////9VDIREAERI0bf/////////+UyERAREjRd//"
"/////////9QyEQERI13////////////kMhEBEjRu/////Hd+////9DIRARI0b////3d3d/////QyERESNd/////nd3fd3d1UMhEREjXf/////+13d3d2VDIR"
"ERI13/////////53dkMiERESNG7//////////nZTIhEREjRt///////////3UyIRESI0Z8///////////lQiEREiNGd33v////////5kMhERI0Vnd3d33v//"
"///+ZDIRESNO7u79d3d37////mQyEREjX////nd3d+////5UIhERJF/////Xd33////2UyERESNf////////////5kMhEBEjXv///////////mUyERARI0X/"
"/////////+ZUMhEQERI0Xv////////1lQyERABESI0Vt7v///tdlQzIREQABESI0RVZmZmZVRDIhERAAABESIjNEREREQzIiEREAAAABERIiIzMzMyIiEREQ"
"AAAAABERESIiIiIhERERAAAAABERESIiIiIiIiIiIhEREAABESIjMzMzMzMzMzMiIREQAREiM0RFVVVVVVVERDMhEQARIjRWZmd3d3d3ZmZUQyERERI0////"
"//////////8yERESJM//////////////5DIRESNN//////////////5DIRESNO//////////////1DIRESNO//////////////VCERERJP//////////////"
"QyERERI0Vnd37////Xd3ZUMiEQARIjRWd3////93dlRDIhEQAREiNFZ3////53VEMyIREAABESI0V9////52VDIiEREAABESI0V9////51QyIREQAAAAERI0"
"V+////1lMyEREAAAAAERI0V+////1kMiERAAAAAAERI0V/////dkMhEQAAAAAAERI0Z////+dUIhEQAAAAAAERI0bf///+ZTIREAAAAAAAERI0bf///+ZDIR"
"EAAAAAAAERI0bv///9ZDIREAAAAAAAERI0b////8VCIRAAAAAAAAARI0b////mUyERAAAAAAAAARI03////mQyERAAAAAAAAARI03////lMiEQAAAAAAAAAR"
"Ej7////UMhEQAAAAAAAAAREiNFVVRDIREAAAAAAAAAABESIzRDMyIREAAAAAAAAAABERIiIiIhERAAAAAAAAAAAAEREREREREQAAAAAAAAAAABERERERERER"
"EREREREREAAAEREiIiIiEREREiIiIiEREAARESMzRDMyIREiIzNDMyERAAERI0RVVUQyIiIzRVVUQyERAREjTf///+QyIjRf////QiEQESI17////UMyM0z/"
"///kMhEREjRe////1UMzRd////5TIRERI0bv///3VDM0bv///9UyEREiNX////91QzRW7////FMhERI0Xf///+dUM0V/////dTIRESNG3////mVDRGf////2"
"UyEREjRu////1lREVt////5kMhERI1fv///9ZURFff///+ZDIREiRX7///92VERX7////WQyERI0XP////dlRFZ+////dUIhESNG3////nZURWf////3UyER"
"EjRu////52VFV8////9lMhERI0bv///9dlVWff///+ZDIRESNG7////XZlZ37////VQyEQEjRu////13Znd/////dTIREBI0bv///+d3d37////mQyERASJF"
"7////9d3z/////dTIhEAESNd/////////////VQyERABEjRu///////////+ZDIhEQARIjVv//////////1kMiERAAERI0Vu////////5lQyIREAAAERIzRW"
"3u///u1lVDIhERAAABERIjRFVmZmZVRDMiEREAAAABERIjM0REREQzMiEREQAAAAABEREiIjMzMyIiERERAAAAAAABERERIiIiIhERERAAAAAAAAERERERER"
"EREREREREREQAAEREiIiIiEREREiIiIiEREAARIjM0QzMiIRIiMzMzMiERAREjRVVVRDIiIjNFVVVDIhEBEjT////9QzIjRN////8yEQESNf////5UMzRW//"
"///TIRERI17////mVDRG3////lMhERIkXv///+dURFfv///9QyEQEiRd////92VVbf///+ZDIRASJF3////3ZmZ+////dUIhEBEjV/////d2Z3////5kMhEQ"
"ESNX/////Xd37///91QyERARI1bv///9d3f////mQyIRABEjRu////13fv///3VDIREAESNG3////nd////9ZTIhEQARI0Z////+d+////dUMhEQABEiRX//"
"//93////1kMiERAAESI1f////n3///51QyERAAAREjV+///+3v///WQyIRAAABESNG7////////3VDIREAAAERI0bf///////9ZDIREAAAABEiRX////////"
"dUIhEQAAAAERI1f///////1kMhEQAAAAAREjVu//////51MiERAAAAABESNG7//////VQyERAAAAAAAREjXv/////mQyEREAAAAAABESNN/////2QyIREAAA"
"AAAAAREjRVZmZlQyIREAAAAAAAABERIzREREMyIREAAAAAAAAAARESIjMzIiIREQAAAAAAAAAAERESIiIhEREAAAAAAAAAARERERERERERIiIhERERERERER"
"AAARESIiIiIiIiIjMyIiIiIiIiIREQABESIzREMzMzM0REQzMjMzRDMyIREBESM0VWVUQzRFVmZVRDNEVVVUMiEQESI+////9VRW/////VRV3////jIRARI0"
"3////mZmz////9ZWbv///8MhEREjXf///+d3fv////12d////+UyERESNd////53d//////Xd9////1TIRESJF3////nd9/////9d37///9kMhERIkXf////"
"d37/////13f////mQyEQEiRc////93f//////Xfv///3VCIRASJFf////nff/////+d////+ZTIREBIkV////+d+//////19////1kMhEQEiRX////98///u"
"///X7///51QiERASJFfv///37//37//9fv///WQyERABIkV/////f///fv//7f///3ZDIREAEiRX/////v//1+///u///+dTIhEAARI1f///////937/////"
"//1kMhEQABEjV+///////nfv//////51QiERAAESNX7//////9d+///////WQyERAAARI1fv//////d37//////3VDIREAABEjRu//////53fv/////+ZTIh"
"EAAAESNG7/////93d9//////1kMhEQAAARI0bf/////nd33/////5lMiEQAAABESNd/////9dmff/////VQyERAAAAERI03/////dlVt/////2QyIRAAAAAB"
"EiTf////5URF3////+UyIREAAAAAERI0RWZmVUMzRFVmZlQzIREAAAAAABESM0RERDMiIzNEREMyIREQAAAAAAEREiIjMyIiIiIiMzIiIREQAAAAAAABERES"
"IiIRERERIiIhEREQAAAAAAAAAAABERESIiERERERERERERAAAAAAEREiIjMiIiIiIiIiIhEQAAAAAREiM0REQzMyMzNEQzIhEQAAAAERI0VWZlVEM0RVVVVD"
"IhEAAAABEj7/////ZVVV3////+IREAAAARI0/////9dmZ/////4yERAAAAESNO/////nd3/////kMhEQAAABEjRv/////Hfv///+VDIRAAAAARI0bv////19"
"////9lMiEQAAAAESJFf/////3////8VDIREAAAABESNW7/////////1kMiEQAAAAAREjRn/////////nVDIREAAAAAERI0Z+///////+dkMiERAAAAABESNF"
"d///////52UzIREAAAAAEREjRXfv/////3dkMiERAAAAABESI0Z3//////93ZDIhEQAAAAAREjNWf///////12QyIREAAAABESI0Z+///////+dkMiEQAAAA"
"AREjRX3////////9ZTIhEQAAABESI1bf/////////nVDIREAAAAREjRt////9+/////WQyERAAABESNFf////9ff////9lMhEQAAAREjVv////53f/////1U"
"IhEAAAESNF7////nd33////+ZDIRAAABEjTv////d2Z3/////9QyEQAAERI9/////WVVVu/////kMhEAAAES3////+VUREXf/////SIRAAABEiNFVVVUMzM0"
"RVZlVEMhEQAAAREiMzRDMyIiIzNEREMyIRAAAAAREiIiIiIiESIiIiIiIREQAAAAARERERERERERESIiERERAAAAABEREREREREREREREREREAAAEREiIiIi"
"IiERIiIiIiEREAABESMzREMzIiIiMzRDMyIRAAERI0RVZVVDMzM0RVVUQyERABEi3/////VEM0Ve////8yEQARI0/////3ZURW3////0MhEAESNN/////nZW"
"Z/////1DIRABEjRu////53d37////lQiEQARIkXf////13fv///+ZDIREAERI1f////+d9////9lMiEQABESNG7////3f////3VDIREAABEjRX/////u////"
"1kMiERAAAREjVu/////////WUyIREAAAERI0bP///////+dUMhERAAABESJFfv//////92QyIREAAAABEiNGz//////3ZTIhEQAAAAAREjRn7/////11QyER"
"AAAAAAERIkV9/////XZDIhEQAAAAAAESI1d////+dlMiERAAAAAAABESNW3////nZDIREAAAAAAAAREjVt////51MiERAAAAAAAAERI0bv///9ZDIREAAAAA"
"AAABESNW/////FQyERAAAAAAAAAREjRv///+dTIhEAAAAAAAAAARI03////mQyERAAAAAAAAAAESNN////5TIhEQAAAAAAAAABESPv///9QyERAAAAAAAAAA"
"AREiNFVVRDIREAAAAAAAAAAAAREiM0QzMiERAAAAAAAAAAAAAREiIiIiEREAAAAAAAAAAAAAEREREREREQAAAAAAAAAAAAAAERESIiIiIiIiIiIRERAAAAAB"
"ERIiMzMzMzMzMzIiIREAAAABESIzREVVVVVVVEQzIhEQAAABEjNFZmZ3d3d3ZmVUMhEQAAAREjz/////////////0yEQAAARI03/////////////0yEQAAAR"
"I07////////////+QyEQAAARI17////////////9QyEQAAARI07////////////VQiEQAAARI0////////////5kMhEQAAARIjRWd3d3/////+ZUIhEQAAAR"
"EjRFZ3d//////nVDIREAAAAREiNFV3fv////52QyIREAAAAREiM0Z37/////dlMiERAAAAAREiNFZ9/////9dUMhERAAAAAREiNGff/////XVDIhEQAAAAER"
"IjRX3/////52UzIREQAAAAERI0Vn/////+dlQyIREQAAABESI0Z//////ndlQyIREQAAABESNGfv////93dlQzIhEQAAAREjRX7/////d3dmVDMhEQAAAREj"
"Vu/////3d3d3ZUMiEQAAARI0bv////////////0yEQAAARI13/////////////0yEQAAERI9//////////////UyEQAAERI+//////////////QyEQAAERI/"
"/////////////+QyEQAAARIjRWZmd3d3dmZmVUMhEQAAAREiNERFVVVVVUREQzIhEAAAABESIjMzMzMzMzMzIiEREAAAABERESIiIiIiIiIiIRERAAAAAAAA"
"AREREREREREAAAAAARESIiIiIhERAAAAAREiMzREMzIREAAAABESNEVVVUQyERAAABESNN/////9IhEAAAESI17/////QyEQAAERI0X/////9DIRAAAREjRv"
"/////kIhEAABEiNd///ndUMhEQABESNF3//+dkMiEQAAERI0bv//1lQyERAAAREjRu///WQyIREAABESNW///3VDIREAAAESI1f//+dTIhEQAAERIkXf//5k"
"MiERAAAREjRt///WQyERAAABESNG7//9VDIREAAAERI1b///dUIhEQAAARIjV///51MiEQAAAREiRc///mQyERAAABESNF3//+ZDIREAAAERI0bv//1kMhEQ"
"AAAREjRu///VQyERAAABESNW///3VCIREAAAESI1f//+dTIhEAAAARI0Xf//5lMiEQAAABEjRt///mQyIRAAAAESNG7//9ZDIhEAAAARI0b///dlMiEQAAAB"
"EjVv//52VDIREAAAESNc///ndUMhEQAAARI13/////0yERAAABEjXv/////DIREAAAESNO/////0MhEAAAARIk/////+QyEQAAABESNEVVVUQyERAAAAAREj"
"M0RDMyERAAAAABERIiIiIiEREAAAAAAREREREREREAAAAAAAARERERAAAAEREREREQABESIiIhERAREiMzMyERAREj7/9DIRERI07/9DIRERI07/9UIhERI0"
"3//UMhERI13//UMhERI13//EMhESI13//VMhESI13//VMhESI13//VMhESI13//VMiESI13//VMiESI13//VMiESI13//VMiESI13//VMiERI13//VMiERI1"
"3//VQiERI13//VQiERI13//VQiERI1b//lQiERI0b//lQiERI0b//lMiERI0z//lMhERI0X//lMhERIkX//kMhEREjT//kMhEREiP//jIREBESMzMyIREBER"
"IiIiERAAEREREREQAAABEREREAAAAAABEREREREREQAAAAAAERIiIiIiEREAAAAAERIjNERDMiERAAAAERIzRVVVVDIhEAAAAREj3/////0yERAAABEjTv//"
"//9TIREAAAESNP/////1QhEQAAARI0/////+VCIRAAABEiRWff//1UIhEAAAESI0Z+///VQiEQAAAREjRX7//3UyERAAABESNFf///ZTIREAAAERI0V///5k"
"MhEQAAAREjRd///mQyERAAABESNG3//9ZDIREAAAERI0bv//1UIhEQAAAREjVv//91MiEQAAABEiNX///2UyERAAAAESJFf//+ZDIREAAAERI0Xf//5kMhEQ"
"AAAREjRu///VQyERAAABESNG7//3VCIRAAAAERI1b///ZTIREAAAERIkV///5kMhEQAAAREjRd///mQyERAAARESNG3//9ZDIREAABESI0bv//1UIhEQAAER"
"I0V///91MiEQAAERIjRn///2UyERAAARIjRWf//+ZDIREAABEjRWff//5UMhEQABESPf/////VMiEQAAERI+/////8QyERAAAREj7/////VDIREAABESP///"
"//5DIREAAAARIjRVVVVDIhEQAAABESIzREQzIhEQAAAAAREiIiIiIREQAAAAAAEREREREREQAAAAAAAAAAABEREREREQAAAAAAAAERESIiIRERAAAAAAABER"
"IiMzIiEREAAAAAARESI0REQyIREQAAAAEREjNF3d1DMhEQAAABERIzVv///lMiERAAABESI0bv///mQyERAAAREiNG7////9VCIREAAREjRd/////+ZDIREA"
"ERIjVv//7v//ZTIREAERI0b///d///5UIhEAESNF7//3d+//5kMhEBESNN///Xd9///UMhERESPf//1lVm7//kMhEBESPv/+VURF3//0MhERES7//1RDM0X/"
"/+MhEAESI0RDMyIzNEQzIREAERIiMyIiIiIzMiIREAARESIiERERIiIhERAAABEREREREREREREQAAAAERERERERERERAAAAERESIiIiIiIRERAAABESIjMz"
"MzMyIiERAAERIjM0REREQzMiEQABEt//////////4hEAARLv/////////+IRAAERIjM0REREQzMiEQAAERIiMzMzMzIiIREAABEREiIiIiIiEREQAAAAERER"
"ERERERERAAAAABEREREQAAAAERESEREQAAAREiIiIhEQAAESIzQzIhEQARHf///jIhEAERI////TIRAAESNP//4yEQABEiNO//0hEAAREiMzMyERAAARESIi"
"IhEAAAARERERERAAAAAREREREAAAAAABERESIiIiERERAAAAAAERESIiMzMyIiEREQAAAAERIiM0REREQzIhEQAAABESI0RVZmZmVUMiERAAAREjNFbe7//+"
"7VQyIREAARIjVv////////xDIhEAERI03/////////9kMhEAESJN///////////lMhEBESNO///+d33////lQiEBESNN7u7nd3fv///mQyEREiNGd3d3d3f/"
"///mQyEREjRWd33u///////WQyEREjRn7//////////WQyERI0V///////////91QyERI0bv///9d37///51QyERI17///93d37///51QyERJF////13d3//"
"//51QyERJF////53ff////52QyERJF/////////////9QyERI17////////////8QiERI03//////n7////1MhERIjRe7//td3fe//7UMhEREiNFVmZlVVVV"
"ZVRDIREBESIzREREQzNERDMyERAAEREiIjMyIiIiIiIhERAAAREREiIiIiIiIhEREQAAAAEREREREREAAAAAAAAAABERIiIiIREQAAAAAAAAAREiMzMzMiER"
"EAAAAAAAERIjRFVUQyIREQAAAAAAERI07///5DIhERAAAAAAESJF////5TMiEREQAAABESNG////1UMyIhERAAABESNX////dlRDMiEREAABEiRd///+d2VU"
"QzIhEQAREjRu///+d3d2VUMiERAREjVu///+d+//7VQyIREREjV////97////+ZDIRERIkV///////////5UIhERI0bf///////////UMhERI0bf////////"
"///VMhERI0bv////d33////lQhERI1f////3d3f////lQiESJFf////Xd3f////lQiESJFf///93d3f////VQiESNG3///53d3f////VMhESNG7///53d33/"
"//9lMhESNG7///93d37///5kMhESNW/////nd+////1UIhESNW///////////+ZDIRESNc///////////9UyIRESNN///+///////VQyERASNO///n3/////"
"1UMhERASI////Xd+//7WVDIhEQARI0RVVVVWZmVUMyIREAAREiM0REREREQzIhERAAARESIiIiIjMyIiIREQAAABERERIiIiIiIREREAAAAAAAARERESIiIh"
"ERERAAAAAAERESIiIzMiIhEREAAAARERIjM0RERDMyIREQAAEREiM0RVVmZVRDIhEQAAERIjRVbe///u1UMyERABESI0Vu///////9QyIRAREiNF3///////"
"//1DIREREjRd//////////9TIRERIkXf///+d33////UIhERI0bv///Xd3fv///UIhESJF3///93Zmfv///TIhESNG7///53ZVZnd2VDIRESNG////12VVVm"
"ZlVDIRESNW////x2VVVmZlQyIRESNd////d2ZWZ3dlQyIRESNd////d3Zn3///4yIRASNG////d3d3////0yERASNG////53d+///+QyERASI13/////////"
"/1QyEQARI0Xv////////9UMhEQAREjRe///////+VDIhEQAREiNFbe///u1lQyIREAABESI0RVZmZVVDMiERAAAAERIiM0RERDMyIRERAAAAARESIiMzMiIi"
"EREQAAAAABERESIiIhERERAAAAAAAAAAAAAAAAEREREREREAAAAAAAAAAAARESIiIiEREAAAAAAAAAARESIjMzMyIRAAAAAAAAARERIjRFVUQyERAAAAAAAR"
"EREiNM////QyEQAAAAAREREiI0Xf///0MhEAAAAREREiIjRW7///5TIRAAABEREiIzNFZ+///+UyEQAAEREiM0RFVnfv///VQhEAARESM0VWZnd3////dTIR"
"ABERI0Vm7//tff///3UyEQAREjRW/////+3///91MhEBESNFb//////////+ZDIRAREjVv///////////WQyERESNF7///////////1kMhEREjVu////x33/"
"///3VCIRESJF3///93d37///91MiEREjRu///+d3d+////dTIRERI1b////Xd3fv///mQyERESNX////x3d37///1kMhEREjXf///3d3d////3VDIRARI13/"
"//93d37///91MiEQESNd////53fv////ZTIREBEjRv///////////mQyERARI0b///////////5UMhEQERI13//////+///9UyERABESNF7/////ff//9UMh"
"EQABESNFzv/+13z///QyERAAARESNEVWZlVVVVVDIhEQAAARESIzRERERERDMiERAAAAARESIiMzIiIiIiIREAAAAAAREREiIiIiIhEREQAAAAAAABERERIi"
"IiEREREAAAAAARERIiIzMyIiEREQAAABEREiMzREREMzIhERAAARESIzRFVmZmVUMyEREAAREiNFZt7//+7VRDIREAERIjRW7///////ZTIhERESI0bf////"
"/////EMhERESNG3//////////lQyEREiRd////53ff///9QyEREjRu///9d3d+///9UyERIkXf///3d3d+///+UyERI0bv///////////9UyERI0b///////"
"/////9UyERI1f////////////9QyERI13///93d3d3d3d1QyERI13///93d3d+7u7VMiERI0b///93d3fv///kMhERI0b////nd37///9UMhERIjXf//////"
"////1TIhEBEjRe/////////9VDIREBESNF3///////9lQyERABESI0Vt7v//7WVUMiERAAERIjRFVmZmVUQzIREQAAAREiIzREREQzMiEREAAAABERIiIzMz"
"IiIRERAAAAAAERERIiIiIREREQAAAAAAAAEREREREREQAAAAARERIiIiIhERAAAAEREiIzNDMyIREAABERIjRFVVVDIhEAABESI0Vt7//tMhEQAREiNG7///"
"/+MhEQERIjRu/////9QhEQERI0XP/////lQiERESNFfv///ndlMhEREiNWfv///XdlMhEREjTf///////lMhEREjXv///////UMhEREjX////////UMhEBEj"
"X///////9UIhEBEjVn7///13ZDIREBEjRn7///12QyIRABEjRn////dlQyERABEjRt////dkMiERABEjRt///+dUMhEQABEjRu///+ZTIhEQABEjVu///9ZD"
"IREAABEjV////9ZDIREAABEjV////3UyIREAABEjXf///mUyERAAABEjXf///mQyERAAABEjXv///lQiEQAAABEjTv///UMhEQAAABEjT///9DIhEQAAABES"
"NEVVQzIREAAAAAERIzMzMiERAAAAAAEREiIiIhERAAAAAAAREREREREQAAAAAAAAAAAAAAAAEREREREREREAAAAAAAAAABERERERIiIiEREAAAAAAAARERER"
"EiIiMzMyIREAAAAAABERESIiIjNERUQyIRAAAAABEREiIjMzREVv//8yERAAAAEREiMzREVVZn7//+MhEQAAARESI0RVZmd3fv///TIREAAAERIzRW3u///v"
"///9QyERAAAREjRW7////////+dUMhEQAAESI1b/////////52UyIRAAAREjRv//////////1kMhEQAAERI13////Xd+///9ZDIREAABEjRu///9d3fv///m"
"QyERAAERI0b///93d37///5UIhEAABESNG////53fv///2UyERAAAREjVt//////////5kMhEQAAESI1d+/////////mVCIREAAREjRXff///////+dUMiEQ"
"AAERI0bP///////td2VDIREAABEiRX///+d3d3d3dlQyERAAERI0bv///nd3d3d3ZUMhEQABESNG///////////nZDIREAARIjV+///////////9VCIRAAES"
"NFfP///////////2QyEQABEjRn7////////////UMhEAARI1b///53d3d9////1DIRAAESRe///nd3d3d////2QyEQABIkX////Xd3d37///5UIhEAASJN//"
"///////////9UyERAAESRf////////////1UMhEQABEjTv///////////VQyERAAARIjXf///////+7GVDIhEQAAERIzRWZmZmZmZlVDMiERAAAAERIjNERE"
"RERERDMiEREAAAABERIiMzMzMzMzIiEREQAAAAABERESIiIiIiIiERERAAAAAAAAAREREREREQAAAAAAAAABERIiIiIREQAAAAAAAAERIjMzMyIhERAAAAAA"
"AREiNEVVRDIhERAAAAAAERI07///5DIhERAAAAABEiRf///9UzIhEREAAAERI0b////VQzIiEREAABESNX////dlQzMiIREAARI0Xf///ndlVEMyIREBESNG"
"7///53d2ZlQyIREREjVu///+d97//lQyIRERI1f////d/////2QyEREiRX///////////lMhERI0bf//////////9kMhESNG3///////////ZDIREjRu////"
"53fv///2QyERI1f////3d33///9lMhEiNX////13d9////ZDIRIkV////3d3fv///mQyESNG3///53d37///1kMhEjRu///+d3d+///9VCIRI0bv///nd3z/"
"//91MiESNG////13d9////ZTIREjVv///9dmfv///mQyERI13///92Zn7///5UMhESNN///+dlZu///9UyEREjTv///mVVb///9UMhERIj////5URN////Qy"
"IRARIzRVVUQzNEVVVDIhEQERIjMzMzIiMzMzMiERABERIiIiIiIiIiIiIREAABERERERERERERERERAAAAAAEREREREQAAAAARESIiIREQAAABESIzMzIhEQ"
"AAERIjRERDIhEAABESPf///zIREAERI07///5CIRABESNP///+QyEQERIjT////UIhEBESNFZ3d2UyIRAREjRWd3dlMiEQESI17///9TIhEREjRe///+UyIR"
"ERI0b////VQiERESNX////1UIhERI0Xf///3UyERESNG3///91MhEREjRu///+ZTIRERI1bv///mQyEREiNX////1kMhERIkV////9VDIRASNG3///91MiEQ"
"EjRt////ZTIREBI0bv///mQyERASNG////1kMhEQEjRv///9VCIRABI03///9kMhEQASJN////VDIREAESPv///kMhEQABEiNFVUQyIREAAREiMzMzIhEQAA"
"EREiIiIhERAAAAEREREREREAAAAAAAAAAAARERERERAAAAAAAAABERIiIhERAAAAAAAAERIjMzMiERAAAAAAAREiNEREMiEQAAAAAAERI9////MhEQAAAAAR"
"EjTv///kIhEAAAAAERI0////5DIRAAAAAREiNP///9QiEQAAAAERI0Vnd3ZTIhEAAAABESNFZ3d2UyIRAAAAARIjXv///1MiEQAAABESNF7///5TIhEAAAAR"
"EjRv///9VCIRAAAAERI1f////VQiEQAAABEjRd////dTIREAAAERI0bf///3UyERAAABESNG7///5lMhEQAAAREjVu///+ZDIREAAAESI1f////WQyERAAAR"
"EjRX////1UMhEAAAERI0bf///3VCIRAAABESNG3///91MhEQAAAREjVu///+ZTIREAABESI1f////WQyERAAAREjRX////1kMhEQAAERI0bf///3VCIRAAAR"
"EiNG3///91MiEQAAERI0V+///+ZDIREAABEiNGfv///mQyERAAERI0Vn////1UIhEAABESNGfv///2UyERAAARI07/////5UMhEQAAESNP/////2QyIRAAAB"
"EjT/////xTMhEQAAARIj7v/+1UMyERAAAAERI0RVVUQyIREQAAAAERIjMzMzIhERAAAAABERIiIiIhEREAAAAAABEREREREREQAAAAAAAAEREREREREAAAAA"
"AAAAAAARESIiIiEREAAAAAAAAAABESIzMzMiEREQAAAAAAAAERIjRFVUMyEREQAAAAAAABESNO///+QyERERAAAAAAARIkX////UMiEREREQAAABESNG////"
"1UMiIhEREQAAAREjV////3VDMzIiIREQAAESJF3///52VEREMzIhEQAREjRu///+dmVVVVRDIREAERI0bv///ndmf////9IRABESNX////13d/////0yEQAR"
"IkV////8d3/////UMhEAESNG3///53f////9UyIRABEjRt///+d+////xUMhEQARI1bv///n7///92UyIRAAESNX////7////3dUMhEQABIkV/////////12"
"UzIREAASNGf////////+dlMiERAAEjRt/////////3ZTIhEQABI0bv/////////XUyIREAASNG7/////////91QyERAAEjVv////9+////1kMhEQABI1f///"
"/nff///+ZDIREAASNd////d3fv///9UyERAAEjTf///nd33////lMhEAABI07///52Zn7///9DIREAASI////+VUVe////4yEQAAESM0VVVEQ0RVVVVDIhEA"
"ABESIzRDMzMzM0RDMiERAAARESIiIiIiIiIiIiEREAAAAREREREREREREREREQAAAAAAAREREREREQAAABERIiIiIREQAAEREiMzMzIREAABESNEVVRDIREA"
"ERI03///8yIRABEiNe///+QyEQERI0X////lMhEBESNG////1TIRARIjXP///8UyERESNF3///91MhEREjRu////ZTIRERI1bv///mQyERESNX////1kMhER"
"IkV////9ZDIRESNG3///91QiEREjRt////dTIhERI0bv///mUyERESNX7///5kMhERIjV////9ZDIRESJFf////VQyEREjRt////dUIhEBI0bf///2UyERAS"
"NG7///5kMhEQEjRv///9ZDIREBI0b////VQiEQASNN////ZDIREAEiTf///1QyERABEj7///5DIREAARIjRVVEMiERAAERIjMzMyIREAABERIiIiIREQAAAB"
"ERERERERAAAAAAAREREiIiIiIiIiEiIiIREREAAAARESIiIiIjMzMiIiIiIiIhERAAAREiM0REREREREMzM0REMzIhEQAREiNFVVVVVmZmVVVVVWVVQyIREB"
"ESNO///Xd97/7XZmfe//5UMiERESNF///9fv////533/////VDIRERI0b///7v/////+3//////lMhERIjXf/////////////////+ZDIREjRt//////////"
"////////9kMhESNG7////nff/////Hfv///2QyERI1b////3d37///93d9////ZTIRIjV////9d3fv///nd37///9kMhEiRX////d3d////9d3fv///mQyES"
"NG3///53d3////13d+///9ZDIRI0bv///nd33///93d37///1UIhEjRu///+d3ff///3d3z///91MiESNG////13d+///+d3ff///2UyERI1b////XZ3////"
"13d+///+ZDIREjXf///3Zmf////XZn7///5UMhESNN///+dlbf///3ZWfv///VMhERI07///5lVd////ZVVv///1QyEREiP////lRE7///5URN////QyIRAR"
"IzRVVUQzNFVVVEMzRFVVQyIREBESIzMzMyIzM0QzMiIzMzMyIREAEREiIiIiIiIiIiIiIiIiIiEREAABEREREREREREREREREREREREQAAAAERERIiIiIiIR"
"EREAAAARESIiIiIiMyIiEREAABESIzREQzREREMyIREAERIjRVVVVVVmZVQyIREBESNO///Xd97//lQyIRERI0X///19/////1QyERESNG///+7//////lMh"
"ERIjXf//////////9kMhESNG3///////////ZDIREjRu////53fv///2QyERI1b////3d33///9lMhEiNX////13d9////ZDIRIkV////3d3fv///mQyESNG"
"3///53d37///1kMhEjRu///+d3d+///9VCIRI0bv///nd3z///91MiESNG////13d9////ZTIREjVv///9dmfv///mQyERI13///92Zn7///5UMhESNN///+"
"dlZu///9UyEREjTv///mVVb///9UMhERIj////5URN////QyIRARIzRVVUQzNEVVVDIhEQERIjMzMzIiMzMzMiERABERIiIiIiIiIiIiIREAABERERERERER"
"ERERERAAAAAAEREREiIiIREREAAAAAABEREiIiMzIiIRERAAAAERESIzNEREQzMiEREAABERIjNEVWZmVUQyIREAABESI0VW3v//7WVDIhEQAREiNFbv////"
"//5kMiERERIjRd/////////mQyERERI0Xf/////////+VDIRESJF3////nd+////ZDIRESNG7///13d37///1TIREiRd////d3d37///1TIREjRu///+d2d3"
"////1TIREjRv///9d2Z3////1TIREjVv///8d2d3////1TIREjXf///3d2d9////ZDIREjXf///3d3d+///+VDIREjRv///3d3d////2UyEREjRv///+d33/"
"///lQyEREiNd//////////5kMhEQESNF7////////+ZDIhEQERI0Xv///////VQyIREAERIjRW3v//7WZUMiEREAAREiNEVWZmVUQzIhERAAABESIjNEREQz"
"MiEREQAAAAEREiIiMzIiIhEREAAAAAAREREiIiIREREQAAAAAAAAERERIiIiIiIREREAAAAAARESIiIiIjMyIiEREAAAABESIzRERERERDMyIREAAAERIjRV"
"VVVVZmZVQyIREAABESNO///Xd+7/7VQyIREAERI0X///5+/////lQyERABESNG///+7//////lQiEQARIjXf///////////EMhEBESNG3///////////1TIR"
"AREjRu////93ff///9VCEQERI1b////3d3f////lQiEBEiNX////13d3////5UIhARIkV////3d3d////9VCIRESNG3///53d3f////VMhEREjRu///+d3d9"
"////ZTIRERI1fv///3d3fv///mQyEREiNX/////nd+////1UIhERIkV////////////mQyERESNGz///////////1TIhEREjRt///////////VQyERARI0bv"
"///u/////9VDIhEQESNW////537//tZUMiERABEjV////9d3dmVUMyIREAARI1f////HZVVEMyIREQAAESNd///+dlRDMyIhERAAABEjXf///nVDMiIREREA"
"AAARI17///5kMiIREREAAAAAESNP///9VDIhEREAAAAAABEjT////UMiEREAAAAAAAAREjRFVUQyIREQAAAAAAAAAREjMzMyIREQAAAAAAAAAAEREiIiIhER"
"AAAAAAAAAAAAEREREREREAAAAAAAAAAAAAAAEREREiIiIiIhEREQAAAAERESIiMzIiIiIiEREAAAEREiMzREREREQzMhEQAAEREjNFVWZlVVVVRDIREAEREj"
"RVbv/+13f///QyEQAREjRW/////+fv//9DIRAREjRW///////v///lMhEBESNG///////////9UyERESNF3///////////1TIRERI1bv///8d9////91MhER"
"IkXf///3d3fv///3UyEREjRu///+d3d+////ZTIRESNW////13d37///5kMhERI1f////Hd3fv///WQyEREjXf///3d3fP///3ZDIRESNd////d3d+////dU"
"IhERI13////nd+////91MhEREjRv///////////mUyERESNG///////////+ZDIREREjXf//////////1kMhERESNF7////+3///91QiEQAREjRc7//tff//"
"/3UyIRABESI0VWZ3d+////ZTIREAAREiM0RVZ37///5kMhEQAAEREiMzRWfv///VQyERAAABERIiM0V////8UyERAAAAARERIjRd////ZDIREAAAAAARESI0"
"3///9UIhEQAAAAAAEREjPv///kMhEQAAAAAAABERIzRVVEMiERAAAAAAAAARESIzMzMiERAAAAAAAAAAEREiIiIhERAAAAAAAAAAABERERERERAAAAAAABER"
"ESIiIREREQAAABERIiIiIiIiEREAABESIzREQzMzMiERABESI0VVVVVVRDIREAERI07//9d97/0yEQERI0X///1////TIREREjRv///v///1MhEREiNd////"
"///+UyERESNG3///////5TIRARI0bv///////kMhEBEjVv/////XZUMhEQEiNX////93ZUMiEQASJFf////XZUMiERABI0bf///nZUMiERAAEjRu///+dUMi"
"EREAASNG7///5lMyEREAABI0b////WQyIREAAAEjVv///9VDIREAAAASNd////ZTIhEQAAABI03///5kMhEQAAAAEjTv///lMiERAAAAASI////+QyERAAAA"
"ABEjNFVUQyERAAAAAAERIjMzMyIREAAAAAARESIiIiEREAAAAAAAEREREREREAAAAAAAAAAAEREREiIiIREREAAAAAARERIiIzMyIiEREAAAABERIjM0RERD"
"MiIREAAAERIjNFVWZmVUQyIREAARESNFZt7//+7VQyIRAAERI0Xf///////8QyERABEjRd/////////1MhEQERI13//////////kMhEBEiRe///9d37///5D"
"IRARI0b///93d33u7tQyERESNM/////u13d3dUMhEREjRv///////+13VDIRERI0bv////////51QyERESNGfv////////5UMhERIjRnd97///////ZDIRES"
"JFZ3d3d93////2QyEREjT///93d33///9UIhERI07///53d+///+UyERESNN//////////5kMhERESNe/////////8UyIRABEjRe///////+ZUMhEQAREjRW"
"3v//7tZUMyERAAEREjNFVmZmVUQyIREQAAEREiM0REREMzIhERAAAAEREiIiMzMiIhEREAAAAAEREREiIiIREREQAAAAAAAREREREREAAAAAABERIiIiEREA"
"AAABERIiMzMyIREAAAEREiNEVUQyEREAABESI0X///QyERAAERIjRd///lMiERABEiNFb///1kMhEQERI0Vt///9ZUMhEBEjRWfv//93ZDIRERI07///////"
"4yERESRf///////9QiERI0X////////UIRESNN////////UyEREiRXf////XdUMhEBIkV9////d2QyIRASI1ff///3ZTMhEQEiNX7///51QyIREBIkV+///+"
"dUMhEQASJFf////XUyIREAEiRd////dlMiERABI0Xf///3dUMhEQASJG7///93ZDIREAEiRe/////tQyERABEjXv/////1MhEQARI07/////5TIRAAESNF//"
"///+QyEQABESNF3v/+5DIhEAAREiNEVVVUMyERAAAREiMzREMyIREAAAARESIiIiIhERAAAAAREREREREREAAAAAABEREREREREREREREQAAARESIiIiIiIi"
"IiIhERAAERIjMzMzIiIzMzMyERABESI0RVVEMzNFVVRDIREBESNO///9RETf///0MhEREjRf///9VVXf///0MhEREjRv///3ZVbv///lMhERIkXf///nZmfv"
"///VMhERI0bf///nZnfv///VMhERI0bv///nd3z///91MhERI1b////Xd33///9lMhESI1f////Xd37///9kMhESJFz///93d37///5kMhESNG3///53d37/"
"//1kMhESNG7///53d3////dUIhESNG7///53d+////dTIhESNG/////Hfv////ZTIRESNG///////////+ZDIRESJF7//////////+VDIRARI13//////u//"
"/9UyERARI0Xv////58///1QyERAREjRd7/7Xd8///0MhEQAREiNEVWZVVVVVVDIhEQABESIzNEREQ0REMyIREAAAEREiIjMyIiIiIiERAAAAAREREiIiIiIh"
"EREQAAAAAREREREREREREREREAAAERIiIiIiIiIiIiIREQABESIzNDMyIiIzMzMiERAREiNFVVRDMzNEVUQyIRAREj7///9URERf///zIRARIk3////VVFbv"
"///TIRERI0b////WZWf///5TIRERI0b////nZn3///1DIRERI0bv///nd3///+ZDIRARI0bv///nd+///8VCIRARI0bf///3d////mQyERARIkXP///3ff//"
"91QyERARIkV////nf///5kMhEQAREjV+///3z///dUMhEQAREjVu///97//9ZDIhEAAREjRu///////3VDIREAAREjRt///////WQyERAAABESNX//////51"
"QiERAAABESNW//////1kMhEQAAABESNG7////+ZTIhEQAAAAESI17////2VDIREAAAAAERI03////lQyERAAAAAAAREjRVZmVEMhERAAAAAAAREiM0REMzIR"
"EQAAAAAAABERIiIiIiEREAAAAAAAAAEREREhERERAAAAAAAAERERERERERESIiERERERERERAAABESIiIiIiIiIiIiIiIiIiIhERAAERIjM0MzMjMzREQzMi"
"MzMzMiERAREiNFVVVEM0RVZlVEM0RFVEMiEQERI+////VUVW////1URV////MhEBEiTf///9ZWbv///9ZVbv///TIRERI0b////Xd3/////XZ3///+UyERES"
"NG////13ff////53ff///VMhEREjRu///9d37////+d37//+ZDIRARI0bv///Xff/////nff///VQiEQESNG7///53//////537///ZDIREBEjRt///+d///"
"////d////VQyERARIkV////n7//u///n3//+ZTIREAESI1f///7f//3f//7e///VQyERABESNX7//+7//3f///7//+ZTIhEQAREjRu//////53//////91Qy"
"ERAAERI0bf/////3d//////+ZTIhEQAAESJF3/////53f/////91QyERAAABESNX/////9d37////9ZDIhEAAAAREjRu////53d+////9lMiERAAAAARIkXv"
"///9Zmbv///9VDIREAAAAAERI07///9lVV7///9UMiERAAAAAAERI0VWZVRDREVWZVQyIREAAAAAABESIzREQzMzM0REMzIhEQAAAAAAABERIiIiIiIiIiIi"
"IhERAAAAAAAAABERERIhEREREiEREREAAAAAAAAAABERERIiEREREREREQAAAAABERIiIiIiIiIiIiEREAAAAAESIzNEQzMzMzMzMiERAAAAERI0VVVVRERF"
"VVRDIREAAAARI+////9lVW7///8yEQAAABEjT////3d37////DIRAAAAESNN////533////UMhEAAAERI0b////nf////VMiEQAAAREjRt////3v///WQyER"
"AAABESNFfv///////mVCIRAAAAERI0V9///////3ZDIREAAAAREjRXf//////3ZUMhEQAAABESNFd9/////3dkMiERAAABESI0Z9//////12QyIREAAAERIj"
"Vn///////nZDIhEQAAAREjRn////////1lMiEQAAAREjRW////7////3UyIRAAABESNG7///59////1UMhEAAAESJF7///53f////mQyEQAAARI03////Xd+"
"////1DIRAAABEj3////XZm3////0MhEAAAES3////mVVVv////0iEQAAARIjRFVVRDNEVVVUQyERAAABESIzMzMzMzM0RDMyIRAAAAARESIiIiIiIiIiIiER"
"EAAAAAEREREREREREREREREAAAAAAAAREREREREREREREREAAAAAEREiIiIiISIiIiIREQAAABESIzMzMyIiIzMzMiERAAAREiNFVVRDMzNEVUQyIRAAAREj"
"7///9UQ0Rf///zIRAAARIk3////VRFbv///TIREAARI0b////WVWf///5TIREAARI0b////XZn3///1TIREAARI0bv///Xd37//+ZDIREAARI0bv///nd9//"
"/9VCIRAAARI0Xf///3d+///2QyERAAARIkV////3d////VQyERAAARIjV////nfv//5kMhEQAAAREjV+///3f///xUMhEQAAAREjRu///33//+ZTIhEQAAAR"
"EjRt///97//3VDIREAAAAREjRn///////mUyIREAAAAREjRX//////91QyERAAAAAREjRX7/////1kMiEQAAAAAREjRX7////+dUMhEQAAAAAREjRX3////9"
"ZDIREAAAAAERIjRXf////3VDIREAAAAAERIzRnf////WQyERAAAAABESI0Vn7///5lMiERAAAAABEiNFZ+///+dUMhEQAAAAABEjT//////3VDIREAAAAAAB"
"Ej3/////9lQyIREAAAAAABEj7/////ZUMiERAAAAAAABEj3v/+5lRDIhEQAAAAAAABESNEVVVEMyIREAAAAAAAAAESIzMzMyIhERAAAAAAAAAAEREiIiIiER"
"EQAAAAAAAAAAAREREREREREAAAAAAAAAAAAAAREREiIiIiIiEREQAAAAAREiIzMzMzMyIhERAAAAERIjREREVEREMyIRAAAAESM0VmZmZmZlVDIREAABESPv"
"/////////9IhEAABEjT//////////0MhEAABEjT//////////kMhEAABEjz/////////5UMhEAABEjRWd3fv////ZTIREAAREiNFd37////9VDIREAAREiNF"
"d9/////WUyIRAAAREiNFd/////51QyERAAAREjNWf////+dkMiERAAERIjRn7////3dUMiERAAERI0V9/////XZTMhEQAAERI0bf////53ZDMhEQAAESNFf/"
"///+d3ZUMhEQABESNG/////nd3dUMiEQABESNe//////////wyEQABEiTf//////////QyEQABEiTv/////////+QyEQABESP//////////+MhEQAAESNEVm"
"ZmZmZlVDMhEAAAERIzRERFVEREMyIREAAAAREiIzMzMzMyIiERAAAAAREREiIiIiIiERERAAAAAAAAAREREREREAAAAAABEREiIiEREAAAAAEREiMzMyIREA"
"AAAREiM0REQyIRAAABESI0Xe//4yEQAAAREjRf///9MhEQABESJF/////TIREAAREjTf////QyERAAESI13//+dUMhEAAREjRe//92QyIRAAERI0bv//dUMh"
"EQAREiNW///mUyIREAERIkV///5kMhEQABESNF3//9VDIREAERIjRu//9lMiERABESNFfv/+ZDIREAARIjVn///lQyERAAESNG3///ZTIhEQABEjX////WQy"
"ERAAARJN///XdUMhEQAAEiTv///nVDIREAABIjV+///VQiERAAARI1Z///1UIhEQAAESNGf//9VCIREAABEjRt///VQiERAAARI0bv//dUIhEAAAESNG7//3"
"UyIRAAABEjVv//91MiERAAARI1b//+dTIhEQAAESNd///nVDIRAAABEjXf//51QyEQAAARI17////kMhEAAAESNO////5DIRAAABEiNf///9QiEQAAAREjRe"
"///DIREAAAAREjNFVEMiEQAAAAEREiMzMyIREAAAAAEREiIiIREQAAAAAAERERERERAAAAAAAAAAABEREREQAAAAAAEREREREQAAAAARESIiIhEQAAABESIz"
"MzIhEAAAAREjT//jIREAABESNN//4yIRAAAREjTf/9QyEQAAERI17//UMhEAAREjRe//VDIRAAERI0bv/lQyEQABESNW//5UMhEAARIjXf/9VCIRABESJF7/"
"/VMiEQAREjRe//ZTIREAERI0bv/2QyERABESNG//5kMhEQARIjXf/+VDIREBESJF3//VQiERAREjRe//1TIREAERI0bv/2QyERABESNG7/5kMhEQAREjVv/+"
"VDIREBESI13//lQyERAREiRd//1TIhEAERI0Xv/2UyERABESNG7/9kMhEQAREjRu/+ZDIREAERI1z//lQyERABESNd//1TIhEQARIkXv/9UyERAAESJF7/9k"
"MhEQABEiRe/+VDIREAAREjX//lMhEQAAERI9//5DIREAABESPf/9MiEQAAABESMzMyIREAAAARESIiIhEQAAAAARERERERAAAAAAAREREREAAAAAAAAAABER"
"EREREQAAAAAAEREiIiIREQAAAAAREiMzMyIREQAAAAESI0RVQzIREAAAAREj3//+VDIREAAAESNO////UyERAAABEjTv///9QyEQAAARI0/////kMhEAABES"
"NGz///1TIRAAAREjRX7//2UyEQAAERIkV+//9lMhEAABESI1f///ZDIRAAAREiRX///mQyEQAAERIkXP//1kMhEAABESNF3//9ZDIRAAAREjRe//92QyEQAA"
"ERI0Xv//dlMhEAABESNF3//+ZTIhAAAREjRX////1CIQAAERI0V33//9QhEAABEiNG3///9TIRAAERIkVv//51QyEQABESNF7//3ZTIhEAAREjVv//51QyER"
"ABESI1f//9ZDIhEQAREjRd///VQyERAAERI0bv//dUIhEQAREiNW///2UyIREAERI0V///9kMhEQABEiNW3//+VDIREAARI0XP///VMiEQABESPf////ZDIR"
"EAAREj3////lQiERAAERI+////VDIREAABESPv/+1UMiERAAABEiNEVEMyIREAAAAREiMzMyIREQAAAAARESIiIRERAAAAAAAREREREREAAAAAAAAAARERER"
"EREREREAAAAAABERERERERERERERAAAAARERIiIiIiIREREREAAAEREiMzMzMyIiIiIhEQABESIzRFVVREMzMzMiERABESNF3v/tZVVURE3iIRABEjTf////"
"/tZmbf/TIRAREj7////////////TIRAREj////////////5CIRAREj/+ZmZu/////+QyERAREt5EREVVbe//5UMiEQABEiMzMzNERVVVRDIhEQABESIiIiIj"
"MzMzMiIREQAAERERERIiIiIiIhEREAAAAREREREREREREREQAAAAAAEREREREREREREAAAAAAAAREREREREQAAAAEREiIiIhEQAAAAERIjMzMiERAAABESM0"
"VUQyIREAABEiPv///jIREAAREjTv///kIhEAAREjX////UMhEAARIkX////UMhEAERI03///9UIhEAERI03///9TIREAERI0Vnd2VDIREAERI0Vnd2VDIREA"
"ERI0X//9QyIREBESI1z//8QyIREBESNF3//2QyERABESNG7//2UyERAREiNX///2UyERAREjRd///mUyERAREjRu///mQyERAREjVu///mQyERARIkXf///m"
"QyERARI0bv///mQyERARI0b////mQyERARI0b////lQiEQARI03////VMhEQARI07////UMhEQARIj/////TIhEAAREjRFVVQzIREAABESMzQzMiERAAABER"
"IiIiIhERAAAAEREREREREQAAAAAAEREREREREAAAARERIiIhEREAABERIjMzMiERAAEREjNEREMiERABESNE3v/UMiEQERI0X////kMhERESNO/////UIhER"
"I03/533/5DIRESNO/nd37+QyEREjX/53fP/UMhERI0//d3//VCERESNO/////kMhEREiNf///9QyIRAREjRe/+VDIhEQAREjNEREMiERAAEREiMzMiIREQAA"
"ERESIiIRERAAAAERERERERAAAAABEREREREQAAEREiIiIREQAREiMzMyIREAESI0REQzIREREjT///8yERESI9///+QyEREjTf///kMhERI07///1DIRESNO"
"///9MhERESP///9DIREREjNEREMiEQAREiMzMyIREAEREiIiIREQAAERERERERAAAAAAAAAREREREREAAAAAAAAAEREiIiIREQAAAAAAABESIzMzIhEQAAAA"
"AAABEiNEVUQyERAAAAAAAREjT///9DIRAAAAAAERI0z///9DIREAAAAAERI03///5TIREAAAAAERI17///5DIREAAAABESI17///1DIREAAAARESNF7///1D"
"IREAAAAREiNFZ3d2UyIREAAAERIjNGd3dlQyIREAABERIzRW///tQyIREAARESM0V9///9QyIREAAREjNGfv///1QyIREAERIjVt////91QyIREQESI0bv//"
"//dlQzIhEQESNF7////ndmVEMiEQESNd////53d2ZVQyERESRe////d3fe7u5CIREjRv///+d3fv//9DIREjRv///+d3f///9DIREiRf////d3////1DIRES"
"Ne/////////+UyERESNG/////////lQyEQESI1b///////xUMhEQERIzRd7v/+1lVDIhEQAREjNFVmZlVEMyEREAARESIzREREMzIhERAAABEREiIjMyIiER"
"EQAAAAEREREiIiEREREAAAAAAAAAAAAAAAARERERERAAAAAAAAAAAAAAABEREiIRERAAAAAAAAAAAAABERIiMzIiERAAAAAAAAAAAAEREiM0RDMhEQAAAAAA"
"AAAAARESI03///4hEAAAAAAAAAABERIjTf//9DIREAAAAAAAAAAREiNN///VQyERAAAAAAAAABESI03//mVDIhEQAAAAAAAAERIjRWd3d2UzIREAAAAAAAAR"
"EiNFZ3d3dlQyERAAAAAAAAERI0Xf////9kMhEQAAAAAAAREiNW//////1UIhEAAAAAAAERI0bf/////+ZDIREAAAAAAREjRX///////mQyERAAAAABESI1bv"
"//////91MhEQAAAAAREjRn////////dUIhEAAAABESI1bv///////9ZDIREAAAAREjRn///+3////WQyERAAABESI1fv///X////5lMhEQAAAREjRt////d+"
"///+dTIREAABESNFfv///nfv///3VCIRAAAREjRt////d37///92QyEQABESNFf////nd+////1kMhEQAREjRt////13ff///+ZDIREBESNFfv///nd33///"
"/nUyERAREjRt////////////91MhEQESJFf/////////////dTIREREjRu/////////////9VCIRERI1b//////////////VQiEREiRe//////////////5U"
"IhERIk3////nd3d3fv///+VCERESJO////12ZmZn3////1MhERES3////2VUREVd////9DIREREjNFVVVEMzMzRFVmVUMhEQAREiM0RDMyIiIjM0REMyIREA"
"EREiIiIiIiESIiIiIiIREQAAERERERERERERERIiERERAAAAAAAAAAAAAREREREREAAAAAAAAAAAAAERESIiIREQAAAAAAAAAAARERIiMzIiERAAAAAAAAAB"
"EREiI0REMyIREAAAAAAAERERIjNO///+IREQAAAAAREREiI0Tf//5DIhEQAAAAERESIjNE3//9VDMhERAAABERIjM0Rd//52VUMiERAAAREiM0RVVnd3d3ZV"
"QyIRAAAREjRWZ3d3d3d3d2VDIREAERI03////////////0MhEAESI17////////////0MhEBESNG7////////////kMhEBESNW7////////////UMhEBEiRX"
"/////////////UMhEREjRt////////////9TIREREjRt////53d3d3d2VDIREREjV+////13d3d3dlQyIRAREjV+////13d3d3dlMyERARIkV+//////////"
"/kMiERARI0bP///////////UMhEQARI0bf///////////EMhEQARI1bv//////////9UMhEQARI1fv//////////5TIhEQASI1fv///8d3d3d2VDIhEAASJF"
"f////3d3d3dlVDIREAASNG3////nd3d3dmVDIhEAASNG3////nd3d3d2VDIhEAASNG7////////////0MhEAASNG7////////////kMhEAASNG7/////////"
"///kMhEAARJF/////////////UIhEAARI9/////////////TIREAARIjRWZmd3dmZmZVQyIRAAAREiM0RFVVVVRERDMiERAAABESIjMzMzMzMzMiIhEQAAAA"
"ERESIiIiIiIiIhEREQAAAAAAAAAAEREREREAAAAAAAABERESIhERAAAAAAABERIiMyIhEQAAAAABERIjREMyIRAAAAAAERIzTv///REQAAAAERIjTv//4yER"
"AAAAEREjTv//VDIREAAAAREjTv/+VUMhEQAAAREiNFZ3ZUMiERAAABESNFZ3dmQyIREAABESNF////9TIhEQAAERI0z////1MiEQAAARIkXf///+VCIRAAAR"
"EjRu////1UIhEAABESNG7////VQiEQAAERI1f////3UyERAAARIkV/////dTIREAAREjRt////5lMhEQABESNG3////mQyERAAERI1bv///9ZDIREAAREjV+"
"////dUMhEAABEiRX////91QiEQAAESNFf////3UyERAAARI0bf///+ZDIREAABEjRt////1kMhEQAAESNG7////WQyERAAARI1fv///3VCIRAAABEjV/////"
"dTIREAAAESNd////5kMhEQAAARI13////lQyERAAABEjXv///9UyERAAAAESNO////ZDIREAAAARI0////9DIhEAAAABESNEVVVDIhEQAAAAAREjM0MzIhEQ"
"AAAAABERIiIiIhEQAAAAAAARERERERERAAAAAAAAAAAAAAAREREREREREREAAAAAAAAAAAERESIiIiIiEREQAAAAAAAAABERIjMzMzMyIiERAAAAAAAAARES"
"M0RVREREMyIREAAAAAAAEREjRd7+1lVd/zIhEAAAAAABERI0Xv///td//0MhEQAAAAERESNF7////////UMhEQAAAAERIjNF////////5UMiERAAABESIzRe"
"/9d+///+dkMyERAAAREiNFZu/Xd3fv/XdlQzIREAARIjRWd3d3ZmZ3d3d2VDIhEAERI03///93ZlZnff///0MhEAESJF7////ndlVmfv///lMhEBESNG7///"
"/9d2Vmfv///lMhEBESNX7/////d2Znf////VMhEBEiRX//////53d33////FMhEREjRt///////Xd33///91MhEREjRt///////3d37///9lMhEREjV+////"
"///+d37///5kMhEREjV+////////13////5kMhERIkV+////////93////1kMhERI0bP/////////t////dUIhERI0bf//////////////dUIhERI0bv///9"
"//////////dTIRERI1fv///n7////////+ZTIRESI1fv///Xfv///////9ZDIRESJFf////Xff///////9ZDIRESJF3///93d+///////3VCIRASNG3///93"
"d3///////3UyERASNG7///53Zn3//////mQyERASNG7///52VWff/////mQyERASJF7///1lRFZ+/////VMiEQARI1////1UM0Vt////9kMhEQARI9////VD"
"MzNF7///9TIhEAARIjRVVUQyIiM0VWZVQzIREAAREiMzMzIiIiIjM0RDMiERAAABERIiIiIREREiIiIiIhEQAAAAEREREREREREREREREREQAAAAAAAAAAAA"
"AAAREREREQAAAAAAAAAAAAAAARERIiEREQAAAAAAAAAAAAABERIiMyIhEQAAAAAAAAAAAAERIjNEQzIREAAAAAAAAAAAAREiNN///+IRAAAAAAAAAAARERI0"
"3///wyERAAAAAAAAABEREjNP//5EMhEQAAAAAAABEREiI0//9lQzIREQAAAAAAERESIzRFZmZUMyIREAAAAAARESIzRFVmdmVUMyIREAAAABESIjRFZnd3d2"
"ZUMyEREAAAERIjRFZ97v//7dZUMyERAAAREiNFbf////////1lMyERABESI0V+//////////5lMiEQAREjRd////////////5kMhEBESNF3/////////////"
"1TIRARIjVv/////////////+ZDIRESNG7/////13d+/////2QyEREjV/////53d3d+////91MhESNF3////ndmZnff////1TIREjRu////12ZVVnf////3Uy"
"ERI1f////3dlVVZ3////91MhESRd////93VURWff////ZTIRIkXf///+dlVEVn3////mQyESJG3////nZVRVZ+////1UMhEiRd////13ZVVnf////3UyERIk"
"Xf///+d2Zmd9////5kMhERI13////3d3d33////3VCIRESNG/////9d3fv////5kMhEREjRu/////////////mUyIREREjVv///////////+ZUMhEQERI0XP"
"//////////1lQyERAAERI0Vu////////5lQzIREQABERI0RW3u///u1lVDIhERAAABERIzRFVmZmZVRDMiEREAAAABERIjM0REREQzMiEREQAAAAABEREiIj"
"MzMyIiERERAAAAAAABERERIiIiIhERERAAAAAAAAAAAAAAAAAREREREQAAAAAAAAAAAAAAARERIiEREQAAAAAAAAAAAAABERIiMiIhEQAAAAAAAAAAAAEREj"
"M0QzIhEQAAAAAAAAAAERESM9///+IREQAAAAAAABERERIj3///wyIREAAAAAABEREREiNP//5EMiEREAAAABERERIiIj//9VQzIiEREAAAEREiIiIiMzRERE"
"QzMiIREAARESMzREMzMzREREREMyIRAAERI0RVVVRDMzNFVWZVQyERAREjTf///+QzM0Rf////QyEQESI17////VQzNFz////kMhEREjRe////1UMzRt////"
"5TIRERI0bv///3VDNFbv///9UyEREiNX////91QzRX7////FMhERI0Xf///+dUNEV/////dTIRESNG3////nVERGf////2UyEREjRu////1lREVt////5kMh"
"ERI1fv///9ZURFff///+ZDIREiRX7///92VERX7////WQyERI0XP////dlRFZ+////dUIhESNG3////nZURWf////3UyEREjRu////52VFV8////9lMhERI0"
"bv///9dlVWff///+ZDIRESNG7////XZlZ37////VQyEQEjRu////13Znd/////dTIREBI0bv///+d3d37////mQyERASJF7////9d3z/////dTIhEAESNd//"
"///////////VQyERABEjRu///////////+ZDIhEQARIjVv//////////1kMiERAAERI0Vu////////5lQyIREAAAERIzRW3u///u1lVDIhERAAABERIjRFVm"
"ZmZVRDMiEREAAAABERIjM0REREQzMiEREQAAAAABEREiIjMzMyIiERERAAAAAAABERERIiIiIhERERAAAAAAAAAAAAAAEREREREREREAAAAAAAAAAAERESIi"
"IiIiEREQAAAAAAAAARESIzMzMzMyIREAAAAAAAABERIzREREREQzIREAAAAAAAEREjPv/+Ve//8yIREAAAAAARESI0///lX///QyERAAAAABEREiPf//1W//"
"/kMhERAAAAEREiI03//9Vf//5DIhERAAARESIzNFVVVVVWZlRDIhEQABESIzREVVVEREVWZlRDIhEQAREjRVZmVURDNFVmZmVDIREBESNN////5UMzRW////"
"9DIRARIjXv///9VDM0XP///+QyERESNG7////VQzRG3////lMhEREjRu////dUM0Vu////1TIRESI1f////3VDNFfv///8UyEREjRd////51RERX////91Mh"
"ERI0bf///+dURFZ/////ZTIRESNG7////WVERW3////mQyEREjV+////1lREV9////5kMhESJFfv///3ZURFfv///9ZDIREjRc////92VEVn7///91QiERI0"
"bf///+dlRFZ/////dTIRESNG7////nZUVXz////2UyEREjRu////12VVZ9////5kMhERI0bv///9dmVnfv///9VDIRASNG7////Xdmd3////91MhEQEjRu//"
"//53d3fv///+ZDIREBIkXv////13fP////91MiEQARI13////////////9VDIREAESNG7///////////5kMiERABEiNW///////////WQyIREAAREjRW7///"
"/////mVDIhEQAAAREjNFbe7//+7WVUMiEREAAAEREiNEVWZmZlVEMyIREQAAAAEREiMzRERERDMyIRERAAAAAAERESIiMzMzIiIREREAAAAAAAEREREiIiIi"
"EREREAAAAAAAAAAAAAAAEREREREQAAAAAAAAAAAREREiIRERAAAAAAAAAAEREiIzMiIREAAAAAAAABERIjREQzIREAAAAAAAERESI07///0hEQAAAAABEREi"
"NN///0MhEQAAAAARERIjTf//1UMhEQAAAAEREiI03//mZUMiERAAABERIjNFZ3d3ZUMiERAAABESM0Vmd3d3dlQyIREAAREjRFbe7//+7WVDIREAARIjVv//"
"//////xTIhEAERI03/////////9kMhEAESJN///////////lMhEBESNO///+d33////lQiEBESNN7u7nd3fv///mQyEREiNGd3d3d3f////mQyEREjRWd33u"
"///////WQyEREjRn7//////////WQyERI0V///////////91QyERI0bv///9d37///51QyERI17///93d37///51QyERJF////13d3////51QyERJF////53"
"ff////52QyERJF/////////////9QyERI17////////////8QiERI03//////n7////1MhERIjRe7//td3fe//7UMhEREiNFVmZlVVVVZVRDIREBESIzRERE"
"QzNERDMyERAAEREiIjMyIiIiIiIhERAAAREREiIiIiIiIhEREQAAAAAAAAABEREREREAAAAAAAAAAAARERIiIREQAAAAAAAAAAEREiIzIiERAAAAAAAAARES"
"IzREMyIRAAAAAAAAEREiNE///+IREAAAAAABERIjRf///TIREAAAAAARESI0X//+VDIhEQAAABEREiNE///WVDIhEQAAARESIjRVZ3d2VDMhERAAEREiNEVn"
"d3d3ZUMiERAAERIjRWfe///u1lQyIRABESI0V+///////2VDIREREiNG3/////////xTIhEREjRt//////////5kMhERIkXf///+d33////UMhERI0bv///X"
"d3fv///VMhESJF3///93d3fv///lMhESNG7////////////VMhESNG/////////////VMhESNX/////////////UMhESNd////d3d3d3d3dUMhESNd////d3"
"d3fu7u1TIhESNG////d3d37///5DIRESNG////53d+////VDIRESI13//////////9UyIRARI0Xv/////////VQyERAREjRd////////ZUMhEQAREiNFbe7/"
"/+1lVDIhEQABESI0RVZmZlVEMyEREAAAERIiM0REREMzIhERAAAAARESIiMzMyIiEREQAAAAABERESIiIiEREREAAAAAAAAAEREREREAAAAAAAERERIiEREQ"
"AAAAARESIjMiIREAAAAAERIjNEQyIREAAAAREiNO///9ERAAABESI07//+MhEQAAAREjTv//1DIREAABESI9//5VQyERAAAREjRWZmVDIhEQABESNFZ3dlQy"
"IREAAREjTv///0MiERABESNF7///5TIhEAAREjRv///9UyIRAAERI1f////VMiEQABEiRd////dTIREAARI0bf///3UyERAAESNG7///5lMhEQABEjVu///+"
"ZDIREAASI1f////WQyERAAEiRX////1UMhEAABI0bf///3UyIRAAASNG3///9lMhEQAAEjRu///+ZDIREAABI0b////WQyERAAASNG////1UIhEAAAEjTf//"
"/2QyERAAABIk3///9UMhEQAAARI+///+QyERAAAAESI0VVRDIhEQAAABESIzMzMiERAAAAARESIiIiEREAAAAAAREREREREQAAAAAAAAAAAREREREREREREA"
"AAAAAAEREiIiIiIiIREQAAAAABERIjM0QzMzMiERAAAAAREiM0VVVVVUQzIREAAAEREjRd7/7XZt/9IREAABERI0Xv///////0MhEAABESNF7////////UMh"
"EAAREiNN////////1UMhEQARIjRe/9d33v/ndUMhEQERI0Vnd3d3d3d3dUMhEQESI17//9d33v/+dkMhERESNG///9ff////9lMiERESNW///+7//////mQy"
"EREiRd///////////2QyEREjRt///////////2UyEREjRu////53fv///3UyEREjVv////d3ff///2UyERIjV////9d3ff///2QyERIkV////3d3fv///mQy"
"ERI0bf///nd3fv///WQyERI0bv///nd3fv///VQiERI0bv///nd3z///91MiERI0b////Xd33///9lMhERI1b////XZn7///5kMhERI13///92Zn7///5UMh"
"ERI03///52Vm7///1TIREBI07///5lVW////VDIREBIj////5URN////QyIRABEjNFVVRDM0RVVUMiERABESIzMzMyIjMzMzIhEQABERIiIiIiIiIiIiIREA"
"AAEREREREREREREREREAAAAAAAAAAAEREREREQAAAAAAAAAAABEREiIhERAAAAAAAAAAARESIjMiIREAAAAAAAABERIjNEQzIhEAAAAAAAARESI0T///4hEQ"
"AAAAAAEREiNF///9MhEQAAAAABERIjRf//5UMiEQAAAAERESIzT//9ZUMiERAAABERIiNEVnd3ZUMiEREAARESIzRWd3d3dlQyIREAAREiNFZt7//+12VDIR"
"EAERIjRW7//////+ZUMhERESI0Xf////////5kMhERESNF3//////////lQyEREiRd////53fv///2QyEREjRu///9d3d+///9UyERIkXf///3d3d+///9Uy"
"ERI0bv///nd3d////9UyERI0b////Xdmd////9UyERI1b////Hdnd////9UyERI13///93dnff///2QyERI13///93d3fv///lQyERI0b///93d3f///9lMh"
"ERI0b////nd9////5UMhERIjXf/////////+ZDIREBEjRe/////////mQyIREBESNF7///////1UMiERABESI0Vt7//+1mVDIhERAAERIjRFVmZlVEMyIREQ"
"AAAREiIzREREMzIhEREAAAABERIiIjMyIiIRERAAAAAAERERIiIiEREREAAAAAAAAAAAAAEREREREQAAAAAAAAAAABEREiIhERAAAAAAAAAAERESIjMiIREA"
"AAAAAAABERIjNEQzIhEQAAAAAAERESI0T///4hERAAAAAREREiNE///9MiERAAAAERESIjNP//5UMyEREAABERIiMzT//9ZUQyIREAAREiM0RERVVmZVRDIh"
"EQERIjRVVVVVVmZmZUMhEQERI07///1VVt////QyERESNF////1lZt////QyERESNG////dmZ+///+UyEREiRd///+dmZ+///9UyEREjRt///+d2d+///9Uy"
"EREjRu///+d3fP///3UyEREjVv///9d3ff///3UyERIjV////9d3fv///2QyERIkXP///3d3fv///mQyERI0bf///nd3fv///WQyERI0bv///nd3f///91Qi"
"ERI0bv///nd37///91MiERI0b////8d+////9lMhERI0b///////////5kMhERIkXv//////////5UMhEREjXf//////7///1TIREBEjRe/////33///VDIR"
"EBESNF3v/+133///QyERABESI0RWZmVVVVVUMiERAAERIjM0REREREQzIhEQAAARESIiMzMiIiIiIREAAAABERESIiIiIiERERAAAAAAAAABERERERERERAA"
"AAAAAAERESIiIiIiIREQAAAAABERIjMzMzMzIhERAAAAARESM0REREREMyERAAAAAREjPv//Ve///CIREAAAERIjT///Zu//9DIREAABERI0X//+Zv//9DIh"
"EQARESM03//9Zv//5UMhEQAREjNFZndmZmd3ZUMhEQERIzRWd3dmZnd3dlQyEQESI07///1mZt////QyERESNF////1mZ9////UyERESNG////dmZ+///+Uy"
"EREiRd///+d2d+///9VCEREjRt///+d2d+///9VCEREjRu///+d3fP///3UyEREjVv///9d3ff///3UyERIjV////9d3fv///2QyERIkXP///3d3fv///mQy"
"ERI0bf///nd3fv///WQyERI0bv///nd3f///91QiERI0bv///nd37///91MiERI0b////8d+////9lMhERI0b///////////5kMhERIkXv//////////5UMh"
"EBEjXf/////+7///1TIREBEjRe/////nz///VDIREBESNF3v/td3z///QyERABESI0RVZlVVVVVUMiERAAERIjM0RERDREQzIhEQAAARESIiMzIiIiIiIREA"
"AAABERESIiIiIiERERAAAAAAABEREREREREREQAAAAERESIiIiIhEREAAAARESIjMzMzIiEREAABERIjNERERDMiEREAAREiNEVWZlVEMiERABERIz3/////"
"/8MhEQAREiNN//////9DIhEAERI0Xf/////+QyIRARESNF3//////FMiEQERIjRt/////+ZTIhEBESI0bf/////mQyIRAREiNW7/////1kMiEQERIjVu////"
"/3VDIREBESNFbv////5lQyEREREjRW7////9ZTIhERERI0Vu////92QyIRERESNFfv///+dUMiEREREjRX7////WVDIRERERI0V/////dkMiERARESNFf///"
"/3VDIhEQEREjRW////5lMyEREBERI0Vv///9VDIhERAREiNFz///5lQyIREAERIjRd///+ZDMhERABESI0Xf///VQyIREQAREiNFZnd2ZUMiEREAERIzRWZ3"
"dlRDIhEQABEiNFZnd3ZUQyIREAARIjTv////9UMhERAAESNF7/////VDIREQABEjRf/////1QyEREAARI0X/////5UMhEQAAESNM/////9UyIREAABEjTf//"
"///UMiERAAARI07/////VDIREQAAESI//////0MiERAAABESM0VVVUQzIREQAAAREiMzREQzIiERAAAAEREiIjMzIiEREQAAAAERESIiIiERERAAAAAAERER"
"EREREREAAAAAAAAAEREREREREREREREREREREREAAAAAAAAAAREREiIiIiIREREREREREREREQAAAAAAABEREiIjMzMyIiEREREREiIiEREAAAAAAAEREiIz"
"NEREQzIiIREREiIiIiIREAAAAAARESIzREVVVVRDMiIREiIzMzMiERAAAAABERIjNFXe///tVEMiIiIjNN///SERAAAAAREiNEXv//////VUMyIjNE3//9Mh"
"EQAAABERI0Rf////////VEMzM0Rf//4zIREAAAAREiNF///+d8///9VEMzRV///0MiERAAAAERI0Xv//53d9///mVERFXv//VDIhEQAAABEiNF///9d3ff//"
"5lVEVd///VQyIREAAAARIjXf//93d33//+ZlVWz//9VDIhEREAAAESNF7//+d3d+///nZmZu//5VQyIRERAAABEjRe///nd3f///12Zn7//2VDMiEREQAAAR"
"I0Xv//53d3///3d3fv//ZVQzIhEREQAAESI17//+d3fv//53d9///WVEMyIhEREQABEiNN///9d+///3d3f//+ZlRDMyIiEREAAREjRe////////x3d+//52"
"ZVREMzIiEREAERIjRe//////93d37//3dmZVVURDMiEREBERIzRd7//+13d3ff//d3dmZmZVVDMiEREBESIzRFVmZmZnd9///Xd3fe///tVDMhERARESIjNE"
"RVVWZnf//+d3ff//////1DMhEQARERIjM0REVWZ+//53d+////////VDIREAARERIiMzNEVn7//3d37///d37//+UyIRAAARERIiIzRFbf//13d///93d9//"
"/1QyEQAAERERIiM0Vm///nd37//+d3d///9UMhEAAAERERIjNFb//+d3d////Xd3z//+VDIRAAABERESI0Vu//52Zn3///d3d9///lQyEQAAABERIjNF7//2"
"ZmZ9///nd3fv//5UMhEAAAARESI0Xf//1lVVbv//53d37//9VDIRAAAAEREjNF///mVERW7//+d3d///9kMiEQAAABESI0X//+VUREVd///nd37//+VDIREA"
"AAAREiNO//5VQzM0XP///Xff//5UMiERAAAAERIj3//9VDMzM0Xv///////1VDIREQAAABESLf//1EMyIiM0Xv/////+VEMiEREAAAAREi///kMyIiIiM0Ve"
"7//tVUQyIREQAAAAAREiMzMzIiERIiM0RVVVVUQzIhERAAAAAAERIiIiIiEREREiIzNEREMzIiEREAAAAAAAEREiIiERERERESIiMzMyIiERERAAAAAAAAER"
"EREREREREREREiIiIiERERAAAAAAAAAAEREREREREREREREREREREREAAAAAAAAAAAABEREREREQAAAAAAAAABEREREREREQAAAAAAAAERERIiIiIREQAAAA"
"AAARERIiMzMyIREQAAAAABEREiM0REMyIREAAAAAERESI0Rf///SERAAAAARERIjRF///+MiEQAAABEREiNFXv//5DIhEQAAARESI0Vu///8QyIREAABERIj"
"RW7///5UMhEQAAEREiNEbf///2UzIREAABERIjRW////1UMiERAAEREiNFbv///mVDIREQABERI0Vt////1kMiEREAEREiNFf////mVDIhEQABERI0Vu////"
"1lMyEREAAREiNFf////3VDIhERABERI0Vu////1lQyIREAAREiNFf////3ZDIhERAAERIjVu////51QyIREQABESNFbv///9ZUMhERAAARIjRX////92QyIR"
"EQAAESI0bf///+dUMiERAAABEjRW7////mVDIhEQAAARI0Vu////1lQyEREAAAESNFf////3ZDIhERAAABIjRd////92QyIREQAAASI0bf////dUMiERAAAA"
"EiNG3////nVDIhEQAAABIjRt////51QyIREAAAASI0bf///+dUMiERAAAAEiNG3////nVDIhEQAAABIjRt////91QyIREAAAARI0XP///+dUMiERAAAAESNF"
"f////nVDIhEQAAABEjRW7///91QyIREAAAARIjRu////dUMiERAAAAESI0Xf///9ZDIhEQAAABESNFb////mQyIREAAAAREiNG7///5kMiERAAAAERIjRd//"
"/+ZDIhEQAAABERIjRe///9QyIREAAAABESI0XP///kMiERAAAAARESI0Xv//9DIREAAAAAARESI0T///0yERAAAAAAERESIzREQzIhEQAAAAAAERESIzMzIi"
"EREAAAAAAAERESIiIiIREQAAAAAAAAERERERERERAAAAAAAAAAAREREREREAAAAAAAAAAAAAAREREREREAAAAAAAAAAAERERERERERAAAAAAAAABEREiIiIh"
"EREAAAAAAAABESIjMzMiEREQAAAAAAARESM0REMyIRERAAAAAAAREiP///VDIhERAAAAAAAREjTv//5UMiEREAAAAAEREjTf///VQyIREQAAAAERIjRf///2"
"UzIREQAAAAERIjRu///9VDIhEQAAAAERIjRu////ZTMhEQAAAAERIjRd////1UMhEQAAAAERIjRX////1kMiEQAAAAERIjRX7///5lMyEQAAAAEREjRX7///"
"9lQyEQAAAAERIjRX/////FQyIQAAAAEREjRW7////WQyIQAAAAEREjRW7////mQyIQAAAAEREjRW7////mUyIQAAAAEREjRW7////mUyIQAAAAERIjRW7///"
"/mUyIQAAAAERIjRX7////mUyIQAAAAERIjRX7////mQyIQAAAAERIjRX7////WQyIQAAABERIjRn7////WQyIQAAABERIzVn////91QyEQAAABERI0Vt////"
"9lQyEQAAABESI0V+////5lMiEQAAARESI0V/////1kMiEQAAARESM1bf////dUMiEQAAAREiNFbv///+ZUMhEQAAEREiNFfv///9ZDIhEQAAEREjNW3////2"
"VDIREQABERIjRW7////WQyIREQABERIjRn////5lQyIREAABERI0Vu////1kMiEREAABESI0V////+ZUMiERAAARESM1bv///2VDIhERAAAREiNFb////WQz"
"IREQAAAREiNG7///5lQyIREQAAAREjRd///+ZUMiEREAAAAREjRe///2VDIhEREAAAAREjTv//9lQyIhERAAAAAREj3///VUMiIREQAAAAAREj///1RDIiER"
"EAAAAAAREiM0REMyIRERAAAAAAARESIzMzIiEREQAAAAAAABERIiIiIREREAAAAAAAAAERERERERERAAAAAAAAAAAREREREREAAAAAAAAAAAAAAAAAERERER"
"EREAAAAAAAAAAAAAABERERERERERAAAAAAAAAAAAERERIiIiIhERAAAAAAAAAAABEREiIzMzMiEREQAAAAAAAAERERIjNEREMyIRERAAAAAAABERESIzT///"
"5DIhEREAAAAAAREREiM0X///5DMhEREAAAAAERERIjNF3///1UMiEREQAAABEREiIzRW3///1UMyIhERAAARESIjM0Vn7///dlQzIiEREAEREiIzRFZ37///"
"d2VEMyIREAERIjNEVmd3///+d3ZUQzIREBERI0RWZ3d3///9d3dmVDIhERESI9///////////////9MhERESNN///////////////0MhERESNO//////////"
"/////0MiERESNP///////////////kMhERESNP///////////////kMhERESNP///////////////TMhERESI0Vmd3ff///Xd3ZlRDIhEQERIzRFZ3fv//93"
"dmVUMyIREAERIiNEVnfv//93ZUQzMiEREAEREiIzRWf///52VDMyIhERAAARESIjRWf///5lQzIiEREQAAABEREiNF3///1kMyIhEREAAAAAEREiNF3///ZU"
"MiERERAAAAAAERESI07///VDIiEREQAAAAAAARESIz////QzIhEREAAAAAAAABERIjNEREMyIRERAAAAAAAAAAEREiMzMzIiEREAAAAAAAAAAAARESIiIiIR"
"ERAAAAAAAAAAAAABEREREREREQAAAAAAAAAAAAAAEREREREREAAAAAAAAAAAEREREREREREQAAABERESIiIiIRERAAABERIiMzMzIiEREAARESIzREREMyIR"
"EAEREiNEVVVVQzIREQERIjTv/////jIhERERI0Xv/////UMhERESI0X//////UMhERESI1b/////9kMiERESNF3/////5kMiERESNF3/////5kMiERESNF7/"
"////5UMhERESNF7/////1UMhERESNF//////ZTMhERESNFZ3///+VDIhERESNFZ9///mVDIRERESM1Z///9lQyIREBESM0bv//1UMyEREBESM03//+VEMiER"
"EBESI0///1RDIhERABESI+//9UQyIREQABESLf//1DMiEREQAAERIjNEMzIhEREAAAEREiIzIiIRERAAAAARESIiIhEREQAAAAABERERERERAAAAAAAAERER"
"EREQAAAAAAAAERERERERERERAAAAERESIiIiIiEREQAAEREiIzMzMzIiEREAAREiM0REREMzIhEQAREiM0VVVVVUMyEREBESM9///////jIhEQERI03/////"
"/+QyERARIjTv//////1DIREBEiNe///////UMhERESI0///////1QyERERIjz///////QyIREBESPf//////5DIhEQERIjRFVWVVRDIhEQARESIzREREQzIh"
"ERAAEREiIzMzMyIhERAAAREREiIiIiIREREAAAEREREREREREREAAAABEREREREREREAAAERERIiIiIhEREAABERIiMzMyIiEREAEREiM0REQzMiERABESI0"
"RVVVVDMhERERIjTv/////jIhERESI07/////1DIREREjRf/////9QyERESI0b/////9UMhEREiNN/////+VDIRERIjTf/////lQyERESI07/////5UMhERES"
"NO/////9QyIREREjP//////DMhERERIjRFVVVUMyIREBERIjNEREQzIhERABERIiMzMzIiEREAABEREiIiIiEREQAAABEREREREREREAAAAAAAAAAAABERER"
"EREAAAAAAAAAAAAREREREREQAAAAAAAAAAERESIiIhERAAAAAAAAABERIiMzMiEREAAAAAAAARESIzREMyIREAAAAAAAEREiM0///jIREQAAAAAAEREjNO//"
"/jIhEQAAAAABERIjRe///DIhEQAAAAABERI0Xf//9UMhEQAAAAARESI0Xv//1TIhEQAAAAARESNFb///ZDIhEQAAAAEREiNF3//+VDIhEQAAAAEREjRW7//9"
"VDIREQAAABERIjRd///2UzIREQAAABERIzRu///lQyIREAAAABESI0Vv//9lQyIREAAAARESI0Xf//5lMyEREAAAARESNFbv//1UMiEREAAAEREiNF3///ZU"
"MiERAAAAEREiNG7//+ZDIhERAAAAERIjRW///8VDIhERAAABERIjRd///mVDIREQAAABERI0Vu///VQyIREQAAARESI0XP//9lQyIREAAAARESI0bv//5lMy"
"EREAAAARESNFb///1UMiEREAAAEREiNF3///ZUMhERAAAAEREjNW7//+ZDIhERAAAAERIjRW///8VDIhERAAABERIjRd///mUzIREQAAABERIzVu///VQyIR"
"EQAAABERI0XP//9lQyEREAAAABESI0Xv//5kMiEREAAAABESI1b///1UMiEREAAAABESNF3//+ZDMhERAAAAABESNF7//9VDIhERAAAAABESNF///1QzIREQ"
"AAAAABESNO///lQyIREQAAAAABESNP///UMiEREAAAAAABESLf//5DMhEREAAAAAABERIzNEMzIhERAAAAAAAAERIiMzMiIREQAAAAAAAAERESIiIhEREAAA"
"AAAAAAARERERERERAAAAAAAAAAABEREREREQAAAAAAAAAAAAAAAAERERESIiIiIRERERAAAAAAAAABERERIiIiIiIiIhERERAAAAAAABERESIjMzMzMzMyIi"
"EREQAAAAABEREiIzRERVVVREMyIhERAAAAABEREiM0RVZmZmZlVEMyIREQAAABEREiNEVmze///+7GVEMiEREAABEREiNFVu/////////WVDIhEQAAEREiNF"
"bf//////////5lMyEREAEREiNFbv///////////+VDIhEQAREiNFbv////////////9lQyERARESM0bf/////////////+ZDIhEBESI0V//////9d33/////"
"5lMiERERI0Vu/////3d3d9/////2VDIRERIjRX/////9d3d3f/////xUMhEREjNW7////+d3d3d//////VQyERESNFf/////13Zmd3/////9ZDIRESI0bf//"
"///HdmZ3z/////1kMhERIjVu/////3dmZnff////92QyEREjRW7////+d2VWd9/////3VDIRESNFf/////53ZVZ37/////dUMhESI0Z//////XZlVnf/////"
"5lQyERIjRt/////9dmVWd//////mUyIREiNW3/////d2ZWZ9/////9ZDIhESI1bv////93ZmZ33/////dUMiERIjVu/////ndmZnfv////9lQyEREiNW7///"
"/+d3Znd//////WUyIRESI1bv////53d3d8/////3VDIhERIjRu/////nd3d37////+ZUMhEREiNG3/////d3d33/////dUMiERARI0Xf/////8d37/////5l"
"QyEREBEjRW//////////////9lQyIREAESI0Xv/////////////FQyIREQAREjRW/////////////WRDIhEQABESI0Xf//////////9lRDIhERAAEREiNFbv"
"////////1lRDIhERAAABERIzRVbe7///7WZVQzIhERAAAAEREiI0RVZmZmZlVUQzIhEREAAAABEREiMzREVVVURDMyIhEREAAAAAARERIiIzMzMzMzIiIRER"
"EAAAAAAAERERIiIiIiIiIiERERAAAAAAAAABERERESIiIiERERERAAAAAAAAAAAAAAAAEREREREREREAAAAAAAAAAAARERERERIREREQAAAAAAAAABEREREi"
"IiIiIhERAAAAAAAAAREREiIiMzMzMiEREAAAAAABERESIiMzREREQyIREAAAAAARERIiIzREVW3//TIhEQAAAAARESIzNEVWZ+//9DIhEQAAAAEREiM0VWZ8"
"7///9UMhEQAAAAERIjRFZn3/////9UMhEQAAABESI0Vs3///////5UMhERAAABESNN//////////1kMhERAAABESNO//////////1kMhERAAABESNO//////"
"////dUMhERAAABESNP//////////dUMhERAAABESPf/////////+dUMhERAAABESM0Vnd3/////+dUMhERAAABESI0Vnd9/////9ZUMhEQAAABERIzRWd9//"
"///3ZTIhEQAAABERIjNFd+/////3ZDIhEQAAABERIjNFd//////3ZDIhEQAAABERIiNFd//////nVDIhEQAAABERIjNFfP/////XVDIhEQAAABERIjNGff//"
"///XVDIhERAAARESIjRWfv////93VDIhERAAARESIzRWfv////93VDIhERAAEREiM0Vnf/////93VDMiERABERIjNFVnf/////53ZUMyEREBESIzRWZ33///"
"//13dlQyIREBESNEVnd33/////x3dlQzIREREiNP//////////////9DIREREjRf//////////////9DIhEREjTf//////////////5TIhEREjTf////////"
"//////5TIhEREjTv//////////////1DIREREjTv//////////////VDIREREjP///////////////QyIREREiNEVWZnd3d3d3dmVUMyEREBESIzREVVVVVV"
"VVVURDMiERABERIiMzNEREREREQzMyIhERAAEREiIiIzMzMzMzMiIiEREQAAEREREiIiIiIiIiIiEREREAAAAAAAAAERERERIiIiIhEREREAAAAAAAAAERER"
"EiIiIiIiIiIREREAAAAAAAAREREiIjMzMzMzMyIhEREAAAAAABEREiIzNERVVVREQzIhEREAAAAAERESIzRFVWZmZmZVRDIiEREAAAARERIjRFZn3u///u1m"
"VDMhERAAAAEREiNFVu/////////mVDIhERAAARESI0Vu///////////9VDIhEQAAERIjRW/////////////2VDIREAABESNFb//////////////lQyERAAER"
"IjRe///////////////VMiEQABESI13/////53d+//////1UMhEAAREjRe/////Xd3d+/////9VDIRAAERI03////+d3d3ff/////VQyEQABESNO////93d3"
"d33/////ZUIhEAAREiPv///+d3d3d//////mUyIRAAERIjRFZmZmZ3d37/////1UMhEQABERIzRFVWZmd3fv/////2VDIREAARESIzRFVWZ3d///////dUMi"
"ERAAEREiIzRFVnd+///////WVDIREQABERIiM0VWd3///////9ZUMiEREAARESIjRFZ33///////dlQzIhEQABEREiNEVnfv//////53ZUMiEREAARESIzRW"
"ff///////HdlQzIhERABERIjNFZ+///////Xd2VUMyIREQARESM0Vn///////nd3dlVDMiERABERIjRW3//////3d3d3dlQzIhEQAREiNFbf/////9d3d3d3"
"dlQyIREAERIjRt///////////////0MiERABESNF3///////////////9TIhEQARIjRv///////////////+VDIREAESNF7////////////////lQyERABEj"
"Rf////////////////1TIhEQARI03////////////////8QyIREAESI9////////////////9UMhEQABEiPv////////////////QyIREAAREjNFVmZ3d3d3"
"d3d3ZlVDMhERAAERIjNERVVVVVVVVVVVREMyIREAAAERIiMzNEREREREREQzMyIhERAAABEREiIiMzMzMzMzMzIiIhEREAAAABERERIiIiIiIiIiIiIREREQ"
"AAAAAAAAAAABEREREiIiIiIRERERAAAAAAAAABERERIiIiIiIiIiERERAAAAAAAAERERIiMzMzMzMzMiIRERAAAAAAARERIiMzREVVVURDMyIRERAAAAAAER"
"EiM0RVVmZmZmVUQyIREQAAAAARESIzRVZ97///7tZVQzIREQAAAAERIjRFbv////////5lQyIREAAAAREiNFbv///////////VQyIREAAAERIzRf////////"
"////9lMiERAAABESNF//////////////5UMhEQAAERIjTv//////////////xDIREAABESI9/////9d3fv/////9UyIRAAAREiPv////d3d3fv/////VMiEQ"
"AAARIj/////Xd3d33/////ZTIhEAAAERIzRVZmZ3d3d+/////mQyIRAAABESIzRFVWZ3d3f/////1UMiEQAAARESIzNEVWd3ff/////mVDIREAAAERESIjNE"
"Vu///////+dlMyERAAABEREiIjNFb///////13VDIhEQAAEREREiIzRX//////93d1QyIREAABERERIiM0Xf///////XdUMiERAAERERIiIjNF3////////X"
"VDIhEQARERIiIjM0Re////////91QyIREAEREiIzMzNEVnd37/////1kMiERARESIzNERERFVnd3/////+ZDIhEQERIjRFVVVVVVZ3d//////mQyIREREiNN"
"3d3dZmZmd3f/////5kMhEQERIz3////9dmZ3d9/////9VDIREBESNO/////nd3d3f/////9lQyERAREjTv/////nd3ff/////+ZDIhEQERI03///////////"
"////9lQyEREBESNF///////////////9VDIhEQAREiNN//////////////1kMyEREAAREjRd////////////9lRDIhEQAAERIjRW7//////////WVEMiEREA"
"ABERIjRFbN7v///t1mVUMyIREQAAABERIjNEVWZmZmZlVEMyIhERAAAAARERIiM0REVVVUREMzIiEREQAAAAARERIiIzMzMzMzMyIiERERAAAAAAAREREiIi"
"IiIiIiIhERERAAAAAAAAAREREREiIiIiIREREREAAAAAAAAAAAAAAAEREREREREREREAAAAAAAAAAAAAERERIiIiIiERERAAAAAAAAAAAAERESIjMzMyIiER"
"ERAAAAAAAAAAEREiIzREREMzIhEREQAAAAAAAAEREiM0VVZVVEMyIhEREAAAAAAAERESM0Xv////5EMyIhEQAAAAAAARESI0Xf/////VREMyIREAAAAAARES"
"I0Vv/////3ZVRDMhERAAAAARESI0Vt/////3d9//0yIREAAAABERIjRX/////+d37//UMhEQAAABERIjRW7////+d33//1QyERAAABERIjRW3/////d3f///"
"ZDIREQAAEREiNFfv////13fv//5kMiERAAEREiNFbf////53fv///mQyIREAEREiNFZ/////53ff///9ZDIhEQARESM0Z+////53d/////1kMiERARESI0Vt"
"////53d/////92QyIREBESI0Vs////93d9/////nZDIhERERIzRn////93d37////+dkMiERERIjRW7///93d3f/////52QzIREREjRW3///93d3d//////X"
"ZUMhEREiNF3///93d3d3/////3d1QyIRESI1b///93d3d33/////d3ZDIhERI0Xv/////////////////1MyEREjRf//////////////////VDIRESNF////"
"//////////////9UMhERI0X//////////////////lQiEREjTf/////////////////9QyIRESI+//////////////////VDIREREjNFVmd3d3d37////+d3"
"VDIhERESIzREVVZmd3f/////53ZEMhEREREiIzNERFVmd//////XZUMiERABEREiIjMzRFVn/////8ZUMiEREAAREREiIiIzRW3/////ZUMiEREAAAERERES"
"IiM0Xv////5UMyEREQAAAAERERERIjNO/////kQyIREQAAAAAAERERESIzRVVmVUMyIREQAAAAAAAAEREREiIzREREMyIRERAAAAAAAAAAEREREiIzMzMiIR"
"ERAAAAAAAAAAAAEREREiIiIiEREQAAAAAAAAAAAAAAEREREREREREQAAAAAAAAAAERERIiIiIiIiIiIREREQAAAAARERIiIjMzMzMzMyIiIRERAAAAARESIz"
"M0RERERERDMzIhERAAAAEREiM0RVVVVVVVVVREMiEREAABERIjRVZmd3d3d3dmZVQyIREAABESI0X//////////////TIREAARESNF3/////////////9DIh"
"EAAREiNF7/////////////9UMhEAEREjRW//////////////5UMhEAEREjRX//////////////1UMhEAERIjRt//////////////1UMhEBERI0Vu////////"
"//////ZTIhEBERI0V+//////////////VDIhEBESI0Z/////d3d3d3d3d2VDIhEBESI0bf///+d3d3d3d3d2RDIhEBESNFbv///9d97//+13d1QzIRERESNF"
"fv///93//////+d2QzIRERESNFf/////////////92VDIREREiNF3//////////////2VDIhEREiNG7//////////////9ZDIhEREiNF///////////////+"
"ZTIhEREiNF/////+d3ff/////+ZTIhEREjNFze7/93d3d9//////ZUIhERIjRVZ3d3d3d3d37/////ZUMhERIjRWd3d3d3d3d37/////ZTIhERIzRWd3d3d3"
"Znd37/////ZTIhERI03////9d3Z3d3/////+ZDIhERI03////9d3d3d9/////9VDIRERI03/////d3d3ff/////mUzIRERI03/////7Xd37//////VQyIRER"
"I0X///////////////5lMyERERI0Xf/////////////+ZUMiERARIjRe/////////////mVDIhERAREjNF7///////////1lQyIREQAREiNEXO/////////W"
"VUMiEREQARESIzRWbe7///7WZVQzIhEREAABERIjNEVWZmZmZVVEMyIRERAAAAEREiIzRERVVUREMzIiEREQAAAAERERIiMzMzMzMzIiIREREAAAAAABERES"
"IiIiIiIiIhERERAAAAAAAAERERESIiIiIREREREAAAAAAAAAAAAAERERERIiIiIRERERAAAAAAAAAAERERIiIiIiIiIiERERAAAAAAABERESIiMzMzMzMyIi"
"EREQAAAAABEREiIzNERFVUREMzIhEREAAAABEREiM0RVVmZmZlVUMyIREQAAABEREiM0VWbe7//+7WVEMiEREAAAEREiNEVt/////////mVDIhEQAAEREiNF"
"Z///////////9lQyEREAEREiNFbf////////////ZDIhEQAREiNFbf/////////////lQyERARESM0bf//////////////VDIREBESI0V+/////9d3fv////"
"/UMhERERI0Vu/////3d3d37////9QyIRERIjRX/////9d3d3ff////1DIhEREjNW3/////d3d3d9/u7u7UMiERESNFf/////53d3d3d3d3dlQyERESI0bf//"
"///Xd+7//td3d1QzIRERIjVu/////3f//////+d2VDIhEREjRX7/////7////////9dUMiERESNFf///////////////92QyIRESI0Z////////////////+"
"ZTMhERIjRt////////////////9lQyEREiNW3//////9d3fv/////9VDIRESI1bv/////nd3d37/////1UMhERIjVv/////3d3d3ff/////VQyEREiNW////"
"/+d3d3d9/////2VDIRESI1bv////53d3d33////+ZTIhERIjVu/////Xd3d3fv////5kMiEREiNG7////+d3d3fv////9lQyERERI0Xf/////nd3fv/////m"
"QyIREBEjRW///////////////2VDIhEQESI0Xv/////////////2VDIhERAREjRW/////////////2VDMhERABESI0Vu///////////mVDMiERAAEREiNFbf"
"////////5mVDMiEREAABERIjRVZ97v//7cZlRDMiEREAAAERESIzRVVmZmZmVUQzIiEREAAAABEREiMzREVVVUREMyIhEREAAAAAARERIiIzMzMzMzMiIhER"
"EAAAAAAAERERIiIiIiIiIiEREREAAAAAAAAAERERESIiIiIREREREAAAAAAAEREREiIiIiIiIiIiIiEREREAARERIiIjMzMzMzMzMzIiIhEREAEREiIzNERE"
"RERERERDMzIiERARESIzRFVVVVVVVVVVVURDMhERERIjRFZmZ3d3d3d3d2ZlVDMhERESNN/////////////////TIRERIjTv////////////////0yIRESNF"
"7////////////////1MiEREjRf////////////////9TIhERI0X////////////////+QyIRESNN////////////////5UMhEREjTf///////////////mVD"
"IRERIj7//////////////+dUMiERERI0RWZ3d3d3ff////92QzIRERESI0RVZnd3d9/////3ZUMiERARESIzRFVnd33////+dlQyIREQEREiIzRFVnff////"
"/XVDIhERAAEREiIzRVZ3/////9dUMyIREQABEREiM0Vnf/////52VDIhERAAABERIjNFZ+/////3ZUMiEREAAAARESI0Vn3/////dlQyIRERAAAAEREiNFbf"
"/////XVDMiEREAAAABESI0Vn7////+dlQyIREQAAAAEREiNFfv////92VDIhERAAAAABERI0Vs/////+dkMyEREAAAAAAREiNFf/////92VDIhERAAAAAAER"
"IjVt/////+dUMiEREAAAAAARESNFbv/////WVDIhERAAAAAAERIjRt/////+dkMiEREAAAAAABESI1bv/////WVDIhEQAAAAAAAREjRW//////dkMiEREAAA"
"AAAAERI0Xv/////nVDIhEQAAAAAAABESNF7/////5lMyEREAAAAAAAAREjTf/////9VDIhEQAAAAAAAAERI03/////9UMyEREAAAAAAAABESI+//////VDIh"
"EQAAAAAAAAARESM0VVZlVEMiEREAAAAAAAAAAREiM0REREMyIREQAAAAAAAAAAEREiIzMzMyIhERAAAAAAAAAAAAERESIiIiIhEREAAAAAAAAAAAAAERERER"
"EREREQAAAAAAAAAAAAAAAAAAAREREREiIiIhEREREAAAAAAAAAEREREiIiIiIiIiERERAAAAAAAAERERIiIzMzMzMzIiERERAAAAAAERESIjM0RFVVREQzIi"
"EREAAAAAERESIzRFVmZmZmVUQzIhERAAAAERESI0RWZ97///7WZUQyIREQAAARESI0Vm7////////9VUMiERAAARESI0Vu///////////mQzIREAABESI0Vv"
"////////////5UMiEQABERI0Vv/////////////9VDIRAAERIjRu//////////////5UMhEAAREjRd/////+d3d+/////2QyEQARESNF7////9d3d3fv////"
"ZDIhABESI0b/////d3d3d+////9kMiEAERIjVv////53d3d3/////mQyEQEREjRW/////nd3d33////9VDIRARESNFbv////53d37////+ZUMhEBERI0Vt//"
"////////////dlMiEQERIjRWfv////////////d1QyIREREiNFd37//////////+d3VDIhERESM0Z3fv//////////13ZUMiERESI0Vn7////////////+d1"
"QyIRERIjRn7//////////////nVDIhEREjRW7/////53d9//////1UMiEREiNG7/////13d3ff/////mQyIRESI1b/////53d3d3/////+ZDIhERI0Xf////"
"/Xd3d3f/////5kMhEREjRd/////9d3d3ff/////VQyERESNF7/////53d3d+/////8VDIRERI0Xv/////+d3ff//////ZDIhEREiNd////////////////1U"
"MhERERI0b///////////////5kMiERAREjRd//////////////1lQyEREBESI0Xf////////////xlQyIREAEREjNFbv/////////9ZUQyIREQABERIzRVbN"
"7v///t1mVUMyIREQAAEREiI0RVZmZmZmZVVEMyIRERAAABERIiMzREVVVVVEQzMiIRERAAAAARERIiIzMzMzMzMyIiERERAAAAAAERERIiIiIiIiIiIhEREQ"
"AAAAAAABERERESIiIiIhEREREQAAAAAAAAAAAREREREiIiIiEREREQAAAAAAAAARERESIiIiIiIiIREREAAAAAAAERERIiIzMzMzMzMiIRERAAAAAAERESIj"
"M0REVVVERDMiIREQAAAAERESIzNEVWZmZmZVRDMiEREAAAERESIzRVZt7v//7tZlQzIhERAAARESI0RW3v///////+1UQyIREAARESI0Vm///////////+ZD"
"MhERARESI0Vt/////////////lQyIREBERI0Vu//////////////ZUMhEQERIjRd///////////////mQyIREREjRW//////53d9//////ZTIhEREiNG7///"
"//d3d3ff////91QyERESNFb/////13d3d3/////9VDIRERI0Xf////93d3d3f/////1kMhEREjRt/////nd3d3d//////WQyERESNG7////+d3d3d9/////9"
"ZDIRESI0bv/////Xd3d3//////xkMhERIjRu//////13d9//////92QyEREiNF3////////////////3VDIRESI0V////////////////+dUMhERIjRW7///"
"////////////5lQiEREiNFZ+///////////////WQyIRESI0Vnff//////5+/////3ZDIhERIjRWd3fe7//td37/////dUMhEREiNFZ3d3d3d3d3f/////1l"
"MiERESI07+7u/nd3d3d/////91QyIRERIzXv///+d3d3d+/////mVDIREREjRe/////Xd3d9/////3VDIhEQESJF7/////13d+/////+ZUMhERARIjXv////"
"//////////ZUMiERABESNF//////////////dUMiEREAERI0Xf///////////+ZUMyIREAAREiNF3//////////+ZUMyIREQABERIjRW7////////nZUMyIR"
"EQAAARESM0VW3u///u1mVEMyIREQAAABERIiNEVWZmZmZVRDMiIREQAAAAARERIjM0RFVVREQzIiEREQAAAAAAERESIjMzMzMzMyIiEREQAAAAAAABERESIi"
"IiIiIiIREREQAAAAAAAAAREREREiIiIhEREREQAAAAAAAAAAEREREREREREQAAAAARERIiIiIhEREQAAABERIiIzMzIiEREAAAEREiMzRERDMiEREAABESIz"
"RVVVVEMiERAAAREjNP/////0MhEQABESI0z/////9DIhEQAREiNd/////+UyIREAERI0Xv/////lQyERABESNF7/////1UMhEQEREjRf/////9UyIREBESI0"
"X/////9kMiERAREiNN/////+VDIREQERIjTf/////kMyEREBESI0RWZ3ZlVDIhEQEREiM0VWZmVUMyIREBERIjRFVmZVRDIhERARESM0VWZmVUQyIREQERIj"
"RFZndmVUMiEREBESI07/////5DIhEQAREjRe/////9QyIREAERI0X//////UMiERABEiNG//////ZDIhEQARIjXf/////mQyEREAESI13/////5UMhERABEi"
"NO/////+VDIREAAREjTv/////UMiERAAERIz//////wzIREQABESI0RVVVVEMiERAAARESIzREREMyIREQAAARESIjMzMyIhERAAAAARERIiIiIhEREAAAAA"
"ARERERERERERAAAAAAAAAAAAAAAAAAABEREREAAAAAAAAAAAAAAAAAERERERERERAAAAAAAAAAAAABERERERERERERAAAAAAAAAAARERERERESIiIRERAAAA"
"AAAAARERERESIiIiIiIhERAAAAAAAREREREiIiMzMzMyIhEQAAAAARERERIiIjMzRERE3jIREAAAARERESIiIzNERVVt7/4yIRAAABERESIiMzREVWbe///9"
"MiERAAERESIjM0RFVmfv/////TIhEQARESIjM0RVZn3///////QyIREBERIjNEVWZ97////////kMiEQAREiM0VWd+/////////+1DIREBESIzRWfv//////"
"///tZUMiERAREiNF7//////////XZVQzIREQERI03/////////13ZlRDMiERABEiNd///////+d3dlVEMyIREQARI0Xv/////nd3d2VUQzIhEREAESNF////"
"/9d3d3ZlRDMiIREQABEjRf//////13d3ZVQzIiEREAARI0X////////sd3ZVQzIhERAAESI0//////////13ZUQyIREQABESNFbv/////////tZUQyEREAAR"
"EiNFZt//////////5UMiERAAEREiNEVn3v/////////TIhEQAAEREiM0VWZ+////////1DIREAABEREiIzRFZn3v/////1QyERAAABEREiIjNEVWZ+////9D"
"IhEQAAABERESIiM0RFVm3v/+QyIRAAAAAAERERIiIzNERVVe/jMhEQAAAAAAERERESIiMzREREMyIREAAAAAAAARERERIiIjMzMyIhERAAAAAAAAABERERES"
"IiIiIiEREAAAAAAAAAAAEREREREREREREQAAAAAAAAAAAAARERERERERERAAAAAAAAAAAAAAAAERERERERAAAAAAAAAAABEREREREREAAAAAAAAAAAAAABER"
"ERERERERERAAAAAAAAAAAAERESIiIiEREREREAAAAAAAAAABERIiMyIiIhEREREQAAAAAAAAEREiMzQzMyIiIRERERAAAAAAABESI+5VVEQzMiIhEREREAAA"
"AAAREjT//tZVVEMzIiIREREAAAABESI0////1mVVRDMiIhEREAAAAREjRf/////nZlVEMyIhEREAAAERI03//////+12ZUQzIiEREAABESNN/////////nZl"
"RDMiERAAAREiPe/////////+1mVDMhERAAERIjRW7//////////WVDIhEQABERIzRWfe/////////+VDIREAARESI0RWZ37/////////wyIRAAEREiM0RWZ3"
"fe///////1QyEQABERIiM0VWd3d33/////5UMhEAERESIzNFVnd3d33////+VDIRABERIiM0RWZ3d97//////VMiEQARESIzRVZnfO////////1DIhEBERIj"
"NFVnfv/////////lQyERAREiNEVm7/////////7WVDIhEQERIzRd//////////12VUMiEREBESNP/////////+dmVUQyIREQARIjT////////nZmVUQzIhER"
"EBESI1//////7WZlVEMzIiEREQAREiPf////1mVVRDMyIiERERAAERIj7//+ZlVUQzMiIiEREREAAAERI+/lVVREMzMiIhEREREAAAABESLDREQzMyIiIRER"
"EREAAAAAARESIzMzMiIiEREREREAAAAAAAARESIiIiIhEREREREAAAAAAAAAERERERERERERERAAAAAAAAAAAAARERERERERERAAAAAAAAAAAAAAABERERER"
"EAAAAAAAAAAAAAAAAAAAERERERIiIiIRERERAAAAAAABERERIiIiIiIiIhEREQAAAAABERESIjMzMzMzMyIhEREAAAABEREiIzNERVVUREMyIRERAAABEREi"
"M0RVZmZmZlVEMyIREQAAEREiNEVWfe///+1mVDMhERAAEREiNFVt////////5lQyIREQAREiNFbf//////////1UMiERARESNFb////////////2VDIREBES"
"I0b/////////////5UMhEQERI0Xv/////////////8QyIRARIjTf/////nd+//////1TIhEREiNe/////nd3f//////VQyERESI1/////3d3d//////9VCIR"
"ERIj3/////d3d3//////ZTIhEBESPf////53d33/////5kMiEQERIzRVZnd3d33//////VQyERAREiM0RVZmd33//////mUyIREBERIjNEVWZ33//////2VD"
"IhEQARESIjNFZn7//////mVDIhEQABEREiI0Vn///////WVDMhERAAARERIjRW7/////52VDMiERAAABEREiNF3/////12VDMiEREAAAARESI0Xv////x2VD"
"MiEREAAAABERIjRe////52VDMiEREAAAAAEREiNFZ3d3d2VDMiEREQAAAAARESI0Vnd3d2VDMiEREQAAAAABERI0RWd3d3ZUMiEREQAAAAAAERIjRe/////l"
"QyIREQAAAAAAAREiNF/////9VDIhERAAAAAAABESI13/////1UMhERAAAAAAAAERI0Xf////9kMiEREAAAAAAAAREjRe/////lQyIREAAAAAAAABESI17///"
"/+VDIREQAAAAAAAAERIjTv////5DIhEQAAAAAAAAABESPP/////UMhERAAAAAAAAAAERIjRFVVVEMiERAAAAAAAAAAARESIzREQzMiEREAAAAAAAAAAAEREi"
"IzMyIiEREAAAAAAAAAAAAREREiIiIhEREQAAAAAAAAAAAAEREREREREREQAAAAAAAAAAAAAAAAAAAAEREREiIiIhERERAAAAAAAAAAAAAAAAERESIiIiIiIi"
"EREQAAAAAAAAAAAAAAEREiIzM0RDMzIiEREAAAAAAAAAAAAAEREiM0RFVVVUQzIhERAAAAAAAAAAAAEREiM0VWZnZmZUQyIREAAAAAAAAAAAEREiM0Xv////"
"//5UMhERAAAAAAAAAAARESI0Xf///////1QyIREAAAAAAAAAARESI0Vv////////ZUMhERAAAAAAAAABESI0Vt/////////WQyIREAAAAAAAABERIjRn////"
"/////+ZTIhEQAAAAAAABERIjRW3/////////51QyEREAAAAAAAEREjNWf//////////3VDIhEQAAAAAAEREiNFfv//////////dkMiERAAAAAAAREiNFZ///"
"/////////WUyIREAAAAAARESI0V+/////9/////+dUMhERAAAAABESI0Vt/////9f/////51QyEREAAAABERIjRn7////+d//////nZDIhEQAAAAERIjRW3/"
"////137/////dkMiERAAAAEREjNGf/////93f//////GUzIREAAAEREiNFff/////Xd//////9dUMhERAAARESNFZ//////nd37/////51QyEREAARESI0V9"
"/////9d3fv/////nZDIhEQABERI0Vn//////d3d9//////dkMiERABERIjRX7/////53d33/////92UyIREAEREjRWf/////93d3ff/////9ZUMhEQAREiNF"
"fv////////////////51QyERARESNFbP/////////////////nVDIREBESI0V+/////////////////+dUMhEQERIjVt//////////////////92QyEREREj"
"RX7//////////////////9ZDIhEREiNG3///////////////////1kMiERESI1b////////////////////mQyIRERI0Xf/////9d3d3d3ff/////+ZDIRER"
"EjRf/////+d3dmZnd9//////9UMhERESI+//////x2VVVVVmfv/////1QyERERIj//////5lVEREREVv//////0yIREBESM0VWZmVURDMzMzNEVWZmZVQzIR"
"EQERIjNEREREMzIiIiIzNERFREQzIhEQABESIjMzMzMiIiIiIiIjMzMzMyIhERAAERESIiIiIiIREREREiIiIiIiIRERAAABEREREREREREREREREREiIRER"
"ERAAAAAAAREREiIiIiIiIiIiIiIREREQAAAAAAERESIiIzMzMzMzMzMiIiIRERAAAAABERIiMzREREREREREQzMyIhEREAAAARESIzRFVVVVVVVVVVVVRDMi"
"EREQAAEREiNEVmZ3d3d3d3d3ZmZUQzIREQAAERIjRf//////////////7WVDIhERABERIzTf////////////////1UMiERABESI0Xf/////////////////V"
"QyERABESI0bv/////////////////2QyIRARESNFbv/////////////////9UzIRARESNFf//////////////////+VDIRAREiNGf//////Xd3d3z//////+"
"VDIRAREiNW3//////Xd3d3fv/////+VDIREREjRX3/////93d3d3ff/////9VDIREREjRX7/////93d3d3fv/////2VDIRERIjRn7/////53d3d3f//////m"
"UyIRERIjVn//////53d3d37/////51QyIRERIzVt//////////////////dlQyIRERI0V9/////////////////ndlMyERERI0V+///////////////+d3dU"
"MiERESI0Z+////////////////53dUMiERESI0Z//////////////////9dUMiERESI1Z///////////////////dkMiERESNFbf/////9d3d3d9//////1k"
"MiERESNFff/////3d3d3d3//////5kMiERESNFfv/////3d3d3d3//////5kMiERESNFfv/////nd3d3d3//////5kMiEREiNGf//////nd3d3d9//////5k"
"MhEQEiNG3//////Xd3d3ff//////1UMhEQEiNG3///////////////////ZUMhEQEiNW7//////////////////+ZDIhEQEiNW7//////////////////mVD"
"IREQEiNG//////////////////9lQyIREAESNF//////////////////ZUMyEREAESNN/////////////////mVDMiERAAESI9///////////////tZlQzIh"
"ERAAERIzRVZnd3d3d3d3d2ZlVEMyIREQAAERIjNEVVVVVVVVVVVVVEQzIiEREAAAEREiIzM0REREREREREMzMiIRERAAAAARERIiIjMzMzMzMzMzIiIhEREQ"
"AAAAABERESIiIiIiIiIiIiIhEREREAAAAAAAAAAAAAERERERIiIiIiIRERERAAAAAAAAAAABERERIiIiIiIiIiIREREQAAAAAAAAERERIiIzMzM0MzMzIiIR"
"ERAAAAAAABEREiIjNERFVVVVREMyIhEREAAAAAARERIiM0RVZmZmZmZVRDMiEREQAAAAERESIzRVZn3u7//+7WZUQyIREQAAABERIiNEVn3/////////7WVD"
"IhERAAARESIjRWbv///////////+ZUMiERAAARESI0Vt//////////////5lQyEREAEREiNFbf///////////////lQyIREAERIjRW3////////////////9"
"VDIREBERIzVt/////////////////+VDIREBESI0V////////Xd3zv//////ZDIRERESNFbv//////d3d3d+//////xTIhEREiNGf//////3d2Zmd9//////"
"1TIhEREjRW7//////XdlVVZ3//////1DIhEREjRX//////53ZVRFVn//////1DIRERIjRt//////13VUREVW7u7u7vVDIRERIjVu//////x2VERERVZmd2ZV"
"QyIRERI0V///////d2VEMzRFVWZmVUMyIRERI0V//////+d1VDMzRFVVVVVEMiERERI0bP/////+d1REM0RFVWZVVDMiERESI0bf/////9d1VERERVZmZmVE"
"MiERASI0bf/////9d2VERFVt7d3d3kMiERASI0bf/////9d2VURVZ//////0MiERARI0bf/////+d2ZVVmff/////kMiERARI0bf/////+d3dmZ3f//////U"
"MhERARI0XP//////13d3d3//////5UMhEQARI0Vu///////nd3fv//////1UMhEQARIjRu/////////////////+VDIhEQAREjRW/////////////////2VD"
"IREQAREiNG3///////////////5lQyIREAARESNFbv/////////////+ZUMiEREAABESI0Vt/////////////WVDMhERAAABERIjRVbf/////////tZVQyIh"
"ERAAAAEREiNEVmfe7//+7ddlVEMiIREQAAAAERESIzRFVmZmZmZlVUQzIhEREAAAAAARERIiM0REVVVVVEQzMiIRERAAAAAAABERESIjMzM0QzMzMiIhEREQ"
"AAAAAAAAEREREiIiIiIiIiIiEREREAAAAAAAAAABEREREiIiIiIhEREREQAAAAAAAAAAAAERERIiIiIiIiIiIhEREREAAAAAAAARERIiIzMzMzMzMiIiIRER"
"EAAAAAABERIiMzREREREREMzMyIhEREAAAAAEREiM0RVVVVVVVVVVEMzIiEREAAAARESI0RWZnd3d3d3ZmZVRDMiEREAAAERIjRf//////////7tdlVDMiER"
"EAARESM03//////////////WVDMiEREAERIjRd///////////////8VDMhERABESI0bv///////////////9VDIhEQEREjRW7////////////////2VDIhEB"
"ERI0V//////////////////mUyIRAREiNGf//////////////////VQyEQERIjVt//////13d33///////5kMiERESNFbf/////3d3d3fv//////ZTIhEREj"
"RX7/////93d3d33//////2VDIRESI0Z+/////+d3Znd3//////51QyEREiNGf//////ndmZnd+//////dUMhERIjVt//////13ZVZnfv/////3VDIRESNFbf"
"/////9dlVVZ3//////91QyEREjRX7/////93ZVVWd///////dUMhESI0V+/////+d2VVVn3//////mVCIREiNGf//////ndlVWd9//////1lMiERIjVn////"
"//53ZVVnfv/////9ZDIhESNFbf/////9d2VWZ37/////91QyEREjRX3/////93dmZnff/////+ZUMhERI0V+//////d3dmd37//////GQyIRESNFfv/////n"
"d3d3ff/////+ZUMhERIjRn//////53d3d9///////WQyIRESI0bf/////9d3d97//////+ZUMiEREiNG3/////////////////91QyIRERIjVu//////////"
"///////3ZDMhERASI1bv////////////////dlQyIREAEiNG////////////////52VDIhERABEjRf///////////////WVEMiEREAARI03/////////////"
"/WZUMyIRERAAESI9///////////t12ZUQzIhEREAABESM0VWZ3d3d3d2ZmVUQzIiEREQAAAREiM0RVVVVVVVVVREMzIiERERAAAAEREiIzM0REREREQzMzIi"
"IREREAAAAAERESIiIzMzMzMzIiIiEREREAAAAAAAERERIiIiIiIiIiIhEREREQAAAAAAAAAAAREREiIiIiIiIiIiIiIREREQAAAAARERIiIzMzMzMzMzMyIi"
"IhEREAAAARESIjM0REREREREREMzMyIREQAAARESIzRFVVVVVVVVVVVURDMiEREAARESI0RWZnd3d3d3d3ZmZlVDIhEQABESI0X/////////////////0yER"
"ABERIzTf/////////////////TIhEAERIjRd/////////////////1QyEQAREiNG7/////////////////VDIRARESNFbv////////////////5TIhEBERI0"
"V//////////////////kMiEQERIjRn/////////////////9QyERAREiNW3//////Xd3d3d3d3ZlQyIRERESNFff/////3d3d3d3d2ZVQzIRERERI0V+////"
"//d3d3d3d2ZVQzIhERERIjRn7/////53d3d3d3ZlRDIhERAREiNWf//////nd3d3d3d2VDMiEREBESM1bf///////////////0MyEREAERI0V9//////////"
"/////+VDIREQAREjRX7///////////////5UMhERABEiNGfv///////////////VQyEREAESI0Z////////////////8QyIREAARIjVn////////////////"
"VDIhEQABEjRW3///////////////5UMiERAAESNFff/////3d3d3d3d3ZUMyEREAARI0V+//////d3d3d3d3ZlQzIREQABEjRX7/////53d3d3d3ZlVDMiER"
"AAEiNGf//////nd3d3d3d2ZUQyIREAASI0bf/////9d3d3d3d3dmVDIhEQABIjRt/////////////////9MyERAAEiNW7/////////////////1DIREAASI1"
"bv////////////////9UMhEQABIjRv/////////////////lQyERAAESNF/////////////////+UyIREAARI03/////////////////5DIhEQABEiPf////"
"/////////////UMhEQAAERIzRVZnd3d3d3d3dmZlVEMiERAAAREiM0RVVVVVVVVVVVVURDMiERAAABERIiMzNERERERERERDMzIiEREAAAARERIiIjMzMzMz"
"MzMzIiIhEREAAAAAERERIiIiIiIiIiIiIiIREREQAAAAAAAAAREREiIiIiIiIiIiIhERERAAAAAAERESIiMzMzMzMzMyIiIhEREAAAABERIiMzRERERERERD"
"MzMiIREQAAARESIzRFVVVVVVVVVVVEQzIhEQAAEREiNEVmZ3d3d3d3ZmZlVDMhERAAERIjRf///////////////+MiERABERIzTf///////////////9QyER"
"ABESI0Xf///////////////9QyERABESI0bv///////////////1QyERARESNFbv///////////////lQyERARESNFf////////////////kMiERAREiNGf/"
"///////////////kMiERAREiNW3//////Xd3d3d3dmVDMhEQEREjRX3/////93d3d3dmZVQzIhEQEREjRX7/////93d3d2ZlVEMyIREQERIjRn7/////53d3"
"d2ZVRDMiEREAERIjVn//////53d3d3ZlVDMiEREAERIzVt//////13d3d3dmVDMiEREAERI0V9///////////////DMhERAAERI0V+//////////////9UMh"
"ERAAESI0Z+//////////////5UMhERAAESI0Z///////////////5UIhEQAAESI1Z///////////////1DIhEQAAESNFbf//////////////1DIREAAAESNF"
"ff//////////////QyIREAAAESNFfv/////3d3d3dmVUMyEREAAAESNFfv/////nd3ZlVVRDMiERAAAAEiNGf//////ndmVURDMyIhERAAAAEiNG3//////X"
"ZVQzMyIiEREQAAAAEiNG3//////XZUMyIiIREREAAAAAEiNG7/////92VDMiIRERERAAAAAAEiNG7/////52QzIiEREREAAAAAAAEiNG//////5lQyIREREQ"
"AAAAAAAAESNF//////5UMiERERAAAAAAAAAAESNN//////1UMiEREQAAAAAAAAAAESI9//////xDIhERAAAAAAAAAAAAERIzRVZmVUQyIREQAAAAAAAAAAAA"
"ERIjNERERDMiEREAAAAAAAAAAAAAEREiIzMzMyIhEREAAAAAAAAAAAAAARERIiIiIiEREQAAAAAAAAAAAAAAABEREREREREREQAAAAAAAAAAAAAAAAAAAAAA"
"ERERERIiIiIiIhEREREAAAAAAAAAAAEREREiIiIiMyIiIiIREREQAAAAAAAAEREREiIjMzM0RDMzMyIhEREQAAAAAAAREREiIzNERFVVVVREQzIiEREQAAAA"
"ABEREiIzRFVWZmZmZmZVRDMiEREAAAAAERESIzRFVmfe7///7tdmVEMiEREAAAARERIjNFZn7//////////mVUMiEREAAAEREiNFVt/////////////+ZUMi"
"ERAAARESI0Vn7//////////////+ZUMhERABERIjRW3////////////////+VDIhEQAREiNFbf/////////////////9VDIREBERIzRt////////////////"
"///lQyERAREiNFfv///////Xd3ff//////9kMhERERI0Vu///////Xd3d3d+//////1DIhEREiNFf//////8d3d3d3d//////9QyIRERI0Vu//////13d3d3"
"d3fu7u7u5UMiERESNFfv/////nd3d3d3d3d3d3d3VDIhERIjRt//////13d3d3d3d3d3d3dlQyIRESI1bv/////8d3d+//////////5UMiEREjRX//////93"
"d3f//////////+VDIhERI0V//////+d3d3///////////VQyIRESNGf//////nd3d///////////dUMhERIjRt//////13d33//////////3VDIRESI0bf//"
"///9d3d+//////////51QyEREiNG3//////Xd3d3d3d+/////+dUMhEREjRt//////53d3d3d3f//////WUyIRERI0bf/////+d3d3d3d+//////1lMiERES"
"NF3//////+d3d3d37//////3ZDIhEREjRW7///////13d33///////51QyERERIjRu///////////////////+dUMhERERI0XP///////////////////mUy"
"IREBESI0bv//////////////////1kMiERAREiNFbv/////////////////8VDIREQEREiNFbv///////////+fv//9lMiERAAEREiNFZ+/////////+d37/"
"/+VDIhEQAAEREiNEVm3u7//+7Xd3Z9///kMiEREAABEREiM0VVZmZmZmZlVVVVVVQzIREQAAABEREiIzREVVVVVUREREREQzIiERAAAAABEREiIjMzNERDMz"
"MzMzMzIiEREQAAAAABERERIiIiIzMiIiIiIiIiERERAAAAAAAAERERESIiIiIiIiIREREREREAAAAAAAAAEREREREREREREREREREREREREQAAAAABEREiIi"
"IiIREREREREiIiIiIRERAAAAARESIjMzMzIiEREREiIjMzMzIiEREAAAEREiM0REREMyIhERIiM0REREMyIREAABERIjRFVmZVRDIiIiIzRFVmZVQzIREQAB"
"ERI0X//////kMiIiM03//////TIhEQARESI03//////UMyIjNF3//////UMhEQAREiNF3//////VQzMzRW7/////9UMhEQAREiNG7/////91QzMzRW7/////"
"9kMiEQEREjRW7/////52VDM0Vn//////5kMiEQEREjRX//////52VEREVn//////5kMiEQERIjRn//////12VURVZ9//////1kMiEQERIjVt//////13ZVVW"
"d9//////xkMiERERI0V9//////d3dmZnd+//////dkMiERERI0V+//////d3d3d3d///////dUMhERESI0Z+/////+d3d3d3d//////+dUMhERESI1Z/////"
"///////////////9dUMhERESM1bf///////////////////9ZUMhERESNFff///////////////////3ZTIhERESNFfv///////////////////3ZDIhEREi"
"NGfv///////////////////nVDIhEREiNGf////////////////////nVDIREREiNWf////////////////////XVDIREBEjRW3////////////////////W"
"UzIREBEjRX3/////93d3d3d37/////92QyIREBEjRX7/////93d3d3d3//////92QyIREBEjRX7/////53dmZnd3//////51QyEREBIjRn//////53ZlVWd3"
"//////11QyEREBIjRt//////12VURVZ9//////1lMiERABIjRt//////12VERFZ+//////dkMiERABIjRu//////dlRDNFZ+//////dUMhERABIjRu/////+"
"dlQzNEZ//////+ZUMhERABIjRv/////+ZUMzM0Vv/////+ZDIhEQABEjRf/////+ZEMiM0Xf/////9VDIREQABEjTf/////9VDIiIzTf/////9QyIREAABEi"
"Pf/////8QzIiIjTv/////1QyEREAABESM0VWZlVEMyIiIiNEVWZlVEMiEREAABESIzREREQzIiEREiIzREREQzIhERAAABERIiMzMzMiIhERERIiMzMzMiIR"
"EQAAAAERESIiIiIiERERERESIiIiIhEREAAAAAARERERERERERERERERERERERERAAAAAAAAAAERERERERERERAAAAABEREiIiIiIhEREAAAARERIiMzMzIi"
"EREAAAAREiIzREREMyIREQAAEREjNFVWZVRDIhEQAAERIjTv/////0MiEQABERI0Xv/////0MiEQABESI0b//////lQyEQARESNFb//////lQyEQAREiNG3/"
"/////VQyEQAREiNW3//////VQyEQEREjRW7/////91QyEQEREjRX7/////91QyEQERIjRn//////51QyEQERIjRn//////1lQyEQERI0Vt//////1lMiERER"
"I0V+//////dkMiERERI0V+//////dkMiERESI0Z//////+dUMiERESI0Z//////+dUMhERESM1bf/////9ZUMhERESNFbf/////9ZTIhERESNFfv/////3ZD"
"IhEREiNFfv/////3ZDIhEREiNGf//////nVDIREREiNGf//////XVDIREREiNW3//////WUyIREBEjNW3/////92QyIREBEjRW7/////92QyIREBEjRX7///"
"//51QyEREBEjRX//////5lQyERABEjRd//////1kMiERABEjRd//////1UMhERABEiNe//////ZUMhERABEiNO//////VDIhEQABESNP/////+QzIREQABES"
"I0RVZmVUMyIREAABERIjNERERDMiEREAAAEREiIzMzMyIhERAAAAARERIiIiIiEREQAAAAABEREREREREREAAAAAAAAAAAAAAAAAAAERERERERERERAAAAAA"
"AAAAAAAAARERIiIiIiIRERAAAAAAAAAAAAABEREiIzMzMiIREQAAAAAAAAAAAAAREiIzREREMyIREQAAAAAAAAAAABERIzRVVmVUQyIREAAAAAAAAAAAEREi"
"NO//////QyERAAAAAAAAAAABESI0Xv/////0MiEQAAAAAAAAAAAREiNW//////5UMhEAAAAAAAAAABERI0Vv/////+VDIRAAAAAAAAAAAREiNG3//////VQy"
"EQAAAAAAAAAAERIjVt//////xUMhEAAAAAAAAAARESNFbv/////3VDIRAAAAAAAAAAERIjRX7/////51QyEQAAAAAAAAARESI0Z//////+ZUMhEAAAAAAAAA"
"EREjNW3//////WVDIRAAAAAAAAARESI0Vt//////1lMiEQAAAAAAAREREiNFfv/////3ZDIhEAAAAAERERERIjRn7/////92QyIRAAABEREREREiNFZ/////"
"/+dUMhEQABEREREREiIjRWf//////nVDIREAEREREiIiIiM0V9//////1lQyERARERIiIjMzMzRWff/////8ZTIhERERIiMzNERDREVn7/////92QyIREREi"
"M0RVVVVERWd+/////+dUMhERERIzRVZmZmVVZnf//////mVDIREREiNP/////3Zmd9//////1lMiERARI0X/////53d3ff/////3ZDIhEQESNF//////13d3"
"//////5lQyEREBEjRf/////+d33//////9ZDIhEQARI0X///////////////9lQyEREAESNF7//////////////9ZDIhERABEiNd//////////////5lQyIR"
"EAARIjRe/////////////mVDIhERAAERIjRe///////////9ZUMiEREAABESI0Vd/////////+ZVQyIREQAAARESI0RWbe7//+7XZlRDIhEREAAAARESIzRF"
"VmZmZmZVRDMiEREQAAAAARESIjNERVVVVERDMiIRERAAAAAAERERIiMzMzMzMzMiIREREAAAAAAAERERIiIiIiIiIiIREREQAAAAAAAAARERERIiIiIiERER"
"EQAAAAAAAAAAAAERERERERERERERERERIiERERERAAAAAAARERIiIiIiIRERERIiIiIiIiIRERAAAAABERIiMzMzMyIiISIiIzMzMzMiIREQAAAAEREiM0RE"
"REMzIiIiIzREREREMyIREQAAARESI0RVZmVVQzMiMzRFVmZmVUQyIREAAAEREjRf/////+RDMzRFbf///////SERAAARESI03//////VRERFVt///////9Mh"
"EQAAERIjRd//////1lRFVn3///////1DIREAABESI0bv/////3ZlVmff///////VMyERAAEREjRW7/////53Zmd9///////9VDIhEQABERI0V//////+d3d3"
"3///////1lMyEREAAREiNGf//////Xd3ff///////WVDIhEQAAERIjVt//////13d9///////9ZUMiEREAARESNFff/////3d33///////1lQzIREQAAEREj"
"RX7/////93ff///////XZDMiEREAABESI0Z+/////+d9///////9dlQyIREQAAAREiNWf//////n3///////12VDMiEREAAAERIzVt//////3f//////93dl"
"QyIREQAAABESNFff//////////////d3VDMiEREAAAAREjRX7//////////////9d1QyIREQAAAAESI0Z+///////////////ndUMiEREAAAABEiNGf/////"
"///////////XZDIhERAAAAARIjVn////////////////52UzIREQAAAAESNFbf////////////////dlQyIREAAAABEjRX3////////////////+dUMiERAA"
"AAARI0V+/////////X///////3ZTIhERAAAAESNFfv///////3d9///////XVDIREQAAABIjRn///////+d3d///////52QyIREAAAASI0bf//////13d3fv"
"//////1lMiERAAAAEiNG3//////Xd3d33///////ZUMhEQAAABIjRu//////d3ZmZ3///////9VDIREAAAASI0bv/////ndlVVZ+///////mQyERAAAAEiNG"
"//////52VERFZ////////EMiEQAAABEjRf/////+ZUQzRFbv//////5DIhEQAAARI03//////VRDMzRF3///////QyERAAAAESI9//////xEMyIzRF//////"
"/9MhEQAAABESM0VWZlVUMyIiIjNFVmZmVUQyIREAAAAREiM0REREMzIiIiIjNERFVEQzIhERAAAAEREiIzMzMyIiERERIiMzMzMzMiEREAAAAAERESIiIiIi"
"EREREREiIiIiIiIREQAAAAAAEREREREREREREREREREiIhEREREAAAAAAAAAARERERERERERAAAAAAAAAAAAAAERESIiIiIhEREAAAAAAAAAAAABERIiMzMz"
"MiIREQAAAAAAAAAAARESIzREREQzIhEQAAAAAAAAAAEREiNEVWZlVDMhERAAAAAAAAAAEREjRf/////+MiERAAAAAAAAABERIjTf/////9QyERAAAAAAAAAB"
"ESI0Xf/////9QyIREAAAAAAAABESI0bv/////2UyIREAAAAAAAARESNFbv/////mUyIREAAAAAAAARESNFf//////mUyIREAAAAAAAAREiNGf//////WUyIR"
"EAAAAAAAAREiNW3//////WQyIREAAAAAAAEREjRW3/////92QyIREAAAAAAAEREjRX7/////92QyIREAAAAAAAERIjRn7/////51QyIREAAAAAAAERIjRn//"
"////51QyIREAAAAAAAERIjVt//////11QyIREQAAAAAAERI0Vt//////1lQyIREQAAAAAAERI0V+//////dlQyIRERAAAAAAESI0V+/////+dlQyIREREAAA"
"AAESI0Z//////+dlQyIhERERAAAAESI1Z//////+dlQzIiIREREAAAESNFbf/////9dlRDMiIiERERAAESNFff/////3d2VEMzMiIhEREAESNFfv/////3d2"
"ZVREQzMiEREAESNFfv/////nd3dmZVVEQyIREQEiNGf//////nd3d3d3ZlVDIhEQEiNG3////////////////9MhEQEiNG3////////////////0MiEREiNW"
"7////////////////1QyEREiNW7////////////////lQyEREiNG/////////////////lQiERESNF/////////////////UMiERESNN////////////////"
"/UMhEQESI9////////////////9DIhEQERIzRVZnd3d3d3d3dmZVQzIhEQERIjNEVVVVVVVVVVVVREMyIREAEREiIzNERERERERERDMzIiEREAARERIiIjMz"
"MzMzMzMyIiIRERAAABERESIiIiIiIiIiIiIREREQAAAAAAABERERIiIiIiEREREREREiIiIiIREREQAAAAARERIiIjMzIiIiIREREiIiIjMiIiIRERAAAAER"
"EiIzM0REQzMyIiIiIjMzNERDMzIiERAAAAERIjNEVVVVVURDMiIiMzRFVVVVREMyEREAABESIzRWZnd3ZmVUMzIzNFVmZ3dmZVQzIREAARESM07////////9"
"RDM0Rd/////////DIhEAAREiNF7////////9VEREVu////////9TIhEAAREjRW/////////9ZURFZ/////////9UMhEAEREjRX/////////9dlVWfv//////"
"//5UMhEAERIjRt/////////+dmZnf/////////1kMhEAERIzVu/////////+d2Z33/////////1kMhEBERI0V+/////////+d3d37/////////dkMhEBESI0"
"Z+/////////+d3d9//////////dUMhEBESI0Z///////////d3d//////////+dUMhEBESI1bf//////////d3d//////////+dUMhEBESNFff/////v///+"
"d3fv///+/////9ZUMhERESNFfv////7v////d3f////u/////8ZTIhEREiNFfv////3v////d37////u/////3ZDIhEREiNGf/////3v////x3////3v////"
"/3ZDIhEREiNWf/////3f////19////fv/////nVDIREREjRW3/////3f////1////+f//////XVDIREREjRX3/////d/////1////3f//////WVDIREREjRX"
"7/////d/////7v///n3/////92UyIRERIjRn7/////d//////////X7/////92QyIRERIjRn/////+d/////////93//////51QyIRERIjVt/////+d/////"
"////13//////51QyERERIjVt/////9d+///////+d8//////1lQyERARI0Vu/////9d+///////9d9//////1lMiERARI0V+/////3d+///////3d+//////"
"dkMiERARI0V//////3d+///////nd+//////dUMhERARI0V//////nd9//////93d//////+ZUMhERARI0Xf/////nd9//////53d//////9ZTIhEQARI0Xf"
"/////Xd9//////13ff/////9VDIhEQARI0Xv/////Hd8/////+d3ff/////2VDIREQARIjTv////92Zn7////9dmbv/////1QyIREAAREjT/////9lVW////"
"/2VVX//////kMyEREAAREiNEVWZlVERFVWZmZVRERVZmZlVDMiERAAARESIzREREQzM0REREREMzNEREREQzIhERAAABERIiMzMzMyIzMzMzMzMiMzMzMzMi"
"IREQAAABERESIiIiIiIiIiIiIiIiIiIiIiIhEREAAAAAERERERERERERESIiERERERIiERERERAAAAAAAAABEREREiEREREREREREREREREREAAAAAARERIi"
"IiIiIRERERERIiIiIiEREQAAAAEREiIzMzMzIiEREREiIjMzMiIhERAAABERIjNEREREMyIiESIjM0REQzMiERAAARESI0RWZmZVRDMiIiIzRVVVVUMyEREA"
"AREiNF/////+ZUMyIiM03/////0yIREAEREjNN//////1lQzMzNF7/////1DIREAERIjRd//////92VEMzRW7/////VDIREAERIjVu///////nZUREVn////"
"//ZDIREBERI0Vu///////9dlVFVn/////+ZDIhEBERI0V/////////d2VVZ9/////+ZDIhEBESI0Z/////////53Zmd9/////9ZDIhEBESI1bf/////////X"
"d3d+/////8ZDIhERESNFff/////////3d3d//////3ZDIRERESNFfv/////////+d3d//////3VDIREREiNGfv//////////13d//////nVDIREREiNWf///"
"////////93ff/////WVDIREREiNW3////////////nfv/////WUyIREREjRX3////////////9fv////92QyIREREjRX7/////////////f/////92QyIRER"
"IjRX7///////////////////51QyIRERIjRn/////+7/////////////51QyERERIjRn/////+fv////////////1lQyERARIkVt/////+d/////////////"
"1lMiERARI0V9/////9d9////////////dkMiERARI0V+/////8d37///////////dkMiERARI0V+/////nd3f//////////+dUMhERASI0Z//////nd3ff//"
"///////9dUMhERASI0bf/////nd3d+/////////9ZTIhEQASI0bf/////XdmZ37////////3ZDIhEQASI0bv/////XZVZnz////////3VDIREQASI0bv////"
"92VVVWfv///////mVDIREQASI0b/////52VERFZ+///////mQyIREAARI0X/////5lRDNEVs///////VQyEREAARI03/////5UQzMzRW3//////UMyERAAAR"
"Ij3/////1EMiIjNFbv////9UMiERAAAREjNFVVVVQzIiIiM0RVZmZlRDIhERAAAREiMzRERDMyIhEiIjNEREREMyIREQAAARESIiMzMyIiERERIiIzMzMzIi"
"EREAAAABEREiIiIiIRERERERIiIiIiIRERAAAAAAEREREREREREREREREREiEREREQAAAAAAAAAAAAAREREREiIiIiIhEREREQAAAAAAAAAAARERESIiIiIz"
"IiIiIhEREQAAAAAAAAARERESIiMzMzREMzMyIiEREQAAAAAAABERESIjM0RFVVVVVEQzMiIREQAAAAAAERESIjNEVVZmZmZmZVVEMyIREQAAAAARERIjNEVW"
"Z97v//7u12VUMyIREQAAABEREiM0Vmzv/////////tZUQyIREQAAARESI0VW3/////////////dUQyIREAABERIjRWf///////////////1UMyEREAEREiNF"
"bf////////////////1UMiERABESI0Vt/////////////////+ZUMhEQEREjNG3//////////////////9ZDIhEBESI0V+///////td3fO///////2UyIRER"
"EjRW7//////9d3d3d+//////91QyERESI0V///////d3dmZ3d///////1UMhEREjRW7//////XdmVVZ3fv/////9ZDIRERI0V//////+d2VURVZ33//////W"
"QyIREiNG3//////XdlRERWd9//////1kMiERIjVu//////x2VEREVXff/////9ZDIhESNFf//////3dlRERFV37//////WQyEREjRX//////53VUQ0RWd+//"
"////dUMhERI0bP/////+d1VEREVnf//////nVDIRESNG3//////XdVRERFZ9//////5lMyERIjRt//////13ZUREVnff/////9ZDIhERI0bf/////9d2VURV"
"Z3//////91QyERESNG3//////nd2VVZnff/////+ZUMhEREjRd//////53d2Znd3//////92QyIRERI0V///////53d3d33//////+ZUMhERESI1bv//////"
"7Xd3fv//////91QyIREBEiNF3//////////////////8ZDMhERAREjRW//////////////////1lQyIREAERIjRd/////////////////WVDIhERABERIzRt"
"///////////////nZUMiEREAABESI0RX7////////////9ZVQyIREQAAARESI0RWzv/////////tZlQzIhEREAAAARESIzRVZ97u///u13ZlRDMiEREQAAAA"
"ARESIzRFVWZmZmZmVVRDMiIRERAAAAAAARESIjM0RFVVVVVEQzMiIREREAAAAAAAARERIiIzMzREMzMzIiIhEREQAAAAAAAAAREREiIiIiMyIiIiIREREQAA"
"AAAAAAAAAREREREiIiIiIhERERERAAAAAAAAAAAAAREREiIiIiIiIiIiIREREQAAAAAAARERIiIzMzMzMzMyIiIhEREAAAAAARESIjM0REREREREMzMiIRER"
"AAAAARESIzRFVVVVVVVVVVRDMiIREQAAARESI0RWZnd3d3d3d2ZlVDMiEREAABESI0X////////////+1lQzIhEQABERIzTf//////////////1UMyEREAER"
"IjRd///////////////9VDIhEQAREiNG7///////////////9lQyERARESNFbv///////////////+VDIhEBERI0V/////////////////5lMiEQERIjRn//"
"///////////////9VDIRAREiNW3//////Xd3ff//////1UMhERESNFff/////3d3d37//////VQyERERI0V+//////d3d3fv/////9VDIRERIjRn7/////53"
"d3d+//////dUMhEREiNGf//////nd3d37/////5lQyERESM1bf/////9d3d33//////mUyIRERI0V9//////13d37//////9VDIREREjRX7/////////////"
"////ZUMhEREiNGfv////////////////1UMiERESI0Z/////////////////1lMyERERIjVn////////////////5lQyIREBEjRW3///////////////1lQy"
"IREQESNFff/////////////+dlQyIREQARI0V+////////////7XZUQyIRERABEjRX7/////53d3d3ZmVUMyIRERAAEiNGf//////nd3ZmVVRDMiIRERAAAS"
"I0bf/////9d2VUREMzIiIRERAAABIjRt//////12VEMzMiIiERERAAAAEiNG7/////92VEMyIiIREREQAAAAASI0bv/////nZUMiIhEREREQAAAAABIjRv//"
"///+ZUMiIREREREAAAAAAAESNF//////5kMyIREREQAAAAAAAAARI03//////VQyIREREAAAAAAAAAABEiPf/////8QyIRERAAAAAAAAAAAAERIzRVZmVUQy"
"IRERAAAAAAAAAAAAAREiM0REREMyIRERAAAAAAAAAAAAABERIiMzMzMiIRERAAAAAAAAAAAAAAARERIiIiIiERERAAAAAAAAAAAAAAAAERERERERERERAAAA"
"AAAAAAAAAAAAAAAAAAAAERERERIiIiIiIREREREAAAAAAAAAAAEREREiIiIiIyIiIiIREREAAAAAAAAAEREREiIjMzM0RDMzMiIhEREAAAAAAAAREREiIzNE"
"RVVVVVREMzIiEREAAAAAABEREiIzRFVWZmZmZmVVRDMiEREAAAAAERESIzRFVmfe7//+7tdlVDMiEREAAAARERIjNFZn7/////////7WVEMiEREAAAEREiNF"
"Vt/////////////3VEMiERAAARESI0Vn7//////////////9VDMhERABERIjRW3////////////////9VDIhEQAREiNFbf/////////////////mVDIREBER"
"IzRt///////////////////VQyIRAREiNFfv//////7Xd3zv//////9lMiERERI0Vu///////Xd3d3fv//////dUMhEREiNFf//////3d3Zmd3f//////9VD"
"IRERI0Vu//////13ZlVWd37//////WQyERESNFfv/////ndlVEVWd9//////1kMhERIjRt//////13ZUREVnff/////9ZDIhESI1bv/////8dlRERFV33///"
"///WQyIREjRX//////93ZURERVd+//////1kMhERI0V//////+d2VENEVnfv/////3VDIRESNGz//////ndVRERFZ3//////51QyERIjRt//////13VURERW"
"ff/////+ZTMhESI0bf/////9d2VERFZ33//////WQyIRESNG3//////XdlVVVmd///////dUMiEREjRt//////53dlVWZ33//////mVDIRERI0Xf/////+d3"
"d2Z3d///////dkMiERESNFf//////+d3d3d9///////mVDIREREiNW7//////+13d37///////dUMiERARIjRu///////////////////WVDIREQERI0Vv//"
"///////////////9ZUMiEREBESI0Xf////////////////11QzIREQARESM0bf//////////////93ZUMiEREAAREiNFbf/////////////ndlQyIREQAAER"
"EiNEVt///////////9d3VDMiEREAAAEREiM0Vmfe7v///////3dlQyIREQAAAAEREiM0RVZ3d9///////nZUMiEREAAAABEREiIzRFVmd+///////WVDIhEQ"
"AAAAABERESIjM0RWd+///////VQyEREAAAAAAAERERIiMzRVbf///////EMhEQAAAAAAAAEREREiIzRFbf//////8yIREAAAAAAAAAARERESIjNFXd3d3d3d"
"MiERAAAAAAAAAAARERERIiNERVVVVUQyIREAAAAAAAAAAAABERERIiMzREREMzIhERAAAAAAAAAAAAAAEREREiIjMzMyIiEREAAAAAAAAAAAAAAAARERESIi"
"IiIiERERAAAAAAAAAAAAAAAAABERERERERERERAAAAAAAAAAAAERERIiIiIiIiIiIiIhERERAAAAAAABEREiIjMzMzMzMzMyIiIhEREAAAAAARESIjM0RERE"
"REREREMzMiIREQAAAAEREiM0RVVVVVVVVVVVVEMzIhERAAABERIjRFZmd3d3d3d3d2ZlVDMiEREAABESI0X//////////////tZUQyIREAARESM03///////"
"/////////WVDIREQAREiNF3////////////////+ZDIhEQAREiNG7/////////////////1UMhEQEREjRW7/////////////////5kMiEQEREjRX////////"
"///////////VQyEQERIjRn///////////////////VQyEQERIjVt//////13d3d9///////lQyERERI0V9//////d3d3d37//////lQyERERI0V+//////d3"
"d3d33//////VQyERESI0Z+/////+d3d3d37//////VQyERESI1Z//////+d3d3d9//////5lQiERESM1bf/////9d3d3ff//////1kMiERESNFff////////"
"//////////dUMhERESNFfv/////////////////9ZDIhEREiNGfv/////////////////WVDIhEREiNGf/////////////////11QyIREREiNWf/////////"
"///////ndlQyIREBEjRW3///////////////d3ZUMiEREBEjRX3///////////////13ZUMiERABEjRX7/////93d+//////53VDMhERABEjRX7/////53d9"
"//////53VDIhERABIjRn//////53d3///////XVDIhERABIjRt//////13d37//////nZDIhEQABIjRt//////13d37//////3ZDIhEQABIjRu//////d3d3"
"f//////9ZTIhEQABIjRu/////+d2Znf//////+ZTIhEQABIjRv/////+dlVWff//////1TIhEQABEjRf/////+ZVRVbf//////1DIhEQABEjTf/////9VURE"
"Vu//////9DIhEQABEiPf/////8RDM0Re///////TIREAABESM0VWZlVUQzMzNEVWZmVUQyIREAABESIzRERERDMiIiMzRERERDMiEREAABERIiMzMzMyIiIi"
"IiMzMzMyIhERAAAAERESIiIiIiIREREiIiIiIiIREQAAAAARERERERERERERERERIREREREQAAAAAAAAAAAREREREiIiIiIREREREAAAAAAAABERERIiIiIi"
"IiIiIhEREQAAAAAAAREREiIjMzMzMzMzIiIRERAAAAAAERESIjM0RFVVVVREMzIhEREAAAABERIiM0RVVmZmZmZVVEMiEREQAAARESIzRFZn3e///+7XZVQz"
"IREQAAEREiM0Vm3//////////mVDIhERAAERIjNFbf////////////1UMiERABERIzRW///////////////lQyERABESI0Vv///////////////9QyERABES"
"NFb////////////////+QyIRAREiNG3//////9d3ff/////+UyIRAREiNW7/////93d3d9//////UyIRAREjRd//////53d3d9//////QyIRAREjRe//////"
"/Xd3d3d3d3dlQyIREREjRu////////7Xd3d3d3ZUQyIRERIjRu///////////td3d3ZUMyERERIjRu/////////////+d3ZUMiERERIjRd//////////////"
"/nZUMiERERIjRW////////////////dkMyERERIjRWf///////////////5lQyERERIjRWd+///////////////VQyERERIjRWd3zv/////////////mQyER"
"ESI0RWd3d3zv///////////mQyIRESI0Vnd3d3d3d97////////mQyERESNF/////+d3d3d9///////mQyERESNF//////d3d3d37//////VQyERESNN////"
"//13d3d37/////9lQyERESNM///////Xd3d+//////5kMiERESNF//////////////////dUMhERESNF/////////////////+ZDIhEQESI13///////////"
"/////mVDIhEQERI0Xv//////////////5lQyIREQERIjRd/////////////9ZUMiEREAEREjNFbf//////////1lVDIhERAAAREiM0VWfe7v///t12ZUQyIR"
"ERAAARESIzRFVmZmd2ZmVVRDMiEREQAAABERIiMzREVVVVVURDMyIhEREAAAAAERESIiMzM0REMzMyIiERERAAAAAAAREREiIiIjMzIiIiIREREQAAAAAAAB"
"EREREiIiIiIiERERERAAAAAAABERERIiIiIiIiIiIiIiIiIREREAABEREiIiMzMzMzMzMzMzMyIiIRERAAEREiIzNEREREREREREREMzMiIREQEREiM0RVVV"
"VVVmZlVVVVVURDMiERAREiNEVmZ3d3d3d3d3d3dmZVQzIRERESNN///////////////////+MiERESI07///////////////////1DIRERI0Xv//////////"
"/////////UMhEREjRf///////////////////1QyERESNF///////////////////+VDIRERI03///////////////////5UMhEREjTf////////////////"
"///UMiERESI+///////////////////9QyERAREjRFZ3d3fv/////3d3d2ZVQyIREBESI0RWZ3d///////d3dmVUMyIREQEREiM0VWd3//////53ZVRDMyIR"
"EQABERIjNEV3z//////XdVQzMiIRERAAERESIzRWff/////9dlQzIiERERAAABERIiNFZ+//////d1QzIiEREQAAAAARESI0Vn7/////92VDIiEREQAAAAAB"
"ERIjRWf//////nZDMiEREQAAAAAAAREiNFZ//////+dUMiEREQAAAAAAABERI0Vt//////11QyIREQAAAAAAAAEREjRX3//////WVDIREQAAAAAAAAARESNF"
"fv/////3ZDIhERAAAAAAAAABESI0V///////dkMiERAAAAAAAAAAERIjRn//////51QyEREAAAAAAAAAAREiNGf//////WVDIREQAAAAAAAAABESI0bf////"
"/9ZTIhEQAAAAAAAAAAERIjVu//////dkMiERAAAAAAAAAAAREiNW7/////91QyEREAAAAAAAAAABESI1b//////mVDIREQAAAAAAAAAAERIjVv/////+ZDIh"
"EQAAAAAAAAAAAREiNN//////1UMhERAAAAAAAAAAAAERI03//////UMiERAAAAAAAAAAAAAREiPv/////1QyEREAAAAAAAAAAAABERIzRVZmVUQyIREQAAAA"
"AAAAAAAAAREiM0REREMyIREQAAAAAAAAAAAAABERIiMzMzMiIREQAAAAAAAAAAAAAAARERIiIiIiEREQAAAAAAAAAAAAAAAAEREREREREREQAAAAAAAAAAAA"
"AAAAEREREREREREREREREREREREREQAAAAARERIiIiIiERERERERIiIiIiEREQAAABERIiMzMzMiIRERESIiMzMzMiIREQAAEREiM0REREMyIhERIiM0RERE"
"MyIREAARESI0RVZmVUQyIhIiM0RVZmVUMyEREAEREjRf/////+QyIiIjTf/////9MiERARESI03//////UMyIiNF3//////UMhEQERIjRd//////1UMiIzRe"
"//////VDIREBESI0bv/////2VDMjNFbv/////2QyIREREjRW7/////5lQzMzRX//////5kMiERERI0V//////+ZUMzNFZ//////+ZDIhEREiNGf//////WVD"
"MzRW3//////WQyIRERIjVt//////1lQzM0V9//////xkMiERESNFbf/////3ZUMzRGfv/////3ZDIhEREjRX7/////92VDM0Vn//////91QyERESI0Z+////"
"/+dlQzNFZ//////+dUMhEREiNGf//////nZUM0RWz//////WVDIRERIjVt//////12VDNEV9//////1lMiERESNFbf/////9dUQzRWfv/////3ZDIhEREjRX"
"7/////93VEREVn7/////92QyIRERI0V+/////+d1RERFZ//////+dUMhERESNGf//////nZVREVnf//////nVDIREBIjRn//////53VURFZ9//////1lMiER"
"ASI0bf/////9d2VVVnff/////8ZDIhEQEiNG3//////XdlVVZ37/////91QyEREBIjRt//////13dmZnd//////+ZUMhEQARI0bf/////+d3d3d37//////W"
"QyIREAESNF3//////8d3d3fv/////+ZUMhERABEjRd///////9d3ff///////WQyIREAARIjVu/////////////////+ZUMiERAAESI0Xv//////////////"
"//9lQyIREQABESNFb////////////////nVDMhERAAAREiNFb//////////////+ZUMyIREAAAEREiNFbv////////////1lQzIhERAAAAEREjNFbO//////"
"///+1mVDMiEREAAAABERIjNFVm3e7//+7ddmVEMiIREQAAAAABERIiM0VVZmZmZmZVVEMyIhERAAAAAAABEREiMzREVVVVVURDMyIhEREAAAAAAAABEREiIj"
"MzNERDMzMiIhEREQAAAAAAAAABERESIiIiIzIiIiIhERERAAAAAAAAAAABERERESIiIiIiERERERAAAAAAAAAAAREREREREREREREREREREREREREAABEREi"
"IiIiIhERERERESIiIiIhEREAEREiIzMzMzIiERERESIjMzMzIiEREBESIzRERERDMiIRESIjNERERDMiERAREjNFVmZlVEMiIiIjNEVVZVVDMiERESI9////"
"//5EMiIiM0Xf/////9IhEREjTf//////VEMiIzRW7/////5DIRERI0X//////2VDMzNFbf/////9QyEREiNG///////WVDM0Vn//////9UMhERIjRu//////"
"1lRERFbf/////9VDIRESI0bf/////9dlREVn7/////5lMiEREiNG3//////nZVVWff/////8VDIhERIjRt//////52ZVZn7/////5lQyERESI0Z///////d2"
"Zmff/////3ZDIhEQESNFf//////3d2Z3//////5lQyIREBEjRX7/////93d3ff/////nZDIhERARI0V9//////13d37/////1lQyIREAESNFbf/////9d3ff"
"/////3ZDMhERABEiNWf//////Xd37/////1lQyIREAARIjRn//////53d//////3ZDIhERAAESI0Z//////+d37/////1lQyIREAABESNFfv/////nd/////"
"/nZDIhERAAAREjRX3/////537/////1lQyIREAAAERI0Vt//////d//////nZDIhERAAABESI1Z//////37/////1lQyIREAAAAREiNGf//////v/////nVD"
"IhERAAAAEREjRX7///////////1lQyIREAAAABERI0V+///////////nVDIhERAAAAABESJFbf//////////dlQyEREAAAAAAREiNG3//////////nVDIhER"
"AAAAAAERIjRn//////////dlMyEREAAAAAABERI0V//////////mVDIhERAAAAAAABESI1bv///////+dkMiEREAAAAAAAAREiNF3////////WVDIREQAAAA"
"AAAAEREjRd////////ZUMiEREAAAAAAAAAERIjRf///////VQyIREQAAAAAAAAABERIjRVZmd2ZlVDIhERAAAAAAAAAAABERIjNEVVVVRDMiEREQAAAAAAAA"
"AAABERIiMzNEMzMiIRERAAAAAAAAAAAAABERIiIiIiIiIhEREAAAAAAAAAAAAAARERESIiIiIREREQAAAAAAAAAAABERERERERERERERESIiERERERERERER"
"EREQAAERESIiIiIiIiIiIiIiIiIiIiIiIiIiIiEREQARESIjMzMzMyIiIiMzMzMzMiIiIjMzMzMiIREQERIjNEREREQzMzM0REVVREMzMzM0REREMyIREBES"
"M0VWZmZVRDM0RVZmZmVUQzNEVVZlVUMyERERIjT//////1VERV7/////9VRERe//////MiERESNF7/////9lVVZu//////ZVVVbv/////kMhEREjRf//////"
"dmZn3//////3ZlZt//////VDIRESI0b//////3dmd///////93Znfv/////1QyEREiNW//////93d3f///////13d3//////1UMhERI0Vu//////d3d9////"
"///9d3ff/////mVDIRESNFbv/////3d3fv///////Xd37/////1lMiEREjRW7/////93d9////////13d//////3ZDIhERI0V+//////d3fv///////9d37/"
"////51QyERESNFfv/////8d3/////////Xd//////9ZTIhEQEjRX3//////Xff////////133/////51QyIREBI0V9//////137////////9d+/////9ZUMh"
"ERASNFbf/////9ff///+/////Xfv////92QyIREQEjRW3//////X7///9/////19/////+dUMiERABIzVt//////1/////f////+f//////GVDIREQASI1bP"
"/////9f////X/////n/////+dkMiERAAEiNWf//////t///+d+////7f/////WVDIREQABIjVn//////7v///nfv///+7/////dkMiEREAASI0Z/////////"
"//d37//////////nVDIhEQAAEiNGf//////////3d+//////////dlQyEREAABIjRn//////////13fv/////////nZDIhEQAAASI0Z//////////nd37///"
"//////1lQyEREAAAESNFfv////////53d9/////////nZDIhERAAABEjRX7////////8d3fv////////11QyIREAAAARI0V+////////93d37////////3ZT"
"IhERAAAAESNFbv///////9d3d9////////51QyIREAAAABEiNW7///////93Znff///////9ZTIhERAAAAARIjRu///////+dmZn3///////51QyIREAAAAA"
"ERI0Xf///////WVVVt///////9ZDIhERAAAAABESI07///////ZVRFXf//////9lQyIREAAAAAARESNN///////VRERE3//////+VDIhERAAAAAAAREiNFVm"
"ZmZlVEMzNEVWZmZmVEMiEREAAAAAAAEREiM0RVVVREMyIjM0RFVVREMyIREQAAAAAAAAEREiIzMzMzMyIiIiIzMzMzMyIhEREAAAAAAAAAEREiIiIiIiIiER"
"IiIiIiIiIhEREQAAAAAAAAABEREREiIiIREREREREiIiIREREQAAAAAAAAAAAAAAARERERIiIhEREREREREREREREQAAAAAAABEREiIiIiIiIiISIiIiIiIi"
"EREQAAAAAAARESIzMzMzMzIiIiIjMzMzMiIREQAAAAABERIjRERVVEQzMzMzNEREREMyIREAAAAAAREiNFVmZmZVVEMzRFVWZlVUMyIREAAAAAERI+//////"
"/mVERFVu//////8iERAAAAAREiPf///////WZVVm7//////0MhEQAAAAERIjXv//////53Zmff//////1DIREAAAABESI13///////d3d3///////UMiERAA"
"AAAREiNG7//////9d3f//////+VDIhEQAAAAEREjRt///////nd+//////5lQyERAAAAAAERI0V+///////X3//////mVDIhEQAAAAABESM1bf//////7O//"
"////dkMyEREAAAAAAREiNGfv////////////92VDIhEQAAAAAAERIjRX3////////////9ZUMiEREAAAAAABERI0Vn7///////////12QzIhEQAAAAAAARES"
"M0Z9///////////nZUMiEREAAAAAAAEREiNFd+/////////+d1QzIREQAAAAAAABERIjRXff////////93ZUMiEREAAAAAAAAREiI0V3f////////3d2QzIh"
"ERAAAAAAABERIjRGd9////////93dUMiEREAAAAAAAARESI0Vnz/////////13VDIhERAAAAAAABERIjNGd///////////d2QzIREQAAAAAAARESI0Vn7///"
"///////+dlQyIREAAAAAABERIjRWfv///////////3ZUMiERAAAAAAEREiM0Z+/////////////nZDIhEQAAAAABERIjRW3//////+//////92UzIREQAAAA"
"EREiNFbf/////+ff//////51QyIREAAAABERI0Vt//////93f///////dkMiERAAAAAREiNFf//////9d37//////+ZUMhEQAAABERI0Vv//////53d8////"
"///3VDIREAAAAREiNG7//////nd3d////////lQyERAAAAERI0Xv/////+d2Zmff//////9kMiEQAAABESNN///////GZVVWfv//////5DIhEQAAAREj3///"
"///9ZVRERW7///////QyIRAAAAERLf//////5VRDM0RW///////+MhEQAAABESI0RVVmVVRDMzMzRFVmZmVUQyIREAAAARESIzRERERDMyIiIzNERVVEQzIh"
"ERAAAAARESIjMzMzMiIiIiIiMzMzMzMiEREAAAAAERESIiIiIiIhERERIiIiIiIiIREQAAAAAAEREREREREREREREREREiIhEREREAAAAAABERERESIRERER"
"EREREREREREREQAAABEREiIiIiIiIRERERIiIiIiIhEREAAAEREiMzMzMzIiIhEiIiMzMzMyIhERAAEREiNEREREQzMiIiIjNEREREMyIREAAREiNFVmZmVU"
"QzMiMzRFVWZVVDMhEQABESPv//////1UQzM0RW7//////iIREBESI9///////mVERERW7//////zIhEQERIjXv//////dlVEVm3//////0MiERAREiNd////"
"///nZVVnf//////9QyIREBESI1b//////+d2Znf//////9VDIREQERIjRt///////Xd3ff/////+ZTMhEQARESNFfv/////+d3ff/////+ZUMiERAAERI0Vu"
"//////93d///////dUMyEREAAREiNGf//////9d+//////dlQyIREAABERI0V+//////5+//////1lQyIREQAAEREiNWf//////+//////51QyIREQAAABES"
"I0V+////////////52QzIhERAAAAEREjRW3///////////92VDIhERAAAAARESI0Z+//////////92VDIhERAAAAAAERIjRX3//////////XVDIhEREAAAAA"
"ARESM1Z//////////nZEMiEREAAAAAAAERIjRX3////////nZUMiEREAAAAAAAAREiNFd+///////ndUMiEREAAAAAAAABERI0Vn3//////3dkMyIREQAAAA"
"AAAAEREiNGd//////9dlQyIREQAAAAAAAAABESI0Z9//////12QyIREQAAAAAAAAAAERIjRn7/////93VDIhERAAAAAAAAAAAREiNGfv/////nZTMhERAAAA"
"AAAAAAABESI0Z+/////+dkMiEREAAAAAAAAAAAERIjRn//////51QyEREAAAAAAAAAAAARESNG3//////WUzIREQAAAAAAAAAAABERI0bf/////9ZDIhEQAA"
"AAAAAAAAAAAREjRe//////ZUMhERAAAAAAAAAAAAABESNF7/////5kMiERAAAAAAAAAAAAAAERIjTv/////lQyIREAAAAAAAAAAAAAAREiNP/////+QyIREQ"
"AAAAAAAAAAAAAAERIjRVVmVVQzIREQAAAAAAAAAAAAAAARESIzREREMyIhEQAAAAAAAAAAAAAAAAEREiIzMzMiIRERAAAAAAAAAAAAAAAAARERIiIiIiIRER"
"AAAAAAAAAAAAAAAAAAERERERERERERAAAAAAAAAAAAAAAAAAERERIiIiIiIiIiIiIiIhEREQAAAAAAERESIiIzMzMzMzMzMzMiIiEREAAAAAEREiIzM0RERE"
"RERERERDMyIhERAAAAAREiM0RFVVVVVVVVVVVVVEMyIREAAAAREiM0VWZnd3d3d3d3d2ZlVEMiERAAABESM9//////////////////8zIREAAAERI03/////"
"/////////////0MhEQAAARIjTv/////////////////+QyERAAAREiNf//////////////////VDIREAABESI1//////////////////1UMhEQAAERIjT///"
"//////////////1kMiERAAAREiPf////////////////5lQyERAAAAERIzRWZ3d3d37///////51QyIREAAAAREiNEVWd3d33///////92UzIREQAAABERIj"
"NFVnd33///////92VDIhERAAAAEREiIzRWZ3f////////GVDIhERAAAAARERIjNFZ3f////////XVDMhERAAAAABEREiM0Vnfv///////XZUMiEREAAAAAER"
"EiI0Vnfv///////nZUMiEREAAAAAARESIzRWfv///////nZUMiEREQAAAAARERIjRWff///////ndUMyIREQAAAAABERIjRWff///////ndlQyIRERAAAAAB"
"ERIjNFff///////3dlQzIhEREAAAAAEREiNFZ////////9d1VDMiEREQAAAAEREiNFZ////////9d2VEMyIRERAAAAEREiNFZ////////+d3ZUQzMiEREAAA"
"ARESM0Z+///////+d3dmVUQzIhEQAAABESI0V9///////+d3d3ZlVEMyEREAABERI0Vt////////d3d3d3dlVDIhEQAAERIjRt//////////////////QyER"
"AAAREjRW//////////////////9DIREAABEiNG///////////////////kMhEQAAESI17//////////////////+QyERAAARIz3///////////////////1D"
"IREAABEiPv//////////////////9UMhEQAAESI+///////////////////0MiERAAAREjNFVmZ3d3d3d3d3d2ZmVUMyERAAABESIzREVVVVVVVVVVVVVVRE"
"MyIREAAAAREiIzM0REREREREREREMzMiIREQAAABEREiIiMzMzMzMzMzMzMiIiEREQAAAAARERESIiIiIiIiIiIiIiIREREQAAAAAAAAABERERERERERAAAA"
"AAAREREiIiIhEREAAAAAERESIjMzIiIREQAAAAEREiM0REMzIhEQAAABERIjRFVVVDMiERAAABESI0//////MiERAAABESNF/////+QyERAAAREiNF/////+"
"QyERAAAREiNd/////9QyIRAAAREjRe/////1QyERAAAREjRe/////1QyERAAEREjRf/////1QyERAAEREjRf/////kMiERAAEREjNFZ3d2ZUMiERAAERIjNF"
"Z3d2VDMhERAAERIjRVZ3dmVDIhERABERIjRd///9VDIhERABERIzRe///9VDIhEQABESI0Vv///9VDIhEQABESI0Xf///8VDIhEQARESM1bf///3VDIREQAR"
"EiNFbv///3VDIREQAREiNFf////3VDIREQEREjNW3////nVDIREQEREjRW7////nVDIREQERIjRX/////nVDIREQERIjRt/////nVDIREQERI0Vu/////mVD"
"IREQERI0V//////mVDIREAERI0V//////mUyIREAESI0bf/////mUyIREAESI0bv/////WQyIREAESI0b//////VQyEREAESI03//////VQyERAAERI07///"
"///UMiERAAERIz///////UMhERAAERIjRFVmZVRDIhEQAAEREiM0REREMyIREQAAARESIjMzMzIiEREAAAABEREiIiIiIRERAAAAAAERERERERERERAAAAAA"
"AREREREREREQAAAREREiIiIhEREAABERIiIzMiIhERABERIjM0RDMyIREAERIjRFVVRDMhEREREjTv////8yIREREiNO/////kMhERESNF/////9QyERERI0"
"X/////1DIREREjTf////9UMhERESNN/////1QyERERI07////+QyIREREiP/////5DIRERERIzRFVVRDIhEQAREiMzREMzIhERABERIiIzMiIhERAAARERIi"
"IiIREREAAAEREREREREREAAAAAAAAAABERERERERERAAAAAAAAAAAAAREREiIiIhEREAAAAAAAAAAAARESIjMzIiIREQAAAAAAAAAAEREiM0REMzIhERAAAA"
"AAAAABERIjRFVVVDMiERAAAAAAAAABESI07/////MyERAAAAAAAAARESNF/////+QyERAAAAAAAAAREiNF/////+QyEREAAAAAAAAREiNd/////9QyIREAAA"
"AAAAEREjRd/////2QyIREAAAAAAAERIjRe/////1QyEREAAAAAABERIjRf/////1QyEREAAAAAARERIjRf/////lQyEREAAAAAARESIzRWd3d3ZUMiEREAAA"
"AAEREiI0VWd3d3ZUMiEREAAAABEREiM0Vnd3d2ZUMiEREAAAARERIjNFZ/////1DMiEREAAAARESI0RWfv////xDMiEREAAAEREiNEVn7////+VDMiEREAAB"
"ERIjRVbP/////9VDMiEREQABESI0Vn7//////WVDMiIREQARESNEZ///////12VEMyIhERAREiNFf//////8d2VUQzIiERAREjRW//////53d2ZlVEMiERER"
"IjRu/////+d3d3d2ZVQyIRERI0Xf/////9d3d+////9DIRERI0Xv/////nd3d/////9DIRERI0Xv/////nd3ff////9DIRERI0Xv/////3d3f/////1DIRER"
"I0Xv/////+d3/////+VDIRERI0Xf/////////////9VDIRERIjVv/////////////lQyIREREjRd////////////5UMyEREREiNF7//////////9VEMhERAR"
"ESNEXf////////1lQzIhERABESI0RWbe///u1mVUMyIREQABERIjNEVWZmZmVVRDIiEREAAAEREiIzREVVVURDMyIhEREAAAARERIiMzMzMzMyIiERERAAAA"
"ABERESIiIiIiIiIREREAAAAAAAERERESIiIiERERERAAAAAAAAAAAAAAAAAAAAAREREREREREAAAAAAAAAAAAAAAAAAAARERESIiERERAAAAAAAAAAAAAAAA"
"AAARERIiIiIiIREAAAAAAAAAAAAAAAAAERESIjMzMzIiERAAAAAAAAAAAAAAAAERESIzREREQzIREAAAAAAAAAAAAAAAARESIzRf/////SERAAAAAAAAAAAA"
"AAARESIzRf////8yIREAAAAAAAAAAAAAARESIzRe////5DIhEQAAAAAAAAAAAAARESIjRe////1UMiERAAAAAAAAAAAAARESIjRd///+dlQyIREAAAAAAAAA"
"AAARERIjRd///9d2VDIhEQAAAAAAAAAAABERIjRWZ3d3d3ZUMiEREAAAAAAAAAABERIjRWd3d3d3dlQyIREQAAAAAAAAABERIjRW7//////+ZDMhERAAAAAA"
"AAAAEREjNW3///////9lQyIREAAAAAAAAAEREiNFf////////3VDIhEQAAAAAAAAAREiNFbf////////1lMiERAAAAAAAAARESM0Z//////////mVDIREQAA"
"AAAAARESI0V9/////////+dUMhERAAAAAAABERIzVn//////////92QyIREAAAAAABERIjRX7//////////3ZDIhEQAAAAAAERIjRWf///////////1lMiER"
"AAAAAAEREiNFfv/////f/////nVDIREQAAAAAREiNFbf/////X/////+dUMhERAAAAARESI0Z+/////nf/////52QyIREAAAABESI0Vt/////9d+/////3ZD"
"IhEQAAABERIzRn//////d3//////xlMyERAAABERIjRX3/////13f//////XVDIREQAAEREjRWf/////53d+/////+dUMhERAAEREiNFff/////Xd37/////"
"52QyIREAARESNFZ//////3d3ff/////3ZDIhEQARESI0V+/////+d3d9//////dlMiERABERI0Vn//////d3d33//////WVDIREAERIjRX7/////////////"
"///+dUMhEQEREjRWz/////////////////51QyERAREiNFfv/////////////////nVDIREBESI1bf//////////////////dkMhERERI0V+////////////"
"///////WQyIRERIjRt///////////////////9ZDIhEREiNW////////////////////5kMiERESNF3//////Xd3d3d33//////mQyERERI0X//////nd3Zm"
"Z3ff//////VDIREREiPv/////8dlVVVVZn7/////9UMhERESI//////+ZVRERERFb//////9MiERAREjNFVmZlVEQzMzMzRFVmZmVUMyEREBESIzRERERDMy"
"IiIiMzRERUREMyIREAAREiIzMzMzIiIiIiIiIzMzMzMiIREQABEREiIiIiIiERERERIiIiIiIiEREQAAARERERERERERERERERERIiEREREQAAAAAAAAAAAA"
"AAABEREREREREQAAAAAAAAAAAAAAAAARERERIiIREREAAAAAAAAAAAAAAAEREREiIiIiIhERAAAAAAAAAAAAABERERIiMzMzMyIREQAAAAAAAAAAABERESIj"
"NEVVRDMiEREAAAAAAAAAARERESIjRF/////9IREQAAAAAAAAERERESIzRV/////jMiEREAAAAAABERERIiIzRV7////UQyIREQAAAAABERESIiIzRV7///9l"
"VDMiEREAAAABERIiIzM0RV7///52ZUMyIREQAAABERIjM0REVW3///13dlVDMiEREAABERIjNEVVZmd3d3d3d3ZVRDIhEQABERIjRVZnd3d3d3d3d3d2ZUMy"
"ERAAERIjRf/////////////////TIhEAEREjNN/////////////////9MiEQAREiNF3/////////////////VDIRABESI0bv////////////////9UMhEBER"
"I0Vu/////////////////lQyEQEREjRX/////////////////+QyIRAREiNGf/////////////////1DIREBESI1bf/////9d3d3d3d3dmVDIhERERI0V9//"
"////d3d3d3d3ZlVDMhEREREjRX7/////93d3d3d3ZlVDMiEREREiNGfv/////nd3d3d3dmVEMiEREBESI1Z//////+d3d3d3d3ZUMyIREQERIzVt////////"
"////////QzIREQAREjRX3///////////////5UMhERABESNFfv///////////////lQyEREAESI0Z+///////////////9VDIREQARIjRn//////////////"
"//xDIhEQABEiNWf///////////////9UMiERAAESNFbf///////////////lQyIREAARI0V9//////d3d3d3d3dlQzIREQABEjRX7/////93d3d3d3dmVDMh"
"ERAAESNFfv/////nd3d3d3dmVUMyIREAASI0Z//////+d3d3d3d3ZlRDIhEQABIjRt//////13d3d3d3d2ZUMiERAAEiNG3/////////////////0zIREAAS"
"I1bv/////////////////UMhEQABIjVu/////////////////1QyERAAEiNG/////////////////+VDIREAARI0X/////////////////5TIhEQABEjTf//"
"///////////////kMiERAAESI9/////////////////9QyERAAAREjNFVmd3d3d3d3d2ZmVUQyIREAABESIzRFVVVVVVVVVVVVREMyIREAAAEREiIzM0RERE"
"REREREMzMiIREQAAABEREiIiMzMzMzMzMzMiIiEREQAAAAAREREiIiIiIiIiIiIiIhERERAAAAAAAAAAAAABEREREREREAAAAAAAAAABERERIiIREREAAAAA"
"AAAAERESIiIiIiEREAAAAAAAARERIjMzMzMiERAAAAAAABEREiM0RERDMiERAAAAAAERESI0Tv////4hEQAAAAABERIjRe/////SIREAAAAAEREiNF3////k"
"MiERAAAAARESIzTf///9VDIhEQAAAAEREiNF////ZUQyIREAAAARESI0X///52VDMhERAAAAEREjNFZ3d3dlQyIREQAAABESI0Vnd3d3ZUMiEREAAAEREjRe"
"//////VDIhEQAAABESI0Xv/////1QyIREAAAEREjRW//////5kMiERAAABERI0V//////+ZDIhEQAAAREiNG3//////WQyIREAAAERIjVt//////1kMiERAA"
"ARESNFbv/////3ZDIhEQAAEREjRX7/////91QyEREAABESI0Z//////+dUMhERAAAREiNGf//////WVDIREAAAERI0Vt//////1lMiERAAARESNFfv/////3"
"ZDIhEQAAEREjRX7/////92QyIREAABESI0Z//////+dUMiERAAAREiNGf//////nVDIREQAAERIzVt//////1lQyERAAABESNFbf/////9ZTIhEQAAAREjRX"
"7/////92QyIREAAAESI0V+//////dkMiERAAABEiNGf//////nVDIREQAAARIjRn//////11QyEREAAAESI1bf/////9ZTIhEQAAABEjNW3/////92QyIREA"
"AAARI0Vu//////dkMiERAAAAESNFfv/////nVDIREQAAABEjRX//////5lQyERAAAAARI0Xf/////9ZDIhEQAAAAESNF3//////VQyEREAAAABEiNe//////"
"ZUMhERAAAAARIjTv/////1QyIREAAAAAERI0//////5DMhERAAAAABESI0RVZmVUMyIREAAAAAARESIzREREQzIhERAAAAAAARESIjMzMzIiEREAAAAAAAAR"
"ERIiIiIiEREQAAAAAAAAARERERERERERAAAAAAAAAAAAAAAAAAAREREREREREREREREAAAAAAAAAAAAAAAEREREiIiIiIiEREREQAAAAAAAAAAAAABEREiIj"
"MzMiIiIiIhERAAAAAAAAAAAAARERIjM0REQzMzMzIiEREAAAAAAAAAAAEREiM0RVVVVUREREMyIREAAAAAAAAAARERIjNFXv/uZmVVX//jIREQAAAAAAAAER"
"ESIzRe/////td27//TIhEQAAAAAAABERESM0X///////////5DIhERAAAAAAARERIiNF7///////////1DMhERAAAAAAERESIjRN///////////9VDMiEREA"
"AAABEREiM0Re//13ff/////WVUMiIREQAAABERIjNEVv/9d3d33u/td2ZUQyIREQAAARESI0RWZ3d3dmZmZnd3d3ZlRDIhEQAAEREiNFZnd3d3dmVVVmZ3d3"
"d2VUMiERAAERIjRf/////ndlVVVVZ9/////9MyERABERI0Xf/////9dmVUVVZ+/////9QyERABESI0Xf//////d2VURVZ+/////1QyERABESI1bv//////53"
"ZVVWZ//////2QyIRARESNFfv///////XdlVWd//////mQyIRARESNFf////////3d2Zmff/////mQyIRAREiNGf////////+d3Znff/////WQyIRAREiNW3/"
"////////13d3fv/////GQyIREREjRX3/////////93d3f/////92QyIREREjRX7//////////nd3f/////91QyERERIjRn7//////////9d3f/////51QyER"
"ERIjVn////////////d33/////1lQyERERIjVt////////////537/////1lMiERERI0V9/////////////X7/////dkMiERERI0V+/////////////3////"
"//dkMiERESI0V+///////////////////+dUMiERESI0Z//////u/////////////+dUMhERESI0Z//////n7////////////9ZUMhEQESJFbf/////nf///"
"/////////9ZTIhEQESNFff/////Xff///////////3ZDIhEQESNFfv/////Hd+///////////3ZDIhEQESNFfv////53d3///////////nVDIREQEiNGf///"
"//53d33//////////XVDIREQEiNG3/////53d3fv/////////WUyIREAEiNG3/////13Zmd+////////92QyIREAEiNG7/////12VWZ8////////91QyEREA"
"EiNG7/////dlVVVn7///////5lQyEREAEiNG/////+dlRERWfv//////5kMiERAAESNF/////+ZUQzRFbP//////1UMhERAAESNN/////+VEMzM0Vt//////"
"1DMhEQAAESI9/////9RDIiIzRW7/////VDIhEQAAERIzRVVVVUMyIiIjNEVWZmZUQyIREQAAERIjM0REQzMiIRIiIzRERERDMiEREAAAEREiIjMzMiIhERES"
"IiMzMzMyIhERAAAAARERIiIiIiERERERESIiIiIiEREQAAAAABERERERERERERERERERIhEREREAAAAAAAAAAAAAAAAAAAABEREREREREAAAAAAAAAAAAAAA"
"AAAAAAEREREiIREREAAAAAAAAAAAAAAAAAAAERERIiIiIiEREAAAAAAAAAAAAAAAAAARERIiMzMzMiERAAAAAAAAAAAAAAAAABEREiM0RERDMiERAAAAAAAA"
"AAAAAAAAERESI0Tv////8hEQAAAAAAAAAAAAAAARERIjRd/////TIREQAAAAAAAAAAAAAREREiNE3////0QyIREAAAAAAAAAAAABERESIzTP///+VDMiERAA"
"AAAAAAAAABERERIiNF////1lQzIhERAAAAAAAAAAERERIiM0Tv//52VUMyIRERAAAAAAAAERERIiIzRFZnd3dmVDMiIREQAAAAAAAREREiIzRFVnd3d3dlVD"
"MiIREQAAAAABEREiIzRFVnd3d3d3dmVEMiEREQAAAAERESIzRFZnfe7//+7td2VEMiEREAAAARERIjRFZs7/////////7WZUMiEREAAAEREiNFVt////////"
"/////3ZUMiERAAARESI0Vn///////////////9ZTMhERABERIjRW3////////////////9VDIhEQAREiNFbf/////////////////mVDIREBERIzRt//////"
"/////////////WQyIRAREiNFfv//////7Xd3zv//////9lMiERERI0Vu///////Xd3d3fv//////dUMhEREiNFf//////3d3Zmd3f//////9VDIRERI0Vu//"
"////13ZlVWd37//////WQyERESNFf//////ndlVEVWd9//////1kMiERIjRt//////13ZUREVnff/////9ZDIhESI1bv/////8dlRERFV33//////WQyIREj"
"RX//////93ZURERVd+//////1kMhERI0V//////+d1VENEVnfv/////3VDIRESNGz//////ndVRERFZ3//////51QyEREjRt//////13VURERWff/////+ZT"
"MhESI0bf/////9d2VERFZ33//////WQyIRESNG3//////XdlVEVWd///////dUMhEREjRt//////53dlVWZ33//////mVDIRERI0Xf/////+d3dmZ3d/////"
"//dkMiERESNFf//////+d3d3d9///////mVDIREREiNW7//////+13d37///////dUMiERARIjRd///////////////////GQzIREQERI0Vv////////////"
"/////9ZUMiERABESI0Xf////////////////1lQyIREQARESM0bf//////////////52VDIhERAAAREiNEV+/////////////WVUMiEREAAAEREiNEVs7///"
"//////7WZUMyIRERAAAAEREiM0VWfe7v//7td2ZUQzIhEREAAAAAEREiM0RVVmZmZmZlVUQzIiEREQAAAAAAEREiIzNERVVVVVREMzIiERERAAAAAAAAERES"
"IiMzM0RDMzMyIiIREREAAAAAAAAAERERIiIiIjMiIiIiEREREAAAAAAAAAAAERERERIiIiIiIREREREQAAAAAAAAAAAAAAAAAAAAABEREREREREAAAAAAAAA"
"AAAAAAAAAAAAEREREiIREREAAAAAAAAAAAAAAAAAAAARERIiIiIiEREAAAAAAAAAAAAAAAAAARERIiMzMzMiEREAAAAAAAAAAAAAAAABEREiM0RERDMiEREA"
"AAAAAAAAAAAAABERESIzTv////8iEREAAAAAAAAAAAARERERIjNN/////TIhERAAAAAAAAAAERERERESI03////0QyIRERAAAAAAAAERERERERIjPP///+VE"
"MiIRERAAAAAAARERERIiIiI0////1VRDMiIREREAAAABEREiIiIiIjPv//5lVEQzMiIhERAAAAEREiIzMzMzMzREVVVVVEREMyIhERAAARESIzREREREMzRE"
"RFVVVVVUQzIhEQABERIjRFVmZVVEMzM0RFVmZmZlQzIhEQARESNF//////5EMzM0Rd//////0yIREBERIjTf/////9VDMzNFXf/////9QyERAREiNF3/////"
"/VQzMzRW7/////9UMhEQERIjRu//////ZUMzM0Vu//////ZDIhERESNFbv/////mVDMzRGf//////mQyIREREjRX//////51QzM0Vn//////5kMiERESI0Z/"
"/////9ZUMzNFbf/////9ZDIhEREiNW3//////WVDMzRX3//////GQyIRERI0Vt//////dlQzNEZ+//////dkMiERESNFfv/////3ZUMzRWf//////3VDIRER"
"IjRn7/////52VDM0Vn//////51QyERESI0Z//////+dlQzRFbP/////9ZUMhEREiNW3//////XZUM0RX3//////WUyIRERI0Vt//////11RDNFZ+//////dk"
"MiERESNFfv/////3dURERWfv/////3ZDIhEREjRX7/////53VEREVn//////51QyERERI0Z//////+dlVERWd//////+dUMhEQEiNGf//////ndVREVn3///"
"///WUyIREBIjRt//////13ZVVWd9//////xkMiERASI0bf/////9d2VVVnfv/////3VDIREQEiNG3//////Xd2Zmd3//////5lQyERABEjRt//////53d3d3"
"fv/////9ZDIhEQARI0Xf//////x3d3d+//////5lQyEREAESNF3///////13d9///////9ZDIhEQABEiNW7/////////////////5lQyIREAARIjRe//////"
"///////////2VDIhERAAERI0Vv///////////////+dUMyEREAABESI0Vv//////////////5lQzIhEQAAARESI0Vu/////////////WVDMiEREAAAARESM0"
"Vs7/////////7WZUMyIREQAAAAEREiM0VWbd7v//7t12ZUQyIhERAAAAAAEREiIzRVVmZmZmZlVUQzIiEREAAAAAAAERESIzNERVVVVVREMzIiEREQAAAAAA"
"AAERESIiMzM0REMzMyIiERERAAAAAAAAAAERERIiIiIjMiIiIiEREREAAAAAAAAAAAERERERIiIiIiIREREREAAAAAAAAAAAAAAAAAABEREREREREREREREA"
"AAAAAAAAAAAAAAERERIiIiIiIiIREREAAAAAAAAAAAAAARERIiIiIiIiIiIiEREAAAAAAAAAAAABERIiMzMzMzMzMzMiEREAAAAAAAAAAAEREiM0RVRERERV"
"RDMiERAAAAAAAAAAARERIz3///9VXf///zMhERAAAAAAAAABEREiNO////ZW7///9DIhERAAAAAAAAERESI0Tv///mZv///+QyIRERAAAAAAARERIiNF////"
"5mb////lQyIREQAAAAAREREiIzRf///9Zt////1UMiIREQAAAAEREiIzNE3////VXf///2VDMiIREAAAARESIzNERVZmZlVVVmd3ZlVDMiEREAABERIjNEVV"
"ZmZlVURFVmd3ZmVEMiERAAEREiNEVmZ3ZmVURERFZnd3d2VUMiERABESI0X//////lREM0RW3//////TMhEQEREjNN//////1UQzNEVt//////1DIREBESI0"
"Xf/////9VEMzRFbv/////2QyERAREiNG7/////91RDM0Rn7/////9kMiERERI0Vu/////+dUQzNFZ//////+ZTIhERESNFf//////nVEMzRWf//////mUyIR"
"ERIjRn//////11QzM0Vt//////1kMiERESI1bf/////9ZUMzRFff/////8ZDIhEREjRW3/////92VDM0Rn7/////92QyIRERI0V+//////dlQzNFZ///////"
"dUMhEREiNGfv/////nZUMzRWf//////nVDIRERIjRn//////52VDNEVs//////1lQyERESI1bf/////9dlQzRFff/////9ZTIhEREjRW3//////XVEM0Vn7/"
"////92QyIRERI0V+//////d1RERFZ+//////dkMiERESNFfv/////ndURERWf//////nVDIREREjRn//////52VURFZ3//////51QyERASI0Z//////+d1VE"
"RWff/////9ZTIhEQEiNG3//////XdlVVZ33//////GQyIREBIjRt//////13ZVVWd+//////dUMhERASI0bf/////9d3ZmZ3f//////mVDIREAESNG3/////"
"/nd3d3d+//////1kMiERABEjRd///////Hd3d37//////mVDIREQARI0Xf///////Xd33///////1kMiERAAESI1bv/////////////////mVDIhEQABEiNF"
"7/////////////////ZUMiEREAAREjRW////////////////51QzIREQAAERIjRW///////////////mVDMiERAAABERIjRW7////////////9ZUMyIREQAA"
"ABERIzRWzv/////////tZlQzIhERAAAAARESIzRVZt3u///u3XZlRDIiEREAAAAAARESIjNFVWZmZmZmVVRDMiIREQAAAAAAARERIjM0RFVVVVVEQzMiIRER"
"AAAAAAAAARERIiIzMzREQzMzIiIREREAAAAAAAAAAREREiIiIiMyIiIiIREREQAAAAAAAAAAAREREREiIiIiIhEREREQAAAAAAAAAAAAAAAAAAAAARERERER"
"EREREiIiIiIREREREREREQAAAAAAAAAAAAAAAAAAERERERERESIiIiIiIiIiIiIREREREREQAAAAAAAAAAAAAAAAERERERERIiIiIiIiIiIiIiIiIhERERER"
"EAAAAAAAAAAAAAABEREREREiIiIiIzMzMzMzMzIiIiIhERERERAAAAAAAAAAAAABERERERIiIiIzMzMzMzMzMzMzMyIiIhEREREQAAAAAAAAAAABERERERIi"
"IjMzM0REREREREREMzMzIiIhEREREAAAAAAAAAABERERESIiIzM0RERFVVVVVVVVRERDMzIiIRERERAAAAAAAAABERERESIiMzNERFVVVmZmZmZlVVVERDMy"
"IiEREREQAAAAAAABEREREiIiMzREVVVmZmd3d3d2ZmZVVURDMyIhEREREAAAAAABEREREiIjM0RFVWZmfN7v////7t12ZlVEQzMiIREREQAAAAABEREREiIj"
"M0RVVmZ+/////////////WZlVEMzIiEREREAAAAAEREREiIjM0RVZm3////////////////mZVRDMyIhEREQAAAAEREREiIjNERVZt//////////////////"
"/mVUQzIiEREREAAAEREREiIjNEVWZ/////////////////////5mVEMyIhEREQAAERERESIjNEVWbv/////////////////////+ZVRDMiIRERAAARERESIj"
"NEVWbv///////////////////////mVUMyIhEREAARERESIjM0VWbv////////////////////////1lRDMiIREQABERERIiM0RWbv//////////////////"
"///////mVUMyIhERABERERIiM0RWbf//////////////////////////1lRDMiEREAERERIiM0RVZ////////////////////////////mVUMyIhEQEREREi"
"IzRVZ/////////////////////////////dlRDIiERAREREiIzRFZt////////////13d33////////////WVEMyIRERERESIjNFVn///////////9d3d3d3"
"7//////////9ZVQzIhERERESIjNEVn7//////////9d3d3d3fP//////////52VDMiIRERERIiM0VWf//////////9d3d3d3d37//////////3ZUQyIhERER"
"IiM0RWfv/////////+d3d3d3d3ff//////////dlRDIiEREREiIzRVZ///////////13d3d3d3d3//////////92VEMyIRERESIzRFZ9//////////53d3d3"
"d3d3f//////////3ZVQzIhERESIjNFVn7//////////Xd3dmZnd3d///////////dlVDMiERERIiM0Vm3//////////3d3dmZmZ3d3//////////92VUMyIR"
"EREiM0RWfv/////////+d3dmZmZnd3f//////////3ZVQzIhEREiIzRVZ///////////13dmZVVmd3ff//////////dlVDMiERESIjNFZn///////////Hd2"
"ZVVWZnd9//////////52VUMyIRERIiNEVn3//////////nd2ZlVVVmd33//////////nZVQzIhEREiM0RWfv/////////+d3ZlVVVWZ3fv/////////+dlVD"
"MiEREiIzRVZ+//////////53dmVVVVZnd+//////////52VEMyIRESIjNFZs///////////Xd2VVVVVmd3///////////nZUQzIhERIiNEVn3//////////3"
"d2ZVVVVWZ3f//////////+dlRDIiEREiM0RWfv/////////+d3ZlVVVVZ3ff//////////12VEMiIREiIzRVZ+//////////53dmVVVVVnd9///////////X"
"ZUMyIhESIjNFVn7//////////nd2ZVVVVmd37//////////3ZlQzIiERIiM0Vmf//////////9d3ZlVVVWZ3f///////////dlVDMiIREiIzRWff////////"
"//13dlVVVVZnd///////////52VEMyIRESIjRFZ9//////////93dmVVVVVmd9///////////XZUQyIhERIiNEVn7//////////nd2ZVVVVmd33/////////"
"/8ZlQzIiEREiM0RWfv/////////+d3ZlVVVWZ3fv/////////+dlVDMiIRESIzRFZ+//////////53dmVVVWZnd+//////////52VEMyIRERIjNEVn//////"
"/////Xd2ZlVVZnd3///////////XZUQyIhEREiM0RWf//////////9d3dmVVZmd33//////////3ZVQzIiERESIzRFZ///////////13d2ZmZmd3fv//////"
"///+dlRDMiIRERIjNEVn///////////Xd3ZmZmd3d+//////////12VEMiIREREiM0RWfv/////////8d3d2Zmd3d9//////////92VUMyIhERESIjRFZ///"
"////////13d3d3d3d37//////////nZUQzIiERERIiNEVn///////////Xd3d3d3d3f//////////3ZlQzIiEREREiIzRWfv/////////+d3d3d3d3fv////"
"/////+dlRDMiIRERESIjNFZ+//////////53d3d3d3ff//////////1mVEMyIRERERIiM0Vm3//////////9d3d3d3d///////////52VEMyIhEREQESIzRV"
"bf///////////Hd3d3ff//////////92VUQyIiERERARIiNEVn////////////53d33v///////////nZUQzIiERERABEiIzRWb/////////////////////"
"///////3ZVQzIiEREREAESIjNFVt///////////////////////////9ZlRDMiIREREAAREiM0RWz//////////////////////////9ZlRDMiIREREQABES"
"IjNFVu/////////////////////////+dlVDMyIhEREQAAEREiM0RWb/////////////////////////dlVEMyIhERERAAARESIjNFVu////////////////"
"////////dlVEMyIhERERAAABERIiM0RVbv/////////////////////+ZlVEMyIiEREREAAAERESIjNEVW7////////////////////9ZlVEMyIiEREREAAA"
"AREREiIzRFVt///////////////////mZlVEMyIiEREREAAAABERESIiM0RVZu////////////////52ZVREMyIiEREREQAAAAEREREiIzNEVWZt7///////"
"/////tdmVVRDMyIiEREREQAAAAABERERIiMzREVWZnze7////+7dd2ZlVURDMyIiEREREQAAAAAAARERESIiMzRFVVZmZnd3d3dmZmVVVEQzMiIiEREREQAA"
"AAAAABEREREiIjM0REVVVWZmZmZmVVVUREMzMiIiEREREQAAAAAAAAARERERIiIjMzRERVVVVVVVVURERDMzIiIhEREREQAAAAAAAAAAERERESIiIzMzNERE"
"RERERERDMzMyIiIhEREREQAAAAAAAAAAABERERESIiIjMzMzMzMzMzMzMzIiIiIREREREQAAAAAAAAAAAAARERERESIiIiIzMzMzMzMzIiIiIiEREREREQAA"
"AAAAAAAAAAAAERERERESIiIiIiIiIiIiIiIiIREREREREQAAAAAAAAAAAAAAABERERERERIiIiIiIiIiIiIhEREREREREAAAAAAAAAAAAAAAAAABERERERER"
"ERIiIiIiIREREREREREREAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARERERERERERERERERERAAAAAAAAAAAAAAAAAAAAAAAREREREREREREREREREREREAAAAA"
"AAAAAAAAAAAAAAABERERERERERERERERERERERERAAAAAAAAAAAAAAAAAAERERERERERESIiIiIiIiIREREREAAAAAAAAAAAAAAAARERERERERESIiIiIiIi"
"IiIiEREREAAAAAAAAAAAAAAAERERERERESIiIiIjMzMzMyIiIREREQAAAAAAAAAAAAABERERERESIiIiIjMzMzMzMzMiIhERERAAAAAAAAAAAAARERERERIi"
"IiIjMzM0RERERDMzIiERERAAAAAAAAAAABERERERIiIiIjMzNERERVVVREQzMiIRERAAAAAAAAAAARERERESIiIjMzNEREVVVVXv///jMiIREREAAAAAAAAA"
"ERERERIiIiMzNEREVVVmZt/////UMyIREREAAAAAAAABEREREiIiMzM0REVVVmZ33//////UMyIhEREAAAAAAAABERERIiIjM0REVVVmZ3ff//////9UQzIh"
"EREQAAAAAAARERESIiMzRERVVmZ3d+////////9UQzIhEREQAAAAAAAREREiIjM0RVVWZnd97/////////5VQzIhEREQAAAAAAERERIiIzREVWZnd87/////"
"//////5lQzIiEREQAAAAAAERERIiM0RVVmd97/////////////1lQzIiEREQAAAAAAERESIjNEVWbe////////////////1lRDIiEREQAAAAAAERESIzNP//"
"//////////////////dlRDIiERERAAAAAAERESIzRP////////////////////dlRDIiERERAAAAABEREiIzRf///////////////////+dlRDIiERERAAAA"
"ABEREiM0Tf///////////////////+dlQzIiERERAAAAABEREiM0Tf///////////////////9dlQzIiERERAAAAABEREiM0Tv///////////////////3Zl"
"QzIiERERAAAAABEREiM0T////////////////////3ZlQzIiERERAAAAABEREiM0T////////////////////nZVQzIiERERAAAAABEREiI0T///////////"
"/////////nZVQzIiEREQAAAAABEREiIz3////////////////////XZUQzIhEREQAAAAABERESIzRFVmd3d3d+///////////XZUQzIhEREQAAAAAAERESIj"
"NEVWZ3d3d+//////////93ZUQyIhEREQAAAAAAERESIjM0RVZnd3d///////////93ZUMyIhEREQAAAAAAERERIiMzRFVmd3d///////////53ZUMyIhEREQ"
"AAAAAAERERIiIzREVWd3ff//////////52VUMyIhEREQAAAAAAEREREiIzNEVWZ3ff//////////12VUMyIhEREQAAAAAAAREREiIjM0RVZ3fv//////////"
"x2VEMyIhEREQAAAAAAARERESIiM0RVZ3f///////////d2VEMyIREREQAAAAAAARERESIiM0RVZ3f//////////+d2VEMyIREREAAAAAAAARERESIiMzRVZ3"
"f//////////+d2VDMiIREREAAAAAAAARERESIiMzRVZ33//////////9dmVDMiIREREAAAAAAAARERESIiMzRVZ37//////////9dlVDMiIREREAAAAAAAAR"
"ERESIiM0RVZ37//////////3dlVDMiIREREQAAAAAAERERESIiM0RWZ3///////////3dlVDMiIREREQAAAAAAERERESIiM0RWd3///////////ndlRDMiIR"
"EREQAAAAABERERESIjM0RWd9///////////ndlRDMiIREREQAAAAABEREREiIjM0VWd9///////////XdlRDMiIREREQAAAAARERERIiIjNEVWd+////////"
"///HdlRDMiIhERERAAAAERERERIiIzNEVmd///////////93dlRDMiIhERERAAAAERERESIiIzRFVnd///////////93dlRDMyIhERERAAABEREREiIiMzRF"
"Znd///////////53dlVEMyIiERERAAABERERIiIzM0RVZ3ff//////////13dlVEMzIiIREREAARERESIiMzNEVWZ3fv//////////13dmVEQzIiIREREAER"
"EREiIjM0RFVmd3fv//////////d3d2VUQzMiIhEREAERERIiIzNERVZnd3f///////////d3d2ZVRDMyIhERERERESIjM0RFVWZ3d3f//////////+d3d3Zl"
"VEMzIhERERERESIjNEVVZnd3d3z//////////+d3d3dmVUQzIiEREREREiIzRFVmZ3d3d33//////////9d3d3d2ZVQzMiEREREREiM0Tv//////////////"
"//////////////5DMiERERERIiM0T/////////////////////////////5DMiIRERERIjNEX/////////////////////////////1EMiIRERERIjNFX///"
"//////////////////////////1EMyIRERERIjNF3/////////////////////////////VEMyIRERESIjRF7////////////////////////////+VEMyIR"
"ERESIjRF7////////////////////////////+VEMyIRERESIjRF/////////////////////////////+VEMiIRERESIjRF////////////////////////"
"/////9VDMiIRERESIjNM/////////////////////////////8VDMiERERERIjNN/////////////////////////////1RDMiERERERIjNO////////////"
"/////////////////kQzIiERERERIiM//////////////////////////////kQzIhERERERIiMzRFVWZmd3d3d3d3d3d3d3d3d3ZmZVREMyIhEREREREiIz"
"NERVVWZmZmZmZmZmZmZmZmZmZVVURDMiIREREBERESIjM0RERVVVVVVVZmZmZmZVVVVVVUREMzIiIREREBERESIiMzM0RERERFVVVVVVVVVVVUREREQzMyIi"
"ERERAAERERIiIjMzMzRERERERERERERERERDMzMzIiIhERERAAERERESIiIiMzMzMzMzMzMzMzMzMzMzMzIiIiEREREQAAAREREREiIiIiIjMzMzMzMzMzMz"
"MzMiIiIiIREREREQAAABERERERIiIiIiIiIiIiIiIiIiIiIiIiIhEREREREAAAAAEREREREREiIiIiIiIiIiIiIiIiIiIhEREREREQAAAAAAAAAAAAAAAAAB"
"ERERERERERESIiIiIiIREREREREREQAAAAAAAAAAAAAAAAAAAREREREREREiIiIiIiIiIiIiIREREREREQAAAAAAAAAAAAAAAAARERERERESIiIiIiIiIiIi"
"IiIiIhEREREREAAAAAAAAAAAAAAAARERERERIiIiIiMzMzMzMzMzIiIiIhERERERAAAAAAAAAAAAAAERERERESIiIiMzMzMzNEQzMzMzMyIiIhEREREQAAAA"
"AAAAAAAAEREREREiIiIzMzNEREREREREREMzMzIiIhEREREAAAAAAAAAAAEREREREiIiMzNERERVVVVVVVVVRERDMzIiIRERERAAAAAAAAAAERERERIiIjMz"
"RERVVVZmZmZmZlVVVERDMyIiEREREQAAAAAAAAARERERIiIzM0RFVVZmZmd3d3d2ZmZVVURDMyIhEREREAAAAAAAARERERIiIzNERVVmZnfd7v///+7td2Zl"
"VEQzMiIREREQAAAAAAARERERIiIzNEVVZmfv////////////7WZlVEMzIiEREREAAAAAABERERIiIzREVWZ+/////////////////mZVRDMyIhEREQAAAAAB"
"ERERIiMzRFVmbv///////////////////WVUQzIiERERAAAAAAERERIiIzRFVm7/////////////////////5mVEMyIhEREAAAAAEREREiIzRFVm////////"
"///////////////+ZVRDMiEREQAAAAAREREiIzRFVt/////////////////////////mVUMyIhERAAAAAREREiIzNFVt//////////////////////////1l"
"RDMiEREAAAABERESIjNEVt///////////////////////////2VUMyIhEQAAAAERERIjNEVt////////////////////////////1lQzIiERAAAAERERIiM0"
"VW/////////////////////////////2VEMyIREAAAAREREiM0RW//////////////////////////////ZVQzIhEQAAABERESIzRV3////////////Xd3d+"
"/////////////WVDMiERAAAAERESIjNFb///////////13d3d3d////////////9ZUMyIhEAAAARERIiNEXf//////////d3d3d3d33///////////1lQzIi"
"EQAAABEREiM0Re//////////d3d3d3d3d+///////////WVDMiIRAAAAERESIzRF//////////13d3d3d3d37//////////9ZUMyIhEAAAARERIjNE3/////"
"////53d3d3d3d3fv//////////xlQzIiEQAAABEREiM0Tv/////////Xd3d3d3d3d+//////////52VDMiERAAAAERESIjRO/////////8d3d3d3d3d3////"
"///////mVUMyIREAAAARERIiM8//////////d3dmZ3d3d37//////////9ZUQzIhEQAAABEREiIz3/////////53ZmZnd3d3f///////////dlRDIiERAAAA"
"ERERIjM0RVZmZ3d3dmZmZnd3d3f///////////9mVDMiIREAAAAREREiIzNEVVZmZmZmZmZnd3d3f////////////WVEMyIREQAAABERESIiMzREVVVWZmZm"
"Zmd3d33////////////mZUQyIhERAAAAAREREiIzM0REVVVVZmZmd3d33////////////3ZVQzIiEREAAAABERESIiMzNEREVVVWZmd3d37////////////9"
"ZlRDMiEREQAAAAEREREiIjMzRERFVVZmd3d3/////////////+dlVDMiIRERAAAAARERESIiIzM0REVVZmd3d9/////////////+dlVEMyIhEREAAAARERER"
"IiIjMzRERVVmd3d+/////////////+dmVEMyIhEREQAAABEREREiIiMzNERVVmd3ff/////////////+dmVUMyIiERERAAAAERERESIiIzM0RVVmd3fv////"
"/////////+d2VUQzIiEREREAAAARERERIiIjM0RFVmd3fv/////////////+d2VUQzIiIREREAAAARERERIiIjM0RFVmd33//////////////9d2VUQzMiIR"
"EREQAAABEREREiIjM0RFVmd37//////////////nd2ZURDMiIhERERAAABEREREiIjM0RFVnd8///////////////nd2ZVRDMiIhERERAAAAEREREiIjM0RV"
"Znd9///////////////Hd2ZVRDMyIiEREREAAAERERESIiM0RVZnd+//////////////53d2ZVREMzIiEREREQAAARERESIiM0RVZnff//////////////d3"
"d3ZlVEMzIiIREREQAAARERESIiM0RVZnfv/////////////9d3d3ZmVUQzMiIhERERAAABERERIiMzRVZnf//////////////nd3d3dmVVRDMyIiEREREAAB"
"ERERIiMzRFZn3//////////////Xd3d3d2ZlVEQzIiIREREQAAERERIiIzRFZn3/////////////53d3d3d3dmVVRDMyIhERERAAEREREiIzRFZn7///////"
"/////+d3d3d3d3d3ZlVEQzIiEREREAAREREiIzRFVn7////////////9d3d3d3d3d3d2ZVRDMyIhEREQABERESIjNFVn7////////////nd3d3d3d3d3d3dm"
"VUQzIiERERAAERESIjNEVm7/////////////////////////////5DMiIREREAARERIiM0VW3//////////////////////////////UQyIhEREQABEREiM0"
"RW3//////////////////////////////9RDMiERERAAEREiIzRVb///////////////////////////////VEMyIREREAARESIjRFbv////////////////"
"//////////////9VQzIhEREQABERIjNFVv///////////////////////////////lVDMiERERAAEREiM0Ve///////////////////////////////+VUMy"
"IRERAAARESIzRW7///////////////////////////////1VQzIhEREAABESIjRF3////////////////////////////////VRDMiEREQAAERIiNEXv////"
"///////////////////////////2VEMiIRERAAAREiIzRf///////////////////////////////+VUMyIhEREAABERIjNF////////////////////////"
"////////5UQzIhEREQAAEREiM03////////////////////////////////VQzIiERERAAARESIzTf///////////////////////////////9RDMiERERAA"
"ABERIiM+////////////////////////////////RDMiIREREAAAERESIzNEVWZmd3d3d3d3d3d3d3d3d3d3d3ZmVVRDMyIhEREQAAARERIiMzREVVZmZmZm"
"Z3d3d3d3d3ZmZmZmZlVURDMyIhEREQAAABERESIjM0REVVVVVVZmZmZmZmZmZmVVVVVVVERDMyIhERERAAAAEREREiIzMzRERERVVVVVVVVVVVVVVVVERERE"
"MzMiIhERERAAAAABERERIiIzMzM0REREREREREREREREREREMzMzIiIhEREREAAAAAERERESIiIjMzMzMzMzMzMzMzMzMzMzMzMzMyIiIhEREREAAAAAABER"
"ERESIiIiIiMzMzMzMzMzMzMzMzMzMiIiIiIREREREAAAAAAAARERERESIiIiIiIiIiIiIiIiIiIiIiIiIiIiEREREREAAAAAAAAAEREREREREiIiIiIiIiIi"
"IiIiIiIiIiIiEREREREREAAAAAAAAAAAAAAAAAAAABERERERERERESIiIiIiEREREREREREQAAAAAAAAAAAAAAAAAAERERERERESIiIiIiIiIiIiIhERERER"
"ERAAAAAAAAAAAAAAAAEREREREREiIiIiIiIiIiIiIiIiIRERERERAAAAAAAAAAAAAAARERERERIiIiIiMzMzMzMzMzIiIiIhERERERAAAAAAAAAAAAABERER"
"ERIiIiIzMzMzMzMzMzMzMzIiIiERERERAAAAAAAAAAAAEREREREiIiIzMzRERERERERERDMzMiIiERERERAAAAAAAAAAAREREREiIiMzM0RERVVVVVVVVURE"
"QzMyIiIRERERAAAAAAAAABERERESIiMzNERFVVVWZmZmZmVVVUREMzIiIRERERAAAAAAAAARERERIiIzNERFVWZmZnd3d3dmZmVVREMzIiIREREQAAAAAAAB"
"EREREiIzM0RVVmZnfd7v///+7ddmZlVEQzIiIREREQAAAAAAERERESIjM0RFVmZ+/////////////nZlVUQzIiIREREAAAAAABERERIiMzRFVmbf////////"
"////////1mVUQzIiERERAAAAAAEREREiIzNEVWbv///////////////////WVUQzIiEREQAAAAABERESIjM0RVbf/////////////////////mVUQzIiEREA"
"AAAAAREREiIzRFZv///////////////////////mVUMyIhERAAAAABERESIjNEVt/////////////////////////mVEMyIREQAAAAAREREiM0RV3///////"
"///////////////////VVDMiIREAAAAAERESIjNFXf//////////////////////////5lRDMiERAAAAABEREiM0Rd////////////////////////////1V"
"QzIhEQAAAAERERIjNEX////////////////////////////+ZUMyIhEAAAABEREiIzRe/////////////////////////////2VEMiIRAAAAARERIiNEX///"
"////////7Hd3ff////////////9lRDMiEQAAAAERESIjRN//////////53d3d3d+////////////ZVQzIhEAAAABEREiI0Tv/////////nd3d3d3d+//////"
"/////2VUMyIRAAAAARERIiM0/////////+d3d3d3d3ff//////////9lVDMiEQAAAAERESIjPf////////93d3d3d3d3f///////////ZVQzIhEAAAABEREi"
"Iz7////////+d3d3d3d3d3///////////mVEMyIRAAAAAREREiM93d3d3d3d13ZmZ3d3d3d///////////1lRDIiEQAAAAARERIiMzRFVVZmZmZmZmZ3d3d3"
"3//////////3ZUQyIhEAAAAAERERIiMzREVVVVZmZmZmd3d3d+//////////5mVDMiIRAAAAABERESIiMzRERFVVVVZmZ3d3d3f//////////3ZVQzIiEQAA"
"AAARERESIiMzNERERVVVZmd3d3d+//////////52VEMyIREAAAAAERERESIiIzMzRERVVWZ3d3d37//////////nZlQzIiERAAAAABERERESIiIjMzNERVZm"
"d3d33///////////d2VEMyIhEQAAAAABERERESIiIjMzREVWb///////////////93ZlRDMiIREAAAAAARERERESIiIjMzRFVt///////////////3d2VUMz"
"IiERAAAAAAERERERESIiIjM0RVbv/////////////+d3dlRDMiIREQAAAAABERERERESIiIzNEVW7/////////////13d2ZUQzIiEREAAAAAEREREREREiIi"
"IzRFVu////////////13d3dmVEMyIhERAAAAABERERERERIiIiM0RWb////////////+x3d3ZVRDMiIREQAAAAERERERERESIiIzNEVt//////////////53"
"d2VUMzIiEREAAAARERERERERIiIiMzRFXf///////////////XdlVDMyIhERAAABERERERERIiIiIjM0RV7///////////////93ZlRDMiIREQAAERERERER"
"IiIiIiMzNEVe////////////////53ZUQzIiEREAARERERERIiIiIiIzMzRFX/////////////////12VEMyIhERABERERERIiIiIiIzMzM0RVVnd3d97///"
"////////dlRDMiIREQARERERIiIiMzMzMzMzRERVZnd3d37//////////9ZVQzIhEREBERERIiIjMzMzMzMzREREVWZnd3d3///////////mVUMyIhERARER"
"EiIjMzNERERERERERFVWZ3d3d///////////5lVDMiERERERESIiMzREREVVVEREREVVVmZ3d3fv/////////+ZVQzIhEREREREiIzNERVVVVVVVVVVVVVZm"
"d3d37//////////mVUMyIRERERESIjM0RVVmZmZmZlVVVVVWZnd3d+//////////5lVDMiEREREREiMzT/////////9mZlVVZmd3d3f//////////+ZVQzIh"
"ERERESIjNN/////////+dmZmZmZ3d3d+///////////mVEMiIREREREiIzTf/////////nd3ZmZnd3d3f///////////1lRDIiERERERIjNE3/////////93"
"d3d3d3d3d+///////////2ZUMyIhERERESIzRe//////////13d3d3d3d37///////////5lVDMiEREREREiM0Xv//////////13d3d3d3fv///////////9"
"ZUQzIhERERERIjNF3////////////Xd3d33v////////////9lVDMiIRERERESIzRd///////////////////////////////8ZUQzIiEREQEREiM0Vf////"
"//////////////////////////5lVDMiIREREBERIjNEXv/////////////////////////////mZUQzIiERERARESIjRF3/////////////////////////"
"////dlVDMiIREREAEREiIzRV7///////////////////////////92VUQzIiERERABEREiM0RW7//////////////////////////3ZVRDMiIREREAARERIi"
"M0RW/////////////////////////+ZlVEMyIhERERAAERERIjM0VW7///////////////////////1mVUQzIiIREREAABERESIjNEVW3///////////////"
"//////1mZVRDMiIhERERAAARERESIjNEVWbf//////////////////12ZVREMyIiEREREAAAARERESIjNERVZm3v/////////////tdmZVVEMzIiIRERERAA"
"AAEREREiIjM0RVVmZn3e7////+7t13ZmZVVEQzMiIhEREREAAAAAEREREiIjM0RFVVZmZmd3d3d3ZmZmVVVEQzMyIiEREREQAAAAAAEREREiIjMzRERVVVZm"
"ZmZmZmVVVVREQzMyIiIRERERAAAAAAABEREREiIiMzNERERVVVVVVVVVVEREMzMyIiIREREREAAAAAAAABERERESIiIzMzNERERERERERERDMzMyIiIhERER"
"EQAAAAAAAAABERERESIiIiMzMzMzNEREMzMzMzMiIiIhERERERAAAAAAAAAAABERERERIiIiIjMzMzMzMzMzMyIiIiIhEREREREAAAAAAAAAAAABERERERES"
"IiIiIiIiIiIiIiIiIiIREREREREQAAAAAAAAAAAAABERERERERIiIiIiIiIiIiIiIiEREREREREQAAAAAAAAAAAAAAAAERERERERERESIiIiIiIiERERERER"
"ERERAAAAAAAAAAAAAAAAAAAAAAAAAAAAERERERERERERERERERERERAAAAAAAAAAAAAAAAAAAAAAAAAAARERERERERIiIiIhEREREREREQAAAAAAAAAAAAAA"
"AAAAAAAAAAERERERESIiIiIiIiIiERERERERAAAAAAAAAAAAAAAAAAAAAAABERERERIiIiIiIiIiIiIhEREREREAAAAAAAAAAAAAAAAAAAAAAREREREiIiIz"
"MzMzMzMiIiIRERERERAAAAAAAAAAAAAAAAAAAAERERERIiIzMzREREQzMzMiIiEREREREAAAAAAAAAAAAAAAAAABEREREiIjMzREREREREQzMyIiIREREREQ"
"AAAAAAAAAAAAAAAAABERERIiIzNERVVVVVVVREMzMiIiERERERAAAAAAAAAAAAAAAAARERESIiMzRFVWZmZmZlVURDMyIiIhEREREAAAAAAAAAAAAAAAERER"
"ESIjNERe//////////9EMzMiIiEREREQAAAAAAAAAAAAABEREREiIzNFVv//////////5URDMzIiIhERERAAAAAAAAAAAAABERERIiMzRFbv//////////1V"
"REMzMiIiERERAAAAAAAAAAAAARERESIiM0RWb//////////+ZlVURDMzIiIREREAAAAAAAAAAAERERESIjNEVm7//////////8ZmVVVEQzMiIREREAAAAAAA"
"AAAAEREREiIzRFVn///////////ndmZlVVRDMyIhEREAAAAAAAAAABERERIiIzRVZ+//////////93d3bf///+MyIhEREQAAAAAAAAARERERIiM0RWbP////"
"//////53d3fv///+QzIiEREQAAAAAAAAARERESIjM0VWfv/////////+d3d33////9QzIiEREQAAAAAAAAEREREiIjNEVn3//////////8d3d37////9RDMi"
"ERERAAAAAAAAEREREiIzRFZn///////////nd3d+/////1VDMiERERAAAAAAABERERIiMzRVZ9//////////93d3d//////1VDMiERERAAAAAAARERERIiM0"
"RWZ///////////x3d3f//////mVDMiIRERAAAAAAARERESIjNEVWfv/////////9d3d33//////WVDMiIRERAAAAAAEREREiIjNFVn3//////////nd3d9//"
"/////WVEMiIREREAAAABEREREiIzRFZn7/////////93d3d+//////92VEMiIREREAAAABERERIiM0RVZ9//////////d3d3fv//////92VEMiIREREAAAAR"
"ERESIiM0VWd//////////9d3d33///////52VEMiIREREAAAERERESIjNEVmfv/////////nd3d3////////52VEMiIREREAAAEREREiIzRFVn3/////////"
"53d3d+////////12VDMiIREREAABERERIiIzRVZ37/////////d3d3fv////////12VDMiIREREAARERESIiM0RWZ+/////////3d3d33/////////dmVDMi"
"IREREAARERESIjNEVWff/////////Xd3d9//////////dmVDMiIREREAEREREiIzNFVnf/////////13d3d//////////+dmVDMiIREREAEREREiIzRFZn7/"
"///////9d3d3ff/////////+dlVDMiIREREBERERIiM0RVZ9/////////nd3d3fv/////////9dlVDMiIREREBERESIiM0VWd/////////53d3d3fv//////"
"///3dlVDMiIRERERERESIjNEVmf////////+d3d3d3f//////////3dlVDMiIhEREREREiIzRFZn7////////nd3d3d3z//////////ndlVDMyIhEREREREi"
"IzRVZ9////////53d3d3d33//////////ndmVEMyIhERERERIiM0RWZ////////+d3d3d3d37//////////Xd2VEMyIhERERERIiM0VWfv///////3d3d3d3"
"d37//////////Xd2VUMyIhERERESIjNEVn7///////93d3d3d3d3//////////93d2VUQzIiERERESIjNFVt///////+d3d3d3d3d3//////////93d2ZUQz"
"IiERERESIzRFbf///////nd3d3d3d3d9//////////53d3ZVQzIiEREREiIzRVb///////53d3d3d3d3d9//////////53d3ZlRDMiEREREiIzRW3///////"
"///////////////////////////+RDMiEREREiI0RW7//////////////////////////////////9VDMiEREREiM0RW////////////////////////////"
"///////9VDMiEREREiM0VW///////////////////////////////////2VDMiIREREiM0VW///////////////////////////////////mVDMiIREREiM0"
"Vt///////////////////////////////////mVDMiEREREiM0Vu///////////////////////////////////mVDMiEREREiM0Ve//////////////////"
"/////////////////WVDMiEREREiM0Vf///////////////////////////////////FVDMiEREREiM0Rf//////////////////////////////////9lRD"
"IiEREREiIzTf//////////////////////////////////5VQzIiERERESIzTf//////////////////////////////////5UQzIhERERESIjPv////////"
"//////////////////////////1UMyIhERERESIjNERVZmZ3d3d3d3d3d3d3//////////93d3dlVEMyIhERERERIiM0RFVVZmZmd3d3d3d3d3//////////"
"93d3ZVRDMiIRERERERIiIzNERFVVVWZmZmd3d3d9//////////53d2VUQzMiIREREREREiIjMzREREVVVVVWZmd3d9//////////13dlVEMzIiERERARERES"
"IiIzMzNEREREVVVWZ3d+//////////13ZVRDMyIhERERABEREREiIiIzMzMzREREVVZnd+//////////d2ZUQzMiIhEREQAAERERESIiIiIjMzMzNERFVmd/"
"//////////dmVEMzIiIREREQAAERERERESIiIiIiMzMzREVWbf/////////+dlVEMyIiEREREAAAABERERERESIiIiIiIzM0RVbf/////////+ZVRDMiIhER"
"ERAAAAAAERERERERESIiIiIjMzRFXv/////////9VUQzIiIRERERAAAAAAABERERERERERIiIiIzNEXv/////////9VEMyIiEREREQAAAAAAAAERERERERER"
"ESIiIjM0T//////////1RDMyIhEREREAAAAAAAAAAAEREREREREREiIiMzRFVWZmZmZlVUQzMiIRERERAAAAAAAAAAAAABERERERERESIiIzNERVVVVVVVRE"
"MzIiIRERERAAAAAAAAAAAAAAAREREREREREiIjMzREREREREQzMyIiEREREQAAAAAAAAAAAAAAAAARERERERESIiIzMzREREQzMzIiIREREREAAAAAAAAAAA"
"AAAAAAAAEREREREREiIiMzMzMzMzIiIiERERERAAAAAAAAAAAAAAAAAAAAAAERERERESIiIiIiIiIiIiIREREREQAAAAAAAAAAAAAAAAAAAAAAABERERERES"
"IiIiIiIiIhERERERAAAAAAAAAAAAAAAAAAAAAAAAAAARERERERERIiIiIREREREREQAAAAAAAAAAAAAAAAAAAAAAAAAAAAEREREREREREREREREREREAAAAA"
"AAAAAAAAAAAAAAAAEREREREREiIiIiIiIiIiIiIiIiIhERERERERAAAAAAAAAAAAARERERESIiIiIiIiIiIiIiIiIiIiIiIRERERERAAAAAAAAAAABERERES"
"IiIiIjMzMzMzMzMzMzMiIiIiIiERERERAAAAAAAAAAERERESIiIjMzMzMzMzMzMzMzMzMzMzIiIiEREREQAAAAAAAAARERESIiIzMzM0REREREREREREREQz"
"MzMyIiIREREQAAAAAAABERERIiIzM0RERERVVVVVVVVVVURERERDMzIiIREREAAAAAAAEREREiIjM0REVVVVVVVWZmZVVVVVVVVUREMzIiIREREAAAAAABER"
"ESIiM0RFVVZmZmZmZmZmZmZmZmZmVVVEQzIiERERAAAAAAEREREiIzRFVWZnd3d3d3d3d3d3d3d3ZmZlVEQzIiEREQAAAAABERESIjNEXv//////////////"
"///////////+MzIhEREAAAAAERERIiM0RV///////////////////////////kMyIhERAAAAABERESIjNFXf//////////////////////////5DMiIREQAA"
"AAERERIiM0RW7//////////////////////////9RDMiEREAAAABERESIjNFVv//////////////////////////9VQzIhERAAAAEREREiM0RWf/////////"
"//////////////////VUMyIREQAAABERESIjNFVt///////////////////////////mVDMiEREAAAAREREiI0RWff//////////////////////////5lQz"
"IhERAAABERESIjNEVn7//////////////////////////9VUMyIREQAAAREREiIzRVZ////////////////////////////VVDMiEREAAAERERIiNEVn3///"
"////////////////////////ZUQzIhERAAAREREiIzRFZ+///////////////////////////mVEMyIREQAAERERIiM0VWfv////////////////////////"
"//5VQzIiEREAABERESIjNFZ3///////////////////////////+VEMyIhERAAAREREiM0RWff///////////////////////////VRDMiIREQABERESIjNE"
"Vn7////////3d3d3d3d3d3d3d3d3dmVUMyIhEREAAREREiIzRWZ/////////53d3d3d3d3d3d3d3d2ZVRDMiIRERAAERERIiM0Vnf////////+d3d3d3d3d3"
"d3d3d3dmVUQzIiEREQABERESIzRFZ9/////////Xd3d3d3d3d3d3d3d2ZVRDMyIhEREAAREREiM0RWfv////////d3d3d3d3d3d3d3d3dmVUQzIiIRERABER"
"ESIjNFVn/////////3d3d33u///+7Xd3d3ZlVEMyIiEREQAREREiIzRWZ/////////53d+//////////53d3ZVRDMiIhEREAERERIiM0Vn3////////9ff//"
"///////////nd2ZUQzMiIRERABERESIjRFZ+////////7v///////////////9dmVUMzIiEREQAREREiM0RWfv/////////////////////////9dlVEMyIh"
"EREBERESIjNFVn///////////////////////////9dlRDMiIRERAREREiIzRVbf///////////////////////////3ZVQzIiEREQERERIiM0VW7///////"
"/////////////////////mZUQzIhEREBERESIjNFVv/////////////////////////////WVEMyIhERAREREiIzRVz/////////////////////////////"
"5lVDMiIRERERERIiM0Vd//////////////////////////////dlQzIiERERERESIjNFXv//////////7Xd3d9/////////////3ZUQyIhERERERIiIzRV//"
"////////93d3d3d37////////////WVEMiIRERERESIjM0Ve7////////nd3d3d3d37///////////5lVDMiEREREREiIzRFVmd3ze7//+d3d3d3d3d3////"
"///////+ZVQzIhERERERIiM0RVZnd3d3d3d3d3d3d3d3d+///////////mVUMyIREREREiIzNEVWZ3d3d3d3d3d3d3d3d3ff//////////5lVDMiERERERIi"
"M0RVZmd3d3d3d3d2ZmZnd3d33//////////+ZVQzIhEREREiIjNEVWZ3d3d3d3d3ZmZmZnd3d9///////////WVEMyIRERERIiM0RVZnd3d3d3d3d2ZmZmZ3"
"d3ff//////////1lRDIiERERESIjNEzd3d3d3d3Xd3ZmZmZmd3d37//////////3ZUQyIhEREREiM0Rf/////////nd3ZmZmZnd3d+//////////92VDMiIR"
"ERERIjNE3/////////53d3ZmZmd3d3z//////////+ZlQzIiERERESIzRd//////////d3d3dmd3d3d+///////////WVEMyIRERERIiM0Xf/////////3d3"
"d3d3d3d33//////////+dlRDIiERERESIjNF3//////////Xd3d3d3d3d////////////mVUMyIhEREREiIzRd///////////Hd3d3d3d3////////////dl"
"RDMiERERERIiM0Vv///////////Xd3d3d33////////////WZUMyIhERERERIjNFb////////////td3d33/////////////dlRDMiIRERERESIzRV7/////"
"/////////////////////////GVUMyIhEREQEREiM0Rd/////////////////////////////9ZlRDMiIREREBERIiNEVu//////////////////////////"
"//5mVEMyIhERERARESIjNFXf///////////////////////////nZVQzMiIREREAERESIzRFbv/////////////////////////+ZlVEMyIhERERABEREiIz"
"RVb/////////////////////////1mVUQzIiEREREAAREREiM0RVb///////////////////////92ZVRDMiIhEREQAAERERIiM0RVbv////////////////"
"/////nZlVEMyIiEREREAABERERIiM0RVbf///////////////////nZlVEQzIiIREREQAAABERESIiM0RVZt/////////////////nZmVUQzMiIhERERAAAA"
"ARERESIjM0RVVmff////////////7XZmVURDMyIiEREREQAAAAARERESIiM0RFVWZnze7v///+7dd2ZlVURDMzIiIRERERAAAAAAERERESIiMzREVVZmZmd3"
"d3dmZmZVVUREMzIiIhEREREAAAAAAAERERESIiMzNERFVVVmZmZmZlVVVURDMzIiIhEREREQAAAAAAAAERERESIiIzM0RERFVVVVVVVURERDMzIiIiERERER"
"AAAAAAAAAAERERESIiIjMzNEREREREREREMzMzIiIiEREREREAAAAAAAAAAAERERERIiIiMzMzMzMzMzMzMzMyIiIiEREREREQAAAAAAAAAAAAEREREREiIi"
"IiMzMzMzMzMzIiIiIiERERERERAAAAAAAAAAAAAAERERERERIiIiIiIiIiIiIiIiIiERERERERAAAAAAAAAAAAAAAAEREREREREiIiIiIiIiIiIiIhERERER"
"EREAAAAAAAAAAAAAAAAAARERERERERERIiIiIiIREREREREREREAAAAAAAAAAAAAAAAAAAAAAAAAEREREREREREREiIiIiEREREREREREQAAAAAAAAAAAAAA"
"AAAAERERERERERIiIiIiIiIiIiIREREREREQAAAAAAAAAAAAAAAAAREREREREiIiIiIiIiIiIiIiIiERERERERAAAAAAAAAAAAAAABEREREREiIiIiMzMzMz"
"MzMyIiIiIhERERERAAAAAAAAAAAAABEREREREiIiIjMzMzMzMzMzMzMyIiIhERERERAAAAAAAAAAAAEREREREiIiIzMzNEREREREREQzMzMiIiERERERAAAA"
"AAAAAAARERERESIiIzM0RERFVVVVVVVEREQzMyIiERERERAAAAAAAAABERERESIiIzM0REVVVWZmZmZlVVVERDMyIiIREREQAAAAAAAAERERERIiIzM0RFVV"
"ZmZmd3d3ZmZlVVREMzIiIREREQAAAAAAAREREREiIjM0RFVWZnfd7v///+7ddmZVVEMzIiEREREAAAAAAAEREREiIjM0RFVmZ97////////////mZlVEMzIi"
"EREREAAAAAARERESIiMzRFVWZ+///////////////+1lVEMzIiERERAAAAABERERIiIzNEVWZ+//////////////////5mVEMzIiERERAAAAERERESIjM0RV"
"Zu/////////////////////WVEMyIhEREQAAABERERIiMzRFZn///////////////////////WVEMyIhEREAAAEREREiIzNEVmz/////////////////////"
"///VVEMiIRERAAARERESIiM0RWbf////////////////////////5lRDMiIREQAAEREREiIzRFZt//////////////////////////5lQzIiEREAARERESIj"
"NEVWf///////////////////////////ZUQzIhERAAERERIiIzRVZ////////////////////////////9VUMyIREQARERESIjNEVn7/////////////////"
"///////////mVDMiEREAERERIiM0RWbf///////////+x3d33///////////5lRDIiERARERESIjNFVn7//////////+d3d3d3fv//////////1UQyIhEQER"
"ERIiM0RWff//////////53d3d3d3fv/////////9VEMiIREBERESIjNFVn7//////////nd3d3d3d33//////////VRDIiEREREREiM0RWff//////////d3"
"d3d3d3d3//////////1UQyIhERERESIjNFVn///////////Xd3d3d3d3d//////////9VDMiIREREREiI0RWff/////////+d3d3d3d3d3fv/////////VQz"
"IiERERESIjNEVn7//////////Xd3d3d3d3d37/////////VEMyIhEREREiIzRWZ///////////d3d3d3d3d3d93d3d3d3d3VRDMiIRERERIjNEVn3///////"
"///3d3d3d3d3d3d3d3d3d3dmVEMyIhEREREiIzRVZ+//////////53d3d3d3d3d3d3d3d3d2ZVRDMiIRERERIiM0Vmf//////////9d3d3fe7///7td3d3d3"
"dmVUQzIiERERESIjRFZ9///////////Xd37//////////nd3d3ZlRDMyIhERERIiM0RWfv//////////d37////////////+d3d2VUQzIiIRERESIjNFVn7/"
"/////////37///////////////53dmVEMyIhEREREiIzRWd/////////////////////////////53ZlRDMiIRERERIiNEVn3///////////////////////"
"//////53ZUQzIiERERESIzRFZ+//////////////////////////////52VUMyIhERERIiM0VWfv//////////////////////////////1lVDMiIRERESIj"
"NFZn7//////////////////////////////+dlRDMiEREREiIzRWd////////////////////////////////3ZUQzIhERERIiNEVn3//////////////Xd3"
"d9/////////////mVUMyIhERESIjRFZ9/////////////Xd3d3d37///////////5lVDMiIREREiM0RWfv///////////9d3d3d3d37//////////+ZlQzIi"
"ERERIjNEVn7///////////13d3d3d3d3///////////3ZUMyIhERESIzRVZ+///////////nd3d3d3d3d+//////////92VDMiIREREiM0VWf///////////"
"d3d3d3d3d3fv//////////dlQzIiERERIjNFVn///////////nd3d3d3d3d37//////////3ZUMyIRERESIzRVZ///////////13d3d3d3d3d+//////////"
"5mVDMiEREREiM0VWz//////////9d3d3d3d3d3fv/////////+ZVQzIhERERIjNFVs//////////93d3d3d3d3d37//////////WVUMyIRERESIzRFZ/////"
"//////d3d3d3d3d3d///////////dlRDIiEREREiM0RWf//////////3d3d3d3d3d37//////////3ZUMyIhERERIiNEVn///////////Hd3d3d3d3d/////"
"//////5lVDMiIRERESIjRFZ+//////////13d3d3d3d37//////////9ZUQzIhEREREiIzRWbv//////////d3d3d3d3fv//////////5mVDMiIREREREiM0"
"VW7///////////d3d3d3d+///////////8ZVQzIiERERERIjNEVt////////////53d3fe////////////5mVEMiIREREBESIjNFZ///////////////////"
"///////////3ZVQzIiERERAREiIzRVbv////////////////////////////1lVEMiIREREQEREiM0RW3////////////////////////////mZUQzIiERER"
"ABERIiM0VW7//////////////////////////+ZlRDMiIREREQARESIjNEVm//////////////////////////5mVUMzIiERERAAERESIjNEVt//////////"
"///////////////mZVRDMiIREREQABEREiIzRFVt///////////////////////9ZlVEMyIhERERAAAREREiIzRFVt/////////////////////+dmVUQzIi"
"IREREQAAEREREiIzRFVm7//////////////////+dmVURDMiIhERERAAAAERERIiIzRFVm3////////////////+dmZVRDMyIiEREREAAAABERERIiIzRFVW"
"bO/////////////sdmZVREMzIiIRERERAAAAABERERIiIzREVVZmfd7v///+7td3ZmVVREMzMiIhEREREAAAAAABERERIiIzNERVVmZmZ3d3d3ZmZlVVREQz"
"MiIiEREREQAAAAAAARERERIiIzM0RFVVVmZmZmZmVVVUREMzMiIiERERERAAAAAAAAARERERIiIjMzRERFVVVVVVVVREREMzMiIiIREREREAAAAAAAAAARER"
"EREiIiMzM0REREREREREQzMzMiIiIREREREQAAAAAAAAAAAREREREiIiIzMzMzMzMzMzMzMzIiIiIRERERERAAAAAAAAAAAAARERERESIiIiIzMzMzMzMzMi"
"IiIiIREREREREAAAAAAAAAAAAAARERERERIiIiIiIiIiIiIiIiIiIREREREREAAAAAAAAAAAAAAAARERERERERIiIiIiIiIiIiIhEREREREREQAAAAAAAAAA"
"AAAAAAABEREREREREREiIiIiIhEREREREREREQAAAAAAAAAAAAAAABERERERERIiIiIiIiIiIiIiIiIiIiIiIiEREREREREAAAAAARERERESIiIiIiIiIiIi"
"IiIiIiIiIiIiIiIiEREREREQAAAAERERERIiIiIiIzMzMzMzMzMzMzMzMzMyIiIiIiERERERAAABEREREiIiIzMzMzMzMzMzMzMzMzMzMzMzMzMiIiIhERER"
"AAARERERIiIzMzM0REREREREREREREREREREQzMzMiIiEREREAARERESIiMzNEREREVVVVVVVVVVVVVVVVRERERDMzIiIREREAEREREiIzNERFVVVVVVVmZm"
"ZmZmZmZmVVVVVVRERDMyIhEREQERERIiMzREVVZmZmZmZmZ3d3d3d3ZmZmZmZlVVREMzIiERERERESIjM0RVVmZnd3d3d3d3d3d3d3d3d3d3dmZlVUQzIiER"
"ERERESIjNN/////////////////////////////////TMiIREREREiIzRN/////////////////////////////////DMiIREREREiI0Re//////////////"
"//////////////////9EMyIREREREiM0Re////////////////////////////////9EMyIRERERIiM0Vf////////////////////////////////5UMyIR"
"ERERIiNEVv////////////////////////////////1UMyIRERERIiNEXf////////////////////////////////1UMyIRERERIjNEXf//////////////"
"//////////////////VEMyIRERERIjNFXv///////////////////////////////9VEMiIRERERIjNFX////////////////////////////////WVDMiIR"
"ERERIjNEX///////////////////////////////5lVDMiERERERIiNE3//////////////////////////////+ZlRDMiERERERIiM03///////////////"
"///////////////nZVQzIiEREREREiM07/////////////////////////////52VUQzIhEREREREiMz7/////////////////////////////dmVEMyIhER"
"EREREiIzRFVWZ3d3d3d3d3d3d3f//////////3dlVDMiIRERERERESIjNERVZmZ3d3d3d3d3d3//////////93ZVRDMiIREREBERESIjM0RFVVZmZ3d3d3d3"
"d///////////12ZUQzIiEREREAERERIiMzNERVVWZmd3d3d3fv/////////9dmVEMyIiERERAAEREREiIjM0REVVVmZ3d3d37//////////XdlVDMyIhERER"
"AAERERESIiMzNERFVWZnd3d+//////////53ZVRDMiIREREQAAARERESIiIzMzREVVZnd3fv/////////+d2VUQzIiIREREQAAABERERIiIjMzNERVZnd33/"
"/////////ndmVEMyIiEREREAAAABEREREiIiMzNERVZ3d9///////////XZlVDMyIhERERAAAAAAERERESIiIzNERVZ3ff//////////13ZVRDMiIRERERAA"
"AAAAERERESIiIzNEVWZ3f//////////+d2VUQzIiIREREQAAAAAAERERERIiIzNEVWd3///////////3dlVEMyIiEREREAAAAAAAARERERIiIzNEVmd+////"
"//////93ZlRDMyIhEREREAAAAAAAARERERIiIzRFVnff//////////13ZVQzMiIRERERAAAAAAAAARERESIiMzRFZnz//////////+d2VUQzIiEREREQAAAA"
"AAAAARERESIiM0RVZ37//////////3dlVEMyIiEREREAAAAAAAAAARERESIjM0VWd+///////////XZlRDMiIhERERAAAAAAAAAAEREREiIjNEVmff//////"
"////53ZVQzMiIRERERAAAAAAAAAAEREREiIzNFVnf//////////+d2VUQzIiIREREQAAAAAAAAAAEREREiIzRFZn7//////////9dmVEMyIiEREREAAAAAAA"
"AAAAERERIiIzRVZ9///////////ndlVDMyIhERERAAAAAAAAAAABERERIiM0RWZ+//////////93ZlRDMiIRERERAAAAAAAAAAABERERIiM0VWfv////////"
"//53ZUQzIiIREREQAAAAAAAAAAABERESIjNEVnf///////////d2VUQzIiEREREAAAAAAAAAAAARERESIjNFVn7//////////+d2VEMyIiEREREAAAAAAAAA"
"AAAREREiIzRFZ9///////////8dlVEMyIhERERAAAAAAAAAAAAAREREiIzRVZ+///////////ndlRDMiIREREQAAAAAAAAAAAAEREREiM0RWff//////////"
"/HZVRDMiIREREQAAAAAAAAAAAAERERIiM0VWf///////////93ZUQzIiEREREAAAAAAAAAAAAAERERIiM0Vm3///////////12ZUQzIiEREREAAAAAAAAAAA"
"AAERERIjNEVn7///////////d2VUMyIhERERAAAAAAAAAAAAAAERESIjNFVt///////////+dmVEMyIhERERAAAAAAAAAAAAABERESIjNFZu///////////9"
"dlVDMiIREREQAAAAAAAAAAAAABERESIjRFZ////////////3dlRDMiIREREQAAAAAAAAAAAAABERESIzRFbf///////////nZlQzIiIREREAAAAAAAAAAAAA"
"ABERESIzRVbv///////////XZUQzIiEREREAAAAAAAAAAAAAABERESIzRVb////////////XZUQzIiERERAAAAAAAAAAAAAAABERESIzRW3///////////92"
"VUMyIhERERAAAAAAAAAAAAAAABERESIzRW7///////////52VEMyIhEREQAAAAAAAAAAAAAAABERESIzRV////////////5lVDMiIREREQAAAAAAAAAAAAAA"
"ABERESIzRd////////////1lRDMiIREREAAAAAAAAAAAAAAAAAERESIzRN////////////1VQzIiEREREAAAAAAAAAAAAAAAAAERESIzRO////////////VU"
"QzIiERERAAAAAAAAAAAAAAAAAAERESIjNO///////////+VEMyIhERERAAAAAAAAAAAAAAAAAAERERIjNP///////////+RDMyIhEREQAAAAAAAAAAAAAAAA"
"AAERERIiM0RFVWZmZmZlVUQzMiIREREQAAAAAAAAAAAAAAAAAAAREREiIzRERVVVVVVVREMzIiEREREAAAAAAAAAAAAAAAAAAAAREREiIjM0RERFVEREQzMy"
"IhERERAAAAAAAAAAAAAAAAAAAAARERESIiMzM0REREQzMzIiIRERERAAAAAAAAAAAAAAAAAAAAABERERIiIjMzMzMzMzMiIiEREREQAAAAAAAAAAAAAAAAAA"
"AAAAEREREiIiIiIjMiIiIiIhEREREAAAAAAAAAAAAAAAAAAAAAAAEREREREiIiIiIiIiIiERERERAAAAAAAAAAAAAAAAAAAAAAAAAREREREREiIiIiIhERER"
"EREQAAAAAAAAAAAAAAAAAAAAAAAAABEREREREREREREREREREREAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABERERERERERESIiIiIRERERERERERAAAA"
"AAAAAAAAAAAAAAABEREREREREiIiIiIiIiIiIhEREREREREAAAAAAAAAAAAAAAAAERERERERIiIiIiIiIiIiIiIiIhEREREREAAAAAAAAAAAAAAAERERERER"
"IiIiIjMzMzMzMzMiIiIiEREREREAAAAAAAAAAAAAARERERERIiIiMzMzMzMzMzMzMzIiIiIREREREAAAAAAAAAAAABERERERIiIjMzM0REREREREREMzMyIi"
"IREREREAAAAAAAAAAAERERERIiIjMzNEREVVVVVVVVRERDMzIiIhEREREAAAAAAAAAAREREREiIjMzRERVVVVmZmZmVVVVREMzMiIhEREREAAAAAAAABERER"
"EiIiMzREVVVmZmZ3d3dmZmZVVEQzMiIhEREREAAAAAAAERERESIiMzREVVZmZ33u7///7t12ZmVURDMyIhERERAAAAAAABERERIiIzNERVZmff//////////"
"/+dmVVRDMyIhERERAAAAAAEREREiIjM0RVZm3////////////////WZVRDMiIhEREQAAAAARERESIiM0RFVm3//////////////////9ZVRDMiIREREAAAAA"
"ERERIiIzRFVmf////////////////////+ZVRDMiIRERAAAAARERESIjNEVWbv/////////////////////+ZVQzMiEREQAAAAERERIiM0RVZv//////////"
"/////////////+ZVQzIiEREAAAARERESIjNFVm/////////////////////////9ZUQzIhERAAAAERERIiM0RWbv/////////////////////////2VUMyIh"
"EQAAAREREiIzRFZu///////////////////////////WVDMiIREAAAERERIiM0VW7///////////////////////////9lRDMiERAAABERESIzRFZv//////"
"//////////////////////ZVQzIhEQAAERERIiM0VW7///////////13d3fe///////////9ZUMyIhEAABERESIjRFZ///////////13d3d3d3//////////"
"/WVDMiIRAAAREREiM0RW7/////////93d3d3d3d3//////////1lQzIiEQAAERESIjNFZu/////////9d3d3d3d3d+/////////9ZUMyIhEAAREREiIzRWf/"
"////////93d3d3d3d3ff/////////WVEMiIRAAERERIiNEVt/////////9d3d3d3d3d33/////////1lQzIiEQABERESIzRFbf/////////Xd3d3d3d3d9//"
"///////3ZUMyIhEAAREREiM0VW7/////////d3d3d3d3d3fv////////5mVDMiIRAAERESIjNFVu/////////3d3d3d3d3d3/////////9ZVQzIhEQAREREi"
"IzRWbv/////////Xd3d3d3d3fv////////92VEMyIREAERERIiM0Vm7/////////53d3d3d3d+/////////+dlRDIiERABERESIjNFZt//////////53d3d3"
"d37//////////WVUMyIhEQAREREiIzRVbP///////////Xd3d97//////////+dlRDMiIREBERERIiM0VWfv//////////////////////////52ZUQzIiER"
"AREREiIzNFVnf//////////////////////////ndlVDMiIREQERERIiMzRWZ33////////////////////////+d3ZUQzIiERERERESIjNEVmd33///////"
"////////////////13dmVEMyIhEREREREiIzRFZ3d33/////////////////////93d3ZlRDMiIRERERESIjM0VWd3d37////////////////////nd3d2VU"
"MzIiEREREREiIzRFVnd3fv/////////////////////nd3dlVDMyIhERERESIiM0RWd3ff///////////////////////3d3ZVQzIiIREREREiIzRFZnd///"
"///////////////////////8d2VUMyIhERERERIiM0VWd9///////////////////////////3dmVEMyIREREREiIzRFZn3/////////////////////////"
"///ndlRDMiERERERIiM0VWff///////////+d3d37////////////nZUQzIhERERESIzRFZt///////////+d3d3d3fv//////////92VEMyIRERERIiM0RW"
"f///////////d3d3d3d3fv//////////5lVDMiERERESIjNFZu///////////Xd3d3d3d3f//////////+ZVQzIhEREREiI0RWf//////////+d3d3d3d3d3"
"///////////mVUMyIRERERIjNFVt///////////Xd3d3d3d3d+//////////5lVDMiERERESIzRWbv//////////d3d3d3d3d3fv//////////ZVQzIhERER"
"IiM0Vn///////////nd3d3d3d3d3///////////2VUMyIRERESIjRFbf//////////53d3d3d3d3ff//////////5lRDMiEREREiI0RW3//////////+d3d3"
"d3d3d37//////////9ZUQyIhERERIiNEVu///////////nd3d3d3d3ff///////////WVEMiIRERESIjRFbv///////////Xd3d3d3d9///////////+ZlQz"
"IiEREREiI0RW7////////////Xd3d3d37////////////WVEMyIRERERIiM0Vu/////////////td3d37/////////////dlRDIiERERERIjNFbf////////"
"///////////////////////mVUMyIhEREBESIzRVb//////////////////////////////+ZlRDMiERERAREiM0RW7/////////////////////////////"
"/WVEMyIhEREQERIiM0Vt/////////////////////////////9ZVQzMiERERABERIjNEVu////////////////////////////1lVEMyIhEREQARESIjNFVv"
"///////////////////////////WZUQzIiEREREAEREiIzRFbP/////////////////////////nZlRDMiIhEREQABEREiIzRFZv////////////////////"
"///9ZlVEMzIiEREREAARERIiMzRFZt/////////////////////+dmVUQzMiIREREQAAERERIiMzRFVm3//////////////////9dmVVRDMyIhEREREAABER"
"ERIiMzRFVmbO//////////////7XdmVVREMzIiEREREQAAABERERIiMzRFVWZnfd7u/////u7cd2ZmVVREMzIiIRERERAAAAARERESIiMzREVVVmZmd3d3d3"
"d2ZmZlVVREMzMiIhEREREAAAAAARERERIiIzNERFVVVmZmZmZmZlVVVUREMzMiIiEREREQAAAAAAARERERIiIzMzRERFVVVVVVVVVVREREMzMiIiEREREREA"
"AAAAAAARERERIiIiMzM0REREREREREREQzMzMiIiIREREREQAAAAAAAAAREREREiIiIzMzMzNERERDMzMzMzIiIiIREREREQAAAAAAAAAAARERERESIiIiIz"
"MzMzMzMzMzMiIiIiIRERERERAAAAAAAAAAAAARERERERIiIiIiIiIiIiIiIiIiIiEREREREREAAAAAAAAAAAAAARERERERESIiIiIiIiIiIiIiIhERERERER"
"EAAAAAAAAAAAAAAAABERERERERERIiIiIiIiIhEREREREREREQAAAAAAAAAAAAAAAAAAAAAAERERERERERERIiIiIiIRERERERERERAAAAAAAAAAAAAAAAAA"
"ERERERERERIiIiIiIiIiIiIiEREREREREAAAAAAAAAAAAAAAARERERERESIiIiIiIiIiIiIiIiIhEREREREAAAAAAAAAAAAAABEREREREiIiIiIzMzMzMzMz"
"MiIiIiEREREREAAAAAAAAAAAABEREREREiIiIjMzMzMzMzMzMzMzMiIiIREREREAAAAAAAAAAAEREREREiIiIzMzNEREREREREREMzMyIiIREREREAAAAAAA"
"AAAREREREiIiIzM0RERFVVVVVVVVRERDMzIiIhEREREAAAAAAAABERERESIiIzM0REVVVWZmZmZmZVVVREQzMiIhEREREAAAAAAAERERERIiIzM0RFVVZmZm"
"d3d3dmZmZVVEQzMiIhEREREAAAAAARERERIiIjM0RFVWZnd97u////7t12ZmVURDMyIhERERAAAAAAEREREiIjM0RFVmZ37////////////+1mZVRDMyIhER"
"ERAAAAARERESIiMzRFVWZ9/////////////////WZVRDMyIhEREQAAABERERIiIzNEVWZ+///////////////////9ZVRDMiIREREQAAARERESIjM0RVZt//"
"///////////////////+ZVRDMiIREREAABERERIiMzRFZn///////////////////////+ZVRDMiIRERAAEREREiIzNEVm3////////////////////////9"
"ZVQzIiEREQABERESIiM0RWbf/////////////////////////9ZUQzIiEREAEREREiIzRFZt///////////////////////////mVUMyIhERABERESIjNEVW"
"3////////////////////////////WVEMyIREQAREREiIzRVbP////////////////////////////9lVDMiIREBERESIjNEVn//////////////////////"
"////////dlRDIiERAREREiIzRVbf///////////+13d3fv///////////9ZVQzIhEQERESIjNEVn////////////13d3d3d+///////////mZUMyIhEREREi"
"IzRVbv//////////53d3d3d3d+//////////52VDMiIRERERIjNEVn///////////3d3d3d3d3ff//////////dlRDIiERERESIzRVbf//////////13d3d3"
"d3d3f//////////3ZUQzIhERERIiM0Vm7//////////nd3d3d3d3d3///////////WVUMyIRERESIjRFZ///////////13d3d3d3d3d+//////////1lVDMi"
"EREREiI0RW3//////////3d3d3d3d3d3fv/////////9ZlQzIhERERIjNFVt//////////93d3d3d3d3d37//////////WZUMyIhERESIzRVbv/////////+"
"d3d3d3d3d3ff//////////1mVDMiIREREiM0Vm7//////////nd3d3d3d3d37//////////9ZlQzIiERERIjNFZv//////////53d3d3d3d3d///////////"
"92ZUMyIREREiIzRWbv/////////+d3d3d3d3d33///////////dmVDMiERERIiM0Vm7//////////3d3d3d3d3d////////////3ZVQzIhERERIjNFVu////"
"///////nd3d3d3d37///////////92VUMyIRERESIzRVbv///////////nd3d3d33////////////+dlRDMiEREREiM0RW3////////////9d3d33///////"
"///////nZUQyIhERERIjNEVn////////////////////////////////12VEMiIRERESIjRFZu///////////////////////////////9dlQzIiEREREiIz"
"RVZ///////////////////////////////92ZUMyIhERERIiM0RWff/////////////////////////////+dlVDMiIRERESIjNEVmfv////////////////"
"/////////////nZUQzIhEREREiIzRFZnfv////////////////////////////12VEMiIRERERIiM0RVZ3ff///////////////n7//////////3dlQzIiER"
"EREiIjNEVWd3d+/////////////9d+//////////52VUMyIhERERIiIzRFVnd3d33//////////nd3f//////////+dlRDMiERERESIjM0RWZ3d3d3d93v//"
"/u13d3d9///////////HZUQyIhEREREiIzRFVmd3d3d3d3d3d3d3d3d3ff//////////dmVDMiIRERERIiM0RWZ3d3d3d3d3d3d3d3d3d37//////////nZU"
"QzIiERERESIzRFzd3d3d3d3dd3d3d3d3d3d+//////////12VEMiIREREREiM0Re/////////3d3d3d3d3d33//////////nZVQzIiERERESIjNEX///////"
"//93d3d3d3d3d+//////////12VEMyIREREREiIzRV//////////d3d3d3d3d3f//////////3ZlQzIiERERERIiM0Vv/////////3d3d3d3d3d+////////"
"//52VEMyIhERERESIjRFz//////////Xd3d3d3d33//////////3ZlQzIiEREREREiI0Rc//////////93d3d3d3ff//////////52VEMyIhEREQERIiM0Vv"
"//////////53d3d3d+///////////3ZVQzIiEREREBERIjNFb////////////Xd3d+////////////1mVEMyIhEREQARESIzRV7/////////////////////"
"///////nZVQzIiEREREAEREiM0Vd///////////////////////////+dlVEMyIhEREQABERIiNEVv///////////////////////////WZUQzIiEREREAAR"
"ESIjNFXf/////////////////////////9ZlRDMiIREREQAAERESIzRFbv////////////////////////1mVUMzIiEREREAABEREiIzRVb/////////////"
"///////////WZVRDMiIREREQAAAREREiM0RV3//////////////////////3ZlVEMyIhERERAAAAERERIiM0RWz////////////////////+dlVUQzIiIRER"
"EQAAABERERIiM0RWbv//////////////////dmVUQzMiIhERERAAAAABERESIjM0RVZu///////////////+xmZVRDMyIiEREREAAAAAARERESIjM0RVZm3/"
"///////////tdmZVREMzIiIRERERAAAAAAARERESIjM0RVVmZ93u////7t13ZmZVREMzMiIhEREREAAAAAAAERERESIjM0RFVVZmZnd3d3ZmZmVVREQzMiIh"
"EREREQAAAAAAAAERERESIiMzRERVVVZmZmZmVVVVREQzMiIiERERERAAAAAAAAAAERERESIiMzNERERVVVVVVVREREMzMyIiIREREREAAAAAAAAAABERERES"
"IiIzMzNERERERERERDMzMiIiIREREREQAAAAAAAAAAABERERERIiIiMzMzMzMzMzMzMzIiIiIRERERERAAAAAAAAAAAAABERERERIiIiIiMzMzMzMzMyIiIi"
"IRERERERAAAAAAAAAAAAAAABERERERESIiIiIiIiIiIiIiIiIREREREREAAAAAAAAAAAAAAAAAEREREREREiIiIiIiIiIiIiEREREREREQAAAAAAAAAAAAAA"
"AAAAERERERERERESIiIiIhEREREREREREQAAAAAAAAAAAAAAAAAAAAABERERERERERERIiIiIhEREREREREREAAAAAAAAAAAAAARERERERERIiIiIiIiIiIi"
"IREREREREQAAAAAAAAAAABERERERERIiIiIiIiIiIiIiIiIRERERERAAAAAAAAAAARERERERIiIiIiMzMzMzMzMyIiIiIREREREQAAAAAAAAEREREREiIiIj"
"MzMzMzMzMzMzMzIiIiERERERAAAAAAABERERERIiIjMzM0REREREREREMzMyIiIRERERAAAAAAAREREREiIiMzM0RERVVVVVVVVEREMzMiIhEREREAAAAAER"
"ERERIiIzM0REVVVVZmZmZmVVVURDMyIiIREREQAAABEREREiIiMzREVVVmZmd3d3d2ZmZVVEQzMiIhERERAAARERERIiIzNERVVmZnfd7v///u3XZmVVRDMy"
"IiERERAAARERESIiMzREVWZn3v///////////nZlVEQzIiEREREAEREREiIjM0RVZm3////////////////WZVRDMiIREREAEREREiIzNEVWbf//////////"
"///////+ZlRDMyIhEREBERERIiMzRVZn/////////////////////GVEMyIhEREBERESIjM0RWbv/////////////////////2ZUQzIiERERERESIjNEVm//"
"//////////////////////ZVQzIiEREREREiIzRFZv////////////////////////5lRDMiEREREREiM0RVb//////////////////////////WVDMiIRER"
"ERIiM0VW7//////////////////////////mVEMiIRERERIjNEVu///////////////////////////2VUMyIRERESIjNFXf////////////////////////"
"///9ZUMyIRERESIjRFb///////////53d97////////////9ZUMyIhERESIzRF3//////////9d3d3fv///////////9ZUQyIhERESIzRV7//////////Xd3"
"d3d////////////+ZUQyIhERESIzRd//////////93d3d3d+///////////+ZUQyIhERESIzRe//////////13d3d3d+///////////9ZUMyIhERESIzRf//"
"////////d3d3d3d+///////////9ZUMyIhERESIzRP/////////+d3d3d3d+///////////8ZUMyIRERESIzTf/////////9d3d3d3d////////////mVUMy"
"IRERESIjPf/////////9d3d3d3fv///////////mVEMyIRERESIjPO7u7u7u7u7nd3d3d3f///////////9mVDMiIRERERIiM0RVVmZnd3d3d3d3d3//////"
"//////5lVDMiIRERERIiMzREVVZmZmZnd3d3d////////////+ZlRDMiEREREREiIzNERFVVVmZmd3d3f////////////9ZVQzIiERERERESIjMzRERVVWZm"
"d3d3/////////////WZUQzIhEREBERERIiIzM0REVVZmd3ff////////////5mVEMyIhEREBERERIiIjMzRERVZmd37////////////9dlVDMiIREREAERER"
"ESIiIzNERVZnd+/////////////XZVRDMiIREREAERERERIiIjM0RVZnf/////////////d2VUQzIiEREREAAREREREiIjM0RVZn/////////////nZlVEMy"
"IiERERAAABEREREiIiM0RVZ+////////////12ZVRDMiIhERERAAABERERESIiM0RVbv///////////ndmVUQzIiIREREQAAAAERERESIiM0RV3/////////"
"//13ZlVEMyIiEREREAAAAAERERESIiM0RW7//////////nd2VURDMiIhEREREAAAAAARERESIiM0RW//////////53ZlVEQzIiIRERERAAAAAAARERESIiM0"
"Rd/////////+d2ZVRDMyIiEREREQAAAAAAARERESIiM0Rd/////////3dmVUQzMiIhEREREAAAAAAAARERESIiM0Re/////////3ZlVEMzIiIRERERAAAAAA"
"AAARERESIjM0Rd7u7u7u7u7WZVRDMyIiIRERERAAAAAAAAARERESIjM0VWZ3d3d3d3dmVURDMiIhEREREQAAAAAAAAARERESIjNEVWZ3d3d3d3dmVUQzMiIh"
"EREREAAAAAAAAAARERESIjNEVWZ3d3d3d3ZlVEMzIiIRERERAAAAAAAAAAAREREiIzNFVmd3d3d3d3ZlVEMzIiEREREQAAAAAAAAAAAREREiIzRFVmd3d3d3"
"d3ZlVEMyIiEREREQAAAAAAAAAAAREREiIzRF///////////lVDMyIhEREREAAAAAAAAAAAAREREiM0RV///////////lVDMyIhERERAAAAAAAAAAAAARERIi"
"M0Rd///////////VVDMiIhERERAAAAAAAAAAAAARERIiM0Vd///////////VVDMiIREREQAAAAAAAAAAAAERERIiM0Vu//////////9lVDMiIREREQAAAAAA"
"AAAAAAERERIiNEVu//////////9lRDMiIREREAAAAAAAAAAAAAERERIjNEVv//////////5lRDMiEREREAAAAAAAAAAAAAERERIjNEVv//////////5lRDMi"
"EREREAAAAAAAAAAAAAERERIjNFXf//////////1lRDIiERERAAAAAAAAAAAAAAERERIjNFXf//////////1VQzIiERERAAAAAAAAAAAAAAARERIjNEXv////"
"//////ZUQzIhEREQAAAAAAAAAAAAAAARERIjNEX//////////+ZUQyIhEREQAAAAAAAAAAAAAAARERIiM0X//////////+VEMyIhEREQAAAAAAAAAAAAAAAR"
"ERIiM03//////////+VEMyIREREAAAAAAAAAAAAAAAAREREiM03//////////9RDMiIREREAAAAAAAAAAAAAAAAREREiIz7//////////0QzIiERERAAAAAA"
"AAAAAAAAAAABEREiIzNEVVVmZmVVVEMzIiERERAAAAAAAAAAAAAAAAABERESIjM0REVVVVVERDMyIhERERAAAAAAAAAAAAAAAAABERERIiMzNEREREREMzMi"
"IREREQAAAAAAAAAAAAAAAAAAERERIiIjMzMzQzMzMyIiEREREAAAAAAAAAAAAAAAAAAAERERESIiIzMzMzMyIiIhEREREAAAAAAAAAAAAAAAAAAAARERERIi"
"IiIiIiIiIiIRERERAAAAAAAAAAAAAAAAAAAAABERERERIiIiIiIiIREREREQAAAAAAAAAAAAAAAAAAAAAAEREREREREiIhEREREREREAAAAAAAAAAAAAAAAA"
"AAAAAAARERERERERERERERERERAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAERERERERESIiIiIiIREREREREQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB"
"EREREREiIiIiIiIiIiIhEREREREAAAAAAAAAAAAAAAAAAAAAAAAAAAAREREREiIiIiIiIiIiIiIiIhEREREQAAAAAAAAAAAAAAAAAAAAAAAAAAERERESIiIi"
"MzMzMzMzMzIiIiIREREQAAAAAAAAAAAAAAAAAAAAAAAAABEREREiIiMzMzNERERDMzMzIiIhERERAAAAAAAAAAAAAAAAAAAAAAAAARERERIiIzM0RERERERE"
"REQzMyIiEREREAAAAAAAAAAAAAAAAAAAAAAAERERESIjMzREVVVVVVVVVVREMzIiIREREQAAAAAAAAAAAAAAAAAAAAABEREREiIzNERVVmZmZmZmZlVURDMy"
"IhEREQAAAAAAAAAAAAAAAAAAAAARERERIiMzRFVWZnd3d3d3dmZVVEMyIhERERAAAAAAAAAAAAAAAAAAAAARERESIjM0RV///////////////UQzIiERERAA"
"AAAAAAAAAAAAAAAAAAEREREiIjNEVt///////////////VRDMiIREREAAAAAAAAAAAAAAAAAABEREREiIzRFZu///////////////lVDMiIREREAAAAAAAAA"
"AAAAAAAAABERERIiM0RWbv///////////////2VEMyIhEREQAAAAAAAAAAAAAAAAARERESIjNEVWf////////////////2VUMyIhEREQAAAAAAAAAAAAAAAA"
"ARERESIjNFVn7////////////////8ZUQzIhEREQAAAAAAAAAAAAAAAAEREREiIzRFZn/////////////////9ZVQzIiERERAAAAAAAAAAAAAAABERERIiMz"
"RVZ+/////////////////9dlRDIiERERAAAAAAAAAAAAAAABERERIiM0RWff/////////////////+dlRDMiERERAAAAAAAAAAAAAAARERESIjNEVmfv////"
"//////////////dlVDMiIREREAAAAAAAAAAAAAAREREiIjNFVn3///////////////////d2VDMiIREREAAAAAAAAAAAAAEREREiIzRFZ37/////////////"
"//////12VEMiIREREAAAAAAAAAAAAAERERIiIzRVZ9////////////////////12VEMyIREREAAAAAAAAAAAABERERIiM0RWd+////////////////////52"
"VUMyIhEREQAAAAAAAAAAABERESIjM0Vmff////////////////////93ZUMyIhEREQAAAAAAAAAAARERESIjNEVnf/////////////////////93ZUQyIhER"
"EQAAAAAAAAAAEREREiIzRFZn7/////////////////////93ZUQzIhEREQAAAAAAAAAAEREREiIzRVZ3///////////t///////////XZUQzIiERERAAAAAA"
"AAABERERIiM0RWZ+//////////59///////////XZVQzIiERERAAAAAAAAABERERIiM0VWd///////////19///////////ndlQzIiERERAAAAAAAAARERES"
"IjNEVnfv//////////d9///////////3dlRDIiERERAAAAAAAAAREREiIjNFVnz//////////9d8///////////3dlRDMiERERAAAAAAAAEREREiIzRFZ37/"
"/////////nd3///////////9dlRDMiIREREAAAAAAAERERIiMzRVZ9///////////Xd3///////////9dlVDMiIREREAAAAAABERERIiM0RWd+//////////"
"93d37//////////+dmVDMiIREREAAAAAABERESIjNEVmff//////////53d37//////////+d2VDMiIREREAAAAAARERESIjNFVnf///////////d3d37///"
"////////d2VEMyIREREAAAAAAREREiIzRFZn3//////////+d3d37///////////d2VEMyIhEREAAAAAEREREiIzRVZ3///////////3d3d33///////////"
"12VUMyIhEREAAAABERERIiM0RWd9///////////nd3d33///////////12ZUMyIhEREAAAABERESIiM0VWd///////////93d3d33///////////53ZUMyIh"
"EREAAAARERESIjNEVnfv//////////53d3d3f///////////93ZUQyIhEREAAAAREREiIjNFVnf///////////d3d3d3f///////////93ZUQzIhEREAAAER"
"EREiIzRFZ37//////////+d3d3d3f////////////XZVQzIiEREAAAERERIiI0RVZ9///////////9d3d3d3fv///////////XZVQzIiEREAABERERIiM0RW"
"d+///////////nd3d3d3fv///////////nZlQzIiEREAABERESIiNEVmff///////////Xd3d3d3fv///////////ndlQzIiEREAARERESIjNEVnf///////"
"////93d3d3d3ff///////////3dlRDIiEREAAREREiIjRFZn3////////////////////////////////3dlRDMiEREAEREREiIzRFZ3////////////////"
"/////////////////9dlVDMiEREAERERIiIzRWZ9/////////////////////////////////9dlVDMiIREAERERIiM0RWd+////////////////////////"
"/////////+dmVDMiIREBERERIiM0VWfv//////////////////////////////////d2VDMiIREBERESIjNEVnf/////////////////////////////////"
"//d2VDMiIREBERESIjNFVn7///////////////////////////////////12VEMiIREREREiIzRFZ8////////////////////////////////////12VEMi"
"IREREREiIzRVZ+////////////////////////////////////52VEMyIREREREiI0RWbf////////////////////////////////////52VEMyIREREREi"
"M0RWf/////////////////////////////////////92VUMyIRERERIiM0VW3/////////////////////////////////////92VUMyIRERERIiNEVm////"
"///////////////////////////////////WVUMyIRERERIjNEVt////////////d3d3d3d3d3d3d37////////////WVUMyIRERERIjNFVu///////////+"
"d3d3d3d3d3d3d37////////////mVUMyIRERERIjNFXv///////////3d3d3d2Zmd3d3d37////////////2VEMyIRERERIjNFX////////////nd3dmZmZm"
"ZmZ3d33////////////2VEMiIRERERIjNF7////////////XdmZVVVVVVVVmd33////////////8VEMiIRERERIjNE////////////53ZlVVRERERFVVZnz/"
"///////////9VDMiIRERERIjNO////////////1mVVRERERERERFVmf////////////9RDMiERERERIjPf////////////ZlVERDMzMzMzREVVbv////////"
"///+QzIiERERERIiPv///////////9VVREMzMzMzMzM0RFX/////////////QzIiEREREREiMzRFVWZmZmZmZVVEQzMzIiIiIjMzREVVZmZmZmZmZVVEMyIh"
"EREREREiIzNERVVVVVVVVURDMzIiIiIiIiIzM0RFVVVmZmVVVURDMyIhEREBERESIjMzRERFVVREREMzMiIiIiIiIiIiMzNEREVVVVVUREMzIiIREREBERES"
"IiMzM0REREREMzMyIiIiIhESIiIiIjMzNEREREREQzMyIiEREREAERERIiIiMzMzMzMzMzIiIiIhERERERIiIiIzMzMzMzMzMzIiIhERERAAERERESIiIiIz"
"MzMiIiIiIhERERERERESIiIiIjMzMzMyIiIiIRERERAAAREREREiIiIiIiIiIiIhERERERERERERESIiIiIiIiIiIiIhEREREQAAABERERERESIiIiIiIhER"
"ERERERERERERERESIiIiIiIiIREREREREAAAAAERERERERERERERERERERERERERERERERERERERERERERERERERAAAAAAAAAAABERERERERIiIiIiIiIiIi"
"IiIiIiIiIiIiIRERERERERAAAAAAAAAAARERERESIiIiIiIiIiIiIiIiIiIiIiIiIiIiIhEREREREAAAAAAAAAERERERIiIiIiMzMzMzMzMzMzMzMzMzMzIi"
"IiIiIhEREREQAAAAAAABEREREiIiIzMzMzMzMzMzMzMzMzMzMzMzMzMzIiIiIREREQAAAAAAARERERIiIzMzM0REREREREREREREREREREQzMzMzIiIhERER"
"AAAAAAEREREiIjMzRERERFVVVVVVVVVVVVVVVUREREREQzMyIiEREREAAAAAEREREiIzNERFVVVVVVZmZmZmZmVVVVVVVVVVVUREMzIiIREREAAAABERERIi"
"MzRFVVZmZmZmZmZmZmZmZmZmZmZmZmVVVURDMiIREREAAAABERESIjNERVZmd3d3d3d3d3d3d3d3d3d3d3d2ZmZVREMyIhEREAAAAREREiIzNF7/////////"
"////////////////////////QzIhEREAAAAREREiIzRF/////////////////////////////////+QzIiEREAAAERERIiM0Rc//////////////////////"
"///////////+RDIiEREAAAERERIiM0Vd/////////////////////////////////+RDMiEREAABERERIjNEVu/////////////////////////////////9"
"VDMiEREAABERESIjNFVu/////////////////////////////////8VDMiEREAABERESIzRFZ+/////////////////////////////////lVDMiEREAARER"
"EiIzRVZ//////////////////////////////////lVDMiEREAAREREiIzRWbf/////////////////////////////////lVDMiEREAAREREiI0RWff////"
"/////////////////////////////VRDMiEREAAREREiM0RWfv/////////////////////////////////VRDIiEREAERERIiM0VWfv////////////////"
"////////////////9VQzIiEREAERERIiM0Vnf/////////////////////////////////5UQzIhEREAERERIiNEVnf/////////////////////////////"
"////5EMyIhEREAERERIjNEVn3////////////Xd3d3d3d3d3d3d3d3d2ZVREMyIhEREBERESIjNFVn7////////////Xd3d3d3d3d3d3d3d3ZmVURDMiIRER"
"EBERESIjNFZn7///////////93d3d3d3d3d3d3d3dmZlVEQzIiIREREBERESIjRFZ37///////////53d3d3d3d3d3d3d2ZmVVREMzIiERERABERESIzRFZ3"
"////////////53d3d3d3d3d3d3dmZlVURDMyIiERERABEREiIzRFZ9////////////13d3d3d3d3d3d3dmZlVEQzMiIhERERAREREiIzRVZ9////////////"
"13d3d3d3d3d3d3d2ZlVEQzIiIREREQAREREiIzRWZ+////////////d3d3d3d3d3d3d3d2ZlVEMzIiIREREQAREREiIzRWd+///////////+d3d3d3d3d3d3"
"d3d3ZlVEMzIiEREREAAREREiM0RWd///////////////////////////////1EMyIiEREREAAREREiM0RWd///////////////////////////////VEMyIi"
"EREREAARERIiM0VWff//////////////////////////////VUMyIhERERAAARERIiM0Vmff//////////////////////////////VUMyIhERERAAARERIi"
"M0Vnfv/////////////////////////////+VUMyIhERERAAARERIiNEVnfv/////////////////////////////9VUMyIhEREQAAARERIjNEVnf///////"
"///////////////////////9VUMyIhEREQAAARESIjNFVn3//////////////////////////////2VUMyIhEREQAAARESIjNFVn3///////////////////"
"///////////2VEMyIhEREQAAARESIjNFZ37//////////////////////////////mVEMyIREREQAAARESIjNFZ37//////////////////////////////m"
"VDMiIREREAAAARESIzRFZ3///////////////////////////////VVDMiIREREAAAARESIzRFZ3///////////////////////////////FRDMiIREREAAA"
"AREiIzRVZ9//////////////////////////////9VRDMiIREREAAAAREiIzRVZ9////////////13d3d3d3d3d3d3d3dmVUQzMiIREREAAAAREiIzRWd+//"
"//////////d3d3d3d3d3d3d3d3ZmVUQzIiIREREAAAAREiIzRWd+///////////+d3d3d3d3d3d3d3d2ZlVUQzIiIREREAAAAREiI0RWd////////////+d3"
"d3d3d3d3d3d3d2ZlVEQzIiIREREAAAAREiM0RWff///////////+d3d3d3d3d3d3d3dmZlVEQzMiIREREAAAAREiM0RWff///////////9d3d3d3d3d3d3d3"
"d2ZlVUQzMiIREREAAAAREiM0VWfv///////////3d3d3d3d3d3d3d3d3ZmVURDMiIREREAAAARIiM0Vmfv///////////3d3d3d3d3d3d3d3d3dmZVRDMiIR"
"EREAAAARIiM0Vmfv///////////nd3d3d3d3d3d3d3d3d2ZlVEMyIhEREAAAARIiM0Vnf//////////////////////////////////UMyIhEREAAAARIiM0"
"Vn3//////////////////////////////////EMyIhEREAAAARIiM0Vn3/////////////////////////////////9UMyIhEREAAAARIiNEVn7/////////"
"////////////////////////9UQyIhEREAAAARIiNEVn7/////////////////////////////////5UQyIhEREAAAARIiNEVn//////////////////////"
"////////////1UQyIhEREAAAARIiM0Vs//////////////////////////////////1UQyIhEREAAAARIiM0Vt//////////////////////////////////"
"ZUQyIhEREAAAAREiM0Vu//////////////////////////////////ZUMyIhEREAAAAREiM0Ve/////////////////////////////////+VUMyIREREAAA"
"AREiM0Re/////////////////////////////////+VEMyIRERAAAAAREiIzRf/////////////////////////////////9VDMiIRERAAAAAREiIzTf////"
"/////////////////////////////9RDMiERERAAAAARESIjPf/////////////////////////////////0QzIiERERAAAAARESIjNERVZmZ3d3d3d3d3d3"
"d3d3d3d3d3d3ZmZVVEMyIhEREQAAAAARERIiM0RFVVZmZmZmZmZ3d2ZmZmZmZmZmZmZVVURDMyIhEREQAAAAARERIiIzNERFVVVVVWZmZmZmZmZmVVVVVVVV"
"VUREMzIiIREREAAAAAAREREiIjMzRERERFVVVVVVVVVVVVVVVVREREREQzMyIiEREREAAAAAARERESIiIzMzNERERERERERERERERERERERDMzMzIiIhERER"
"AAAAAAABEREREiIiIzMzMzMzMzMzMzMzMzMzMzMzMzMzIiIiIRERERAAAAAAAAERERERIiIiIiMzMzMzMzMzMzMzMzMzMzIiIiIiIhEREREQAAAAAAAAARER"
"ERESIiIiIiIiIiIiIiIiIiIiIiIiIiIiIhEREREREAAAAAAAAAABERERERESIiIiIiIiIiIiIiIiIiIiIiIiIhERERERERAAAAAAAAAAAAAAAAABERERERER"
"EREREREREREREAAAAAAAAAABERERERESIiIiIiERERERERAAAAAAAAABERERESIiIiIiIiIiIREREREQAAAAAAABEREREiIiIiIzMyIiIiIRERERAAAAAAAB"
"EREREiIiMzMzMzMzMyIiIREREQAAAAABERERIiIjMzREREREMzMyIiERERAAAAAAERERIiIzNERERVVEREQzMiIhEREQAAAAEREREiIzNERVVVVVVVVEQzIi"
"IRERAAAAEREREiIzREVVZmZmZmVVREMyIhEREAAAAREREiIzNF////////////5DMiIREQAAARERESIjNEX////////////kMyIhERAAABERESIjNEXf////"
"///////9RDMiEREAAAERERIjM0Vd////////////1UMyIREQAAERERIiM0RW7///////////9VQzIiERAAAREREiIzRWb////////////mVEMiIREAABERES"
"IzRFZ////////////+ZUQyIhEQABERESIjNFVt////////////1lRDMiERAAERERIiM0Vm3////////////WVEMyIREAAREREiM0RWfv///////////3ZUQz"
"IhEQAREREiIzRVZ+////////////dlRDMiERABERESIjNFZn////////////52VEMiIREAERERIiM0Vnf////////////nZUQyIhEQAREREiM0RWff//////"
"/////9dlRDIiERAREREiIzRFZ9////////////12VEMiIREBERESIjNFVn7///////////93ZUMyIhEQERERIiM0Vnf////////////nZlQzIiERAREREiI0"
"RWd////////////+dlVDMiIREBERESIzRFZ9////////////12VUMyIhERERESIjNFVn3////////////XZUQzIhERERERIiM0Vmfv///////////3dlRDMi"
"EREREREiIzRWd+////////////d2VEMiIRERERESIjRFZ3////////////53ZUMyIhERERERIjNEVnf////////////nZlQzIiERERERIiM0VWff////////"
"///9dlVDMiIREREREiIzRVZ9////////////12VUMyIhERERESIjNFZn7///////////93ZUQzIhERERERIiM0Vnf////////////3dlRDIiEREREREiM0RW"
"d////////////+d2VDMiIREREREiIzRFZ8////////////52ZUMyIhERERESIjNFVn3////////////XZVQzIiERERERIiM0Vmfv///////////8dlRDMiIR"
"EREREiIzRWd+////////////d2VEMyIRERERESIjRFZ3////////////53ZUMyIhERERERIjNEVnf////////////nZlQzIiERERERIiM0VWff//////////"
"/9dlVDMiIREREREiIzRVZ9////////////12VUMyIhERERESIjNFZn7///////////93ZUQzIhEREQERIiM0Vnf////////////3dlRDIiERERAREiI0RWd/"
"///////////+d2VDMiIREREBESIzRFZ8////////////52VUMyIhEREQERIjNEVn3////////////XZVQzIiERERAREiM0VWfv///////////8dlRDMiERER"
"ABEiIzRVZ+////////////d2VEMiIREREAESIjNFZn////////////52ZUMyIhEREQARIiM0Vnf////////////nZVQzIiERERABEiIzRWff///////////9"
"dlRDMiERERAAESIjRFZ9////////////12VEMiIREREAARIiNEVn7///////////93ZUMyIhEREQABEiI0RWfv///////////3ZVQzIiERERAAESIjRFZ///"
"/////////+dlRDMiEREREAARIiNEVn////////////52VEMiIREREAABEiI0RW3////////////WZUMyIhEREQAAESIjRFbf///////////9ZUQzIhERERAA"
"ARIiM0Vu////////////ZlRDIiERERAAABESIzRV////////////5lVDMiIREREAAAERIjNEX////////////mVEMyIREREQAAAREiI0Tf///////////9VE"
"MyIhEREQAAABESIjNN////////////1UMzIiERERAAAAEREiMz7///////////9UQzIiERERAAAAARESIjNERVVmZmZmZVVEQzIiEREREAAAABEREiIzNERV"
"VVVVVVREMzIiEREREAAAAAERESIiMzREREVVREREMzIiIREREQAAAAAREREiIjMzNEREREQzMzIiIREREQAAAAABERERIiIiMzMzMzMzMiIiIREREQAAAAAA"
"ARERERIiIiIjMzIiIiIiEREREQAAAAAAAAERERERIiIiIiIiIiIhEREREQAAAAAAAAAREREREREiIiIiIhEREREREQAAAAAAAAAAERERERERERERERERERER"
"EQAAAAAAAAAAAAAAAAABERERERERERERERERERERERERERERERERERERERERERERERERAAAAAAAAAAAAAREREREREiIiIiIiERERERERERERERERIiIiIiIi"
"IiIhEREREREAAAAAAAAAAAERERERIiIiIiIiIiIiIRERERERERERIiIiIiIiIiIiIiIhEREREQAAAAAAAAABEREREiIiIiMzMzIiIiIiIRERERERIiIiIiMz"
"MzMzMyIiIiIREREQAAAAAAAAARERERIiIjMzMzMzMzMyIiIiEREREiIiIjMzMzMzMzMzMzIiIhERERAAAAAAAAERERESIiMzM0RERERDMzMiIiIiIiIiIiMz"
"M0REREREREMzMyIiIREREAAAAAAAEREREiIzM0RERVVVRERDMzIiIiIiIiIzM0RERVVVVVREREMzMiIREREAAAAAABERERIiMzREVVVVVVVVVEQzMyIiIiIj"
"MzREVVVVVVVVVVVERDMyIhEREAAAAAABERESIjM0RVVmZmZmZmVVRDMzMiIiMzNERVVmZmZmZmZmVVREMyIhEREAAAAAARERESIzNE7///////////5EQzMz"
"MzM0RFVWb///////////////IiEREQAAAAAREREiIzRF////////////5URDMzMzNERVZm///////////////zIiEREQAAAAERERIiM0Rc////////////5V"
"REMzM0RFVWZ///////////////8zIiEREQAAAAERERIiM0Vd////////////1lVERERERVZnf///////////////QzIiEREQAAABERERIjNEVu//////////"
"//xlVURERFVWZ3///////////////0QzIiEREQAAABERESIjNFVu////////////dmVVREVVZnd///////////////5UQzIiEREQAAABERESIzRFZ+//////"
"/////+d2ZVVVVWZ3f//////////////+VUQzIhEREAAAAREREiIzRVZ////////////+d2ZlVVZmd3///////////////mVEMyIhEREAAAAREREiIzRWbf//"
"/////////9d3ZmZmZ3d///////////////9lVEMyIhEREAAAAREREiI0RWff///////////9d3dmZnd3f///////////////ZlRDMiIREREAAAAREREiM0RW"
"fv///////////3d3d3d3d3///////////////nZVQzMiEREREAAAERERIiM0VWfv///////////nd3d3d3d///////////////52VUQzIiERERAAAAERERIi"
"M0Vnf////////////nd3d3d3f//////////////+dmVEMyIhERERAAAAERERIiNEVnf////////////nd3d3d3///////////////nZlRDMiIhEREQAAAAER"
"ERIjNEVn3////////////Xd3d3d///////////////52ZVQzMiIREREQAAABERESIjNFVn7////////////Xd3d3f//////////////+d2VUQzIiEREREAAA"
"ABERESIjNFZn7///////////93d3d3///////////////ndlVEMyIiEREREAAAABERESIjRFZ37///////////53d3d///////////////13ZVRDMiIhERER"
"AAAAABERESIzRFZ3////////////53d3f//////////////9d2ZUQzMiIRERERAAAAABEREiIzRFZ9////////////13d3///////////////XdmVUQzIiIR"
"EREQAAAAAREREiIzRVZ9////////////13d///////////////13dlVEMyIiEREREAAAAAAREREiIzRWZ+////////////d3f//////////////9d3ZlRDMy"
"IhEREREAAAAAAREREiIzRWd+///////////+d3///////////////Xd2ZUQzMiIhERERAAAAAAAREREiM0RWd////////////+d///////////////13d2VU"
"QzIiIRERERAAAAAAAREREiM0RWd////////////93//////////////9d3d2VUMzIiIREREQAAAAAAARERIiM0VWff///////////+///////////////9d3"
"dmVEMyIiEREREQAAAAAAARERIiM0Vmff///////////////////////////+d3dmVEMyIiEREREAAAAAAAARERIiM0Vnfv//////////////////////////"
"//d3dlVEMyIiEREREAAAAAAAARERIiNEVnfv////////////////////////////53dlVEMyIhEREREAAAAAAAARERIjNEVnf///////////////////////"
"//////93dmVEMyIhERERAAAAAAAAARESIjNFVn3//////////////////////////////XdmVEMyIhERERAAAAAAAAARESIjNFVn3///////////////////"
"///////////3d2VEMyIhERERAAAAAAAAARESIjNFZ37//////////////////////////////9d2VUMyIiERERAAAAAAAAARESIjNFZ37///////////////"
"///////////////+d2VUMzIiERERAAAAAAAAARESIzRFZ3////////////////////////////////13ZUQzIiERERAAAAAAAAARESIzRFZ3////////////"
"////////////////////53ZVQzIiEREREAAAAAAAAREiIzRVZ9////////////////////////////////93ZlQzIiEREREAAAAAAAAREiIzRVZ9////////"
"/////////////////////////ndlRDMiIREREAAAAAAAAREiIzRWZ+//////////////////7f/////////////3dlVDMiIREREAAAAAAAAREiIzRWd+////"
"/////////////9d+/////////////9dmVDMiIREREQAAAAAAAREiI0RWd/////////////////93d9/////////////+d2VEMyIhEREQAAAAAAAREiM0RWff"
"///////////////+d3d37/////////////12VUMyIhEREQAAAAAAAREiM0RWff///////////////Xd3d37/////////////52ZUMyIhEREQAAAAAAAREiM0"
"VWfv//////////////d3d3d3f/////////////93ZUQzIhERERAAAAAAARIiM0VWfv/////////////nd3d3d3fv/////////////nZVQzIiERERAAAAAAAR"
"IiM0Vmfv////////////x3d3d3d3ff/////////////nZlQzIiERERAAAAAAARIiM0Vmf////////////nd3d3d3d3d//////////////9dlRDMiERERAAAA"
"AAARIiM0Vn3////////////Xd3d2Zmd3d+//////////////dlVDMiIRERAAAAAAARIiM0Vn3////////////Xd3ZmZmZnd9//////////////1mVDMiIRER"
"AAAAAAARIiM0Vn7///////////93d2ZlVVZmd3//////////////52VEMiIREREAAAAAARIiNEVn7///////////53dmVVVVVWZ33//////////////GVEMy"
"IREREAAAAAARIiNEVn////////////53dlVUREVVZ3f//////////////mVUMyIREREAAAAAARIiM0Vs////////////53ZVVERERVVnfv/////////////2"
"VUMyIREREAAAAAARIiM0Vt////////////12ZVRERERFVmff/////////////9VUMyIREREAAAAAAREiM0Ve////////////xmVURDMzREVWZ///////////"
"////VUMyIREREAAAAAAREiM0Ve////////////dlVEMzMzM0RVZ+//////////////1UMyIRERAAAAAAAREiM0Re///////////+ZVREMzMzMzRFVt//////"
"////////5EMyIRERAAAAAAAREiIzRf///////////+ZURDMzIiMzNEVW7//////////////UMyIRERAAAAAAAREiIzTf///////////9VUQzMiIiIzM0RV3/"
"/////////////jMiIRERAAAAAAARESIjPf///////////9REMzIiIiIiMzRFX//////////////zMiIRERAAAAAAARESIjM0RVVmZmZmZlVURDMyIiIiIiIz"
"NEVVZmZmdmZmZlVUQzIiERERAAAAAAARERIiMzREVVVVVVVVREMzIiIiIiIiIjM0RFVVVmZmZVVVREMzIiEREQAAAAAAARERIiIzNERERVVUREQzMyIiIhER"
"EiIiMzNEREVVVVVVRERDMyIhEREQAAAAAAAREREiIiMzNEREREQzMzIiIiERERERIiIjMzNERERERERDMzIiIREREQAAAAAAARERERIiIjMzMzMzMzMiIiIR"
"EREREREiIiIzMzMzMzMzMzMiIiEREREAAAAAAAABEREREiIiIiIzMyIiIiIhERERERERERIiIiIjMzMzMzMiIiIhERERAAAAAAAAAAERERERIiIiIiIiIiIi"
"ERERERERERERERIiIiIiIiIiIiIiERERERAAAAAAAAAAARERERERIiIiIiIhERERERERERERERERERIiIiIiIiIiEREREREQAAAAAAAAAAABERERERERERER"
"EREREREREREREREREREREREREREREREREREREAAAAAAAAAAAAAAAAAABEREREREREREREREREREREREREREREREREREREREREREREREQAAAAAAAAAAERERER"
"ESIiIiIiIiERERERERERERERERERERIiIiEREREREREQAAAAAAAAAREREREiIiIiIiIiIiIhEREREREREREREREiIiIiIiIiIREREREQAAAAAAABEREREiIi"
"IjMzMzMyIiIiIRERERERERERIiIiIiIiIiIiIhEREREAAAAAAAERERESIiIzMzMzMzMzMiIiIREREREREREiIiIzMzMzMzIiIiEREREAAAAAARERESIiIzM0"
"REREREQzMzIiIhEREREREiIiMzMzRERDMzMyIiERERAAAAAAEREREiIzNEREVVVVVEREMzIiIiEREREiIiMzNERERERERDMyIiERERAAAAARERESIjM0RFVV"
"ZmZlVVREQzMiIiIRIiIiIzNERFVVVVVUREMyIiEREQAAAAERERIiM0RFVmZmd2ZmZlVEQzMiIiIiIiIjM0RFVWZmZmVVREMyIhEREAAAAREREiIzNE7/////"
"/////mVUQzMiIiIiIiMzRN///////////kMyIhERAAAAERERIiM0Rf///////////mVURDMyIiIiIzNEXf//////////5DMiIREQAAAREREiIzRFz///////"
"/////WZURDMyIiIjM0RV7//////////9RDMiEREAAAERERIiM0Vd////////////92ZURDMzMzMzRFVu///////////VQzIhERAAARERESIzRFbv////////"
"////52ZVRDMzMzNEVWb///////////VUMyIhEQAAERERIiM0VW7/////////////12ZVRDMzM0RFVs///////////mVEMiIREAABERESIzRFZ+//////////"
"///+d2ZURDMzREVWff//////////5lRDMiERAAERERIiM0VWf//////////////9d2VVRERERFZn7//////////+ZUQzIhEQABERESIjNFZt////////////"
"///9d2VVREREVWd+///////////WVEMyIREAAREREiI0RWff///////////////3d2VVRERVZnf///////////xlRDMiERAAERERIjNEVn7/////////////"
"///nd2ZVVVVWd3///////////3ZUQzIhEQAREREiIzRWZ+/////////////////Xd2ZVVVZnff//////////52VEMyIREAERERIiM0Vnf///////////////"
"//53d2ZlZmd33//////////+dlRDIiERABERESIjRFZ3//////////////////13d2ZmZ3d+///////////XZUQyIhEQAREREiM0RWff////////////////"
"//d3d2Znd3fv//////////12VDMiIREBERESIjNFVn7///////////////////d3d3d3d3///////////3dlQzIiERAREREiIzRWd+//////////////////"
"/9d3d3d3d///////////92ZUMyIhEQERERIiNEVnfv///////////////////8d3d3d33//////////+dlVDMiIREBERESIzRFZ3////////////////////"
"/nd3d3d+///////////nZVQzIiERARERIiM0RWff/////////////////////Xd3d3fv//////////12VEMyIRERERESIjNFVn3/////////////////////"
"93d3d3///////////9dlRDMiEREREREiIzRWZ+//////////////////////53d3d///////////93ZUQyIhERERERIiM0Vnfv//////////////////////"
"13d33//////////+d2VDMiIRERERESIzRFZ3////////////////////////d3d9///////////nZlQzIiEREREREiM0RWd////////////////////////+"
"d3fv//////////12VUMyIhERERESIjNFVn3////////////////////////9d37//////////9dlVDMiIREREREiIzRVZ9/////////////////////////3"
"d///////////93ZUQzIhERERERIiM0Vnfv/////////////////////////nf///////////d2VEMiIRERERESIjRFZ37//////////////////////////d"
"///////////ndlQzIiEREREREiM0RWd///////////////////////////////////////52ZUMyIhERERESIjNEVn3/////////////////////////////"
"/////////9dlVDMiIREREREiIzRVZ9///////////u///////////////////////////XZUQzIiERERERIiM0Vmfv//////////5+//////////////////"
"////////d2VEMyIRERERESIjNFZ37//////////9d//////////////////////////ndlRDIiEREREREiI0RWd////////////Xff//////////////////"
"//////53ZUMyIhERERERIjNEVnf///////////d3fv///////////////////////9dmVDMiIREREREiIzRFZ9///////////3d3f///////////////////"
"/////XZVQzIiERERERIiM0VWff//////////53d33///////////////////////d2VEMyIREREBESIjNFZn7//////////+d3d37///////////////////"
"///3dlRDMiERERAREiIzRWd+///////////Xd3d3//////////////////////53ZUMyIhEREQERIiNEVnf///////////13d3d9////////////////////"
"/+dmVDMiIREREBESIjRFZ9///////////3d3d3d+/////////////////////XZVQzIiERERAREiM0RWff//////////93d3d3d/////////////////////"
"12VEMyIREREAERIjNEVn7//////////+d3d3d3ff///////////////////3dlRDIiERERABEiIzRVZ+///////////nd3d3d3fv//////////////////53"
"ZUMyIhEREQARIiM0VWfv//////////13d3d3d3f//////////////////+dmVDMiIREREAESIjNFZn///////////3d3ZmZnd33//////////////////nZV"
"QzIhERERABEiIzRWbf//////////93d2ZmZmd37/////////////////12VEMyIREREAARIiM0Vn3///////////d3ZlVVZmd3/////////////////3dlRD"
"IiERERAAESIjNFZ+///////////ndmVVVVVmd9////////////////92ZUMyIhEREQABEiIzRWfv//////////13ZVVEVVVmd+///////////////+dlVDMi"
"IREREAARIiM0Vn///////////9dmVURERVVnd+///////////////nZUQzIhEREQAAESIjNFbP//////////93ZVRERERFVnff//////////////1mVDMiIR"
"EREAABEiIzRW3///////////dmVERDNERFVnfv/////////////9ZVQzIiERERAAAREiM0Ve///////////nZVRDMzM0RFVnf/////////////92VEMyIRER"
"EAAAERIjNFXv//////////5lVEMzMzM0RFVn3////////////+ZVQzIiERERAAABESIzRF7//////////9ZURDMzMzMzRFVn3////////////mVEMyIhEREQ"
"AAAREiIzRf///////////FVEMzIiIjMzRFVm7///////////5VQzIiERERAAAAEREiM03///////////VUQzMiIiIjMzRFVt///////////9VEMyIhEREQAA"
"ABERIiM9///////////1RDMyIiIiIiMzRFVe///////////EQzIiERERAAAAARESIjM0RVVmZmZmVVREMzIiIiIiIiMzREVVZmZndmZmVVRDMiIREREQAAAA"
"ERESIjM0RFVVVVVVREMzMiIiIiIiIiMzNERVVWZmZVVVREMyIiERERAAAAABERESIjMzREREREREMzMiIiIREREiIiIzM0REVVVVVUREMzIiIREREQAAAAAR"
"EREiIiMzM0RERDMzMiIiIRERERESIiIjMzRERERERDMzMiIhERERAAAAAAERERESIiIzMzMzMzMiIiIRERERERERIiIiMzMzMzMzMzMiIiEREREAAAAAAAER"
"ERESIiIiIiIiIiIiIREREREREREREiIiIiMzMzMyIiIiEREREQAAAAAAAAERERERIiIiIiIiIiIRERERERERERERESIiIiIiIiIiIiERERERAAAAAAAAAAER"
"EREREREiIiIhEREREREREREREREREREiIiIiIiIhEREREREQAAAAAAAAAAEREREREREREREREREREREREREREREREREREREREREREREREQAAAAAAAAAAAAAA"
"AAABERERERERIiIiIiIiIiIiIiIiIiIiIiIREREREREREAAAAAAAAAAAAAEREREREiIiIiIiIiIiIiIiIiIiIiIiIiIiIREREREREAAAAAAAAAAAAREREREi"
"IiIiIzMzMzMzMzMzMzMzMzMiIiIiIiEREREREAAAAAAAAAABEREREiIiIzMzMzMzMzMzMzMzMzMzMzMzMzIiIiIREREREQAAAAAAAAERERESIiMzMzNERERE"
"REREREREREREREQzMzMyIiIhEREREQAAAAAAARERESIiMzNEREREVVVVVVVVVVVVVVVVRERERDMzMiIhEREREAAAAAAAEREREiIzNERFVVVVVVZmZmZmZmZm"
"ZlVVVVVUREQzMiIiEREREAAAAAARERESIjM0RVVWZmZmZmZmZmZmZmZmZmZmZmVVVERDMyIiEREREAAAAAERERIiM0RFVmZ3d3d3d3d3d3d3d3d3d3d3d2Zm"
"VURDMyIiEREREAAAAREREiIzNF7/////////////////////////7tdmZVRDMyIhERERAAAAERERIiM0Rf/////////////////////////////WZVREMyIh"
"EREQAAAREREiIzRFz//////////////////////////////+ZlVEMyIhEREAAAERERIiM0Vd////////////////////////////////ZlVDMyIRERAAARER"
"ESIzRFbv////////////////////////////////xlRDMiIREQAAERERIiM0VW7/////////////////////////////////ZlRDMiEREAABERESIzRFZ+//"
"///////////////////////////////+ZVQzIiERAAERERIiM0VWf//////////////////////////////////9ZUQzIhEQABERESIjNFZt////////////"
"///////////////////////2VUMyIhEAAREREiI0RWff///////////////////////////////////WVDMiIRAAERERIjNEVn7/////////////////////"
"//////////////5lRDIiEQAREREiIzRVZ+///////////////////////////////////+ZVQzIhEAERERIiM0Vnf///////////////////////////////"
"/////2ZUMyIRABERESIjRFZ3////////////////////////////////////92VDMiEQAREREiM0RWff///////////9d3d3d3d3d9//////////////dlQz"
"IhEBERESIjNFVn7////////////Xd3d3d3d3d9/////////////3ZUMyIhAREREiIzRWZ+////////////d3d3d3d3d3d+////////////92VDMiIQERERIi"
"NEVnfv///////////nd3d3d3d3d3ff///////////+dlQzIhEBERESIzRFZ3////////////53d3d3d3d3d33////////////mZUMyIRARERIiM0RWff////"
"///////9d3d3d3d3d3d9////////////5lVDMiERERESIjNFVn3////////////Xd3d3d3d3d3fv///////////9ZUQzIhEREREiIzRWZ+////////////d3"
"d3d3d3d3d3////////////92VEMiIRERERIiM0Vnfv///////////nd3d3d3d3d3fv///////////+ZlQzIiERERESIzRFZ3////////////53d3d3d3d3d+"
"/////////////WVUMyIREREREiM0RWd////////////+d3d3d3d3d9/////////////+dlRDIiERERESIjNFVn3/////////////////////////////////"
"//92VUMyIhEREREiIzRWZ9///////////////////////////////////+dlRDMiERERERIiM0Vnfv//////////////////////////////////52VUMyIh"
"ERERESIjRFZ37//////////////////////////////////nZlRDMiIREREREiM0RWd///////////////////////////////////d2VUMyIhERERESIjNF"
"Vn3/////////////////////////////////93ZVRDMiIREREREiIzRVZ9/////////////////////////////////ndmVEMyIhERERERIiM0Vmfv//////"
"/////////////////////////9d2ZVQzIiIRERERESIjNFZ37//////////////////////////////9d3dlVEMyIhEREREREiM0RWd/////////////////"
"/////////////9d3d2ZUQzIiIREREBERIjNEVnf/////////////////////////////53d3dlVDMyIhERERAREiIzRVZ9//////////////////////////"
"////d3d2ZUQzIiIREREAERIiM0VWff/////////////////////////////9d3dlVDMyIhERERABESIjNFZn7///////////93d3d9/////////////nd3ZU"
"QzMiIREREQAREiIzRWd+///////////+d3d3d/////////////53d2VEMyIiERERAAERIiNEVnf////////////nd3d3fv////////////13dlRDMiIhEREQ"
"ABESIzRFZ9////////////53d3d33////////////+d3ZUQzIiEREREAAREiM0RWff///////////9d3d3d3/////////////3d2VEMyIhERERAAERIjNFVn"
"7///////////93d3d3d//////////////XdlRDMiIREREAABEiIzRVZ+////////////d3d3d3fv////////////53ZUQzIiERERAAARIiM0Vmfv////////"
"///nd3d3d33/////////////d2VUMyIhEREQAAESIjNFZn////////////53d3d3d37////////////9dlVDMiIREREAABEiIzRWff///////////9d3d3d3"
"d+/////////////nZlQzIiERERAAARIiM0Vn3////////////Xd3Zmd3ff////////////93ZUMyIhEREQAAESIjNFZ+////////////d3dmZmd3f///////"
"//////52VEMiIREREAABEiI0RWfv///////////nd2ZmZmd37////////////+dlRDIiERERAAARIiNEVn////////////53ZmVVZmd9/////////////9ZU"
"QzIhEREQAAESIjNFbP///////////+d2ZVVVZnff/////////////WVEMyIREREAABEiIzRW3////////////XZlVVVVZnfv////////////9lRDMiEREQAA"
"AREiM0Ve////////////x2VVRERVZn7/////////////ZUQyIhEREAAAERIjNFXv///////////3ZlVERERVZt/////////////+VEMiIRERAAABESIzRF7/"
"//////////5mVURERERVZv/////////////lQzIiEREQAAAREiIzRf///////////+ZVREMzNERVbv/////////////UMyIREREAAAERIiM03///////////"
"/VVEMzMzNERV3/////////////5DMiERERAAABERIiM9////////////1UQzMzMzNERV//////////////MyIhEREQAAARESIjM0RVVmZmZmZlVURDMzIiMz"
"M0RFVmZmZmZmZlVUQzMiERERAAAAERESIjM0RFVVVVVVVUREMzIiIiIzM0RFVVVWZmVVVURDMyIhEREQAAABEREiIjM0RERFVVREREMzMiIiIiIjMzRERFVV"
"VVVEREMzIiERERAAAAAREREiIiMzNEREREQzMzMiIiIiIiIiMzM0RERERERDMzMiIhEREQAAAAERERESIiIzMzMzMzMzMiIiIiESIiIiIzMzMzMzMzMzIiIh"
"ERERAAAAAAERERESIiIiIjMzIiIiIiIhEREREiIiIiIzMzMzMyIiIiEREREQAAAAAAERERERIiIiIiIiIiIiIRERERERERIiIiIiIiIiIiIiERERERAAAAAA"
"AAERERERESIiIiIiIRERERERERERERESIiIiIiIiIhEREREREAAAAAAAAAEREREREREREREREREREREREREREREREREREREREREREREQAAAAAAAAAAAAAAAA"
"AAABERERERERERESIiIiIiIiIhERERERERERAAAAAAAAAAAAAAAAAAARERERERERIiIiIiIiIiIiIiIiIREREREREQAAAAAAAAAAAAAAABERERERERIiIiIi"
"IiIiIiIiIiIiIiERERERERAAAAAAAAAAAAAAARERERERIiIiIiMzMzMzMzMzMzMiIiIiEREREREAAAAAAAAAAAABEREREREiIiIjMzMzMzRERERDMzMzMiIi"
"IhEREREQAAAAAAAAAAARERERESIiIjMzM0RERERERERERERDMzMiIiERERERAAAAAAAAAAERERERIiIiMzM0RERVVVVVVVVVVVRERDMzIiIhEREREAAAAAAA"
"ABERERESIiIzM0REVVVWZmZmZmZmZVVVREQzMyIiEREREQAAAAAAAREREREiIjMzREVVVmZmZ3d3d3d3ZmZmVVREMzIiIREREQAAAAAAARERESIiIzNERVVm"
"Znd93u7////u7dd2ZlVUQzMiIhERERAAAAAAEREREiIjM0RFVWZ3ff/////////////+12ZVREMzIiEREREAAAABERERIiIzNEVVZnfv////////////////"
"/+dmVUQzIiIREREAAAARERESIiMzRFVmff/////////////////////mZVRDMiIREREAAAARERESIjNERWZ9////////////////////////5lVEMyIhEREA"
"AAEREREiIzRFVmfv/////////////////////////mVUQzIhEREAAAERERIiMzRVZt///////////////////////////+ZUQzIiEREAABERERIiM0VWbf//"
"//////////////////////////5lRDIiEREAABERESIjNEVm3//////////////////////////////VVDMiEREAARERESIzRFVt////////////////////"
"///////////2VDMiIREAAREREiIzRVZ////////////////////////////////9VEMiIREAAREREiM0RWf////////////////////////////////+VEMy"
"IREAARERIiM0VW3////////////////////////////////+VUMyIREAERERIiNEVn7/////////////7Xd3d87/////////////VUMyIREAERERIjNFVt//"
"///////////nd3d3d3fP////////////VUMyIREAERESIjNFZu////////////53d3d3d3d9////////////VUMyIREAERESIjRFZ/////////////d3d3d3"
"d3d3////////////VEMyIREBERESIjRFbf///////////+d3d3d3d3d37///////////VEMiIREBERESIzRVbf///////////+d3d3d3d3d37//////////+"
"VDMiIREBERESIzRWbv////////////53d3d3d3d3fd3d3d3d3d3VRDMiIREBERESIzRWfv/////////////9d3d3d3d3d3d3d3d3d2ZVRDMiIREBERESIzRW"
"f////////////////td3d3d3d3d3d3d3dmVUQzIiEREBEREiIzRWf//////////////////+13d3d3d3d3d3ZlVEMzIiEREBEREiIzRWf///////////////"
"//////7Xd3d3d3d3ZlVEMzIiEREBEREiIzRWbv///////////////////////td3d3d3ZlVEMyIiEREREREiIzRVbv/////////////////////////9d3d3"
"ZlVEMyIiEREREREiIzRVbv///////////////////////////sd3ZlVEMyIhEREREREiIzRFZ/////////////////////////////53dlVEMyIhEREREREi"
"IzRFZ+/////////////////////////////3dmVEMyIhEREREREiIjRFZn//////////////////////////////12VUMyIhEREREREiIjNFVn3/////////"
"/////////////////////GZUQzIiEREREREiIzNEVmff/////////////////////////////3ZUQzIiEREREREiIjNEVmd9////////////////////////"
"/////9ZVQzIiEREREREiIzNEVWd3fv////////////////////////////ZlQzIiEREREREiIzNEVWd3d9////////////////////////////1lRDIiERER"
"ERIiIzRFVmd3d3fe//////////////////////////1lRDIiERERERIiMzRFVmd3d3d3fO////////////////////////5lRDMiERERESIiM0RVZnd3d3d3"
"d3d97/////////////////////5lVDMiERERESIjNEVVZnd3d3d3d3d3d33v//////////////////5lVDMiERERESIjNEVWZ3d3d3d3d3d3d3d3fe//////"
"//////////5lVDMiEREREiIzRF3d3d3d3d3d3Xd3d3d3d3fO//////////////5lRDMiEREREiIzRe///////////3d3d3d3d3d33/////////////1lRDIi"
"EREREiI0Re///////////3d3d3d3d3d3ff////////////dlRDIiEREREiM0Re///////////3d3d3d3d3d3ff////////////dlQzIiEREREiM0Vf//////"
"/////9d3d3d3d3d3ff///////////+ZlQzIhEREREiM0Vv///////////+d3d3d3d3d3f////////////9ZVQzIhEREREiM0Vv////////////53d3d3d3d3"
"7////////////nZUQyIhEREREiM0Vu/////////////+13d3d33v/////////////WZUMyIhEREREiM0Vu//////////////////////////////////92VE"
"MyIREREREiM0Vu//////////////////////////////////1mVDMiIREREREiM0Vd//////////////////////////////////dlRDMiIREREREiI0RW//"
"///////////////////////////////9ZVQzIiERERAREiIzRW7////////////////////////////////WZUQzIiERERARESIzRV3/////////////////"
"//////////////1mVEMyIhERERARESIjRFbv/////////////////////////////+ZlVDMyIhEREQARESIjNEVu/////////////////////////////WZV"
"RDMiIREREQARERIiM0VW7//////////////////////////+dmVUQzIiEREREAARERIiM0RVbf/////////////////////////XZlVEMyIiERERAAAREREi"
"IzRFVm7//////////////////////9dmVURDMiIhERERAAARERESIjNEVWZt7///////////////////12ZlVEMzIiIREREQAAARERESIiM0RFVmZ97/////"
"/////////+13ZmVURDMyIiEREREAAAABERERIiIzNEVVZmd3zd7u/////u7cd3ZmVVREMzMiIhEREREAAAAAEREREiIjM0RFVVZmZnd3d3d3d3dmZmVVVEQz"
"MyIiIRERERAAAAAAERERESIiMzNERFVVVmZmZmZmZmZlVVVERDMzIiIhEREREQAAAAAAARERERIiIjMzREREVVVVVVVVVVVVVEREMzMiIiIREREREAAAAAAA"
"ABERERESIiIzMzNERERERERERERERDMzMyIiIhERERERAAAAAAAAAAERERERIiIiIzMzMzREREREREMzMzMyIiIiEREREREQAAAAAAAAAAARERERESIiIiIj"
"MzMzMzMzMzMzMiIiIiIRERERERAAAAAAAAAAAAABERERERESIiIiIiIiIzMyIiIiIiIiIREREREREQAAAAAAAAAAAAAAERERERERESIiIiIiIiIiIiIiIiIR"
"EREREREREAAAAAAAAAAAAAAAABERERERERERIiIiIiIiIiIiEREREREREREQAAAAAAAAAAAAAAERERERERIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiERERERER"
"EAAAAAEREREREiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIhERERERAAAAERERERIiIiIiMzMzMzMzMzMzMzMzMzMzMzMzMiIiIiIhEREREQAAERERESIiIj"
"MzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMyIiIhEREREAERERESIiIzMzNERERERERERERERERERERERERERDMzMyIiIREREQARERESIiMzNERERFVVVVVVVVVV"
"VVVVVVVVVVVEREREQzMyIiERERERERESIjM0REVVVVVVVmZmZmZmZmZmZmZmZlVVVVVVRERDMyIhERERERESIjM0RFVWZmZmZmZ3d3d3d3d3d3d3ZmZmZmZm"
"VVVEQzMiIRERERESIjM0RVVmZ3d3d3d3d3d3d3d3d3d3d3d3d3d3ZmZVVEMyIhERERERIiM0T//////////////////////////////////////TMiIRERER"
"IiM0Tf/////////////////////////////////////9MyIhEREREiI0Re//////////////////////////////////////RDMiERERESIzRF7/////////"
"////////////////////////////9UMyIRERERIjNFX//////////////////////////////////////lQzIiERERIiNEVv////////////////////////"
"/////////////9VEMiIREREiI0Rd//////////////////////////////////////1UQyIhERESIzRF3/////////////////////////////////////9l"
"RDIiERERIjNFXv/////////////////////////////////////2VDMiIREREiM0Rf/////////////////////////////////////+VUMyIRERESIzRF//"
"////////////////////////////////////5UQzIhERERIiNEX//////////////////////////////////////VRDIiEREREiIzTf////////////////"
"/////////////////////9VDMiIRERERIjNO//////////////////////////////////////VEMyIREREREiM07///////////////////////////////"
"//////9EMyIhERERESIjNEVWZnd3d3d3d9////////////13d3d3d3d2ZlVEMyIhEREREREiIzRFVWZnd3d3d37///////////93d3d3d3ZmVVVEMzIiERER"
"ERESIjM0RFVWZnd3d3fv///////////nd3d3dmZVVUREMzIiERERAREREiIzNERFVWZ3d3d////////////+d3d3ZmVVVERDMyIiEREREBERERIiIzNERFVm"
"d3d8////////////53d2ZlVUREMzMyIiEREREAARERESIiMzNERVZnd33////////////Xd3ZVVERDMzMiIiEREREQABEREREiIjMzREVWd3fv//////////"
"/8d3ZVREMzMzIiIhEREREQAAARERERIiIjM0RVZnd+///////////+d3ZlREMzMiIiIhEREREQAAAAERERESIiIzNEVmd37///////////53dlVEMzMiIiIR"
"EREREQAAAAABEREREiIjM0RVZ3f////////////ndmVEMzIiIiEREREREQAAAAAAARERERIiIzRFVnff///////////9d2VUQzIiIhEREREREAAAAAAAABER"
"EREiIjM0VWd9////////////13ZUQzMiIhEREREREAAAAAAAAAARERERIiMzRVZ37///////////93ZlRDMiIhEREREREAAAAAAAAAABEREREiIzNFVnfv//"
"/////////ndlVDMyIhERERERAAAAAAAAAAAAARERESIjM0VWd+///////////+d2VEMyIiERERERAAAAAAAAAAAAABERERIiIzRWZ3////////////53ZUQz"
"IiERERERAAAAAAAAAAAAAAAREREiIjNFZn3////////////XZlQzMiIRERERAAAAAAAAAAAAAAABERESIjNEVnfv///////////8dlVDMiIRERERAAAAAAAA"
"AAAAAAAAERERIiM0RWd+////////////d2VEMyIhERERAAAAAAAAAAAAAAAAAREREiIzRFZ37///////////53ZUQzIiERERAAAAAAAAAAAAAAAAABERESIj"
"NEVnf////////////ndlQzIiEREREAAAAAAAAAAAAAAAAAERERIiM0VWff///////////9dmVDMiIREREAAAAAAAAAAAAAAAAAAREREiIzRVZ9//////////"
"//12VUMyIhEREQAAAAAAAAAAAAAAAAABERESIjNFZn7///////////93ZUQzIiERERAAAAAAAAAAAAAAAAAAERERIiM0Vnfv///////////ndlRDMiERERAA"
"AAAAAAAAAAAAAAAAAREREiI0RWd+///////////+d2VDMiIREREAAAAAAAAAAAAAAAAAABERESIjRFZ3////////////52VUMyIhEREQAAAAAAAAAAAAAAAA"
"AAERERIjNEVn3////////////XZVQzIiERERAAAAAAAAAAAAAAAAAAAREREiM0RWff///////////9dlRDMiERERAAAAAAAAAAAAAAAAAAABEREiIzRVZ+//"
"//////////d2VEMiIREREAAAAAAAAAAAAAAAAAAAERESIjNFVn7///////////52ZUMyIhEREQAAAAAAAAAAAAAAAAAAARERIiM0Vmf////////////nZVQz"
"IiERERAAAAAAAAAAAAAAAAAAABEREiIzRWff///////////9dlRDMiEREREAAAAAAAAAAAAAAAAAAAERESIjNFZ9////////////12VEMiIREREAAAAAAAAA"
"AAAAAAAAAAARERIiM0Vn7///////////92ZUMyIhEREQAAAAAAAAAAAAAAAAAAABEREiIzRWfv///////////nZVQzIiERERAAAAAAAAAAAAAAAAAAAAERES"
"IjNFZ+///////////+dlRDMiEREREAAAAAAAAAAAAAAAAAAAARERIiM0Vn////////////5mVEMiIREREAAAAAAAAAAAAAAAAAAAABERESIzRW3/////////"
"///WVUMyIhEREQAAAAAAAAAAAAAAAAAAAAERERIjNFXf///////////9ZUQzIhERERAAAAAAAAAAAAAAAAAAAAAREREiM0Ve////////////ZVQzIiERERAA"
"AAAAAAAAAAAAAAAAAAABERESIzRF7///////////5lRDMiIREREAAAAAAAAAAAAAAAAAAAAAARERIiM0X////////////lVDMiIREREAAAAAAAAAAAAAAAAA"
"AAAAABEREiIzTf///////////+VEMyIhEREQAAAAAAAAAAAAAAAAAAAAAAERERIiM9////////////1EMyIhEREQAAAAAAAAAAAAAAAAAAAAAAABEREiIzNE"
"VVZmZmZmZVVEMyIiERERAAAAAAAAAAAAAAAAAAAAAAAAERERIiMzREVVVVVVVVREMyIiERERAAAAAAAAAAAAAAAAAAAAAAAAABERESIjM0RERFVURERDMyIi"
"EREREAAAAAAAAAAAAAAAAAAAAAAAAAERERIiIjMzNEREREMzMyIiEREREAAAAAAAAAAAAAAAAAAAAAAAAAARERERIiIjMzMzMzMzMiIiEREREAAAAAAAAAAA"
"AAAAAAAAAAAAAAAAERERESIiIiIjMzIiIiIhEREREQAAAAAAAAAAAAAAAAAAAAAAAAAAABERERESIiIiIiIiIiIREREREQAAAAAAAAAAAAAAAAAAAAAAAAAA"
"AAARERERERIiIiIiIhEREREREQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEREREREREREREREREREREAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAR"
"ERERERERIiIiIiIRERERERERAAAAAAAAAAAAAAAAAAAAAAAAAAAAERERERESIiIiIiIiIiIhEREREREAAAAAAAAAAAAAAAAAAAAAAAAAABERERESIiIiIiIi"
"IiIiIiIhEREREQAAAAAAAAAAAAAAAAAAAAAAAAAREREREiIiIzMzMzMzMzMiIiIRERERAAAAAAAAAAAAAAAAAAAAAAAAERERESIiIzMzM0RERDMzMzIiIhER"
"EREAAAAAAAAAAAAAAAAAAAAAABEREREiIjMzREREREREREQzMyIiEREREAAAAAAAAAAAAAAAAAAAAAARERERIiIzNERFVVVVVVVVREQzMiIREREQAAAAAAAA"
"AAAAAAAAAAAAERERESIjM0RFVVZmZmZmZlVURDMyIhERERAAAAAAAAAAAAAAAAAAAAEREREiIzNEVVZmd3d3d3dmZlVEMzIiERERAAAAAAAAAAAAAAAAAAAB"
"ERERIiIzRFXv/////////////kQzIiEREREAAAAAAAAAAAAAAAAAARERESIiM0RVb//////////////lRDMiIREREAAAAAAAAAAAAAAAAAARERESIjNEVW7/"
"/////////////lVEMyIREREQAAAAAAAAAAAAAAAAEREREiIzRFVt///////////////2VUMyIhEREQAAAAAAAAAAAAAAABERERIiIzRVZ+//////////////"
"/9ZUQyIhEREQAAAAAAAAAAAAAAABERERIiM0RWff///////////////9ZVQzIiERERAAAAAAAAAAAAAAARERESIjNEVmf////////////////+ZlQzIiERER"
"AAAAAAAAAAAAAAAREREiIjNFVn3////////////////+dlRDMiERERAAAAAAAAAAAAAAEREREiIzRFZ3//////////////////dlVDMiIREREAAAAAAAAAAA"
"ABERERIiM0RWZ+//////////////////xmVDMiIREREAAAAAAAAAAAABERERIiM0RWd///////////////////12VEMiIREREAAAAAAAAAAAARERESIjNEVm"
"fv//////////////////52VEMyIREREQAAAAAAAAAAARERESIjNFVnf///////////////////52VUMyIhEREQAAAAAAAAAAEREREiIzRFZ37///////////"
"////////53ZUMyIhEREQAAAAAAAAAAERERIiIzRVZ9////////////////////93ZUQyIhEREQAAAAAAAAABERERIiM0RWd+/////////+///////////XZU"
"QzIhEREQAAAAAAAAARERESIjM0Vmff/////////37//////////nZVQzIiERERAAAAAAAAARERESIjNEVnf//////////n7//////////nZlQzIiERERAAAA"
"AAAAEREREiIzRFZn3//////////X3//////////ndlQzIiERERAAAAAAAAEREREiIzRVZ3//////////533//////////3dlRDIiERERAAAAAAABERERIiM0"
"RWZ+//////////133//////////9dlRDMiIRERAAAAAAABERESIiM0VWd//////////+d3f//////////9dlVDMiIREREAAAAAARERESIjNEVnfv////////"
"/9d3f//////////+dmVDMiIREREAAAAAAREREiIjNFVn3//////////nd3f//////////+d2VDMiIREREAAAAAEREREiIzRFZ37//////////Xd3fv//////"
"///+d2VEMyIREREAAAAAERERIiM0RWZ9//////////53d3ff//////////12VEMyIREREAAAABERERIiM0RWd+//////////13d3ff//////////12VUMyIh"
"EREAAAARERESIjNEVmff//////////d3d3ff//////////52ZUMyIhEREAAAARERESIjNFVnf//////////9d3d3d///////////53ZUMyIhEREAAAEREREi"
"IzRFZn7//////////nd3d3d///////////53ZUQzIhEREAAAEREREiIzRVZ3///////////Xd3d3d////////////HZUQzIhEREAABERERIiM0RWd+//////"
"////53d3d3d+///////////XZVQzIiEREAABERERIiM0VWfP//////////13d3d3d+///////////XZVQzIiEREAARERESIjNEVnfv//////////d3d3d3d+"
"///////////ndlQzIiEREAAREREiIjNFVn3//////////9d3d3d3d9///////////3dlRDIiEREAAREREiIzRFZ37//////////////////////////////3"
"dlRDIiEREAEREREiIzRVZ9///////////////////////////////9dlRDMiEREAERERIiM0RWd+///////////////////////////////9dlVDMiEREBER"
"ERIiM0VWff///////////////////////////////+dlVDMiIREBERESIjNEVnf/////////////////////////////////dmVDMiIREBERESIjNFVn7///"
"/////////////////////////////+d2VDMiIREBERESIzRFZ3//////////////////////////////////12VDMiIREREREiIzRVZ+////////////////"
"//////////////////12VEMiIREREREiIzRWbf//////////////////////////////////52VEMiIREREREiM0RWfv////////////////////////////"
"//////92VEMiIRERERIiM0VW3///////////////////////////////////92VEMiIRERERIiM0Vm////////////////////////////////////92VEMy"
"IRERERIjNEVt/////////////////////////////////////WVEMyIRERERIjNEVu///////////nd3d3d3d3d3d3fv///////////WVEMyIRERERIjNFXv"
"///////////Xd3d3d3d3d3d3fv///////////mVEMiIRERERIjNFX///////////93d3d3ZmZ3d3d3fv///////////2VEMiIRERERIjNF7///////////13"
"d2ZmZmZmZmd3fv///////////2VDMiIRERERIjNE///////////+d3ZmVVVVVVVWZnff///////////9VDMiERERERIjNO///////////9dmVVVEREREVVVm"
"bf///////////9RDMiERERERIjPf///////////2ZVVEREREREREVVbP///////////+QzIiERERERIiPv///////////VVUREMzMzMzNERFVf//////////"
"//QzIiERERERIiMzRFVWZmZmZmVVREQzMzMzMzMzRERVVmZmZmZmZVVEMyIhEREREREiIzNERVVVVVVVVERDMzMiIiIiMzM0REVVVWZlVVVURDMyIhEREBER"
"ESIjMzRERERERERDMzMiIiIiIiIiMzM0RERVVVVEREMzIiIREREBERESIiIzMzRERERDMzMyIiIiIiIiIiIjMzM0REREREMzMyIiEREREAERERIiIiMzMzMz"
"MzMyIiIiIiEREiIiIiIjMzMzMzMzMzIiIhERERAAERERESIiIiIjMzIiIiIiIhERERERESIiIiIjMzMzMyIiIiEREREQAAAREREREiIiIiIiIiIiIhERERER"
"ERERESIiIiIiIiIiIiIhEREREQAAABERERERESIiIiIiIRERERERERERERERERIiIiIiIiIREREREREAAAAAERERERERERERERERERERERERERERERERERER"
"ERERERERERERAAAAAAAAAAARERERERESIiIiIiIiIiIiIiIiIiIiIiIREREREREQAAAAAAAAABERERERIiIiIiIiIiIiIiIiIiIiIiIiIiIiEREREREQAAAA"
"AAAAERERERIiIiIiMzMzMzMzMzMzMzMzMyIiIiIiIhEREREQAAAAAAARERERIiIiMzMzMzMzMzMzMzMzMzMzMzMzMyIiIiEREREQAAAAABEREREiIjMzMzRE"
"RERERERERERERERERDMzMzMiIiEREREAAAAAEREREiIjMzRERERFVVVVVVVVVVVVVEREREREQzMzIiEREREAAAABERERIiMzRERVVVVVVVVVVVVVVVVVVVVV"
"VVVEREMzIiERERAAAAEREREiIzNEVVVmZmZmZmZmZmZmZmZmZmZmZVVVREMzIiEREQAAABERESIjNERVZmZ3d3d3d3d3d3d3d3d3d3d2ZmZVVEMzIiEREAAA"
"EREREiM0Rf///////////////////////////////+MyIhERAAABERESIjNEX////////////////////////////////kMyIREQAAERERIiM0RW////////"
"////////////////////////1DMiEREAABERESIjNFXf///////////////////////////////8QzIiERAAEREREiM0RW7/////////////////////////"
"//////9UQyIhEQABERESIjNFVu////////////////////////////////VEMiIREAAREREiM0RWf////////////////////////////////lRDIiERABER"
"ESIjNFVn////////////////////////////////1UQyIhEQAREREiIzRWbf///////////////////////////////9VDMiIREAERERIiNEVn3/////////"
"//////////////////////9VQzIhERABERESIzRFZ+////////////////////////////////VEMyIREQERERIiM0VWf///////////////////////////"
"/////lQzIiEREBERESIjNFZ3////////////////////////////////5EMyIhERAREREiI0RWd////////////nd3d3d3d3d3d3d3d3dmVUQzIiEREQERER"
"IjNEVn3///////////13d3d3d3d3d3d3d3dmZVREMyIhEREREREiIzRVZ+///////////9d3d3d3d3d3d3d3d2ZlVEQzIiERERERERIiM0Vmfv//////////"
"93d3d3d3d3d3d3d3ZmVURDMyIhERERERESIjRFZ3////////////d3d3d3d3d3d3d3dmZVVEMzIiEREREREREiM0RWd////////////nd3d3d3d3d3d3d3Zm"
"VUQzMiIhERERERESIjNEVn3///////////53d3d3d3d3d3d3d2ZlVEMzIiEREREBEREiIzRVZ9///////////9d3d3d3d3d3d3d3d2ZVRDMiIhERERARERIi"
"M0Vmfv////////////////////////////1UQzIiEREREAERESIjNFZ3/////////////////////////////9VEMyIhERERABEREiM0RWd/////////////"
"////////////////9VRDMiIREREQARERIjNEVnf////////////////////////////+VUQzIiERERAAEREiIzRVZ9/////////////////////////////m"
"VEMyIREREQABERIiM0Vmfv////////////////////////////5lQzIiEREREAARESIjNFZ37////////////////////////////9VUMyIhERERAAEREiI0"
"RWd//////////////////////////////VVDMiIREREAABERIjNEVnf/////////////////////////////ZUQzIiERERAAAREiIzRFZ9//////////////"
"///////////////mVEMyIhEREQAAERIiM0VWff////////////////////////////5VRDIiEREREAABESIjNFZn7////////////////////////////+VE"
"MyIhERERAAAREiIzRWd//////////////////////////////VRDMiIREREAAAERIiNEVnf///////////93d3d3d3d3d3d3d3ZmVUQzIiERERAAABESIzRF"
"Z3///////////+d3d3d3d3d3d3d3dmZVRDMyIhEREQAAAREiM0RWff///////////Xd3d3d3d3d3d3d2ZlVUQzIiIREREAAAERIjNFVn3///////////13d3"
"d3d3d3d3d3dmZVREMyIiERERAAABEiIzRVZ+///////////3d3d3d3d3d3d3d2ZmVURDMyIhEREQAAARIiM0Vnf///////////93d3d3d3d3d3d3d2ZlVUQz"
"MiIREREAAAESIjNFZ3///////////+d3d3d3d3d3d3d3d2ZlVEQzIiERERAAABEiIzRWfP///////////nd3d3d3d3d3d3d3d2ZlVEMzIhEREQAAARIiNEVn"
"3///////////13d3d3d3d3d3d3d3d2ZlVEMyIhEREAAAESIjRFZ+////////////////////////////////5DMiIRERAAABEiI0RWfv////////////////"
"///////////////9QzIiEREQAAARIjNEVn////////////////////////////////9UQyIhEREAAAESIzRFZ////////////////////////////////+VE"
"MiIRERAAABEiM0RW3////////////////////////////////lRDIiEREQAAARIjNEVt////////////////////////////////5UQyIhEREAAAESIjRFbv"
"///////////////////////////////9VEMiIRERAAABEiI0RW/////////////////////////////////VQzIiEREQAAARIiM0Vv//////////////////"
"//////////////VUMyIREREAAAERIjNFX////////////////////////////////lRDMiERERAAABESIzRN////////////////////////////////5UMy"
"IhEREAAAAREiIzTv///////////////////////////////+RDMiIRERAAAAEREiM07////////////////////////////////UMyIhEREQAAABERIiM0RV"
"VmZ3d3d3d3d3d3d3d3d3d3d3ZmZlVUQzMiIRERAAAAARERIiM0RFVWZmZmZmZmZmZmZmZmZmZmZmVVVURDMyIhEREQAAAAERESIiMzREVVVVVVVWZmZmZlVV"
"VVVVVVVVVERDMzIiERERAAAAABERESIiMzNEREREVVVVVVVVVVVVVVRERERERDMzIiIREREQAAAAARERESIiIzMzNERERERERERERERERERERDMzMzIiIhER"
"EREAAAAAARERERIiIjMzMzMzMzMzMzMzMzMzMzMzMzMyIiIiEREREQAAAAAAERERERIiIiIiIzMzMzMzMzMzMzMzMzMiIiIiIiERERERAAAAAAAAERERERES"
"IiIiIiIiIiIiIiIiIiIiIiIiIiIhEREREREAAAAAAAAAEREREREREiIiIiIiIiIiIiIiIiIiIiIiIREREREREQAAAAAAAAAAAAAAAAAAAAERERERERERESIi"
"IiIiIiIRERERERERERAAAAAAAAAAAAAAAAABERERERERESIiIiIiIiIiIiIiIREREREREQAAAAAAAAAAAAAAABERERERESIiIiIiIiIiIiIiIiIiIhERERER"
"EAAAAAAAAAAAAAAREREREREiIiIiMzMzMzMzMzMzIiIiIhEREREREAAAAAAAAAAAARERERESIiIiIzMzMzM0RERDMzMzMyIiIhEREREQAAAAAAAAAAARERER"
"ESIiIjMzM0REREREREREREMzMyIiIhEREREAAAAAAAAAAREREREiIiMzM0RERFVVVVVVVVVUREQzMyIiIRERERAAAAAAAAAREREREiIjMzRERVVVVmZmZmZm"
"ZlVVVERDMyIiEREREQAAAAAAARERERIiIjM0RFVVZmZmd3d3d3d2ZmZVVEQzMiIhERERAAAAAAARERERIiIzNERVVmZnd93u7////u7ddmZlVEQzMiIREREQ"
"AAAAARERERIiIzNEVVZmd97/////////////7WZlVEMzIiERERAAAAABERERIiIzREVWZ37//////////////////WZVRDMyIhEREQAAABERERIiIzRFVmfO"
"/////////////////////WVUQzIiERERAAAAERERIiIzRFVmfv///////////////////////FVEMyIhEREAAAEREREiIzRFVn7/////////////////////"
"////1VRDMiEREQAAAREREiIzRFVn7//////////////////////////9VUMyIhERAAAREREiIzNFVn////////////////////////////9lRDIiEREAABER"
"ESIjNFVn/////////////////////////////+VUMyIREQABERESIjNEVm7//////////////////////////////FQzIiERAAERERIiM0VW7///////////"
"///////////////////9VEMiIREAAREREiM0RWf///////////////////////////////5UQyIhEQABEREiIzRVbv////////////7Xd3d97///////////"
"/1RDMiERABERESIjRFZ////////////+d3d3d3d9////////////VEMyIREAERERIjNEVt///////////+d3d3d3d3fv//////////5UQzIhEQARERIiM0Vm"
"7///////////13d3d3d3d8///////////lRDIiERABEREiIzRWf////////////Hd3d3d3d3fv/////////+VDMiIREAERESIjRFbf///////////+d3d3d3"
"d3d///////////5EMyIhEQERERIiNEVt/////////////Xd3d3d3d3d3d3d3d3dmVUQzIiERAREREiM0RW7//////////////Xd3d3d3d3d3d3d3dmVUQzIi"
"IREBERESIzRVbv///////////////+13d3d3d3d3d3d2ZVRDMiIREQERERIjNFVu///////////////////t13d3d3d3d2ZVRDMyIhERAREREiM0VW7/////"
"/////////////////td3d3d3ZlVEMzIiEREBERESIzRFbv///////////////////////+13d3dmVUQzIiIREQERERIjNEVt////////////////////////"
"//53d3ZVRDMiIhERAREREiI0RWf///////////////////////////53dmVEMyIhERERERESIjNFZt////////////////////////////12ZUQzIiERERER"
"ERIiM0VWfv///////////////////////////9dlVEMyIhEREREREiIzRVZ8/////////////////////////////WZUQzIiEREREREiIjNEVmff////////"
"////////////////////dlRDMiIRERERESIiM0RWZ33////////////////////////////mVUMyIhERERERIiMzRFVnd37/////////////////////////"
"//ZlQzIiEREREREiIzNEVmd3d37//////////////////////////WVEMiIREREREiIjNEVWZ3d3d33v///////////////////////+ZUQyIhERERESIjM0"
"RVZnd3d3d3fe//////////////////////5lRDMiERERERIiM0RVZnd3d3d3d3d33v///////////////////mVEMyIRERERIiMzRVZnd3d3d3d3d3d3d97/"
"///////////////+ZUQyIhEREREiIzRFVmd3d3d3d3d3d3d3d3fe//////////////1lRDIiERERESIjNF7//////////3d3d3d3d3d+/////////////WVE"
"MiIRERERIjNEX///////////d3d3d3d3d3fv///////////3ZUQyIhEREREiM0Xf//////////93d3d3d3d3d9////////////dlQzIiEREREiIzRd//////"
"/////nd3d3d3d3d33///////////5mVDMiERERESIjRF3///////////13d3d3d3d3fv///////////WVUMyIRERERIiNEXf///////////9d3d3d3d3fv//"
"/////////3ZUQyIhEREREiI0Rd/////////////td3d3d97////////////+ZVQzIiERERESIjRFb/////////////////////////////////dlRDMiERER"
"EREiM0Vv////////////////////////////////5lVDMiIRERERESIzRV7///////////////////////////////52VEMyIhERERERIjNEXf//////////"
"/////////////////////WVUMyIhEREQEREiI0RW7//////////////////////////////WVUQzIiERERARESIjNFXf////////////////////////////"
"/WZUQzIiEREREBEREiM0RW7////////////////////////////WZUQzIiEREREAERESIjNEVu//////////////////////////52ZUQzMiIREREAAREREi"
"IzRVbf////////////////////////12VUQzMiIREREQABERESIjNEVWbv/////////////////////+dmVUQzMiIREREQAAEREREiIzREVWbf//////////"
"/////////XZmVUQzMiIRERERAAABERERIiMzRFVmZ97//////////////td2ZlVEQzMiIRERERAAAAAREREiIjM0RVVmZ3fd7u/////+7dd3ZmVVREMzIiIR"
"ERERAAAAABERERIiIzNERVVWZmZ3d3d3d3d2ZmZVVURDMzIiIRERERAAAAAAARERESIiIzNERFVVVmZmZmZmZmZVVVREQzMyIiIREREREAAAAAABEREREiIi"
"MzM0RERVVVVVVVVVVVREREMzMiIiIREREREAAAAAAAAREREREiIiMzMzREREREREREREREMzMzIiIiEREREREAAAAAAAAAEREREREiIiIzMzMzNEREREQzMz"
"MzMiIiIhERERERAAAAAAAAAAABEREREREiIiIiMzMzMzMzMzMzMiIiIiIRERERERAAAAAAAAAAAAAREREREREiIiIiIiIiIyIiIiIiIiIhERERERERAAAAAA"
"AAAAAAAAARERERERESIiIiIiIiIiIiIiIiEREREREREQAAAAAAAAAAAAAAAAEREREREREREiIiIiIiIiIiEREREREREREQAAAAAAAAAAAAAAAAAAAAARERER"
"EREREREiIiIiIiIREREREREREQAAAAAAAAAAAAAAAAERERERERESIiIiIiIiIiIiIiEREREREREAAAAAAAAAAAAAABEREREREiIiIiIiIiIiIiIiIiIiERER"
"EREQAAAAAAAAAAAAARERERESIiIiIjMzMzMzMzMzIiIiIhERERERAAAAAAAAAAAAERERERIiIiIzMzMzM0REQzMzMzMiIiIREREREAAAAAAAAAABERERESIi"
"IzMzNEREREREREREQzMzIiIiEREREQAAAAAAAAARERERIiIjMzRERFVVVVVVVVVVRERDMzIiIRERERAAAAAAAAERERESIiMzNERFVVVmZmZmZmZVVVREQzMi"
"IhERERAAAAAAABEREREiIjM0RFVWZmZnd3d3d3ZmZlVVRDMyIiEREREAAAAAABERERIiMzREVVZmd93u/////u7dd2ZlVEQzMiIREREQAAAAARERESIjM0RV"
"Zmfv/////////////+1mZVRDMyIhEREQAAAAAREREiIzREVWbv/////////////////+ZlVEMyIhEREQAAAAERERIiM0RVZu/////////////////////mVU"
"QzIiERERAAAAERESIjM0VW3///////////////////////1VRDMiERERAAABERESIjNEVu/////////////////////////VVDMiIRERAAABERESIzRFX///"
"///////////////////////mVEMyIRERAAAREREiIzRV7//////////////////////////9VUMyIhERAAAREREiM0Re///////////////////////////+"
"ZUQyIhERAAERERIiM0Tf////////////////////////////ZUQzIhERAAERERIiM0X/////////////////////////////ZVQzIhERAAERERIiM0X/////"
"//////7Xd3d+////////////ZlQzIiERABERESIjNE3//////////8d3d3d3f///////////dlQzIiERABERESIjNE7//////////Xd3d3d3ff//////////"
"dlQzIiERARERESIjNEVWd3d3d3d3d3d3d3d3ff//////////dlRDIiERARERESIjNEVWd3d3d3d3d3d3d3d3ff/////////+dlRDIiERAREREiIjNEVmd3d3"
"d3d3d3d3d3d3fv/////////+dlRDIiEREREREiIzNFVmd3d3d3d3zd7u7//////////////9dlRDIiEREREREiIzRFVnd3d3fe/////////////////////8"
"dlQzIiERERERIiMzRFZnd3fv///////////////////////3dlQzIiERERERIiM0RVZ3ff/////////////////////////ndlQzIiERERESIjM0VWZ9////"
"///////////////////////nZlQzIiERERESIjNEVmfv///////////////////////////XZlQzIiERERESIzNFVn7////////////////////////////X"
"ZVQzIiEREREiIzRFZu////////////////////////////93ZVQzIiEREREiIzRVbv/////////////td3d8//////////93ZVQzIiEREREiM0RWf///////"
"/////Xd3d3d9//////////53ZVQzIiERERIiM0VW7//////////9d3d3d3d+//////////53ZUQzIiERERIiM0Vm///////////Xd3d3d3d+//////////13"
"ZUQzIiERERIiNEVt//////////13d3d3d3d///////////13ZVQzIiERERIjNFVu//////////d3d3d3d3ff//////////d3ZVQzIiERERIjNFVv////////"
"//d3d3d3d3fv//////////d3ZVQzIiERERIjNFbf//////////d3d3d3d3////////////d3ZlQzIiERERIjNFbf//////////13d3d3ff////////////13"
"ZlQzIiERERIjNFbv///////////nd3d+/////////////////lRDIiERERIjNFXv/////////////////////////////////lRDIiERERIjNFXv////////"
"/////////////////////////VRDIiERERIjNEXf/////////////////////////////////VQzIiERERIiNEXP////////////////////////////////"
"9lQzIiEREREiM0Vf////////////////////////////////5VQzIhEREREiM0Rd/////////////////+d/////////////5UQzIhEREREiIzRV3///////"
"/////////Xd9////////////5UQyIhERERESIzRFXf/////////////9d3d33///////////1EMyIhERERESIjNEVW3//////////+d3d3d3d+/////////+"
"VDMiIRERERERIiM0RVVm3u///+7dd3ZmZmZmZnfe7///7tVVRDMiIRERERERIiIzREVVZmZmZmZmZmZlVVVVVmZmZmZmZVVEQzIiEREREREREiIjM0RFVVVm"
"ZmZVVVVVVERFVVVVVVVVVURDMyIhEREQARERESIiMzNEREVVVVVVRERERERERERFVVREREMzMiIhEREQABERERIiIjMzNEREREREREMzMzMzM0REREREMzMy"
"IiEREREAAAERERESIiIzMzMzMzMzMzMzMzMzMzMzMzMzMzIiIhEREREAAAEREREREiIiIjMzMzMzMyIiIiIiIiMzMzMyIiIiIRERERAAAAARERERERIiIiIi"
"IiIiIiIiIiIiIiIiIiIiIiIhEREREQAAAAABERERERESIiIiIiIiIiIiIiIiIiIiIiIiIREREREREAAAAAAAAREREREREREREiEhERERERERERERERERERER"
"ERERAAAAAAAAAAAAAAARERERERERERESIiIiIhEREREREREREAAAAAAAAAAAAAAAERERERERERIiIiIiIiIiIiIhERERERERAAAAAAAAAAAAAAEREREREREi"
"IiIiIiIiIiIiIiIiERERERERAAAAAAAAAAAAERERERESIiIiIjMzMzMzMzMiIiIiEREREREQAAAAAAAAABEREREREiIiIjMzMzMzMzMzMzMzIiIiEREREREA"
"AAAAAAABERERERIiIiMzMzREREREREREQzMzMiIiERERERAAAAAAABERERESIiIjMzREREVVVVVVVVRERDMzMiIhEREREAAAAAABERERESIiIzM0REVVVVZm"
"ZmZlVVVUREMzIiIRERERAAAAABEREREiIiMzNERVVWZmZmZ3ZmZmZlVUREMzIiEREREQAAABEREREiIiMzREVVZmZ93u////7u12ZmVURDMyIhERERAAAAER"
"EREiIjM0RFVmZt7////////////9ZmVUQzMiIREREQAAEREREiIjM0RVVmfv///////////////+1lVEMzIiERERAAEREREiIjM0RVZn7///////////////"
"///+ZVRDMiIREREAERERESIjNERVZu/////////////////////9ZUQzIiEREQARERESIjNERWbf///////////////////////lVEMyIRERARERESIjM0RW"
"bf////////////////////////1VQzIiEREBERESIiM0RWbv/////////////////////////9VEMyIRERERERIiM0RWbv//////////////////////////"
"5VQzIhERERERIiM0RVbf///////////////////////////2VDMiIREREREiIzRVZ/////////////////////////////5UQyIhEREREiIzRFZ/////////"
"///+d3d33v///////////lRDIiERERESIjNFZt///////////nd3d3d33//////////+VEMyIRERESIjNEVn///////////Xd3d3d3d9//////////9UQzIh"
"ERERIiM0Vm7//////////nd3d3d3d3fv/////////1RDMiEREREiM0RWf//////////3d3d3d3d3d+//////////VEMiIREREiIzRVbf/////////+d3d2Zm"
"Zmd37/////////9UMyIhERESIjRFZ+//////////d3d2ZmZmZmf//////////kQzIiERERIjNEVn//////////53d2ZlVVVmZnx3d3d3dmbFRDMiERERIiM0"
"VW3//////////nd3ZlVVVVZmZnd3d3dmZVRDMiIREREiIzRWfv/////////9d3dmVVVVVWZmZ3d3ZmZVREMyIhERESIjRFZ///////////d3dmVVVVVVVmZm"
"ZmZmZVVEMzIiERERIiNEVn//////////93d2ZVVVVVVWZmZmZmZlVEQzIiIREREiM0RWf//////////nd3ZlVVVVVVZmZmZmZmVURDMiIRERESIzRVbf////"
"/////+d3dmVVVVVWZmZnd2ZmZVRDMyIhERERIjNFVt//////////13d2ZlVVVWZmd3d3d2ZlVEQzIiEREREiM0VW7//////////Xd3ZmZVVmZm3d3d3d3d3d"
"QzMiIRERESIzRVbv/////////9d3d2ZmZmZnf/////////9EMyIhERERIjNFVu//////////13d3dmZmZ3ff/////////kQzIhEREREiM0RW3//////////n"
"d3d3d3d3d+/////////9RDMiERERESIjRFbf/////////+d3d3d3d3d9//////////VEMyIRERERIiNEVt///////////Xd3d3d3d3//////////5UQyIhER"
"EREiIzRWb///////////13d3d3d9///////////VQzIiERERESIjNFVv///////////+13d3ff///////////lRDMiIRERAREiM0RW3/////////////////"
"///////////2VEMyIREREBESIjNFZv///////////////////////////9VUMyIhEREQERIiM0RW7//////////////////////////+ZUQzIhEREQARESIj"
"NFVv/////////////////////////+ZUQzIiERERABERIiM0RW3////////////////////////+ZVQzMiEREREAERESIjNFVt//////////////////////"
"/9ZVRDMiIREREAARERIiM0RVbf/////////////////////mZVRDMiIREREQABERESIjNEVWbv///////////////////GVVRDMiIREREQAAEREREiIzRFVm"
"zv///////////////+ZmVURDMiIhERERAAABERESIiM0RFVmfv////////////7WZlVUQzMiIhERERAAAAEREREiIjM0RVVmZ93u////7u3HZmZVVEQzMiIh"
"ERERAAAAABERERIiIzNERVVWZmZnd3ZmZmZlVVREMzMiIhEREREAAAAAARERESIiMzNERFVVVmZmZmZVVVVERDMzIiIhEREREAAAAAABEREREiIiMzNERERV"
"VVVVVVREREQzMyIiIREREREAAAAAAAAREREREiIiMzM0REREREREREQzMzMiIiIREREREAAAAAAAAAERERERIiIiIzMzMzMzMzMzMzMyIiIiEREREREAAAAA"
"AAAAABERERERIiIiIjMzMzMzMzMyIiIiIhEREREREAAAAAAAAAAAAREREREREiIiIiIiIiIiIiIiIiEREREREREAAAAAAAAAAAAAARERERERESIiIiIiIiIi"
"IiIRERERERERAAAAAAAAAAAAAAAAERERERERERESIiIiIhERERERERERERAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABEREREREREREREREREREAAAAA"
"AAAAAAAAAAAAAAAAAAAAAAABERERERERESIhEREREREREAAAAAAAAAAAAAAAAAAAAAAAAAAAERERERESIiIiIiIiEREREREAAAAAAAAAAAAAAAAAAAAAAAAA"
"AREREREiIiIiIiIiIiIhEREREAAAAAAAAAAAAAAAAAAAAAAAAREREREiIiIzMzMzMzIiIhERERAAAAAAAAAAAAAAAAAAAAAAABERERESIiIzMzNEQzMzMiIh"
"ERERAAAAAAAAAAAAAAAAAAAAAAERERERIiIzM0REREREQzMiIhEREQAAAAAAAAAAAAAAAAAAAAEREREREiIjM0RFVVVVVERDMyIhEREAAAAAAAAAAAAAAAAA"
"AAERERERESIiM0RFVWZmZlVURDMiIRERAAAAAAAAAAAAAAAAAAERERERERIiIzRF//////////9DMiIREQAAAAAAAAAAAAAAAAEREREREREiIjNEVf//////"
"////RDIiEREAAAAAAAAAAAAAABERERERERESIiM0RV3//////////kQzIhERAAAAAAAAAAAAABERERERERESIiIzNEVu//////////5UMyIhEQAAAAAAAAAA"
"ABERERERERERIiIjM0RVbv/////////9VEMiIREAAAAAAAAAABERERERERESIiIiMzRFVn7//////////VRDMiERAAAAAAAAABEREREREREiIiIiIzM0RWZ/"
"//////////ZVQzIhEQAAAAAAABERERERERIiIiIiIzMzRFVn3//////////mVUMyIREAAAAAAAERERERERIiIiIjMzMzREVWZ9//////////5lVDMiERAAAA"
"AAARERERESIiIiMzMzMzRERVZnfv/////////9ZVQzIhEQAAAAABERERESIiIjMzMzRERERVVmd37//////////WVUMyIREAAAAAERERERIiIjMzNERERVVV"
"VWZ3d///////////dlVDMiERAAAAARERERIiIjMzRERVVVVVZmZnd3f//////////3ZVQzIhEQAAABEREREiIjMzREVVVmZmZmZ3d3d9//////////52VEMy"
"IREAAAERERESIiMzREVVZmZ3d3d3d3d3ff/////////+dlRDMiERAAARERERIiIzNEVVZmfe7//+7Xd3d37//////////XZUQyIhEQAAEREREiIjNERVZn7/"
"///////+d3d+//////////12VEMiIREAARERESIiM0RVZn7///////////53f//////////3dlQzIiERABERERIiIzRFVm7/////////////98//////////"
"93ZUMyIhEQARERESIjNEVWbv//////////////7v/////////+dlVDMiIREBERERIiM0RVZ////////////////////////////XZVQzIiERAREREiIzNFVn"
"////////////////////////////12VEMyIRERERERIiM0RWbv///////////////////////////3dlRDMiEREREREiIzRFZu//////////////////////"
"//////93ZUQyIhERERERIiM0VW3////////////////////////////+d2VDMiIREREREiIzRFZ//////////////////////////////nZlQzIiERERERIi"
"M0Vm3/////////////////////////////12VUMyIhEREREiIzRFZ////////////+d3d97////////////9dlRDMiIRERERIiM0Vm3///////////d3d3d3"
"7///////////93ZUQzIhERERESIzRFZ+//////////93d3d3d37///////////d2VEMiIRERERIiM0VWz//////////3d3d3d3d9///////////nZlQzIiER"
"ERESIjRFZ+//////////53d3d3d3d///////////52VUMyIhEREREiM0RWf//////////8d3d3d3d3f//////////9dlVDMiIRERESIjNFVn//////////53"
"d3d3d3d3///////////HZUQzIhEREREiIzRWbf/////////+d3d3d3d3d///////////d2VEMyIRERERIiNEVn7//////////Xd3d3d3d3f//////////ndl"
"QzIiERERESIzRFZ///////////d3d3d3d3d9//////////52ZUMyIhEREREiM0VWf//////////3d3d3d3d3ff/////////9dlVDMiIRERERIjNFVn//////"
"////53d3d3d3d37//////////XZUQzIiEREQEiIzRWbf/////////+d3d3d3d3d+//////////d2VEMyIREREBIiM0Vn3//////////Xd3d3d3d3f///////"
"///3dlQzIiERERASIjNFZ+//////////13d3d3d3d9//////////52ZUMyIhEREQEiIzRWfv/////////9d3d3d3d3f//////////+dlVDMiIREREBIiM0Vn"
"7//////////Xd3d3d3d9///////////XZUQzIhEREQASIjNFZ+//////////53d3d3d3f///////////x2VEMiIREREAESIzRWbv//////////d3d3d3d+//"
"/////////3ZlQzIiERERABEiM0VW7//////////+d3d3d37///////////52VUMyIhEREQARIjNFVu////////////13d37////////////+dlRDMiIREREA"
"ESIzRFbf/////////////////////////////XZUQyIhEREQABEiI0RWz/////////////////////////////1mVDMiIREREAARIiM0VW//////////////"
"///////////////3ZVQzIiERERAAERIjNEVu////////////////////////////92VEMyIREREQABESIjNFbf///////////////////////////+ZlQzIi"
"ERERAAAREiIzRFbv/////////////////e/////////mVUMyIhEREQAAEREiI0RVb////////////////9fv////////1lRDMiERERAAABERIiM0RW3/////"
"//////////137////////9VUMyIhEREQAAARERIiM0RW3/////////////93d+////////9VRDMiIREREAAAERESIjM0VWb////////////Xd3fv///////+"
"VEMyIhEREQAAABERESIjNERVbf/////////Xd3d33////////kQzIiEREREAAAARERESIjM0RVZm3u///u13dmZmZmZmZmZmVVRDMyIhEREQAAAAAREREiIj"
"M0RFVWZmZmZmZmZVVVVVVVVVVVREMzIiEREREAAAAAEREREiIjM0REVVVVZlVVVVVVRERVVVVEREMzMiIREREQAAAAAAERERESIiMzNEREVVVVVERERERERE"
"REREMzMiIhERERAAAAAAAAERERESIiIzMzREREREREMzMzMzMzMzMzMiIiEREREQAAAAAAAAERERESIiIjMzMzMzMzMzMzMzMzMzMyIiIiERERERAAAAAAAA"
"AAERERERIiIiIjMzMzMzIiIiIiIiIiIiIiIREREREAAAAAAAAAAAEREREREiIiIiIiIiIiIiIiIiIiIiIiEREREREQAAAAAAAAAAAAEREREREREiIiIiIiIi"
"IiIiIiIhEREREREREQAAAAAAAAAAAAAAERERERERERERERERERERERERERERERERERAAAAAAAAAAAAAAAAAAAAARERERERERERESIiIiIhEREREREREREAAA"
"AAAAAAAAAAAAERERERERESIiIiIiIiIiIiIhERERERERAAAAAAAAAAAAAAERERERERIiIiIiIiIiIiIiIiIiEREREREQAAAAAAAAAAABERERERESIiIiIzMz"
"MzMzMzMiIiIiIREREREQAAAAAAAAABEREREREiIiIzMzMzMzMzMzMzMzIiIiEREREREAAAAAAAABERERERIiIiMzM0RERERERERERDMzMiIiERERERAAAAAA"
"ABERERESIiIzMzRERFVVVVVVVVVEREMzMiIhEREREAAAAAABERERESIiIzNERFVVVWZmZmZmZVVUREMzIiIhERERAAAAABEREREiIiMzREVVVmZmZ3d3d3Zm"
"ZmVVREMzIiIREREQAAABEREREiIjMzRFVWZmd93u////7u13ZmVVRDMyIiEREREAAAEREREiIjM0RFVmZ97////////////tdmVURDMiIREREQAAEREREiIj"
"M0RVZmfv///////////////+dmVEQzIiERERAAEREREiIjNERVZn7//////////////////+ZlVDMyIhEREAEREREiIjNEVWZ+/////////////////////8"
"ZUQzIiEREQARERESIjNEVWbf///////////////////////WVEMyIhERARERESIjNEVWbf////////////////////////ZVRDMiEREBERESIjM0VWbv////"
"/////////////////////mVUMyIhERERERIiM0RWbv//////////////////////////1lRDIiERERERIiM0RWbf///////////////////////////2VEMy"
"IREREREiI0RVZ/////////////////////////////ZlQzIiEREREiIzRFZ////////////+d3d3zv///////////WVEMiIRERESIzRFZt///////////Xd3"
"d3d3f//////////+ZUQzIhERESIjNFVn///////////Xd3d3d3d9//////////5lVDMiERERIiNEVm7//////////Xd3d3d3d3fv/////////2ZUMyIREREi"
"M0RWf//////////nd3d3d3d3d+//////////ZlQzIhEREiIzRWbf/////////9d3d3d3d3d37/////////9mVDMiIRESIjRFZ+//////////////////////"
"/////////nZUMyIhERIjNFVn///////////////////////////////+ZlQzIiERIiM0Vm3///////////////////////////////5mVDMiEREiIzRWfv//"
"/////////////////////////////mVUMyIRESIjRFZ////////////////////////////////+ZUQzIhERIiNEVn//////////////////////////////"
"//1lRDIiEREiM0RWf////////////////////////////////WVDMiIRESIzRVbf///////////////////////////////2VUMyIhERIjNFVt//////////"
"13d3d3d3d3d3d3d3d3d3d3ZUQzIiEREiM0VW7//////////Xd3d3d3d3d3d3d3d3d3d3ZlRDMiERESIzRVbv/////////9d3d3d3d3d3d3d3d3d3d3dlVDMi"
"IRERIjNFVu//////////13d3d3d3d3d//////////9VUMyIhEREiM0VW3//////////Xd3d3d3d3d9//////////ZUQzIiERESIjRFbf/////////+d3d3d3"
"d3d3//////////5VRDIiERERIiNEVt///////////Xd3d3d3d37//////////VVDMiIREREiIzRWf///////////13d3d3d3///////////2VEMyIhERESIj"
"NFVv///////////+13d3fe///////////+VUQyIhEREREiM0RW3/////////////////////////////ZUQzIiERERESIjNFZv//////////////////////"
"//////1lRDMiERERERIiM0RW7///////////////////////////5lRDMiIRERERESIjRFVv//////////////////////////5lVDMyIRERERERIiM0RW3/"
"////////////////////////5lVEMyIhEREQERESIjNFVt////////////////////////5lVEMyIhERERARERIiM0RVbf//////////////////////1lVE"
"MyIiERERABERESIjNEVWbv///////////////////9ZlVEMzIiEREREAEREREiIzRFVmfv/////////////////WZlVEMzIiEREREAABERESIiM0RFVmbe//"
"///////////tdmZVREMzIiEREREAAAEREREiIjM0RVVmZ33u7////+7dd2ZmVURDMyIiEREREQAAABERERIiIzNERVVWZmZ3d3d3dmZmZVVERDMyIiEREREQ"
"AAAAARERESIiIzNERFVVVmZmZmZmVVVUREQzMiIiEREREQAAAAABERERESIiMzNERERVVVVVVVVURERDMzIiIiEREREQAAAAAAAREREREiIiMzMzRERERERE"
"RERDMzMyIiIhEREREQAAAAAAAAEREREREiIiIzMzMzMzMzMzMzMzIiIiIREREREQAAAAAAAAABERERERIiIiIiMzMzMzMzMzIiIiIiEREREREQAAAAAAAAAA"
"AREREREREiIiIiIiIiIiIiIiIiIhEREREREQAAAAAAAAAAAAARERERERESIiIiIiIiIiIiIiEREREREREAAAAAAAAAAAAAAAERERERERERESIiIiIiIRERER"
"EREREREAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEREREREREREREREREREREREQAAAAAAAAAAAAAAAAAAAAAAAAAAAAABERERERERERERERERERERER"
"EREREAAAAAAAAAAAAAAAAAAAAAAAAAARERERERERERERERESIiIiIiIREREREQAAAAAAAAAAAAAAAAAAAAAAEREREREREREREREREiIiIiIiIiIiERERERAA"
"AAAAAAAAAAAAAAAAAAAREREREREREREREREiIiIiIjMzMyIiIhERERAAAAAAAAAAAAAAAAAAAREREREREREREREREiIiIiMzMzMzMzMyIiEREREAAAAAAAAA"
"AAAAAAABERERERERERERESIiIiIiIzMzNERERDMzIiIREREAAAAAAAAAAAAAAAERERERERERESIiIiIiIiIzMzREREVURERDMiIhEREAAAAAAAAAAAAAABER"
"EREREREiIiIiIiIiIzMzNERFVVVVVVREMyIhEREQAAAAAAAAAAAAEREREREREiIiIiIiMzMzMzNERFVV7////////jIiEREQAAAAAAAAAAABEREREREiIiIi"
"IzMzMzMzREREVVVm/////////TIiEREQAAAAAAAAAAARERERESIiIiMzMzM0REREREVVVmZ/////////4zMiEREQAAAAAAAAAAERERERIiIiMzMzREREREVV"
"VVVmZnfv////////5DMiEREQAAAAAAAAABERERESIiIzM0RERFVVVVVWZmZnd37/////////RDMiEREQAAAAAAAAARERERIiIjMzRERVVVVmZmZmZ3d3d///"
"///////9RDMiEREQAAAAAAAAARERESIiMzNERVVWZmZnd3d3d3d37//////////lRDIiEREQAAAAAAAAEREREiIjM0RFVWZmfN7u/////u7f//////////5V"
"QzIiEREQAAAAAAABERERIiIzREVWZn3v/////////////////////9ZUQzIiEREQAAAAAAABERESIiM0RVVm3///////////////////////52VUQzIhEREQ"
"AAAAAAARERESIjNEVWbv///////////////////////XdmVEMyIhEREQAAAAAAAREREiIzRFVt///////////////////////3d3dmVEMyIhEREQAAAAAAER"
"ERIiM0RVbv///////////////////////+d3dlVDMiIREREAAAAAAAERERIiM0VW7/////////////////////////53dlRDMiIREREAAAAAABERESIjNEVt"
"///////////////////////////HZlRDMiIREREAAAAAABERESIjNFXP///////////sd3d+///////////nZVQzIiEREREAAAAAABERESIzRFbv////////"
"//d3d3d33//////////3ZVQzIiEREREAAAAAAREREiIzRVb//////////3d3d3d3f//////////3ZUQzIiERERAAAAAAAREREiI0RW3//////////Xd3d3d3"
"fv/////////8ZUQzIiERERAAAAAAAREREiM0RW7/////////53d3d3d3ff/////////8ZUQyIhERERAAAAAAARERIiM0VW//////////53d3d3d3fv//////"
"///2ZUMyIhERERAAAAAAERERIiM0Vm//////////53d3d3d3fv/////////mVUMyIhEREQAAAAAAERERIiM0Vn//////////53d3d3d33//////////mVEMy"
"IhEREQAAAAAAERERIiNEVn//////////93d3d3d3//////////92VEMyIREREQAAAAABERERIjNEVn///////////Xd3d3d///////////5lVDMiIREREAAA"
"AAABERESIjNEVn3//////////+d3d37///////////1lRDMiIREREAAAAAABERESIjNEVnfv/////////////////////////9ZVRDMiEREREAAAAAABERES"
"IjNEVnff/////////////////////////mZUQzIiERERAAAAAAARERESIjNFVnd9////////////////////////52VUQzIiERERAAAAAAAREREiIzRFZnd3"
"3//////////////////////+dlVEMyIhERERAAAAAAAREREiIzRFZ3d3ff////////////////////93ZlRDMyIhEREQAAAAAAEREREiIzRVZ3d37///////"
"/////////////9d2ZVRDMiIhEREQAAAAAAERERIiM0RWZ3fv///////////////////+d3dmVUQzMiIREREQAAAAAAERERIiM0RWd9/////////////////+"
"7dd3d3dmVUQzIiIREREQAAAAAAERERIiM0Vmfv/////////nd3d3d3d3d3d3d3ZlVEMzIiIREREQAAAAABERESIjNEVn7/////////13d3d3d3d3d3d3d3Zl"
"VEMzIiIREREQAAAAABERESIjNFVt/////////+d3d3d3d3d3d3d3d3ZlVEMzIiIREREQAAAAABERESIzRFZ+/////////9d3d3d3d3d3d3d3d3ZlVEQzIiIR"
"EREQAAAAAREREiIzRFbf/////////+d3d3d3d3d3d3d3d3dmVUQzMiIREREQAAAAAREREiIzRVbv//////////53d3d3d3d3d3d3d3dmVURDMiIREREQAAAA"
"AREREiIzRWf/////////////////////////7td2ZVRDMiIhEREQAAAAEREREiM0RWf////////////////////////////tZlVEMyIhEREQAAAAERERIiM0"
"RWf/////////////////////////////5mVEMyIhEREQAAAAERERIiM0VWfv/////////////////////////////mVUQyIhEREQAAAAERERIiM0VWfv////"
"/////////////////////////+ZUQzIhEREQAAAAERESIjNEVnd///////////////////////////////1VQzIiEREQAAAAERESIjNEVnd9////////////"
"//////////////////5lQzIiEREQAAAAERESIjNFVnd37/////////////////////////////5lRDIiEREQAAAAERESIzRFZnd37///////////////////"
"///////////FRDMiEREQAAAAEREiIzRFZ3fv///////////////////////////////FVDMiERERAAAAEREiIzRVZ3//////////////////////////////"
"///VVDMiERERAAAAEREiM0RWd//////////nd3d3d3d3d3fv///////////VVDMiERERAAAAERIiM0VWf/////////13d3d3d3d3d3d+//////////9lVDMi"
"ERERAAAAERIiM0Vm7////////9d3d3d3d3d3d3d9//////////9lRDMiEREQAAAAERIiNEVu/////////3d3d3d3d3d3d3d9//////////5lRDIiEREQAAAA"
"ERIjNEVv/////////3d3d3d3d3d3d3d9//////////5lQzIiEREQAAAAERIjNFXf/////////3d3d3d3d3d3d3d///////////ZlQzIiEREQAAAAERIjNFXv"
"/////////+d3d3d3d3d3d3fv/////////+ZUQzIhEREQAAAAERIjNFXv//////////53d3d3d3d3d9///////////9ZUMyIhEREQAAAAERIjNFXv////////"
"/////////////////////////mVEMyIhEREAAAAAERIjNEXv////////////////////////////////9lVEMiIREREAAAAAERIiNEXv////////////////"
"////////////////ZlRDMiIREREAAAAAERIiM0Xf///////////////////////////////2ZUQzIiERERAAAAAAEREiM0Ve////////////////////////"
"//////5mVUMzIiERERAAAAAAEREiI0Rc/////////////////////////////9ZlVEMyIhERERAAAAAAEREiIzRF3///////////////////////////1mVU"
"RDMiIREREQAAAAAAERESIjNEVu////////////////////////7WZlVEMzIiIREREQAAAAAAERESIjM0RVbu///////////////////u12ZmVURDMyIiERER"
"EAAAAAAAERERIiMzRFVWZmZ3d3d3d3d3d3d3d3ZmZmVVREMzMiIhERERAAAAAAAAEREREiIzNERVVWZmZmZmZmZmZmZmZmZVVVVERDMyIiEREREQAAAAAAAA"
"ARERESIjMzRERVVVVVVVVVVVVVVVVVVURERDMzIiIhEREREQAAAAAAAAARERERIiIzMzRERERERERERERERERERERDMzMiIiIREREREAAAAAAAAAABEREREi"
"IiMzMzM0REREREREREREQzMzMzMyIiIhERERERAAAAAAAAAAAAERERERIiIiMzMzMzMzMzMzMzMzMzMzIiIiIiEREREREQAAAAAAAAAAAAARERERESIiIiIi"
"IiMzMzMzMyIiIiIiIiIiIREREREREAAAAAAAAAAAAAABEREREREiIiIiIiIiIiIiIiIiIiIiIiEREREREREQAAAAAAAAAAAAAAAAERERERERERIiIiIiIiIi"
"IiIiIiIREREREREREREAAAAAAAAAAAAAAAAAAAAAABEREREREREREREREREQAAAAAAAAAAABEREREREREREREREREREAAAAAAAAAABEREREREiIiIiIhERER"
"ERAAAAAAAAABERERESIiIiIiIiIiEREREQAAAAAAABEREREiIiIzMzMyIiIhEREREAAAAAAAEREREiIjMzMzMzMzIiIhEREQAAAAAAEREREiIzMzRERERDMz"
"IiIREREAAAAAEREREiIzNERFVVRERDMyIhEREQAAAAAREREiIzNEVVVVVVVUQzMiIRERAAAAAREREiIzNO//////////0zIhEREAAAABERESIjNE7///////"
"//9DMiIREQAAAAERERIjNEz//////////0MyIhERAAAAERERIiM0Tf/////////+RDMiEREAAAAREREiI0Re//////////5EMyIREQAAARERESIzRF7/////"
"/////UQzIhERAAABERESIjNEXv/////////1RDMiEREAAAERERIiM0Vf//////////VEMyIREQAAEREREiIzRd//////////9UQzIhERAAAREREiIzRF3///"
"///////lRDMiEREAABERESIjNEXv/////////9VEMyIREQABERERIiM0RVZnd3d3d3dmVUMyIhERAAEREREiIzRFVnd3d3d3dmVUQzIiEREAAREREiIzRFVm"
"d3d3d3d2ZVRDMiIREQABERESIjNEVWZ3d3d3d3ZlVEMyIhERABERERIiM0RWZ3d3d3d3dmVUQzIiEREAERERIiM0RV7//////////VRDMiIREQAREREiIzRF"
"bv/////////8VEMyIhERARERESIjNFVv//////////ZUQzIiEREBERESIjNEVt//////////9lRDMiIREQERERIiM0VW3//////////mVEMyIhERAREREiI0"
"RWbv/////////9ZUQzIiERERERESIzRFZ+//////////1lRDMiIRERERESIjNFVn7/////////92VEMyIRERERERIiM0Vmf//////////3ZUQzIhEREREREi"
"I0RWff/////////+dlRDMiERERERESIzRFZ+//////////52VEMiIRERERESIjNFVn7//////////XZUQyIhERERERIiM0VWfv/////////9dlQzIiERERER"
"EiIzRWZ///////////dmVDMiIRERERESIzRFZ9//////////92VUMyIhERERESIjNEVn3//////////nZVQzIiERERERIiM0VWfv/////////9dlRDMiERER"
"EREiIzRVZ+//////////12VEMyIRERERESIjNFZn7/////////93ZUQyIhERERERIiNEVnf//////////3ZlQzIiEREREREiM0RWff/////////+dlVDMiIR"
"EREREiIzRFZ9//////////52VUMyIhERERESIjNFVn7//////////XZUQzIiERERERIiM0Vmfv/////////9dlRDMiERERAREiIzRWd+//////////d2VEMi"
"IREREBESIjRFZ8//////////92ZUMyIhEREQERIiNEVn3//////////nZVQzIiERERAREiM0RWfv/////////9dlRDMiIREREBESIzRFZ+//////////12VE"
"MyIREREAESIjNFVn7/////////92ZUMyIhEREQARIiM0VWf//////////3ZVQzIiERERABEiIzRVbf/////////+dlRDMiIREREAESIjNFZt//////////52"
"VEMyIREREAARIiM0Vm7//////////WZUMyIhEREQABEiIzRWbv/////////9ZVQzIiERERAAESIjNFVu//////////dlRDMiEREREAAREiM0Vc//////////"
"9mVDMiIREREAABESIzRV3//////////mVUMyIhEREQAAERIjNEXv/////////+ZUQzIhERERAAAREiI0Re//////////1VQzIiERERAAABESIjNE7///////"
"//9VRDMiIREREAAAEREiM0T//////////1RDMiIREREAAAARESIjPf//////////RDMiIREREQAAABEREiMzRFVVZmZlVVRDMyIhEREQAAAAERESIjM0REVV"
"VVVERDMyIhERERAAAAAREREiIzM0REREREQzMiIhERERAAAAABERERIiIzMzM0MzMzMiIhEREREAAAAAERERESIiIzMzMzMyIiIhEREREAAAAAABEREREiIi"
"IiIiIiIiIREREREAAAAAAAARERERESIiIiIiIiEREREREAAAAAAAAAERERERERESIhEREREREREAAAAAAAAAABEREREREREREREREREREAAAAAAAAAAAAAAA"
"AAAAAAAAAAAREREREREREREREREREAAAAAAAAAAAAAAAAAAAARERERERERERERERERERAAAAAAAAAAAAAAAAAAARERERERIiIiIiIREREREQAAAAAAAAAAAA"
"AAAAAREREREiIiIiIiIiIhEREREAAAAAAAAAAAAAAAARERERIiIiMzMzMiIiIRERERAAAAAAAAAAAAAAABERERIiIzMzMzMzMyIiIREREAAAAAAAAAAAAAAB"
"ERERIiMzM0REREQzMyIiERERAAAAAAAAAAAAABERERIiMzRERVVUREQzMiIREREAAAAAAAAAAAAAERERIiMzRFVVVVVVVEMzIiEREQAAAAAAAAAAAAERERIi"
"MzTv/////////9MyIRERAAAAAAAAAAAAAREREiIzRO//////////QzIiEREAAAAAAAAAAAABERESIzRM//////////9DMiIREQAAAAAAAAAAABERESIjNE3/"
"/////////kQzIhERAAAAAAAAAAAAERERIiNEXv/////////+RDMiEREAAAAAAAAAAAEREREiM0Re//////////1EMyIREQAAAAAAAAAAAREREiIzRF7/////"
"////9UQzIhERAAAAAAAAAAABERESIjNFX//////////1RDMiEREAAAAAAAAAABERERIiM0Xf//////////VEMyIREQAAAAAAAAAAERERIiM0Rd//////////"
"5UQzIhERAAAAAAAAAAAREREiIzRF7//////////VRDMiEREAAAAAAAAAARERESIjNEVWZ3d3d3d3ZlVDMiIREQAAAAAAAAABERERIiM0RVZ3d3d3d3ZlVEMy"
"IhERAAAAAAAAAAERERIiM0RVZnd3d3d3dmVUQzIiEREAAAAAAAAAAREREiIzRFVmd3d3d3d2ZVRDMiIREQAAAAAAAAARERESIjNEVmd3d3d3d3ZlVEMyIhER"
"AAAAAAAAABERESIjNEVe//////////1UQzIiEREAAAAAAAAAERERIiM0RW7//////////FRDMiIREQAAAAAAAAEREREiIzRVb//////////2VEMyIhERAAAA"
"AAAAAREREiIzRFbf//////////ZUQzIiEREAAAAAAAABERESIjNFVt//////////5lRDMiIREQAAAAAAAAERERIiNEVm7//////////WVEMyIhERAAAAAAAA"
"EREREiM0RWfv/////////9ZUQzIiEREAAAAAAAAREREiIzRVZ+//////////dlRDMiEREQAAAAAAABERESIjNFZn//////////92VEMyIRERAAAAAAAAERER"
"IiNEVn3//////////nZUQzIhEREAAAAAAAAREREiM0RWfv/////////+dlRDIiEREQAAAAAAAREREiIzRVZ+//////////12VEMiIRERAAAAAAABERESIjNF"
"Vn7//////////XZUMyIhEREAAAAAAAERERIiM0Vmf//////////3ZlQzIiEREQAAAAAAAREREiM0RWff//////////dlVDMiIRERAAAAAAAREREiIzRFZ9//"
"////////52VUMyIhEREAAAAAABERESIjNFVn7//////////XZUQzIhEREQAAAAAAERERIiM0VWfv/////////9dlRDMiERERAAAAAAAREREiIzRWZ+//////"
"////d2VEMiIREREAAAAAABERESIjRFZ3//////////92ZUMyIhEREQAAAAABERESIjNEVn3//////////nZlQzIiERERAAAAAAERERIiM0VWff/////////+"
"dlVDMiIREREAAAAAAREREiIzRVZ+//////////12VEMyIhEREQAAAAABERESIjNFZn7//////////XZUQzIhEREQAAAAAAERERIjNEVnfv/////////3dlRD"
"IiERERAAAAAAERERIiM0RWfP//////////d2VDMiIREREAAAAAAREREiIzRVZ9//////////52ZUMyIhEREQAAAAABERESIjNFVn7//////////XZVQzIiER"
"ERAAAAAAERERIiM0Vmfv/////////9dlRDMiIREREAAAAAEREREiM0RWd+//////////d2VEMyIREREAAAAAAREREiIzRFZ3//////////93ZUQyIhEREQAA"
"AAABERESIjNFVn3//////////nZlQzIiERERAAAAABERERIiM0VWff/////////+dlVDMiIREREAAAAAERERIiM0RWZ+//////////12VEMyIhEREQAAAAER"
"EREiIzRFZ37//////////XZUQzIhEREQAAAAARERESIjNEVnfv/////////3dlRDIiERERAAAAARERESIiM0VWfP//////////dmVDMiIREREAAAABERERIi"
"M0RWZ9//////////52VUMyIhEREQAAAAEREREiIzRFZ37//////////nZUQzIiERERAAAAEREREiIjNFVnfv/////////9dlRDMiERERAAAAARERESIjNEVm"
"d+//////////d2VEMiIREREAAAARERESIiM0RWd3//////////92ZUMyIhEREQAAABERESIiM0RVZ33//////////3ZVQzIiERERAAABERERIiMzRFZnff//"
"///////+dlRDMiEREREAAAERERIiMzRFVnd+//////////12VEMiIREREAAAEREREiIzRFVnd3///////////WZUMyIhEREQAAAREREiIzRFVmd33///////"
"///3ZVQzIiERERAAABERESIzNEVmd33//////////+dlRDMiERERAAAAERESIjNEVmd37///////////5mVDMiIREREAAAARERIiNE3///////////////92"
"VEMyIhEREQAAABEREiM0Tv///////////////mZUQyIhEREQAAAAEREiIzRe///////////////9ZVQzIiERERAAAAARESIjNF7///////////////ZlRDMi"
"EREREAAAABERIiNEX///////////////1lRDMiIREREAAAAAEREiI0Tf//////////////5lVDMiIREREQAAAAARESIjNN//////////////5lVEMyIhEREQ"
"AAAAABERIiM07/////////////5lVEMyIhERERAAAAAAERESIzTv////////////1lVEMyIiERERAAAAAAARERIiM+///////////9ZlVEMzIiEREREAAAAA"
"ABEREiIzRF3e7////u1mZVREMzIiEREREAAAAAAAERERIiM0RFVWZmZmZlVURDMyIiEREREAAAAAAAAREREiIjNERFVVVVVVVERDMyIiEREREQAAAAAAABER"
"ERIiIzNEREREREREMzMyIiEREREQAAAAAAAAARERESIiMzMzREREMzMzMiIiEREREQAAAAAAAAABEREREiIiIzMzMzMzMyIiIhEREREQAAAAAAAAAAARERER"
"EiIiIiIiIiIiIiIREREREQAAAAAAAAAAAAEREREREiIiIiIiIiIiEREREREQAAAAAAAAAAAAABERERERERIiIiIiEREREREREQAAAAAAAAAAAAAAARERERER"
"EREREREREREREREAAAAAAAAAAAAAAAAAAAAAARERERERERERERERERERAAAAAAAAAAARERERERERIiEREREREREQAAAAAAAAARERERESIiIiIiIiEREREREA"
"AAAAAAAREREREiIiIiIiIiIiIRERERAAAAAAARERERIiIiMzMzMzMiIiEREREQAAAAARERERIiIzMzM0MzMzMiIhERERAAAAABERERIiMzNEREREREMzMiIR"
"EREAAAABERERIiMzRERVVVVUREMzIiEREQAAAAERERIiMzRFVVZmZlVVRDMyIRERAAAAEREREiIzRO//////////0zIiEREAAAAREREiIzRF7/////////9E"
"MyIREQAAAREREiIzRFz//////////0QzIiERAAABERESIjNFXf/////////+VEMiIREAABERERIjNEVu//////////5UQzIhEQAAERERIiM0VW7/////////"
"/VVDMiERAAAREREiI0RWbv/////////2VUMyIREAARERESIzRFZ///////////ZVQzIiEQABERESIjNFVt//////////9mVDMiIRAAERERIiM0Vm3///////"
"///mZUMyIhEAAREREiM0RWfv/////////9ZlQzIiEQAREREiIzRVZ+//////////1lVDMiIRABERESIjNFVn7/////////92VUMyIhEAERERIiM0Vnf/////"
"/////3ZVQzIhEQAREREiM0RWff/////////+dlRDMiERAREREiIzRFZ+//////////52VEMyIREBERESIjNFVn7//////////XZUQyIhEQERERIiM0Vmfv//"
"///////8dlRDIiERAREREiI0RWd///////////d2VDMiIREBERESIzRFZ9//////////92ZUMyIhERERESIjNEVn3//////////nZVQzIiERERERIiM0VWfv"
"/////////9dlVDMiIREREREiIzRWZ+//////////12VEMyIRERERESIjRFZ37/////////93ZUQyIhERERERIjNEVnf//////////3dlQzIiERERERIiM0RW"
"ff/////////+dmVDMiIREREREiIzRVZ+//////////52VUMyIhERERESIjNFZn7//////////XZUQzIiERERERIiM0Vnfv/////////9dlRDMiIREREREiI0"
"RWd///////////d2VEMiIREREREiIzRFZ9//////////92ZUMyIhERERESIjNEVn3//////////nZVQzIiERERERIiM0VWfv/////////9dlVDMiIREREREi"
"IzRWZ+//////////12VEMyIhERERESIjRFZ37/////////93ZUQzIhERERERIiNEVnf//////////3dlRDIiEREREREiM0RWff/////////+dmVDMiIRERER"
"EiIzRVZ9//////////52VUMyIhERERESIjNFVn7//////////XZUQzIiERERERIiM0Vmfv/////////9dlRDMiERERAREiIzRWd+//////////d2VEMiIRER"
"EBESIjRFZ8//////////92ZUMyIhEREQERIiNEVn3//////////nZVQzIiERERAREiM0RWfv/////////9dlRDMiIREREBESIzRFZ+//////////12VEMyIR"
"EREAESIjNFVn7/////////92ZUMyIhEREQARIiM0VWf//////////3ZVQzIiERERABEiIzRVbf/////////+dlRDMiIREREAESIjNFZt//////////52VEMy"
"IREREAARIiM0Vm7//////////WZUMyIhEREQABEiIzRWbv/////////9ZVQzIiERERAAESIjNFVu//////////dlRDMiEREREAAREiM0Vc//////////9mVD"
"MiIREREAABESIzRV3//////////mVUMyIhEREQAAERIjNEXv/////////+ZUQzIhERERAAAREiI0Re//////////1VQzIiERERAAABESIjNE7/////////9V"
"RDMiIREREAAAEREiM0T//////////1RDMiIREREAAAARESIjPf//////////RDMiIREREQAAABEREiMzRFVVZmZlVVRDMyIhEREQAAAAERESIjM0REVVVVVE"
"RDMyIhERERAAAAAREREiIzM0REREREQzMiIhERERAAAAABERERIiIzMzM0MzMzMiIhEREREAAAAAERERESIiIzMzMzMyIiIhEREREAAAAAABEREREiIiIiIi"
"IiIiIREREREAAAAAAAARERERESIiIiIiIiEREREREAAAAAAAAAERERERERESIhEREREREREAAAAAAAAAABEREREREREREREREREREAAAAAAAAAAAAAAAARER"
"EREREREREREREREREREREREREREREREQAAAAAAAAAAAREREREREREREiIiIiIiIiIiIiIiIREREREREAAAAAAAAAARERERERIiIiIiIiIiIiIiIiIiIiIiIh"
"ERERERAAAAAAAAAREREREiIiIiIiIiIiIiIiMzMzMzIiIiIhEREREQAAAAAAARERERIiIiIzMzMzMzMzMzMzMzMzMzMyIiIREREREAAAAAARERESIiIjMzMz"
"MzMzMzMzNEREREREMzMyIiIRERERAAAAARERESIiMzM0RERERERERERERFVVVUREQzMyIiEREREAAAARERESIiMzREREVVVEREREVVVVVVVVVVVEQzMiIhER"
"ERAAABERESIiM0REVVVVVVVVVVVVZmZmZmZmZVVEQzMiIREREQABERERIiM0RFVmZmZmZmZmZmZnd97///7tZlVEMzIiERERAAERERIiM0Tf///////+d3d3"
"d37////////+ZVRDMiIREREAEREREiMzRd////////53d3fP///////////9VUQzIiEREQAREREiIzRF7////////3d3fv/////////////lVEMyIRERARER"
"ESIzRFb/////////d33///////////////1VQzIiEREBERESIjNFVv////////937////////////////mVEMyIREQERERIiNEVm/////////n//////////"
"////////1VQzIiEREREREiM0RW3////////////////////////////mVEMiIREREREiIzRWbv///////////////////////////+ZUQzIhERERESIjRFZ+"
"////////////////////////////9lVDMiERERERIjNEVn/////////////////////////////2ZUMyIhERERIiM0VWf///////////////////////////"
"//dlQzIiEREREiIzRWff////////////53d37///////////92VEMiIRERESIzRFZ9////////////d3d3d9///////////3ZUQyIhERERIjNEVn7///////"
"////d3d3d3fv//////////dlRDIiERERIiM0VWf///////////d3d3d3d+//////////92VEMiIREREiIzRWZ///////////13d3d3d37//////////nZUQy"
"IhERESIjRFZ3//////////53d3d3d3fv/////////+dlRDIiERERIjNEVn3//////////nd3d3d3d+//////////12VDMiIRERIiM0RWfv/////////9d3d3"
"d3d37/////////93ZUMyIhEREiIzRVZ+//////////13d3d3d3f//////////3ZlQzIiERESIjNFZn//////////93d3d3d3ff//////////dmVDMiIRERIi"
"M0Vnf//////////3d3d3d3d9//////////52VUMyIREREiM0RWfP/////////+d3d3d3d37//////////XZUQzIhERESIzRFZ9//////////53d3d3d3fv//"
"///////9dlRDIiERESIjNEVn7//////////Xd3Zmd3d+//////////d2VEMiIRERIiM0VWfv/////////8d3dmZ3d3//////////92ZUMyIhEREiIzRWZ///"
"////////d3dmZnd33//////////nZVQzIiERESIjNFZn//////////53d2Zmd3fv/////////+dlRDMiERERIiM0Vn3//////////nd2ZmZnd+//////////"
"12VEMyIREREiI0RWff/////////9d3ZmZmd37//////////HZUMyIhERESIjRFZ+//////////13dmZmZ3f//////////3ZlQzIiERERIjNEVn//////////"
"93dmZmZnff//////////dlVDMiIREREiM0RWf//////////3d2ZlZmd9//////////52VEMyIRERESIzRFbP/////////+d2ZlVmZ37//////////WZUMyIh"
"ERERIjNEVt//////////53ZlVVZnfv/////////9ZVQzIiEREREiI0RW7//////////XZlVVVmd+//////////dlRDMiIRERESIjRFbv/////////9dmVVVV"
"Zn//////////9mVEMiIRERERIiNEVv//////////dmVVVVVm3//////////mVUMyIhEREREiIzRV//////////5mVVREVVbv/////////+ZUQzIiERERERIj"
"NE3//////////mVVRERFVu//////////1VQzIiERERAREiM0Tf/////////9VVRERERV7//////////FRDMiIREREBESIjNO//////////1VREQ0REX/////"
"/////1RDMiIREREAERIiM0//////////9URDMzM0Tf//////////RDMiIREREQARESIjNERVVmZmZVVURDMzMzNERVVWZmZlVVRDMyIhEREQABEREiIzNERV"
"VVVVREQzMzMzMzNERVVVVVVERDMyIhERERAAERESIiMzREREREREMzMyIiIjMzNEREREREQzMyIhERERAAAREREiIjMzMzRDMzMzMiIiIiIjMzMzREQzMzMi"
"IhEREREAABERERIiIiMzMzMzMyIiIiIiIiIiMzMzMzMyIiIhEREREAAAARERERIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIREREREAAAABERERERIiIiIiIiIi"
"IREREREiIiIiIiIiIiEREREREAAAAAARERERERERIiIhERERERERERERESIiIhEREREREREAAAAAAAEREREREREREREREREREREREREREREREREREREREAAA"
"AAAAAAAAAAAAABERERERERERERIiIiIhEREREREREREAAAAAAAAAAAAAAAAREREREREREiIiIiIiIiIiIhEREREREREAAAAAAAAAAAAAARERERERESIiIiIi"
"IiIiIiIiIiIRERERERAAAAAAAAAAAAARERERERIiIiIiMzMzMzMzMyIiIiIREREREQAAAAAAAAAAERERERESIiIiMzMzMzMzMzMzMzMiIiIREREREAAAAAAA"
"AAEREREREiIiIzMzNERERERERERDMzMiIiERERERAAAAAAAAERERERIiIiMzNERERVVVVVVVVEREMzMiIiEREREQAAAAAAERERERIiIjMzRERVVVVmZmZmVV"
"VVREMzMiIhEREREAAAAAERERESIiIzM0RFVVZmZmd3d3ZmZmVVREMzIiIRERERAAAAERERESIiIzNERVVmZn3e7////u3XZmZVREMzIiEREREAAAARERESIi"
"MzREVWZn3v///////////+1mVVRDMyIhERERAAARERESIiMzRFVWZ+////////////////1mVUQzMiIREREAARERESIiMzRFVmfv//////////////////1l"
"VEMyIhEREQARERERIiM0RFVm7////////////////////+ZVRDMiIRERABERERIiM0RFZt///////////////////////mVUQzIiEREBERERIiMzRFZt////"
"////////////////////5lVDMiIREQERERIiIzRFZu/////////////////////////9ZUQzIiEREREREiIzRFZu//////////////////////////9lVDMi"
"IREREREiIzRFVt///////////////////////////9ZUQzIhERERESIjNFVn////////////////////////////9lVDMiIRERESIjNEVn////////////53"
"d3ff///////////8ZUMyIhERERIiM0Vm3//////////+d3d3d3fv//////////1lRDIiERERIiM0RWf//////////9d3d3d3d37//////////mVUMyIREREi"
"IzRWbv/////////+d3d3d3d3ff/////////+ZlQzIhERESIzRFZ///////////d3d3d3d3d3//////////92VDMiIRESIjNFVt//////////53d3d3d3d3f/"
"/////////3ZUMyIhERIiNEVn7/////////93d3d3d3d3d//////////+dlQzIiEREiM0RWf//////////nd3d2Zmd3d3//////////52VDMiIREiIzRVbf//"
"///////+d3d2ZmZ3d3f//////////nZUMyIhESIjNFZ+//////////13d2ZmZmd3ff/////////+dlQzIiERIiNEVn//////////93d3ZmZmZ3d9////////"
"//5mVDMiIREiI0RWf//////////3d3dmZmZnd37//////////WVUMyIRESIzRFZ//////////+d3d2ZmZnd3fv/////////9ZVQzIhERIjNFVt//////////"
"53d3ZmZmd3d+//////////dlRDMiEREiM0VW3//////////Xd3dmZmd3d8//////////52VEMiIRESIzRVbv/////////9d3d3ZmZ3d33//////////WZUMy"
"IhERIjNFVu//////////13d3d3d3d3fv/////////3ZUQzIhEREiM0VW7//////////Xd3d3d3d3d//////////+dlRDIiERESIzRFbf/////////9d3d3d3"
"d3d+//////////1lVDMiIRERIiNEVt//////////53d3d3d3d9//////////92VEMyIhEREiI0RW3//////////9d3d3d3d3///////////WVUMyIhERESIj"
"NFZv//////////93d3d3d9///////////3ZUQzIiERERIiM0VW////////////53d3ff///////////9ZVQzIiERERESIzRFbf//////////////////////"
"/////+ZlRDMiIRERERIiM0Vm///////////////////////////+ZlRDMiIREREREiIzRFbv//////////////////////////ZlVDMyIhERERERIiM0VW//"
"////////////////////////dlVEMyIhEREREREiIzRFbf///////////////////////+ZlVEMyIhERERARERIiM0VW3//////////////////////+ZlVE"
"MyIiEREREBEREiIzRFVt/////////////////////3ZlVEMyIiEREREAERERIiM0RVZu///////////////////WZVREMyIiEREREAARERESIjNEVWbO////"
"////////////1mZVRDMyIiEREREQAAERERIiIzREVWZ+////////////7XZmVVRDMyIiEREREQAAARERESIiMzRFVWZn3e7////u7XdmZlVURDMyIiEREREQ"
"AAAAEREREiIjM0RFVVZmZnd3d2ZmZmVVREQzMiIiEREREQAAAAABERERIiIzM0REVVVWZmZmZlVVVUREMzMiIhEREREQAAAAAAERERESIiIzM0RERFVVVVVV"
"VEREQzMzIiIhEREREQAAAAAAABERERESIiIzMzRERERERERERDMzMiIiIREREREQAAAAAAAAAREREREiIiIjMzMzMzMzMzMzMzIiIiEREREREQAAAAAAAAAA"
"EREREREiIiIiMzMzMzMzMyIiIiIhEREREREQAAAAAAAAAAABERERERESIiIiIiIiIiIiIiIiIREREREREQAAAAAAAAAAAAABERERERERIiIiIiIiIiIiIhER"
"EREREREAAAAAAAAAAAAAAAARERERERERERIiIiIiERERERERERERAAAAAAAAAAAAAAAAAAEREREREREREREREREREREREREREQAAAAAAAAABERERERERESIi"
"IiIiIiIhEREREREREAAAAAAAARERERERIiIiIiIiIiIiIiIiIREREREQAAAAAAERERERIiIiIiIiIiIiIiIiIiIiEREREQAAAAABEREREiIiIjMzMzMzMzMz"
"MzMyIiIhERERAAAAARERESIiIzMzMzMzMzMzMzMzMzMyIiEREREAAAEREREiIjMzNERERERERERERERDMzIiIREREAABERERIiIzNEREVVVVVEREVVVVREQz"
"MyIhEREAABERESIiM0RFVVVVVVVVVVVVVVVVREMyIhEREAARERESIjNEVVZmZmZmZmZmZmbe///tQzIiEREAAREREiIzRN////////53d3ff//////4zIiER"
"EAEREREiM0Rd////////53d3////////1DMiEREAERERIiM0Ve////////53ff////////xDMiIREBERERIjNEVv////////937/////////VEMiIREBERES"
"IjNFVv////////99//////////VEMiIREBERESIjRFZv////////5//////////+VEMyIREREREiIzRVbf/////////v/////////9VEMiIREREREiIzRWbv"
"///////////////////9VEMiIREREREiI0RWfv///////////////////2VEMiIREREREiM0VWf////////////////////1VDMiERERERIiM0Vmf///////"
"/////////////lRDMiERERERIiM0Vn3////////////////////lRDIiERERERIjNEVn3////////////////////UQzIiERERERIjNEVn7/////////////"
"///////EMyIhERERESIjNFVn//////////////7Xd3ZlVEMyIhERERESIjNFZn/////////////nd3dmVVRDMiIRERERESIjRFZ3////////////13d3ZlVE"
"QzIiIRERERESIzRFZ9///////////9d3dmVVRDMyIiERERAREiIzRVZ+///////////Xd3ZlVEQzMiIhERERAREiIzRVZ+//////////93d2ZVREMzIiIRER"
"EQAREiIzRWZ///////////53dmVUQzMiIiEREREAAREiI0RWd///////////d3dlVEMzIiIhEREREAAREiM0RWfP/////////+d3ZVRDMyIiIRERERAAAREi"
"M0RWff/////////+d2ZUQzMiIiEREREQAAARIiM0VWfv/////////9d2VUQzIiIREREREAAAARIiM0VWfv/////////8d2VEMyIiIRERERAAAAARIiM0Vmf/"
"/////////3dlVEMyIiEREREQAAAAARIiM0Vmf//////////ndlRDMiIhEREREAAAAAARIiM0Vn3//////////ndlRDMiIhERERAAAAAAARIiNEVn3///////"
"///XZVQzMiIRERERAAAAAAARIiNEVn7//////////XZUQzIiEREREQAAAAAAARIjNEVn//////////93ZUQzIiEREREAAAAAAAARIjNEVn//////////92VU"
"MyIhERERAAAAAAAAARIjNEVs//////////52VEMyIhERERAAAAAAAAARIjNEVt//////////52VEMyIREREQAAAAAAAAARIiNEVu//////////1lVDMiIRER"
"EQAAAAAAAAARIiNEVu//////////1lRDMiIREREAAAAAAAAAARIiNEVv//////////ZlRDIiEREREAAAAAAAAAARIiM0Vf/////////+ZUQzIiERERAAAAAA"
"AAAAAREiM0Tf/////////+VUQzIhERERAAAAAAAAAAAREiM0Tf/////////9VEMyIhEREQAAAAAAAAAAAREiIzTv/////////9RDMiIREREQAAAAAAAAAAAR"
"EiIzT//////////0QzMiIREREAAAAAAAAAAAARESIjNERVVmZmZVVEMzIiEREREAAAAAAAAAAAARERIiMzREVVVVVURDMyIhERERAAAAAAAAAAAAARERIiIz"
"NEREREREMzMiIhEREQAAAAAAAAAAAAAREREiIjMzMzMzMzMyIiIREREQAAAAAAAAAAAAARERESIiIjMzMzMyIiIhEREREAAAAAAAAAAAAAABEREREiIiIiIi"
"IiIiIRERERAAAAAAAAAAAAAAABERERERIiIiIiIiIREREREQAAAAAAAAAAAAAAAAERERERERERIhEREREREREAAAAAAAAAAAAAAAAAARERERERERERERERER"
"ERAAAAAAAAAAAAAAAAAAAAAAAAAAABEREREREREREREREREAAAAAAAAAAAAAAAAAEREREREREREREREREREQAAAAAAAAAAAAAAERERERERIiIiIiEREREREQ"
"AAAAAAAAAAAAARERERESIiIiIiIiIhEREREQAAAAAAAAAAABERERESIiIiMzMyIiIhEREREAAAAAAAAAAAERERERIiIjMzMzMzMiIiEREREAAAAAAAAAARER"
"ERIiIjMzREREQzMyIiEREREAAAAAAAABEREREiIjM0RERVVERDMyIiEREREAAAAAAAERERESIiMzREVVVVVVREMyIhEREREAAAAAARERERIiIzNEXv//////"
"/UMyIhERERAAAAABEREREiIjM0RV////////1DMyIhERERAAAAERERESIiMzRFbf///////1RDMiIREREQAAABEREREiIzNEVm7///////5VRDMiIREREQAA"
"ERERESIjM0RWbf///////+ZUQzIiEREREAABERERIiIzRFVn/////////mVUQzIiERERAAEREREiIjNEVWff////////12VEMyIhERERARERESIiM0RVZ37/"
"///////9dlVEMyIhEREQERERIiIzRFVnfP////////93ZlRDMiIhEREREREiIjNEVWZ37////////+d3ZVRDMiIRERERERIiM0RVZnd//////////nd3ZVQz"
"MiEREREREiIzRFVmd37/////////13d2ZUQzIiERERERIiM0VWZ3d//////////9d3d2VUMyIhEREREiIzRF/////////////////////UMyIRERERIiM0Xf"
"////////////////////VDMiIRERESIzRF3////////////////////1QzIiEREREiM0Ve////////////////////5UQyIhERESIjNFbv//////////////"
"/////+VEMiIREREiI0RW/////////////////////VRDIiERERIiNEVv////////////////////1UMyIhERESIjRF3////////////////////1VDMiERER"
"EiI0Rd////////////////////9UQzIhERERIiNEXv///////////////////+VEMiIRERESIjRF/////////////////////kQzIiEREREiIzRWZ3d/////"
"//////d3d3dlVEMyIRERERIiM0VWd3f//////////nd3d2VUQzIiERERESIjNFVnd9//////////53d3ZVRDMiIREREREiIzRFZ3ff/////////+d3dmVEMz"
"IiERERERIiM0RWd37//////////Xd2ZURDMiIREREBESIjNEVnd///////////13dlVEMyIiERERAREiIzRFZ3f//////////3d2ZUQzMiIREREQERIiM0RW"
"d9//////////53dlVDMyIiERERABESIjNEVnff/////////+d2ZUQzIiIREREQARESIzRFZ37//////////ndlVEMyIiEREREAEREiM0RWd+//////////13"
"ZVQzMiIREREQABERIjNEVnf//////////3d2VEMyIiEREREAAREiIzRFZ3//////////53dlRDMiIhERERAAERIiM0RWff/////////+d3ZUQzIiIREREQAB"
"ESIjNFVn3//////////nd2VEMyIiEREREAAREiIzRVZ+//////////13dlRDMiIhEREQAAERIiM0VWfv/////////9d3ZUQzMiIREREAABESIjNFZn//////"
"////93d2VUQzIiERERAAAREiIzRWbP//////////d3dmVEMyIhEREQAAEREiM0Vm3//////////3d3dlRDMiIREREAABERIjNFZu///////////Xd3ZVRDMi"
"ERERAAARESIzRVbv///////////Xd2ZUQzIhEREQAAEREiM0VW7////////////////UMyIhEREAABERIjNEVu////////////////1DMiIRERAAARESIzRF"
"bv///////////////1QzIiEREQAAEREiIzRW7///////////////9UMyIhEREAABERIiM0Ve///////////////+VDMiIRERAAARERIjNEXf////////////"
"///VQzIhEREAAAERESIjNFX///////////////1EMyIRERAAABEREiIzRF3//////////////1RDMiEREQAAAREREiIzRFz/////////////9EMyIhEREAAA"
"ERERIiMzRFXe///////////9RDMiERERAAAAERERIiM0RFVm3u////7t1VVEMyIhEREAAAABERERIiMzRFVVZmZmZmZVVEQzIiERERAAAAABERERIiMzRERV"
"VVVVVVVERDMiIhEREQAAAAARERERIiIzNERERVVERERDMyIiERERAAAAAAARERERIiIzMzNEREREMzMzIiIREREQAAAAAAARERERIiIiMzMzMzMzMyIiIhER"
"ERAAAAAAAAAREREREiIiIiMzMzIiIiIhEREREQAAAAAAAAARERERESIiIiIiIiIiIhERERERAAAAAAAAAAAREREREREiIiIiIiEREREREREAAAAAAAAAAAAR"
"EREREREREREREREREREREAAAAAAAAAAAAAABEREREREREREREREREREREREREREREREREREREQAAAAAAAREREREREREiIiERERERERERERERIiIhERERERER"
"EAAAAAARERERESIiIiIiIiIiIREREREiIiIiIiIiIhERERERAAAAAREREREiIiIiIiIiIiIiIiIiIiIiIiIiIiIiIiEREREQAAABERERIiIiMzMzMzMyIiIi"
"IiIiIiIzMzMzMzIiIhEREREAABERERIiIjMzM0RDMzMzMiIiIiIzMzM0REMzMzIiIREREQABERERIiIzM0RERERERDMzMiIiMzM0RERERERDMzIiERERAAER"
"ERIiIzNERVVVVVVERDMzMzMzNERFVVVVVERDMyIhEREAEREREiIzREVVZmZmVVVEQzMzMzREVVVmZmZVVUQzMiEREQEREREiIzRN//////////5EQzMzREXv"
"/////////9MyIhERAREREiIzRF7//////////VREREREVf//////////RDMiEREBERESIjNEXv/////////8VEREREVd//////////9EMyIhERERESIjNEVv"
"//////////ZVRERFVW3//////////lRDIiERERERIiM0VW//////////9mVURFVWbv/////////9VEMyIREREREiM0RW3//////////mZVVVVWZ+////////"
"//1UQzIhEREREiIzRVbf/////////9dmVVVVZn7/////////9lVDMiERERESIjNFZu//////////12ZVVVZnf//////////2VUMyIhERERIjNEVn7///////"
"//93ZlVVZmff/////////+ZlQzIhERERIiM0VWf//////////3dmZVZmd9//////////5mVDMiIREREiIzRWbf/////////+d2ZmZmZ37//////////WVUMy"
"IhERESIjRFZ9//////////53dmZmZ3fv/////////9ZVQzIiERERIjNEVn7//////////Xd2ZmZnd///////////dlVDMiIRERIiM0VWfv/////////8d3Zm"
"Znd9//////////92VUMyIREREiIzRWZ///////////d3dmZmd33//////////nZUQzIhERESIjNFZ3//////////93d2Zmd3fv/////////+dlRDMiERERIj"
"NEVn3//////////nd3ZmZ3d+//////////12VEMiIREREiM0RWff/////////+d3dmZ3d37//////////HZUQyIhEREiIzRVZ+//////////13d3Znd3f///"
"///////3ZlQzIiERESIjNFZn7/////////93d3d3d3ff/////////+dmVDMiIRERIiM0Vnf//////////3d3d3d3d9//////////52VUMyIhEREiI0RWfP//"
"////////d3d3d3d37//////////XZUQzIiERESIzRFZ9//////////53d3d3d3fv/////////9dlRDMiERERIjNEVn7//////////Xd3d3d3d///////////"
"d2VEMiIREREiM0VWfv/////////9d3d3d3d8//////////92ZUMyIhERESIzRVZ///////////d3d3d3d33//////////nZVQzIiERERIjNFZn//////////"
"93d3d3d3fv/////////+dlVDMiIRERIiM0Vm3//////////nd3d3d3d///////////12VEMyIREREiIzRWbf/////////+d3d3d3d+///////////HZUQyIh"
"ERERIjNFZ+///////////Xd3d3d9///////////3ZlQzIiEREREiM0Vm7//////////+d3d3d+///////////+dlVDMiIRERESIzRWb////////////td3ff"
"////////////52VEMyIhERERIjNFVv/////////////////////////////XZUQyIhEREREiM0RW7////////////////////////////9ZlQzIiERERESIj"
"RFbv////////////////////////////dlVDMiIRERERIiM0Vu////////////////////////////92VEMyIREREREiIzRV3///////////////////////"
"/////mZUMyIhEREQERIjNEVv/////////////////93////////+ZUQzIiERERAREiIzRV3////////////////3ff////////1lRDMiERERABESIjNEVv//"
"/////////////nd9/////////VRDMiIREREAEREiIzRV3//////////////Xd33////////1VEMyIREREQARESIjNEVW////////////53d3fv///////+VE"
"MyIhEREQABEREiIzRFVt/////////9d3d3d+////////5EMyIhERERAAERERIiM0RVVt3v///t13dmZmZmZmZmZmZVVEMzIiERERAAAREREiIjNERVVmZmZm"
"ZmZmVVVVVVVVVVVVREMzIiEREREAABERERIiIzNERVVVVVVVVVVURERERFVVRERDMzIiEREREAAAARERESIiMzNEREVVVVVEREREREREREREMzMyIiEREREA"
"AAAAEREREiIiMzM0REREREQzMzMzMzMzMzMzMiIiEREREQAAAAAREREREiIiMzMzMzMzMzMzMzMzMzMzMiIiIhEREREQAAAAAAEREREREiIiIjMzMzMzIiIi"
"IiIiIiIiIiIREREREQAAAAAAABEREREREiIiIiIiIiIiIiIiIiIiIiIhEREREREQAAAAAAAAAREREREREiIiIiIiIiIiIiIiIhEREREREREREAAAAAAAAAAA"
"AREREREREREREREREREREREREREREREREREAAAAAAAA=";

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

/* ---- Arcade Station: lector de JPG propio (baseline/secuencial, 1 o 3 componentes, submuestreo y reinicios), sin librerias ---- */
typedef struct { uint8_t vals[256]; int mincode[17], maxcode[17], valptr[17]; int ok; } JHuff;
typedef struct { const uint8_t *p, *e; int cur, nb, mk; } JBits;
static const uint8_t jz_zz[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };
static const float jz_ct[64] = { 0.3535534f,0.4903926f,0.4619398f,0.4157348f,0.3535534f,0.2777851f,0.1913417f,0.0975452f,0.3535534f,0.4157348f,0.1913417f,-0.0975452f,-0.3535534f,-0.4903926f,-0.4619398f,-0.2777851f,0.3535534f,0.2777851f,-0.1913417f,-0.4903926f,-0.3535534f,0.0975452f,0.4619398f,0.4157348f,0.3535534f,0.0975452f,-0.4619398f,-0.2777851f,0.3535534f,0.4157348f,-0.1913417f,-0.4903926f,0.3535534f,-0.0975452f,-0.4619398f,0.2777851f,0.3535534f,-0.4157348f,-0.1913417f,0.4903926f,0.3535534f,-0.2777851f,-0.1913417f,0.4903926f,-0.3535534f,-0.0975452f,0.4619398f,-0.4157348f,0.3535534f,-0.4157348f,0.1913417f,0.0975452f,-0.3535534f,0.4903926f,-0.4619398f,0.2777851f,0.3535534f,-0.4903926f,0.4619398f,-0.4157348f,0.3535534f,-0.2777851f,0.1913417f,-0.0975452f };

static int jb_bit(JBits *b)
{
    if (b->nb == 0) {
        int c = 0;
        if (!b->mk && b->p < b->e) {
            c = *b->p++;
            if (c == 0xFF) {
                if (b->p < b->e && *b->p == 0) b->p++;
                else { b->p--; b->mk = 1; c = 0; }
            }
        }
        b->cur = c; b->nb = 8;
    }
    b->nb--;
    return (b->cur >> b->nb) & 1;
}
static int jb_bits(JBits *b, int n) { int v = 0; while (n-- > 0) v = (v << 1) | jb_bit(b); return v; }
static int jh_dec(JBits *b, const JHuff *h)
{
    int code = 0, l;
    for (l = 1; l <= 16; l++) {
        code = (code << 1) | jb_bit(b);
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l]) return h->vals[h->valptr[l] + code - h->mincode[l]];
    }
    return -1;
}
static int jext(int v, int n) { return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }

static void jidct(const int *in, uint8_t *out, int stride)
{
    float tmp[64];
    int x, y, u, v;
    for (v = 0; v < 8; v++) {                    /* filas: tmp[v][x] = sum_u ct[x][u] * in[v][u] */
        const int *r = in + v * 8;
        if (!(r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7])) { float d = jz_ct[0] * (float)r[0]; for (x = 0; x < 8; x++) tmp[v * 8 + x] = d; continue; }
        for (x = 0; x < 8; x++) { float s = 0; for (u = 0; u < 8; u++) s += jz_ct[x * 8 + u] * (float)r[u]; tmp[v * 8 + x] = s; }
    }
    for (x = 0; x < 8; x++)                      /* columnas: out[y][x] = sum_v ct[y][v] * tmp[v][x] */
        for (y = 0; y < 8; y++) {
            float s = 0; int p;
            for (v = 0; v < 8; v++) s += jz_ct[y * 8 + v] * tmp[v * 8 + x];
            p = (int)(s + 128.5f);
            if (s + 128.5f < 0) p = 0;
            out[y * stride + x] = (uint8_t)(p < 0 ? 0 : p > 255 ? 255 : p);
        }
}

/* Decodifica un JPG en memoria (mismos codigos que cv_png_decode): 0 = bien (*px = malloc de w*h ARGB; liberar con free),
 * 1 = no es JPG, 2 = demasiado grande, 3 = formato no soportado (progresivo, CMYK...), 4 = archivo danado, 5 = sin memoria */
static int cv_jpg_decode(const uint8_t *d, size_t n, uint32_t bg, uint32_t **out, int *ow, int *oh)
{
    uint16_t qt[4][64];
    JHuff hd[4], ha[4];
    int have_q[4] = { 0, 0, 0, 0 };
    int w = 0, hgt = 0, nc = 0, hmax = 1, vmax = 1, dri = 0, rc = 4, i, got_sof = 0;
    int cid[4] = { 0 }, ch[4] = { 1, 1, 1, 1 }, cv[4] = { 1, 1, 1, 1 }, cq[4] = { 0 }, ctd[4] = { 0 }, cta[4] = { 0 };
    uint8_t *pl[4] = { NULL, NULL, NULL, NULL };
    int pw[4] = { 0 }, ph[4] = { 0 };
    uint32_t *px = NULL;
    size_t pos = 2;
    (void)bg;
    memset(hd, 0, sizeof hd); memset(ha, 0, sizeof ha);
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return 1;
    while (pos + 4 <= n) {
        int m, len;
        if (d[pos] != 0xFF) { pos++; continue; }
        m = d[pos + 1];
        if (m == 0xFF) { pos++; continue; }
        if (m == 0x00 || (m >= 0xD0 && m <= 0xD8) || m == 0x01) { pos += 2; continue; }
        if (m == 0xD9) break;
        len = (d[pos + 2] << 8) | d[pos + 3];
        if (len < 2 || pos + 2 + (size_t)len > n) { rc = 4; goto fail; }
        {
            const uint8_t *s = d + pos + 4;
            int sl = len - 2;
            if (m == 0xDB) {
                while (sl >= 65) {
                    int pq = s[0] >> 4, tq = s[0] & 15, k;
                    s++; sl--;
                    if (tq > 3 || (pq && sl < 128) || (!pq && sl < 64)) { rc = 4; goto fail; }
                    for (k = 0; k < 64; k++) { qt[tq][k] = pq ? (uint16_t)((s[2 * k] << 8) | s[2 * k + 1]) : s[k]; }
                    s += pq ? 128 : 64; sl -= pq ? 128 : 64;
                    have_q[tq] = 1;
                }
            } else if (m == 0xC4) {
                while (sl >= 17) {
                    int tc = s[0] >> 4, th = s[0] & 15, cnt[17], tot = 0, l, code = 0, k = 0;
                    JHuff *h;
                    if (th > 3 || tc > 1) { rc = 4; goto fail; }
                    h = tc ? &ha[th] : &hd[th];
                    for (l = 1; l <= 16; l++) { cnt[l] = s[l]; tot += cnt[l]; }
                    if (tot > 256 || sl < 17 + tot) { rc = 4; goto fail; }
                    memcpy(h->vals, s + 17, (size_t)tot);
                    for (l = 1; l <= 16; l++) {
                        h->valptr[l] = k; h->mincode[l] = code; code += cnt[l]; k += cnt[l];
                        h->maxcode[l] = cnt[l] ? code - 1 : -1;
                        code <<= 1;
                    }
                    h->ok = 1;
                    s += 17 + tot; sl -= 17 + tot;
                }
            } else if (m == 0xDD) {
                if (sl >= 2) dri = (s[0] << 8) | s[1];
            } else if (m == 0xC0 || m == 0xC1) {
                if (sl < 6 || s[0] != 8) { rc = 3; goto fail; }
                hgt = (s[1] << 8) | s[2]; w = (s[3] << 8) | s[4]; nc = s[5];
                if (w <= 0 || hgt <= 0) { rc = 4; goto fail; }
                if (w > 1024 || hgt > 1024) { rc = 2; goto fail; }
                if ((nc != 1 && nc != 3) || sl < 6 + 3 * nc) { rc = 3; goto fail; }
                for (i = 0; i < nc; i++) {
                    cid[i] = s[6 + 3 * i]; ch[i] = s[7 + 3 * i] >> 4; cv[i] = s[7 + 3 * i] & 15; cq[i] = s[8 + 3 * i] & 3;
                    if (ch[i] < 1 || ch[i] > 4 || cv[i] < 1 || cv[i] > 4) { rc = 3; goto fail; }
                    if (ch[i] > hmax) hmax = ch[i];
                    if (cv[i] > vmax) vmax = cv[i];
                }
                if (nc == 1) { ch[0] = cv[0] = 1; hmax = vmax = 1; }
                got_sof = 1;
            } else if ((m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC)) {
                rc = 3; goto fail;
            } else if (m == 0xDA) {
                int ns, mcux, mcuy, mx, my, k, c, pred[4] = { 0, 0, 0, 0 }, cnt = 0;
                JBits b;
                if (!got_sof || sl < 1) { rc = 4; goto fail; }
                ns = s[0];
                if (ns != nc || sl < 1 + 2 * ns + 3) { rc = 3; goto fail; }
                for (i = 0; i < ns; i++) {
                    int id = s[1 + 2 * i];
                    for (k = 0; k < nc; k++) if (cid[k] == id) { ctd[k] = s[2 + 2 * i] >> 4 & 3; cta[k] = s[2 + 2 * i] & 3; }
                }
                for (c = 0; c < nc; c++) if (!have_q[cq[c]] || !hd[ctd[c]].ok || !ha[cta[c]].ok) { rc = 4; goto fail; }
                mcux = (w + 8 * hmax - 1) / (8 * hmax); mcuy = (hgt + 8 * vmax - 1) / (8 * vmax);
                for (c = 0; c < nc; c++) {
                    pw[c] = mcux * 8 * ch[c]; ph[c] = mcuy * 8 * cv[c];
                    pl[c] = (uint8_t *)malloc((size_t)pw[c] * (size_t)ph[c]);
                    if (!pl[c]) { rc = 5; goto fail; }
                    memset(pl[c], 128, (size_t)pw[c] * (size_t)ph[c]);
                }
                b.p = d + pos + 2 + len; b.e = d + n; b.cur = 0; b.nb = 0; b.mk = 0;
                for (my = 0; my < mcuy; my++)
                    for (mx = 0; mx < mcux; mx++) {
                        for (c = 0; c < nc; c++) {
                            int vy, hx;
                            for (vy = 0; vy < cv[c]; vy++)
                                for (hx = 0; hx < ch[c]; hx++) {
                                    int blk[64], t, kk;
                                    memset(blk, 0, sizeof blk);
                                    t = jh_dec(&b, &hd[ctd[c]]);
                                    if (t < 0 || t > 15) { rc = 4; goto fail; }
                                    pred[c] += t ? jext(jb_bits(&b, t), t) : 0;
                                    blk[0] = pred[c] * (int)qt[cq[c]][0];
                                    for (kk = 1; kk < 64;) {
                                        int rs = jh_dec(&b, &ha[cta[c]]), r, sz;
                                        if (rs < 0) { rc = 4; goto fail; }
                                        r = rs >> 4; sz = rs & 15;
                                        if (!sz) { if (r == 15) { kk += 16; continue; } break; }
                                        kk += r;
                                        if (kk > 63) { rc = 4; goto fail; }
                                        blk[jz_zz[kk]] = jext(jb_bits(&b, sz), sz) * (int)qt[cq[c]][kk];
                                        kk++;
                                    }
                                    jidct(blk, pl[c] + (size_t)((my * cv[c] + vy) * 8) * (size_t)pw[c] + (size_t)((mx * ch[c] + hx) * 8), pw[c]);
                                }
                        }
                        cnt++;
                        if (dri && cnt % dri == 0 && (my < mcuy - 1 || mx < mcux - 1)) {
                            while (b.p + 1 < b.e && !(b.p[0] == 0xFF && b.p[1] >= 0xD0 && b.p[1] <= 0xD7)) b.p++;
                            if (b.p + 1 < b.e) b.p += 2;
                            b.nb = 0; b.mk = 0; pred[0] = pred[1] = pred[2] = pred[3] = 0;
                        }
                    }
                px = (uint32_t *)malloc((size_t)w * (size_t)hgt * 4);
                if (!px) { rc = 5; goto fail; }
                for (my = 0; my < hgt; my++)
                    for (mx = 0; mx < w; mx++) {
                        int Y = pl[0][(size_t)(my * cv[0] / vmax) * (size_t)pw[0] + (size_t)(mx * ch[0] / hmax)], r, g, bl;
                        if (nc == 3) {
                            int Cb = pl[1][(size_t)(my * cv[1] / vmax) * (size_t)pw[1] + (size_t)(mx * ch[1] / hmax)] - 128;
                            int Cr = pl[2][(size_t)(my * cv[2] / vmax) * (size_t)pw[2] + (size_t)(mx * ch[2] / hmax)] - 128;
                            r = Y + ((91881 * Cr) >> 16); g = Y - ((22554 * Cb + 46802 * Cr) >> 16); bl = Y + ((116130 * Cb) >> 16);
                            r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; bl = bl < 0 ? 0 : bl > 255 ? 255 : bl;
                        } else r = g = bl = Y;
                        px[(size_t)my * (size_t)w + (size_t)mx] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
                    }
                for (c = 0; c < 4; c++) free(pl[c]);
                *out = px; *ow = w; *oh = hgt;
                return 0;
            }
        }
        pos += 2 + (size_t)len;
    }
    rc = 4;
fail:
    for (i = 0; i < 4; i++) free(pl[i]);
    free(px);
    return rc;
}


/* ---- Arcade Station v0.3: marcos redondeados y iconos de neon (menu principal y selector de ROM) ---- */
static void hr_rfill(int x0, int y0, int x1, int y1, int r, uint32_t col, int a)
{
    int dx, dy;
    hr_rect(x0 + r, y0, x1 - r, y1, col, a);
    hr_rect(x0, y0 + r, x0 + r, y1 - r, col, a);
    hr_rect(x1 - r, y0 + r, x1, y1 - r, col, a);
    for (dy = 0; dy < r; dy++)
        for (dx = 0; dx < r; dx++) {
            int ex = r - 1 - dx, ey = r - 1 - dy;
            if (ex * ex + ey * ey <= r * r) {
                hr_px(x0 + dx, y0 + dy, col, a); hr_px(x1 - 1 - dx, y0 + dy, col, a);
                hr_px(x0 + dx, y1 - 1 - dy, col, a); hr_px(x1 - 1 - dx, y1 - 1 - dy, col, a);
            }
        }
}
static void hr_rframe(int x0, int y0, int x1, int y1, int r, int th, uint32_t col, int a)
{
    int dx, dy, ro = r * r, ri = r - th > 0 ? (r - th) * (r - th) : 0;
    hr_rect(x0 + r, y0, x1 - r, y0 + th, col, a);
    hr_rect(x0 + r, y1 - th, x1 - r, y1, col, a);
    hr_rect(x0, y0 + r, x0 + th, y1 - r, col, a);
    hr_rect(x1 - th, y0 + r, x1, y1 - r, col, a);
    for (dy = 0; dy < r; dy++)
        for (dx = 0; dx < r; dx++) {
            int ex = r - 1 - dx, ey = r - 1 - dy, d2 = ex * ex + ey * ey;
            if (d2 <= ro && d2 >= ri) {
                hr_px(x0 + dx, y0 + dy, col, a); hr_px(x1 - 1 - dx, y0 + dy, col, a);
                hr_px(x0 + dx, y1 - 1 - dy, col, a); hr_px(x1 - 1 - dx, y1 - 1 - dy, col, a);
            }
        }
}
/* cuadro de neon: relleno oscuro + halo en 3 capas + tubo */
static void hr_nrect(int x0, int y0, int x1, int y1, int r, int th, uint32_t col, int fill_a)
{
    int g;
    if (fill_a) hr_rfill(x0, y0, x1, y1, r, 0xFF0A0220u, fill_a);
    for (g = 3; g >= 1; g--) hr_rframe(x0 - g * 4, y0 - g * 4, x1 + g * 4, y1 + g * 4, r + g * 4, th + g * 4, col, g == 3 ? 14 : g == 2 ? 24 : 40);
    hr_rframe(x0, y0, x1, y1, r, th, col, 256);
}
static void hr_ellipse(int cx, int cy, int rx, int ry, int th, uint32_t col, int a)
{
    int x, y;
    int64_t rxo = rx + th / 2, ryo = ry + th / 2, rxi = rx - th / 2, ryi = ry - th / 2;
    for (y = cy - (int)ryo; y <= cy + (int)ryo; y++)
        for (x = cx - (int)rxo; x <= cx + (int)rxo; x++) {
            int64_t dx = x - cx, dy = y - cy;
            int in_o = dx * dx * ryo * ryo + dy * dy * rxo * rxo <= rxo * rxo * ryo * ryo;
            int in_i = rxi > 0 && ryi > 0 && dx * dx * ryi * ryi + dy * dy * rxi * rxi < rxi * rxi * ryi * ryi;
            if (in_o && !in_i) hr_px(x, y, col, a);
        }
}
static void hr_dot(int cx, int cy, int r, uint32_t col, int a)
{
    int x, y;
    for (y = cy - r; y <= cy + r; y++)
        for (x = cx - r; x <= cx + r; x++)
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) hr_px(x, y, col, a);
}
/* iconos de neon: 0 = ROM, 1 = dos mandos, 2 = mundo, 3 = engranaje. s = escala x256 */
static void hr_icon(int kind, int cx, int cy, int s, uint32_t col)
{
    int pass;
    uint32_t core = 0xFF000000u | ((uint32_t)((((col >> 16) & 255) * 90 + 255 * 166) >> 8) << 16) | ((uint32_t)((((col >> 8) & 255) * 90 + 255 * 166) >> 8) << 8) | (uint32_t)(((col & 255) * 90 + 255 * 166) >> 8);
#define SC(v) ((v) * s / 256)
    for (pass = 0; pass < 3; pass++) {
        int ex = pass == 0 ? 12 : pass == 1 ? 5 : 0, a = pass == 0 ? 26 : pass == 1 ? 60 : 256, h = ex / 2;
        int th = SC(7) + ex;
        uint32_t c = pass == 2 ? core : col;
        if (kind == 0) {
            int x0 = cx - SC(40), x1 = cx + SC(40), y0 = cy - SC(46), y1 = cy + SC(46);
            hr_rect(x0 - h, y0 - h, x1 + h, y0 + th - h, c, a); hr_rect(x0 - h, y1 - th + h, x1 + h, y1 + h, c, a);
            hr_rect(x0 - h, y0 - h, x0 + th - h, y1 + h, c, a); hr_rect(x1 - th + h, y0 - h, x1 + h, y1 + h, c, a);
            hr_rect(cx - SC(24) - h, y0 - h, cx + SC(24) + h, cy - SC(20) + h, c, a);
            hr_rect(cx - SC(22) - h, cy + SC(6) - h, cx + SC(22) + h, cy + SC(14) + h, c, a);
        } else if (kind == 1) {
            int k;
            for (k = -1; k <= 1; k += 2) {
                int x = cx + k * SC(34);
                hr_ellipse(x, cy, SC(26), SC(26), th, c, a);
                hr_rect(x - SC(12) - h, cy - SC(3) - h, x + SC(12) + h, cy + SC(3) + h, c, a);
                hr_rect(x - SC(3) - h, cy - SC(12) - h, x + SC(3) + h, cy + SC(12) + h, c, a);
            }
        } else if (kind == 2) {
            hr_ellipse(cx, cy, SC(44), SC(44), th, c, a);
            hr_ellipse(cx, cy, SC(18), SC(44), th * 3 / 4, c, a);
            hr_rect(cx - SC(44), cy - SC(2) - h, cx + SC(44), cy + SC(3) + h, c, a);
        } else {
            static const int ox[8] = { 38, 27, 0, -27, -38, -27, 0, 27 }, oy[8] = { 0, 27, 38, 27, 0, -27, -38, -27 };
            int k;
            hr_ellipse(cx, cy, SC(20), SC(20), th, c, a);
            for (k = 0; k < 8; k++) hr_dot(cx + SC(ox[k]), cy + SC(oy[k]), SC(8) + h, c, a);
        }
    }
#undef SC
}

/* ---- v1.9.8: cuadro de caratula en ELEGIR ROM (carpeta USRDIR/covers, mismo nombre que la ROM, .png) ---- */
#define CV_BX0 850            /* cuadro de la caratula (marco; las caratulas de arcade son verticales) */
#define CV_BX1 1100
#define CV_BY0 170
#define CV_BY1 500
#define CV_BW  242            /* area de imagen dentro del marco */
#define CV_BH  322
enum { CVS_PEND = 0, CVS_OK, CVS_NONE, CVS_BAD, CVS_BIG, CVS_UNSUP };
static uint32_t g_cv_px[CV_BW * CV_BH];
static int g_cv_w, g_cv_h, g_cv_state;
static char g_cv_file[ROM_NAMELEN + 8];      /* nombre de la caratula esperada: <ROM sin extension>.jpg ("" = no hay juego elegido) */

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
/* busca en covers/ un .jpg (o .jpeg / .png) que se llame como la ROM (sin distinguir mayusculas). 1 = lo encontro y deja la ruta en path */
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
        const char *x = e->d_name + bl + 1;
        if (n < bl + 4 || n > bl + 5) continue;
        if (!cv_ceq(e->d_name, romname, bl) || e->d_name[bl] != '.') continue;
        if (!((n == bl + 4 && (cv_ceq(x, "jpg", 3) || cv_ceq(x, "png", 3))) || (n == bl + 5 && cv_ceq(x, "jpeg", 4)))) continue;
        snprintf(path, cap, "%s/%s", dir, e->d_name);
        found = 1;
        break;
    }
    closedir(d);
    return found;
}
/* carga la caratula de la ROM g_roms[idx] (o deja el motivo en g_cv_state) */
static void cv_update_name(const char *romname)
{
    char path[420];
    uint8_t *b = NULL;
    size_t bs = 0;
    uint32_t *px = NULL;
    int w = 0, h = 0, rc;
    struct stat st;
    snprintf(g_cv_file, sizeof g_cv_file, "%.*s.jpg", (int)rom_base_len(romname), romname);
    g_cv_state = CVS_NONE;
    if (!cv_find(romname, path, sizeof path)) return;
    if (stat(path, &st) != 0) return;
    if ((unsigned long)st.st_size > CV_MAXFILE) { g_cv_state = CVS_BIG; logf_("caratula: %s pesa mas de 3 MB, no se muestra", g_cv_file); return; }
    if (!read_whole_file(path, &b, &bs)) { g_cv_state = CVS_BAD; return; }
    rc = (bs >= 2 && b[0] == 0xFF && b[1] == 0xD8) ? cv_jpg_decode(b, bs, 0xFF02050Eu, &px, &w, &h) : cv_png_decode(b, bs, 0xFF02050Eu, &px, &w, &h);
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
static void cv_update(int idx) { cv_update_name(g_roms[idx].name); }
static void cv_draw(void)
{
    int cx = (CV_BX0 + CV_BX1) / 2, y, x0, y0;
    char t[ROM_NAMELEN + 8];
    hr_nrect(CV_BX0, CV_BY0, CV_BX1, CV_BY1, 12, 4, HC_RED, 235);
    if (g_cv_state == CVS_PEND) return;
    if (g_cv_state == CVS_OK) {
        x0 = CV_BX0 + 4 + (CV_BW - g_cv_w) / 2;
        y0 = CV_BY0 + 4 + (CV_BH - g_cv_h) / 2;
        for (y = 0; y < g_cv_h; y++) memcpy(&g_hr[(y0 + y) * HR_W + x0], &g_cv_px[(size_t)y * CV_BW], (size_t)g_cv_w * 4);
        return;
    }
    if (!g_cv_file[0]) {
        hr_textc(cx, 320, "Sin juego", FS_M, HC_MUTE);
        hr_textc(cx, 358, "elegido", FS_S, HC_MUTE);
        return;
    }
    hr_textc(cx, 290, g_cv_state == CVS_NONE ? "Sin carátula" : g_cv_state == CVS_BIG ? "Imagen muy grande" : g_cv_state == CVS_UNSUP ? "No compatible" : "Archivo inválido", FS_M, HC_DIM);
    hr_textc(cx, 336, g_cv_state == CVS_NONE ? "Pon un JPG en" : g_cv_state == CVS_BIG ? "máx. 1024 px y 3 MB" : g_cv_state == CVS_UNSUP ? "usa JPG normal," : "Revisa el archivo:", FS_S, HC_DIM);
    if (g_cv_state == CVS_NONE) hr_textc(cx, 362, "la carpeta covers:", FS_S, HC_DIM);
    else if (g_cv_state == CVS_UNSUP) hr_textc(cx, 362, "no progresivo", FS_S, HC_DIM);
    fit_px(t, sizeof t, g_cv_file, CV_BW - 8, FS_S);
    hr_textc(cx, 408, t, FS_S, HC_YEL);
}

/* Selector de ROM: lista de USRDIR/roms. 1 = eligio y cargo una ROM, 0 = volvio sin elegir.
 * v1.9.2: con pick_only = 1 (el host en plena partida online) NO carga nada: deja nombre, CRC32 y tamano de la ROM elegida
 * en g_pick_* y devuelve 1; la partida sigue en marcha por debajo (ver gsw_ask_host). */
static int ui_rom_select(int pick_only)
{
    enum { VIS = 7 };
    int sel = 0, top = 0, i, last_sel = -1, hold = 0;
    unsigned long dk = ~0ul;
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
                { ui_message("NO PUDE LEER LA ROM", g_roms[sel].name, 300); dk = ~0ul; }
            } else if (e & (UI_OK | UI_START)) {
                ui_show("CARGANDO ROM...", g_roms[sel].name, NULL); dk = ~0ul;
                if (rom_load(sel)) {
                    snprintf(g_last_rom, sizeof g_last_rom, "%s", g_rom_name);     /* v1.9: la recuerda para la proxima vez */
                    opts_save();
                    return 1;
                }
                { ui_message("NO PUDE LEER LA ROM", g_roms[sel].name, 300); dk = ~0ul; }
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
            unsigned long key = (unsigned long)sel * 131ul + (unsigned long)top * 7919ul + (unsigned long)g_cv_state * 17ul + (unsigned long)g_nroms * 3ul + (g_nroms > 0 && g_roms[sel].has_crc ? 5ul : 0ul) + 1ul;
            if (key != dk) {                       /* solo se redibuja cuando algo cambia */
            dk = key;
            hr_begin();
            hr_text(70, 100, "Elige tu juego", FS_T, HC_GRID, 0);
            if (g_nroms == 0) {
                hr_panel(70, 130, 1210, 400);
                hr_text(110, 200, "NO HAY ROMS EN LA CARPETA:", FS_L, HC_WHITE, 2);
                hr_text(110, 250, "ARCA00001/USRDIR/roms", FS_L, HC_YEL, 2);
                hr_text(110, 310, "Pon ahí tus juegos arcade (.zip) y vuelve a entrar aquí.", FS_M, HC_DIM, 2);
                hr_hints(hh0, 1);
            } else {
                snprintf(line, sizeof line, "%d / %d", sel + 1, g_nroms);
                hr_text(1210 - hr_tw(line, FS_M), 72, line, FS_M, HC_DIM, 2);
                hr_nrect(60, 130, 720, 590, 14, 3, HC_GRID, 205);
                for (i = 0; i < VIS && top + i < g_nroms; i++) {
                    int k = top + i, yy = 146 + i * 62;
                    char tt[ROM_NAMELEN + 4], nm2[ROM_NAMELEN + 4];
                    rom_title(tt, sizeof tt, g_roms[k].name);
                    fit_px(nm2, sizeof nm2, tt, 540, FS_L);
                    if (k == sel) {
                        hr_selbar(80, yy, 700, yy + 56);
                        hr_tri(96, yy + 28, 22);
                        hr_text(128, yy + 39, nm2, FS_L, HC_RED, 0);
                    } else hr_text(128, yy + 39, nm2, FS_L, HC_DIM, 0);
                }
                if (top > 0) hr_arrow(390, 138, 1);
                if (top + VIS < g_nroms) hr_arrow(390, 582, 0);
                cv_draw();
                {
                    char tt[ROM_NAMELEN + 4], nm2[ROM_NAMELEN + 4];
                    rom_title(tt, sizeof tt, g_roms[sel].name);
                    fit_px(nm2, sizeof nm2, tt, 330, FS_L);
                    hr_textc(975, 560, nm2, FS_L, HC_RED);
                }
                if (g_roms[sel].has_crc) snprintf(line, sizeof line, "%u KB   CRC32 %08X", (unsigned)((g_roms[sel].size + 512) / 1024), (unsigned)g_roms[sel].crc);
                else snprintf(line, sizeof line, "%u KB   CRC32 ........", (unsigned)((g_roms[sel].size + 512) / 1024));
                hr_textc(975, 602, line, FS_S, HC_DIM);
                hr_hints(hh1, 3);
            }
            } else g_hr_on = 1;                   /* sin cambios: se vuelve a copiar el cuadro ya dibujado */
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
    int i, dk = -1;
    char cv_for[ROM_NAMELEN];
    g_stage = "menu";
    cv_for[0] = 0; g_cv_file[0] = 0; g_cv_state = CVS_NONE;
    ui_prime();
    while (g_running) {
        uint16_t e;
        char l[80], nm[48];
        pump();
        e = ui_edges();
        if (hr_usable()) {                    /* cuadricula 2x2: arriba/abajo cambian de fila, izquierda/derecha de columna */
            if (e & (UI_UP | UI_DOWN)) g_menu_sel ^= 2;
            if (e & (UI_LEFT | UI_RIGHT)) g_menu_sel ^= 1;
        } else {
            if (e & UI_UP)   g_menu_sel = (g_menu_sel + 3) % 4;
            if (e & UI_DOWN) g_menu_sel = (g_menu_sel + 1) % 4;
        }
        if (e & UI_CANCEL) {
            /* v1.9.4: O en el menu principal sale al XMB, sin confirmar (igual que la salida del sistema: g_running = 0) */
            logf_("salida al XMB pedida con O en el menu principal");
            g_running = 0;
            return -1;
        }
        if (e & (UI_OK | UI_START)) {
            /* sin ROM elegida el juego queda en stand-by: solo se puede entrar a ELEGIR ROM y a OPCIONES */
            if (modes[g_menu_sel] != MODE_ROM && modes[g_menu_sel] != MODE_OPT && !g_rom) { ui_message("PRIMERO ELIGE UN JUEGO", "OPCION: ELEGIR JUEGO", 120); dk = -1; }
            else return modes[g_menu_sel];
        }
        if (hr_usable()) {
            static const HHint hh[2] = { { "X", "ELEGIR", HC_BX }, { "O", "SALIR", HC_BO } };
            static const char *lab[4] = { "ELEGIR JUEGO", "JUGAR LOCAL", "JUGAR ONLINE", "OPCIONES" };
            static const char *sub[4] = { "Tus juegos", "1 a 4 mandos", "2 a 4 jugadores", "Ajustes" };
            unsigned dis = 0;
            int key;
            for (i = 0; i < 4; i++) if (!g_rom && modes[i] != MODE_ROM && modes[i] != MODE_OPT) dis |= 1u << i;
            if (g_rom && strcmp(cv_for, g_rom_name)) { snprintf(cv_for, sizeof cv_for, "%s", g_rom_name); cv_update_name(g_rom_name); }
            key = g_menu_sel * 16 + (g_rom ? 8 : 0) + g_cv_state + 1;
            if (key != dk) {                       /* solo se redibuja cuando algo cambia (el cuadro queda en g_hr) */
                dk = key;
                hr_begin();
                hr_text(56, 112, "Arcade Station", FS_T, HC_RED, 0);
                hr_text(76, 160, "LOCAL Y ONLINE PARA PS3", FS_M, HC_GRID, 0);
                for (i = 0; i < 4; i++) {
                    int sel = (i == g_menu_sel), off = (dis >> i) & 1;
                    int x0 = 70 + (i & 1) * 340, y0 = 190 + (i >> 1) * 212, x1 = x0 + 310, y1 = y0 + 188, tx;
                    uint32_t col = sel ? HC_RED : off ? HC_MUTE : HC_GRID;
                    if (sel) { x0 -= 8; y0 -= 8; x1 += 8; y1 += 8; }
                    hr_nrect(x0, y0, x1, y1, 14, sel ? 5 : 3, col, 205);
                    tx = (x0 + x1) / 2;
                    hr_icon(i, tx, y0 + (sel ? 66 : 60), sel ? 248 : 214, col);
                    hr_textc(tx, y1 - (sel ? 50 : 44), lab[i], sel ? FS_L : FS_M, col);
                    hr_textc(tx, y1 - 16, sub[i], FS_S, off ? HC_MUTE : HC_DIM);
                }
                cv_draw();
                if (g_rom) {
                    char tt[ROM_NAMELEN + 4], nm2[ROM_NAMELEN + 4];
                    rom_title(tt, sizeof tt, g_rom_name);
                    fit_px(nm2, sizeof nm2, tt, 330, FS_L);
                    hr_textc(975, 566, nm2, FS_L, HC_RED);
                    hr_textc(975, 604, "JUEGO ELEGIDO", FS_S, HC_DIM);
                } else {
                    hr_textc(975, 566, "SIN JUEGO", FS_L, HC_MUTE);
                    hr_textc(975, 604, "Elige uno en ELEGIR JUEGO", FS_S, HC_DIM);
                }
                hr_hints(hh, 2);
            } else g_hr_on = 1;                   /* sin cambios: se vuelve a copiar el cuadro ya dibujado (cada buffer de pantalla lo necesita) */
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
