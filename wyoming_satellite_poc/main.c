#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cJSON.h>
#include <pthread.h>
#include <stdatomic.h>
#include "ringbuffer.h"

#define PORT 10700
#define RX_BUFFER_SIZE (4096)
#define PACKET_BUFFER_SIZE (8096)

// There should not be (normally) packet bigger than ~2048 bytes, still, having some gap in any case.

uint8_t packet_buffer[PACKET_BUFFER_SIZE];
dev_ringbuf_t ringbuf;
atomic_int send_audio_chunks = ATOMIC_VAR_INIT(0);
atomic_int send_test_audio = ATOMIC_VAR_INIT(0);

struct packet {
    cJSON* header;
    cJSON* data;
    uint8_t* payload;
    uint16_t payload_length;
};

void packet_free(struct packet pkt, uint8_t free_payload)
{
    if (pkt.header != NULL) cJSON_free(pkt.header);
    if (pkt.data != NULL) cJSON_free(pkt.data);
    if (free_payload && pkt.payload != NULL) free(pkt.payload);
}

int newsockfd;

void send_packet(struct packet pkt)
{
    char* data_json = NULL;
    size_t data_json_length;
    if (pkt.data != NULL) {
        data_json = cJSON_PrintUnformatted(pkt.data);
        data_json_length = strlen(data_json);
        cJSON_AddItemToObject(pkt.header, "data_length", cJSON_CreateNumber(data_json_length));
    }
    if (pkt.payload != NULL) {
        cJSON_AddItemToObject(pkt.header, "payload_length", cJSON_CreateNumber(pkt.payload_length));
    }
    char* header_json = cJSON_PrintUnformatted(pkt.header);
    size_t header_json_length = strlen(header_json);
    // We are abusing the fact that every string ends with \0
    header_json[header_json_length] = '\n';
    send(newsockfd, header_json, header_json_length + 1, 0);
    free(header_json);
    if (pkt.data != NULL) {
        send(newsockfd, data_json, data_json_length, 0);
        free(data_json);
    }
    if (pkt.payload != NULL) {
        send(newsockfd, pkt.payload, pkt.payload_length, 0);
    }
}

void on_packet(struct packet pkt)
{
    char* type = cJSON_GetStringValue(cJSON_GetObjectItem(pkt.header, "type"));
    if (strcmp(type, "audio-chunk") != 0) printf("Got: %s\n", type);
    if (strcmp(type, "describe") == 0) {
        cJSON *header = cJSON_CreateObject();
        cJSON_AddStringToObject(header, "type", "info");
        cJSON_AddStringToObject(header, "version", "1.5.2");
        cJSON *data = cJSON_CreateObject();
        cJSON *asr_array = cJSON_CreateArray();
        cJSON_AddItemToObject(data, "asr", asr_array);
        cJSON *tts_array = cJSON_CreateArray();
        cJSON_AddItemToObject(data, "tts", tts_array);
        cJSON *handle_array = cJSON_CreateArray();
        cJSON_AddItemToObject(data, "handle", handle_array);
        cJSON *intent_array = cJSON_CreateArray();
        cJSON_AddItemToObject(data, "intent", intent_array);
        cJSON *wake_array = cJSON_CreateArray();
        cJSON_AddItemToObject(data, "wake", wake_array);

        cJSON *satellite_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(satellite_obj, "name", "my satellite");
        cJSON *attribution_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(attribution_obj, "name", "");
        cJSON_AddStringToObject(attribution_obj, "url", "");
        cJSON_AddItemToObject(satellite_obj, "attribution", attribution_obj);
        cJSON_AddTrueToObject(satellite_obj, "installed");
        cJSON_AddStringToObject(satellite_obj, "description", "my satellite");
        cJSON_AddStringToObject(satellite_obj, "version", "1.2.0");
        cJSON_AddNullToObject(satellite_obj, "area");
        cJSON_AddNullToObject(satellite_obj, "snd_format");
        cJSON_AddItemToObject(data, "satellite", satellite_obj);
        struct packet res_pkt = {
            .header = header,
            .data = data
        };
        send_packet(res_pkt);
        packet_free(res_pkt, 1);
    } else if (strcmp(type, "run-satellite") == 0) {
        cJSON* header = cJSON_CreateObject();
        cJSON_AddStringToObject(header, "type", "run-pipeline");
        cJSON_AddStringToObject(header, "version", "1.5.2");
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "start_stage", "wake");
        cJSON_AddStringToObject(data, "end_stage", "tts");
        cJSON_AddTrueToObject(data, "restart_on_end");
        struct packet res_pkt = {
            .header = header,
            .data = data
        };
        send_packet(res_pkt);
        packet_free(res_pkt, 1);
    } else if (strcmp(type, "detect") == 0) {
        atomic_store(&send_audio_chunks, 1);
    } else if (strcmp(type, "synthesize") == 0) {
        printf("Synthesize Result: %s\n", cJSON_GetStringValue(cJSON_GetObjectItem(pkt.data, "text")));
    } else if (strcmp(type, "transcript") == 0) {
        printf("Transcript Result: %s\n", cJSON_GetStringValue(cJSON_GetObjectItem(pkt.data, "text")));
    }

    packet_free(pkt, 1);
