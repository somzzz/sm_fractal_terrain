#include <SDL/SDL.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#include "errors.h"
#include <pthread.h>
#include <math.h>

#define WIDTH 4096
#define HEIGHT 4096
#define MINHEIGHT (-20000)
#define MAXHEIGHT 20000
#define BILLION  1000000000L;

#define NUM_THREADS 8

//Fiddle with these two to make different types of landscape at different distances
#define RANGE_CHANGE 13000
#define REDUCTION 0.7
#define BILLION  1000000000L;


typedef struct {
    int x;
    int y;
} Point;

pthread_barrier_t barrier; 
pthread_barrierattr_t attr;
 
pthread_mutex_t work_mutex = PTHREAD_MUTEX_INITIALIZER;    
pthread_cond_t work_cv = PTHREAD_COND_INITIALIZER;

pthread_mutex_t display_mutex = PTHREAD_MUTEX_INITIALIZER;    
pthread_cond_t display_cv = PTHREAD_COND_INITIALIZER;

int stop_signal;

SDL_Surface *screen;
int heightmap[WIDTH + 1][HEIGHT + 1];
SDL_Event event;

int w = WIDTH;
int h = HEIGHT;
float deviance = 1.0;

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

static Uint32 height_to_colour(int height, SDL_Surface *s) {
    int value;
    int range;

    range = MAXHEIGHT - MINHEIGHT;
    if (height < MINHEIGHT)
        height = MINHEIGHT;
    else if (height > MAXHEIGHT)
        height = MAXHEIGHT;
    value = ((float)(height - MINHEIGHT) / range) * 255;

    if (height < 0)
        return SDL_MapRGB(s->format, 0, 0, value);
    return SDL_MapRGB(s->format, 30, value, 30);
    return SDL_MapRGB(s->format, value, value, value);
}

static void heightmap_to_screen(SDL_Surface *s) {
    int i, e;

    for (e = 0; e < HEIGHT ; ++e) {
        for (i = 0; i < WIDTH; ++i) {
            set_point(i, e, height_to_colour(heightmap[i][e], s));
        }
    }
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

static void draw_all_squares(int startx, int starty, int W, int H, int w, int h, float deviance) {
    SDL_Rect r;
    r.w = w;
    r.h = h;

    for (r.y = starty; r.y < H; r.y += r.h)
        for (r.x = startx; r.x < W; r.x += r.w)
            heightmap[r.x + r.w / 2][r.y + r.h / 2] = rect_avg_heights(&r) + rand_range(-RANGE_CHANGE, RANGE_CHANGE) * deviance;
}

static void draw_all_diamonds(int startx, int starty, int W, int H, int w, int h, float deviance) {
    SDL_Rect r;
    r.w = w;
    r.h = h;

    for (r.y = starty - r.h / 2; r.y < H; r.y += r.h)
        for (r.x = startx; r.x < W; r.x += r.w)
            heightmap[r.x + r.w / 2][r.y + r.h / 2] = diam_avg_heights(&r) + rand_range(-RANGE_CHANGE, RANGE_CHANGE) * deviance;
    for (r.y = starty; r.y < H; r.y += r.h)
        for (r.x = startx - r.w / 2; r.x + r.w / 2 < W; r.x += r.w)
            heightmap[r.x + r.w / 2][r.y + r.h / 2] = diam_avg_heights(&r) + rand_range(-RANGE_CHANGE, RANGE_CHANGE) * deviance;
}

static void shift_all(int amnt) {
    int i, e;
    for (e = 0; e < HEIGHT + 1; ++e)
        for (i = 0; i < WIDTH + 1; ++i)
            heightmap[i][e] += amnt;
}

static void *make_map(void *args) {
    int i, e;
    int status;

    struct timespec start, stop;
    double accum;

    long my_id = (long)args;
    srand(time(NULL));

    while (1) {

        // wait for work request
        status = pthread_mutex_lock(&work_mutex);
        if (status) err_abort(status, "lock mutex");

        status = pthread_cond_wait(&work_cv, &work_mutex);
        if (status) err_abort(status, "wait for condition");

        status = pthread_mutex_unlock(&work_mutex);
        if (status) err_abort(status, "unlock mutex");

        // Check if ESC was pressed (stop_signal was set)
        if (stop_signal) {
            pthread_exit(NULL);
        }

        w = WIDTH;
        h = HEIGHT;
        deviance = 1;

        if (my_id == 0) {

            //Reset the whole heightmap to the minimum height
            for (e = 0; e < HEIGHT + 1; ++e)
                for (i = 0; i < WIDTH + 1; ++i)
                    heightmap[i][e] = MINHEIGHT;

            //Add our starting corner points
            heightmap[0][0] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);
            heightmap[0][HEIGHT] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);
        }

        pthread_barrier_wait (&barrier);

        clock_gettime(CLOCK_REALTIME, &start);
        if (my_id == 0) {
            while (h >= 2 && w >= 2) {
                draw_all_squares(0, 0, WIDTH, HEIGHT, w, h, deviance);
                draw_all_diamonds(0, 0, WIDTH, HEIGHT, w, h, deviance);

                w /= 2;
                h /= 2;

                deviance *= REDUCTION;

                if (pow(4, (HEIGHT /  (2 * h))) == NUM_THREADS) {
                    break;
                }
            }
        }

        pthread_barrier_wait (&barrier);

        int startx = 0;
        int starty = 0;
        int local_w = w;
        int local_h = h;
        float local_deviance = deviance;

        int proc = 0;
        for (i = 0; i < HEIGHT; i += h) {
            for (e = 0; e < WIDTH; e += w) {
                if (proc == my_id) {
                    startx = e;
                    starty = i;
                }
                proc++;
            }
        }

        int local_W = startx + w;
        int local_H = starty + h;


        // Else do work and create map
        while (local_h >= 2 && local_w >= 2) {
            // Individual computation
            draw_all_squares(startx, starty, local_W, local_H, local_w, local_h, local_deviance);
            pthread_barrier_wait(&barrier);

            draw_all_diamonds(startx, starty, local_W, local_H, local_w, local_h, local_deviance);
            pthread_barrier_wait(&barrier);

            local_w /= 2;
            local_h /= 2;

            local_deviance *= REDUCTION;
        }

        pthread_barrier_wait(&barrier);

        if (my_id == 0) {
            clock_gettime(CLOCK_REALTIME, &stop);

            accum = ( stop.tv_sec - start.tv_sec )
                + (double)( stop.tv_nsec - start.tv_nsec )
                    / (double)BILLION;
            printf("[PTHREADS] Make_map: %lf\n", accum);

            status = pthread_mutex_lock(&display_mutex);
            if (status) err_abort(status, "lock mutex");

            status = pthread_cond_signal(&display_cv);
            if (status) err_abort(status, "signal condition");

            status = pthread_mutex_unlock(&display_mutex);
            if (status) err_abort(status, "unlock mutex");
        }
    }
}

