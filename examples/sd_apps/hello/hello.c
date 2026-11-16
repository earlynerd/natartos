/* Console proof first. Display and joystick support follow with the picker. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *data = getenv("NATARTOS_APP_DATA");
    const char *package = getenv("NATARTOS_APP_PACKAGE");
    if (!data || !package || getenv("LD_PRELOAD") || getenv("LD_LIBRARY_PATH")) return 1;
    FILE *message = fopen("assets/message.txt", "rb");
    if (!message) { perror("message asset"); return 1; }
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), message)) fputs(buffer, stdout);
    int read_error = ferror(message);
    fclose(message);
    if (read_error) return 1;
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/launches.txt", data) >= (int)sizeof(path)) return 1;
    FILE *saved = fopen(path, "ab");
    if (!saved) { perror("saved data"); return 1; }
    fputs("Hello Goggles launched successfully.\n", saved);
    if (fclose(saved)) return 1;
    printf("package=%s\ndata=%s\n", package, data);
    for (int i=1; i<argc; ++i) printf("arg[%d]=%s\n", i, argv[i]);
    puts("Hello Goggles completed.");
    return 0;
}
