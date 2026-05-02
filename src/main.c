/*
 * Génération 2D de Rendu de Planète Low Poly
 * ============================================
 * Génère une planète low-poly procédurale avec:
 *  - Triangulation de Delaunay (Bowyer-Watson)
 *  - Bruit de gradient 2D pour élévation et humidité
 *  - Biomes colorés (océan profond, plage, prairie, forêt, montagne, neige)
 *  - Halo atmosphérique, calottes polaires, nuages
 *  - Fond étoilé
 *  - Affichage SDL2 + sauvegarde BMP
 *
 * Compilation: voir Makefile
 * Usage:       ./planet [seed]
 */

#define _USE_MATH_DEFINES
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>

/* ─── Constantes ─────────────────────────────────────────────── */
#define WIN_W         900
#define WIN_H         900
#define PLANET_R      360          /* rayon de la planète (pixels)   */
#define PLANET_CX     (WIN_W / 2)
#define PLANET_CY     (WIN_H / 2)

#define MAX_POINTS    2200         /* points Delaunay dans la planète */
#define MAX_TRIS      8000         /* triangles max                   */
#define MAX_STARS     800

#define NUM_PERLIN    8            /* octaves bruit                   */
#define CLOUD_BANDS   6

/* ─── Types ──────────────────────────────────────────────────── */
typedef struct { double x, y; } Vec2;

typedef struct {
    int a, b, c;   /* indices dans le tableau de points */
} Triangle;

typedef struct {
    int a, b, c;
    double cx, cy, r2; /* cercle circonscrit */
} DelTri;

typedef struct { double r, g, b; } Color;

/* ─── Variables globales ─────────────────────────────────────── */
static Vec2     pts[MAX_POINTS + 4];  /* +4 pour le super-triangle */
static int      npts = 0;

static DelTri   tris[MAX_TRIS];
static int      ntris = 0;

static unsigned int g_seed = 0;

/* ─── PRNG déterministe ──────────────────────────────────────── */
static unsigned int lcg_state;
static void   lcg_seed(unsigned int s) { lcg_state = s; }
static double lcg_rand(void) {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return (lcg_state >> 1) / (double)0x7FFFFFFFu;
}

/* ─── Bruit de gradient 2D simplifié ────────────────────────── */
#define PERM_SIZE 512
static int perm[PERM_SIZE];

static void init_perm(unsigned int seed) {
    int i, j, tmp;
    for (i = 0; i < 256; i++) perm[i] = i;
    lcg_seed(seed);
    for (i = 255; i > 0; i--) {
        j = (int)(lcg_rand() * (i + 1));
        tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
    }
    for (i = 0; i < 256; i++) perm[i + 256] = perm[i];
}

static double fade(double t) { return t * t * t * (t * (t * 6 - 15) + 10); }
static double lerp(double a, double b, double t) { return a + t * (b - a); }

static double grad2(int hash, double x, double y) {
    switch (hash & 7) {
        case 0: return  x + y;
        case 1: return -x + y;
        case 2: return  x - y;
        case 3: return -x - y;
        case 4: return  x;
        case 5: return -x;
        case 6: return  y;
        case 7: return -y;
        default: return 0;
    }
}

static double perlin2(double x, double y) {
    int xi = (int)floor(x) & 255;
    int yi = (int)floor(y) & 255;
    double xf = x - floor(x);
    double yf = y - floor(y);
    double u = fade(xf), v = fade(yf);
    int aa = perm[perm[xi]     + yi];
    int ab = perm[perm[xi]     + yi + 1];
    int ba = perm[perm[xi + 1] + yi];
    int bb = perm[perm[xi + 1] + yi + 1];
    return lerp(lerp(grad2(aa, xf,     yf    ),
                     grad2(ba, xf - 1, yf    ), u),
                lerp(grad2(ab, xf,     yf - 1),
                     grad2(bb, xf - 1, yf - 1), u), v);
}