int main(void) {
    long t;
    int status;
    pthread_t threads[NUM_THREADS];
    pthread_attr_t attr;

    // Init SDL
    SDL_Init(SDL_INIT_EVERYTHING);
    screen = SDL_SetVideoMode(WIDTH, HEIGHT, 32, SDL_HWSURFACE);

    // Create threads
    stop_signal = 0;

    pthread_barrier_init (&barrier, NULL, NUM_THREADS);

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    for (t = 0; t < NUM_THREADS; t++) {
        status = pthread_create(&threads[t], &attr, make_map, (void *)t);
        if (status) err_abort(status, "create thread");
    }

    // Poll SDL events
    while (1) {
        SDL_PollEvent(&event);

        if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.sym == SDLK_ESCAPE) {
                // Set stop signal
                stop_signal = 1;

                // Signal threads to start work ( meaning to read the stop)
                status = pthread_mutex_lock(&work_mutex);
                if (status) err_abort(status, "lock mutex");

                status = pthread_cond_broadcast(&work_cv);
                if (status) err_abort(status, "signal condition");

                status = pthread_mutex_unlock(&work_mutex);
                if (status) err_abort(status, "unlock mutex");
                break;
            }
            else if (event.key.keysym.sym == SDLK_RIGHTBRACKET) {
                shift_all(200);
            }
            else if (event.key.keysym.sym == SDLK_LEFTBRACKET) {
                shift_all(-200);
            }
            else if (event.key.keysym.sym == SDLK_SPACE) {
                // Add first task in queue
                // Signal therads to work (signal is 0, so create a map)
                status = pthread_mutex_lock(&work_mutex);
                if (status) err_abort(status, "lock mutex");

                status = pthread_cond_broadcast(&work_cv);
                if (status) err_abort(status, "signal condition");

                status = pthread_mutex_unlock(&work_mutex);
                if (status) err_abort(status, "unlock mutex");

                // Wait for result
                status = pthread_mutex_lock(&display_mutex);
                if (status) err_abort(status, "lock mutex");

                status = pthread_cond_wait(&display_cv, &display_mutex);
                if (status) err_abort(status, "signal condition");

                status = pthread_mutex_unlock(&display_mutex);
                if (status) err_abort(status, "unlock mutex");

                heightmap_to_screen(screen);
                SDL_Flip(screen);
            } else
                continue;
           //SDL_Delay(1);
        }
    }

    /* Wait for all threads to complete */
    for (t = 0; t < NUM_THREADS; t++) {
        pthread_join(threads[t], NULL);
    }
    printf ("Main(): Waited on %d threads. Done.\n", NUM_THREADS);

    // Close resources
    SDL_FreeSurface(screen);
    SDL_Quit();

    /* Clean up and exit */
    pthread_attr_destroy(&attr);
    pthread_mutex_destroy(&work_mutex);
    pthread_cond_destroy(&work_cv);

    pthread_mutex_destroy(&display_mutex);
    pthread_cond_destroy(&display_cv);

    pthread_exit(NULL);

    return 0;
}
