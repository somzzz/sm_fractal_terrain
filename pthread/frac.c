#include <SDL/SDL.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <iostream>

#include <sstream>
#include <queue>
#include "errors.h"
#include <pthread.h>

#define WIDTH 4096
#define HEIGHT 4096
#define MINHEIGHT (-20000)
#define MAXHEIGHT 20000
#define BILLION  1000000000L;

#define NUM_THREADS 8


//Fiddle with these two to make different types of landscape at different distances
#define RANGE_CHANGE 13000
#define REDUCTION 0.7


typedef struct {
    int x;
    int y;
} Point;


typedef struct {
    int x, y;   // initial matrix top left corner
    int w;      // width
    int h;      // height
} Task;

pthread_barrier_t barrier; 
pthread_barrierattr_t attr;
 
pthread_mutex_t work_mutex = PTHREAD_MUTEX_INITIALIZER;    
pthread_cond_t work_cv = PTHREAD_COND_INITIALIZER;

pthread_mutex_t display_mutex = PTHREAD_MUTEX_INITIALIZER;    
pthread_cond_t display_cv = PTHREAD_COND_INITIALIZER;

pthread_mutex_t done_mutex = PTHREAD_MUTEX_INITIALIZER;    
pthread_cond_t done_cv = PTHREAD_COND_INITIALIZER;

pthread_mutex_t diamond_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t square_mutex = PTHREAD_MUTEX_INITIALIZER;   

int stop_signal;

std::queue<Task> diamondTasks;
std::queue<Task> squareTasks;

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
            //std::cout << heightmap[i][e] << " ";
        }
        //std::cout << std::endl;
    }
}

