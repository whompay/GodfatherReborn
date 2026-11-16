/* ui_i18n.c -- translations of the port's own UI text (overlay menus, notices, stores, main-menu entry).
 *
 * ui_tr(en) returns the text for the game language (PS3_LANG, the same setting the game itself uses), or `en` when there
 * is no translation. Keys are the exact English strings in the source, including printf formats (a translated format
 * keeps the same conversions in the same order). Strings are UTF-8. UI_TR=0 turns translations off. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

typedef struct { const char* en; const char* tr; } UiTr;

static const UiTr k_es[] = {
    /* main menu */
    { "Quit to Desktop", "Salir al escritorio" },
    /* pause menu: PC settings / co-op */
    { "Settings", "Ajustes" }, { "Back   Settings & Quit", "Volver   Ajustes y salir" }, { "Quit Game", "Salir del juego" }, { "Back", "Volver" },
    { "PC Settings", "Ajustes de PC" }, { "CO-OP", "COOPERATIVO" }, { "PC SETTINGS", "AJUSTES DE PC" },
    { "Player Name", "Nombre del jugador" }, { "Host a Game", "Crear partida" }, { "Join Address", "Dirección" },
    { "Join Game", "Unirse a partida" }, { "Leave Co-op", "Salir del cooperativo" },
    { "Up/Down  Select", "Arriba/Abajo  Elegir" }, { "Enter  Choose", "Intro  Aceptar" }, { "Esc  Back", "Esc  Volver" },
    { "Type  Edit", "Escribe  Editar" }, { "Enter  Keep", "Intro  Guardar" }, { "Esc  Cancel", "Esc  Cancelar" },
    { "Up/Down  Scroll", "Arriba/Abajo  Desplazar" }, { "Left/Right  Change", "Izq./Der.  Cambiar" },
    { "Enter  Select", "Intro  Elegir" }, { "F10  Back", "F10  Volver" }, { "F10 / Back", "F10 / Volver" },
    { "SETTINGS MARKED (RESTART) APPLY THE NEXT TIME THE GAME STARTS.",
      "LOS AJUSTES MARCADOS (REINICIO) SE APLICAN LA PRÓXIMA VEZ QUE INICIES EL JUEGO." },
    { "(RESTART)", "(REINICIO)" },
    { "QUIT THE GAME? UNSAVED PROGRESS WILL BE LOST.", "¿SALIR DEL JUEGO? SE PERDERÁ EL PROGRESO NO GUARDADO." },
    { "QUIT", "SALIR" }, { "CANCEL", "CANCELAR" }, { "YES", "SÍ" }, { "NO", "NO" },
    { "Display Mode", "Modo de pantalla" }, { "Windowed", "Ventana" }, { "Fullscreen", "Pantalla completa" },
    { "VSync", "Sincronización vertical" }, { "Off", "No" }, { "On", "Sí" },
    { "Frame Rate Limit", "Límite de FPS" }, { "Button Prompts", "Iconos de botones" },
    { "Automatic", "Automático" }, { "Controller", "Mando" }, { "Keyboard & Mouse", "Teclado y ratón" },
    { "Mouse Speed", "Velocidad del ratón" }, { "1 (Slow)", "1 (Lenta)" }, { "5 (Fast)", "5 (Rápida)" },
    { "Resolution", "Resolución" }, { "Shadows", "Sombras" }, { "Shadow Quality", "Calidad de sombras" },
    { "Original", "Original" }, { "High (2x)", "Alta (2x)" }, { "Ultra (4x)", "Ultra (4x)" },
    { "Texture Filtering", "Filtrado de texturas" }, { "Game Default", "Por defecto" }, { "Bilinear", "Bilineal" },
    { "4x Anisotropic", "Anisotrópico 4x" }, { "8x Anisotropic", "Anisotrópico 8x" }, { "16x Anisotropic", "Anisotrópico 16x" },
    { "Mipmaps", "Mipmaps" }, { "Texture Upscaling", "Reescalado de texturas" },
    { "Ambient Occlusion", "Oclusión ambiental" }, { "Strong", "Fuerte" }, { "Bloom", "Resplandor" },
    { "Low", "Bajo" }, { "High", "Alto" }, { "Color Grading", "Corrección de color" }, { "Film", "Película" },
    { "Contrast", "Contraste" }, { "Sharpening", "Nitidez" }, { "Wet Streets", "Calles mojadas" },
    { "When Raining", "Cuando llueve" }, { "When Raining (Subtle)", "Cuando llueve (suave)" }, { "Always", "Siempre" },
    { "Upscaling", "Reescalado" }, { "Frame Generation", "Generación de fotogramas" }, { "On (FSR)", "Sí (FSR)" },
    { "Language", "Idioma" }, { "English (US)", "Inglés (EE. UU.)" }, { "English (UK)", "Inglés (R. U.)" },
    { "French", "Francés" }, { "German", "Alemán" }, { "Italian", "Italiano" }, { "Spanish", "Español" },
    { "Dutch", "Neerlandés" }, { "Polish", "Polaco" }, { "Portuguese", "Portugués" }, { "Russian", "Ruso" },
    { "Korean (text)", "Coreano (texto)" }, { "Chinese (text)", "Chino (texto)" },
    /* store / inventory */
    { "Store", "Tienda" }, { "Inventory", "Inventario" }, { "FOR SALE", "A LA VENTA" }, { "YOUR POCKETS", "TUS BOLSILLOS" },
    { "CARRYING", "LLEVAS" }, { "Buy", "Comprar" }, { "Use", "Usar" }, { "Pockets", "Bolsillos" }, { "Leave", "Salir" },
    { "YOUR CASH", "TU DINERO" }, { "CASH", "DINERO" },
    { "NOTHING FOR SALE HERE", "AQUÍ NO SE VENDE NADA" }, { "YOUR POCKETS ARE EMPTY", "TUS BOLSILLOS ESTÁN VACÍOS" },
    { "YOU ARE NOT CARRYING ANYTHING", "NO LLEVAS NADA" },
    { "D-Pad  Select", "Cruceta  Elegir" }, { "Cross  Buy", "Equis  Comprar" }, { "Enter  Buy", "Intro  Comprar" },
    { "Cross  Use", "Equis  Usar" }, { "Enter  Use", "Intro  Usar" }, { "R1  Inventory", "R1  Inventario" },
    { "Tab  Inventory", "Tab  Inventario" }, { "R1  Store", "R1  Tienda" }, { "Tab  Store", "Tab  Tienda" },
    { "Circle  Back", "Círculo  Volver" },
    { "Tobacconist", "Estanco" }, { "Doctor's Office", "Consulta médica" }, { "Bar", "Bar" }, { "Restaurant", "Restaurante" },
    { "Bakery", "Panadería" }, { "Florist", "Floristería" }, { "Butcher", "Carnicería" }, { "General Store", "Ultramarinos" },
    { "Tailor", "Sastrería" },
    { "Cigarettes", "Cigarrillos" }, { "Havana Cigar", "Puro habano" }, { "Whiskey", "Whisky" },
    { "Bottle of Chianti", "Botella de Chianti" }, { "Plate of Pasta", "Plato de pasta" }, { "Cannoli", "Cannoli" },
    { "Loaf of Bread", "Hogaza de pan" }, { "Salami", "Salami" }, { "Bandages", "Vendas" }, { "Morphine", "Morfina" },
    { "Bouquet of Roses", "Ramo de rosas" },
    { "A pack of twenty. Light one up with J, or use it here. Sprinting, fighting or getting hurt puts it out.",
      "Un paquete de veinte. Enciende uno con J o úsalo aquí. Correr, pelear o recibir daño lo apaga." },
    { "Hand-rolled, from the old country by way of Havana. Smoked like a cigarette, savoured like a reward.",
      "Liado a mano, del viejo país pasando por La Habana. Se fuma como un cigarrillo y se saborea como un premio." },
    { "A stiff drink to steady the nerves. Restores some health.", "Un trago fuerte para templar los nervios. Recupera algo de salud." },
    { "Red wine from Tuscany. Restores a little health.", "Vino tinto de la Toscana. Recupera un poco de salud." },
    { "Mama's recipe. A full meal restores a good share of health.", "La receta de mamá. Una comida completa recupera bastante salud." },
    { "Leave the gun, take the cannoli. Restores a little health.", "Deja la pistola, coge los cannoli. Recupera un poco de salud." },
    { "Fresh this morning. Restores a little health.", "Recién hecho esta mañana. Recupera un poco de salud." },
    { "Cured, sliced thick. Restores a little health.", "Curado y en lonchas gruesas. Recupera un poco de salud." },
    { "Stops the bleeding. Restores a good share of health.", "Corta la hemorragia. Recupera bastante salud." },
    { "The doctor asks no questions. Restores all health.", "El médico no hace preguntas. Recupera toda la salud." },
    { "Red roses. A gift for someone. Not much use in a gunfight.", "Rosas rojas. Un regalo para alguien. No sirven de mucho en un tiroteo." },
    /* notices */
    { "%s  +%d health", "%s  +%d de salud" }, { "%s - keep it for someone special", "%s: guárdalo para alguien especial" },
    { "%s won't serve you - %s", "%s no te atiende: %s" }, { "The shopkeeper", "El tendero" },
    { "Already at full health", "Ya tienes la salud al máximo" }, { "Bought %s  -$%d", "Comprado: %s  -$%d" },
    { "Bought the %s  -$%d", "Comprado: %s  -$%d" }, { "Cigarette in your mouth - K to put it out", "Cigarrillo en la boca: K para apagarlo" },
    { "Flicked the cigarette away", "Has tirado el cigarrillo" }, { "Gabriel doesn't have that in your size yet", "Gabriel aún no lo tiene en tu talla" },
    { "No other patterns for that one", "No hay otros estampados para esa prenda" },
    { "Not enough cash - %s costs $%d", "No tienes suficiente dinero: %s cuesta $%d" },
    { "Not enough cash - the %s costs $%d", "No tienes suficiente dinero: %s cuesta $%d" },
    { "Not now", "Ahora no" }, { "Not sold here", "Aquí no se vende" },
    { "Out of cigarettes - buy a pack at a tobacconist or a bar", "No te quedan cigarrillos: compra un paquete en un estanco o un bar" },
    { "That pattern comes in one colour", "Ese estampado solo viene en un color" }, { "The cigarette burned out", "El cigarrillo se ha consumido" },
    { "The tailor can't find that fabric", "El sastre no encuentra esa tela" }, { "The tailor can't fit you right now", "El sastre no puede tomarte medidas ahora" },
    { "The tailor can't sell that right now", "El sastre no puede venderte eso ahora" }, { "The tailor fumbled - try again", "Al sastre se le ha resbalado: inténtalo de nuevo" },
    { "The tailor is busy - try again in a moment", "El sastre está ocupado: inténtalo dentro de un momento" },
    { "The till is stuck - try again", "La caja se ha atascado: inténtalo de nuevo" }, { "Three cigarettes left", "Quedan tres cigarrillos" },
    { "Wearing the %s", "Llevas puesto: %s" }, { "You can't carry any more", "No puedes llevar más" },
    { "%s timed out (connection lost)", "%s: tiempo agotado (conexión perdida)" }, { "Calling the host at %s", "Llamando al anfitrión en %s" },
    { "Could not open UDP port %d (is another copy of the game hosting?)", "No se pudo abrir el puerto UDP %d (¿hay otra copia del juego como anfitrión?)" },
    { "Echo test session", "Sesión de prueba (eco)" }, { "Hosting on UDP port %d", "Anfitrión en el puerto UDP %d" },
    { "Left co-op", "Has salido del cooperativo" }, { "Refused by the host: %s", "El anfitrión te ha rechazado: %s" },
    { "Refused: the host's game is full", "Rechazado: la partida del anfitrión está llena" },
    { "\"%s\" is not a usable address", "\"%s\" no es una dirección válida" },
    /* street dialogue */
    { "Say hello", "Saludar" }, { "What's the word?", "¿Qué se cuenta?" }, { "Intimidate", "Intimidar" }, { "Flirt", "Ligar" },
    { "Talk", "Hablar" }, { "Hold", "Mantén" }, { "to skip", "para saltar" }, { "Leaderboards", "Clasificaciones" }, { "Co-op", "Cooperativo" }, { "You", "Tú" }, { "TALK", "HABLAR" }, { "T  TALK", "T  HABLAR" },
    /* misc HUD */
    { "Skipping...", "Saltando..." }, { "Hold", "Mantén" }, { "NO AMMO", "SIN MUNICIÓN" }, { "EMPTY", "VACÍO" },
    { "Weapon", "Arma" }, { "QUICK SLOTS", "ACCESOS RÁPIDOS" }, { "MOUSE  SELECT", "RATÓN  ELEGIR" }, { "R  SELECT", "R  ELEGIR" },
    { "RELEASE Q  EQUIP", "SUELTA Q  EQUIPAR" }, { "RELEASE L2  EQUIP", "SUELTA L2  EQUIPAR" },
};