/* fBm (fractal Brownian motion) */
static double fbm(double x, double y, int octaves,
                  double lacunarity, double gain) {
    double val = 0, amp = 0.5, freq = 1;
    int i;
    for (i = 0; i < octaves; i++) {
        val += amp * perlin2(x * freq, y * freq);
        amp  *= gain;
        freq *= lacunarity;
    }
    return val;
}

/* Bruit ridgé — crée des crêtes montagneuses réalistes */
static double ridged_fbm(double x, double y, int octaves,
                          double lacunarity, double gain) {
    double val = 0, amp = 0.5, freq = 1;
    int i;
    for (i = 0; i < octaves; i++) {
        double n = perlin2(x * freq, y * freq);
        /* Inverser la valeur absolue : crée des pics étroits */
        n = 1.0 - 2.0 * fabs(n);
        val += amp * n * n;  /* carré = pics plus nets */
        amp  *= gain;
        freq *= lacunarity;
    }
    return val * 0.75 - 0.40; /* recentré autour de 0 */
}

/* ─── Helpers géométriques ───────────────────────────────────── */
static double dist2(Vec2 a, Vec2 b) {
    double dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

/* Cercle circonscrit du triangle (p0,p1,p2).
   Retourne 0 si dégénéré, 1 sinon. */
static int circumcircle(Vec2 p0, Vec2 p1, Vec2 p2,
                         double *cx, double *cy, double *r2) {
    double ax = p1.x - p0.x, ay = p1.y - p0.y;
    double bx = p2.x - p0.x, by = p2.y - p0.y;
    double D  = 2 * (ax * by - ay * bx);
    if (fabs(D) < 1e-10) return 0;
    *cx = p0.x + (by * (ax * ax + ay * ay) - ay * (bx * bx + by * by)) / D;
    *cy = p0.y + (ax * (bx * bx + by * by) - bx * (ax * ax + ay * ay)) / D;
    *r2 = dist2(p0, (Vec2){*cx, *cy});
    return 1;
}

/* ─── Bowyer-Watson Delaunay ─────────────────────────────────── */
/* Structures temporaires pour les arêtes polygonales */
typedef struct { int a, b; } Edge;
#define MAX_EDGES 4096
static Edge edge_buf[MAX_EDGES];
static int  nedges;

static void add_edge(int a, int b) {
    if (nedges >= MAX_EDGES) return; /* évite le dépassement de tampon */
    edge_buf[nedges].a = a;
    edge_buf[nedges].b = b;
    nedges++;
}

static void bowyerwatson_insert(int pi) {
    /* Trouver tous les triangles dont le cercle circonscrit contient pi */
    int bad[MAX_TRIS], nbad = 0;
    int i;
    for (i = 0; i < ntris; i++) {
        double dx = pts[pi].x - tris[i].cx;
        double dy = pts[pi].y - tris[i].cy;
        if (dx * dx + dy * dy < tris[i].r2 - 1e-10)
            bad[nbad++] = i;
    }

    /* Construire le polygone frontière (arêtes non partagées) */
    nedges = 0;
    for (i = 0; i < nbad; i++) {
        int a = tris[bad[i]].a;
        int b = tris[bad[i]].b;
        int c = tris[bad[i]].c;
        add_edge(a, b); add_edge(b, c); add_edge(c, a);
    }
    /* Marquer les arêtes doubles */
    int shared[MAX_EDGES];
    memset(shared, 0, sizeof(shared));
    int j;
    for (i = 0; i < nedges; i++)
        for (j = i + 1; j < nedges; j++)
            if ((edge_buf[i].a == edge_buf[j].b &&
                 edge_buf[i].b == edge_buf[j].a) ||
                (edge_buf[i].a == edge_buf[j].a &&
                 edge_buf[i].b == edge_buf[j].b))
                shared[i] = shared[j] = 1;

    /* Supprimer les mauvais triangles */
    int write = 0;
    for (i = 0; i < ntris; i++) {
        int isbad = 0;
        for (j = 0; j < nbad; j++) if (bad[j] == i) { isbad = 1; break; }
        if (!isbad) tris[write++] = tris[i];
    }
    ntris = write;

    /* Créer de nouveaux triangles depuis le polygone frontière vers pi */
    for (i = 0; i < nedges; i++) {
        if (shared[i]) continue;
        int a = edge_buf[i].a, b = edge_buf[i].b;
        double cx, cy, r2;
        if (!circumcircle(pts[a], pts[b], pts[pi], &cx, &cy, &r2)) continue;
        if (ntris >= MAX_TRIS) {
            fprintf(stderr, "Avertissement: limite MAX_TRIS atteinte.\n");
            break;
        }
        tris[ntris].a  = a; tris[ntris].b = b; tris[ntris].c = pi;
        tris[ntris].cx = cx; tris[ntris].cy = cy; tris[ntris].r2 = r2;
        ntris++;
    }
}

static void init_super_triangle(void) {
    /* Super-triangle englobant tout l'écran */
    double M = WIN_W * 20.0;
    pts[npts++] = (Vec2){ WIN_W / 2.0,      -M        };
    pts[npts++] = (Vec2){ WIN_W / 2.0 + M,   M        };
    pts[npts++] = (Vec2){ WIN_W / 2.0 - M,   M        };
    double cx, cy, r2;
    circumcircle(pts[npts-3], pts[npts-2], pts[npts-1], &cx, &cy, &r2);
    tris[ntris].a = npts-3; tris[ntris].b = npts-2; tris[ntris].c = npts-1;
    tris[ntris].cx = cx; tris[ntris].cy = cy; tris[ntris].r2 = r2;
    ntris++;
}

static void remove_super_triangle(void) {
    int super_start = npts - 3; /* indices du super-triangle */
    int write = 0, i;
    for (i = 0; i < ntris; i++) {
        if (tris[i].a >= super_start ||
            tris[i].b >= super_start ||
            tris[i].c >= super_start) continue;
        tris[write++] = tris[i];
    }
    ntris = write;
}

/* ─── Génération des points de la planète ────────────────────── */
static void generate_planet_points(void) {
    int i;
    /* Points réguliers jittered dans un disque légèrement plus grand
       pour que les bords soient bien triangulés */
    int grid = (int)sqrt((double)MAX_POINTS * 1.4);
    double step = (PLANET_R * 2.2) / grid;
    double ox = PLANET_CX - PLANET_R * 1.1;
    double oy = PLANET_CY - PLANET_R * 1.1;

    for (i = 0; i < grid * grid && npts < MAX_POINTS; i++) {
        int gx = i % grid, gy = i / grid;
        double px = ox + (gx + lcg_rand()) * step;
        double py = oy + (gy + lcg_rand()) * step;
        double dx = px - PLANET_CX, dy = py - PLANET_CY;
        double r  = sqrt(dx * dx + dy * dy);
        /* Inclure les points légèrement hors disque pour bordure propre */
        if (r <= PLANET_R * 1.05)
            pts[npts++] = (Vec2){ px, py };
    }
    /* Quelques points sur la circonférence exacte */
    int rim = 80;
    for (i = 0; i < rim && npts < MAX_POINTS; i++) {
        double a = 2 * M_PI * i / rim;
        pts[npts++] = (Vec2){ PLANET_CX + PLANET_R * cos(a),
                              PLANET_CY + PLANET_R * sin(a) };
    }
}

/* ─── Couleur de biome ───────────────────────────────────────── */
typedef struct {
    double h;   /* seuil de hauteur */
    Color  col;
} BiomeEntry;

static BiomeEntry BIOME[] = {
    { -1.20, { 0.04, 0.10, 0.28 } }, /* océan abyssal       */
    { -0.55, { 0.06, 0.18, 0.45 } }, /* océan profond       */
    { -0.18, { 0.09, 0.32, 0.64 } }, /* mer                 */
    {  0.00, { 0.13, 0.48, 0.78 } }, /* eau peu profonde    */
    {  0.03, { 0.80, 0.74, 0.50 } }, /* plage / sable       */
    {  0.12, { 0.38, 0.65, 0.25 } }, /* prairie             */
    {  0.28, { 0.18, 0.46, 0.16 } }, /* forêt dense         */
    {  0.40, { 0.26, 0.54, 0.20 } }, /* forêt claire        */
    {  0.50, { 0.50, 0.46, 0.28 } }, /* savane / garrigue   */
    {  0.58, { 0.46, 0.38, 0.28 } }, /* collines rocheuses  */
    {  0.66, { 0.38, 0.33, 0.27 } }, /* montagne            */
    {  0.74, { 0.58, 0.54, 0.50 } }, /* roche alpine        */
    {  0.82, { 0.85, 0.87, 0.95 } }, /* neige légère        */
    {  1.20, { 0.95, 0.97, 1.00 } }, /* neige épaisse       */
};
static int NBIOME = 14;

static Color biome_color(double h, double m) {
    /* Légère influence de l'humidité sur la couleur */
    int i;
    Color c = BIOME[NBIOME - 1].col;
    for (i = 0; i < NBIOME - 1; i++) {
        if (h <= BIOME[i + 1].h) {
            c = BIOME[i].col;
            break;
        }
    }
    /* Humidité : rend les zones terrestres un peu plus vertes/sombres */
    if (h > 0.05 && h < 0.70) {
        c.r -= m * 0.07;
        c.g += m * 0.06;
        c.b += m * 0.02;
    }
    /* Clamp */
    if (c.r < 0) c.r = 0;
    if (c.r > 1) c.r = 1;
    if (c.g < 0) c.g = 0;
    if (c.g > 1) c.g = 1;
    if (c.b < 0) c.b = 0;
    if (c.b > 1) c.b = 1;
    return c;
}

/* ─── Dessin de triangle rempli ──────────────────────────────── */
static void fill_triangle(SDL_Renderer *ren,
                           Vec2 p0, Vec2 p1, Vec2 p2,
                           Color col, double shade) {
    /* Appliquer ombrage */
    Uint8 r = (Uint8)((col.r * shade) * 255);
    Uint8 g = (Uint8)((col.g * shade) * 255);
    Uint8 b = (Uint8)((col.b * shade) * 255);

    SDL_SetRenderDrawColor(ren, r, g, b, 255);

    /* Rasterisation simple : scanlines */
    Vec2 v[3] = { p0, p1, p2 };
    /* Trier par Y */
    if (v[0].y > v[1].y) { Vec2 t = v[0]; v[0] = v[1]; v[1] = t; }
    if (v[1].y > v[2].y) { Vec2 t = v[1]; v[1] = v[2]; v[2] = t; }
    if (v[0].y > v[1].y) { Vec2 t = v[0]; v[0] = v[1]; v[1] = t; }

    int y0 = (int)ceil(v[0].y), y2 = (int)floor(v[2].y);
    int y;
    for (y = y0; y <= y2; y++) {
        double t02 = (v[2].y == v[0].y) ? 1 :
                     (y - v[0].y) / (v[2].y - v[0].y);
        double x02 = v[0].x + t02 * (v[2].x - v[0].x);
        double xb;
        if (y <= (int)floor(v[1].y)) {
            double t01 = (v[1].y == v[0].y) ? 1 :
                         (y - v[0].y) / (v[1].y - v[0].y);
            xb = v[0].x + t01 * (v[1].x - v[0].x);
        } else {
            double t12 = (v[2].y == v[1].y) ? 1 :
                         (y - v[1].y) / (v[2].y - v[1].y);
            xb = v[1].x + t12 * (v[2].x - v[1].x);
        }
        int xa = (int)ceil(fmin(x02, xb));
        int xe = (int)floor(fmax(x02, xb));
        SDL_RenderDrawLine(ren, xa, y, xe, y);
    }
}

/* Dessine le contour fin d'un triangle */
static void stroke_triangle(SDL_Renderer *ren,
                             Vec2 p0, Vec2 p1, Vec2 p2,
                             Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, r, g, b, a);
    SDL_RenderDrawLine(ren, (int)p0.x, (int)p0.y, (int)p1.x, (int)p1.y);
    SDL_RenderDrawLine(ren, (int)p1.x, (int)p1.y, (int)p2.x, (int)p2.y);
    SDL_RenderDrawLine(ren, (int)p2.x, (int)p2.y, (int)p0.x, (int)p0.y);
}

