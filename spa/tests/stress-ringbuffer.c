#define _GNU_SOURCE
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <sched.h>

#include <spa/utils/ringbuffer.h>

#define ARRAY_SIZE 64
#define MAX_VALUE 0x10000

struct spa_ringbuffer rb;
uint32_t size;
uint8_t *data;

static int fill_int_array(int *array, int start, int count)
{
	int i, j = start;
	for (i = 0; i < count; i++) {
		array[i] = j;
		j = (j + 1) % MAX_VALUE;
	}
	return j;
}

static int cmp_array(int *array1, int *array2, int count)
{
	int i;
	for (i = 0; i < count; i++)
		if (array1[i] != array2[i]) {
			printf("%d != %d at offset %d\n", array1[i], array2[i], i);
			return 0;
		}

	return 1;
}

static void *reader_start(void *arg)
{
	int i = 0, a[ARRAY_SIZE], b[ARRAY_SIZE];
	unsigned long j = 0, nfailures = 0;

	printf("reader started on cpu: %d\n", sched_getcpu());

	i = fill_int_array(a, i, ARRAY_SIZE);

	while (1) {
		uint32_t index;

		if (spa_ringbuffer_get_read_index(&rb, &index) >= ARRAY_SIZE * sizeof(int)) {
			spa_ringbuffer_read_data(&rb, data, size, index & (size - 1), b,
						 ARRAY_SIZE * sizeof(int));

			if (!cmp_array(a, b, ARRAY_SIZE)) {
				nfailures++;
				printf
				    ("failure in chunk %lu - probability: %lu/%lu = %.3f per million\n",
				     j, nfailures, j, (float) nfailures / (j + 1) * 1000000);
				i = (b[0] + ARRAY_SIZE) % MAX_VALUE;
			}
			i = fill_int_array(a, i, ARRAY_SIZE);
			j++;

			spa_ringbuffer_read_update(&rb, index + ARRAY_SIZE * sizeof(int));
		}
	}

	return NULL;
}

static void *writer_start(void *arg)
{
	int i = 0, a[ARRAY_SIZE];
	printf("writer started on cpu: %d\n", sched_getcpu());

	i = fill_int_array(a, i, ARRAY_SIZE);

	while (1) {
		uint32_t index;

		if (spa_ringbuffer_get_write_index(&rb, &index) >= ARRAY_SIZE * sizeof(int)) {
			spa_ringbuffer_write_data(&rb, data, size, index & (size - 1), a,
						  ARRAY_SIZE * sizeof(int));
			spa_ringbuffer_write_update(&rb, index + ARRAY_SIZE * sizeof(int));

			i = fill_int_array(a, i, ARRAY_SIZE);
		}
	}

	return NULL;
}

int main(int argc, char *argv[])
{
	printf("starting ringbuffer stress test\n");

	sscanf(argv[1], "%d", &size);

	printf("buffer size (bytes): %d\n", size);
	printf("array size (bytes): %ld\n", sizeof(int) * ARRAY_SIZE);

	spa_ringbuffer_init(&rb);
	data = malloc(size);

	pthread_t reader_thread, writer_thread;
	pthread_create(&reader_thread, NULL, reader_start, NULL);
	pthread_create(&writer_thread, NULL, writer_start, NULL);

	while (1)
		sleep(1);

	return 0;
}
