#include <SDL/SDL.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <omp.h>


#define WIDTH 4096
#define HEIGHT 4096
#define MINHEIGHT (-20000)
#define MAXHEIGHT 20000
#define BILLION  1000000000L;

#define NUM_THREADS 4

//Fiddle with these two to make different types of landscape at different distances
#define RANGE_CHANGE 13000
#define REDUCTION 0.7

typedef struct {
    int x;
    int y;
} Point;

SDL_Surface *screen;
int heightmap[WIDTH + 1][HEIGHT + 1];
SDL_Event event;

static void square_step(SDL_Rect *r, float deviance);
static void get_keypress(void);
static void shift_all(int amnt);

static int rand_range(int low, int high) {
    return rand() % (high - low) + low;
}

static void set_point(int x, int y, int value) {
    Uint32 *pix;
    int offset;

    pix = (Uint32 *)screen->pixels;
    offset = x + y * screen->w;

    pix[offset] = value;
}

static Uint32 height_to_colour(int height) {
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

static int rect_avg_heights(SDL_Rect *r) {
    int total;
    total = heightmap[r->x][r->y];
    total += heightmap[(r->x + r->w) % WIDTH][r->y];
    total += heightmap[r->x][r->y + r->h];
    total += heightmap[(r->x + r->w) % WIDTH][r->y + r->h];
    return total / 4;
}

static int diam_avg_heights(SDL_Rect *r) {
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

static void shift_all(int amnt) {
    int i, e;
    for (e = 0; e < HEIGHT; ++e)
        for (i = 0; i < WIDTH; ++i)
            heightmap[i][e] += amnt;
}

static void make_map(void) {
    register int w = WIDTH;
    register int h = HEIGHT;
    register float deviance;
    register int i, e;

    register int id = omp_get_thread_num();

    struct timespec start, stop, start1, stop1, start12, stop12;
    double accum;
    int rx, ry;

    
    deviance = 1.0;
    clock_gettime(CLOCK_REALTIME, &start);

    #pragma omp parallel private(id, i, e, rx, ry) shared(w, h)
    {
        //Reset the whole heightmap to the minimum height
        #pragma omp for
        for (i = 0; i < HEIGHT * WIDTH; i++) {
                heightmap[i / HEIGHT][i % HEIGHT] = MINHEIGHT;
        }

        //Add our starting corner points

        #pragma omp single
        {
            heightmap[0][0] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);
            heightmap[0][HEIGHT] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);
            heightmap[WIDTH][0] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);
            heightmap[WIDTH][HEIGHT] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);
        }
        
        while(w >= 2 && h >= 2) {
               
            // Diamond step 
            // clock_gettime(CLOCK_REALTIME, &start12);
            
            #pragma omp sections nowait
            {
                #pragma omp section
                {
                    for (ry = 0; ry < HEIGHT / 2; ry += h) {
                        for (rx = 0; rx < WIDTH; rx += w) {
                            SDL_Rect r;
                            r.h = h; r.w = w;
                            r.x = rx; r.y = ry;
                            heightmap[r.x + (r.w >> 1)][r.y + (r.h >> 1)] =
                            rect_avg_heights(&r)
                                + rand_range(-RANGE_CHANGE, RANGE_CHANGE) * deviance;
                        }
                    }
                }

                #pragma omp section
                {
                    for (ry = HEIGHT / 2; ry < HEIGHT; ry += h) {
                        for (rx = 0 ; rx < WIDTH; rx += w) {
                            SDL_Rect r;
                            r.h = h; r.w = w;
                            r.x = rx; r.y = ry;
                            heightmap[r.x + (r.w >> 1)][r.y + (r.h >> 1)] =
                            rect_avg_heights(&r)
                                + rand_range(-RANGE_CHANGE, RANGE_CHANGE) * deviance;
                        }
                    }
                }
            }
            
            // clock_gettime(CLOCK_REALTIME, &stop12);

            // accum = ( stop12.tv_sec - start12.tv_sec )
            //     + (double)( stop12.tv_nsec - start12.tv_nsec )
            //         / (double) BILLION;
            // printf("[OPENMP] Diamonds: %lf\n", accum);


            #pragma omp barrier

            // clock_gettime(CLOCK_REALTIME, &start1);


            // Square step
            #pragma omp sections
            {
                #pragma omp section
                {
                    for (ry = 0 - (h >> 1); ry < HEIGHT; ry += h) {
                        for (rx = 0; rx < WIDTH; rx += w) {
                            SDL_Rect r;
                            r.h = h; r.w = w;
                            r.x = rx; r.y = ry;
                            heightmap[r.x + (r.w >> 1)][r.y + (r.h >> 1)] =
                                diam_avg_heights(&r)
                                    + rand_range(-RANGE_CHANGE, RANGE_CHANGE) * deviance;
                        }
                    }
                }

                #pragma omp section
                {
                    int rx, ry;
                    for (ry = 0; ry < HEIGHT; ry += h) {
                        for (rx = 0 - (w >> 1); rx < WIDTH - (w >> 1); rx += w) {
                            SDL_Rect r;
                            r.h = h; r.w = w;
                            r.x = rx; r.y = ry;
                            heightmap[r.x + (r.w >> 1)][r.y + (r.h >> 1)] =
                                diam_avg_heights(&r)
                                    + rand_range(-RANGE_CHANGE, RANGE_CHANGE) * deviance;
                        }
                    }
                }
            }
            
            // clock_gettime(CLOCK_REALTIME, &stop1);
            // accum = ( stop1.tv_sec - start1.tv_sec )
            //     + (double)( stop1.tv_nsec - start1.tv_nsec )
            //         / (double) BILLION;
            // printf("[OPENMP] Squares : %lf\n", accum);

            #pragma omp single
            {
                deviance *= REDUCTION;
                w = w >> 1;
                h = h >> 1;
            }
        }

        // Display on screen
        #pragma omp for
        for (i = 0; i < HEIGHT * WIDTH; i++) {
            set_point(i / HEIGHT, i % HEIGHT,
                height_to_colour(heightmap[i / HEIGHT][i % HEIGHT]));
        }
    }

    clock_gettime(CLOCK_REALTIME, &stop);
    accum = ( stop.tv_sec - start.tv_sec )
                + (double)( stop.tv_nsec - start.tv_nsec )
                    / (double) BILLION;
            printf("[OPENMP] Overall time on key pressed event: %lf\n", accum);

    
    SDL_Flip(screen);
}

static void get_keypress(void) {

    //heightmap_to_screen();
    SDL_Flip(screen);

    while (1) {
        SDL_PollEvent(&event);

        if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.sym == SDLK_ESCAPE) {
                exit(0);
            }
            else if (event.key.keysym.sym == SDLK_RIGHTBRACKET) {
                shift_all(200);
            }
            else if (event.key.keysym.sym == SDLK_LEFTBRACKET) {
                shift_all(-200);
            }
            else if (event.key.keysym.sym == SDLK_SPACE) {
                make_map();
            } else
                break;
        }

        //SDL_Flip(screen);
        //SDL_Delay(10);
    }
}


int main(void) {
    srand(time(NULL));
    SDL_Init(SDL_INIT_EVERYTHING);
    screen = SDL_SetVideoMode(WIDTH, HEIGHT, 32, SDL_HWSURFACE);

    struct timespec start, stop;
    double accum;

    omp_set_dynamic(0);
    omp_set_num_threads(NUM_THREADS);
    make_map();

    SDL_Flip(screen);

    while(1) {
        get_keypress();
    }

    SDL_FreeSurface(screen);
    SDL_Quit();

    return 0;
}