//    printf("H: %p (%s), D: %p, P: %p, %d\n", pkt.header, cJSON_GetStringValue(cJSON_GetObjectItem(pkt.header, "type")), pkt.data, pkt.payload, pkt.payload_length);
}

void process_packets()
{
    static uint8_t state = 0;
    static uint8_t process_buffer[4096];
    static struct packet pkt;
    static uint16_t data_length = 0;
    static uint16_t payload_length = 0;
    // The method for reading packets is similar to Python Wyoming. Not best, but works well.
    while (!ringbuffer_empty(&ringbuf)) {
        if (state == 0) {
            data_length = 0;
            payload_length = 0;
            pkt.data = NULL;
            pkt.payload = NULL;
            pkt.payload_length = 0;

            // Since AliOS ringbuffer doesn't have any methods for reading without modifying "read index", let's use buffers directly
            uint8_t header_found = 0;
            uint32_t header_length = 0;
            {
                uint32_t copy_sz = ringbuffer_available_read_space(&ringbuf);
                uint32_t i;

                /* cp data to user buffer */
                for (i = 0; i < copy_sz; i++) {
                    uint8_t ch = ringbuf.buffer[(ringbuf.ridx + i) % ringbuf.length];
                    if (ch == '\n') {
                        header_found = 1;
                        header_length = i + 1;
                        break;
                    }
                }
            }
            if (!header_found) break;
            ringbuffer_read(&ringbuf, process_buffer, header_length);

            pkt.header = cJSON_ParseWithLength(process_buffer, header_length);
            cJSON* data_length_field = cJSON_GetObjectItem(pkt.header, "data_length");
            if (data_length_field != NULL) {
                data_length = cJSON_GetNumberValue(data_length_field);
            }

            cJSON* payload_length_field = cJSON_GetObjectItem(pkt.header, "payload_length");
            if (payload_length_field != NULL) {
                payload_length = cJSON_GetNumberValue(payload_length_field);
            }
            if (data_length != 0) {
                state = 1;
            } else if (payload_length != 0) {
                state = 2;
            } else {
                on_packet(pkt);
            }
        } else if (state == 1) {
            if (ringbuffer_available_read_space(&ringbuf) < data_length) break;
            ringbuffer_read(&ringbuf, process_buffer, data_length);
            pkt.data = cJSON_ParseWithLength(process_buffer, data_length);
            if (payload_length != 0) {
                state = 2;
            } else {
                state = 0;
                on_packet(pkt);
            }
        } else if (state == 2) {
            if (ringbuffer_available_read_space(&ringbuf) < payload_length) break;
            pkt.payload = malloc(payload_length);
            ringbuffer_read(&ringbuf, pkt.payload, payload_length);
            pkt.payload_length = payload_length;
            on_packet(pkt);
            state = 0;
        }
    }
}