/* ─── Étoiles ────────────────────────────────────────────────── */
typedef struct { int x, y; Uint8 bri; int size; } Star;
static Star stars[MAX_STARS];
static int  nstars;

static void generate_stars(void) {
    int i;
    nstars = MAX_STARS;
    for (i = 0; i < nstars; i++) {
        stars[i].x   = (int)(lcg_rand() * WIN_W);
        stars[i].y   = (int)(lcg_rand() * WIN_H);
        stars[i].bri = (Uint8)(120 + lcg_rand() * 135);
        stars[i].size = (lcg_rand() < 0.07) ? 2 : 1;
    }
}

static void draw_stars(SDL_Renderer *ren) {
    int i;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    for (i = 0; i < nstars; i++) {
        /* Ne dessiner que les étoiles hors du disque planétaire */
        double dx = stars[i].x - PLANET_CX;
        double dy = stars[i].y - PLANET_CY;
        if (dx * dx + dy * dy <= (double)(PLANET_R + 4) * (PLANET_R + 4)) continue;
        Uint8 b = stars[i].bri;
        SDL_SetRenderDrawColor(ren, b, b, b, 255);
        if (stars[i].size == 2) {
            SDL_Rect rc = { stars[i].x - 1, stars[i].y - 1, 2, 2 };
            SDL_RenderFillRect(ren, &rc);
        } else {
            SDL_RenderDrawPoint(ren, stars[i].x, stars[i].y);
        }
    }
}

