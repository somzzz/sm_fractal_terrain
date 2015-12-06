#include <SDL/SDL.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "errors.h"
#include <pthread.h>
#include <mpi.h>


#define WIDTH 4096
#define HEIGHT 4096
#define MINHEIGHT (-20000)
#define MAXHEIGHT 20000
#define BILLION  1000000000L;

#define TASK_TAG 1
#define RESULT_TAG 2

//Fiddle with these two to make different types of landscape at different distances
#define RANGE_CHANGE 13000
#define REDUCTION 0.7

#define NUM_THREADS 4


typedef struct {
    int v1, v2, v3, v4; // corner values
    int x, y;  // initial matrix top left corner
    int w;  // width
    int h;  // height
} Task;

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

int stop_signal, received;

SDL_Surface *screen;
int heightmap[WIDTH + 1][HEIGHT + 1];
SDL_Event event;

int myid, numprocs;

int node_w = WIDTH;
int node_h = HEIGHT;
int node_W = WIDTH; 
int node_H = HEIGHT;
float node_deviance = 1.0;

static void shift_all(int amnt);

static int rand_range(int low, int high) {
    return rand() % (high - low) + low;
}

static void set_point(int x, int y, int value) {
    Uint32 *pix;
    int offset;

    pix = (Uint32 *) screen->pixels;
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

static void heightmap_to_screen(void) {
    int i, e;

    for (e = 0; e < HEIGHT; ++e)
        for (i = 0; i < WIDTH; ++i)
            set_point(i, e, height_to_colour(heightmap[i][e]));
}

static int rect_avg_heights(SDL_Rect *r) {
    int total;
    total = heightmap[r->x][r->y];
    total += heightmap[(r->x + r->w) % node_W][r->y];
    total += heightmap[r->x][r->y + r->h];
    total += heightmap[(r->x + r->w) % node_H][r->y + r->h];
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
    total += heightmap[(r->x + r->w) % node_W][r->y + r->h / 2];

    //BOTTOM
    if (r->y + r->h < node_H) {
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
    for (e = 0; e < HEIGHT; ++e)
        for (i = 0; i < WIDTH; ++i)
            heightmap[i][e] += amnt;
}

// Makes a map with PTHREADS
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

        if (my_id == 0) {

            //Reset the whole heightmap to the minimum height
            for (e = 0; e < node_H + 1; ++e)
                for (i = 0; i < node_W + 1; ++i)
                    heightmap[i][e] = MINHEIGHT;

            //Add our starting corner points
            heightmap[0][0] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);
            heightmap[0][node_H] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);
        }

        pthread_barrier_wait (&barrier);

        clock_gettime(CLOCK_REALTIME, &start);
        if (my_id == 0) {
            while (node_h >= 2 && node_w >= 2) {
                draw_all_squares(0, 0, node_W, node_H, node_w, node_h, node_deviance);
                draw_all_diamonds(0, 0, node_W, node_H, node_w, node_h, node_deviance);

                node_w /= 2;
                node_h /= 2;

                node_deviance *= REDUCTION;

                if (pow(4, (node_H /  (2 * node_h))) == NUM_THREADS) {
                    break;
                }
            }
        }

        pthread_barrier_wait (&barrier);

        int startx = 0;
        int starty = 0;
        int local_w = node_w;
        int local_h = node_h;
        float local_deviance = node_deviance;

        int proc = 0;
        for (i = 0; i < node_H; i += node_h) {
            for (e = 0; e < node_W; e += node_w) {
                if (proc == my_id) {
                    startx = e;
                    starty = i;
                }
                proc++;
            }
        }

        int local_W = startx + node_w;
        int local_H = starty + node_h;


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
            //printf("[PTHREADS] Make_map: %lf\n", accum);

            while (!received) {      
                status = pthread_mutex_lock(&display_mutex);
                if (status) err_abort(status, "lock mutex");

                status = pthread_cond_signal(&display_cv);
                if (status) err_abort(status, "signal condition");

                status = pthread_mutex_unlock(&display_mutex);
                if (status) err_abort(status, "unlock mutex");
            }
        }
    }
}