static const UiTr* s_tab; static int s_n = -1;
enum { HN = 1024 };
static short s_h[HN];   /* open-addressing index into s_tab, -1 = empty */

static uint32_t h_str(const char* s) { uint32_t h = 2166136261u; while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; } return h; }

/* No PS3_LANG: a single-language disc (e.g. BLES00047, PS3_GAME/USRDIR/text/localetable.txt line 3 "pal_es") plays in
 * its own language whatever the system language is -- follow the disc. argv[1] is the EBOOT.elf in USRDIR. */
static const char* disc_lang(void)
{
#ifdef _WIN32
    static char lang[16];
    int n = 0; LPWSTR* av = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!av) return NULL;
    wchar_t path[1024]; path[0] = 0;
    if (n > 1 && wcslen(av[1]) < 900) {
        wcscpy(path, av[1]); wchar_t* s = wcsrchr(path, L'\\'); wchar_t* s2 = wcsrchr(path, L'/'); if (s2 > s) s = s2;
        if (s) wcscpy(s + 1, L"text\\localetable.txt"); else path[0] = 0;
    }
    LocalFree(av);
    FILE* f = path[0] ? _wfopen(path, L"rb") : NULL;
    if (!f) return NULL;
    char line[64]; int i = 0; const char* r = NULL;
    while (fgets(line, sizeof line, f) && ++i <= 3) {
        if (i < 3) continue;
        if (!strncmp(line, "pal_es", 6)) r = "spanish";
        else if (!strncmp(line, "pal_fr", 6)) r = "french";
        else if (!strncmp(line, "pal_it", 6)) r = "italian";
        else if (!strncmp(line, "pal_de", 6)) r = "german";
    }
    fclose(f);
    if (r) { snprintf(lang, sizeof lang, "%s", r); return lang; }