/* ─── Halo atmosphérique — glow circulaire lisse ────────────── */
static void draw_atmosphere(SDL_Renderer *ren,
                             double r_atm, Uint8 ar, Uint8 ag, Uint8 ab) {
    /* Approche pixel : alpha décroît exponentiellement avec la distance
       au-delà du rayon de la planète → glow parfaitement circulaire. */
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    int x, y;
    int R_out = (int)(PLANET_R + r_atm) + 2;
    for (y = PLANET_CY - R_out; y <= PLANET_CY + R_out; y++) {
        if (y < 0 || y >= WIN_H) continue;
        for (x = PLANET_CX - R_out; x <= PLANET_CX + R_out; x++) {
            if (x < 0 || x >= WIN_W) continue;
            double dx = x - PLANET_CX, dy = y - PLANET_CY;
            double dist = sqrt(dx * dx + dy * dy);
            if (dist <= PLANET_R || dist > PLANET_R + r_atm) continue;
            /* t = 0 au bord de la planète, 1 au bord de l'atmosphère */
            double t = (dist - PLANET_R) / r_atm;
            double a = (1.0 - t) * (1.0 - t) * (1.0 - t) * 200.0;
            if ((int)a <= 0) continue;
            SDL_SetRenderDrawColor(ren, ar, ag, ab, (Uint8)a);
            SDL_RenderDrawPoint(ren, x, y);
        }
    }
}

