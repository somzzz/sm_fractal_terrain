#include <SDL/SDL.h>
#include <time.h>
#include <stdio.h>

#define WIDTH 1024
#define HEIGHT 512
#define MINHEIGHT (-20000)
#define MAXHEIGHT 20000


//Fiddle with these two to make different types of landscape at different distances
#define RANGE_CHANGE 13000
#define REDUCTION 0.7

typedef struct {
    int x;
    int y;
} Point;

SDL_Surface *screen;
int heightmap[WIDTH][HEIGHT+1];
SDL_Event event;

static void square_step(SDL_Rect *r, float deviance);
static void get_keypress(void);
static void shift_all(int amnt);

static int
rand_range(int low, int high) {
    return rand() % (high - low) + low;
}

static void
set_point(int x, int y, int value) {
    Uint32 *pix;
    int offset;

    pix = screen->pixels;
    offset = x + y * screen->w;

    pix[offset] = value;
}

static Uint32
height_to_colour(int height) {
    int value;
    int range;

    range = MAXHEIGHT - MINHEIGHT;
    if (height < MINHEIGHT)
        height = MINHEIGHT;
    else if (height > MAXHEIGHT)
        height = MAXHEIGHT;
    value = ((float)(height - MINHEIGHT) / range) * 255;

    if (height < 0)
        return SDL_MapRGB(screen->format, 0, 0, value);
    return SDL_MapRGB(screen->format, 30, value, 30);
    return SDL_MapRGB(screen->format, value, value, value);
}

static void
heightmap_to_screen(void) {
    int i, e;

    for (e = 0; e < HEIGHT; ++e)
        for (i = 0; i < WIDTH; ++i)
            set_point(i, e, height_to_colour(heightmap[i][e]));
}

static int
rect_avg_heights(SDL_Rect *r) {
    int total;
    total = heightmap[r->x][r->y];
    total += heightmap[(r->x + r->w) % WIDTH][r->y];
    total += heightmap[r->x][r->y + r->h];
    total += heightmap[(r->x + r->w) % WIDTH][r->y + r->h];
    return total / 4;
}

static int
diam_avg_heights(SDL_Rect *r) {
    int total;
    int divisors;

    divisors = 1;
    total = 0;
    //TOP
    if (r->y >= 0) {
        total += heightmap[r->x + r->w / 2][r->y];
        divisors++;
    }
    //LEFT
    if (r->x >= 0) {
        total += heightmap[r->x][r->y + r->h / 2];
        divisors++;
    }
    //RIGHT
    total += heightmap[(r->x + r->w) % WIDTH][r->y + r->h / 2];
    //BOTTOM
    if (r->y + r->h < HEIGHT) {
        total += heightmap[r->x + r->w / 2][r->y + r->h];
        divisors++;
    }

    if (!divisors) {
        puts("Floating point error in diam_avg_heights. Exiting cleanly.");
        exit(1);
    }

    return total / divisors;
}

static void
draw_all_squares(int w, int h, float deviance) {
    SDL_Rect r;
    r.w = w;
    r.h = h;

    for (r.y = 0; r.y < HEIGHT; r.y += r.h)
        for (r.x = 0; r.x < WIDTH; r.x += r.w)
            heightmap[r.x + r.w / 2][r.y + r.h / 2] = rect_avg_heights(&r) + rand_range(-RANGE_CHANGE, RANGE_CHANGE) * deviance;
}

static void
draw_all_diamonds(int w, int h, float deviance) {
    SDL_Rect r;
    r.w = w;
    r.h = h;

    for (r.y = 0 - r.h / 2; r.y < HEIGHT; r.y += r.h)
        for (r.x = 0; r.x < WIDTH; r.x += r.w)
            heightmap[r.x + r.w / 2][r.y + r.h / 2] = diam_avg_heights(&r) + rand_range(-RANGE_CHANGE, RANGE_CHANGE) * deviance;
    for (r.y = 0; r.y < HEIGHT; r.y += r.h)
        for (r.x = 0 - r.w / 2; r.x + r.w / 2 < WIDTH; r.x += r.w)
            heightmap[r.x + r.w / 2][r.y + r.h / 2] = diam_avg_heights(&r) + rand_range(-RANGE_CHANGE, RANGE_CHANGE) * deviance;
}

static void
sanitise_map(void) {
    //Reset the whole heightmap to the minimum height
    for (int e = 0; e < HEIGHT; ++e) {
        for (int i = 0; i < WIDTH; ++i) {
            if (heightmap[i][e] < MINHEIGHT)
                heightmap[i][e] = MINHEIGHT;
            if (heightmap[i][e] > MAXHEIGHT)
                heightmap[i][e] = MAXHEIGHT;
        }
    }
}

static void
shift_all(int amnt) {
    for (int e = 0; e < HEIGHT; ++e)
        for (int i = 0; i < WIDTH; ++i)
            heightmap[i][e] += amnt;
}

static void
make_map(void) {
    int w = WIDTH;
    int h = HEIGHT;
    float deviance;

    //Reset the whole heightmap to the minimum height
    for (int e = 0; e < HEIGHT; ++e)
        for (int i = 0; i < WIDTH; ++i)
            heightmap[i][e] = MINHEIGHT;

    //Add our starting corner points
    heightmap[0][0] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);
    heightmap[0][HEIGHT] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);

    deviance = 1.0;
    while (1) {
        draw_all_squares(w, h, deviance);
        draw_all_diamonds(w, h, deviance);

        w /= 2;
        h /= 2;

        if (w < 2 && h < 2)
            break;

        if (w < 2)
            w = 2;
        if (h < 2)
            h = 2;
        deviance *= REDUCTION;
    }
}

static void
get_keypress(void) {
    heightmap_to_screen();
    SDL_Flip(screen);
    while (1) {
        SDL_PollEvent(&event);
        if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.sym == SDLK_ESCAPE)
                exit(0);
            else if (event.key.keysym.sym == SDLK_RIGHTBRACKET)
                shift_all(200);
            else if (event.key.keysym.sym == SDLK_LEFTBRACKET)
                shift_all(-200);
            else if (event.key.keysym.sym == SDLK_SPACE)
                make_map();
            else
                break;
        }
        heightmap_to_screen();
        SDL_Flip(screen);
        SDL_Delay(10);
    }
}

int main(void) {
    srand(time(NULL));
    SDL_Init(SDL_INIT_EVERYTHING);
    screen = SDL_SetVideoMode(WIDTH, HEIGHT, 32, SDL_HWSURFACE);

    make_map();
    heightmap_to_screen();
    SDL_Flip(screen);

    while(1)
        get_keypress();
   
    SDL_FreeSurface(screen);
    SDL_Quit();

    return 0;
}
