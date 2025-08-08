#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <pthread.h>
#include <time.h>
#include <sys/utsname.h>

#define PORT 3040
#define BUFFER_SIZE 4096
#define MAX_PATH 256
#define THREAD_POOL_SIZE 4

typedef struct {
    int* queue;
    int head;
    int tail;
    int count;
    int size;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
} thread_pool_queue;

thread_pool_queue client_queue;
pthread_mutex_t stats_mutex;

static unsigned long long last_cpu_total = 0;
static unsigned long long last_cpu_busy = 0;
static unsigned long long last_net_bytes_sent = 0;
static unsigned long long last_net_bytes_recv = 0;
static time_t last_net_timestamp = 0;

void init_queue(int size) {
    client_queue.queue = (int*)malloc(sizeof(int) * size);
    client_queue.head = 0;
    client_queue.tail = 0;
    client_queue.count = 0;
    client_queue.size = size;
    pthread_mutex_init(&client_queue.mutex, NULL);
    pthread_cond_init(&client_queue.not_empty, NULL);
}

void enqueue(int client_socket) {
    pthread_mutex_lock(&client_queue.mutex);
    client_queue.queue[client_queue.tail] = client_socket;
    client_queue.tail = (client_queue.tail + 1) % client_queue.size;
    client_queue.count++;
    pthread_cond_signal(&client_queue.not_empty);
    pthread_mutex_unlock(&client_queue.mutex);
}

int dequeue() {
    pthread_mutex_lock(&client_queue.mutex);
    while (client_queue.count == 0) {
        pthread_cond_wait(&client_queue.not_empty, &client_queue.mutex);
    }
    int client_socket = client_queue.queue[client_queue.head];
    client_queue.head = (client_queue.head + 1) % client_queue.size;
    client_queue.count--;
    pthread_mutex_unlock(&client_queue.mutex);
    return client_socket;
}

unsigned long long get_uptime_seconds() {
    FILE *fp;
    double uptime_seconds;
    fp = fopen("/proc/uptime", "r");
    if (fp == NULL) {
        return 0;
    }
    fscanf(fp, "%lf", &uptime_seconds);
    fclose(fp);
    return (unsigned long long)uptime_seconds;
}

double get_cpu_usage_percent() {
    pthread_mutex_lock(&stats_mutex);
    FILE *fp = fopen("/proc/stat", "r");
    if (fp == NULL) {
        pthread_mutex_unlock(&stats_mutex);
        return 0.0;
    }
    unsigned long long user, nice, system, idle;
    char line[256];
    fgets(line, sizeof(line), fp);
    sscanf(line, "cpu %llu %llu %llu %llu", &user, &nice, &system, &idle);
    fclose(fp);
    unsigned long long busy_time = user + nice + system;
    unsigned long long total_time = busy_time + idle;
    double cpu_percent = 0.0;
    if (last_cpu_total != 0) {
        unsigned long long delta_total = total_time - last_cpu_total;
        unsigned long long delta_busy = busy_time - last_cpu_busy;
        if (delta_total > 0) {
            cpu_percent = ((double)delta_busy / (double)delta_total) * 100.0;
        }
    }
    last_cpu_total = total_time;
    last_cpu_busy = busy_time;
    pthread_mutex_unlock(&stats_mutex);
    return cpu_percent;
}

void get_ram_swap_stats(unsigned long long *ram_total_kb, unsigned long long *ram_used_kb,
                        unsigned long long *swap_total_kb, unsigned long long *swap_used_kb) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp == NULL) {
        *ram_total_kb = 0;
        *ram_used_kb = 0;
        *swap_total_kb = 0;
        *swap_used_kb = 0;
        return;
    }
    unsigned long long total_mem = 0, free_mem = 0, buffers = 0, cached = 0;
    unsigned long long total_swap = 0, free_swap = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "MemTotal:") == line) {
            sscanf(line, "MemTotal: %llu kB", &total_mem);
        } else if (strstr(line, "MemFree:") == line) {
            sscanf(line, "MemFree: %llu kB", &free_mem);
        } else if (strstr(line, "Buffers:") == line) {
            sscanf(line, "Buffers: %llu kB", &buffers);
        } else if (strstr(line, "Cached:") == line) {
            sscanf(line, "Cached: %llu kB", &cached);
        } else if (strstr(line, "SwapTotal:") == line) {
            sscanf(line, "SwapTotal: %llu kB", &total_swap);
        } else if (strstr(line, "SwapFree:") == line) {
            sscanf(line, "SwapFree: %llu kB", &free_swap);
        }
    }
    fclose(fp);
    *ram_total_kb = total_mem;
    *swap_total_kb = total_swap;
    if (total_mem > 0) {
        *ram_used_kb = total_mem - free_mem - buffers - cached;
    } else {
        *ram_used_kb = 0;
    }
    if (total_swap > 0) {
        *swap_used_kb = total_swap - free_swap;
    } else {
        *swap_used_kb = 0;
    }
}

int get_cpu_temp_millicelsius() {
    FILE *fp;
    fp = fopen("/sys/class/thermal/thermal_zone2/temp", "r");
    if (fp == NULL) {
        return 0;
    }
    int temp_millicelcius;
    fscanf(fp, "%d", &temp_millicelcius);
    fclose(fp);
    return temp_millicelcius;
}

void get_disk_usage(const char *path, unsigned long long *total_bytes, unsigned long long *used_bytes, double *percent) {
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0) {
        *total_bytes = 0;
        *used_bytes = 0;
        *percent = 0.0;
        return;
    }
    unsigned long long total = vfs.f_blocks * vfs.f_bsize;
    unsigned long long free = vfs.f_bfree * vfs.f_bsize;
    unsigned long long used = total - free;
    *total_bytes = total;
    *used_bytes = used;
    if (total > 0) {
        *percent = (double)used / (double)total * 100.0;
    } else {
        *percent = 0.0;
    }
}