/* ─── Calotte polaire — positionnée aux vrais pôles ─────────── */
static void draw_icecap(SDL_Renderer *ren, int south, double cap_frac) {
    /* La calotte est un segment sphérique au pôle nord ou sud.
       On colorie les pixels dans le disque planète dont la coordonnée
       polaire y est dans la zone polaire, avec un fondu vers le bord. */
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    /* Limite polaire en y normalisé : -1=pôle nord, +1=pôle sud */
    double pole_limit = 1.0 - cap_frac * 2.0; /* ex: cap=0.20 → limit=0.60 */
    int x, y;
    for (y = 0; y < WIN_H; y++) {
        for (x = 0; x < WIN_W; x++) {
            double dx = x - PLANET_CX, dy = y - PLANET_CY;
            double d2 = dx * dx + dy * dy;
            if (d2 > (double)PLANET_R * PLANET_R) continue;
            /* Coordonnée y normalisée [-1, 1] */
            double ny = dy / PLANET_R; /* -1=nord, +1=sud */
            double check = south ? ny : -ny; /* > pole_limit = dans la calotte */
            if (check < pole_limit) continue;
            /* Fondu progressif : 0 au bord de la calotte, 1 au pôle */
            double t = (check - pole_limit) / (1.0 - pole_limit);
            Uint8 alpha = (Uint8)(t * t * 180);
            SDL_SetRenderDrawColor(ren, 225, 232, 250, alpha);
            SDL_RenderDrawPoint(ren, x, y);
        }
    }
}

