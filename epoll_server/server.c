#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/epoll.h>

#define MAX_EVENTS 64
#define PORT 8080
#define BUFFER_SIZE 1024

// Встановлює неблокуючий режим для файлового дескриптора
static int setnonblocking(int fd) {
    int flags;
    if ((flags = fcntl(fd, F_GETFL, 0)) == -1) {
        flags = 0;
    }
    // Встановлюємо O_NONBLOCK
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl O_NONBLOCK failed");
        return -1;
    }
    return 0;
}

// Обробка даних: читання та Echo
int do_echo(int fd) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    int success = 0;

    // Цикл для Edge-Triggered режиму (ET), щоб прочитати всі доступні дані
    while (1) {
        bytes_read = read(fd, buffer, BUFFER_SIZE);

        if (bytes_read > 0) {
            // Echo: відправляємо прочитані байти назад
            if (write(fd, buffer, bytes_read) < 0) {
                perror("Write error in echo");
                success = -1; // Помилка запису
                break;
            }
        } else if (bytes_read == 0) {
            // Клієнт закрив з'єднання
            success = -1;
            break;
        } else if (bytes_read < 0) {
            // Помилка. Перевіряємо, чи це не помилка EAGAIN/EWOULDBLOCK
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Це нормально, всі дані прочитані (для неблокуючого сокета)
                break;
            }
            perror("Read error");
            success = -1;
            break;
        }
    }
    return success;
}


int main() {
    int server_fd, epoll_fd, new_socket;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    int opt = 1;
    
    // 1. Створення слухаючого сокета
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
    
    // Встановлюємо неблокуючий режим для слухаючого сокета
    if (setnonblocking(server_fd) == -1) {
        exit(EXIT_FAILURE);
    }
    printf("Epoll Echo Server listening on port %d\n", PORT);

    // 2. Створення epoll інстансу
    if ((epoll_fd = epoll_create1(0)) == -1) {
        perror("epoll_create1 failed");
        exit(EXIT_FAILURE);
    }

    // 3. Реєстрація слухаючого сокета в epoll
    struct epoll_event event;
    // EPOLLIN: подія читання, EPOLLET: Edge-Triggered (рекомендовано для продуктивності)
    event.events = EPOLLIN | EPOLLET; 
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        perror("epoll_ctl: server_fd failed");
        exit(EXIT_FAILURE);
    }

    struct epoll_event events[MAX_EVENTS];

    // 4. Головний цикл
    while (1) {
        // Очікуємо подій (блокируючий виклик)
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue; 
            perror("epoll_wait failed");
            exit(EXIT_FAILURE);
        }

        for (int n = 0; n < nfds; ++n) {
            int current_fd = events[n].data.fd;

            if (current_fd == server_fd) {
                // Нове з'єднання
                while (1) {
                    new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);
                    if (new_socket == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // Всі очікуючі з'єднання прийняті
                            break; 
                        } else {
                            perror("accept error");
                            break;
                        }
                    }
                    
                    // 5. Налаштування нового клієнта
                    if (setnonblocking(new_socket) == -1) {
                        close(new_socket);
                        continue;
                    }

                    event.events = EPOLLIN | EPOLLET; 
                    event.data.fd = new_socket;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_socket, &event) == -1) {
                        perror("epoll_ctl: client_fd failed");
                        close(new_socket);
                    } else {
                        printf("New connection accepted and registered: FD %d\n", new_socket);
                    }
                }
            } else {
                // Подія на клієнтському сокеті (читання)
                if (events[n].events & EPOLLIN) {
                    if (do_echo(current_fd) < 0) {
                        // Клієнт відключився або помилка, закриваємо та видаляємо
                        printf("Client FD %d disconnected or error.\n", current_fd);
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, current_fd, NULL);
                        close(current_fd);
                    }
                }
            }
        }
    }

    close(server_fd);
    close(epoll_fd);
    return 0;
}
