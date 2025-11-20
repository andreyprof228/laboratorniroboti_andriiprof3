#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <liburing.h> // Ключова бібліотека для io_uring
#include <fcntl.h>
#include <errno.h>

#define PORT 8080
#define QUEUE_DEPTH 128
#define BUFFER_SIZE 1024

// Коди операцій для User Data (для ідентифікації події в CQ)
enum {
    ACCEPT_OP,
    READ_OP,
    WRITE_OP,
};

// Структура для передачі даних між SQ та CQ
struct conn_info {
    int fd;
    int op;
    size_t size;
    char buffer[BUFFER_SIZE];
};

static void add_accept(struct io_uring *ring, int server_fd, struct sockaddr_in *client_addr, socklen_t *client_len) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return;
    
    // Налаштування запиту accept
    io_uring_prep_accept(sqe, server_fd, (struct sockaddr *)client_addr, client_len, 0);

    // Налаштування User Data
    struct conn_info *info = malloc(sizeof(*info));
    if (!info) {
        perror("malloc conn_info failed");
        return;
    }
    info->fd = server_fd;
    info->op = ACCEPT_OP;
    io_uring_sqe_set_data(sqe, info);

    io_uring_submit(ring);
}

static void add_read(struct io_uring *ring, int fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return;
    
    struct conn_info *info = malloc(sizeof(*info));
    if (!info) {
        perror("malloc conn_info failed");
        return;
    }
    info->fd = fd;
    info->op = READ_OP;

    // Підготовка операції читання (читання в info->buffer)
    io_uring_prep_read(sqe, fd, info->buffer, BUFFER_SIZE, 0);
    io_uring_sqe_set_data(sqe, info);

    io_uring_submit(ring);
}

static void add_write(struct io_uring *ring, int fd, char *buffer, size_t len) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return;
    
    struct conn_info *info = malloc(sizeof(*info));
    if (!info) {
        perror("malloc conn_info failed");
        return;
    }
    info->fd = fd;
    info->op = WRITE_OP;

    // Підготовка операції запису (Echo)
    io_uring_prep_write(sqe, fd, buffer, len, 0);
    io_uring_sqe_set_data(sqe, info);

    io_uring_submit(ring);
}

int setup_listening_socket() {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }
    printf("IO_uring Echo Server listening on port %d\n", PORT);
    return server_fd;
}

int main() {
    int server_fd = setup_listening_socket();
    struct io_uring ring;
    
    // Ініціалізація io_uring
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
        perror("io_uring_queue_init failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    // 1. Подання першого запиту accept
    add_accept(&ring, server_fd, &client_addr, &client_len); 

    // 2. Головний цикл: очікування завершень (CQE)
    while (1) {
        struct io_uring_cqe *cqe;
        
        // Очікуємо подій завершення (блокуючий виклик)
        io_uring_wait_cqe(&ring, &cqe); 
        
        struct conn_info *info = io_uring_cqe_get_data(cqe);
        int res = cqe->res; // Результат операції

        // 3. Обробка події завершення
        switch (info->op) {
            case ACCEPT_OP:
                if (res < 0) {
                    fprintf(stderr, "Accept failed: %s\n", strerror(-res));
                } else {
                    int client_fd = res;
                    printf("New connection accepted: FD %d\n", client_fd);
                    
                    // a) Негайно подаємо новий запит accept
                    add_accept(&ring, server_fd, &client_addr, &client_len);
                    
                    // b) Починаємо читати дані від нового клієнта
                    add_read(&ring, client_fd);
                }
                break;

            case READ_OP:
                if (res <= 0) {
                    if (res < 0) fprintf(stderr, "Read error on FD %d: %s\n", info->fd, strerror(-res));
                    printf("Client FD %d closed connection.\n", info->fd);
                    close(info->fd);
                } else {
                    // Успішне читання. Подаємо запит на запис (Echo)
                    add_write(&ring, info->fd, info->buffer, res); 
                }
                break;

            case WRITE_OP:
                if (res < 0) {
                    fprintf(stderr, "Write error on FD %d: %s\n", info->fd, strerror(-res));
                    close(info->fd);
                } else {
                    // Успішний запис. Подаємо новий запит на читання для того ж клієнта.
                    add_read(&ring, info->fd);
                }
                break;
        }

        // Очищення та звільнення пам'яті
        io_uring_cqe_seen(&ring, cqe);
        free(info);
    }

    // Очищення (недосяжне у циклі)
    io_uring_queue_exit(&ring);
    close(server_fd);
    return 0;
}