// Split the Map between numprocs
int main(int argc, char *argv[]) {
    const int master = 0;
    int should_continue = 0;
    MPI_Status stat;

    int w = WIDTH;
    int h = HEIGHT;
    float deviance;
    int i, e;

    struct timespec start, stop;
    double accum;

    // Each MPI node will have NUM_THREADS threads
    long t;
    int status;
    pthread_t threads[NUM_THREADS];
    pthread_attr_t attr;
    stop_signal = 0;

    // Start MPI
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &numprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &myid);

    // Start PTHREADS
    pthread_barrier_init (&barrier, NULL, NUM_THREADS);

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    for (t = 0; t < NUM_THREADS; t++) {
        status = pthread_create(&threads[t], &attr, make_map, (void *)t);
        if (status) err_abort(status, "create thread");
    }

    // Mpi Task type
    MPI_Datatype taskType, oldtypes[1]; 
    int blockcounts[1];
    MPI_Aint offsets[1];
     
    offsets[0] = 0;
    oldtypes[0] = MPI_INT;
    blockcounts[0] = 8;

    MPI_Type_struct(1, blockcounts, offsets, oldtypes, &taskType);
    MPI_Type_commit(&taskType);

    srand(time(NULL));

    // SDL Init - only master handles the I/O
    if (myid == master) {
        SDL_Init(SDL_INIT_EVERYTHING);
        screen = SDL_SetVideoMode(WIDTH, HEIGHT, 32, SDL_HWSURFACE);
    }


    while (1) {
        w = WIDTH;
        h = HEIGHT;
        received = 0;

        if (myid == master) {
            clock_gettime(CLOCK_REALTIME, &start);
        }

        // Init heightmap for everybody
        for (e = 0; e < HEIGHT; ++e)
            for (i = 0; i < WIDTH; ++i)
                heightmap[i][e] = MINHEIGHT;

        if (myid == master) {

            // Add our starting corner points
            heightmap[0][0] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);
            heightmap[0][HEIGHT] = rand_range(-RANGE_CHANGE, RANGE_CHANGE);

            deviance = 1.0;
            while (h >= 2 || w >= 2) {
                draw_all_squares(0, 0, WIDTH, HEIGHT, w, h, deviance);
                draw_all_diamonds(0, 0, WIDTH, HEIGHT, w, h, deviance);

                deviance *= REDUCTION;

                w /= 2;
                h /= 2;
                
                // Divide parts of the matrix between processes after we reach the
                // step where the matrix is split into nrprocs pieces.
                // For calculations before this step, we do not engage into communiation
                // with others procs to prevent useless overhead.
                if (pow(4, (HEIGHT/  (2 * h))) == numprocs - 1) {
                    break;
                }
            }
        }

        MPI_Bcast(&h, 1, MPI_INT, master, MPI_COMM_WORLD);
        MPI_Bcast(&w, 1, MPI_INT, master, MPI_COMM_WORLD);
        MPI_Bcast(&deviance, 1, MPI_FLOAT, master, MPI_COMM_WORLD);

        //int borders[numprocs][4];
        int *buffer = (int *) calloc(w * h, sizeof(int));
        int H = h, W = w;

        // Continue processing. But split work.
        if (myid == master) {
            int proc = 1;

            if (numprocs != 1) {
                for (i = 0; i < HEIGHT; i += h) {
                    for (e = 0; e < WIDTH; e += w) {

                        if (proc > numprocs - 1) {
                            exit(1);
                        }

                        Task t;
                        t.v1 = heightmap[e][i];
                        t.v2 = heightmap[e][i + h];
                        t.v3 = heightmap[e + w][i];
                        t.v4 = heightmap[e + w][h + w];
                        t.x = e; t.y = i; t.w = w; t.h = h;

                        // printf("Master sending to %d values = %d %d %d %d, pos = %d %d, h = %d w = %d \n", 
                        //     proc, t.v1, t.v2, t.v3, t.v4, t.x, t.y, t.h, t.w);
                        MPI_Send(&t, 1, taskType, proc, TASK_TAG, MPI_COMM_WORLD);
                        proc++;
                    }
                }
            }

        } else {
            Task t;
            MPI_Recv(&t, 1, taskType, master, TASK_TAG, MPI_COMM_WORLD, &stat);
            // printf("Process %d received values = %d %d %d %d, pos = %d %d, h = %d w = %d \n", 
            //     myid, t.v1, t.v2, t.v3, t.v4, t.x, t.y, t.h, t.w);

            // init
            heightmap[0][0] = t.v1;
            heightmap[0][h] = t.v2;
            heightmap[w][0] = t.v3;
            heightmap[w][h] = t.v4;

            node_H = h;
            node_W = w;
            node_w = w;
            node_h = h;
            node_deviance = deviance;
        }

        MPI_Barrier(MPI_COMM_WORLD);

        if (myid != master) {

            // Each MPI node will calculate its heightmap with ptheards
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
            received = 1;

            status = pthread_mutex_unlock(&display_mutex);
            if (status) err_abort(status, "unlock mutex");

        }

        MPI_Barrier(MPI_COMM_WORLD);

        if (myid == master) {

            // Receive buffers from workers.
            for (i = 1; i < numprocs; i++) {
                Task t;
                MPI_Recv(&t, 1, taskType, i, TASK_TAG, MPI_COMM_WORLD, &stat);
                MPI_Recv(buffer, H * W, MPI_INT, i, RESULT_TAG, MPI_COMM_WORLD, &stat);

                // Store them in heightmap
                int j;
                for (j = 0; j < W; j++) {
                    memcpy(&heightmap[t.x + j][t.y], buffer + (j * W), W * sizeof(int));
                }
            }
        } else {
            Task t;
            for (i = 0; i < W; i++) {
                memcpy(buffer + (i * H), &heightmap[i][0], H * sizeof(int));
            }

            // Send the buffer to master
            MPI_Send(&t, 1, taskType, master, TASK_TAG, MPI_COMM_WORLD);
            MPI_Send (buffer, H * W, MPI_INT, master, RESULT_TAG, MPI_COMM_WORLD);
        }

        MPI_Barrier(MPI_COMM_WORLD);
        free(buffer);

        if (myid == master) {
            clock_gettime(CLOCK_REALTIME, &stop);

            accum = ( stop.tv_sec - start.tv_sec )
            + (double)( stop.tv_nsec - start.tv_nsec )
               / (double)1000000000;
            printf("[HYBRID] Overall time on key pressed event: %lf\n", accum);

            heightmap_to_screen();
            SDL_Flip(screen);

            while (1) {
                SDL_PollEvent(&event);
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    should_continue = 0;
                    break;
                }
            }
        }
        // if (myid == master) {
        //     heightmap_to_screen();
        //     SDL_Flip(screen);

        //     while (1) {
        //         SDL_PollEvent(&event);

        //         if (event.key.keysym.sym == SDLK_ESCAPE) {
        //             should_continue = 0;
        //             break;
        //         }
        //         else if (event.key.keysym.sym == SDLK_RIGHTBRACKET) {
        //             shift_all(200);
        //         }
        //         else if (event.key.keysym.sym == SDLK_LEFTBRACKET) {
        //             shift_all(-200);
        //         }
        //         else if (event.key.keysym.sym == SDLK_SPACE) {
        //             should_continue = 1;
        //             break;
        //         }
        //     }
        // }

        should_continue = 0;

        // Find out from master if the program should close or create another map.
        MPI_Bcast(&should_continue, 1, MPI_INT, master, MPI_COMM_WORLD);

        if (!should_continue) {
            break;
        }
    }

    // Close threads
    stop_signal = 1;

    status = pthread_mutex_lock(&work_mutex);
    if (status) err_abort(status, "lock mutex");

    status = pthread_cond_broadcast(&work_cv);
    if (status) err_abort(status, "signal condition");

    status = pthread_mutex_unlock(&work_mutex);
    if (status) err_abort(status, "unlock mutex");

    // Wait for all threads to complete
    for (t = 0; t < NUM_THREADS; t++) {
        pthread_join(threads[t], NULL);
    }
    printf ("Main(%d): Waited on %d threads. Done.\n", myid, NUM_THREADS);

    // Clean up and exit
    pthread_attr_destroy(&attr);
    pthread_mutex_destroy(&work_mutex);
    pthread_cond_destroy(&work_cv);

    pthread_mutex_destroy(&display_mutex);
    pthread_cond_destroy(&display_cv);

    // All work done
    // Master closes I/O
    if (myid == master) {
        SDL_FreeSurface(screen);
        SDL_Quit();
    }


    // Close MPI
    MPI_Finalize();
    pthread_exit(NULL);

    return 0;
}