void* cli_thread_func(void* opaque)
{
    while (1) {
        int ch = getchar();
        if (ch == 's') {
            printf("Gonna send voice\n");
            atomic_store(&send_test_audio, 1);
        }
    }
}

void* microphone_thread(void* opaque)
{
    static uint8_t audio_buffer[2048];
    static FILE* test_file = NULL;
    while (1) {
        if (atomic_load(&send_audio_chunks)) {
            memset(audio_buffer, 0x0, sizeof(audio_buffer));
            if (atomic_load(&send_test_audio)) {
                if (test_file == NULL) {
                    test_file = fopen("test-turn-on-the-light.raw", "rb");
                }
                size_t read_size = fread(audio_buffer, 1, 2048, test_file);
                if (read_size <= 0) {
                    atomic_store(&send_test_audio, 0);
                    fclose(test_file);
                    test_file = NULL;
                    printf("Test audio finished\n");
                }
            }
            cJSON *header = cJSON_CreateObject();
            cJSON_AddStringToObject(header, "type", "audio-chunk");
            cJSON_AddStringToObject(header, "version", "1.5.2");

            cJSON *data = cJSON_CreateObject();
            cJSON_AddNumberToObject(data, "rate", 16000);
            cJSON_AddNumberToObject(data, "width", 2);
            cJSON_AddNumberToObject(data, "channels", 1);
            cJSON_AddNumberToObject(data, "timestamp", 4407203886274);
            struct packet pkt = {
                .header = header,
                .data = data,
                .payload = audio_buffer,
                .payload_length = 2048
            };
            send_packet(pkt);
            packet_free(pkt, 0);
        }

        /**
        * We have frequency 16000 Hz, thus 1000ms / 16000 Hz = 0.0625ms (duration of one sample)
        * One (mono) sample have 2bytes (16bit).
        * Python Wyoming satellite send 2048 bytes in one audio chunk.
        * This means that we will be sending the buffer every 2048 / 2 * 0.0625 = 64ms :-)
        * NOTE: This is not fully accurate, as we don't count with the time when this code is executed.
        */
        usleep(64 * 1000);
    }
}


int main() {

    ringbuffer_create(&ringbuf, (char*)packet_buffer, PACKET_BUFFER_SIZE);

    int sockfd, portno, client_len;
    struct sockaddr_in serv_addr, cli_addr;

    // Create a socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Error opening socket");
        exit(1);
    }

    // Initialize server address structure
    memset((char *) &serv_addr, 0, sizeof(serv_addr));
    portno = PORT;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);

    // Bind the socket to the server address
    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        perror("Error on binding");
        exit(1);
    }

    // Listen for incoming connections
    listen(sockfd, 5);
    printf("Server listening on port %d\n", portno);

    client_len = sizeof(cli_addr);

    // Accept a connection
    newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, (socklen_t *) &client_len);
    if (newsockfd < 0) {
        perror("Error on accept");
        exit(1);
    }

    pthread_t audio_thread;
    pthread_create(&audio_thread, NULL, microphone_thread, NULL);

    pthread_t cli_thread;
    pthread_create(&cli_thread, NULL, cli_thread_func, NULL);

    printf("Connection accepted\n");

    char rx_buffer[RX_BUFFER_SIZE];
    while (1) {
        ssize_t n = read(newsockfd, rx_buffer, RX_BUFFER_SIZE - 1);
//        printf("Received %zd\n", n);
        if (n == 0) break;

        ringbuffer_write(&ringbuf, (uint8_t*)rx_buffer, n);
        process_packets();
    }

    pthread_join(audio_thread, NULL);
    return 0;
}