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

/* ---- logo "Arcade Station" (lettering del usuario): PNG con transparencia incrustado; se decodifica una vez sobre negro y sobre blanco para recuperar el alfa ---- */
static const char logo_b64[] =
"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"
"SUVORK5CYII=";
static uint32_t *g_logo;                   /* alfa en el byte alto + color ya multiplicado por el alfa */
static int g_logo_w, g_logo_h, g_logo_tried;

static void logo_load(void)
{
    size_t n = strlen(logo_b64), i, o = 0;
    uint8_t *png;
    uint32_t *pb = NULL, *pw = NULL, acc = 0;
    int w1 = 0, h1 = 0, w2 = 0, h2 = 0, bits = 0;
    if (g_logo_tried) return;
    g_logo_tried = 1;
    png = (uint8_t *)malloc(n * 3 / 4 + 8);
    if (!png) return;
    for (i = 0; i < n; i++) {
        char c = logo_b64[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A'; else if (c >= 'a' && c <= 'z') v = c - 'a' + 26; else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62; else if (c == '/') v = 63; else continue;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; png[o++] = (uint8_t)(acc >> bits); }
    }
    if (cv_png_decode(png, o, 0xFF000000u, &pb, &w1, &h1) == 0 && cv_png_decode(png, o, 0xFFFFFFFFu, &pw, &w2, &h2) == 0 && w1 == w2 && h1 == h2) {
        g_logo = (uint32_t *)malloc((size_t)w1 * (size_t)h1 * 4);
        if (g_logo) {
            for (i = 0; i < (size_t)w1 * (size_t)h1; i++) {
                int dr = (int)((pw[i] >> 16) & 255) - (int)((pb[i] >> 16) & 255), dg = (int)((pw[i] >> 8) & 255) - (int)((pb[i] >> 8) & 255), db = (int)(pw[i] & 255) - (int)(pb[i] & 255), d = dr > dg ? dr : dg, a;
                if (db > d) d = db;
                a = 255 - d;
                if (a < 0) a = 0;
                g_logo[i] = ((uint32_t)a << 24) | (pb[i] & 0xFFFFFFu);
            }
            g_logo_w = w1; g_logo_h = h1;
        }
    }
    free(pb); free(pw); free(png);
}
static void logo_draw(int x0, int y0)
{
    int x, y;
    if (!g_logo) return;
    for (y = 0; y < g_logo_h; y++) {
        int py = y0 + y;
        if (py < 0 || py >= HR_H) continue;
        for (x = 0; x < g_logo_w; x++) {
            uint32_t p = g_logo[(size_t)y * (size_t)g_logo_w + (size_t)x], *d;
            int a = (int)(p >> 24), inv, px = x0 + x, r, g, b;
            if (!a || px < 0 || px >= HR_W) continue;
            d = &g_hr[py * HR_W + px];
            inv = 255 - a;
            r = (int)((p >> 16) & 255) + ((int)((*d >> 16) & 255) * inv + 127) / 255;
            g = (int)((p >> 8) & 255) + ((int)((*d >> 8) & 255) * inv + 127) / 255;
            b = (int)(p & 255) + ((int)(*d & 255) * inv + 127) / 255;
            *d = 0xFF000000u | ((uint32_t)(r > 255 ? 255 : r) << 16) | ((uint32_t)(g > 255 ? 255 : g) << 8) | (uint32_t)(b > 255 ? 255 : b);
        }
    }
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
                logo_load();
                if (g_logo) logo_draw(56, 28);
                else hr_text(56, 112, "Arcade Station", FS_T, HC_RED, 0);
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
