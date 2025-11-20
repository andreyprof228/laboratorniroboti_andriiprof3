
TARGET = server

SRCS = server.c


CC = gcc

# Флаги компила:
# -Wall: Включить все предупреждения
# -Wextra: Включить дополнительные предупреждения
# -O2: Оптимизация
CFLAGS = -Wall -Wextra -O2

#правило сборки
all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

#для очистки скомпилированных файлов
clean:
	rm -f $(TARGET)
