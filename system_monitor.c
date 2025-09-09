#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>

#define PORT 3040
#define BUFFER_SIZE 4096
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

typedef struct {
    double cpu_usage_percent;
    double net_upload_bytes_sec;
    double net_download_bytes_sec;
    unsigned long long net_total_bytes_sent;
    unsigned long long net_total_bytes_recv;
} live_stats_t;

thread_pool_queue client_queue;
pthread_mutex_t stats_mutex;
pthread_mutex_t live_stats_mutex;
live_stats_t live_stats;

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
    if (!fp) return 0;
    fscanf(fp, "%lf", &uptime_seconds);
    fclose(fp);
    return (unsigned long long)uptime_seconds;
}

void get_ram_swap_stats(unsigned long long *ram_total_kb, unsigned long long *ram_used_kb,
                        unsigned long long *swap_total_kb, unsigned long long *swap_used_kb) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        *ram_total_kb = *ram_used_kb = *swap_total_kb = *swap_used_kb = 0;
        return;
    }
    unsigned long long total_mem = 0, free_mem = 0, buffers = 0, cached = 0;
    unsigned long long total_swap = 0, free_swap = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "MemTotal:") == line) sscanf(line, "MemTotal: %llu kB", &total_mem);
        else if (strstr(line, "MemFree:") == line) sscanf(line, "MemFree: %llu kB", &free_mem);
        else if (strstr(line, "Buffers:") == line) sscanf(line, "Buffers: %llu kB", &buffers);
        else if (strstr(line, "Cached:") == line) sscanf(line, "Cached: %llu kB", &cached);
        else if (strstr(line, "SwapTotal:") == line) sscanf(line, "SwapTotal: %llu kB", &total_swap);
        else if (strstr(line, "SwapFree:") == line) sscanf(line, "SwapFree: %llu kB", &free_swap);
    }
    fclose(fp);
    *ram_total_kb = total_mem;
    *swap_total_kb = total_swap;
    *ram_used_kb = (total_mem > 0) ? total_mem - free_mem - buffers - cached : 0;
    *swap_used_kb = (total_swap > 0) ? total_swap - free_swap : 0;
}

int get_cpu_temp_millicelsius() {
    FILE *fp = fopen("/sys/class/thermal/thermal_zone2/temp", "r");
    if (!fp) return 0;
    int temp_millicelcius;
    fscanf(fp, "%d", &temp_millicelcius);
    fclose(fp);
    return temp_millicelcius;
}

void get_disk_usage(const char *path, unsigned long long *total_bytes, unsigned long long *used_bytes, double *percent) {
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0) {
        *total_bytes = *used_bytes = 0;
        *percent = 0.0;
        return;
    }
    unsigned long long total = vfs.f_blocks * vfs.f_bsize;
    unsigned long long free = vfs.f_bfree * vfs.f_bsize;
    unsigned long long used = total - free;
    *total_bytes = total;
    *used_bytes = used;
    *percent = (total > 0) ? (double)used / (double)total * 100.0 : 0.0;
}

double get_cpu_usage_percent() {
    pthread_mutex_lock(&live_stats_mutex);
    double val = live_stats.cpu_usage_percent;
    pthread_mutex_unlock(&live_stats_mutex);
    return val;
}

void get_network_stats(double *upload_speed_bytes_sec,
                        double *download_speed_bytes_sec,
                        unsigned long long *total_bytes_sent,
                        unsigned long long *total_bytes_recv) {
    pthread_mutex_lock(&live_stats_mutex);
    *upload_speed_bytes_sec = live_stats.net_upload_bytes_sec;
    *download_speed_bytes_sec = live_stats.net_download_bytes_sec;
    *total_bytes_sent = live_stats.net_total_bytes_sent;
    *total_bytes_recv = live_stats.net_total_bytes_recv;
    pthread_mutex_unlock(&live_stats_mutex);
}

void get_os_info(char* kernel_version, size_t kernel_size, char* distro_name, size_t distro_size) {
    FILE* fp;
    char line[256];
    strcpy(kernel_version, "Unknown");
    strcpy(distro_name, "Unknown");
    fp = fopen("/proc/version", "r");
    if (fp) {
        if (fgets(line, sizeof(line), fp)) {
            sscanf(line, "Linux version %s", kernel_version);
        }
        fclose(fp);
    }
    fp = fopen("/etc/os-release", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "PRETTY_NAME=") == line) {
                char* start = strchr(line, '"');
                if (start) {
                    start++;
                    char* end = strchr(start, '"');
                    if (end) {
                        *end = '\0';
                        strncpy(distro_name, start, distro_size - 1);
                        distro_name[distro_size - 1] = '\0';
                        break;
                    }
                }
            }
        }
        fclose(fp);
    }
}