static int rect_avg_heights(SDL_Rect *r) {
    int total;
    // // UPPER LEFT
    // total = heightmap[r->x - r->w / 2][r->y - r->y / 2];
    // // UPPER RIGHT
    // total += heightmap[r->x - r->w / 2][r->y + r->y / 2];
    // // LOWER LEFT
    // total += heightmap[r->x + r->w / 2][r->y - r->y / 2];
    // // LOWER RIGHT
    // total += heightmap[r->x + r->w / 2][r->y + r->y / 2];
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
    if (r->x - r->w / 2 >= 0) {
        total += heightmap[r->x - r->w / 2][r->y];
        divisors++;
    }
    
    //LEFT
    if (r->y - r->h / 2 >= 0) {
        total += heightmap[r->x][r->y - r->h / 2];
        divisors++;
    }
    
    //RIGHT
    if (r->y + r->h / 2 <= HEIGHT) {
        total += heightmap[r->x][r->y + r->h / 2];
        divisors++;
    }
    
    //BOTTOM
    if (r->x + r->w / 2 <= WIDTH) {
        total += heightmap[r->x + r->w / 2][r->y];
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
    for (e = 0; e < HEIGHT + 1; ++e)
        for (i = 0; i < WIDTH + 1; ++i)
            heightmap[i][e] += amnt;
}

static void *make_map(void *args) {
    int w = WIDTH;
    int h = HEIGHT;
    float deviance;
    int i, e;
    int status;

    struct timespec start, stop;
    double accum;

    long my_id = (long)args;
    srand(time(NULL));

    //SDL_Surface *screen1;
    //printf("Starting thread: %ld\n", my_id);

    // if (my_id == 1) {
    //     SDL_Init(SDL_INIT_EVERYTHING);
    //     screen1 = SDL_SetVideoMode(WIDTH, HEIGHT, 32, SDL_HWSURFACE);
    // }

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

        int chillax;
        Task t;
        SDL_Rect r;
        deviance = 1.0;
        w = WIDTH;
        h = HEIGHT;

        if (my_id == 1) {

            Task t;
            t.x = 0; t.y = 0;
            t.w = WIDTH; t.h = HEIGHT;

            clock_gettime(CLOCK_REALTIME, &start);

            //Reset the whole heightmap to the minimum height
            for (e = 0; e < HEIGHT + 1; ++e)
                for (i = 0; i < WIDTH + 1; ++i)
                    heightmap[i][e] = MINHEIGHT;

            //Add our starting corner points
            heightmap[0][0] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);
            heightmap[0][HEIGHT] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);
            // heightmap[WIDTH][0] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);
            // heightmap[WIDTH][HEIGHT] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);

            //std::cout << heightmap[0][0] << "!!" << heightmap[0][HEIGHT] << std::endl;

            status = pthread_mutex_lock(&diamond_mutex);
            if (status) err_abort(status, "lock mutex");

            diamondTasks.push(t);

            status = pthread_mutex_unlock(&diamond_mutex);
            if (status) err_abort(status, "unlock mutex");
        }

            ///printf ("Thread %ld: waiting at barrier 1 w= %d h = %d\n", my_id, w, h);
        pthread_barrier_wait (&barrier);


        // Else do work and create map
        while (1) {

            // Start diamond step
            while (1) {
                chillax = 0;

                status = pthread_mutex_lock(&diamond_mutex);
                if (status) err_abort(status, "lock mutex");

                if (!diamondTasks.empty()) {
                    //printf ("Thread %ld: got size %ld\n", my_id, diamondTasks.size());
                    t = diamondTasks.front();
                    diamondTasks.pop();
                } else {
                    chillax = 1;
                }

                status = pthread_mutex_unlock(&diamond_mutex);
                if (status) err_abort(status, "unlock mutex");

                if (chillax) {
                    //printf ("Thread %ld: got no diamond task\n", my_id);
                    break;
                }

                // Do diamonds Task
                //printf ("Thread diamond %ld: w = %d h = %d\n", my_id, w, h);
                //printf ("Thread %ld: doing diamonds task x = %d y = %d w = %d h = %d\n", my_id, t.x, t.y, t.w, t.h);
                r.w = t.w; r.h = t.h;
                r.x = t.x; r.y = t.y;

                heightmap[r.x + r.w / 2][r.y + r.h / 2] = 
                    rect_avg_heights(&r)
                        + rand_range(-RANGE_CHANGE, RANGE_CHANGE) * deviance;


                status = pthread_mutex_lock(&square_mutex);
                if (status) err_abort(status, "lock mutex");

                squareTasks.push(t);

                status = pthread_mutex_unlock(&square_mutex);
                if (status) err_abort(status, "unlock mutex");
            }


            // Wait all threads
           // printf ("Thread %ld: waiting at barrier 2 w= %d h = %d\n", my_id, w, h);
            pthread_barrier_wait(&barrier);
            if (my_id == 1) {
                // std::ostringstream oss;
                // for (e = 0; e < HEIGHT + 1; ++e) {
                // for (i = 0; i < WIDTH + 1; ++i) {
                // oss << heightmap[i][e] << " ";
                // }
                // oss << "\n";
                // }
                //                 oss << "\n END ITER \n";  
                // std::cout << oss.str();
            }
             //           printf ("Thread %ld: waiting at barrier aux w= %d h = %d\n", my_id, w, h);
            pthread_barrier_wait(&barrier);

            // Start square step
            while (1) {
                chillax = 0;

                status = pthread_mutex_lock(&square_mutex);
                if (status) err_abort(status, "lock mutex");

                if (!squareTasks.empty()) {
                    t = squareTasks.front();
                    squareTasks.pop();
                } else {
                    chillax = 1;
                }

                status = pthread_mutex_unlock(&square_mutex);
                if (status) err_abort(status, "unlock mutex");

                if (chillax) {
                    //printf ("Thread %ld: got no square task\n", my_id);
                    break;
                }

                // Do square task
                //printf ("Thread %ld: doing square task x = %d y = %d w = %d h = %d\n", my_id, t.x, t.y, t.w, t.h);
            //printf ("Thread square%ld: w = %d h = %d\n", my_id, w, h);
                Task new_task;
                new_task.w = t.w / 2; new_task.h = t.h / 2;

                r.w = t.w; r.h = t.h;

                //  x ..o.. x
                //    ..x.. 
                //  x ..... x
                //
                r.x = t.x; r.y = t.y + t.h / 2;
                heightmap[r.x][r.y] =
                    diam_avg_heights(&r)
                        + rand_range(-RANGE_CHANGE, RANGE_CHANGE) * deviance;

                //  x ..... x
                //  o ..x.. 
                //  x ..... x
                //
                r.x = t.x + t.w / 2; r.y = t.y;
                heightmap[r.x][r.y] =
                    diam_avg_heights(&r)
                        + rand_range(-RANGE_CHANGE, RANGE_CHANGE) * deviance;

                //  x ..... x
                //    ..x.. 
                //  x ..o.. x
                //
                r.x = t.x + t.w; r.y = t.y + t.h / 2;
                heightmap[r.x][r.y] =
                    diam_avg_heights(&r)
                        + rand_range(-RANGE_CHANGE, RANGE_CHANGE) * deviance;

                //  x ..... x
                //    ..x.. o
                //  x ..... x
                //
                r.x = t.x + t.w / 2; r.y = t.y + t.h;
                heightmap[r.x][r.y] =
                    diam_avg_heights(&r)
                        + rand_range(-RANGE_CHANGE, RANGE_CHANGE) * deviance;

                // Create new tasks
                if (t.h >= 2 && t.w >= 2) {
                    status = pthread_mutex_lock(&diamond_mutex);
                    if (status) err_abort(status, "lock mutex");

                    // stanga sus
                    new_task.x = t.x; new_task.y = t.y;
                    diamondTasks.push(new_task);

                    // sus mijloc
                    new_task.x = t.x; new_task.y = t.y + t.h / 2;
                    diamondTasks.push(new_task);

                    // stanga mijloc
                    new_task.x = t.x + t.w / 2; new_task.y = t.y;
                    diamondTasks.push(new_task);

                    // mijloc mijloc
                    new_task.x = t.x + t.w / 2; new_task.y = t.y + t.h / 2;
                    diamondTasks.push(new_task);

                    status = pthread_mutex_unlock(&diamond_mutex);
                    if (status) err_abort(status, "unlock mutex");
                }            
            }

            // Wait all threads
            //printf ("Thread %ld: waiting at barrier 3 w= %d h = %d\n", my_id, w, h);
            pthread_barrier_wait(&barrier);

            if (diamondTasks.empty()) {

                // Signal master thread work done
                if (my_id == 1) {
                    //heightmap_to_screen();
                    clock_gettime(CLOCK_REALTIME, &stop);

                    accum = ( stop.tv_sec - start.tv_sec )
                             + (double)( stop.tv_nsec - start.tv_nsec )
                               / (double)1000000000;
                    printf("[PTHREADS] Make_map: %lf\n", accum);

                    status = pthread_mutex_lock(&display_mutex);
                    if (status) err_abort(status, "lock mutex");

                    status = pthread_cond_signal(&display_cv);
                    if (status) err_abort(status, "signal condition");

                    status = pthread_mutex_unlock(&display_mutex);
                    if (status) err_abort(status, "unlock mutex");
                }

                break;
            }

            //printf ("Thread %ld: waiting at barrier 4 w= %d h = %d\n", my_id, w, h);
            pthread_barrier_wait(&barrier);
            deviance *= REDUCTION;
            w /= 2; h /= 2;
        }

        // Display done
        // status = pthread_mutex_lock(&done_mutex);
        // if (status) err_abort(status, "lock mutex");

        // status = pthread_cond_wait(&done_cv, &done_mutex);
        // if (status) err_abort(status, "wait for condition");

        // status = pthread_mutex_unlock(&done_mutex);
        // if (status) err_abort(status, "unlock mutex");
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

    int pressed_once = 1;
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
            SDL_Delay(1000);
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

    pthread_mutex_destroy(&done_mutex);
    pthread_cond_destroy(&done_cv);

    pthread_mutex_destroy(&diamond_mutex);
    pthread_mutex_destroy(&square_mutex);

    pthread_exit(NULL);

    return 0;
}
