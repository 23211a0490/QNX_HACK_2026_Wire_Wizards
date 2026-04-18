#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#define SHM_NAME "/qnx_temp_shm"

int main() {
    srand((unsigned int)time(NULL));

    /* create shared memory */
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return -1; }
    ftruncate(fd, sizeof(int));

    int *shared_temp = mmap(NULL, sizeof(int),
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
    if (shared_temp == MAP_FAILED) { perror("mmap"); return -1; }

    *shared_temp = 25; /* starting temp LOWER */
    int sim_temp = 25;

    printf("[TEMP_SIM] Started, writing to %s\n", SHM_NAME);

    while (1) {
        /* oscillate between 25 and 55 to test fan on/off */
        int direction = (sim_temp < 35) ? 1 : -1;

        if (sim_temp < 35) {
            sim_temp += 2;  /* heat up to threshold */
        } else if (sim_temp > 50) {
            sim_temp -= 3;  /* cool down fast below threshold */
        } else {
            sim_temp += ((rand() % 3) - 1);  /* random walk in middle */
        }

        if (sim_temp < 25) sim_temp = 25;
        if (sim_temp > 60) sim_temp = 60;

        *shared_temp = sim_temp;
        printf("[TEMP_SIM] temp = %d°C (fan should turn %s at 42°C)\n",
               sim_temp, sim_temp >= 42 ? "ON" : "OFF");
        sleep(2);
    }

    munmap(shared_temp, sizeof(int));
    shm_unlink(SHM_NAME);
    return 0;
}