void* stats_updater(void* arg) {
    unsigned long long last_cpu_total = 0, last_cpu_busy = 0;
    unsigned long long last_net_sent = 0, last_net_recv = 0;
    time_t last_time = time(NULL);
    while (1) {
        FILE *fp = fopen("/proc/stat", "r");
        if (fp) {
            char line[1024];
            if (fgets(line, sizeof(line), fp)) {
                unsigned long long vals[16] = {0};
                int n = 0;
                char *saveptr = NULL;
                strtok_r(line, " \t\n", &saveptr);
                char *tok;
                while ((tok = strtok_r(NULL, " \t\n", &saveptr)) && n < 16) {
                    vals[n++] = strtoull(tok, NULL, 10);
                }
                unsigned long long total = 0;
                for (int i = 0; i < n; i++) total += vals[i];
                unsigned long long idle_all = 0;
                if (n > 3) idle_all += vals[3];
                if (n > 4) idle_all += vals[4];
                unsigned long long busy = total - idle_all;
                if (last_cpu_total != 0) {
                    unsigned long long delta_total = total - last_cpu_total;
                    unsigned long long delta_busy = busy - last_cpu_busy;
                    if (delta_total > 0) {
                        pthread_mutex_lock(&live_stats_mutex);
                        live_stats.cpu_usage_percent = ((double)delta_busy / (double)delta_total) * 100.0;
                        pthread_mutex_unlock(&live_stats_mutex);
                    }
                }
                last_cpu_total = total;
                last_cpu_busy = busy;
            }
            fclose(fp);
        }
        fp = fopen("/proc/net/dev", "r");
        if (fp) {
            char line[512];
            unsigned long long sent_total = 0, recv_total = 0;
            fgets(line, sizeof(line), fp);
            fgets(line, sizeof(line), fp);
            while (fgets(line, sizeof(line), fp)) {
                char *iface = strstr(line, ":");
                if (iface) {
                    unsigned long long recv_bytes, sent_bytes;
                    sscanf(iface, ":%llu %*u %*u %*u %*u %*u %*u %*u %llu",
                           &recv_bytes, &sent_bytes);
                    recv_total += recv_bytes;
                    sent_total += sent_bytes;
                }
            }
            fclose(fp);
            time_t now = time(NULL);
            double delta_t = difftime(now, last_time);
            if (last_time != 0 && delta_t > 0) {
                pthread_mutex_lock(&live_stats_mutex);
                live_stats.net_upload_bytes_sec = (double)(sent_total - last_net_sent) / delta_t;
                live_stats.net_download_bytes_sec = (double)(recv_total - last_net_recv) / delta_t;
                live_stats.net_total_bytes_sent = sent_total;
                live_stats.net_total_bytes_recv = recv_total;
                pthread_mutex_unlock(&live_stats_mutex);
            }
            last_net_sent = sent_total;
            last_net_recv = recv_total;
            last_time = now;
        }
        sleep(1);
    }
    return NULL;
}

void process_request(int client_socket) {
    char buffer[BUFFER_SIZE];
    char method[16], path[256];
    read(client_socket, buffer, BUFFER_SIZE - 1);
    sscanf(buffer, "%15s %255s", method, path);
    char json_response[BUFFER_SIZE * 2];
    char http_header[256];
    if (strcmp(path, "/distro") == 0) {
        char kernel_version[128];
        char distro_name[128];
        get_os_info(kernel_version, sizeof(kernel_version), distro_name, sizeof(distro_name));
        snprintf(json_response, sizeof(json_response),
            "{"
            "\"kernel_version\": \"%s\","
            "\"distro_name\": \"%s\""
            "}",
            kernel_version,
            distro_name
        );
        snprintf(http_header, sizeof(http_header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n", strlen(json_response));
    } else if (strcmp(path, "/stats") == 0) {
        unsigned long long cpu_uptime_seconds = get_uptime_seconds();
        double cpu_usage_percent = get_cpu_usage_percent();
        unsigned long long ram_total_kb, ram_used_kb, swap_total_kb, swap_used_kb;
        get_ram_swap_stats(&ram_total_kb, &ram_used_kb, &swap_total_kb, &swap_used_kb);
        int cpu_temp_millicelsius = get_cpu_temp_millicelsius();
        unsigned long long main_disk_total_bytes, main_disk_used_bytes;
        double main_disk_percent;
        get_disk_usage("/", &main_disk_total_bytes, &main_disk_used_bytes, &main_disk_percent);
        double upload_speed_bytes_sec, download_speed_bytes_sec;
        unsigned long long total_bytes_sent, total_bytes_recv;
        get_network_stats(&upload_speed_bytes_sec, &download_speed_bytes_sec, &total_bytes_sent, &total_bytes_recv);
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
            "\"main_disk_usage_percent\": %.2f"
            "}",
            cpu_uptime_seconds,
            cpu_usage_percent,
            ram_total_kb, ram_used_kb, swap_total_kb, swap_used_kb,
            cpu_temp_millicelsius,
            upload_speed_bytes_sec, download_speed_bytes_sec,
            total_bytes_sent, total_bytes_recv,
            main_disk_total_bytes, main_disk_used_bytes, main_disk_percent
        );
        snprintf(http_header, sizeof(http_header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n", strlen(json_response));
    } else {
        strcpy(json_response, "Not Found");
        snprintf(http_header, sizeof(http_header),
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %zu\r\n"
            "\r\n", strlen(json_response));
    }
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
    pthread_t updater_thread;
    pthread_mutex_init(&stats_mutex, NULL);
    pthread_mutex_init(&live_stats_mutex, NULL);
    memset(&live_stats, 0, sizeof(live_stats));
    init_queue(100);
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_create(&threads[i], NULL, worker_thread_function, NULL);
    }
    pthread_create(&updater_thread, NULL, stats_updater, NULL);
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
    while (1) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept");
            exit(EXIT_FAILURE);
        }
        enqueue(new_socket);
    }
    pthread_mutex_destroy(&stats_mutex);
    pthread_mutex_destroy(&live_stats_mutex);
    return 0;
}