/* ─── Nuages — petits cirrus/cumulus discrets ────────────────── */
typedef struct {
    double cx, cy, rx, ry, angle, alpha;
} CloudPatch;
#define MAX_CLOUDS 40
static CloudPatch clouds[MAX_CLOUDS];
static int nclouds;

static void generate_clouds(void) {
    int i;
    nclouds = 20 + (int)(lcg_rand() * 16);
    if (nclouds > MAX_CLOUDS) nclouds = MAX_CLOUDS;
    for (i = 0; i < nclouds; i++) {
        double ang = lcg_rand() * 2 * M_PI;
        /* Éviter les pôles (y normalisé dans [-0.85, 0.85]) */
        double max_rad = PLANET_R * (0.60 + lcg_rand() * 0.30);
        clouds[i].cx    = PLANET_CX + max_rad * cos(ang);
        clouds[i].cy    = PLANET_CY + max_rad * sin(ang);
        /* Nuages petits et allongés */
        clouds[i].rx    = 14 + lcg_rand() * 32;
        clouds[i].ry    = 5  + lcg_rand() * 10;
        clouds[i].angle = lcg_rand() * M_PI;
        clouds[i].alpha = 0.18 + lcg_rand() * 0.30;  /* bien plus discrets */
    }
}

static void draw_cloud(SDL_Renderer *ren, CloudPatch *c) {
    /* Rendu avec fondu radial (plus opaque au centre, transparent au bord) */
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    double ca = cos(c->angle), sa = sin(c->angle);
    int rx = (int)c->rx + 1, ry = (int)c->ry + 1;
    int dx, dy;
    for (dy = -ry; dy <= ry; dy++) {
        for (dx = -rx; dx <= rx; dx++) {
            double lx = dx * ca + dy * sa;
            double ly = -dx * sa + dy * ca;
            double ellipse_val = (lx * lx) / ((double)rx * rx) +
                                 (ly * ly) / ((double)ry * ry);
            if (ellipse_val > 1.0) continue;
            /* Fondu du centre vers le bord */
            double fade_v = 1.0 - ellipse_val;
            Uint8 alpha = (Uint8)(fade_v * fade_v * c->alpha * 210);
            if (alpha == 0) continue;
            int px = (int)c->cx + dx;
            int py = (int)c->cy + dy;
            if (px < 0 || px >= WIN_W || py < 0 || py >= WIN_H) continue;
            double ddx = px - PLANET_CX;
            double ddy = py - PLANET_CY;
            if (ddx * ddx + ddy * ddy > (double)PLANET_R * PLANET_R) continue;
            SDL_SetRenderDrawColor(ren, 240, 244, 255, alpha);
            SDL_RenderDrawPoint(ren, px, py);
        }
    }
}

/* ─── Dessin du masque circulaire (fond de l'espace par-dessus) ── */
/* On dessine l'espace sur tout ce qui est hors du cercle planète */
static void draw_space_mask(SDL_Renderer *ren) {
    /* Fond déjà noir. On passe juste en NONE pour les étoiles. */
    /* Masque circulaire : remplir un anneau autour de la planète avec le
       fond de l'espace (couleur 0,0,0 alpha=255) pour cacher les triangles
       débordant légèrement. */
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(ren, 5, 3, 15, 255);
    int x, y;
    int R = PLANET_R + 2;
    for (y = 0; y < WIN_H; y++) {
        for (x = 0; x < WIN_W; x++) {
            double dx = x - PLANET_CX, dy = y - PLANET_CY;
            if (dx * dx + dy * dy > (double)R * R) {
                SDL_RenderDrawPoint(ren, x, y);
            }
        }
    }
}

