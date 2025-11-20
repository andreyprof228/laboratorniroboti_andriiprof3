#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h> // Для waitpid
#include <signal.h>   // Для обробки SIGCHLD
#include <errno.h>    // Для errno та EINTR

#define PORT 8080
#define BUFFER_SIZE 1024

// Функція для збору завершених дочірніх процесів (запобігання "зомбі")
void sigchld_handler(int s) {
    // WNOHANG: повертає негайно, якщо немає дочірніх процесів для збору
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

// Логіка обробки клієнта (TCP Echo Server)
void handle_connection(int client_socket) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    
    // Цикл Echo
    while ((bytes_read = read(client_socket, buffer, BUFFER_SIZE)) > 0) {
        // Якщо прочитано > 0 байт, негайно відправляємо їх назад
        if (write(client_socket, buffer, bytes_read) < 0) {
            perror("Write error in echo");
            break;
        }
    }

    if (bytes_read == 0) {
        printf("Client socket %d closed connection.\n", client_socket);
    } else if (bytes_read < 0) {
        perror("Read error");
    }

    // Закриваємо сокет і завершуємо дочірній процес
    close(client_socket);
    exit(EXIT_SUCCESS);
}

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    int opt = 1;
    pid_t child_pid;

    // 1. Налаштування обробника сигналу SIGCHLD для збору "зомбі"
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction failed");
        exit(EXIT_FAILURE);
    }

    // 2. Створення серверного сокета
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Дозволити повторне використання адреси/порту
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("Setsockopt failed");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // 3. Прив'язка сокета до порту
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    // 4. Режим очікування з'єднань
    if (listen(server_fd, 10) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    printf("Blocking Echo Server (1 process per client) listening on port %d\n", PORT);
    
    // 5. Головний цикл accept()
    while (1) {
        printf("\nParent (PID %d) waiting for a new connection...\n", getpid());
        
        // Виклик accept() - БЛОКУЮЧИЙ
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen)) < 0) {
            // Перевірка, чи не перервав accept() сигнал SIGCHLD
            if (errno == EINTR) {
                continue; 
            }
            perror("Accept failed");
            continue;
        }

        // 6. Створення дочірнього процесу
        child_pid = fork();
        
        if (child_pid == 0) {
            // -------- Дочірній процес --------
            printf("Child process (PID %d) created for client socket %d.\n", getpid(), new_socket);
            close(server_fd); // Дочірній процес закриває слухаючий сокет
            handle_connection(new_socket); // Обробка клієнта (цикл echo)
            // Процес завершується всередині handle_connection
            
        } else if (child_pid > 0) {
            // -------- Батьківський процес --------
            close(new_socket); // Батьківський процес закриває сокет клієнта
            // Батьківський процес повертається до accept()
        
        } else {
            perror("Fork failed");
            close(new_socket);
            continue;
        }
    }

    // 7. Закриття основного сокета (недосяжне)
    close(server_fd);
    return 0;
}
