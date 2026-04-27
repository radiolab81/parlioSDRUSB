// sudo apt install libusb-1.0-0-dev
// gcc bridge.c -o usb_bridge -lusb-1.0 -lpthread
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <libusb-1.0/libusb.h>

#define DATA_PORT 1234
#define CTRL_PORT 5000
#define USB_VID   0x303A
#define USB_PID   0x4000
#define EP_OUT    0x01

// Globale Variablen (werden vom Control-Thread aktualisiert)
volatile float current_samplerate = 5.0f; 
volatile int current_bitwidth = 8;

libusb_device_handle *dev_handle = NULL;

// --- HILFSFUNKTION: USB INITIALISIERUNG ---
int init_usb() {
    if (libusb_init(NULL) < 0) return -1;
    dev_handle = libusb_open_device_with_vid_pid(NULL, USB_VID, USB_PID);
    if (!dev_handle) {
        fprintf(stderr, "Fehler: ESP32-P4 nicht gefunden (VID: 0x%04X, PID: 0x%04X)\n", USB_VID, USB_PID);
        return -1;
    }
    libusb_set_auto_detach_kernel_driver(dev_handle, 1);
    if (libusb_claim_interface(dev_handle, 0) < 0) return -1;
    printf("USB: ESP32-P4 High-Speed verbunden.\n");
    return 0;
}

// --- THREAD 1: DATA BRIDGE (Port 1234 -> USB Bulk) ---
void *data_bridge_thread(void *arg) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(DATA_PORT) };
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 1);

    const size_t USB_CHUNK = 16384; // 16KB ist optimal für High-Speed
    uint8_t *tcp_buffer = malloc(USB_CHUNK);
    size_t accumulated = 0;

    while (1) {

        printf("DATA: Warte auf SDR-Software auf Port %d...\n", DATA_PORT);
        int client = accept(server_fd, NULL, NULL);
        printf("DATA: Verbindung hergestellt.\n");

        while (1) {
            // Wir lesen nur so viel, wie in unseren Chunk passt
            // Das sorgt für saubere 512-Byte-aligned Transfers
            int n = recv(client, tcp_buffer + accumulated, USB_CHUNK - accumulated, 0);
            if (n <= 0) break;
            accumulated += n;

            if (accumulated >= USB_CHUNK) {
                // --- BERECHNUNG DER DROSSEL ---
                // Bytes pro Sekunde = MSPS * 1.000.000 * (Breite/8)
                //float bytes_per_second = current_samplerate * 1000000.0f * (current_bitwidth / 8.0f);
                // Zeit für diesen Chunk in Mikrosekunden
                //uint32_t chunk_time_us = (uint32_t)((USB_CHUNK / bytes_per_second) * 1000000.0f);

                int transferred = 0;
                 // Timeout auf 1000ms
                int res = libusb_bulk_transfer(dev_handle, EP_OUT, tcp_buffer, accumulated, &transferred, 1000);
                //int res = libusb_bulk_transfer(dev_handle, EP_OUT, tcp_buffer, USB_CHUNK, &transferred, 1000);                

                if (res == 0) {
                    accumulated = 0;
                } 
                else if (res == LIBUSB_ERROR_TIMEOUT) {
                    // Der ESP32 hat NAK geschickt (Puffer voll).
                    printf("ESP32 USB-Handler zu beschäftigt, wir warten einen Moment... Please hold the line ...\n");
                    usleep(100);
                }
               else {
                    // Echter Fehler (Device weg etc.)
                    fprintf(stderr, "USB - Error : %s\n", libusb_error_name(res));
                    if (res == LIBUSB_ERROR_NO_DEVICE) exit(1);
                    accumulated = 0; // Notfalls Puffer verwerfen
                }
            }
        }
        accumulated = 0;
        close(client);
        printf("DATA: Client getrennt.\n");
    }
    return NULL;
}

// --- THREAD 2: CONTROL BRIDGE (Port 5000 -> USB Control) ---
void *control_bridge_thread(void *arg) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(CTRL_PORT) };
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);

    while (1) {
        int client = accept(server_fd, NULL, NULL);
        char cmd[64] = {0};
        int n = recv(client, cmd, sizeof(cmd)-1, 0);
        
        if (n > 0) {
            cmd[n] = '\0';
            if (strncmp(cmd, "rate ", 5) == 0) {
                float rate = atof(cmd + 5);
                current_samplerate = rate;
                uint16_t wValue = (uint16_t)(rate * 10); // Rate als Fixpunkt (5.0 -> 50)
                printf("CTRL: Setze Rate auf %.2f MSPS (USB Request 0x01)\n", rate);
                libusb_control_transfer(dev_handle, 0x40, 0x01, wValue, 0, NULL, 0, 1000);
            } 
            else if (strncmp(cmd, "width ", 6) == 0) {
                int width = atoi(cmd + 6);
                current_bitwidth = width;
                printf("CTRL: Setze Breite auf %d Bit (USB Request 0x02)\n", width);
                libusb_control_transfer(dev_handle, 0x40, 0x02, (uint16_t)width, 0, NULL, 0, 1000);
            }
        }
        close(client);
    }
    return NULL;
}

int main() {
    if (init_usb() != 0) return 1;

    pthread_t t1, t2;
    pthread_create(&t1, NULL, data_bridge_thread, NULL);
    pthread_create(&t2, NULL, control_bridge_thread, NULL);

    printf("Bridge läuft. Daten: TCP 1234 -> USB Bulk. Control: TCP 5000 -> USB Control.\n");
    
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    libusb_release_interface(dev_handle, 0);
    libusb_close(dev_handle);
    libusb_exit(NULL);
    return 0;
}