#endif
    return NULL;
}

static void ui_tr_init(void)
{
    const char* off = getenv("UI_TR"); const char* l = getenv("PS3_LANG");
    if (!l || !*l) l = disc_lang();
    int n = 0;
    if (!(off && *off == '0') && l && !strcmp(l, "spanish")) { s_tab = k_es; n = (int)(sizeof k_es / sizeof k_es[0]); }
    for (int i = 0; i < HN; i++) s_h[i] = -1;
    for (int i = 0; i < n; i++) {
        uint32_t k = h_str(s_tab[i].en) & (HN - 1);
        while (s_h[k] >= 0) k = (k + 1) & (HN - 1);
        s_h[k] = (short)i;
    }
    s_n = n;   /* last: another thread may call ui_tr meanwhile */
}

const char* ui_tr(const char* en)
{
    if (s_n < 0) ui_tr_init();
    if (!en || !s_n) return en;
    for (uint32_t k = h_str(en) & (HN - 1); s_h[k] >= 0; k = (k + 1) & (HN - 1))
        if (!strcmp(s_tab[s_h[k]].en, en)) return s_tab[s_h[k]].tr;
    return en;
}

/* 1 when the port's UI is translated (callers that build text from pieces can pick a word order). */
int ui_tr_active(void) { if (s_n < 0) ui_tr_init(); return s_n > 0; }