void get_network_stats(double *upload_speed_bytes_sec, double *download_speed_bytes_sec, unsigned long long *total_bytes_sent, unsigned long long *total_bytes_recv) {
    pthread_mutex_lock(&stats_mutex);
    FILE *fp = fopen("/proc/net/dev", "r");
    if (fp == NULL) {
        pthread_mutex_unlock(&stats_mutex);
        *upload_speed_bytes_sec = 0.0;
        *download_speed_bytes_sec = 0.0;
        *total_bytes_sent = 0;
        *total_bytes_recv = 0;
        return;
    }
    unsigned long long current_sent = 0, current_recv = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *iface = strstr(line, ":");
        if (iface) {
            unsigned long long recv_bytes, sent_bytes;
            sscanf(iface, ":%llu %*u %*u %*u %*u %*u %*u %*u %llu", &recv_bytes, &sent_bytes);
            current_recv += recv_bytes;
            current_sent += sent_bytes;
        }
    }
    fclose(fp);
    *upload_speed_bytes_sec = 0.0;
    *download_speed_bytes_sec = 0.0;
    time_t current_timestamp = time(NULL);
    if (last_net_timestamp != 0) {
        double delta_time = difftime(current_timestamp, last_net_timestamp);
        if (delta_time > 0) {
            *upload_speed_bytes_sec = (double)(current_sent - last_net_bytes_sent) / delta_time;
            *download_speed_bytes_sec = (double)(current_recv - last_net_bytes_recv) / delta_time;
        }
    }
    *total_bytes_sent = current_sent;
    *total_bytes_recv = current_recv;
    last_net_bytes_sent = current_sent;
    last_net_bytes_recv = current_recv;
    last_net_timestamp = current_timestamp;
    pthread_mutex_unlock(&stats_mutex);
}

void process_request(int client_socket) {
    char buffer[BUFFER_SIZE];
    read(client_socket, buffer, BUFFER_SIZE - 1);
    unsigned long long cpu_uptime_seconds = get_uptime_seconds();
    double cpu_usage_percent = get_cpu_usage_percent();
    unsigned long long ram_total_kb, ram_used_kb, swap_total_kb, swap_used_kb;
    get_ram_swap_stats(&ram_total_kb, &ram_used_kb, &swap_total_kb, &swap_used_kb);
    int cpu_temp_millicelsius = get_cpu_temp_millicelsius();
    unsigned long long main_disk_total_bytes, main_disk_used_bytes;
    double main_disk_percent;
    get_disk_usage("/", &main_disk_total_bytes, &main_disk_used_bytes, &main_disk_percent);
    unsigned long long usb_disk_total_bytes, usb_disk_used_bytes;
    double usb_disk_percent;
    get_disk_usage("/mnt/usb", &usb_disk_total_bytes, &usb_disk_used_bytes, &usb_disk_percent);
    double upload_speed_bytes_sec, download_speed_bytes_sec;
    unsigned long long total_bytes_sent, total_bytes_recv;
    get_network_stats(&upload_speed_bytes_sec, &download_speed_bytes_sec, &total_bytes_sent, &total_bytes_recv);
    char json_response[BUFFER_SIZE * 2];
    snprintf(json_response, sizeof(json_response),
        "{"
        "\"cpu_uptime_seconds\": %llu,"
        "\"cpu_usage_percent\": %.2f,"
        "\"ram_total_kb\": %llu,"
        "\"ram_used_kb\": %llu,"
        "\"swap_total_kb\": %llu,"
        "\"swap_used_kb\": %llu,"
        "\"cpu_temp_millicelsius\": %d,"
        "\"net_upload_bytes_sec\": %.2f,"
        "\"net_download_bytes_sec\": %.2f,"
        "\"net_total_bytes_sent\": %llu,"
        "\"net_total_bytes_recv\": %llu,"
        "\"main_disk_total_bytes\": %llu,"
        "\"main_disk_used_bytes\": %llu,"
        "\"main_disk_usage_percent\": %.2f,"
        "\"usb_disk_total_bytes\": %llu,"
        "\"usb_disk_used_bytes\": %llu,"
        "\"usb_disk_usage_percent\": %.2f"
        "}",
        cpu_uptime_seconds,
        cpu_usage_percent,
        ram_total_kb, ram_used_kb, swap_total_kb, swap_used_kb,
        cpu_temp_millicelsius,
        upload_speed_bytes_sec, download_speed_bytes_sec,
        total_bytes_sent, total_bytes_recv,
        main_disk_total_bytes, main_disk_used_bytes, main_disk_percent,
        usb_disk_total_bytes, usb_disk_used_bytes, usb_disk_percent
    );
    char http_header[256];
    snprintf(http_header, sizeof(http_header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", strlen(json_response));
    write(client_socket, http_header, strlen(http_header));
    write(client_socket, json_response, strlen(json_response));
    close(client_socket);
}

void* worker_thread_function(void* arg) {
    while (1) {
        int client_socket = dequeue();
        process_request(client_socket);
    }
    return NULL;
}

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    int opt = 1;
    pthread_t threads[THREAD_POOL_SIZE];
    pthread_mutex_init(&stats_mutex, NULL);
    init_queue(100);
    
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_create(&threads[i], NULL, worker_thread_function, NULL);
    }
    
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    
    printf("Starting system monitor API on port %d with a thread pool of %d workers\n", PORT, THREAD_POOL_SIZE);
    while(1) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept");
            exit(EXIT_FAILURE);
        }
        enqueue(new_socket);
    }
    
    pthread_mutex_destroy(&stats_mutex);
    return 0;
}
