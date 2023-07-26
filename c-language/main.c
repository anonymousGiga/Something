#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

#define FILE1_NAME "./file1"
#define FILE2_NAME "./file2"
#define FILE3_NAME "./out1"
#define FILE4_NAME "./out2"
#define FILE1_SIZE (1024 * 1024 * 1024 * 160L) // 160G
#define FILE2_SIZE (1024 * 1024 * 100)         // 100M
// #define NUM_ITERATIONS 4194304                // file2总的循环次数, file1的总循环次数为
#define NUM_ITERATIONS 419444         // file2总的循环次数, file1的总循环次数为100*419444
#define NUM_READS_FILE1 100           // 读取多次file1后再读取一次file2
#define NUM_STATISTICS_INTERVAL 10000 // 10000次统计一下均值
#define PAGE_SIZE (4 * 1024)

static inline uint64_t rdtsc()
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc"
                         : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void rdtscp(uint32_t *aux, uint64_t *ts)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtscp"
                         : "=a"(lo), "=d"(hi), "=c"(*aux));
    *ts = ((uint64_t)hi << 32) | lo;
}

double get_cpu_frequency()
{
    FILE *fp;
    char buf[1024];
    double frequency = -1.0;

    fp = fopen("/proc/cpuinfo", "r");
    if (fp == NULL)
    {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    while (fgets(buf, sizeof(buf), fp) != NULL)
    {
        if (sscanf(buf, "cpu MHz\t\t: %lf", &frequency) == 1)
        {
            break;
        }
    }

    fclose(fp);

    if (frequency == -1.0)
    {
        fprintf(stderr, "Failed to get CPU frequency from /proc/cpuinfo\n");
        exit(EXIT_FAILURE);
    }

    return frequency * 1e6;
}

double cycles_to_ns(uint64_t cycles)
{
    double frequency = get_cpu_frequency();
    return cycles * 1e9 / frequency;
}

void print_times(double t1, double t2, int n, int fd)
{
    // printf("File %d, average time of 1000 times: %.2f\n", n, t1);
    // printf("File %d, min time of 1000 times: %.2f\n", n, t2);
    char buf[40];
    int len = sprintf(buf, "%.2f, %.2f\n", t1, t2);
    write(fd, buf, len);
}

int main()
{
    int fd1, fd2, fd3, fd4;
    struct stat sb1, sb2;
    char *addr1, *addr2;
    uint64_t start, end;
    uint32_t aux;
    double total_time_file1 = 0, total_time_file2 = 0;
    long max_time_file1 = 0, min_time_file1 = 10000;
    long max_time_file2 = 0, min_time_file2 = 10000;

    // 打开第一个文件并映射到内存中
    fd1 = open(FILE1_NAME, O_RDONLY);
    if (fd1 == -1)
    {
        perror("open");
        exit(EXIT_FAILURE);
    }
    if (fstat(fd1, &sb1) == -1)
    {
        perror("fstat");
        exit(EXIT_FAILURE);
    }
    addr1 = mmap(NULL, sb1.st_size, PROT_READ, MAP_PRIVATE, fd1, 0);
    if (addr1 == MAP_FAILED)
    {
        perror("mmap");
        exit(EXIT_FAILURE);
    }

    // 打开第二个文件并映射到内存中
    fd2 = open(FILE2_NAME, O_RDONLY);
    if (fd2 == -1)
    {
        perror("open");
        exit(EXIT_FAILURE);
    }
    if (fstat(fd2, &sb2) == -1)
    {
        perror("fstat");
        exit(EXIT_FAILURE);
    }
    addr2 = mmap(NULL, sb2.st_size, PROT_READ, MAP_PRIVATE, fd2, 0);
    if (addr2 == MAP_FAILED)
    {
        perror("mmap");
        exit(EXIT_FAILURE);
    }

    // mlock第二个文件的内存
    if (mlock(addr2, sb2.st_size) == -1)
    {
        perror("mlock");
        exit(EXIT_FAILURE);
    }

    // 使用vmtouch将第二个文件的数据加载到内存中
    if (system("vmtouch -vt " FILE2_NAME) != 0)
    {
        perror("vmtouch");
        exit(EXIT_FAILURE);
    }

    // 查看内存情况
    printf("==========================================\n");
    printf("Before read file\n");
    system("vmtouch file1");
    system("vmtouch file2");
    printf("==========================================\n");
    printf("\n");

    // 打开文件，用来存储结果
    mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    fd3 = open(FILE3_NAME, O_WRONLY | O_APPEND | O_CREAT | O_TRUNC, mode);
    if (fd3 == -1)
    {
        perror("open");
        exit(EXIT_FAILURE);
    }

    fd4 = open(FILE4_NAME, O_WRONLY | O_APPEND | O_CREAT | O_TRUNC, mode);
    if (fd4 == -1)
    {
        perror("open");
        exit(EXIT_FAILURE);
    }

    // 测试读取文件的性能
    int i, j;
    long offset1 = 0, offset2 = 0;
    int cnt1 = 0, cnt2 = 0;
    srand(time(NULL));
    for (i = 0; i < NUM_ITERATIONS; i++)
    {
        // 随机读取第一个文件中的某个位置的内容
        for (j = 0; j < NUM_READS_FILE1; j++)
        {
            // offset1 = rand() % FILE1_SIZE;
            offset1 = (offset1 + PAGE_SIZE) % FILE1_SIZE;
            rdtscp(&aux, &start);
            char c = *(addr1 + offset1);
            rdtscp(&aux, &end);
            uint64_t delt1 = (end - start);
            if (cnt1 < NUM_STATISTICS_INTERVAL)
            {
                total_time_file1 += cycles_to_ns(delt1);
                cnt1++;
            }
            else
            {
                cnt1 = 0;
                print_times((total_time_file1 / NUM_STATISTICS_INTERVAL), cycles_to_ns(min_time_file1), 1, fd3);
                total_time_file1 = cycles_to_ns(delt1);
            }
            if (delt1 < min_time_file1)
            {
                min_time_file1 = delt1;
            }
        }

        // 随机读取第二个文件中的某个位置的内容
        // offset = rand() % FILE2_SIZE;
        offset2 = (offset2 + PAGE_SIZE) % FILE2_SIZE;
        rdtscp(&aux, &start);
        char c = *(addr2 + offset2);
        rdtscp(&aux, &end);
        uint64_t delt2 = (end - start);
        if (cnt2 < NUM_STATISTICS_INTERVAL)
        {
            total_time_file2 += cycles_to_ns(delt2);
            cnt2++;
        }
        else
        {
            cnt2 = 0;
            print_times((total_time_file2 / NUM_STATISTICS_INTERVAL), cycles_to_ns(min_time_file2), 2, fd4);
            total_time_file2 = cycles_to_ns(delt2);
        }
        if (delt2 < min_time_file2)
        {
            min_time_file2 = delt2;
        }
    }

    print_times((total_time_file1 / cnt1), cycles_to_ns(min_time_file1), 1, fd3);
    print_times((total_time_file2 / cnt2), cycles_to_ns(min_time_file2), 2, fd4);

    // 查看内存情况
    printf("==========================================\n");
    printf("After read file\n");
    system("vmtouch file1");
    system("vmtouch file2");
    printf("==========================================\n");

    // 清理资源
    if (munmap(addr1, sb1.st_size) == -1)
    {
        perror("munmap");
        exit(EXIT_FAILURE);
    }
    if (munmap(addr2, sb2.st_size) == -1)
    {
        perror("munmap");
        exit(EXIT_FAILURE);
    }
    close(fd1);
    close(fd2);
    close(fd3);
    close(fd4);

    return 0;
}