/* ─── Bande de lumière (spéculaire) ─────────────────────────── */
static void draw_specular(SDL_Renderer *ren, double light_ax, double light_ay) {
    /* Simule une légère réflexion spéculaire sur l'océan */
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    int y, x;
    for (y = 0; y < WIN_H; y++) {
        for (x = 0; x < WIN_W; x++) {
            double dx = x - PLANET_CX, dy = y - PLANET_CY;
            double d2 = dx * dx + dy * dy;
            if (d2 > (double)PLANET_R * PLANET_R) continue;
            /* Normal sphère */
            double nx = dx / PLANET_R;
            double ny = dy / PLANET_R;
            double nz = sqrt(fmax(0, 1 - nx * nx - ny * ny));
            /* Direction lumière */
            double dot = nx * light_ax + ny * light_ay + nz * 0.55;
            if (dot > 0.88) {
                Uint8 alpha = (Uint8)((dot - 0.88) / 0.12 * 90);
                SDL_SetRenderDrawColor(ren, 255, 255, 240, alpha);
                SDL_RenderDrawPoint(ren, x, y);
            }
        }
    }
}

/* ─── Main ───────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    /* Seed */
    if (argc > 1)
        g_seed = (unsigned int)atoi(argv[1]);
    else
        g_seed = (unsigned int)time(NULL);
    printf("Seed: %u\n", g_seed);

    /* Init SDL */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window   *win = SDL_CreateWindow("Planète Low Poly",
                                         SDL_WINDOWPOS_CENTERED,
                                         SDL_WINDOWPOS_CENTERED,
                                         WIN_W, WIN_H, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
                        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!win || !ren) {
        fprintf(stderr, "SDL Create: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    /* ── Initialisation du bruit ── */
    init_perm(g_seed);
    lcg_seed(g_seed ^ 0xDEADBEEF);

    /* Paramètres planète aléatoires */
    double freq_e   = 1.2 + lcg_rand() * 1.8;   /* fréquence élévation   */
    double freq_m   = 0.8 + lcg_rand() * 1.2;   /* fréquence humidité    */
    double sea_level = 0.02 + lcg_rand() * 0.20; /* niveau mer            */
    double ice_frac  = 0.15 + lcg_rand() * 0.25; /* taille calotte polaire*/
    /* Couleur atmosphère */
    double atm_r_f  = 0.3 + lcg_rand() * 0.5;
    double atm_g_f  = 0.5 + lcg_rand() * 0.4;
    double atm_b_f  = 0.7 + lcg_rand() * 0.3;
    Uint8  atm_r    = (Uint8)(atm_r_f * 255);
    Uint8  atm_g    = (Uint8)(atm_g_f * 255);
    Uint8  atm_b    = (Uint8)(atm_b_f * 255);
    /* Direction lumière (soleil) */
    double light_ax = -0.4 + lcg_rand() * 0.8;
    double light_ay = -0.4 + lcg_rand() * 0.8;

    /* ── Génération Delaunay ── */
    npts = 0; ntris = 0;
    init_super_triangle();
    generate_planet_points();
    printf("Points: %d\n", npts - 3);
    int i;
    for (i = 3; i < npts; i++)
        bowyerwatson_insert(i);
    remove_super_triangle();
    printf("Triangles: %d\n", ntris);

    /* ── Génération des étoiles et nuages ── */
    generate_stars();
    generate_clouds();

    /* ── Pré-calcul couleur de chaque triangle ── */
    typedef struct { Color col; double shade; int in_planet; } TriInfo;
    TriInfo *tinfo = malloc(ntris * sizeof(TriInfo));
    if (!tinfo) {
        fprintf(stderr, "Erreur: allocation mémoire échouée.\n");
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    for (i = 0; i < ntris; i++) {
        Vec2 p0 = pts[tris[i].a];
        Vec2 p1 = pts[tris[i].b];
        Vec2 p2 = pts[tris[i].c];
        /* Centroïde */
        double cx = (p0.x + p1.x + p2.x) / 3.0;
        double cy = (p0.y + p1.y + p2.y) / 3.0;
        double dx = cx - PLANET_CX, dy = cy - PLANET_CY;
        double r  = sqrt(dx * dx + dy * dy);

        tinfo[i].in_planet = (r < PLANET_R * 1.0);

        if (!tinfo[i].in_planet) { tinfo[i].col = (Color){0,0,0}; tinfo[i].shade = 1; continue; }

        /* Coordonnées normalisées [-1,1] pour le bruit */
        double nx = dx / PLANET_R, ny = dy / PLANET_R;

        /* Élévation = fBm de base + bruit ridgé pour les montagnes */
        double elev_base  = fbm(nx * freq_e + 3.7, ny * freq_e + 1.3,
                                NUM_PERLIN, 2.0, 0.52);
        double elev_ridge = ridged_fbm(nx * freq_e + 9.2, ny * freq_e + 5.8,
                                       5, 2.0, 0.50);
        /* Les crêtes n'élèvent que les zones déjà positives (continents) */
        double blend = (elev_base > 0) ? 0.40 : 0.10;
        double elev  = elev_base + blend * elev_ridge;

        /* Amplifier les reliefs positifs pour faire ressortir montagnes/neige */
        if (elev > 0.15) elev = 0.15 + (elev - 0.15) * 1.6;

        /* Humidité */
        double moist = fbm(nx * freq_m + 7.1, ny * freq_m + 4.9,
                           5, 2.0, 0.55);
        moist = (moist + 0.8) / 1.6;
        if (moist < 0) moist = 0;
        if (moist > 1) moist = 1;

        /* Ajuster par rapport au niveau de la mer */
        elev -= sea_level;

        tinfo[i].col = biome_color(elev, moist);

        /* Ombrage diffus (normal sphère approchée) */
        double nz  = sqrt(fmax(0, 1 - nx * nx - ny * ny));
        double dot = -(nx * light_ax + ny * light_ay) + nz * 0.6;
        tinfo[i].shade = 0.45 + 0.55 * fmax(0.0, fmin(1.0, dot));
    }

    /* ── Rendu ── */
    SDL_SetRenderDrawColor(ren, 5, 3, 15, 255);
    SDL_RenderClear(ren);

    /* Triangles planète */
    for (i = 0; i < ntris; i++) {
        if (!tinfo[i].in_planet) continue;
        Vec2 p0 = pts[tris[i].a];
        Vec2 p1 = pts[tris[i].b];
        Vec2 p2 = pts[tris[i].c];
        fill_triangle(ren, p0, p1, p2, tinfo[i].col, tinfo[i].shade);
        /* Contour très subtil pour l'effet low-poly */
        stroke_triangle(ren, p0, p1, p2, 0, 0, 0, 18);
    }

    /* Masque circulaire : cacher ce qui dépasse */
    draw_space_mask(ren);

    /* Étoiles — dessinées APRÈS le masque pour qu'elles restent visibles */
    draw_stars(ren);

    /* Calottes polaires */
    draw_icecap(ren, 0, ice_frac);
    draw_icecap(ren, 1, ice_frac * 0.75);

    /* Nuages */
    for (i = 0; i < nclouds; i++)
        draw_cloud(ren, &clouds[i]);

    /* Spéculaire */
    draw_specular(ren, light_ax, light_ay);

    /* Atmosphère */
    draw_atmosphere(ren, 55, atm_r, atm_g, atm_b);

    /* ── Sauvegarder en BMP ── */
    SDL_Surface *surf = SDL_CreateRGBSurface(0, WIN_W, WIN_H, 32,
                        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    if (surf) {
        SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888,
                             surf->pixels, surf->pitch);
        if (SDL_SaveBMP(surf, "planet.bmp") == 0)
            printf("Rendu sauvegardé : planet.bmp\n");
        SDL_FreeSurface(surf);
    }

    SDL_RenderPresent(ren);

    /* ── Boucle événements ── */
    SDL_Event ev;
    int running = 1;
    while (running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            if (ev.type == SDL_KEYDOWN &&
                (ev.key.keysym.sym == SDLK_ESCAPE ||
                 ev.key.keysym.sym == SDLK_q))
                running = 0;
        }
        SDL_Delay(16);
    }

    free(tinfo);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
