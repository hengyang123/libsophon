#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include "bmlib_runtime.h"

unsigned long long time_interval(struct timeval &last_time, struct timeval &fail_time) {
    return (fail_time.tv_sec - last_time.tv_sec) * 1000000 + fail_time.tv_usec - last_time.tv_usec;
}

void show_global_mem(bm_handle_t handle, unsigned long long addr, unsigned long long byte_size, int type_byte) {
    unsigned char *data = (unsigned char *)malloc(byte_size);
    if (!data) {
        printf("host malloc faied!\n");
        return;
    }
    bm_device_mem_t mem = bm_mem_from_device(addr, byte_size);
    bm_status_t ret = bm_memcpy_d2s(handle, data, mem);
    if (ret != BM_SUCCESS) {
        printf("d2s addr=0x%llx, size=%lld failed!\n", addr, byte_size);
        free(data);
        return;
    }
    printf("[");
    unsigned long long elem_num = byte_size / type_byte;
    if (type_byte == 4) {
        unsigned int *int_data = (unsigned int *)data;
        for (unsigned long long i = 0; i < elem_num; i++) {
            printf("0x%08x,", int_data[i]);
        }
    } else if (type_byte == 2) {
        unsigned short *short_data = (unsigned short *)data;
        for (unsigned long long i = 0; i < elem_num; i++)
        {
            printf("0x%04x,", short_data[i]);
        }
        printf("\n");
    } else {
        for (unsigned long long i = 0; i < elem_num; i++)
        {
            printf("0x%02x,", data[i]);
        }
    }
    printf("]\n");
    free(data);
}

int main(int argc, char **argv) {

    unsigned long long size = 128;
    int sleep_ms = 100;
    int dev_id = 0;
    int loop = -1;
    int count = 0;
    bm_status_t ret = BM_SUCCESS;

    int arg_idx = 1;
    if (argc > arg_idx) {
        sleep_ms = atoi(argv[arg_idx++]);
    }
    if (argc > arg_idx) {
        size = atoi(argv[arg_idx++]);
    }

    if (argc > arg_idx) {
        dev_id = atoi(argv[arg_idx++]);
    }
    if (argc > arg_idx) {
        loop = atoi(argv[arg_idx++]);
    }

    printf("Usage: %s [sleep_ms] [mem_size] [dev_id] [loop]\n", argv[0]);
    printf("current config: sleep_ms=%d, mem_size=%lld, dev_id=%d, loop=%d\n", sleep_ms, size, dev_id, loop);
    printf("================================================================\n");
    struct timeval last_time;
    unsigned long long last_addr;
    struct timeval fail_time;
    unsigned long long fail_addr = -1;
    char time_str[128] = {0};

    bm_handle_t handle = 0;
    ret = bm_dev_request(&handle, dev_id);
    if (ret != BM_SUCCESS) {
        printf("Open dev %d failed!\n", dev_id);
        return -1;
    }

    unsigned char *input_data = (unsigned char *)malloc(size);
    unsigned char *output_data = (unsigned char *)malloc(size);
    assert(input_data && output_data);

    for (unsigned long long i = 0; i < size; i++) {
        input_data[i] = (unsigned char)(i + 1);
    }

    int return_code = 0;
    bm_device_mem_t mem;

    while (loop < 0 || count < loop)
    {
        ret = bm_malloc_device_byte_heap_mask(handle, &mem, 7, size);
        if (ret != BM_SUCCESS) {
            printf("alloc mem size=%lld failed!\n", size);
            return_code = -2;
            break;
        }

        ret = bm_memcpy_s2d(handle, mem, input_data);
        if (ret != BM_SUCCESS) {
            printf("s2d addr=0x%llx, size=%lld failed!\n",
                    bm_mem_get_device_addr(mem),
                    (long long)bm_mem_get_device_size(mem));
            return_code = -3;
            break;
        }

        memset(output_data, 0, size);

        ret = bm_memcpy_d2s(handle, output_data, mem);
        if (ret != BM_SUCCESS) {
            printf("d2s addr=0x%llx, size=%lld failed!\n",
                    bm_mem_get_device_addr(mem),
                    (long long)bm_mem_get_device_size(mem));
            return_code = -4;
            break;
        }

        for (unsigned long long i = 0; i < size; i++) {
            if (output_data[i] != input_data[i]) {
                printf("data %lld check failed! exp: %d, got %d\n", (long long)i, input_data[i], output_data[i]);
                return_code = -5;
                break;
            }
        }

        if (return_code != 0) {
            for (unsigned long long i = 0; i < size; i++) {
                printf("data %lld: exp: %d, got %d\n", i, input_data[i], output_data[i]);
            }
            fail_addr = bm_mem_get_device_addr(mem);
            gettimeofday(&fail_time, 0);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&fail_time.tv_sec));
            printf("fail_addr: 0x%llx, fail_time: %s us=%d\n", fail_addr, time_str, (int)fail_time.tv_usec);

            if (last_addr != -1) {
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&last_time.tv_sec));
                printf("last_addr: 0x%llx, last_time: %s us=%d\n", last_addr, time_str, (int)last_time.tv_usec);
                unsigned long long elapsed = time_interval(last_time, fail_time);
                printf("time_elapsed: %lld\n", elapsed);
            }
            break;
        } else {
            last_addr = bm_mem_get_device_addr(mem);
            gettimeofday(&last_time, 0);
        }

        bm_free_device(handle, mem);

        count++;
        if (count % 100 == 0) {
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&last_time.tv_sec));
            printf("test_count:%d, addr: 0x%llx, time: %s us=%d\n", count, last_addr, time_str, (int)last_time.tv_usec);
            // show_global_mem(handle, last_addr, size, 4);
        }
        if (sleep_ms > 0) {
            usleep(sleep_ms * 1000);
        }
    }

    free(input_data);
    free(output_data);
    bm_dev_free(handle);

    return return_code;
}
